/** @file code_gen_lepus.hpp @brief Lepus LL / LR / GLR 后端。 */
#ifndef PARSE_ENGINE_CODE_GEN_LEPUS_HPP
#define PARSE_ENGINE_CODE_GEN_LEPUS_HPP
#include "HareParse/project.hpp"
namespace HareParse {
/** @brief 判断 Lepus 是否支持模式。@param mode 规范模式名。@return 支持时为真。 */
bool supportsLepusMode(const std::string &mode);
/** @brief 生成自包含 Lepus 解析器源码与使用说明。
 * @param request 工程配置。@param analysis 已构造的分析表。
 * @param out 产物映射。@param error 失败原因。@return 生成成功时为真。
 */
bool generateLepusProject(const ProjectRequest &request,
                          const AnalysisResult &analysis,
                          std::map<std::string, std::string> &out, Error &error);
}
#endif
