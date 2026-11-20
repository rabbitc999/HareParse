/**
 * @file analysis_lr_fast.cpp
 * @brief LR 自动机的整数项编号 + 位集快速构造实现。
 */
#include "HareParse/analysis_lr_fast.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <unordered_map>

#include "HareParse/analysis_internal.hpp"

namespace HareParse
{
namespace analysis_lr_fast
{
namespace
{

using BitSet = std::vector<std::uint64_t>;

/** @brief 按规则表顺序给所有 LR(0) core 编号。 */
std::vector<LrItem> buildCores(
    const std::map<std::string, std::vector<Production>> &rules)
{
    std::vector<LrItem> cores;
    for (const auto &rule : rules)
    {
        for (std::size_t prod = 0; prod < rule.second.size(); ++prod)
        {
            const std::size_t length = rule.second[prod].rhs.size();
            for (std::size_t dot = 0; dot <= length; ++dot)
            {
                cores.push_back({rule.first, static_cast<int>(prod),
                                 static_cast<int>(dot)});
            }
        }
    }
    return cores;
}

/** @brief 按终结符表顺序编号，并把 FIRST 位集宽度算出来。 */
class TokenIndex
{
public:
    /** @brief 构造终结符索引。 @param terminals 终结符表。 */
    explicit TokenIndex(const std::vector<std::string> &terminals)
    {
        for (const std::string &name : terminals)
        {
            ids.emplace(name, static_cast<int>(names.size()));
            names.push_back(name);
        }
        words = (names.size() + 63u) / 64u;
        if (words == 0u)
        {
            words = 1u;
        }
    }
    /** @brief 生成一个空位集。 @return 全零位集。 */
    BitSet empty() const { return BitSet(words, 0u); }
    /** @brief 查终结符编号。 @param name 终结符名。 @return 编号，找不到返回 -1。 */
    int idOf(const std::string &name) const
    {
        const auto found = ids.find(name);
        return found == ids.end() ? -1 : found->second;
    }
    /** @brief 终结符总数。 @return 个数。 */
    std::size_t size() const { return names.size(); }

private:
    std::map<std::string, int> ids;
    std::vector<std::string> names;
    std::size_t words = 1u;
};

/** @brief 位集按位或。 @param dst 目标。 @param src 来源。 @return 目标是否变化。 */
bool orInto(BitSet &dst, const BitSet &src)
{
    bool changed = false;
    for (std::size_t index = 0; index < dst.size(); ++index)
    {
        const std::uint64_t value = src[index];
        if ((dst[index] | value) != dst[index])
        {
            dst[index] |= value;
            changed = true;
        }
    }
    return changed;
}

/** @brief 位集是否非空。 @param bits 位集。 @return 有置位返回真。 */
bool anyBit(const BitSet &bits)
{
    return std::any_of(bits.begin(), bits.end(),
                       [](std::uint64_t word) { return word != 0u; });
}

/** @brief 把位集还原成终结符名集合。 @param bits 位集。 @param tokens 终结符表。 @return 名字集合。 */
std::set<std::string> tokensOf(const BitSet &bits,
                               const std::vector<std::string> &tokens)
{
    std::set<std::string> result;
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        if ((bits[index / 64u] >> (index % 64u)) & 1u)
        {
            result.insert(tokens[index]);
        }
    }
    return result;
}

/** @brief 单个 core 的闭包/转移元数据。 */
struct CoreMeta
{
    LrItem item;                       ///< 对应的 LR(0) 项
    std::string symbol;                ///< 圆点后的符号（圆点在末尾时为空）
    int shiftCore = -1;                ///< 移进该符号后的 core 编号
    std::vector<int> closureTargets;   ///< 圆点后是非终结符时，其各产生式的起始 core
    BitSet spontaneous;                ///< FIRST(β) 中非 ε 的终结符位集
    bool nullable = false;             ///< β 是否可空（决定是否传播整组展望符）
};

/** @brief core 编号器 + FIRST(β) 预计算。 */
class LrContext
{
public:
    /**
     * @brief 构造上下文。
     * @param rules 增广文法规则表。
     * @param nontermSet 非终结符集合。
     * @param start 增广起始符号。
     * @param first FIRST 集合。
     * @param terminals 终结符表。
     */
    LrContext(const std::map<std::string, std::vector<Production>> &rules,
              const std::set<std::string> &nontermSet, const std::string &start,
              const std::map<std::string, std::set<std::string>> &first,
              const std::vector<std::string> &terminals)
        : rules_(rules), nontermSet_(nontermSet), first_(first),
          tokens_(terminals), cores_(buildCores(rules))
    {
        for (std::size_t index = 0u; index < cores_.size(); ++index)
        {
            coreIds_.emplace(cores_[index], static_cast<int>(index));
        }
        meta_.resize(cores_.size());
        for (std::size_t index = 0u; index < cores_.size(); ++index)
        {
            initMeta(static_cast<int>(index));
        }
        const auto found =
            coreIds_.find(LrItem{start, 0, 0});
        startCore_ = found == coreIds_.end() ? -1 : found->second;
        BitSet endBits = tokens_.empty();
        const int endId = tokens_.idOf("END");
        if (endId >= 0)
        {
            endBits.assign(tokens_.empty().size(), 0u);
            endBits[static_cast<std::size_t>(endId) / 64u] |=
                (std::uint64_t{1} << (static_cast<std::size_t>(endId) % 64u));
        }
        endBits_ = std::move(endBits);
    }
    /** @brief 按 LrItem 查 core 编号。 @param item LR(0) 项。 @return 编号，找不到返回 -1。 */
    int coreIdOf(const LrItem &item) const
    {
        const auto found = coreIds_.find(item);
        return found == coreIds_.end() ? -1 : found->second;
    }
    /** @brief core 总数。 @return 个数。 */
    std::size_t coreCount() const { return cores_.size(); }
    /** @brief 终结符索引。 @return 引用。 */
    const TokenIndex &tokens() const { return tokens_; }
    /** @brief core 元数据。 @return 引用。 */
    const std::vector<CoreMeta> &meta() const { return meta_; }
    /** @brief core 列表。 @return 引用。 */
    const std::vector<LrItem> &cores() const { return cores_; }
    /** @brief 起始 core 编号。 @return 编号。 */
    int startCore() const { return startCore_; }
    /** @brief END 展望符位集。 @return 位集。 */
    const BitSet &endBits() const { return endBits_; }
    /** @brief 规则表。 @return 引用。 */
    const std::map<std::string, std::vector<Production>> &rules() const
    {
        return rules_;
    }

private:
    void initMeta(int coreId)
    {
        CoreMeta &meta = meta_[static_cast<std::size_t>(coreId)];
        meta.item = cores_[static_cast<std::size_t>(coreId)];
        meta.spontaneous = tokens_.empty();
        const std::vector<std::string> &rhs =
            rules_.at(meta.item.lhs)[static_cast<std::size_t>(meta.item.prod)].rhs;
        const std::size_t dot = static_cast<std::size_t>(meta.item.dot);
        if (dot >= rhs.size())
        {
            return;
        }
        meta.symbol = rhs[dot];
        const auto shifted = coreIds_.find(
            LrItem{meta.item.lhs, meta.item.prod, meta.item.dot + 1});
        meta.shiftCore = shifted == coreIds_.end() ? -1 : shifted->second;
        if (nontermSet_.count(meta.symbol) == 0u)
        {
            return;
        }
        const auto targetRule = rules_.find(meta.symbol);
        if (targetRule != rules_.end())
        {
            for (std::size_t index = 0u; index < targetRule->second.size(); ++index)
            {
                const auto target =
                    coreIds_.find(LrItem{meta.symbol, static_cast<int>(index), 0});
                if (target != coreIds_.end())
                {
                    meta.closureTargets.push_back(target->second);
                }
            }
        }
        bool nullable = true;
        for (std::size_t index = dot + 1u; index < rhs.size(); ++index)
        {
            const std::string &symbol = rhs[index];
            if (nontermSet_.count(symbol) == 0u)
            {
                addToken(meta.spontaneous, symbol);
                nullable = false;
                break;
            }
            const auto firstFound = first_.find(symbol);
            if (firstFound == first_.end())
            {
                nullable = false;
                break;
            }
            for (const std::string &token : firstFound->second)
            {
                if (token != HareParse::k_epsilon)
                {
                    addToken(meta.spontaneous, token);
                }
            }
            if (firstFound->second.count(HareParse::k_epsilon) == 0u)
            {
                nullable = false;
                break;
            }
        }
        meta.nullable = nullable;
    }

    void addToken(BitSet &bits, const std::string &name)
    {
        const int id = tokens_.idOf(name);
        if (id < 0)
        {
            return;
        }
        bits[static_cast<std::size_t>(id) / 64u] |=
            (std::uint64_t{1} << (static_cast<std::size_t>(id) % 64u));
    }

    const std::map<std::string, std::vector<Production>> &rules_;
    const std::set<std::string> &nontermSet_;
    const std::map<std::string, std::set<std::string>> &first_;
    TokenIndex tokens_;
    std::vector<LrItem> cores_;
    std::map<LrItem, int> coreIds_;
    std::vector<CoreMeta> meta_;
    int startCore_ = -1;
    BitSet endBits_;
};

/** @brief LR(0) 状态转成按 core 编号排序的整数序列。 */
std::vector<int> coresOfState(const SlrState &state, const LrContext &ctx)
{
    std::vector<int> result;
    result.reserve(state.items.size());
    for (const LrItem &item : state.items)
    {
        const int coreId = ctx.coreIdOf(item);
        if (coreId >= 0)
        {
            result.push_back(coreId);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

/** @brief 状态内容的哈希（供规范 LR(1) 状态去重）。 */
std::size_t hashCanon(const CanonState &state)
{
    std::size_t hash = 1469598103934665603u;
    for (const auto &entry : state.items)
    {
        hash ^= static_cast<std::size_t>(entry.first) + 0x9e3779b97f4a7c15u +
                (hash << 6u) + (hash >> 2u);
        for (const std::uint64_t word : entry.second)
        {
            hash ^= static_cast<std::size_t>(word) + 0x9e3779b97f4a7c15u +
                    (hash << 6u) + (hash >> 2u);
        }
    }
    return hash;
}

/** @brief 规范 LR(1) 闭包：给定的项 + 展望符种子扩展到完整状态。 */
CanonState canonicalClosure(
    const LrContext &ctx,
    const std::vector<std::pair<int, BitSet>> &seed)
{
    const std::size_t count = ctx.coreCount();
    std::vector<BitSet> la(count);
    std::vector<char> present(count, 0);
    std::vector<char> spontaneousDone(count, 0);
    std::vector<char> queued(count, 0);
    for (std::size_t index = 0u; index < count; ++index)
    {
        la[index] = ctx.tokens().empty();
    }
    std::deque<int> work;
    const auto mark = [&](int core)
    {
        if (!queued[static_cast<std::size_t>(core)])
        {
            queued[static_cast<std::size_t>(core)] = 1;
            work.push_back(core);
        }
    };
    const auto add = [&](int core, const BitSet &bits)
    {
        if (!anyBit(bits))
        {
            return;
        }
        if (orInto(la[static_cast<std::size_t>(core)], bits))
        {
            present[static_cast<std::size_t>(core)] = 1;
            mark(core);
        }
        else
        {
            present[static_cast<std::size_t>(core)] = 1;
        }
    };
    for (const auto &entry : seed)
    {
        add(entry.first, entry.second);
    }
    while (!work.empty())
    {
        const int core = work.front();
        work.pop_front();
        queued[static_cast<std::size_t>(core)] = 0;
        const CoreMeta &meta = ctx.meta()[static_cast<std::size_t>(core)];
        if (!spontaneousDone[static_cast<std::size_t>(core)])
        {
            spontaneousDone[static_cast<std::size_t>(core)] = 1;
            for (const int target : meta.closureTargets)
            {
                add(target, meta.spontaneous);
            }
        }
        if (!meta.nullable)
        {
            continue;
        }
        for (const int target : meta.closureTargets)
        {
            add(target, la[static_cast<std::size_t>(core)]);
        }
    }
    CanonState result;
    for (std::size_t core = 0u; core < count; ++core)
    {
        if (present[core] && anyBit(la[core]))
        {
            result.items.emplace_back(static_cast<int>(core), la[core]);
        }
    }
    return result;
}

/** @brief 规范 LR(1) 的 GOTO 转移。 */
CanonState canonicalGoto(const LrContext &ctx, const CanonState &state,
                         const std::string &symbol)
{
    const std::size_t count = ctx.coreCount();
    std::vector<BitSet> seeds(count);
    std::vector<char> used(count, 0);
    for (std::size_t index = 0u; index < count; ++index)
    {
        seeds[index] = ctx.tokens().empty();
    }
    for (const auto &entry : state.items)
    {
        const CoreMeta &meta = ctx.meta()[static_cast<std::size_t>(entry.first)];
        if (meta.shiftCore >= 0 && meta.symbol == symbol)
        {
            orInto(seeds[static_cast<std::size_t>(meta.shiftCore)], entry.second);
            used[static_cast<std::size_t>(meta.shiftCore)] = 1;
        }
    }
    std::vector<std::pair<int, BitSet>> seedList;
    for (std::size_t core = 0u; core < count; ++core)
    {
        if (used[core] && anyBit(seeds[core]))
        {
            seedList.emplace_back(static_cast<int>(core), seeds[core]);
        }
    }
    if (seedList.empty())
    {
        return {};
    }
    return canonicalClosure(ctx, seedList);
}

} // namespace

void buildLalr(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, const std::string &start,
    const std::map<std::string, std::set<std::string>> &first,
    const std::vector<std::string> &terminals, std::vector<LalrState> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions)
{
    const LrContext ctx(rules, nontermSet, start, first, terminals);
    std::vector<SlrState> lr0States;
    std::map<std::pair<int, std::string>, int> lr0Transitions;
    analysis_detail::lr0BuildAutomaton(rules, nontermSet, start, lr0States,
                                       lr0Transitions);
    const std::size_t stateCount = lr0States.size();
    const std::size_t coreCount = ctx.coreCount();
    std::vector<std::vector<int>> stateCores(stateCount);
    for (std::size_t state = 0u; state < stateCount; ++state)
    {
        stateCores[state] = coresOfState(lr0States[state], ctx);
    }

    const std::size_t words = ctx.tokens().empty().size();
    const auto instanceId = [coreCount](std::size_t state, int core)
    { return state * coreCount + static_cast<std::size_t>(core); };
    std::vector<BitSet> la(stateCount * coreCount);
    for (BitSet &bits : la)
    {
        bits.assign(words, 0u);
    }
    std::vector<std::vector<std::size_t>> links(stateCount * coreCount);
    std::deque<std::size_t> work;
    std::vector<char> queued(stateCount * coreCount, 0);
    const auto mark = [&](std::size_t instance)
    {
        if (!queued[instance])
        {
            queued[instance] = 1;
            work.push_back(instance);
        }
    };

    for (std::size_t state = 0u; state < stateCount; ++state)
    {
        for (const int core : stateCores[state])
        {
            const CoreMeta &meta = ctx.meta()[static_cast<std::size_t>(core)];
            const std::size_t source = instanceId(state, core);
            for (const int target : meta.closureTargets)
            {
                const std::size_t destination = instanceId(state, target);
                if (anyBit(meta.spontaneous) &&
                    orInto(la[destination], meta.spontaneous))
                {
                    mark(destination);
                }
                if (meta.nullable)
                {
                    links[source].push_back(destination);
                }
            }
        }
    }

    for (const auto &transition : lr0Transitions)
    {
        const std::size_t sourceState =
            static_cast<std::size_t>(transition.first.first);
        const std::size_t targetState =
            static_cast<std::size_t>(transition.second);
        for (const int core : stateCores[sourceState])
        {
            const CoreMeta &meta = ctx.meta()[static_cast<std::size_t>(core)];
            if (meta.shiftCore >= 0 && meta.symbol == transition.first.second)
            {
                links[instanceId(sourceState, core)].push_back(
                    instanceId(targetState, meta.shiftCore));
            }
        }
    }

    if (!la.empty() && ctx.startCore() >= 0 && !ctx.endBits().empty())
    {
        const std::size_t startInstance =
            instanceId(0u, ctx.startCore());
        if (orInto(la[startInstance], ctx.endBits()))
        {
            mark(startInstance);
        }
    }
    while (!work.empty())
    {
        const std::size_t source = work.front();
        work.pop_front();
        queued[source] = 0;
        for (const std::size_t destination : links[source])
        {
            if (orInto(la[destination], la[source]))
            {
                mark(destination);
            }
        }
    }

    outStates.clear();
    outStates.reserve(stateCount);
    for (std::size_t state = 0u; state < stateCount; ++state)
    {
        LalrState result;
        for (const int core : stateCores[state])
        {
            const std::set<std::string> tokens =
                tokensOf(la[instanceId(state, core)], terminals);
            if (!tokens.empty())
            {
                result.items[ctx.cores()[static_cast<std::size_t>(core)]] = tokens;
            }
        }
        outStates.push_back(std::move(result));
    }
    outTransitions = lr0Transitions;
}

void buildCanonicalLr1(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, const std::string &start,
    const std::map<std::string, std::set<std::string>> &first,
    const std::vector<std::string> &terminals,
    std::vector<CanonState> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions)
{
    const LrContext ctx(rules, nontermSet, start, first, terminals);
    outStates.clear();
    outTransitions.clear();
    if (ctx.startCore() < 0)
    {
        return;
    }
    const CanonState initial =
        canonicalClosure(ctx, {{ctx.startCore(), ctx.endBits()}});
    outStates.push_back(initial);
    std::unordered_map<std::size_t, std::vector<int>> buckets;
    buckets[hashCanon(initial)].push_back(0);
    std::vector<int> pending{0};
    for (std::size_t cursor = 0u; cursor < pending.size(); ++cursor)
    {
        const int stateId = pending[cursor];
        std::set<std::string> symbols;
        for (const auto &entry :
             outStates[static_cast<std::size_t>(stateId)].items)
        {
            const CoreMeta &meta =
                ctx.meta()[static_cast<std::size_t>(entry.first)];
            if (!meta.symbol.empty())
            {
                symbols.insert(meta.symbol);
            }
        }
        for (const std::string &symbol : symbols)
        {
            const CanonState target =
                canonicalGoto(ctx, outStates[static_cast<std::size_t>(stateId)],
                              symbol);
            if (target.items.empty())
            {
                continue;
            }
            const std::size_t targetHash = hashCanon(target);
            int targetId = -1;
            const auto bucket = buckets.find(targetHash);
            if (bucket != buckets.end())
            {
                for (const int candidate : bucket->second)
                {
                    if (outStates[static_cast<std::size_t>(candidate)] == target)
                    {
                        targetId = candidate;
                        break;
                    }
                }
            }
            if (targetId < 0)
            {
                targetId = static_cast<int>(outStates.size());
                outStates.push_back(target);
                buckets[targetHash].push_back(targetId);
                pending.push_back(targetId);
            }
            outTransitions[std::make_pair(stateId, symbol)] = targetId;
        }
    }
}

void canonicalToLr1Items(
    const std::vector<CanonState> &states,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    std::vector<std::set<Lr1Item>> &outStates)
{
    const std::vector<LrItem> cores = buildCores(rules);
    outStates.clear();
    outStates.reserve(states.size());
    for (const CanonState &state : states)
    {
        std::set<Lr1Item> converted;
        for (const auto &entry : state.items)
        {
            const LrItem &core = cores[static_cast<std::size_t>(entry.first)];
            for (std::size_t token = 0u; token < terminals.size(); ++token)
            {
                if ((entry.second[token / 64u] >> (token % 64u)) & 1u)
                {
                    converted.insert({core.lhs, core.prod, core.dot,
                                      terminals[token]});
                }
            }
        }
        outStates.push_back(std::move(converted));
    }
}

void canonicalToLalrStates(const std::vector<CanonState> &states,
                           const std::map<std::string, std::vector<Production>> &rules,
                           const std::vector<std::string> &terminals,
                           bool mergeCores, std::vector<LalrState> &outStates,
                           std::vector<int> *outStateMap)
{
    const std::vector<LrItem> cores = buildCores(rules);
    outStates.clear();
    if (outStateMap != nullptr)
    {
        outStateMap->clear();
    }
    std::map<std::vector<int>, int> coreIds;
    for (const CanonState &state : states)
    {
        LalrState converted;
        std::vector<int> core;
        for (const auto &entry : state.items)
        {
            if (!anyBit(entry.second))
            {
                continue;
            }
            core.push_back(entry.first);
            converted.items[cores[static_cast<std::size_t>(entry.first)]] =
                tokensOf(entry.second, terminals);
        }
        int mapped = -1;
        if (mergeCores)
        {
            const auto found = coreIds.find(core);
            if (found == coreIds.end())
            {
                mapped = static_cast<int>(outStates.size());
                coreIds.emplace(core, mapped);
                outStates.push_back(std::move(converted));
            }
            else
            {
                mapped = found->second;
                for (const auto &entry : converted.items)
                {
                    outStates[static_cast<std::size_t>(mapped)].items[entry.first]
                        .insert(entry.second.begin(), entry.second.end());
                }
            }
        }
        else
        {
            mapped = static_cast<int>(outStates.size());
            outStates.push_back(std::move(converted));
        }
        if (outStateMap != nullptr)
        {
            outStateMap->push_back(mapped);
        }
    }
}

int canonicalMergedStateCount(const std::vector<CanonState> &states)
{
    std::map<std::vector<int>, int> coreIds;
    for (const CanonState &state : states)
    {
        std::vector<int> core;
        for (const auto &entry : state.items)
        {
            if (anyBit(entry.second))
            {
                core.push_back(entry.first);
            }
        }
        coreIds.emplace(std::move(core), 0);
    }
    return static_cast<int>(coreIds.size());
}

} // namespace analysis_lr_fast
} // namespace HareParse
