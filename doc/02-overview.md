# 02 仓库文档总览

> **RustView 的那条线**：它同样在本仓库里长大——极简 Rust 文法、借用分析与零依赖
> SVG / HTML 都是在这里定型的。它现已拆分为独立仓库 <https://github.com/rabbitc999/rust-view>；
> 本文里指向 `rust-view/` 的链接都改指新仓库的同名文档。

本页说明 HareParse 仓库各处文档的归属。仓库总入口是[根目录 README](../README.md)；生成器、Lepus、MetroLang、RustView 各有独立工程文档。

## 独立工程

| 工程 | 文档 | 内容 |
| --- | --- | --- |
| HareParse 解析器生成器 | [使用入口](../source/README.md) · [专题文档索引](README.md) | 文法定义文件（`.gram`）、分析模式、八种目标语言、生成产物和实现 |
| MetroLang | [工具链入口](https://github.com/rabbitc999/metro-lang/blob/master/README.md) · [语言参考](https://github.com/rabbitc999/metro-lang/blob/master/doc/01-metrolang-reference.md) · [JSON 格式](https://github.com/rabbitc999/metro-lang/blob/master/doc/02-json-format.md) · [交互式查看器](https://github.com/rabbitc999/metro-lang/blob/master/map_view/README.md) | 地铁线网数据语言：文法、编译 / 反编译、SVG 地图与路线查询（**独立仓库**） |
| RustView | [项目入口](https://github.com/rabbitc999/rust-view/blob/master/README.md) · [效果图与演示页](https://github.com/rabbitc999/rust-view/blob/master/doc/preview.html) · [文法](https://github.com/rabbitc999/rust-view/blob/master/rust_subset.gram) | 极简 Rust 伪代码的借用语义可视化：HareParse 生成前端、作用域栈 + 借用状态表分析、手写 SVG / HTML；**1.0 功能冻结**（**独立仓库**） |

## 解析器生成器文档

生成器的实现与手册与仓库其他文档同放本目录，编号见下表。

| 编号 | 文档 | 内容 |
| --- | --- | --- |
| 03 | [架构](03-architecture.md) | 生成器的模块与数据流 |
| 04 | [构建和测试](04-build-and-test.md) | 生成器构建、模板重建与回归 |
| 05 | [文法](05-grammar.md) | 文法定义文件规范与校验 |
| 06 | [分析](06-analysis.md) | LL、LR、IELR 等算法 |
| 07 | [代码生成](07-codegen.md) | 模板与语言后端 |
| 08 | [生成代码](08-generated-code.md) | 产物结构与 API |
| 09 | [示例文法](09-examples.md) | `example/` 文法导读 |
| 10 | [工程示例](10-project-examples.md) | `project/` 项目导读 |
| 11 | [文法使用手册](11-grammar-usage.md) | 指令与语义动作 |
| 12 | [生成模式](12-modes.md) | 十六种分析和生成模式 |
| 13 | [目标语言与产物](13-languages-and-products.md) | 后端选择与产物形态 |
| 14 | [支持矩阵](14-language-mode-support.md) | 语言、模式和增量 API 的当前范围 |
| 15 | [双语、内嵌资产与发布](15-i18n-and-packaging.md) | 显示选项、内嵌手册与语言服务器、Unicode 控制台、独立 Python 打包脚本 |
| 16 | [扩展正则表达式](16-regex-extensions.md) | 前瞻、词边界、忽略大小写与反向引用 |
| 17 | [Unicode 正则模式](17-regex-unicode.md) | UTF-8 码点、Unicode 属性与简单大小写折叠 |
| 18 | [自定义错误提示](18-custom-error-messages.md) | 各目标语言的错误提示回调与上下文 |
| 19 | [正则运行时资源预算](19-regex-runtime-budgets.md) | 步数、工作内存、取消与结构化失败状态 |
| 20 | [正则兼容性与纯校验](20-regex-compatibility.md) | 逐规则引擎与不写盘校验 |
| 21 | [二进制增量生成](21-incremental-generation.md) | 分析缓存、失效与损坏恢复 |
| 22 | [VS Code 工作流](22-vscode-workflow.md) | 大文法同步、增量生成与安全重命名 |
| 23 | [Lepus 后端](23-lepus-backend.md) | Lepus 目标语言的生成方式、API 与回归 |

## 其他项目与工具

| 类别 | 文档 |
| --- | --- |
| 示例工程 | [project 总入口](../project/README.md) · [课程实验](../project/lab_report_nju/README.md) |
| 独立库 | [library 目录](../library/)；各库的 `README.md` 和技术报告随库维护 |
| 编辑器 | [VS Code 插件](../vscode/README.md) |
| 文档维护 | [统一口径、模板与文档生成器](../project/document/README.md) |
| 仓库协作 | [交接单](../AGENTS.md) · [文法 AI 插件](../plugins/hareparse-grammar/README.md) |
| 跨项目叙事 | [01 满级文法](01-gram-legend.md) |

改动某个工程时，以该工程的源码、测试和本地文档为准；解析器的语言与模式范围统一见[支持矩阵](14-language-mode-support.md)。

## 自定义分配器库

[`library/allocator`](../library/allocator/README.md) 提供链表池、线性池和 C++17 容器适配，命名空间为 `Memory`。构建产物位于 `build/allocator/`。

## Lepus 二进制流解析

`std::binary::BinaryReader` 通过 C++ 胶水读取大小端基础数值、定长字符串与数组。

## Lepus JSON 与 TOML

JSON/TOML 解析接口 将 `library/json`、`library/toml` 的副本接入 Lepus，提供解析、嵌套读取、遍历、诊断和序列化。


