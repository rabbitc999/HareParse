/**
 * @file analysis_glr.cpp
 * @brief GLR：LALR 项集 + 多动作 ACTION 表，冲突保留给 GSS 运行时。
 *
 * GLR 与 LALR 使用同一套 LR(1) 项集与同心合并 lookahead，唯一的区别是
 * **不主动消除冲突**：
 *
 * - 能用 %left / %right / %nonassoc / %prec 消解的移进/归约冲突，仍在生成期
 *   消解（与非 GLR 模式一致，写进 [冲突消解]）；
 * - 消解不了的移进/归约冲突与全部归约/归约冲突，全部保留进
 *   AnalysisResult::glrAlternatives：同一个 ACTION 单元挂多个候选动作，
 *   由生成的 GSS 运行时并行探索；
 * - AnalysisResult::action 仍写一个「主动作」（有移进就移进，否则第一个归约），
 *   因此无冲突文法的 ACTION/GOTO 表与 LALR 逐项一致。
 *
 * 运行时按 [GLR 冲突] 小节列出的候选动作分叉；路径在 GSS 节点上合并，
 * 语义值由歧义解决函数决定保留哪一个。
 */

#include "HareParse/analysis.hpp"
#include "HareParse/analysis_internal.hpp"
#include "HareParse/analysis_lr_fast.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "HareParse/analysis_internal.hpp"
#include "HareParse/grammar_loader.hpp"

namespace HareParse
{

using namespace analysis_detail;
using namespace lr1_detail;

namespace
{

/// ACTION 单元键：(状态, Token)。
using GlrCellKey = std::pair<int, std::string>;

/**
 * @brief 把动作压成整数，供候选集合去重（避免反复比较 LrAction）。
 * @param action 动作。
 * @return 整数编码；种类占高位，值占低位。
 */
int encodeGlrAction(const LrAction &action)
{
    return (static_cast<int>(action.kind) << 30) |
           (action.value & 0x3fffffff);
}

/**
 * @brief 把候选动作排序成「接受 / 移进 / 归约」的稳定顺序。
 * @param list 候选动作列表。
 */
void orderActions(std::vector<LrAction> &list)
{
    std::stable_sort(
        list.begin(), list.end(),
        [](const LrAction &left, const LrAction &right)
        {
            const auto rank = [](const LrAction &action)
            {
                if (action.kind == LrAction::Kind::Accept)
                {
                    return 0;
                }
                if (action.kind == LrAction::Kind::Shift)
                {
                    return 1;
                }
                return 2;
            };
            const int leftRank = rank(left);
            const int rightRank = rank(right);
            if (leftRank != rightRank)
            {
                return leftRank < rightRank;
            }
            return left.value < right.value;
        });
}

/**
 * @brief 把候选动作列表渲染成一段文本。
 * @param list 候选动作列表。
 * @param rules 规则表。
 * @param productionIds 产生式编号映射。
 * @return 形如 “移进 3、按产生式 7 归约” 的文本。
 */
std::string actionListText(
    const std::vector<LrAction> &list,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::map<std::pair<std::string, int>, int> &productionIds)
{
    std::string text;
    for (std::size_t index = 0; index < list.size(); ++index)
    {
        if (index != 0)
        {
            text += "、";
        }
        text += slrActionText(list[index], rules, productionIds);
    }
    return text;
}

} // namespace

bool Analysis::glrAnalyze(const Grammar &grammar,
                          const std::vector<std::string> &tokens,
                          const PriorityConfig *priority,
                          AnalysisResult &out, Error &error)
{
    (void)error;
    const PriorityConfig normalized = GrammarLoader::normalizePriority(priority);
    AnalysisResult &result = out;
    result.priority = normalized;
    std::set<std::string> nontermSet;
    prepareLrAnalysis(grammar, tokens, k_glrStartSymbol, "glr", result,
                      nontermSet);
    const std::string &start = result.grammar.start;

    // 1. LALR 项集族（LR(0) + lookahead 传播，快速路径）。
    std::vector<LalrState> states;
    analysis_lr_fast::buildLalr(
        result.grammar.rules, nontermSet, start, result.first, result.terminals,
        states, result.transitions);
    result.lr1StateCount = static_cast<int>(states.size());
    result.hasFollow = false;
    result.lalrStates = states;
    const auto &transitions = result.transitions;

    // 2. 收集每个 ACTION 单元的全部候选动作（此时还不消解冲突）。
    const auto &rules = result.grammar.rules;
    const auto productionIds = slrProductionIds(rules);
    std::set<std::string> terminalSet(result.terminals.begin(),
                                      result.terminals.end());
    std::unordered_map<std::string, int> nontermIds;
    for (std::size_t index = 0; index < result.nonterms.size(); ++index)
    {
        nontermIds[result.nonterms[index]] = static_cast<int>(index);
    }
    // Token 编号按字典序分配，保证整数键的遍历顺序与原字符串键一致。
    std::vector<std::string> tokenNames(result.terminals.begin(),
                                        result.terminals.end());
    std::sort(tokenNames.begin(), tokenNames.end());
    const int tokenCount = static_cast<int>(tokenNames.size());
    std::unordered_map<std::string, int> tokenIds;
    for (std::size_t index = 0; index < tokenNames.size(); ++index)
    {
        tokenIds[tokenNames[index]] = static_cast<int>(index);
    }
    // 预解 rhs 与出边：把字符串键的查表改成整数键，消除热循环里的
    // 字符串比较与临时 pair 构造。
    std::vector<std::vector<const std::vector<std::string> *>> rhsByProd(
        result.nonterms.size());
    std::vector<std::vector<int>> prodIdByNt(result.nonterms.size());
    for (const auto &rule : rules)
    {
        const auto ntIt = nontermIds.find(rule.first);
        if (ntIt == nontermIds.end())
        {
            continue;
        }
        auto &slot = rhsByProd[static_cast<std::size_t>(ntIt->second)];
        auto &ids = prodIdByNt[static_cast<std::size_t>(ntIt->second)];
        slot.resize(rule.second.size());
        ids.resize(rule.second.size(), -1);
        for (std::size_t prod = 0; prod < rule.second.size(); ++prod)
        {
            slot[prod] = &rule.second[prod].rhs;
            const auto idIt = productionIds.find(std::make_pair(rule.first,
                                                                static_cast<int>(prod)));
            if (idIt != productionIds.end())
            {
                ids[prod] = idIt->second;
            }
        }
    }
    // 转移表用 (状态, Token) 整数键，避免热循环里的字符串 pair 查找。
    std::unordered_map<long long, int> transitionsById;
    for (const auto &entry : transitions)
    {
        const auto tokIt = tokenIds.find(entry.first.second);
        if (tokIt != tokenIds.end())
        {
            transitionsById[static_cast<long long>(entry.first.first) *
                                tokenCount +
                            tokIt->second] = entry.second;
        }
    }
    // 候选动作按整数 cell 收集；整数编码去重替代线性查找 LrAction。
    std::unordered_map<long long, std::size_t> cellIndex;
    std::vector<std::pair<int, int>> cellCoords;
    std::vector<std::vector<LrAction>> cellActions;
    std::vector<std::unordered_set<int>> cellSeen;
    const auto cellFor = [&](int state, int token) -> std::size_t
    {
        const long long key =
            static_cast<long long>(state) * tokenCount + token;
        const auto found = cellIndex.find(key);
        if (found != cellIndex.end())
        {
            return found->second;
        }
        const std::size_t id = cellActions.size();
        cellIndex.emplace(key, id);
        cellCoords.emplace_back(state, token);
        cellActions.emplace_back();
        cellSeen.emplace_back();
        return id;
    };
    const auto addUnique = [&](int state, int token, const LrAction &action)
    {
        const std::size_t id = cellFor(state, token);
        if (cellSeen[id].insert(encodeGlrAction(action)).second)
        {
            cellActions[id].push_back(action);
        }
    };
    for (std::size_t stateId = 0; stateId < states.size(); ++stateId)
    {
        for (const auto &entry : states[stateId].items)
        {
            const LrItem &item = entry.first;
            const auto ntIt = nontermIds.find(item.lhs);
            const std::vector<std::string> *rhsPtr = nullptr;
            const int *prodIdPtr = nullptr;
            if (ntIt != nontermIds.end())
            {
                const std::size_t nt = static_cast<std::size_t>(ntIt->second);
                if (static_cast<std::size_t>(item.prod) < rhsByProd[nt].size())
                {
                    rhsPtr = rhsByProd[nt][static_cast<std::size_t>(item.prod)];
                    if (prodIdByNt[nt][static_cast<std::size_t>(item.prod)] >= 0)
                    {
                        prodIdPtr =
                            &prodIdByNt[nt][static_cast<std::size_t>(item.prod)];
                    }
                }
            }
            if ((rhsPtr != nullptr) &&
                static_cast<std::size_t>(item.dot) < rhsPtr->size())
            {
                const std::string &symbol =
                    (*rhsPtr)[static_cast<std::size_t>(item.dot)];
                const auto tokIt = tokenIds.find(symbol);
                if (tokIt != tokenIds.end())
                {
                    const long long key =
                        static_cast<long long>(stateId) * tokenCount +
                        tokIt->second;
                    const auto found = transitionsById.find(key);
                    if (found != transitionsById.end())
                    {
                        addUnique(static_cast<int>(stateId), tokIt->second,
                                  {LrAction::Kind::Shift, found->second});
                    }
                }
                continue;
            }
            if (item.lhs == start)
            {
                addUnique(static_cast<int>(stateId), tokenIds.at("END"),
                          {LrAction::Kind::Accept, 0});
                continue;
            }
            if (prodIdPtr != nullptr)
            {
                for (const std::string &token : entry.second)
                {
                    const auto tokIt = tokenIds.find(token);
                    if (tokIt != tokenIds.end())
                    {
                        addUnique(static_cast<int>(stateId), tokIt->second,
                                  {LrAction::Kind::Reduce, *prodIdPtr});
                    }
                }
            }
        }
    }

    // 3. 优先级消解：只处理「一个移进 × 各归约」的成对冲突。
    std::map<int, std::pair<std::string, int>> productionPriority;
    for (const auto &entry : computeProductionPriority(rules, normalized))
    {
        const auto idIt = productionIds.find(entry.first);
        const int *id =
            idIt != productionIds.end() ? &idIt->second : nullptr;
        if (id != nullptr)
        {
            productionPriority[*id] = entry.second;
        }
    }
    for (std::size_t cell = 0; cell < cellActions.size(); ++cell)
    {
        const int stateId = cellCoords[cell].first;
        const std::string token =
            tokenNames[static_cast<std::size_t>(cellCoords[cell].second)];
        std::vector<LrAction> &list = cellActions[cell];
        int shiftIndex = -1;
        for (std::size_t index = 0; index < list.size(); ++index)
        {
            if (list[index].kind == LrAction::Kind::Shift)
            {
                shiftIndex = static_cast<int>(index);
            }
        }
        if (shiftIndex < 0)
        {
            continue;
        }
        std::vector<bool> drop(list.size(), false);
        bool nonassocError = false;
        for (std::size_t index = 0; index < list.size(); ++index)
        {
            if (list[index].kind != LrAction::Kind::Reduce)
            {
                continue;
            }
            std::string reason;
            const std::string resolved = tryResolveShiftReduce(
                list[static_cast<std::size_t>(shiftIndex)], list[index], token,
                normalized.precedence, normalized.associativity,
                productionPriority, reason);
            const std::string kind =
                slrConflictText(list[static_cast<std::size_t>(shiftIndex)],
                                list[index]);
            if (resolved == "shift")
            {
                drop[index] = true;
                result.resolutions.push_back(
                    {stateId, token, kind, reason, "移进"});
            }
            else if (resolved == "reduce")
            {
                drop[static_cast<std::size_t>(shiftIndex)] = true;
                result.resolutions.push_back(
                    {stateId, token, kind, reason, "归约"});
            }
            else if (resolved == "none")
            {
                drop[index] = true;
                drop[static_cast<std::size_t>(shiftIndex)] = true;
                nonassocError = true;
                result.resolutions.push_back(
                    {stateId, token, kind, reason, "错误"});
            }
        }
        if (nonassocError)
        {
            list.clear();
            continue;
        }
        std::vector<LrAction> kept;
        for (std::size_t index = 0; index < list.size(); ++index)
        {
            if (!drop[index])
            {
                kept.push_back(list[index]);
            }
        }
        list = kept;
    }

    // 4. 写表：单动作单元直接写 action，多动作单元额外记候选列表。
    for (std::size_t cell = 0; cell < cellActions.size(); ++cell)
    {
        if (cellActions[cell].empty())
        {
            continue;
        }
        std::vector<LrAction> list = cellActions[cell];
        orderActions(list);
        const std::pair<int, std::string> cellKey = std::make_pair(
            cellCoords[cell].first,
            tokenNames[static_cast<std::size_t>(cellCoords[cell].second)]);
        result.action[cellKey] = list.front();
        if (list.size() <= 1)
        {
            continue;
        }
        result.glrAlternatives[cellKey] = list;
        const std::string kind = slrConflictText(list[0], list[1]);
        LrConflict conflict;
        conflict.kind = kind;
        conflict.state = cellCoords[cell].first;
        conflict.token = cellKey.second;
        conflict.text =
            "GLR 冲突（状态 " + std::to_string(cellCoords[cell].first) +
            "，Token " + cellKey.second + "）：" + kind + "；候选动作 [" +
            actionListText(list, rules, productionIds) + "]；涉及项集 " +
            lalrStateText(states[static_cast<std::size_t>(cellCoords[cell].first)],
                          rules) +
            "；已保留到 GSS 运行时";
        result.unresolved.push_back(conflict);
    }

    // 5. GOTO 表。
    for (const auto &entry : transitions)
    {
        const std::string &symbol = entry.first.second;
        if (!terminalSet.count(symbol))
        {
            result.gotoTable[std::make_pair(entry.first.first,
                                            nontermIds[symbol])] =
                entry.second;
        }
    }

    // 6. 摊平产生式（ACTION 的归约项指向它）。
    for (const auto &rule : result.grammar.rules)
    {
        for (const Production &production : rule.second)
        {
            Production item = production;
            item.id = static_cast<int>(result.productions.size());
            item.lhs = rule.first;
            result.productions.push_back(item);
        }
    }
    return true;
}

std::string formatGlrReport(const AnalysisResult &analysis)
{
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());
    std::vector<std::string> lines;
    lines.push_back(
        "[生成模式] GLR：LALR 项集 + 多动作 ACTION 表，冲突保留到 GSS 运行时");
    lines.push_back("");
    lines.push_back("[最终产生式]");
    for (const Production &production : analysis.productions)
    {
        lines.push_back("P" + std::to_string(production.id) + ": " +
                        production.lhs + " -> " + joinRhs(production.rhs));
    }
    lines.push_back("");
    lines.push_back("[FIRST]");
    for (const std::string &name : analysis.nonterms)
    {
        std::string joined;
        for (const std::string &item :
             orderedTokens(analysis.first.at(name), order))
        {
            if (!joined.empty())
            {
                joined += ", ";
            }
            joined += (item == k_epsilon) ? "%empty" : item;
        }
        lines.push_back("FIRST(" + name + ") = { " + joined + " }");
    }
    lines.push_back("");
    lines.push_back("[GLR 自动机]");
    lines.push_back(
        "LR(1) 状态数 = " + std::to_string(analysis.lr1StateCount) +
        "，合并后 LALR 状态数 = " +
        std::to_string(analysis.lalrStates.size()) + "，GOTO 转移数 = " +
        std::to_string(analysis.transitions.size()) + "，冲突单元数 = " +
        std::to_string(analysis.glrAlternatives.size()));
    lines.push_back("");
    lines.push_back("[GLR 冲突]");
    if (analysis.unresolved.empty())
    {
        lines.push_back(
            "（无：该文法无未消解冲突，GLR 表与 LALR 等价，运行时退化为"
            "确定性解析）");
    }
    else
    {
        for (const LrConflict &item : analysis.unresolved)
        {
            lines.push_back(item.text);
        }
    }
    lines.push_back("");
    lines.push_back("[ACTION 表]");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size();
         ++stateId)
    {
        for (const std::string &token : analysis.terminals)
        {
            const GlrCellKey key(static_cast<int>(stateId), token);
            const auto found = analysis.action.find(key);
            if (found == analysis.action.end())
            {
                continue;
            }
            const auto alternatives = analysis.glrAlternatives.find(key);
            std::string text = slrEntryText(found->second);
            if (alternatives != analysis.glrAlternatives.end())
            {
                text = "";
                for (std::size_t index = 0; index < alternatives->second.size();
                     ++index)
                {
                    if (index != 0)
                    {
                        text += " | ";
                    }
                    text += slrEntryText(alternatives->second[index]);
                }
            }
            lines.push_back("ACTION[" + std::to_string(stateId) + ", " +
                            token + "] = " + text);
        }
    }
    lines.push_back("");
    lines.push_back("[GOTO 表]");
    for (const auto &entry : analysis.gotoTable)
    {
        lines.push_back(
            "GOTO[" + std::to_string(entry.first.first) + ", " +
            analysis.nonterms[static_cast<std::size_t>(entry.first.second)] +
            "] = " + std::to_string(entry.second));
    }
    lines.push_back("");
    lines.push_back("[冲突消解]");
    if (!analysis.resolutions.empty())
    {
        for (const LrResolution &entry : analysis.resolutions)
        {
            lines.push_back(resolutionLine(entry));
        }
    }
    else
    {
        lines.push_back("（无）");
    }
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (index != 0)
        {
            result += "\n";
        }
        result += lines[index];
    }
    result += "\n";
    return result;
}

std::string formatGlrAutomaton(const AnalysisResult &analysis)
{
    std::vector<std::string> lines;
    lines.push_back("[GLR 项集族]");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size();
         ++stateId)
    {
        lines.push_back("I" + std::to_string(stateId) + ":");
        for (const auto &entry : analysis.lalrStates[stateId].items)
        {
            std::vector<std::string> rhs =
                analysis.grammar.rules.at(entry.first.lhs)
                    [static_cast<std::size_t>(entry.first.prod)]
                        .rhs;
            const std::size_t position = std::min(
                static_cast<std::size_t>(entry.first.dot), rhs.size());
            rhs.insert(rhs.begin() + static_cast<std::ptrdiff_t>(position), ".");
            std::string joinedRhs;
            for (std::size_t index = 0; index < rhs.size(); ++index)
            {
                if (index != 0)
                {
                    joinedRhs += " ";
                }
                joinedRhs += rhs[index];
            }
            std::string lookaheads;
            for (const std::string &token : entry.second)
            {
                if (!lookaheads.empty())
                {
                    lookaheads += ", ";
                }
                lookaheads += token;
            }
            lines.push_back("    (" + entry.first.lhs + " -> " + joinedRhs +
                            ", {" + lookaheads + "})");
        }
    }
    lines.push_back("");
    lines.push_back("[GOTO 转移]");
    for (const auto &entry : analysis.transitions)
    {
        lines.push_back("GOTO(I" + std::to_string(entry.first.first) + ", " +
                        entry.first.second + ") = I" +
                        std::to_string(entry.second));
    }
    lines.push_back("");
    lines.push_back("[GLR 保留冲突]");
    if (analysis.unresolved.empty())
    {
        lines.push_back("（无）");
    }
    else
    {
        for (const LrConflict &item : analysis.unresolved)
        {
            lines.push_back(item.text);
        }
    }
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (index != 0)
        {
            result += "\n";
        }
        result += lines[index];
    }
    result += "\n";
    return result;
}

std::string formatGlrTables(const AnalysisResult &analysis)
{
    std::vector<std::string> lines;
    lines.push_back("[ACTION 表]（冲突单元用 | 列出全部候选动作）");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size();
         ++stateId)
    {
        for (const std::string &token : analysis.terminals)
        {
            const GlrCellKey key(static_cast<int>(stateId), token);
            const auto found = analysis.action.find(key);
            if (found == analysis.action.end())
            {
                continue;
            }
            const auto alternatives = analysis.glrAlternatives.find(key);
            std::string text;
            if (alternatives == analysis.glrAlternatives.end())
            {
                text = slrEntryText(found->second);
            }
            else
            {
                for (std::size_t index = 0; index < alternatives->second.size();
                     ++index)
                {
                    if (index != 0)
                    {
                        text += " | ";
                    }
                    text += slrEntryText(alternatives->second[index]);
                }
            }
            lines.push_back("ACTION[" + std::to_string(stateId) + ", " +
                            token + "] = " + text);
        }
    }
    lines.push_back("");
    lines.push_back("[GOTO 表]");
    for (const auto &entry : analysis.gotoTable)
    {
        lines.push_back(
            "GOTO[" + std::to_string(entry.first.first) + ", " +
            analysis.nonterms[static_cast<std::size_t>(entry.first.second)] +
            "] = " + std::to_string(entry.second));
    }
    lines.push_back("");
    lines.push_back("[冲突消解]");
    if (!analysis.resolutions.empty())
    {
        for (const LrResolution &entry : analysis.resolutions)
        {
            lines.push_back(resolutionLine(entry));
        }
    }
    else
    {
        lines.push_back("（无）");
    }
    lines.push_back("");
    lines.push_back("[GLR 冲突]");
    if (!analysis.unresolved.empty())
    {
        for (const LrConflict &item : analysis.unresolved)
        {
            lines.push_back(item.text);
        }
    }
    else
    {
        lines.push_back("（无）");
    }
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (index != 0)
        {
            result += "\n";
        }
        result += lines[index];
    }
    result += "\n";
    return result;
}

} // namespace HareParse
