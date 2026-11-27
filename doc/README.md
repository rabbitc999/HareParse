# 仓库文档

Lepus 目标语言的生成方式、API 与支持边界见 [23 Lepus 后端](23-lepus-backend.md)。

各目标语言的 `%postlex` 写法见[词法后处理与占位 Token](05-grammar.md#词法后处理与占位-token)，注释的读取方式见[保留注释及源位置](05-grammar.md#保留注释及源位置)。

> **中文（默认）** · [English](en/README.md) —— 英文版与中文版内容对应；中文版优先维护，
> 两处说法有出入时以中文版为准。尚无独立英文译文的页面由英文索引链到中文原文。

> **Lepus 的那段光辉历史**：这门语言早期就在本仓库里长大——生成器、StackVM、标准库、
> 胶水与反射都是在这里一起打磨出来的（下面这些 Lepus 文档就是那段记录）。它现已拆分为
> 独立仓库 <https://github.com/rabbitc999/Lepus>；本文里指向 `lepus/` 的链接都改指新仓库的
> 同名文档。

HareParse 的[中英双语显示、内嵌手册与语言服务器、发布说明](15-i18n-and-packaging.md)覆盖 `--ui-lang`、`man` 与 `lsp` 子命令、控制台与管道、两个工具的独立打包脚本。

Lepus 1.0.0 的名称、`.lep` 工程布局与中文／英文／日语语言选择见[Lepus 名称与三语说明](https://github.com/rabbitc999/Lepus/blob/master/doc/37-name-freeze.md)。
Lepus 的[文档索引](https://github.com/rabbitc999/Lepus/blob/master/doc/README.md)结合正式 example 源码、expected 输出与错误边界逐步讲解。初学者从 [06 基础程序](https://github.com/rabbitc999/Lepus/blob/master/doc/01-quickstart.md) 开始，专项环境和验收范围见 [28 样例验证](https://github.com/rabbitc999/Lepus/blob/master/doc/11-algorithm-validation.md)。


引用来源与词法生命周期检查见 [07 引用与迭代](https://github.com/rabbitc999/Lepus/blob/master/doc/22-resource-lifetime.md)，专项回归为 `tests/lepus_reference_escape.py`。

`T &` 默认只读引用见 [引用语法糖](https://github.com/rabbitc999/Lepus/blob/master/doc/22-resource-lifetime.md)；未分配区域的 panic、自定义堆范围检查及 AOT 默认 unchecked / 显式 checked 见 [18 内存分配器](https://github.com/rabbitc999/Lepus/blob/master/doc/20-memory-allocator.md)，专项为 `tests/lepus_reference_sugar.py` 和 `tests/lepus_memory_guard.py`。

引用与泛型、枚举、回调、迭代器、接口及 Drop 的跨功能组合见 [reference_combinations.lep](https://github.com/rabbitc999/Lepus/blob/master/example/samples/reference_combinations.lep)，专项回归为 `tests/lepus_reference_combinations.py`，覆盖正常解释 / AOT 与非法逃逸诊断。

从 [02 仓库文档总览](02-overview.md) 进入。此目录是**仓库全部文档的唯一入口**：整个仓库的导航、跨项目说明，以及 HareParse 解析器生成器的手册与实现文档都放在这里。

| 文档 | 内容 |
| --- | --- |
| [02 仓库文档总览](02-overview.md) | 各工程、附加库及全部文档的入口 |
| [01 满级文法](01-gram-legend.md) | 跨解析器、Lepus 和课程实验的虚构外传，续写 Lepus 1.0、迭代链与多数据库验收 |
| [03 架构](03-architecture.md) | 生成器的模块、数据流与代码约定 |
| [04 构建和测试](04-build-and-test.md) | 生成器构建、模板重建与回归 |
| [05 文法](05-grammar.md) | 文法定义文件（`.gram`）规范与生成期校验 |
| [06 分析](06-analysis.md) | LL、LR、IELR 等算法 |
| [07 代码生成](07-codegen.md) | 模板系统、语言后端与 C/C++ 命名风格、C++ DFA 与标准库一致性回归 |
| [08 生成代码](08-generated-code.md) | 产物清单与 API |
| [09 示例文法](09-examples.md) | `example/` 导读 |
| [10 工程示例](10-project-examples.md) | `project/` 导读 |
| [11 文法使用手册](11-grammar-usage.md) | 指令、语义动作与错误恢复 |
| [12 生成模式](12-modes.md) | 十六种生成模式与运行时 |
| [13 目标语言与产物](13-languages-and-products.md) | 后端选择与产物形态 |
| [14 支持矩阵](14-language-mode-support.md) | 语言、模式与增量 API 的当前范围 |
| [15 双语、内嵌资产与发布](15-i18n-and-packaging.md) | 显示选项、内嵌手册与语言服务器、Unicode 控制台、独立 Python 打包脚本 |
| [16 扩展正则表达式](16-regex-extensions.md) | 前瞻、词边界、忽略大小写、反向引用与 C99 有序匹配运行时 |
| [17 Unicode 正则模式](17-regex-unicode.md) | UTF-8 码点、Unicode 17.0 属性、词边界和简单大小写折叠 |
| [18 自定义错误提示](18-custom-error-messages.md) | 全部八种语言的错误提示回调、上下文与默认提示恢复 |
| [19 正则运行时资源预算](19-regex-runtime-budgets.md) | 步数、工作内存、回溯、前瞻、取消及结构化失败状态 |
| [20 正则兼容性与纯校验](20-regex-compatibility.md) | 逐规则引擎、语义保证、严格模式及不写盘校验 |
| [21 二进制增量生成](21-incremental-generation.md) | 分析缓存、语义刷新、失效与损坏恢复、C++11 子集回归 |
| [22 VS Code 工作流](22-vscode-workflow.md) | 大文法同步、取消、增量生成配置、工作区引用与安全重命名 |
| [23 Lepus 后端](23-lepus-backend.md) | Lepus 目标语言的生成方式、API 与支持边界 |
| [Lepus EBNF 语法参考](https://github.com/rabbitc999/Lepus/blob/master/doc/14-grammar-ebnf.md) | 当前词法、全部产生式、匿名函数类型、泛型入口与语义检查边界 |
| [Lepus 语言哲学](https://github.com/rabbitc999/Lepus/blob/master/doc/03-core-concepts.md) | 简单编写，到处运行；值语义、显式引用、资源管理与没有移动语义 |
| [文档维护规范](../project/document/README.md) | 统一口径、技术文档模板、文档生成器与同步检查 |

## mc 连外部数据库

Lepus 支持 MySQL、SQLite、PostgreSQL 与 Microsoft SQL Server / ODBC 的标准库模块
`std::sql`、宿主胶水 `glue/std_sql.hpp`，
以及 Lepus 工程内的[学生信息管理示例工程](https://github.com/rabbitc999/Lepus/blob/master/example/students-server/README.md)；保留的建表 / 初始数据 /
查询 SQL 在 `lepus/sql-lepus/sql/`，库的统一入口与后端选择见
[14 SQL 数据库](https://github.com/rabbitc999/Lepus/blob/master/doc/33-reflection-serialization-sql.md)，详细接口见 sql-lepus 接口参考。

## mc 连外部网络

[13 TCP 与极简 HTTP](https://github.com/rabbitc999/Lepus/blob/master/doc/07-stdlib-overview.md)：标准库模块
`std::net` 与宿主胶水 `glue/std_net.hpp`，
提供 `TcpSocket` / `TcpListener`（连接超时、行读取）与 `HttpClient`（GET / POST、
`parseUrl` / `buildRequest` / `parseResponse`）；确定性样例是
[`lepus/example/samples/std_net.lep`](https://github.com/rabbitc999/Lepus/blob/master/example/samples/std_net.lep)。

Lepus 的后置泛型声明、函数与回调类型推导、枚举变体简写见泛型与集合文档。

Lepus 的 `ExplicitConvert<T>` / `ImplicitConvert<T>`、显式选择规则与转换结果所有权见类型转换接口。

## mc 集合字面量与键协议

泛型与集合文档说明 Set `{a,b,c}`、Map `{key:value,...}`、空集合上下文推导、自定义键的 `Equal<bool,T>` 和可选 `Hash<T>`。

## mc 值语义与所有权

Lepus 支持 `static T new(...)` 构造入口：自动提供 self 并返回拥有值，禁止丢弃结果；默认声明仍合法。宏临时变量支持 `$0`～`$9`。

03 值语义与链式调用：字符串 / 容器 / 大数与结构体
一样是值类型（`String a = b;` 是复制不是别名），编译器通过内部临时变量管理 `f().g()`
等链式调用的中间值，完整表达式结束后自动析构、释放资源并清空内部槽，保留最终值和所需的借用来源，并说明相关修复及回归；迭代链见
05 标准迭代器。

Lepus 的两层增量缓存、共享依赖校验和限制见[部署文档](https://github.com/rabbitc999/Lepus/blob/master/doc/06-deployment.md)。

## 统一构建目录

<!-- docgen:build-summary:start -->
构建入口是 `build.sh`，实际步骤由 `build-all.sh` 维护。Windows 下使用 Git Bash；Linux/macOS 使用 POSIX Shell。脚本构建 HareParse、库和示例工程（Lepus、MetroLang、RustView 已拆分为独立仓库），不运行测试；上述独立仓库不在统一构建清单中。

```sh
sh build.sh                  # 全量构建
sh build.sh --list           # 检测环境并显示清单，不执行构建
sh build.sh -q               # 安静模式，完整输出仍写入日志
sh clear.sh --dry-run        # 预览清理范围
sh clear.sh                 # 清理构建产物
```

构建入口产生的文件统一位于仓库根目录 `build/`。生成器在 `build/bin/`（release 在 `build/bin/release/`），Rust 在 `build/rust/<库名>/`，示例工程在 `build/project/<工程名>/`，日志在 `build/build-all/logs/`，全量示例产物在 `build/build-all/examples/`。Lepus、MetroLang 与 RustView 已拆分为独立仓库，构建产物在各自仓库里。

构建入口实际运行 `python3`、`python`，选择可用的 Python 3 解释器，跳过不可用的占位程序和 Python 2；检测结果传给生成器，也可用 `PYTHON` 覆盖选择结果。C/C++ 编译器、make 和 Python 3 为必需工具；Rust、.NET 和 Go 工具链缺失时跳过对应部分。

清理仅限根目录 `build/` 内被 Git 忽略的产物，保留受版本控制文件、未被忽略的未跟踪文件和嵌套 Git 仓库。检测到 `tmp/build.lock` 时拒绝清理；旧版本留在其他目录的产物不在清理范围内。

上述路径适用于仓库统一构建入口。直接运行子工程的 Makefile、生成脚本或测试工具时，以该工程文档中的路径为准。

维护这段说明时修改 [统一配置](../project/document/documentation.json) 或 [构建说明模板](../project/document/templates/build-summary.md.tmpl)，再运行文档生成器。模板使用方法见 [文档维护规范](../project/document/README.md)。
<!-- docgen:build-summary:end -->

## 自定义分配器库

[`library/allocator`](../library/allocator/README.md) 提供链表池、线性池和 C++17 容器适配，命名空间为 `Memory`。构建产物位于 `build/allocator/`。

## Lepus 二进制流解析

`std::binary::BinaryReader` 通过 C++ 胶水读取大小端基础数值、定长字符串与数组。
核心 lang IEEE 754 双向接口 提供 float32/float64 与四/八字节列表及数组互转。

## Lepus JSON 与 TOML

JSON/TOML 解析接口 将 `library/json`、`library/toml` 的副本接入 Lepus，提供解析、嵌套读取、遍历、诊断和序列化。

[Lepus 引用与迭代](https://github.com/rabbitc999/Lepus/blob/master/doc/22-resource-lifetime.md)：非空引用、默认解引用、只读转换、字符串 / 大数 / 集合 / 字典 / 元组引用、字符单字节读写、只读方法检查、列表元素写回与 P1 三后端回归。

[Lepus 库迁移与回归验证](https://github.com/rabbitc999/Lepus/blob/master/doc/11-algorithm-validation.md)：标准库、机器人、GUI、小车与海龟测试入口。

Lepus 的无名变长符号表、lepus/CLI 开关、空间实测与逆向成本见[符号压缩文档](https://github.com/rabbitc999/Lepus/blob/master/doc/16-compact-symbols.md)。

Lepus PC 字节码加密：独立 OpenSSL 工具、密钥管理与完整性验收。

[Lepus 可选 panic 调用栈](https://github.com/rabbitc999/Lepus/blob/master/doc/18-panic-trace.md)：宏裁剪与隐藏符号时的诊断策略。

Lepus 行级调试支持宏正文、include 与子模块来源，见 [panic 与源码行号](https://github.com/rabbitc999/Lepus/blob/master/doc/18-panic-trace.md)。

[Lepus 元组只读引用回归](https://github.com/rabbitc999/Lepus/blob/master/example/samples/p2_tuple_readonly.lep)：元组元素与嵌套来源的只读引用、整体替换及独立复制；语义说明见 [07 引用与迭代](https://github.com/rabbitc999/Lepus/blob/master/doc/22-resource-lifetime.md)。

Lepus 标准迭代器：惰性链式适配、列表引用迭代、所有权转移与消费操作。

Lepus 导入与名称解析说明无需 using 的库名称简写、用户声明优先和同名导入消歧。

Lepus 注解与反射的编译期注解、`@Reflect` 自动元数据生成、Lepus 类型化查询与全部形式调用桥接、三后端端到端验收与性能报告、实现范围、默认关闭开关与产物兼容规则见 30 注解与反射。

Lepus 类型与内存审计修复、loop 拥有值退出及借用返回的三后端回归见 值与资源。

Lepus 新建应用工程（lepus new）

[Lepus 简短场景指令与拖入文件运行](https://github.com/rabbitc999/Lepus/blob/master/doc/09-toolchain.md)。

Lepus 运算符所有权与提前退出测试见09 值与资源。

Lepus Web 学生信息管理系统：真实 HTTP、SQLite、浏览器页面和三后端验收。

Lepus 仅保留 AOT 与 `.svmb` 编译产物，删除内嵌 VM 导出。参见导出器说明与[场景指令](https://github.com/rabbitc999/Lepus/blob/master/doc/09-toolchain.md)。

Lepus 的 lambda 与函数指针回调支持迭代器链与回调签名推导，支持独立的按值捕获环境，并拒绝引用和缺少安全复制协议的资源捕获；完整样例见[lambda_callbacks.lep](https://github.com/rabbitc999/Lepus/blob/master/example/samples/lambda_callbacks.lep)。

Lepus 递归与嵌套所有权：局部聚合、引用槽、数组、函数值与接口组合；[计算器工具链示例](https://github.com/rabbitc999/Lepus/blob/master/example/calculator/README.md)包含扁平与嵌套两种 AST。

[HareParse 文法 AI 插件](../plugins/hareparse-grammar/README.md)：内嵌生成器与起步模板，覆盖十六种模式、全部目标语言的编写、检查、生成、构建与试跑工具。

