/**
 * @file analysis_lr_fast.hpp
 * @brief LR 自动机的整数项编号 + 位集快速构造（生成器内部使用）。
 *
 * 这里把 LR(0) 项（core）与终结符都编成整数，展望符集合用位集表示：
 * - LALR / GLR / IELR 的快速路径直接走 LR(0) 项集 + lookahead 传播，
 *   不再先构造规模大得多的规范 LR(1) 自动机；
 * - 规范 LR(1) 自动机仍然提供，但改用同样的整数编号与位集实现；
 * - 对外仍可转成旧的 `LrItem` / `Lr1Item` 容器，保持分析层其余代码不变。
 */
#ifndef HAREPARSE_ANALYSIS_LR_FAST_HPP
#define HAREPARSE_ANALYSIS_LR_FAST_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "HareParse/analysis.hpp"

namespace HareParse
{
namespace analysis_lr_fast
{

/**
 * @brief 规范 LR(1) 状态的紧凑表示：每项一个 core 编号 + 展望符位集。
 */
struct CanonState
{
    /// (core 编号, 展望符位集)，按 core 编号升序排列。
    std::vector<std::pair<int, std::vector<std::uint64_t>>> items;

    /** @brief 状态相等比较（用于自动机状态去重）。 @param other 另一个状态。 @return 相等返回真。 */
    bool operator==(const CanonState &other) const
    {
        return items == other.items;
    }
};

/**
 * @brief 直接构造 LALR 项集族与 GOTO 转移。
 * @param rules 增广文法规则表。
 * @param nontermSet 非终结符集合。
 * @param start 增广起始符号。
 * @param first FIRST 集合。
 * @param terminals 终结符表（决定位集宽度）。
 * @param outStates 输出 LALR 状态。
 * @param outTransitions 输出 GOTO 转移。
 */
void buildLalr(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, const std::string &start,
    const std::map<std::string, std::set<std::string>> &first,
    const std::vector<std::string> &terminals, std::vector<LalrState> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions);

/**
 * @brief 构造规范 LR(1) 项集族（整数项编号 + 位集实现）。
 * @param rules 增广文法规则表。
 * @param nontermSet 非终结符集合。
 * @param start 增广起始符号。
 * @param first FIRST 集合。
 * @param terminals 终结符表（决定位集宽度）。
 * @param outStates 输出紧凑 LR(1) 状态。
 * @param outTransitions 输出 GOTO 转移。
 */
void buildCanonicalLr1(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, const std::string &start,
    const std::map<std::string, std::set<std::string>> &first,
    const std::vector<std::string> &terminals,
    std::vector<CanonState> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions);

/**
 * @brief 把紧凑 LR(1) 状态转回旧的 `Lr1Item` 集合。
 * @param states 紧凑状态。
 * @param rules 增广文法规则表。
 * @param terminals 终结符表。
 * @param outStates 输出旧表示的状态族，顺序与输入一致。
 */
void canonicalToLr1Items(
    const std::vector<CanonState> &states,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    std::vector<std::set<Lr1Item>> &outStates);

/**
 * @brief 把紧凑 LR(1) 状态转成 `LalrState`（可选同心合并）。
 * @param states 紧凑状态。
 * @param rules 增广文法规则表（用于把 core 编号还原成项）。
 * @param terminals 终结符表。
 * @param mergeCores 为真时按 core 合并；为假时每个状态独立。
 * @param outStates 输出状态族。
 * @param outStateMap 输出紧凑状态编号到输出状态编号的映射；不需要可传 nullptr。
 */
void canonicalToLalrStates(const std::vector<CanonState> &states,
                           const std::map<std::string, std::vector<Production>> &rules,
                           const std::vector<std::string> &terminals,
                           bool mergeCores, std::vector<LalrState> &outStates,
                           std::vector<int> *outStateMap = nullptr);

/**
 * @brief 统计紧凑 LR(1) 状态按 core 合并后的状态数。
 * @param states 紧凑状态。
 * @return 合并后的状态数。
 */
int canonicalMergedStateCount(const std::vector<CanonState> &states);

} // namespace analysis_lr_fast
} // namespace HareParse

#endif // HAREPARSE_ANALYSIS_LR_FAST_HPP
