/**
 * @file engine.hpp
 * @brief 模板引擎本体：`tmpl::TemplateEngine`（外加便于调用的 `tmpl::render`）。
 *
 * 用法就一行：
 *
 * ```cpp
 * const std::string code = tmpl::render(source, model);   // source 是字符串，不是文件
 * ```
 *
 * 命名空间叫 `tmpl`，所以自己代码里别再拿 `tmpl` 当变量名：`tmpl::render(tmpl, m)`
 * 里的第二个 `tmpl` 会把命名空间遮住（编译器只会说"'tmpl' is not a namespace"）。
 * 本文件的模板字符串参数一律叫 `source`，就是这个原因。
 *
 * **引擎不读文件**：模板是调用方给的字符串（想从磁盘读就自己读，想从资源里
 * 取就从资源里取），这样引擎不掺和路径、编码、权限这些事。
 *
 * 支持的语法（只做代码生成够用的那一小套，刻意不做表达式求值）：
 *
 * ```
 * {{ path.to.value }}      变量替换：缺失输出空串，不抛异常
 * {% if cond %} … {% else %} … {% endif %}    单路径真值判断，可无限嵌套
 * {% for item in list %} … {% endfor %}       遍历数组，循环体里能访问外层变量
 *                                             （循环里还有 loop_index / loop_first /
 *                                              loop_last / loop_count 可用）
 * {% include "名字" %}     插入另一段模板：片段表（名字 → 片段正文）由调用方给，
 *                                             片段里还能再 include
 * {% raw %} … {% endraw %}                   原样输出：里面的 {{ }} / {% %} 不当标签
 * {# 注释 #}               解析时直接丢掉
 * ```
 *
 * `{% include %}` 的几条约定：
 *
 *   - 片段正文**按模板渲染**，用的是**同一个上下文**——片段里能用外层变量，
 *     放进 `{% for %}` 里还能取到循环变量；
 *   - 片段名引号可省（`{% include "a" %}` / `{% include 'a' %}` / `{% include a %}`），
 *     名字里不认转义；
 *   - 片段里再 `{% include %}` 就是嵌套包含，层数上限 `k_maxIncludeDepth`，
 *     超了抛 `std::runtime_error`——写坏的互相包含不会把栈转爆；
 *   - 片段表里没有这个名字、压根没给片段表、或者标签里没写出名字 → 输出空串：
 *     和"缺变量 / 空列表 / 不认识的指令"一个口径，不报错、不崩。
 *
 * 实现要点：
 *
 *   - **不用 `<regex>`**：全程 `find()` + `substr()`，可预测、不 backtracking；
 *   - **`renderBlock(source, pos, ctx, stopTag, env)`**：一个主循环，从 `pos` 往下扫，
 *     遇到 `stopTag` 就"吃掉它并返回"，控制权交回上层——`if` / `for` 的嵌套
 *     就是靠这个停-回机制递归起来的（见各分支里的注释）；`env` 里装着
 *     `{% include %}` 的片段表与嵌套深度（引擎本身仍然无状态，这两个值只跟着
 *     递归走）；
 *   - **`findMatching`**：按**嵌套深度**找配对的 `{% endif %}` / `{% endfor %}`，
 *     所以内层标签不会被当成外层的结束；
 *   - **`ChildModel`**：循环体每轮一个子上下文，`item` 命中当前元素，其它名字
 *     顺着 `parent` 往上找——循环体里照样能用外层变量；
 *   - **未闭合标签抛 `std::runtime_error`**，其余（缺变量、空列表、缺片段、
 *     不认识的指令）一律安全跳过，不崩溃。
 *
 * 额外的"整行删除"规则（写代码生成模板时非常实用）：块标签（`{% %}` /
 * `{# #}`）如果**独占一行**（前面只有缩进、后面只有行尾空白），那么这一行
 * 连同换行一起删掉。上面例子里的
 *
 * ```
 * class {{ name }} {
 * {% for m in methods %}
 *     {{ m.ret_type }} {{ m.name }}({{ m.args }});
 * {% endfor %}
 * };
 * ```
 *
 * 渲染出来不会多出一堆空行；而 `{% if x %}public {% endif %}class` 这种
 * 同一行里有别的内容的标签不受影响，只删标签本身。
 */

#ifndef PARSE_ENGINE_TEMPLATE_ENGINE_HPP
#define PARSE_ENGINE_TEMPLATE_ENGINE_HPP

#include "model.hpp"
#include "naming.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * @def TMPL_THROW
 * @brief 报告模板错误：默认抛 `std::runtime_error`；`-fno-exceptions` 下改为
 *        打印一句并 `std::abort()`。
 *
 * HareParse 生成器整个是「不开异常」的（`bool` 返回 + `Error &` 出参），
 * 而 README 承诺 `-fno-exceptions` 也能构建——生成器确实 include 了本头文件。
 * 所以这里留一个编译期开关：启用异常时行为与从前**逐字相同**（照抛
 * `std::runtime_error`）；禁用异常时不让编译失败，把「模板写坏了」这件
 * 编程期错误变成 abort（输入里的模板都来自生成器自己，正常不会命中）。
 */
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define TMPL_THROW(message) throw std::runtime_error(message)
#else
#define TMPL_THROW(message)                                                    \
    do                                                                         \
    {                                                                          \
        std::fputs(std::string(message).c_str(), stderr);                      \
        std::fputc('\n', stderr);                                              \
        std::abort();                                                          \
    } while (false)
#endif

namespace tmpl
{

/**
 * @brief `{% include "名字" %}` 的片段表：名字 → 片段正文。
 *
 * 用 `std::map` 而不是 `unordered_map`：片段就那几个，**保序**（= 你填进去的
 * 顺序，调试时看得舒服）比 O(1) 更值——和 `Object` 用 `vector` 是同一个取舍。
 *
 * 表是**调用方**的东西：引擎不读文件，想让片段来自磁盘就自己读进来填表，
 * 想懒加载就在填表这一步做（引擎只在渲染到那一行时才查表，不会预先展开）。
 */
using SnippetTable = std::map<std::string, std::string>;

/**
 * @brief 空片段表：`render()` / `renderValues()` 没给片段表时用的缺省引用。
 *
 * 模板里写了 `{% include %}` 但没给表时，所有 include 都当"取不到"跳过。
 *
 * @return 一张空表（静态，**别改它**）。
 */
inline const SnippetTable &noSnippets()
{
    static const SnippetTable k_empty;
    return k_empty;
}

/**
 * @brief 循环体的子上下文：`item` 自己的值 + 指向外层的引用。
 *
 * `{% for m in methods %}` 每轮构造一个 `ChildModel`：`get("m")` 命中当前元素，
 * 其它名字顺着 `parent` 往上找。这就是"循环体里还能用外层变量"的实现——
 * 一个手写的动态作用域链，不需要额外的符号表。
 */
class ChildModel : public Model
{
public:
    /**
     * @brief 造一个子上下文。
     * @param scope 外层上下文（引用：生命周期要活过本对象）。
     * @param name 循环变量名（`{% for m in … %}` 里的 `m`）。
     * @param value 当前元素。
     */
    ChildModel(const Model &scope, std::string name, Value value,
               std::int64_t index = 0, std::int64_t count = 0)
        : parent(scope), item_name(std::move(name)), item_value(std::move(value)),
          item_index(index), item_count(count)
    {
    }

    /**
     * @brief 换下一轮的元素（循环体复用同一个子上下文时用）。
     *
     * 循环变量名与外层作用域在整段循环里不变，只有"当前元素 + 下标"会变；
     * 复用同一个 `ChildModel` 就省掉了每轮重新拷贝变量名的那次分配。
     *
     * @param value 当前元素。
     * @param index 当前元素下标（0 起）。
     * @param count 这一轮循环一共几个元素。
     */
    void setItem(Value value, std::int64_t index, std::int64_t count)
    {
        item_value = std::move(value);
        item_index = index;
        item_count = count;
    }

    /**
     * @brief 按名字取值：先找循环变量与循环元信息，再顺着作用域链往上找。
     *
     * 循环元信息（模板里排版数组、`switch` 分支这些就靠它，省得在 C++ 里拼
     * 分隔符 / 换行）：
     *   - `loop_index`：当前元素下标（0 起）；
     *   - `loop_first`：是不是第一个；
     *   - `loop_last`：是不是最后一个；
     *   - `loop_count`：这一轮循环一共几个元素。
     *
     * @param key 字段名。
     * @return 值；都找不到返回空 `Value{}`。
     */
    Value get(const std::string &key) const override
    {
        if (key == item_name) return item_value;
        if (key == "loop_index") return Value(item_index);
        if (key == "loop_first") return Value(item_index == 0);
        if (key == "loop_last") return Value((item_count > 0) && (item_index + 1 == item_count));
        if (key == "loop_count") return Value(item_count);
        return parent.get(key); // 作用域链向上查找
    }

    const Model &parent;  ///< 外层上下文。
    std::string item_name; ///< 循环变量名。
    Value item_value;      ///< 当前元素。
    std::int64_t item_index = 0; ///< 当前元素下标（0 起）。
    std::int64_t item_count = 0; ///< 这一轮循环的元素个数。
};

/**
 * @brief 模板引擎：把模板字符串 + `Model` 渲染成文本。
 *
 * 无状态、全静态，直接用 `TemplateEngine::render(source, root)`；平时更推荐用
 * 文件末尾那个 `tmpl::render(source, model)` 便捷函数（它替你包好适配器）。
 */
class TemplateEngine
{
public:
    /**
     * @brief `{% include %}` 的嵌套深度上限。
     *
     * 片段 A 包含 B、B 又包含 A 这种写坏的模板，光靠"深度"兜住：到了这个层数
     * 就抛 `std::runtime_error`，不会一直递归到爆栈。正常模板嵌套几层就够，
     * 给到 32 是留足余量（层数是"同时打开了多少个片段"，不是"一共 include 了几次"）。
     */
    static constexpr std::size_t k_maxIncludeDepth = 32u;

    /**
     * @brief 渲染整份模板。
     * @param source 模板字符串。
     * @param root 根上下文。
     * @param snippets `{% include %}` 的片段表（名字 → 片段正文）；不传就是空表，
     *                 模板里的 include 一律输出空串。
     * @return 渲染结果。
     * @throws std::runtime_error 有标签没闭合（`{%`、`{{`、`{#` 找不到对应的结束
     *         标记），或 `{% include %}` 嵌套超过 `k_maxIncludeDepth`。
     */
    static std::string render(const std::string &source, const Model &root,
                              const SnippetTable &snippets = noSnippets())
    {
        MatchMemo memo;
        const Env env{snippets, 0u, &memo};
        std::size_t pos = 0;
        // 输出缓冲由调用方持有、一路往下传着追加：每层 if / for 不再各自造一个
        // 子串再拷回父缓冲（大表格渲染时那是几十万次临时字符串分配）。
        std::string out;
        out.reserve(source.size() + 64u);
        renderBlock(out, source, pos, root, std::string_view(), env);
        return out;
    }

private:
    /**
     * @brief `{% if %}` / `{% for %}` / `{% raw %}` 配对位置的记忆表。
     *
     * 表格模板里"每个单元格一次 `{% if loop_first %}`"这种写法，同一个标签体
     * 会被求值几十万次；每次求值都要把模板往后扫到配对的 `{% endif %}`（模板
     * 有几千字节时就是几十万次全模板扫描）。配对位置只跟"模板内容 + 起点"有关，
     * 所以一次渲染里记住就行：键 =（模板首地址、起点、块种类），值 = 配对位置。
     *
     * 记忆表是**一次 render() 的局部量**，渲染结束即销毁，不会跨调用残留；
     * 键里的模板首地址保证不同片段（`{% include %}`）之间不会串味。
     */
    struct MatchKey
    {
        const void *source;  ///< 模板字符串的首地址（区分不同模板 / 片段）。
        std::size_t pos;     ///< 块标签之后的起点。
        std::size_t kind;    ///< 块种类（0 = if，1 = for，2 = raw，3 = 顶层 else）。

        bool operator==(const MatchKey &other) const
        {
            return source == other.source && pos == other.pos && kind == other.kind;
        }
    };

    /** @brief `MatchKey` 的哈希（指针按位混入，位置与种类各乘一个奇数）。 */
    struct MatchKeyHash
    {
        std::size_t operator()(const MatchKey &key) const
        {
            std::size_t h = std::hash<const void *>()(key.source);
            h ^= key.pos + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            h ^= key.kind + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }
    };

    /** @brief 配对位置记忆表：`MatchKey` → 配对标签（或顶层 else）的位置。 */
    using MatchMemo = std::unordered_map<MatchKey, std::size_t, MatchKeyHash>;

    /**
     * @brief 渲染一趟的"外部环境"：片段表 + include 嵌套深度 + 配对记忆表。
     *
     * 引擎不把这几项放进成员（那样就不是无状态的了），而是当参数一路跟着
     * 递归往下传。每进一层 `{% include %}` 换一个 `includeDepth + 1` 的副本
     * （记忆表则共用同一张指针），这就是循环包含的刹车。
     */
    struct Env
    {
        const SnippetTable &snippets; ///< `{% include %}` 的片段表。
        std::size_t includeDepth = 0u; ///< 当前片段嵌套层数（0 = 不在片段里）。
        MatchMemo *memo = nullptr;     ///< 配对位置记忆表（一次渲染共用一张）。
    };

    // -----------------------------------------------------------------------
    //  主循环
    // -----------------------------------------------------------------------

    /**
     * @brief 从 `pos` 开始渲染，直到模板结束或撞上 `stopTag`。
     *
     * 这是整个引擎的心脏：`if` / `for` 的分支内容都是"再用一个 `stopTag` 调用
     * 一次自己"渲染出来的，所以嵌套是天然的，不需要建语法树。
     *
     * @param source 模板字符串。
     * @param pos 当前位置；返回时停在 **停下来的那个标签之后**（没停就是
     *            `source.size()`）。调用方靠它继续往下渲染。
     * @param ctx 当前上下文（可能是 `ChildModel`，往上还能找到外层变量）。
     * @param stopTag 停止标签（如 `"{% endif %}"`）；空表示一路渲染到底。
     * @param env 片段表与 include 嵌套深度（`{% include %}` 要用；其余分支只是
     *            原样往下传）。
     * @param out 输出缓冲（**追加**写）；各层共用同一个，省掉"每层造一个子串
     *            再拷回父层"的开销。
     * @throws std::runtime_error 标签没闭合，或 include 嵌套超过 `k_maxIncludeDepth`。
     */
    static void renderBlock(std::string &out,
                            const std::string &source,
                            std::size_t &pos,
                            const Model &ctx,
                            std::string_view stopTag,
                            const Env &env)
    {
        while (pos < source.size())
        {
            // 1. 到达停止标签（比如 {% endif %}）：吃掉它，把控制权交回上层。
            if (!stopTag.empty() && isStopTag(source, pos, stopTag))
            {
                const std::size_t end = source.find("%}", pos);
                if (end == std::string::npos)
                    TMPL_THROW("未闭合标签: " + std::string(stopTag));
                pos = consumeBlockTagLine(source, pos, end + 2, out);
                return;
            }

            // 2. 找下一个 {{ / {% / {#，取最近的那个（一趟扫描）。
            const std::size_t nextPos = nextMarker(source, pos);

            // 3. 后面没有标签了：剩下的全是纯文本。
            if (nextPos == std::string::npos)
            {
                out.append(source, pos, std::string::npos);
                pos = source.size();
                break;
            }

            // 4. 先把标签之前的纯文本原样输出。
            out.append(source, pos, nextPos - pos);
            pos = nextPos;

            // 5. 按标签种类分发（种类就看 `{` 后面那个字符，不必再比位置）。
            const char marker = source[pos + 1];
            if (marker == '#')
            {
                // ---- 注释：{# … #} 整段丢掉，不输出 ----
                const std::size_t end = source.find("#}", pos);
                if (end == std::string::npos) TMPL_THROW("未闭合注释: {#");
                pos = consumeBlockTagLine(source, pos, end + 2, out);
            }
            else if (marker == '{')
            {
                // ---- 变量：{{ path.to.value }} ----
                const std::size_t end = source.find("}}", pos);
                if (end == std::string::npos) TMPL_THROW("未闭合变量标签: {{");
                const std::string path(trimView(
                    std::string_view(source).substr(pos + 2, end - pos - 2)));
                out += resolve(ctx, path).toString(); // 缺失变量 → 空串
                pos = end + 2;
            }
            else
            {
                // ---- 块标签：{% … %} ----
                const std::size_t end = source.find("%}", pos);
                if (end == std::string::npos) TMPL_THROW("未闭合标签: {%");
                const std::size_t tagStart = pos;
                const std::size_t tagEnd = end + 2;
                // 标签内容只当"视图"看：判断指令名、取路径都不必先造一个 string。
                const std::string_view tag = trimView(
                    std::string_view(source).substr(pos + 2, end - pos - 2));

                // 内容式停止判定：{%endif%} 这种不留空格的写法也能停住。
                if (!stopTag.empty() && matchesStopCommand(tag, stopTag))
                {
                    pos = consumeBlockTagLine(source, tagStart, tagEnd, out);
                    return;
                }

                // 标签独占一行时整行删掉；bodyStart 就是"标签之后该从哪继续"。
                const std::size_t bodyStart = consumeBlockTagLine(source, tagStart, tagEnd, out);

                if (commandIs(tag, "if"))
                {
                    // ---- 条件：{% if cond %} … {% else %} … {% endif %} ----
                    const std::string condPath(trimView(tag.substr(2)));
                    const bool cond = resolve(ctx, condPath).truthy();

                    // 配对的 endif：按嵌套深度找，内层的 if/endif 只是把深度加减一下。
                    const std::size_t endifPos =
                        matchOrScan(source, bodyStart, "{% if ", "{% endif %}", 0u, env);
                    // 与它配对的**第一个顶层** else（没有就是 npos）。
                    const std::size_t elsePos =
                        elseOrScan(source, bodyStart, endifPos, env);

                    if (cond)
                    {
                        // then 分支：渲染到 {% else %} 或 {% endif %} 为止。
                        std::size_t branchPos = bodyStart;
                        renderBlock(out, source, branchPos, ctx,
                                    elsePos == std::string::npos ? "{% endif %}" : "{% else %}",
                                    env);
                    }
                    if (!cond && elsePos != std::string::npos)
                    {
                        // else 分支：从 {% else %} 之后渲染到 {% endif %} 为止。
                        std::size_t branchPos = afterTag(source, elsePos);
                        renderBlock(out, source, branchPos, ctx, "{% endif %}", env);
                    }

                    // 不管走了哪条分支，都从配对的 {% endif %} 之后接着渲染。
                    pos = afterTag(source, endifPos);
                }
                else if (commandIs(tag, "for"))
                {
                    // ---- 循环：{% for item in list.path %} … {% endfor %} ----
                    const std::size_t endforPos =
                        matchOrScan(source, bodyStart, "{% for ", "{% endfor %}", 1u, env);
                    const std::string_view spec = trimView(tag.substr(3)); // "item in list.path"
                    const std::size_t inPos = spec.find(" in ");

                    if (inPos == std::string_view::npos)
                    {
                        // 写坏的 for：整块跳过（不能只跳标签，否则会连着把后面吃掉）。
                        pos = afterTag(source, endforPos);
                    }
                    else
                    {
                        const std::string itemName(trimView(spec.substr(0, inPos)));
                        const std::string listPath(trimView(spec.substr(inPos + 4)));
                        const Value list = resolve(ctx, listPath);

                        // 列表不存在或为空：整块跳过（但位置要挪到 endfor 之后）。
                        if (list.isArray() && !list.asArray()->items.empty())
                        {
                            const std::vector<Value> &elements = list.asArray()->items;
                            const std::int64_t elementCount =
                                static_cast<std::int64_t>(elements.size());
                            // 循环变量名在整段循环里不变：子上下文只建一次，每轮
                            // 只换"当前元素 + 下标"，省掉每轮拷一次变量名。
                            ChildModel child(ctx, itemName, Value{});
                            for (std::size_t index = 0u; index < elements.size(); ++index)
                            {
                                // 每轮只换"当前元素 + 下标"，其余走作用域链。
                                child.setItem(elements[index],
                                              static_cast<std::int64_t>(index),
                                              elementCount);
                                std::size_t bodyPos = bodyStart;
                                renderBlock(out, source, bodyPos, child, "{% endfor %}", env);
                            }
                        }
                        pos = afterTag(source, endforPos);
                    }
                }
                else if (commandIs(tag, "raw"))
                {
                    // ---- 原样段：{% raw %} … {% endraw %} ----
                    // 里面的 {{ }} / {% %} / {# #} 一个字都不当标签，直接抄进产物。
                    // 生成 C / C# 这类语言的模板常要写字面量花括号（比如
                    // 说明 `{{ target_code }}` 这种文法写法），就靠它来"转义"。
                    const std::size_t rawEnd =
                        matchOrScan(source, bodyStart, "{% raw %}", "{% endraw %}", 2u, env);
                    out.append(source, bodyStart, rawEnd - bodyStart);
                    pos = afterTag(source, rawEnd);
                }
                else if (commandIs(tag, "include"))
                {
                    // ---- 片段：{% include "名字" %} ----
                    // 片段正文取自调用方给的片段表，**按模板渲染**：用的是同一个
                    // ctx，所以片段里能用外层变量、放进循环体里也能取到循环变量。
                    const std::string name = includeName(tag);
                    const auto found = env.snippets.find(name);
                    if (!name.empty() && (found != env.snippets.end()))
                    {
                        if (env.includeDepth >= k_maxIncludeDepth)
                        {
                            // 片段互相包含（哪怕绕了好几层）在此打住，别把栈转爆。
                            TMPL_THROW("{% include %} 嵌套过深（疑似片段互相包含）: " +
                                       name);
                        }
                        // 独占一行时，这一行前面的缩进已经被 consumeBlockTagLine
                        // 从 out 末尾收回去了——所以片段正文接在"收干净之后"的
                        // out 后面，不会多出缩进，也不会多出空行。
                        const Env nested{env.snippets, env.includeDepth + 1u, env.memo};
                        std::size_t snippetPos = 0u;
                        renderBlock(out, found->second, snippetPos, ctx,
                                    std::string_view(), nested);
                    }
                    // 表里没这个名字 / 没给表 / 标签里没写名字：输出空串，安全跳过。
                    pos = bodyStart;
                }
                else
                {
                    // {% else %} / {% endif %} / {% endfor %} 走到这里说明配对错乱，
                    // 不认识的指令也在这里：安全跳过（只删标签本身，不删内容）。
                    pos = bodyStart;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    //  嵌套匹配
    // -----------------------------------------------------------------------

    /**
     * @brief 块配对位置的取值口：先查记忆表，查不到才真扫一遍并存进去。
     *
     * 表格模板的循环体里同一个 `{% if %}` 会被求值几十万次，而这个位置只跟
     * "模板 + 起点"有关，所以第一次扫完就记住。`kind` 只用做记忆表的键
     * （0 = if，1 = for，2 = raw，3 = 顶层 else），不影响扫描逻辑。
     *
     * @param source 模板字符串。
     * @param pos 块标签之后的起点。
     * @param openTag 开始标签（如 `"{% if "`）。
     * @param closeTag 结束标签（如 `"{% endif %}"`）。
     * @param kind 块种类（记忆表键的一部分）。
     * @param env 渲染环境（记忆表挂在它上面；为空就每次真扫）。
     * @return 配对结束标签的位置。
     * @throws std::runtime_error 找不到配对的结束标签。
     */
    static std::size_t matchOrScan(const std::string &source,
                                   std::size_t pos,
                                   std::string_view openTag,
                                   std::string_view closeTag,
                                   std::size_t kind,
                                   const Env &env)
    {
        if (env.memo == nullptr) return findMatching(source, pos, openTag, closeTag);

        const MatchKey key{source.data(), pos, kind};
        const auto found = env.memo->find(key);
        if (found != env.memo->end()) return found->second;

        const std::size_t result = findMatching(source, pos, openTag, closeTag);
        env.memo->emplace(key, result);
        return result;
    }

    /**
     * @brief `{% if %}` 顶层 `{% else %}` 位置的取值口：同样先查记忆表。
     *
     * 键里的起点 `from` 唯一决定扫描终点 `to`（= 配对 endif），所以不必把 `to`
     * 也放进键。
     *
     * @param source 模板字符串。
     * @param from 查找起点（`if` 标签之后）。
     * @param to 查找终点（配对 `{% endif %}` 的位置）。
     * @param env 渲染环境（记忆表挂在它上面；为空就每次真扫）。
     * @return `{% else %}` 的位置；没有返回 `npos`。
     */
    static std::size_t elseOrScan(const std::string &source,
                                  std::size_t from,
                                  std::size_t to,
                                  const Env &env)
    {
        if (env.memo == nullptr) return findTopLevelElse(source, from, to);

        const MatchKey key{source.data(), from, 3u};
        const auto found = env.memo->find(key);
        if (found != env.memo->end()) return found->second;

        const std::size_t result = findTopLevelElse(source, from, to);
        env.memo->emplace(key, result);
        return result;
    }

    /**
     * @brief 找与 `openTag` 配对的 `closeTag`，按**嵌套深度**取最外层那个。
     *
     * 例如从 `{% if a %}` 之后找 `{% endif %}`：中间每遇到一个 `{% if … %}`
     * 深度 +1、每遇到一个 `{% endif %}` 深度 -1，深度回到 0 的那个才是要的
     * ——内层的 endif 不会被当成外层的结束。
     *
     * @param source 模板字符串。
     * @param pos 开始找的位置（通常是开始标签之后）。
     * @param openTag 开始标签（如 `"{% if "`），只有指令名有用。
     * @param closeTag 结束标签（如 `"{% endif %}"`）。
     * @return 配对结束标签的起始位置。
     * @throws std::runtime_error 找不到配对的结束标签。
     */
    static std::size_t findMatching(const std::string &source,
                                    std::size_t pos,
                                    std::string_view openTag,
                                    std::string_view closeTag)
    {
        const std::string_view open = tagCommand(openTag);
        const std::string_view close = tagCommand(closeTag);
        int depth = 1;
        std::size_t cur = pos;

        while (true)
        {
            const std::size_t tagPos = nextTag(source, cur, source.size());
            if (tagPos == std::string::npos)
                TMPL_THROW("未闭合标签: " + std::string(closeTag));
            const std::size_t end = source.find("%}", tagPos);
            if (end == std::string::npos)
                TMPL_THROW("未闭合标签: " + std::string(closeTag));

            const std::string_view command = trimView(
                std::string_view(source).substr(tagPos + 2, end - tagPos - 2));
            if (!open.empty() && commandIs(command, open))
            {
                ++depth; // 又开了一层
            }
            else if (command == close)
            {
                --depth; // 关掉一层
                if (depth == 0) return tagPos;
            }

            cur = end + 2;
        }
    }

    /**
     * @brief 找 `[from, to)` 之间**顶层**的第一个 `{% else %}`。
     *
     * "顶层"= `if` 嵌套深度为 0：内层 if 自己的 else 不算（它属于内层的
     * `{% endif %}`），否则 `{% else %}` 会被判错。
     *
     * @param source 模板字符串。
     * @param from 开始找的位置。
     * @param to 结束位置（不含）。
     * @return `{%` 的位置；没有顶层 else 返回 `npos`。
     */
    static std::size_t findTopLevelElse(const std::string &source, std::size_t from, std::size_t to)
    {
        int depth = 0;
        std::size_t pos = from;

        while (true)
        {
            const std::size_t tagPos = nextTag(source, pos, to);
            if (tagPos == std::string::npos) return std::string::npos;
            const std::size_t end = source.find("%}", tagPos);
            if (end == std::string::npos) return std::string::npos;

            const std::string_view command = trimView(
                std::string_view(source).substr(tagPos + 2, end - tagPos - 2));
            if (commandIs(command, "if")) ++depth;                   // 内层 if
            else if (command == "endif") { if (depth > 0) --depth; }  // 内层 endif
            else if (command == "else" && depth == 0) return tagPos;  // 外层等着的那个

            pos = end + 2;
        }
    }

    // -----------------------------------------------------------------------
    //  空白处理
    // -----------------------------------------------------------------------

    /**
     * @brief 去掉字符串两端的空白。
     * @param text 文本。
     * @return 去掉两端空白后的文本。
     */
    static std::string trim(const std::string &text)
    {
        return std::string(trimView(text));
    }

    /**
     * @brief 去掉两端的空白（返回视图，不拷贝）。
     *
     * 热路径（每个标签、每个变量都要 trim 一次）用它：只挪指针，不分配。
     *
     * @param text 文本视图（**必须**在返回值使用期间保持有效）。
     * @return 去掉两端空白后的视图。
     */
    static std::string_view trimView(std::string_view text)
    {
        std::size_t begin = 0;
        std::size_t end = text.size();
        while (begin < end && isSpace(text[begin])) ++begin;
        while (end > begin && isSpace(text[end - 1])) --end;
        return text.substr(begin, end - begin);
    }

    /**
     * @brief 去掉 `text` 右端的**行内空白**（空格 / 制表符），就地截断。
     *
     * 块标签独占一行时要"整行删掉"：标签左边那点缩进在发现标签之前就已经
     * 写进 `out` 了，用它把缩进收回来（换行要留着，否则两行会粘在一起）。
     * 就地 `resize` 而不是返回新字符串——热路径上每个块标签都要走一次。
     *
     * @param text 文本（就地截断）。
     */
    static void trimRightInPlace(std::string &text)
    {
        std::size_t end = text.size();
        while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
        text.resize(end);
    }

    /**
     * @brief 判断 `[tagStart, tagEnd)` 这个块标签是不是**独占一行**。
     *
     * 条件：标签前面（本行内）只有空格 / 制表符，标签后面只有空格 / 制表符
     * 然后就是换行或模板结束。这种标签通常只是"给模板用的"，整行删掉才不会
     * 在生成代码里留一堆空行；`{% if x %}public {% endif %}class` 这种同行里
     * 还有别的内容的标签不会被判成独占一行。
     *
     * @param source 模板字符串。
     * @param tagStart 标签起始位置（`{%` 或 `{#`）。
     * @param tagEnd 标签之后的位置（`%}` 或 `#}` 之后再往后一格）。
     * @return 独占一行返回 true。
     */
    static bool isStandaloneLine(const std::string &source, std::size_t tagStart, std::size_t tagEnd)
    {
        // 前面：往左退到行首，中间只能有空格 / 制表符。**碰到第一个非空白就
        // 立刻判负**——一行很长的表格行里，标签左边通常紧挨着上一个单元格的
        // 内容，退一步就能收工，不必先算出行首再整段扫一遍。
        std::size_t index = tagStart;
        while (index > 0 && source[index - 1] != '\n')
        {
            if (!isSpace(source[index - 1])) return false;
            --index;
        }

        // 后面：往右退到行尾，中间只能有空格 / 制表符（\r 归 isSpace 管）。
        // 同理，碰到第一个非空白立刻判负。
        index = tagEnd;
        while (index < source.size())
        {
            if (source[index] == '\n') return true;
            if (!isSpace(source[index])) return false;
            ++index;
        }
        return true;
    }

    /**
     * @brief 吃掉 `pos` 起的行尾行内空白与一个换行。
     * @param source 模板字符串。
     * @param pos 位置（通常是块标签之后）。
     * @return 换行之后的位置。
     */
    static std::size_t skipLineEnd(const std::string &source, std::size_t pos)
    {
        while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r')) ++pos;
        if (pos < source.size() && source[pos] == '\n') ++pos;
        return pos;
    }

    /**
     * @brief 块标签之后该从哪继续：独占一行时连行尾空白与换行一起跳过。
     * @param source 模板字符串。
     * @param tagStart 标签起始位置。
     * @param tagEnd 标签之后的位置。
     * @return 继续渲染的位置。
     */
    static std::size_t nextPosAfterTag(const std::string &source, std::size_t tagStart, std::size_t tagEnd)
    {
        if (!isStandaloneLine(source, tagStart, tagEnd)) return tagEnd;
        return skipLineEnd(source, tagEnd);
    }

    /**
     * @brief 已经处理完一个块标签：独占一行时把这一行整行删掉。
     *
     * 两件事：把 `out` 末尾已经写出去的本行缩进收回来（`trimRightInPlace`），再把
     * 标签之后到行尾的空白连同换行吃掉（`skipLineEnd`）。
     *
     * @param source 模板字符串。
     * @param tagStart 标签起始位置。
     * @param tagEnd 标签之后的位置。
     * @param out 当前已经产出的文本（会被就地裁掉行首缩进）。
     * @return 继续渲染的位置。
     */
    static std::size_t consumeBlockTagLine(const std::string &source,
                                           std::size_t tagStart,
                                           std::size_t tagEnd,
                                           std::string &out)
    {
        if (!isStandaloneLine(source, tagStart, tagEnd)) return tagEnd;
        trimRightInPlace(out); // 收回本行的缩进（换行保留）
        return skipLineEnd(source, tagEnd);
    }

    /**
     * @brief 从 `tagPos`（`{%` 的位置）出发，返回这个标签之后该继续的位置。
     * @param source 模板字符串。
     * @param tagPos 标签起始位置。
     * @return 继续渲染的位置（独占一行时已经跳过整行）。
     */
    static std::size_t afterTag(const std::string &source, std::size_t tagPos)
    {
        const std::size_t end = source.find("%}", tagPos);
        if (end == std::string::npos) return source.size();
        return nextPosAfterTag(source, tagPos, end + 2);
    }

    // -----------------------------------------------------------------------
    //  标签扫描与识别
    // -----------------------------------------------------------------------

    /**
     * @brief 从 `pos` 开始找下一个**标签起点**（`{{` / `{%` / `{#`）。
     *
     * 只扫一次 `{`：模板里除了这三种开头，几乎不会出现别的花括号（`{% raw %}`
     * 段里才有），所以"找 `{` 再看后一个字符"比分别找三个两字符记号快得多
     * ——后者每次都要把"找不到"的那种一路扫到模板末尾，热循环里就是几十万次
     * 全模板扫描。
     *
     * @param source 模板字符串。
     * @param pos 起始位置。
     * @return 标签起点；后面没有标签返回 `npos`。
     */
    static std::size_t nextMarker(const std::string &source, std::size_t pos)
    {
        std::size_t at = source.find('{', pos);
        while (at != std::string::npos)
        {
            if (at + 1 >= source.size()) return std::string::npos;
            const char kind = source[at + 1];
            if (kind == '{' || kind == '%' || kind == '#') return at;
            at = source.find('{', at + 1); // 普通花括号（比如 C 代码里的大括号）：接着找
        }
        return std::string::npos;
    }

    /**
     * @brief 从 `pos` 开始找下一个 `{% … %}` 标签，沿途跳过 `{# … #}` 注释。
     * @param source 模板字符串。
     * @param pos 起始位置。
     * @param limit 只在这个位置之前找（不含）。
     * @return 标签起始位置；找不到返回 `npos`。
     */
    static std::size_t nextTag(const std::string &source, std::size_t pos, std::size_t limit)
    {
        while (pos < limit)
        {
            const std::size_t markerPos = nextMarker(source, pos);
            if (markerPos == std::string::npos) return std::string::npos;

            // 注释里的 {% endif %} 之类的文字不算标签。
            if (source[markerPos + 1] == '#')
            {
                const std::size_t end = source.find("#}", markerPos);
                if (end == std::string::npos) return std::string::npos; // 注释没闭合：后面都不算标签
                pos = end + 2;
                continue;
            }

            // `{{`（变量）不是块标签：只跳过这一个 `{` 接着找。**不能跳过两个字符**
            // ——`{% raw %}{{% endraw %}` 这种"原样段里就一个左花括号"的写法，
            // 跳过两个会把紧跟着的 `{%` 吃掉。
            if (source[markerPos + 1] == '{')
            {
                pos = markerPos + 1;
                continue;
            }

            if (markerPos >= limit) return std::string::npos;
            return markerPos;
        }
        return std::string::npos;
    }

    /**
     * @brief 把字面标签拆成指令名：`"{% endif %}"` → `"endif"`。
     * @param tag 字面标签或指令名。
     * @return 指令名（视图，指向 `tag` 自己的内存，不分配）。
     */
    static std::string_view tagCommand(std::string_view tag)
    {
        std::string_view out = trimView(tag);
        if (out.size() >= 2 && out.compare(0, 2, "{%") == 0) out.remove_prefix(2);
        if (out.size() >= 2 && out.compare(out.size() - 2, 2, "%}") == 0) out.remove_suffix(2);
        return trimView(out);
    }

    /**
     * @brief 标签（已 trim）的指令名是不是 `command`。
     *
     * `commandIs("if has_ctor", "if")` 为真，`commandIs("endif", "if")` 为假
     * ——多了个 `end` 前缀就是另一条指令。
     *
     * @param tag 标签内容（已 trim）。
     * @param command 指令名，如 `"if"`。
     * @return 是这条指令返回 true。
     */
    static bool commandIs(std::string_view tag, std::string_view command)
    {
        if (tag == command) return true;
        return tag.size() > command.size() &&
               tag.compare(0, command.size(), command) == 0 &&
               isSpace(tag[command.size()]);
    }

    /**
     * @brief 从 `{% include … %}` 的标签里取出**片段名**。
     *
     * 三种写法都认：
     *
     * ```
     * {% include "header" %}      // 双引号
     * {% include 'header' %}      // 单引号
     * {% include header %}        // 不带引号（取到空白为止）
     * ```
     *
     * 名字里不认转义（片段名不是变量，走的是片段表的键，不需要转义）；
     * `include` 后面什么都没有、或者引号没闭合，都返回空串——调用方按
     * "取不到片段"处理（输出空串，不报错）。
     *
     * 实现上不数 `include` 有几个字母，而是从第一个空白处切开：反正调用点
     * 已经用 `commandIs(tag, "include")` 判过指令名了。
     *
     * @param tag 标签内容（已 trim，如 `include "header"`）。
     * @return 片段名；取不到返回空串。
     */
    static std::string includeName(std::string_view tag)
    {
        const std::size_t blank = tag.find_first_of(" \t\r\n");
        if (blank == std::string_view::npos) return std::string(); // 光有指令名，没写名字
        const std::string_view rest = trimView(tag.substr(blank));
        if (rest.empty()) return std::string();

        const char quote = rest[0];
        if ((quote == '"') || (quote == '\''))
        {
            const std::size_t close = rest.find(quote, 1u);
            if (close == std::string_view::npos) return std::string(); // 引号没闭合
            return std::string(rest.substr(1u, close - 1u));
        }
        const std::size_t stop = rest.find_first_of(" \t\r\n");
        return std::string((stop == std::string_view::npos) ? rest : rest.substr(0u, stop));
    }

    /**
     * @brief 判断 `source` 的 `pos` 处是不是 `stopTag`。
     *
     * `stopTag` 传的是字面标签（`"{% endif %}"`），但这里同时接受
     * `{%endif%}` 这种不留空格的写法：字面匹配失败时，把标签内容 trim 出来
     * 再和 `stopTag` 的指令名比一次。
     *
     * @param source 模板字符串。
     * @param pos 位置。
     * @param stopTag 停止标签。
     * @return 是停止标签返回 true。
     */
    static bool isStopTag(const std::string &source, std::size_t pos, std::string_view stopTag)
    {
        if (source.compare(pos, stopTag.size(), stopTag.data(), stopTag.size()) == 0) return true;
        if (source.compare(pos, 2, "{%") != 0) return false;
        const std::size_t end = source.find("%}", pos + 2);
        if (end == std::string::npos) return false;
        const std::string_view command = trimView(
            std::string_view(source).substr(pos + 2, end - pos - 2));
        return command == tagCommand(stopTag);
    }

    /**
     * @brief 标签内容（已 trim）是不是 `stopTag` 对应的指令。
     * @param tag 标签内容（已 trim）。
     * @param stopTag 停止标签。
     * @return 是停止指令返回 true。
     */
    static bool matchesStopCommand(std::string_view tag, std::string_view stopTag)
    {
        return tag == tagCommand(stopTag);
    }

    /**
     * @brief 是不是空白字符（模板里的空白按 ASCII 处理，不认 locale）。
     * @param ch 字符。
     * @return 空白返回 true。
     */
    static bool isSpace(char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
    }
};

/**
 * @brief 直接按"名字 → 值"渲染模板（省掉为此专门写模型类）。
 *
 * ```cpp
 * const std::string text = tmpl::renderValues(source, {
 *     {"mode_name", modeLabelText(mode)},
 *     {"ns", nameSpace},
 * });
 * ```
 *
 * 模板里有 `{% include %}` 时把片段表一起传进来（不传就是空表，include 输出空串）：
 *
 * ```cpp
 * const tmpl::SnippetTable snippets = {
 *     {"header", "// 由 {{ name }} 自动生成\n"},
 *     {"body",   "{% include \"header\" %}int main() {}\n"},
 * };
 * tmpl::renderValues(source, {{"name", "calc"}}, snippets);
 * ```
 *
 * @param source 模板文本。
 * @param values 名字 / 值对。
 * @param snippets `{% include %}` 的片段表（名字 → 片段正文）。
 * @return 渲染结果。
 * @throws std::runtime_error 模板里有标签没闭合，或 include 嵌套过深。
 */
inline std::string renderValues(
    const std::string &source,
    std::initializer_list<std::pair<std::string, Value>> values = {},
    const SnippetTable &snippets = noSnippets())
{
    const MapModel model(values);
    return TemplateEngine::render(source, model, snippets);
}

/**
 * @brief 便捷渲染：把模型类包成 `ModelAdapter` 再交给 `TemplateEngine`。
 *
 * ```cpp
 * const std::string code = tmpl::render(csharp_tmpl, model);
 * ```
 *
 * @tparam T 模型类（需要有 `TEMPLATE_MODEL` 绑定）。
 * @param source 模板字符串（引擎不读文件，模板由调用方给）。
 * @param model 模型对象。
 * @param snippets `{% include %}` 的片段表（名字 → 片段正文）；不传就是空表。
 * @return 渲染结果。
 * @throws std::runtime_error 模板里有标签没闭合，或 include 嵌套过深。
 */
template <class T>
std::string render(const std::string &source, const T &model,
                   const SnippetTable &snippets = noSnippets())
{
    const ModelAdapter<T> adapter(model);
    return TemplateEngine::render(source, adapter, snippets);
}

} // namespace tmpl

#endif // PARSE_ENGINE_TEMPLATE_ENGINE_HPP
