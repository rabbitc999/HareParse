---
name: hareparse-grammar
description: 用 HareParse 编写、检查、生成并试跑完整的 .gram 文法：覆盖词法、优先级、语义动作、错误恢复、十六种生成模式和全部目标语言（C99/C++17/Python/C#/Rust/Go/Java/JS/Lepus/gram 导出）；适合配置、协议、脚本、编程语言、表达式、数据格式、模板、日志与嵌套文档。
---

# HareParse 文法与代码生成

HareParse 是 yacc 风格的解析器生成器：`.gram` 是这门语言的权威语法定义。生成器把文法编译成分析表（或递归下降 / 图表解析器），再按目标语言输出一套能直接编译运行的解析器工程。插件内嵌生成器原程序（`bin/HareParse.exe`）、内嵌手册与起步模板（`bin/grammar-template.gram`），安装后不依赖仓库路径或系统 `PATH`。

## 工具与工作循环

| 工具 | 用途 |
| --- | --- |
| `hareparse_write_grammar` | 写入完整 `.gram` 原文，默认立即执行 `--check` 并返回诊断。 |
| `hareparse_check` | 只做内存校验（冲突、缺动作、正则兼容性），不写任何产物。 |
| `hareparse_generate` | 生成目标语言产物；模式、语言、命名空间、前缀、类名、教学模式、Mermaid、构建文件、条件编译、增量缓存、完整报告都可指定。 |
| `hareparse_parse_text` | 用已生成的 Python 解析器跑**整段文本**，返回状态、错误行列、语义值、语法树和 `%commit` 片段。 |
| `hareparse_run` | 构建并运行生成目录里的演示程序：C / C++17 / Python / C# / Rust / Go / Java / JS。 |
| `hareparse_man` | 查构建期内嵌手册：文档页、文法指令、生成模式、目标语言、示例文法、关键词搜索。 |
| `hareparse_dump` | 调试视图：LR 自动机、SLR/LALR 表、Mermaid 状态机、`%epilogue`/`%main` 段、`%if` 预处理文本。 |
| `hareparse_clear` | 按 `.hareparse-manifest` 清单清理生成目录。 |

所有路径相对会话工作目录解析；写入默认限制在会话工作目录内。生成目录里会留下 `RESULT.md`（生成信息、产物清单、冲突消解与运行方法）和 `.hareparse-manifest`（产物清单）。

推荐循环：

1. 不确定指令、模式或语言支持时先 `hareparse_man`（`['directives']`、`['modes']`、`['languages']`、`['examples']`、`['--search','关键词']`、`['05']`）。
2. `hareparse_write_grammar` 写入完整文法，默认 `--check` 立即报冲突与缺动作。
3. `hareparse_generate` 生成 **Python** 先跑通结构（免编译，迭代最快）。
4. `hareparse_parse_text` 喂代表性样本，核对错误行列、状态、语法树与语义值。
5. 结构稳定后再加语义动作，再换交付语言（C99 / C++17 / C# / Rust / Go / Lepus），最后用 `hareparse_run` 验证真实编译和运行。

## 先判断值不值得用

这个工具回答“文本长什么样、怎样生成解析器”，不回答“文本为什么这样、命令是否有效、数值从哪来”。出现下面任意两条，就停止扩文法，转回状态机 / 逆向 / 样本对比：

1. 目标在语义层、状态层或逆向层（常量来源、命令配对、协议约束、设备行为）。
2. 输入太“平”（逐行命令、固定参数、没有嵌套 / 表达式 / 优先级 / 多义）。
3. 真正要看的字段在注释里，文法已经把它跳过。
4. 需要命令表、固件差异、状态机、回归样本——这些不会因为用了生成器就自动获得。
5. 一次性任务，复用次数很少，接入与维护成本高于节省。
6. 连续 2–3 轮「改文法 → 重生成 → 喂样本」仍分不开有效 / 无效输入。
7. 开始写大动作、大 `%main` 来补业务规则。

止损后保留已有 `.gram` 与生成的解析器做辅助语法检查，把投入转到语义与状态验证。一句话：**能解析 ≠ 能归因。**

## `.gram` 文件骨架

```gram
%lang python          /* 目标语言；命令行 --lang 优先 */
%mode ielr            /* 默认 IELR(1)；也可 ll1 / lalr / glr / earley / cyk / dd / lex … */
%start file           /* 起始非终结符，缺省第一条规则的左部 */

%define EQ    "="     /* 固定字面量 Token；一个名字可写多个候选 "==|=" */
%token regex NUMBER "[0-9]+"              /* 正则 Token */
%identifier "[A-Za-z_][A-Za-z0-9_]*"      /* 标识符 → 内建 VARIABLE */
%trivia "\s*#.*"                          /* 注释：匹配即丢 */

%tree                 /* 先生成语法树，不写语义动作 */

file  := pairs;
pairs := pairs pair | pair;
pair  := VARIABLE EQ value;
value := NUMBER | VARIABLE;
```

- 规则一行一条，或用 yacc 风格的 `|` 分支、整条以 `;` 结束；两种可以混用。
- 空产生式写 `%empty`（或右部什么都不写）。
- 规则里的 `VARIABLE` 是 `%identifier` 的内建 Token；固定字面量用 `%define` / `%token NAME "字面量"` 登记。

## 词法层

| 写法 | 含义 |
| --- | --- |
| `%define NAME "字面量"` / `%token NAME "字面量"` | 固定字符串 Token；多个候选用 `|` 分隔（`%define EQ "==|="`）。 |
| `%token regex NAME "正则"` | 正则 Token；生成期编译成 DFA/共享运行时。 |
| `%identifier "正则"` | 声明标识符怎么切；文法里用内建 `VARIABLE`。 |
| `%trivia "正则"` / `%commit skip "正则"` | 匹配即丢，不进语法层也不进容器。 |
| `%commit [类型] "正则"` | 收进 `commits` 容器（元素 `{line, type, text}`），不打断解析。 |
| `%token NL "\n"` | 换行是**真实 Token**，不再被当空白跳过。 |
| `%token SP "\b"` | `\b` 是“一个空白字符”的通配（空格 / `\t` / 换行 / `\v` / `\f` / `\r`）。 |
| `%space " \t"` | 取 Token 前只跳过空格与制表；缺省 `"\b"` 跳过全部空白；`%space ""` 一个都不跳。 |

词法细节：

- 最长匹配优先；同长度按声明先后；固定字面量与标识符同长时字面量优先（关键字表就是这么实现的）。
- 正则不能匹配空串；`{m,n}` 的重复上限是 32；默认按 ASCII 语义，`(?u)` 才启用 Unicode。正则扩展、Unicode 与资源预算见 `hareparse_man ['17-regex-extensions']`、`['18-regex-unicode']`、`['20-regex-runtime-budgets']`。
- `%token regex` 里用 `\s`；`%space` 只接受字面空白集合。
- 一行一条记录：`%token NL "\n"` + `%space " \t"`；按空白分隔字段：`%token SP "\b"`。

## 语法层

- 冲突用优先级消解：`%left` / `%right` / `%nonassoc`，同一条指令里自左向右递增，后声明的行优先级更高；产生式末尾用 `%prec TOKEN` 借用伪 Token 的优先级（典型是一元负号）。
- `%expect N` 明确放行 N 个未消解的移进 / 归约冲突；`%teach`（或 `--teach`）不再因未消解冲突中止，并在报告里给出教学提示与对照。
- `%start NAME` 指定起始符；`%empty` 写空产生式。
- 规则缺少语义动作且没有 `%tree` 会直接报错：先 `%tree` 看结构，再按需加动作。

## 语义动作与用户代码

- 动作写在右部末尾的 `{ ... }` 里；`$1 … $n` 读右部第 n 个符号的语义值，`$$` 写本次归约的结果。动作语法就是目标语言：C 写表达式 / 语句，Python 写 Python 表达式，`&&` / `||` 在 Python 里写 `and` / `or`。
- 语义值模型按目标语言：
  - **C99 / C++17**：用 `%value_type 类型` 或 `%union` + `%type` 声明；成员绑定与 `%token <成员>` 见 `hareparse_man ['%union']`。
  - **Python**：语义值是任意对象，不需要也不接受 `%value_type` / `%union`。
  - **C# / Rust / Go**：用目标语言类型；Rust 语义值要求 `Copy + Default + Debug`。
  - **Java / JS**：窄后端，只支持 `%tree` 与结构判定，不执行语义动作。
- 用户代码：`%code … %endcode`（C / C++17 贴进 `parser_user.h`，Python 插在 import 之后）写辅助函数；`%epilogue` 贴到解析器实现末尾；`%main` 替换演示入口的正文；`%class_body … %endbody` 用于 C# / Rust / Go 的类或实现内部。
- 命名：`%class` 改 Python 类名，`%namespace` 改 C++17 命名空间与文件名前缀，`%prefix` 改 C99 公开符号前缀，`%package` 改 C# namespace / Rust crate / Go module，`%import` 追加 using / use / import。
- 文件组织：`%include "其它.gram"` 内联另一个文法；`%copy file 路径` / `%copy dir 路径` 把库文件或目录拷进产物目录；`%if` / `%elif` / `%else` / `%endif` 配合 `--define NAME[=VALUE]` 做条件编译。
- 其余指令（`%example`、`%error_context`、`%error_recovery_tokens`、`%max_errors`、`%max_stack_depth`、`%destructor`、`%cpp_only`、`%cpp_lexer`、`%mermaid`、`%build_files` …）以 `hareparse_man ['directives']` 与对应单页为准。

## 错误恢复

- `error` 产生式是 yacc 风格伪终结符：`stmt := error SEMI { yyerrok; $$ = 0; }` 丢掉坏语句、继续解析后面的语句。
- `yyerrok` 清掉错误状态，让后续错误照常上报；不写它，接下来若干 Token 内的错误会被抑制。
- `%sync TOKEN` 声明同步锚点：LR 家族在 `error` 产生式找不到恢复点时兜底，LL 家族作为额外汇入点。
- `%max_errors N` 控制一次解析最多报几条错；`%error_recovery_tokens N` 控制恢复后静默几个 Token；`%error_context` 让错误记录附带出错行原文与 `^` 指示。
- GLR / Earley / CYK 不支持 `error` 产生式与 `%destructor`；Earley / CYK 也不支持 `%sync`。

## 生成模式与目标语言

默认 `%mode ielr`（覆盖全部 LR(1) 文法）。模式共 16 种：

`ll0` / `ll1` / `llk` / `lr0` / `slr` / `slrk` / `lalr` / `lalrk` / `lr1` / `lrk` / `ielr` / `glr` / `earley` / `cyk` / `dd` / `lex`。

- **LL 家族**：要求已消除左递归；`llk` / `ll0` 配 `%lookahead`（0..INT_MAX，缺省 2）。
- **LR 家族**：`lrk` / `lalrk` / `slrk` 用 `%lookahead k` 或 `--lookahead k`（1..3），`k=1` 时分别等于 `lr1` / `lalr` / `slr`。
- **GLR**：GSS 多路径，能容忍二义文法；支持 C++17 / Python / C# / Rust / Go / Lepus。
- **Earley / CYK**：图表算法，适合二义、可空、左递归文法；支持 C++17 / Python / C# / Rust / Go / Java / JS / Lepus。
- **DD**：带回溯的递归下降，按声明顺序返回第一条成功路径；**禁止直接和间接左递归**。
- **lex**：只生成词法器。

目标语言共 10 个（`--lang` / `%lang`）：

| 目标 | 支持情况 | 产物要点 |
| --- | --- | --- |
| `c` | 16 种模式中的 13 种（无 GLR / Earley / CYK） | `parser.c/h`、`parser_token.c/h`、`parser_table.c`、`main.c`、Makefile、CMakeLists.txt |
| `cpp17` | 全部 16 种 | `parser.cpp/hpp`、`main.cpp`、Makefile、CMakeLists.txt |
| `python` | 全部 16 种 | `parser.py`、`main.py`，免编译 |
| `csharp` | 全部 16 种 | `Parser.cs`、`Token.cs`、`Program.cs`、`parser.csproj` |
| `rust` | 全部 16 种 | Cargo 工程：`src/parser.rs`、`src/token.rs`、`src/main.rs` |
| `go` | 全部 16 种 | Go 模块：`parser.go`、`token.go`、`main.go`、`go.mod` |
| `java` | 仅 `dd` / `earley` / `cyk` | 自包含 `Parser.java`，不支持动作 / 类型 / 用户代码 |
| `js` | 仅 `dd` / `earley` / `cyk` | 带可擦除类型标注的 `parser.ts`（Node.js 22+ 可直接运行） |
| `lepus` | 全部 16 种 | 自包含 `parser.lep`，需 Lepus 工具链编译 |
| `gram` | 全部 16 种（只导出） | 文法原文 + `<名字>.analysis.md` + `RESULT.md`，不生成解析器 |

默认目标 `cpp17`。Java / JS 是窄后端：没有语义动作、`%union` / `%value_type`、`%sync` / `%commit` / `%copy` 与用户代码段。完整矩阵见 `hareparse_man ['15-language-mode-support']` 与 `['14-languages-and-products']`。

## 生成、运行与产物

- `hareparse_generate` 的关键参数：`path`、`outDir`（必填）、`lang`、`mode`、`lookahead`、`namespace`、`className`、`prefix`、`cppLexer`（`std` / `dfa` / `vm`）、`defines`（`["DEBUG=1"]`）、`teach`、`mermaid`、`buildFiles`、`incremental` / `cacheDir` / `rebuild`、`fullReport`、`report`、`syntaxOnly`、`strictRegex`、`timeoutMs`。
- 不传 `lang` / `mode` 时尊重文法里的 `%lang` / `%mode`；都没写才用 CLI 缺省（`cpp17` / `ielr`）。
- `RESULT.md` 记录生成信息、产物清单、冲突消解与运行方法；`--full-report` 附上 FIRST/FOLLOW、ACTION/GOTO 完整报告；`--report 文件` 单独导出分析报告。
- 运行：C / C++17 用生成目录的 `make`（产物 `parser_demo`），Python `python main.py`，C# `dotnet run`，Rust `cargo run --quiet`，Go `go run .`，Java `javac -encoding UTF-8 Parser.java` + `java Parser`，JS `node parser.ts`（Node 22+）。演示程序默认从标准输入读**一行**；`%main` 可以换成自己的入口。Rust 产物声明了 `regex` crate，首次构建需要联网或配置镜像。
- `hareparse_parse_text` 用 Python 产物的 `run_text` 解析**整段文本**，比演示入口更适合多行日志 / 文件；返回 `status`、`errors`、`result`、`tree`、`commits`。
- 增量解析只在 C++17 与 Rust 提供（`runTextIncremental` / `run_text_incremental`），与 Make/Cargo 的增量构建无关。
- `--no-build-files` 只留目标语言源码；`--syntax-only` 强制语法树并忽略动作，用于 Token / 语法树可视化。

## 排查

- 生成失败先看 `--check` 的 `stderr`：行列号、缺失语义动作、模式与语言不匹配、正则错误都会直接指出。
- 冲突：优先用 `%left` / `%right` / `%nonassoc` / `%prec`；确实要保留未消解冲突时用 `%expect N`；`%teach` 会给出教学对照，不中止生成。
- 看不清状态机时用 `hareparse_dump`：`lr-automaton`（项集与转移）、`slr-tables`（ACTION/GOTO）、`mermaid`（图）、`sections`（`%epilogue` / `%main` / `%code` 段与动作行号）、`preprocess`（`%if` 处理后的文本与行号映射）。
- 常见错误：LL / DD 文法写了左递归；缺少语义动作也没开 `%tree`；Python 里写了 `%value_type` / `%union`；Java / JS 用了动作或 `%commit`；Rust 语义值不满足 `Copy + Default + Debug`；正则匹配空串或重复上限超过 32。
- 语法通过只说明文本形状正确，不代表命令语义、协议状态或数值来源正确。

## 分析日志 / DSL 的专门套路

- 先写十来行小文法，用 `%tree` 跑通结构；`hareparse_parse_text` 只喂 `head` 出来的前若干行。
- 一行一条记录：`%token NL "\n"` + `%space " \t"`；按空白分隔字段：`%token SP "\b"`。
- 逐行解析整份文件时**只打印失败行号与原因**，不要把所有成功输出贴进上下文：

  ```python
  import parser
  engine = parser.Parser()
  bad = 0
  for number, line in enumerate(open('a.log', encoding='utf-8'), 1):
      if engine.run_text(line.rstrip('\n')) != parser.STATUS_OK:
          bad += 1
          print(number, engine.last_error)
  print('失败行数:', bad)
  ```

  让命令自己收敛，用几十 token 的输出换掉整份日志。
- 注释 / 标注里有关键信息时用 `%commit 类型 "正则"` 收进 `commits`，只把片段喂给模型，而不是把整份日志读进上下文。
- 命中“先判断值不值得用”里的止损信号就停，保留文法做辅助检查。

## 交付与集成

- 要交付解析库时给 C99 / C++17 产物：生成目录自带 Makefile / CMakeLists.txt，`%copy` 可把运行时依赖一并放进产物；`parser.c` / `parser.cpp` 只依赖同目录的 token / table 文件。
- 生成代码的公开 API、错误结构、所有权与内存管理见 `hareparse_man ['08']`；目标语言与产物形态见 `['14']`；完整工程示例见 `['11']`。
- 生成器自己的内嵌手册、模板与发布方式见 `['16']`；新增 / 修改指令后应以 `hareparse_man ['directives']` 的实际输出为准。
- 单文件可用是设计目标：生成器不读仓库模板；产物落在 `--out-dir`，由 `.hareparse-manifest` 登记，`hareparse_clear` 可精确清理。

## 别做的事

- 不要手写解析驱动或把生成器当“词法提取器”：块栈、逐行折叠、分析表流程都交给 `.gram` 与生成器。
- 不要用一个万能 `KW` / 任意 token 流 / 一个大正则代替产生式；关键字、块、命令都要在产生式里点名与组合。
- 不要把整份日志、整份产物或完整解析输出读进上下文——先 `head`、先只打印失败行。
- 不要为了“以后可能用到”写大文法：先 `%tree` 把结构跑通，再按需加动作。
- 不要把 Python 产物当交付物（它是分析工具）；交付用 C / C++17（或项目指定的目标语言）。
- 不要把语法通过当成命令有效：文法接受只说明文本形式匹配，不说明协议、状态、固件或数值来源正确。
