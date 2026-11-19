/**
 * parser.cpp —— LL(1) 解析器实现（C++17）
 *
 * 由 HareParse 自动生成，请勿手工修改。
 *
 * 文件布局：
 *   1) 未命名命名空间：生成期算好的分析表 / 词法 DFA / 树与动作辅助函数；
 *   2) Engine::Private ：词法器与分析驱动（Qt 风格 d-pointer）；
 *   3) 公开 API 定义   ：statusName / tokenName / tokenText / Tree / Engine。
 *
 * 内存全部由标准库管理（ParserMemory::String / std::string_view / std::vector /
 * std::unique_ptr），可由 PARSER_MEMORY_NAMESPACE 指定自定义分配器。
 */

#include "parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Parser
{

namespace
{
/* ============ 生成期算好的表：词法 DFA / ACTION-GOTO / 预测分析表 ============ */

/// Token 名称表：下标就是 TokenType 的值。
inline constexpr const char *const kTokenNames[][2] =
{
    {"STRING", ""},
    {"NUMBER", ""},
    {"VARIABLE", ""},
    {"LBRACE", ""},
    {"RBRACE", ""},
    {"LBRACKET", ""},
    {"RBRACKET", ""},
    {"COLON", ""},
    {"COMMA", ""},
    {"TRUE", ""},
    {"FALSE", ""},
    {"NULL", ""},
    {"END", ""},
    {"INVALID", ""}
};

/* ---------- 词法表 ---------- */
/// 固定字面量词法规则（按长度降序；表尾是一个哨兵）。
struct LexRule
{
    const char *text;     ///< 字面量（UTF-8；\b 通配写成 0x1F）
    std::uint32_t length; ///< 字节长度
    TokenType type;       ///< 命中的 Token 类型
    bool blankFirst;      ///< 首字节是空白 / \b：跳空白时先看它
    bool wildcard;        ///< 含 \b 通配：命中后词素按输入抄一份
    int order;            ///< 词法声明序（%commit 同长度比较用）
};

inline constexpr LexRule kLexRules[] =
{
    { "false", 5u, TOKEN_FALSE, false, false, 7 },
    { "null", 4u, TOKEN_NULL, false, false, 8 },
    { "true", 4u, TOKEN_TRUE, false, false, 6 },
    { ":", 1u, TOKEN_COLON, false, false, 4 },
    { ",", 1u, TOKEN_COMMA, false, false, 5 },
    { "{", 1u, TOKEN_LBRACE, false, false, 0 },
    { "[", 1u, TOKEN_LBRACKET, false, false, 2 },
    { "}", 1u, TOKEN_RBRACE, false, false, 1 },
    { "]", 1u, TOKEN_RBRACKET, false, false, 3 },
    { nullptr, 0u, TokenType::TOKEN_END, false, false, 0 }
};

/// 关键字表项。
struct Keyword
{
    const char *text; ///< 字面量
    TokenType type;   ///< 对应的 Token
};

/// 关键字表：%define / %token 里像标识符的字面量。
inline constexpr Keyword kKeywords[] =
{
    { "false", TOKEN_FALSE },
    { "null", TOKEN_NULL },
    { "true", TOKEN_TRUE }
};

/**
 * @brief 关键字查表。
 * @param pText 文本。
 * @return 关键字对应的 Token；不是关键字时返回 TOKEN_INVALID。
 */
inline TokenType keywordType(const char *pText)
{
    if (pText == nullptr)
    {
        return TokenType::TOKEN_INVALID;
    }
    for (const Keyword &entry : kKeywords)
    {
        if (std::strcmp(pText, entry.text) == 0)
        {
            return entry.type;
        }
    }
    return TokenType::TOKEN_INVALID;
}

/* %token regex / %identifier / %commit 的正则：用标准库 <regex> 在每个
 * 位置各做一次最长匹配（ECMAScript 语法，见 lexRegexMatch）。 */
using LexRegex = std::regex;
/** @brief 选择显式 Unicode 程序，或将 (?i) 转换为标准库选项。
 * @param pattern 正则文本。 @return 已编译规则。
 */
inline LexRegex lexCompileRegex(const char *pattern = "")
{
    bool ignoreCase = false;
    while (std::strncmp(pattern, "(?", 2u) == 0)
    {
        const char *end = pattern + 2u;
        while (*end == 'i') ++end;
        if (end == pattern + 2u || *end != ')') break;
        ignoreCase = true;
        pattern = end + 1u;
    }
    const auto flags = std::regex_constants::ECMAScript |
        (ignoreCase ? std::regex_constants::icase : std::regex_constants::syntax_option_type{});
    return std::regex(pattern, flags);
}
/* %commit 声明的片段：与 %token 一起做最长匹配；同长度时按声明序先声明优先。 */
inline const LexRegex kCommitRes[] =
{
    lexCompileRegex()
};
inline constexpr const char *const kCommitTypes[][2] =
{
    { nullptr, nullptr }
};
inline constexpr std::uint8_t kCommitSkips[] =
{
    0u
};
inline constexpr int kCommitOrders[] =
{
    0
};
inline constexpr std::size_t kCommitCount = 0u;

/**
 * @brief 用 std::regex 从 text[pos] 起匹配，返回匹配到的字节数（0 = 不匹配）。
 *
 * 每条规则按 ECMAScript 的分支顺序及贪婪 / 非贪婪语义匹配；词法器再在
 * 各条规则的结果之间取最长者。`match_continuous` 保证匹配必须从
 * pos 起（std::regex_search 默认是往后找）。
 * @param pattern 正则。
 * @param text 输入文本。
 * @param pos 起始位置。
 * @param limits 有序运行时的可选预算，标准库路径不提供预算保证。
 * @param result 可空的结构化结果输出。
 * @return 匹配到的字节数（0 表示不匹配）。
 */
inline std::uint32_t lexRegexMatch(const LexRegex &pattern,
                                   const ParserMemory::String &text,
                                   std::size_t pos,
                                   const PeRegexLimits *limits = nullptr,
                                   PeRegexResult *result = nullptr)
{
    if (pos > text.size())
    {
        return 0u;
    }
    const std::regex &standard = pattern;
    if (result != nullptr) *result = {};
    (void)limits;
    std::cmatch match;
    const char *const pBegin = text.data() + pos;
    const char *const pEnd = text.data() + text.size();
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
#endif
    {
        if (!std::regex_search(pBegin, pEnd, match, standard,
                               std::regex_constants::match_continuous)) return 0u;
        if (result != nullptr) {
            result->status = PE_REGEX_MATCH;
            result->byteLength = static_cast<std::size_t>(match.length());
        }
        return static_cast<std::uint32_t>(match.length());
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    catch (const std::bad_alloc &) {
        if (result != nullptr) result->status = PE_REGEX_NO_MEMORY;
        return 0u;
    } catch (const std::regex_error &error) {
        if (result != nullptr) result->status =
            (error.code() == std::regex_constants::error_space ||
             error.code() == std::regex_constants::error_stack) ? PE_REGEX_NO_MEMORY : PE_REGEX_LIMIT;
        return 0u;
    }
#endif
}


/* ---------- 分析表 ---------- */
/* 预测分析表：单元里的产生式编号，-1 表示没有可用产生式。 */
inline constexpr std::int16_t kLlTable[11][13] =
{
    { -1, -1, -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, -1 },
    { 1, 1, -1, 1, -1, 1, -1, -1, -1, 1, 1, 1, -1 },
    { 2, 2, -1, 2, -1, 2, -1, -1, -1, 2, 2, 2, -1 },
    { 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, 5, -1, 4, -1, -1, -1, -1 },
    { -1, -1, -1, -1, 7, -1, -1, -1, 6, -1, -1, -1, -1 },
    { -1, -1, -1, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 9, 9, -1, 9, -1, 9, 10, -1, -1, 9, 9, 9, -1 },
    { 11, -1, -1, -1, 12, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 16, 17, -1, 14, -1, 15, -1, -1, -1, 18, 19, 20, -1 }
};
/// LL(k)：预测分析表的列编码（升序）与对应的列号。
inline constexpr std::int64_t kLookaheadCode[1] =
{
    0
};
inline constexpr std::uint16_t kLookaheadColumn[1] =
{
    0u
};
/// FOLLOW 集：true 表示这个终结符在该非终结符的 FOLLOW 里（LL 的错误恢复用）。
inline constexpr bool kLlFollow[11][13] =
{
    { false, false, false, false, true, false, true, false, true, false, false, false, true },
    { false, false, false, false, false, false, true, false, false, false, false, false, false },
    { false, false, false, false, false, false, false, false, false, false, false, false, true },
    { false, false, false, false, true, false, false, false, false, false, false, false, false },
    { false, false, false, false, false, false, true, false, false, false, false, false, false },
    { false, false, false, false, true, false, false, false, false, false, false, false, false },
    { false, false, false, false, true, false, true, false, true, false, false, false, true },
    { false, false, false, false, false, false, true, false, false, false, false, false, false },
    { false, false, false, false, true, false, false, false, false, false, false, false, false },
    { false, false, false, false, true, false, false, false, true, false, false, false, false },
    { false, false, false, false, true, false, true, false, true, false, false, false, true }
};

inline constexpr std::uint8_t kProductionIsLeftEmpty[] =
{
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
};



/* ---------- 语法树表 ---------- */
/* 语法树元数据：右部符号表（终结符用下标，非终结符用 kTerminalCount + 下标）。 */
inline constexpr std::uint16_t treeProdStart[] =
{
    0u,
    3u,
    5u,
    6u,
    8u,
    10u,
    10u,
    12u,
    12u,
    15u,
    16u,
    16u,
    17u,
    17u,
    20u,
    21u,
    22u,
    23u,
    24u,
    25u,
    26u
};
inline constexpr std::uint16_t treeProdLength[] =
{
    3u,
    2u,
    1u,
    2u,
    2u,
    0u,
    2u,
    0u,
    3u,
    1u,
    0u,
    1u,
    0u,
    3u,
    1u,
    1u,
    1u,
    1u,
    1u,
    1u,
    1u
};
inline constexpr std::uint16_t treeProdSymbols[] =
{
    5u,
    20u,
    6u,
    23u,
    17u,
    23u,
    22u,
    18u,
    8u,
    14u,
    8u,
    16u,
    3u,
    21u,
    4u,
    14u,
    16u,
    0u,
    7u,
    23u,
    19u,
    13u,
    0u,
    1u,
    9u,
    10u,
    11u
};



/* ============ 内部常量、Token 语义值钩子与错误表 ============ */

/// 文法里是否写了 error 产生式（写了才能把 error 当伪 Token 移进）。
inline constexpr bool kHasErrorToken = false;
/// error 伪终结符在终结符表里的列号（没写 error 产生式时是 0）。
inline constexpr std::uint16_t kErrorTokenId = 0u;
/// 文法里是否声明了 %sync。
inline constexpr bool kHasSyncTokens = false;

/**
 * @brief 一次归约的语义结果。
 *
 * 语义动作里的 `$$` 翻成 `result.value`（`$n` 翻成 `stack->values[...]`），
 * LR / LL 两条驱动共用这一个壳子。
 */
struct ParseResult
{
    ParserValue_T value{}; ///< `$$`
};

/**
 * @brief 把状态码转成错误码数值（与 C 目标的 PARSER_ERR_* 一一对应）。
 * @param status 状态码。
 * @return 数值形式。
 */
inline std::int32_t failCode(Status status) noexcept
{
    return static_cast<std::int32_t>(status);
}

/**
 * @brief 取 Token 类型名（与 C 目标的 Parser_TokenName 同义，越界时 "UNKNOWN"）。
 * @param type Token 类型。
 * @return 只读名字。
 */
inline ParserMemory::String tokenTypeText(TokenType type)
{
    const char *name = tokenName(type);
    return (name != nullptr) ? ParserMemory::String(name) : ParserMemory::String("UNKNOWN");
}

/**
 * @brief 取 Token 的语义值。
 *
 * 用户定义了 PARSER_USER_TOKEN_VALUE(pToken) 就用用户的；没定义时按文法
 * 选的语义值模型给零值（写了 %union 时词法器已经按 `%token <成员>` 把词素
 * 写进 value，直接读 `pToken->value` 就是语义值）。
 */
inline ParserValue_T tokenValue(const Token &token)
{
#ifdef PARSER_USER_TOKEN_VALUE
    return PARSER_USER_TOKEN_VALUE(&token);
#else
    (void)token;
    return ParserValue_T{};
#endif
}

/**
 * @brief 这是一个同步 Token 吗（`%sync`）。
 *
 * 没写 %sync 时恒为 false：错误恢复里那些 panic 代码仍然编译，但永远不生效。
 */
inline bool isSyncToken(std::uint16_t type) noexcept
{
    switch (type)
    {
    default: return false;
    }
}

/* ---------- 文法规模（全部来自生成期，运行期只读） ---------- */

/// 终结符个数（含 END）。
inline constexpr std::uint32_t kTerminalCount = 13u;
/// 非终结符个数。
inline constexpr std::uint32_t kNontermCount = 11u;
/// 起始非终结符编号。
inline constexpr std::uint16_t kStartId = 2u;
/// LR 自动机状态个数（LL(1) 模式没有状态机，这里是 0）。
inline constexpr std::uint32_t kStateCount = 0u;
/// ACTION 表里的「接受」标记。
inline constexpr std::int32_t kAcceptAction = INT32_MAX;
/// LR(k) / LALR(k) 的展望符长度 k（其余模式都是 1）。
inline constexpr std::uint32_t kLookaheadK = 1u;
/**
 * @brief 归约层的列数（k > 1 时才有归约层；k = 1 时为 0）。
 *
 * k > 1 时 ACTION 分两层：`kAction` 放移进，`kReduce` 按 k 元组展望符建列。
 * k = 1 时归约都在 `kAction` 里，这一层不查。
 */
inline constexpr std::uint32_t kLookaheadColumnCount = 0u;
/// 无效下标（对应 C 目标的 PARSER_INVALID_INDEX）。
inline constexpr std::uint16_t kInvalidIndex = 0xFFFFu;
/// 无效状态（对应 C 目标的 UINT16_MAX）。
inline constexpr std::uint16_t kInvalidState = 0xFFFFu;
/// 每条错误最多记几个「期望 Token」；期望表按行（LR 状态 / LL 非终结符）排布。
inline constexpr std::uint32_t kMaxExpected = 8u;
inline constexpr std::uint32_t kExpectedRowCount = 11u;
/// 期望集为空时的兜底说明（LL 说「产生式」、LR 说「动作」；五档同文案）。
inline constexpr const char *kExpectedFallback = "该状态下无可用产生式";
/// 非终结符名表（%tree 建树用；LR 归约时把编号翻成名字）。
inline constexpr const char *const kTreeNontermNames[] =
{
    "array",
    "elements",
    "json",
    "members",
    "more_elements",
    "more_members",
    "object",
    "opt_elements",
    "opt_members",
    "pair",
    "value"
};
/// 每一行「期望哪些 Token」：语法错误时填 Error::expected()。
/// 用 32 位：大文法（上百个 Token）的期望表偏移会超过 16 位范围，
/// 收窄到 uint16 会截断偏移并读越界，这里不能改回 uint16。
inline constexpr std::uint32_t kExpectedStart[12] =
{
    0u, 1u, 8u, 15u, 16u, 18u, 20u, 21u, 29u, 31u, 32u, 39u
};
inline constexpr std::uint32_t kExpectedData[] =
{
    5u, 10u, 3u, 5u, 11u, 1u, 0u, 9u, 10u, 3u, 5u, 11u, 1u, 0u, 9u, 0u, 8u, 6u, 8u, 4u, 3u, 10u, 3u, 5u, 11u, 1u, 6u, 0u, 9u, 4u, 0u, 0u, 10u, 3u, 5u, 11u, 1u, 0u, 9u
};

/* ============ 语法树辅助：建叶子 / 建分支 / 深拷贝 / 打印 ============ */

/// 名字为空时的兜底（保证树里不会出现 nullptr）。
[[maybe_unused]] ParserMemory::String symbolOrPlaceholder(const char *name)
{
    return (name != nullptr) ? ParserMemory::String(name) : ParserMemory::String("?");
}

/// 建一个终结符叶子节点：名字是 Token 类型名，词素是 Token 的显示文本。
[[maybe_unused]] ParserMemory::UniquePtr<Tree> makeLeaf(const Token &token)
{
    ParserLocation location;
    location.line = static_cast<int>(token.line);
    location.column = static_cast<int>(token.column);
    location.offset = static_cast<int>(token.offset);
    return ParserMemory::makeUnique<Tree>(symbolOrPlaceholder(tokenName(token.type)),
                                  tokenText(token), location);
}

/// 把栈上 [base, base + length) 的节点收成一个非终结符节点。
[[maybe_unused]] ParserMemory::UniquePtr<Tree> makeBranch(
    ParserMemory::Vector<ParserMemory::UniquePtr<Tree>> &nodes, const char *name,
    std::size_t base, std::size_t length)
{
    ParserMemory::UniquePtr<Tree> node =
        ParserMemory::makeUnique<Tree>(symbolOrPlaceholder(name));
    for (std::size_t index = 0u; index < length; ++index)
    {
        ParserMemory::UniquePtr<Tree> &child = nodes[base + index];
        if (child != nullptr)
        {
            node->addChild(std::move(child));
        }
    }
    return node;
}

/// 深拷贝一棵树。
[[maybe_unused]] ParserMemory::UniquePtr<Tree> cloneTree(const Tree &source)
{
    ParserMemory::UniquePtr<Tree> copy =
        ParserMemory::makeUnique<Tree>(source.name(), source.text(),
                               source.location());
    for (std::size_t index = 0u; index < source.childCount(); ++index)
    {
        const Tree *child = source.child(index);
        if (child != nullptr)
        {
            copy->addChild(cloneTree(*child));
        }
    }
    return copy;
}

/// 非终结符编号 -> 名字（%tree 建内部分支节点用）。
[[maybe_unused]] inline const char *treeTreeName(std::uint16_t id) noexcept
{
    return (id < kNontermCount) ? kTreeNontermNames[id] : nullptr;
}

/// 递归拼一棵树的缩进文本。
[[maybe_unused]] void appendTreeText(const Tree &node, std::size_t depth,
                                     ParserMemory::String &out)
{
    out.append(depth * 2u, ' ');
    out += node.name();
    if (!node.text().empty())
    {
        out += '(';
        out += node.text();
        out += ')';
    }
    out += '\n';
    for (std::size_t index = 0u; index < node.childCount(); ++index)
    {
        const Tree *child = node.child(index);
        if (child != nullptr)
        {
            appendTreeText(*child, depth + 1u, out);
        }
    }
}

}

class Engine::Private
{
public:
    RegexLimits m_regexLimits{}; ///< 当前实例的正则预算。
    RegexMatchResult m_regexResult{}; ///< 最近匹配或本次解析的首个资源失败。

    /** @brief 统一格式化并保存错误，保留结构化诊断字段。 */
    template<class... Extra>
    void appendError(std::int32_t code, std::uint32_t line, std::uint32_t column,
                     ParserMemory::String message, Extra&&... extra)
    {
        if (m_errorFormatter) message = m_errorFormatter(code, line, column, message);
        m_errors.emplace_back(code, line, column, std::move(message),
                              std::forward<Extra>(extra)...);
    }
    ErrorFormatter m_errorFormatter; ///< 调用方提供的错误提示函数。

    /* ---------- 错误表 ---------- */

    /// @return 本次解析累积的错误。
    ParserMemory::Vector<Error> &errors() noexcept { return m_errors; }

    /// @return 最近一条错误；没有错误时返回 nullptr。
    const Error *lastError() const noexcept
    {
        return m_errors.empty() ? nullptr : &m_errors.back();
    }

    /// 追加一条错误（错误表满时丢弃，与 C 目标的 PARSER_MAX_ERRORS 一致）。
    void setError(std::int32_t code, std::uint32_t line, std::uint32_t column,
                  ParserMemory::String message)
    {
        if (m_errors.size() >= kMaxErrors)
        {
            return;
        }
        appendError(code, line, column, std::move(message));
    }

    /// 记一条「期望某个 Token」的语法错误（终结符不匹配时用，文案与 C 目标一致）。
    void setExpectedOneError(std::uint16_t expectedType)
    {
        const char *pExpected =
            tokenName(static_cast<TokenType>(expectedType));
        ParserMemory::String message = "语法错误：期望 ";
        message += (pExpected != nullptr) ? pExpected : "?";
        message += "，实际是 ";
        message += tokenTypeText(m_current.type);
        if (m_errors.size() >= kMaxErrors)
        {
            return;
        }
        appendError(static_cast<std::int32_t>(Status::ErrExpected),
                              m_current.line, m_current.column,
                              std::move(message),
                              static_cast<std::uint32_t>(m_current.type),
                              ParserMemory::String(m_current.text), 0u,
                              ParserMemory::Vector<std::uint32_t>{static_cast<std::uint32_t>(
                                  expectedType)},
                              buildErrorContext(m_current.line,
                                                m_current.column));
    }

    /**
     * @brief 记一条「语法错误」：带上期望集合、实际 Token、状态号与词素。
     *
     * 文案五档统一：
     *   - 期望集非空：「语法错误：期望 A / B 之一，实际是 C」（只列前 4 个，
     *     其余用「等 N 个」带过；只有一个期望时不写「之一」）；
     *   - 期望集为空：「语法错误：<kExpectedFallback>」。
     * @param row 出错时的行（LR 是状态号、LL 是栈顶符号编号）。
     */
    void setSyntaxError(std::uint32_t row)
    {
        ParserMemory::Vector<std::uint32_t> expected;
        std::uint32_t total = 0u;
        if (row < kExpectedRowCount)
        {
            const std::uint32_t begin = kExpectedStart[row];
            const std::uint32_t end = kExpectedStart[row + 1u];
            total = end - begin;
            for (std::uint32_t at = begin;
                 (at < end) && (expected.size() < kMaxExpected); ++at)
            {
                expected.push_back(kExpectedData[at]);
            }
        }
        const char *pActual =
            kTokenNames[static_cast<std::size_t>(m_current.type)][0];
        ParserMemory::String message = "语法错误：";
        if (total == 0u)
        {
            message += kExpectedFallback;
        }
        else
        {
            const std::uint32_t listed =
                (expected.size() > 4u)
                    ? 4u
                    : static_cast<std::uint32_t>(expected.size());
            for (std::uint32_t at = 0u; at < listed; ++at)
            {
                const char *name =
                    kTokenNames[static_cast<std::size_t>(expected[at])][0];
                message += (at == 0u) ? "期望 " : " / ";
                message += (name != nullptr) ? name : "?";
            }
            if (total > 1u)
            {
                if (total > listed)
                {
                    message += " 等 " + ParserMemory::toString(total) + " 个";
                    message += "之一";
                }
                else
                {
                    message += " 之一";
                }
            }
            message += "，实际是 ";
            message += (pActual != nullptr) ? pActual : "?";
        }
        std::int32_t code = static_cast<std::int32_t>(Status::ErrSyntax);
        if (total != 0u)
        {
            code = (m_current.type == TokenType::TOKEN_END)
                       ? static_cast<std::int32_t>(Status::ErrUnexpectedEof)
                       : static_cast<std::int32_t>(Status::ErrExpected);
        }
        if (m_errors.size() >= kMaxErrors)
        {
            return;
        }
        appendError(code, m_current.line, m_current.column, message,
                              static_cast<std::uint32_t>(m_current.type),
                              ParserMemory::String(m_current.text), row,
                              std::move(expected),
                              buildErrorContext(m_current.line,
                                                m_current.column));
    }

    /**
     * @brief 拼「出错那一行原文 + 一行 ^ 指示」（文本模式才有源文）。
     * @param line 行号（1 起）。
     * @param column 列号（1 起）。
     * @return 两行文本；拿不到源文时返回空串。
     */
    ParserMemory::String buildErrorContext(std::uint32_t line,
                                  std::uint32_t column) const
    {
        if (m_input.empty())
        {
            return ParserMemory::String();
        }
        std::size_t start = 0u;
        std::uint32_t current = 1u;
        for (std::size_t index = 0u; index < m_input.size(); ++index)
        {
            if (m_input[index] == '\n')
            {
                ++current;
                if (current == line)
                {
                    start = index + 1u;
                    break;
                }
            }
        }
        if (current != line)
        {
            return ParserMemory::String();
        }
        std::size_t end = m_input.find_first_of("\r\n", start);
        if (end == ParserMemory::String::npos)
        {
            end = m_input.size();
        }
        std::size_t caret = (column > 0u) ? (column - 1u) : 0u;
        if (caret > (end - start))
        {
            caret = end - start;
        }
        return m_input.substr(start, end - start) + "\n" +
               ParserMemory::String(caret, ' ') + "^";
    }

    /// 动作里的 `$token`：当前 lookahead 的文本（空文本返回空串）。
    ParserMemory::String currentTokenText() const
    {
        return ParserMemory::String(m_current.text);
    }

    /// 当前 lookahead 的位置；动作里的 `$line` / `$column` 读它。
    ParserLocation currentLocation() const noexcept
    {
        ParserLocation location;
        location.line = static_cast<int>(m_current.line);
        location.column = static_cast<int>(m_current.column);
        location.offset = static_cast<int>(m_current.offset);
        return location;
    }

    /// 动作里的 `$code`：最近一条错误的码；还没有错误时返回 0。
    std::int32_t lastErrorCode() const noexcept
    {
        const Error *entry = lastError();
        return (entry != nullptr) ? entry->code() : 0;
    }

    /// 动作里的 `$message`：最近一条错误的说明；还没有错误时返回空串。
    ParserMemory::String lastErrorMessage() const
    {
        const Error *entry = lastError();
        return (entry != nullptr) ? entry->message() : ParserMemory::String();
    }

    /* ---------- 解析入口 ---------- */

    /**
     * @brief 文本模式：把输入拷一份（保证 '\0' 结尾）并读入第一个词法单元。
     * @param text 输入文本。
     * @return Status::Ok 表示就绪。
     */
    Status prepareText(std::string_view text)
    {
        m_input.assign(text);
        m_text = m_input.c_str();
        m_textPos = 0u;
        m_textMode = true;
        return prepareCommon();
    }

    /**
     * @brief Token 模式：直接用调用方给的 Token 序列。
     * @param tokens Token 序列（解析期间必须保持有效）。
     * @return Status::Ok 表示就绪。
     */
    Status prepareTokens(const ParserMemory::Vector<Token> &tokens)
    {
        m_tokenArray = tokens.data();
        m_tokenCount = tokens.size();
        m_tokenPos = 0u;
        m_textMode = false;
        m_text = nullptr;
        m_textPos = 0u;
        return prepareCommon();
    }

    /// 两种模式共用的复位动作：清错误、清词素、取第一个 lookahead。
    Status prepareCommon()
    {
        m_errors.clear();
        m_regexResult = {};
        m_commits.clear();
        m_lexeme.clear();
        m_errorStatus = 0;
        m_line = 1u;
        m_column = 1u;
        m_current = Token();
        if (advance() != Status::Ok)
        {
            return Status::ErrLexer;
        }
        return fillLookaheadWindow();
    }

    /**
     * @brief LR(k) / LALR(k)：把 current 之后的 k-1 个 Token 预先取进窗口。
     *
     * 窗口头始终是 m_current，所以错误恢复里「不消耗 lookahead」、`yyclearin`
     * 这些操作都只是挪动窗口。k = 1 时窗口是空的，这个函数是空操作。
     * @return Status::Ok 表示就绪。
     */
    Status fillLookaheadWindow()
    {
        m_lookahead.assign(static_cast<std::size_t>(
                               kLookaheadK > 1u ? kLookaheadK - 1u : 0u),
                           Token());
        if (m_lookahead.empty())
        {
            return Status::Ok;
        }
        const Token head = m_current;
        for (std::size_t index = 0u; index < m_lookahead.size(); ++index)
        {
            if (advance() != Status::Ok)
            {
                return Status::ErrLexer;
            }
            m_lookahead[index] = m_current;
        }
        m_current = head;
        return Status::Ok;
    }

    /**
     * @brief LR(k) / LALR(k)：窗口左移一位（current 前进，末尾补一个新 Token）。
     * @return Status::Ok 表示成功。
     */
    Status advanceWindow()
    {
        if (m_lookahead.empty())
        {
            return advance();
        }
        Token fresh;
        if (advance() != Status::Ok)
        {
            return Status::ErrLexer;
        }
        fresh = m_current;
        m_current = m_lookahead.front();
        for (std::size_t index = 0u; index + 1u < m_lookahead.size(); ++index)
        {
            m_lookahead[index] = m_lookahead[index + 1u];
        }
        m_lookahead.back() = fresh;
        return Status::Ok;
    }

    /**
     * @brief 读一个字符并维护行列号（只在文本模式下用）。
     * @return 读到的字符。
     */
    char readChar()
    {
        const char ch = m_text[m_textPos++];
        if (ch == '\n')
        {
            ++m_line;
            m_column = 1u;
        }
        else
        {
            ++m_column;
        }
        return ch;
    }

    /**
     * @brief 取下一个 lookahead：文本模式走词法器，Token 模式按序号取。
     * @return Status::Ok 表示成功。
     */
    Status advance()
    {
        if (!m_textMode)
        {
            if (m_tokenPos < m_tokenCount)
            {
                m_current = m_tokenArray[m_tokenPos++];
            }
            else
            {
                m_current = Token();
                m_current.type = TokenType::TOKEN_END;
            }
            return Status::Ok;
        }
        Token token;
            const Status status = lexNext(token);
            if (status != Status::Ok)
            {
                // 按细分码给一句人话：未闭合字符串 / 认不出来的字符 / 其它
                const char *why =
                    (status == Status::ErrLexUnterminated)
                        ? "字符串没有闭合"
                        : (status == Status::ErrRegexLimit ? "正则匹配超出资源预算" :
                           status == Status::ErrRegexCancelled ? "正则匹配已取消" :
                           status == Status::ErrRegexMemory ? "正则匹配内存不足" :
                           ((status == Status::ErrLexChar) ? "认不出来的字符" : "词法解析失败"));
                setError(failCode(status == Status::ErrLexUnterminated ||
                                          status == Status::ErrLexChar ||
                                          static_cast<int>(status) <= -30
                                      ? status
                                      : Status::ErrLexer),
                         token.line, token.column, why);
            token.type = TokenType::TOKEN_INVALID;
            m_current = token;
            return status;
        }
        m_current = token;
        return Status::Ok;
    }

    /**
     * @brief 解析文本。
     * @param text 输入文本。
     * @param result 输出语义值；可为 nullptr。
     * @return Status::Ok 表示成功。
     */
    Status runText(std::string_view text, ParserValue_T *result)
    {
        const Status ready = prepareText(text);
        if (ready != Status::Ok)
        {
            return Status::ErrLexer;
        }
        ParserMemory::UniquePtr<Tree> ignoredTree;
        return runLoop(result, &ignoredTree);
    }

    /**
     * @brief 解析 Token 序列。
     * @param tokens Token 序列。
     * @param result 输出语义值；可为 nullptr。
     * @return Status::Ok 表示成功。
     */
    Status runTokens(const ParserMemory::Vector<Token> &tokens, ParserValue_T *result)
    {
        const Status ready = prepareTokens(tokens);
        if (ready != Status::Ok)
        {
            return Status::ErrLexer;
        }
        ParserMemory::UniquePtr<Tree> ignoredTree;
        return runLoop(result, &ignoredTree);
    }

    /**
     * @brief 解析文本并构建语法树（只建树，不跑语义动作）。
     * @param text 输入文本。
     * @return 语法树根；失败返回 nullptr（原因在 errors() 里）。
     */
    ParserMemory::UniquePtr<Tree> parseTree(std::string_view text)
    {
        ParserMemory::UniquePtr<Tree> tree;
        if (prepareText(text) != Status::Ok)
        {
            return tree;
        }
        (void)runLoop(nullptr, &tree, true);
        return tree;
    }

    /* ---------- 引擎状态 ---------- */
    ParserMemory::Vector<Error> m_errors;      ///< 本次解析的错误表
    /// %commit 收集到的片段（按出现顺序；跳过空白后按最长匹配识别）
    ParserMemory::Vector<ParserCommit> m_commits;
    ParserMemory::String m_input;              ///< 文本模式的输入副本
    const char *m_text = nullptr;     ///< m_input.c_str()
    std::size_t m_textPos = 0u;       ///< 文本模式的读写位置
    const Token *m_tokenArray = nullptr; ///< Token 数组（调用方所有）
    std::size_t m_tokenCount = 0u;    ///< Token 个数
    std::size_t m_tokenPos = 0u;      ///< 下一个 Token 下标
    bool m_textMode = true;           ///< true：文本模式；false：Token 模式
    Token m_current;                  ///< 当前 lookahead
    /// LR(k) / LALR(k)：current 之后的 k-1 个 Token（k = 1 时为空）
    ParserMemory::Vector<Token> m_lookahead;
    ParserMemory::String m_lexeme;             ///< 词素缓冲
    std::uint32_t m_line = 1u;        ///< 当前行号
    std::uint32_t m_column = 1u;      ///< 当前列号
    std::int32_t m_errorStatus = 0;   ///< >0 表示正处在错误恢复过程中

    /**
     * @brief 固定字面量命中判断（\b 通配命中任意一个空白字符）。
     * @param rule 词法规则。
     * @param pText 文本起点。
     * @return 命中返回 true。
     */
    static bool lexRuleHit(const LexRule &rule, const char *pText)
    {
        if (!rule.wildcard)
        {
            return std::strncmp(pText, rule.text, rule.length) == 0;
        }
        for (std::uint32_t index = 0u; index < rule.length; ++index)
        {
            if (rule.text[index] == '\x1f')
            {
                const unsigned char ch =
                    static_cast<unsigned char>(pText[index]);
                if ((ch != ' ') && (ch != '\t') && (ch != '\n') &&
                    (ch != '\v') && (ch != '\f') && (ch != '\r'))
                {
                    return false;
                }
            }
            else if (rule.text[index] != pText[index])
            {
                return false;
            }
        }
        return true;
    }

    /* ---------- 词法器 ---------- */

    /**
     * @brief 取下一个词法单元。
     *
     * 顺序与 C 目标一致：双引号字符串（内建，或用户自己声明了 STRING 的词法
     * 时跳过）→ %token regex 与固定字面量一起做最长匹配（字面量优先）→ 数字
     * → 标识符（%identifier 的 DFA 或默认的 C 风格标识符）。固定字面量命中时
     * 词素直接指向字面量本身，所以 Token::text() 不会失效。
     * @param token 输出词法单元（失败时也会带上行列号）。
     * @return Status::Ok 表示取到；ErrLexer 表示词法错误。
     */
    Status lexNext(Token &token)
    {
        /* %commit 命中时是「吃掉这段原文（收进容器或丢掉）再继续取 Token」，
         * 不是 Token，所以整段套一个循环。 */
        for (;;)
        {
            token = Token();
        while (std::isspace(static_cast<unsigned char>(m_text[m_textPos])) != 0)
        {
            readChar();
        }
            token.line = m_line;
            token.column = m_column;
            const char *pStart = &m_text[m_textPos];
            token.offset = static_cast<std::uint32_t>(m_textPos);
            if (m_regexResult.status < 0) return static_cast<Status>(m_regexResult.status);
            if (*pStart == '\0')
            {
                token.type = TokenType::TOKEN_END;
                return Status::Ok;
            }

        /* 内建双引号字符串：词法阶段完成反转义，结果写进 m_lexeme。 */
        if (*pStart == '"')
        {
            m_lexeme.clear();
            readChar();
            while ((m_text[m_textPos] != '\0') && (m_text[m_textPos] != '"'))
            {
                char ch = readChar();
                if (ch == '\\')
                {
                    const char escaped = readChar();
                    switch (escaped)
                    {
                    case 'n': ch = '\n'; break;
                    case 'r': ch = '\r'; break;
                    case 't': ch = '\t'; break;
                    case '\\': ch = '\\'; break;
                    case '"': ch = '"'; break;
                    default: return Status::ErrLexer;
                    }
                }
                m_lexeme.push_back(ch);
            }
            if (m_text[m_textPos] != '"')
            {
                /* 读到输入末尾还没等到收尾的引号 */
                return Status::ErrLexUnterminated;
            }
            readChar();
            token.type = TokenType::TOKEN_STRING;
            token.text = m_lexeme;
            token.name = m_lexeme.c_str();

            return Status::Ok;
        }

        /* 固定字面量与 %token regex 一起做最长匹配，字面量优先。 */
        {
            std::uint32_t bestLength = 0u;
            TokenType bestType = TokenType::TOKEN_INVALID;
            bool bestFromRegex = false;
            bool bestWildcard = false;
            const char *pBestText = nullptr;
            int bestOrder = 0;
            for (const LexRule &rule : kLexRules)
            {
                if ((rule.text != nullptr) &&
                    lexRuleHit(rule, pStart))
                {
                    const unsigned char last = static_cast<unsigned char>(
                        rule.text[rule.length - 1u]);
                    const unsigned char next =
                        static_cast<unsigned char>(pStart[rule.length]);
                    if (((std::isalnum(last) != 0) || (last == '_')) &&
                        ((std::isalnum(next) != 0) || (next == '_')))
                    {
                        continue;
                    }
                    if (rule.length > bestLength)
                    {
                        bestLength = rule.length;
                        bestType = rule.type;
                        bestFromRegex = false;
                        bestWildcard = rule.wildcard;
                        pBestText = rule.text;
                        bestOrder = rule.order;
                    }
                }
            }
            /* %commit：与 Token 规则共用一份最长匹配；同长度时按
               声明序，先声明优先（skip 命中不算 Token，直接丢掉）。 */
            std::uint32_t commitLength = 0u;
            std::size_t commitIndex = 0u;
            for (std::size_t index = 0u; index < kCommitCount; ++index)
            {
                const std::uint32_t matched =
                    lexRegexMatch(kCommitRes[index], m_text, m_textPos, &m_regexLimits, &m_regexResult);
                if (m_regexResult.status < 0) return static_cast<Status>(m_regexResult.status);
                if (matched > commitLength)
                {
                    commitLength = matched;
                    commitIndex = index;
                }
            }
            if ((commitLength != 0u) &&
                ((commitLength > bestLength) ||
                 ((commitLength == bestLength) &&
                  ((bestType == TokenType::TOKEN_INVALID) ||
                   (kCommitOrders[commitIndex] < bestOrder)))))
            {
                const std::uint32_t commitLine = m_line;
                const char *const pCommitText = pStart;
                if (kCommitSkips[commitIndex] == 0u)
                {
                    m_commits.push_back(
                        ParserCommit{static_cast<int>(commitLine),
                                     kCommitTypes[commitIndex][0],
                                     ParserMemory::String(pCommitText, commitLength)});
                }
                for (std::uint32_t index = 0u; index < commitLength; ++index)
                {
                    readChar();
                }
                continue;
            }
            if (bestType != TokenType::TOKEN_INVALID)
            {
                for (std::uint32_t index = 0u; index < bestLength; ++index)
                {
                    readChar();
                }
                token.type = bestType;
                if (bestFromRegex)
                {
                    m_lexeme.assign(pStart, bestLength);
                    token.text = m_lexeme;
                    token.pName = m_lexeme.c_str();
                }
                else if (bestWildcard)
                {
                    /* \b 通配命中：词素要按输入抄一份，不能直接指字面量 */
                    m_lexeme.assign(pStart, bestLength);
                    token.text = m_lexeme;
                    token.pName = m_lexeme.c_str();
                }
                else
                {
                    token.text = pBestText;
                    token.pName = pBestText;
                }
                return Status::Ok;
            }
        }

                    /* 小数点和指数决定 float，后缀 d 决定 double；后缀 u 只用于整数。 */
        if ((std::isdigit(static_cast<unsigned char>(*pStart)) != 0) ||
            ((*pStart == '.') &&
             (std::isdigit(static_cast<unsigned char>(pStart[1])) != 0)))
        {
            char *pEnd = nullptr;
            bool hasDot = false;
            bool hasExponent = false;
            char suffix = '\0';
            double parsed = 0.0;
            while (std::isdigit(
                       static_cast<unsigned char>(m_text[m_textPos])) != 0)
            {
                readChar();
            }
            if (m_text[m_textPos] == '.')
            {
                hasDot = true;
                readChar();
                while (std::isdigit(
                           static_cast<unsigned char>(m_text[m_textPos])) != 0)
                {
                    readChar();
                }
            }
            if ((m_text[m_textPos] == 'e') || (m_text[m_textPos] == 'E'))
            {
                hasExponent = true;
                readChar();
                if ((m_text[m_textPos] == '+') || (m_text[m_textPos] == '-'))
                {
                    readChar();
                }
                if (std::isdigit(
                        static_cast<unsigned char>(m_text[m_textPos])) == 0)
                {
                    return Status::ErrLexer;
                }
                while (std::isdigit(
                           static_cast<unsigned char>(m_text[m_textPos])) != 0)
                {
                    readChar();
                }
            }
            if (std::isalpha(
                    static_cast<unsigned char>(m_text[m_textPos])) != 0)
            {
                suffix = readChar();
            }
            if (std::isalpha(
                    static_cast<unsigned char>(m_text[m_textPos])) != 0)
            {
                return Status::ErrLexer;
            }
            const std::size_t length =
                static_cast<std::size_t>(&m_text[m_textPos] - pStart);
            m_lexeme.assign(pStart, length);
            if (suffix != '\0')
            {
                m_lexeme.resize(length - 1u);
            }
            errno = 0;
            if ((suffix == 'u') || (suffix == 'U'))
            {
                unsigned long value = 0ul;
                if (hasDot || hasExponent)
                {
                    return Status::ErrLexer;
                }
                value = std::strtoul(m_lexeme.c_str(), &pEnd, 10);
                if ((errno == ERANGE) || (*pEnd != '\0') ||
                    (value > UINT32_MAX))
                {
                    return Status::ErrLexer;
                }
                parsed = static_cast<double>(static_cast<std::uint32_t>(value));
            }
            else if ((suffix == 'd') || (suffix == 'D'))
            {
                if (!hasDot && !hasExponent)
                {
                    return Status::ErrLexer;
                }
                parsed = std::strtod(m_lexeme.c_str(), &pEnd);
                if ((errno == ERANGE) || (*pEnd != '\0') ||
                    (std::isfinite(parsed) == 0))
                {
                    return Status::ErrLexer;
                }
            }
            else if (suffix != '\0')
            {
                return Status::ErrLexer;
            }
            else if (hasDot || hasExponent)
            {
                const float value = std::strtof(m_lexeme.c_str(), &pEnd);
                if ((errno == ERANGE) || (*pEnd != '\0') ||
                    (std::isfinite(value) == 0))
                {
                    return Status::ErrLexer;
                }
                parsed = static_cast<double>(value);
            }
            else
            {
                /* 整数可以到 64 位：`Token::value` 只有 32 位，所以**别拿它当准**，
                 * 前端要用整数的话按词素原文（`token.text`）自己解析（`strtoll`）。 */
                const long long value = std::strtoll(m_lexeme.c_str(), &pEnd, 10);
                if ((errno == ERANGE) || (*pEnd != '\0') ||
                    (value < INT64_MIN) || (value > INT64_MAX))
                {
                    return Status::ErrLexer;
                }
                parsed = static_cast<double>(value);
            }
            token.type = TokenType::TOKEN_NUMBER;
            token.text = m_lexeme;
            token.name = m_lexeme.c_str();
            token.value = static_cast<std::int32_t>(parsed);
            return Status::Ok;
        }

                    /* 标识符：%identifier 的 DFA 优先，其次退回默认的 C 风格标识符。 */
        if ((std::isalpha(static_cast<unsigned char>(*pStart)) != 0) ||
            (*pStart == '_'))
        {
            const std::size_t begin = m_textPos;
            while ((std::isalnum(
                        static_cast<unsigned char>(m_text[m_textPos])) != 0) ||
                   (m_text[m_textPos] == '_'))
            {
                readChar();
            }
            m_lexeme.assign(pStart, m_textPos - begin);
            token.type = TokenType::TOKEN_VARIABLE;
            token.text = m_lexeme;
            token.pName = m_lexeme.c_str();

            return Status::Ok;
        }

            /* 一条规则都没命中：认不出来的字符（细分码） */
            return Status::ErrLexChar;
        }
    }



    /* ---------- LL(1) 分析驱动 ---------- */

    /// 分析栈上的项目类型。
    enum class ItemKind
    {
        Terminal,   ///< 终结符：与 lookahead 比对
        Nonterm,    ///< 非终结符：查预测分析表展开
        Action,     ///< 语义动作：执行后完成语义帧
        ActionKeep, ///< 语义动作：执行后保留语义帧（左递归基线规则）
        Forward,    ///< 转发语义槽：$n 就是本产生式的结果
        LeftLoop,   ///< 左递归尾部：接着展开或收尾
        TreeEnd     ///< 语法树：闭合当前节点
    };

    /// 分析栈项目。
    struct StackItem
    {
        ItemKind kind = ItemKind::Terminal;
        std::uint16_t symbol = 0u;
        std::uint16_t frameIndex = kInvalidIndex;
        std::uint16_t valueIndex = 0u;
        std::uint16_t seedFrameIndex = kInvalidIndex;
    };

    /// 语义帧：一个非终结符展开期间的全部语义槽。
    struct LlFrame
    {
        std::uint16_t parentFrameIndex = kInvalidIndex;
        std::uint16_t parentValueIndex = 0u;
        std::array<ParserValue_T, kValueSlots> values{};
        std::array<ParserMemory::String, kValueSlots> names{};
        ParserValue_T resultValue{};
        bool valuesDiscarded = false; ///< 错误恢复丢弃该帧时置位
    };

    /// 压入分析栈项目。
    bool push(ItemKind kind, std::uint16_t symbol, std::uint16_t frameIndex,
              std::uint16_t valueIndex, std::uint16_t seedFrameIndex)
    {
        if (m_llStack.size() >= kMaxStackDepth)
        {
            return false;
        }
        StackItem item;
        item.kind = kind;
        item.symbol = symbol;
        item.frameIndex = frameIndex;
        item.valueIndex = valueIndex;
        item.seedFrameIndex = seedFrameIndex;
        m_llStack.push_back(item);
        return true;
    }

    /// 弹出分析栈项目。
    StackItem pop()
    {
        const StackItem item = m_llStack.back();
        m_llStack.pop_back();
        return item;
    }

    /// 按产生式展开语义帧（生成的 case 各推一条产生式的右部）。
    Status expandProduction(std::uint16_t production, std::uint16_t frameIndex)
    {
        (void)frameIndex;
        switch (production)
        {
        case 0u:
        {
            if (!push(ItemKind::Action, 0u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_RBRACKET), frameIndex, 3u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 7u, frameIndex, 2u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_LBRACKET), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 1u:
        {
            if (!push(ItemKind::Action, 1u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 4u, frameIndex, 2u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 10u, frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 2u:
        {
            if (!push(ItemKind::Action, 2u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 10u, frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 3u:
        {
            if (!push(ItemKind::Action, 3u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 5u, frameIndex, 2u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 9u, frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 4u:
        {
            if (!push(ItemKind::Action, 4u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 1u, frameIndex, 2u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_COMMA), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 5u:
        {
            if (!push(ItemKind::Action, 5u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 6u:
        {
            if (!push(ItemKind::Action, 6u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 3u, frameIndex, 2u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_COMMA), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 7u:
        {
            if (!push(ItemKind::Action, 7u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 8u:
        {
            if (!push(ItemKind::Action, 8u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_RBRACE), frameIndex, 3u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 8u, frameIndex, 2u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_LBRACE), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 9u:
        {
            if (!push(ItemKind::Action, 9u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 1u, frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 10u:
        {
            if (!push(ItemKind::Action, 10u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 11u:
        {
            if (!push(ItemKind::Action, 11u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 3u, frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 12u:
        {
            if (!push(ItemKind::Action, 12u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 13u:
        {
            if (!push(ItemKind::Action, 13u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 10u, frameIndex, 3u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_COLON), frameIndex, 2u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_STRING), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 14u:
        {
            if (!push(ItemKind::Action, 14u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 6u, frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 15u:
        {
            if (!push(ItemKind::Action, 15u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Nonterm, 0u, frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 16u:
        {
            if (!push(ItemKind::Action, 16u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_STRING), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 17u:
        {
            if (!push(ItemKind::Action, 17u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_NUMBER), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 18u:
        {
            if (!push(ItemKind::Action, 18u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_TRUE), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 19u:
        {
            if (!push(ItemKind::Action, 19u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_FALSE), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        case 20u:
        {
            if (!push(ItemKind::Action, 20u, frameIndex, 0u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            if (!push(ItemKind::Terminal, static_cast<std::uint16_t>(TOKEN_NULL), frameIndex, 1u, kInvalidIndex))
            {
                return Status::ErrStack;
            }
            break;
        }
        default: return Status::ErrSyntax;
        }
        return Status::Ok;
    }

    /// 执行一个产生式的语义动作（$n 读 values，$$ 写 result.value）。
    Status executeAction(std::uint16_t production, std::uint16_t frameIndex)
    {
        LlFrame &frame = m_llFrames[frameIndex];
        std::array<ParserValue_T, kValueSlots> &values = frame.values;
        ParseResult result;
        result.value = frame.resultValue;
        (void)values;
        switch (production)
        {
        case 0u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 1u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 2u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 3u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 4u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 5u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 6u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 7u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 8u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 9u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 10u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 11u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 12u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 13u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 14u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 15u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 16u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 17u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 18u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 19u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 20u:
        {
            result.value = ParserValue_T{};
            break;
        }
        default: return Status::ErrSyntax;
        }
        frame.resultValue = result.value;
        return Status::Ok;
    }

    /// 转发语义槽：把 $n 直接当作本产生式的结果值。
    void forwardFrame(LlFrame &frame, std::uint16_t valueIndex)
    {
        frame.resultValue = frame.values[valueIndex];
    }

    /// 完成语义帧：把结果交给父帧，或作为根结果。
    Status completeFrame(std::uint16_t frameIndex)
    {
        LlFrame &frame = m_llFrames[frameIndex];
        // 错误恢复丢掉的帧还留在栈顶（标了 valuesDiscarded）：先退掉它们，
        // 否则后面的完成动作会看到「自己的帧不在栈顶」而误报栈错误
        while ((m_llFrames.size() > (static_cast<std::size_t>(frameIndex) + 1u)) &&
               m_llFrames.back().valuesDiscarded)
        {
            m_llFrames.pop_back();
        }
        if (m_llFrames.empty() || (frameIndex != (m_llFrames.size() - 1u)))
        {
            return Status::ErrStack;
        }
        if (frame.parentFrameIndex == kInvalidIndex)
        {
            m_rootResult = frame.resultValue;
        }
        else
        {
            LlFrame &parent = m_llFrames[frame.parentFrameIndex];
            parent.values[frame.parentValueIndex] = frame.resultValue;
        }
        m_llFrames.pop_back();
        return Status::Ok;
    }

    /// 捕获终结符：记下词素，并把 Token 语义值写进槽位。
    Status captureTerminal(std::uint16_t frameIndex, std::uint16_t valueIndex)
    {
        LlFrame &frame = m_llFrames[frameIndex];
        if (valueIndex >= kValueSlots)
        {
            return Status::ErrStack;
        }
        frame.names[valueIndex].assign(m_current.text);
        frame.values[valueIndex] = tokenValue(m_current);
        return Status::Ok;
    }

    /// 栈顶项目能不能接受这个 Token（终结符相等，或非终结符的预测表里有它）。
    /**
     * @brief 取预测分析表的列号：LL(0) 恒为 0，LL(1) 用 Token 号，LL(k) 二分查 k 元组。
     * @return 列号；LL(k) 找不到对应的 k 元组时返回 -1。
     */
    std::int32_t llTableColumn() const noexcept
    {
        if (kLookaheadK == 0u)
        {
            return 0;
        }
        if (kLookaheadK == 1u)
        {
            return static_cast<std::int32_t>(m_current.type);
        }
        std::uint64_t code = static_cast<std::uint64_t>(m_current.type);
        std::uint64_t scale = kTerminalCount;
        for (std::uint32_t index = 1u; index < kLookaheadK; ++index)
        {
            code +=
                static_cast<std::uint64_t>(m_lookahead[index - 1u].type) * scale;
            scale *= kTerminalCount;
        }
        std::int32_t low = 0;
        std::int32_t high =
            static_cast<std::int32_t>(kLookaheadColumnCount) - 1;
        while (low <= high)
        {
            const std::int32_t mid = low + (high - low) / 2;
            const std::uint64_t midCode =
                static_cast<std::uint64_t>(kLookaheadCode[mid]);
            if (midCode == code)
            {
                return static_cast<std::int32_t>(kLookaheadColumn[mid]);
            }
            if (midCode < code)
            {
                low = mid + 1;
            }
            else
            {
                high = mid - 1;
            }
        }
        return -1;
    }

    /// 栈上第 index 项能不能**取**当前 Token（主循环能接着走）。
    ///
    /// 终结符要相等；非终结符看预测表**这一列**上有没有条目（FIRST，或者可空
    /// 非终结符的 FOLLOW——ε 产生式的那些列）。
    bool llItemTakes(std::size_t index, std::int32_t column,
                     std::uint16_t head) const noexcept
    {
        const StackItem &item = m_llStack[index];
        if (item.kind == ItemKind::Terminal)
        {
            return item.symbol == head;
        }
        if ((item.kind == ItemKind::Nonterm) ||
            (item.kind == ItemKind::LeftLoop))
        {
            if (item.symbol >= kNontermCount)
            {
                return false;
            }
            if ((column >= 0) && (kLlTable[item.symbol][column] >= 0))
            {
                return true;
            }
            return false;
        }
        return false;
    }

    /// 栈上第 index 项是不是「当前 Token 在它的 FOLLOW 里」。
    ///
    /// 非可空非终结符的 FOLLOW 不在预测表里：这个 Token 说明**这一层该结束了**，
    /// 恢复时应当放弃这一层（弹掉它），而不是把 Token 丢掉。
    bool llItemInFollow(std::size_t index, std::uint16_t head) const noexcept
    {
        const StackItem &item = m_llStack[index];
        if ((item.kind != ItemKind::Nonterm) && (item.kind != ItemKind::LeftLoop))
        {
            return false;
        }
        if ((item.symbol >= kNontermCount) || (head >= kTerminalCount) ||
            (kLookaheadK == 0u))
        {
            return false;
        }
        return kLlFollow[item.symbol][head];
    }

    /// 栈顶能不能取当前 Token。
    bool llStackTakes(std::int32_t column, std::uint16_t head) const noexcept
    {
        if (m_llStack.empty())
        {
            return false;
        }
        return llItemTakes(m_llStack.size() - 1u, column, head);
    }

    /// 栈顶那一项是不是「当前 Token 在它的 FOLLOW 里」。
    bool llStackInFollow(std::uint16_t head) const noexcept
    {
        if (m_llStack.empty())
        {
            return false;
        }
        return llItemInFollow(m_llStack.size() - 1u, head);
    }

    /// 栈顶**之下**还有没有哪一项能取当前 Token（FOLLOW 集恢复用）。
    bool llDeeperTakes(std::int32_t column, std::uint16_t head) const noexcept
    {
        if (m_llStack.empty())
        {
            return false;
        }
        std::size_t index = m_llStack.size() - 1u;
        while (index > 0u)
        {
            index--;
            if (llItemTakes(index, column, head))
            {
                return true;
            }
        }
        return false;
    }

    /// 丢弃一个语义帧里还活着的语义值，并把它标成「已丢弃」。
    void llDiscardFrame(LlFrame &frame)
    {
        if (frame.valuesDiscarded)
        {
            return;
        }
        frame.valuesDiscarded = true;
    }

    /// 把帧栈顶这些「已丢弃」的帧一起退掉（帧栈是严格 LIFO）。
    void llUnwindFrames()
    {
        while (!m_llFrames.empty() && m_llFrames.back().valuesDiscarded)
        {
            m_llFrames.pop_back();
        }
    }

    /// 放弃「第 frameIndex 个语义帧以及它里面已经展开的内层帧」。
    ///
    /// 错误恢复丢掉一个构造时用它：不能只清一项所属的帧，否则内层帧会留在帧栈
    /// 上，后面外层的 completeFrame 会因为「自己的帧不在栈顶」误报栈错误。
    void llDiscardFramesFrom(std::uint16_t frameIndex)
    {
        for (std::size_t index = frameIndex; index < m_llFrames.size(); ++index)
        {
            llDiscardFrame(m_llFrames[index]);
        }
        llUnwindFrames();
    }

    /// 弹掉栈顶一项，并丢弃它对应语义帧里的语义值。
    void llDiscardTop()
    {
        if (m_llStack.empty())
        {
            return;
        }
        const StackItem item = pop();
        if (item.frameIndex == kInvalidIndex)
        {
            return;
        }
        llDiscardFrame(m_llFrames[item.frameIndex]);
        llUnwindFrames();
    }

    /**
     * @brief LL 的错误恢复（panic mode）：对齐到「栈上某一项能接受当前 Token」。
     *
     * 每一步按这个顺序试：
     *   ① 栈顶能取它（预测表这一列有产生式）→ 对齐完成，主循环继续；
     *   ② 当前 Token 是 %sync 锚点 → 弹栈到能接受它的位置（写了 %sync 才有）；
     *   ③ 栈顶那一项「该结束了」（当前 Token 在它的 FOLLOW 里），或者栈里更深处
     *      有项能取它 → **放弃栈顶那一项所属的构造**（连它里面已展开的内层构造
     *      一起弹掉，这就是 FOLLOW 集恢复）；
     *   ④ 谁都不认它 → 丢掉这个 Token，接着看下一个。
     * 弹栈与丢输入共用一份预算（256 步），病态输入也不会把解析拖死。
     * @param failedItem 刚才出错、被主循环弹掉的那一项（先放回栈上再恢复：
     *        「丢掉它」还是「丢掉 Token 后重试它」由下面几步自己决定）。
     * @return Status::Ok 表示已经对齐（主循环继续）。
     */
    Status synchronizeLl(const StackItem &failedItem)
    {
        if (kLookaheadK == 0u)
        {
            // LL(0)：预测表只有 ε 一列，没有「当前 Token 属于谁」的信息，
            // FOLLOW 恢复无从判起 → 保持「一错即停」
            return Status::ErrSyntax;
        }
        // 出错的那一项是主循环弹出来的：放回去，让恢复逻辑统一处理
        if (!push(failedItem.kind, failedItem.symbol, failedItem.frameIndex,
                  failedItem.valueIndex, failedItem.seedFrameIndex))
        {
            return Status::ErrStack;
        }
        std::uint32_t budget = 256u;
        while (budget > 0u)
        {
            const std::int32_t column = llTableColumn();
            const std::uint16_t head =
                static_cast<std::uint16_t>(m_current.type);
            // ① 栈顶能取它 → 对齐完成（主循环能接着推进）
            if (llStackTakes(column, head))
            {
                return Status::Ok;
            }
            // ② %sync 锚点：弹栈到能接受它的位置
            if (kHasSyncTokens && (m_current.type != TokenType::TOKEN_END) &&
                isSyncToken(head))
            {
                while ((budget > 0u) && !m_llStack.empty() &&
                       !llStackTakes(column, head))
                {
                    llDiscardTop();
                    budget--;
                }
                return m_llStack.empty() ? Status::ErrSyntax : Status::Ok;
            }
            // ③ FOLLOW 集恢复：栈顶那一项「该结束了」，或者更深处有项能取当前
            //    Token → 放弃「栈顶那一项所属的构造」（连同它里面已经展开的内层
            //    构造），把当前 Token 交给外层
            if (!m_llStack.empty() &&
                (llStackInFollow(head) || llDeeperTakes(column, head)))
            {
                const std::uint16_t frame = m_llStack.back().frameIndex;
                if (frame == kInvalidIndex)
                {
                    llDiscardTop();
                    budget--;
                    continue;
                }
                llDiscardFramesFrom(frame);
                while ((budget > 0u) && !m_llStack.empty() &&
                       (m_llStack.back().frameIndex >= frame))
                {
                    llDiscardTop();
                    budget--;
                }
                continue;
            }
            // ④ 没人能接受它：丢掉这个 Token
            if (m_current.type == TokenType::TOKEN_END)
            {
                return Status::ErrSyntax;
            }
            if (advanceWindow() != Status::Ok)
            {
                return Status::ErrLexer;
            }
            budget--;
        }
        return Status::ErrSyntax;
    }

    /**
     * @brief 非递归预测分析主循环。
     * @param out 输出语义值；可为 nullptr。
     * @return Status::Ok 表示成功。
     */
    Status parseLoopLl(ParserValue_T *out)
    {
        if (!push(ItemKind::Nonterm, kStartId, kInvalidIndex, 0u, kInvalidIndex))
        {
            return Status::ErrStack;
        }
        while (!m_llStack.empty())
        {
            const StackItem item = pop();
            if (item.kind == ItemKind::Terminal)
            {
                if (static_cast<std::uint16_t>(m_current.type) != item.symbol)
                {
                    // 终结符不匹配：只期望这一个（与 C / Python 目标的文案一致）
                    setExpectedOneError(item.symbol);
                    if (synchronizeLl(item) != Status::Ok)
                    {
                        return Status::ErrSyntax;
                    }
                    continue;
                }
                if (captureTerminal(item.frameIndex, item.valueIndex) != Status::Ok)
                {
                    return Status::ErrStack;
                }
                if (advanceWindow() != Status::Ok)
                {
                    return Status::ErrLexer;
                }
                continue;
            }
            if (item.kind == ItemKind::Nonterm)
            {
                if ((static_cast<std::uint16_t>(m_current.type) >= kTerminalCount) ||
                    (item.symbol >= kNontermCount))
                {
                    // 与 Python / C# / C 一致：报「期望…实际是…」
                    setSyntaxError(static_cast<std::uint32_t>(item.symbol));
                    if (synchronizeLl(item) != Status::Ok)
                    {
                        return Status::ErrSyntax;
                    }
                    continue;
                }
                const std::int16_t production =
                    kLlTable[item.symbol][llTableColumn()];
                if (production < 0)
                {
                    setSyntaxError(
                        static_cast<std::uint32_t>(item.symbol));
                    if (synchronizeLl(item) != Status::Ok)
                    {
                        return Status::ErrSyntax;
                    }
                    continue;
                }
                if (m_llFrames.size() >= kMaxStackDepth)
                {
                    setError(failCode(Status::ErrStack), m_current.line,
                             m_current.column, "语义帧栈已满");
                    return Status::ErrStack;
                }
                const std::uint16_t frameIndex =
                    static_cast<std::uint16_t>(m_llFrames.size());
                LlFrame frame;
                frame.parentFrameIndex = item.frameIndex;
                frame.parentValueIndex = item.valueIndex;
                if (item.seedFrameIndex != kInvalidIndex)
                {
                    frame.values[1] =
                        m_llFrames[item.seedFrameIndex].resultValue;
                }
                m_llFrames.push_back(std::move(frame));
                if (expandProduction(static_cast<std::uint16_t>(production),
                                     frameIndex) != Status::Ok)
                {
                    setError(failCode(Status::ErrStack), m_current.line,
                             m_current.column, "分析栈已满");
                    return Status::ErrStack;
                }
                continue;
            }
            if (item.kind == ItemKind::LeftLoop)
            {
                LlFrame &frame = m_llFrames[item.frameIndex];
                if ((static_cast<std::uint16_t>(m_current.type) >= kTerminalCount) ||
                    (item.symbol >= kNontermCount))
                {
                    setSyntaxError(static_cast<std::uint32_t>(item.symbol));
                    if (synchronizeLl(item) != Status::Ok)
                    {
                        return Status::ErrSyntax;
                    }
                    continue;
                }
                const std::int16_t production =
                    kLlTable[item.symbol][llTableColumn()];
                if (production < 0)
                {
                    setSyntaxError(static_cast<std::uint32_t>(item.symbol));
                    if (synchronizeLl(item) != Status::Ok)
                    {
                        return Status::ErrSyntax;
                    }
                    continue;
                }
                if (kProductionIsLeftEmpty[production])
                {
                    if (completeFrame(item.frameIndex) != Status::Ok)
                    {
                        return Status::ErrStack;
                    }
                }
                else
                {
                    frame.values[1] = frame.resultValue;
                    if (expandProduction(static_cast<std::uint16_t>(production),
                                         item.frameIndex) != Status::Ok)
                    {
                        setError(failCode(Status::ErrStack), m_current.line,
                                 m_current.column, "分析栈已满");
                        return Status::ErrStack;
                    }
                }
                continue;
            }
            // Action / ActionKeep / Forward：先跑动作，再看要不要收尾
            if (item.kind == ItemKind::Forward)
            {
                forwardFrame(m_llFrames[item.frameIndex], item.valueIndex);
            }
            else if (executeAction(item.symbol, item.frameIndex) != Status::Ok)
            {
                return Status::ErrSyntax;
            }
            if ((item.kind != ItemKind::ActionKeep) &&
                (completeFrame(item.frameIndex) != Status::Ok))
            {
                return Status::ErrStack;
            }
        }
        if (m_current.type != TokenType::TOKEN_END)
        {
            setError(failCode(Status::ErrSyntax), m_current.line, m_current.column,
                     "输入尚未完全消费");
            return Status::ErrSyntax;
        }
        if (out != nullptr)
        {
            *out = m_rootResult;
        }
        return Status::Ok;
    }

    /**
     * @brief 语法树解析主循环（%tree）：只按预测表建树，不跑语义动作。
     * @param outTree 输出语法树根。
     * @return Status::Ok 表示成功。
     */
    Status parseTreeLoopLl(ParserMemory::UniquePtr<Tree> *outTree)
    {
        if (!push(ItemKind::Nonterm, kStartId, kInvalidIndex, 0u, kInvalidIndex))
        {
            return Status::ErrStack;
        }
        while (!m_llStack.empty())
        {
            const StackItem item = pop();
            if (item.kind == ItemKind::Terminal)
            {
                if (static_cast<std::uint16_t>(m_current.type) != item.symbol)
                {
                    setSyntaxError(
                        static_cast<std::uint32_t>(item.symbol));
                    return Status::ErrSyntax;
                }
                if (m_treePath.empty())
                {
                    return Status::ErrSyntax;
                }
                m_treePath.back()->addChild(makeLeaf(m_current));
                if (advanceWindow() != Status::Ok)
                {
                    return Status::ErrLexer;
                }
                continue;
            }
            if (item.kind == ItemKind::Nonterm)
            {
                if ((static_cast<std::uint16_t>(m_current.type) >= kTerminalCount) ||
                    (item.symbol >= kNontermCount))
                {
                    setSyntaxError(static_cast<std::uint32_t>(item.symbol));
                    return Status::ErrSyntax;
                }
                const std::int16_t production =
                    kLlTable[item.symbol][llTableColumn()];
                if (production < 0)
                {
                    setSyntaxError(
                        static_cast<std::uint32_t>(item.symbol));
                    return Status::ErrSyntax;
                }
                if (m_treePath.size() >= kMaxStackDepth)
                {
                    setError(failCode(Status::ErrStack), m_current.line,
                             m_current.column, "语法树节点栈已满");
                    return Status::ErrStack;
                }
                ParserMemory::UniquePtr<Tree> node = ParserMemory::makeUnique<Tree>(
                    treeTreeName(static_cast<std::uint16_t>(item.symbol)));
                Tree *nodeRef = node.get();
                if (m_treePath.empty())
                {
                    m_treeRoot = std::move(node);
                }
                else
                {
                    Tree *parent = m_treePath.back();
                    parent->addChild(std::move(node));
                    nodeRef = parent->child(parent->childCount() - 1u);
                    if (nodeRef == nullptr)
                    {
                        return Status::ErrStack;
                    }
                }
                if (!push(ItemKind::TreeEnd, 0u, kInvalidIndex, 0u, kInvalidIndex))
                {
                    setError(failCode(Status::ErrStack), m_current.line,
                             m_current.column, "分析栈已满");
                    return Status::ErrStack;
                }
                m_treePath.push_back(nodeRef);
                const std::uint32_t rhsStart = treeProdStart[production];
                const std::uint32_t rhsLength = treeProdLength[production];
                for (std::uint32_t index = rhsLength; index > 0u; --index)
                {
                    const std::uint16_t symbol =
                        treeProdSymbols[rhsStart + index - 1u];
                    const bool terminal = (symbol < kTerminalCount);
                    const std::uint16_t id =
                        terminal ? symbol
                                 : static_cast<std::uint16_t>(symbol -
                                                              kTerminalCount);
                    if (!push(terminal ? ItemKind::Terminal : ItemKind::Nonterm,
                              id, kInvalidIndex, 0u, kInvalidIndex))
                    {
                        setError(failCode(Status::ErrStack), m_current.line,
                                 m_current.column, "分析栈已满");
                        return Status::ErrStack;
                    }
                }
                continue;
            }
            if (item.kind == ItemKind::TreeEnd)
            {
                if (m_treePath.empty())
                {
                    setError(failCode(Status::ErrSyntax), m_current.line,
                             m_current.column, "语法树闭合失败");
                    return Status::ErrSyntax;
                }
                m_treePath.pop_back();
                continue;
            }
            setError(failCode(Status::ErrSyntax), m_current.line, m_current.column,
                     "语法树分析出现未知项目");
            return Status::ErrSyntax;
        }
        if ((m_treeRoot == nullptr) || !m_treePath.empty() ||
            (m_current.type != TokenType::TOKEN_END))
        {
            setError(failCode(Status::ErrSyntax), m_current.line, m_current.column,
                     "输入尚未完全消费");
            return Status::ErrSyntax;
        }
        if (outTree != nullptr)
        {
            *outTree = std::move(m_treeRoot);
        }
        return Status::Ok;
    }

    /**
     * @brief LL(1) 入口：语义模式与 %tree 模式二选一。
     * @param out 输出语义值；可为 nullptr。
     * @param outTree 输出语法树；treeMode 为 true 时使用。
     * @param treeMode 是否构建语法树。
     * @return Status::Ok 表示成功。
     */
    Status runLoop(ParserValue_T *out, ParserMemory::UniquePtr<Tree> *outTree,
                   bool treeMode = false)
    {
        m_llStack.clear();
        m_llFrames.clear();
        m_rootResult = ParserValue_T{};
        if (treeMode)
        {
            m_treeRoot.reset();
            m_treePath.clear();
            return parseTreeLoopLl(outTree);
        }
        return parseLoopLl(out);
    }

    /// LL 增量入口：当前版本按完整文本做保守失效。
    ///
    /// 命中时恢复上次解析的错误表、commit 表与语义结果，不再走词法/预测分析；
    /// 文本变化时自动回退到完整解析并刷新缓存。
    Status runTextIncremental(std::string_view text, ParserValue_T *out,
                              IncrementalCache &cache)
    {
        if (cache.valid && (cache.text.size() == text.size()) &&
            std::equal(cache.text.begin(), cache.text.end(), text.begin()))
        {
            m_errors = cache.errors;
            m_commits = cache.commits;
            if (out != nullptr)
            {
                *out = cache.result;
            }
            return cache.status;
        }
        ParserValue_T value{};
        const Status status = runText(text, &value);
        cache.valid = true;
        cache.text.assign(text.data(), text.size());
        cache.status = status;
        cache.result = value;
        cache.errors = m_errors;
        cache.commits = m_commits;
        if (out != nullptr)
        {
            *out = value;
        }
        return status;
    }

    /// LL 分析栈（跨次解析复用）。
    ParserMemory::Vector<StackItem> m_llStack;
    /// LL 语义帧栈。
    ParserMemory::Vector<LlFrame> m_llFrames;
    /// 根语义值。
    ParserValue_T m_rootResult{};
    /// %tree 的根节点与当前路径（路径节点的所有权在父节点 / 根手里）。
    ParserMemory::UniquePtr<Tree> m_treeRoot;
    ParserMemory::Vector<Tree *> m_treePath;

};

/* ============ 公开 API 的定义 ============ */

const char *statusName(Status status) noexcept
{
    switch (status)
    {
    case Status::Ok: return "Ok";
    case Status::ErrStack: return "ErrStack";
    case Status::ErrSyntax: return "ErrSyntax";
    case Status::ErrLexer: return "ErrLexer";
    case Status::ErrDuplicate: return "ErrDuplicate";
    case Status::ErrUndefined: return "ErrUndefined";
    case Status::ErrSystem: return "ErrSystem";
    case Status::ErrRegexLimit: return "ErrRegexLimit";
    case Status::ErrRegexCancelled: return "ErrRegexCancelled";
    case Status::ErrRegexMemory: return "ErrRegexMemory";
    }
    return "Unknown";
}

const char *tokenName(TokenType type) noexcept
{
    const std::uint32_t index = static_cast<std::uint32_t>(type);
    if (index >= kTokenNameCount)
    {
        return nullptr;
    }
    if (kTokenNames[index][1][0] == '\0')
    {
        return kTokenNames[index][0];
    }
    static thread_local ParserMemory::String displayed;
    displayed = ParserMemory::String(kTokenNames[index][0]) + "(" +
                kTokenNames[index][1] + ")";
    return displayed.c_str();
}

ParserMemory::String tokenText(const Token &token)
{
    if (!token.text.empty())
    {
        return ParserMemory::String(token.text);
    }
    const char *name = tokenName(token.type);
    return (name != nullptr) ? ParserMemory::String(name) : ParserMemory::String();
}

Error::Error(std::int32_t code, std::uint32_t line, std::uint32_t column,
             ParserMemory::String message)
    : m_code(code), m_line(line), m_column(column), m_message(std::move(message))
{
}

Error::Error(std::int32_t code, std::uint32_t line, std::uint32_t column,
             ParserMemory::String message, std::uint32_t tokenType,
             ParserMemory::String tokenText, std::uint32_t state,
             ParserMemory::Vector<std::uint32_t> expected, ParserMemory::String context)
    : m_code(code), m_line(line), m_column(column), m_message(std::move(message)),
      m_tokenType(tokenType), m_tokenText(std::move(tokenText)), m_state(state),
      m_expected(std::move(expected)), m_context(std::move(context))
{
}

ParserMemory::String Error::toString() const
{
    return "第 " + ParserMemory::toString(m_line) + " 行第 " +
           ParserMemory::toString(m_column) + " 列：" + m_message;
}

Tree::Tree(ParserMemory::String name, ParserMemory::String text, ParserLocation location)
    : m_name(std::move(name)), m_text(std::move(text)),
      m_location(location)
{
}

const Tree *Tree::child(std::size_t index) const noexcept
{
    return (index < m_children.size()) ? m_children[index].get() : nullptr;
}

Tree *Tree::child(std::size_t index) noexcept
{
    return (index < m_children.size()) ? m_children[index].get() : nullptr;
}

void Tree::addChild(ParserMemory::UniquePtr<Tree> child)
{
    if (child != nullptr)
    {
        m_children.push_back(std::move(child));
    }
}

const ParserLocation &Tree::location() const noexcept
{
    if ((m_location.line == 0) && !m_children.empty())
    {
        // 分支节点：取第一个有位置的子节点（覆盖文本的起点）
        for (const ParserMemory::UniquePtr<Tree> &child : m_children)
        {
            if ((child != nullptr) && (child->location().line != 0))
            {
                return child->location();
            }
        }
    }
    return m_location;
}

ParserMemory::String Tree::toString() const
{
    ParserMemory::String text;
    appendTreeText(*this, 0u, text);
    return text;
}

void Tree::print(std::FILE *stream) const
{
    const ParserMemory::String text = toString();
    std::FILE *target = (stream != nullptr) ? stream : stdout;
    std::fwrite(text.data(), 1u, text.size(), target);
}

Engine::Engine() : d(ParserMemory::makeUnique<Private>())
{
}

Engine::~Engine() = default;

/** @brief 设置当前实例的正则预算。@param limits 新预算。@return 是否完整支持。 */
bool Engine::setRegexLimits(const RegexLimits &limits)
{
    if (!kRegexBudgetSupported) return false;
    d->m_regexLimits = limits;
    return true;
}
/** @brief 返回最近的匹配结果。@return 结构化状态与统计。 */
const RegexMatchResult &Engine::lastRegexResult() const noexcept
{
    return d->m_regexResult;
}

/** @brief 设置当前实例的错误提示函数。
 * @param formatter 空函数恢复默认提示。
 */
void Engine::setErrorFormatter(ErrorFormatter formatter)
{
    d->m_errorFormatter = std::move(formatter);
}

Status Engine::runText(std::string_view text, ParserValue_T *result)
{
    d->m_regexResult = {};
    const Status status = d->runText(text, result);
    return d->m_regexResult.status < 0 ? static_cast<Status>(d->m_regexResult.status) : status;
}

Status Engine::runTextIncremental(std::string_view text, ParserValue_T *result,
                                  IncrementalCache &cache)
{
    d->m_regexResult = {};
    const Status status = d->runTextIncremental(text, result, cache);
    if (d->m_regexResult.status < 0) cache.valid = false;
    return d->m_regexResult.status < 0 ? static_cast<Status>(d->m_regexResult.status) : status;
}

Status Engine::run(const ParserMemory::Vector<Token> &tokens, ParserValue_T *result)
{
    d->m_regexResult = {};
    const Status status = d->runTokens(tokens, result);
    return d->m_regexResult.status < 0 ? static_cast<Status>(d->m_regexResult.status) : status;
}

ParserMemory::UniquePtr<Tree> Engine::parseTree(std::string_view text)
{
    return d->parseTree(text);
}

const ParserMemory::Vector<Error> &Engine::errors() const noexcept
{
    return d->m_errors;
}

const Error *Engine::lastError() const noexcept
{
    return d->lastError();
}

ParserLocation Engine::currentLocation() const noexcept
{
    return d->currentLocation();
}

const ParserMemory::Vector<ParserCommit> &Engine::commits() const noexcept
{
    return d->m_commits;
}



} // namespace Parser
