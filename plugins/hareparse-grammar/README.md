# hareparse-grammar

DeepSeek Harness 插件：让模型直接编写、检查、生成、构建并试跑 **完整** 的 HareParse `.gram` 文法。插件内嵌真实的 HareParse 生成器原程序（`bin/HareParse.exe`）、起步模板与文法编写 Skill，安装后不依赖仓库路径或系统 `PATH`。

覆盖范围：

- **16 种生成模式**：`ll0` / `ll1` / `llk` / `lr0` / `slr` / `slrk` / `lalr` / `lalrk` / `lr1` / `lrk` / `ielr` / `glr` / `earley` / `cyk` / `dd` / `lex`。
- **10 个目标**：C99、C++17、Python、C#、Rust、Go、Java、JavaScript/TypeScript、Lepus，以及只导出文法的 `gram`。
- **完整文法能力**：`%define` / `%token` / `%token regex` / `%identifier` / `%trivia` / `%commit` / `%space`、优先级与 `%prec`、`%tree`、语义动作、`%value_type` / `%union` / `%type`、`%code` / `%epilogue` / `%main` / `%class_body`、`%include` / `%copy` / `%if`、错误恢复（`error` 产生式 / `%sync` / `%max_errors` …）、增量分析缓存、Mermaid、教学模式、完整分析报告。

## 包含什么

| 组成 | 说明 |
| --- | --- |
| `hareparse-grammar` Skill | 来自 `skills/hareparse-grammar/SKILL.md`，工作前自动进入模型上下文；构建时附加 DSH 工具使用说明。 |
| `hareparse_write_grammar` | 写入完整 `.gram`，默认立即 `--check`。 |
| `hareparse_check` | 只做内存校验：冲突、缺失语义动作、正则兼容性；不写产物。 |
| `hareparse_generate` | 生成目标语言工程；支持模式、语言、`lookahead`、`namespace`、`className`、`prefix`、`cppLexer`、`defines`、`teach`、`mermaid`、`buildFiles`、`incremental` / `cacheDir` / `rebuild`、`fullReport`、`report`、`syntaxOnly`、`strictRegex`。 |
| `hareparse_run` | 构建并运行生成目录的演示程序：C / C++17 / Python / C# / Rust / Go / Java / JS。 |
| `hareparse_parse_text` | 用 Python 产物解析**整段文本**，返回状态、错误行列、语义值、语法树、`%commit`。 |
| `hareparse_man` | 查询构建期内嵌手册：文档页、指令、模式、语言、示例、关键词搜索、JSON。 |
| `hareparse_dump` | LR 自动机、SLR/LALR 表、Mermaid、`%epilogue`/`%main`/`%code` 段、`%if` 预处理文本。 |
| `hareparse_clear` | 按 `.hareparse-manifest` 清单清理产物。 |
| `bin/HareParse.exe` | 插件自带的生成器原程序，由 `npm run package` 从仓库构建产物复制；`bin/bundle.json` 记录大小、SHA-256 与构建时间。 |
| `assets/grammar-template.gram` | 起步模板（`key=value`、`%tree`、注释与指令速查），安装时同时复制到 `bin/`。 |

插件以 host-plane 行注册：工具进入全局工具层，Skill 进入全局 Skill 层，所有 agent preset 都能继承，不需要修改某个预设。

## 安装

插件包必须是“已打包”状态：先运行打包脚本，把生成器复制到 `bin/`。

```powershell
cd plugins/hareparse-grammar
node scripts/package.mjs
```

打包脚本按以下顺序寻找生成器：`HAREPARSE_BIN` / `HAREPARSE_COMPILER` → 仓库 `build/bin/HareParse(.exe)` → 仓库根目录 → `PATH`。仓库里的生成器用 `make -C source` 构建；`bin/HareParse.exe` 约为 5.6 MiB。

然后在 DeepSeek Harness 中任选一种方式安装：

1. **界面（推荐）**：侧边栏 **Plugins → Add plugin → Install a third-party plugin**，填入本目录的绝对路径，例如
   `D:\Desktop\Workspace\HareParse\plugins\hareparse-grammar`。界面会执行 pnpm 安装、把本包加入 profile bundles，并在 HMR 可用时立即装载。
2. **命令行**：

   ```powershell
   dsh plugin --profile desktop add "link:D:\Desktop\Workspace\HareParse\plugins\hareparse-grammar"
   ```

   当前 CLI 在安装带 `dsh.bundle.patch` 的包时会把它追加到 profile 的 `dsh.profile.bundles`。如果目标版本没有自动追加，手工在 `~/.dsh/profiles/<profile>/package.json` 的 `dsh.profile.bundles` 末尾加入 `"hareparse-grammar"`，配置 HMR 会重新组合。

安装后新开一个会话（或让 HMR 完成重组），Skill 目录会出现 `hareparse-grammar`，工具列表会出现上述八个 `hareparse_*` 工具。

> 桌面版 profile 由 Electron 应用管理。安装包操作最好在应用内 Plugins 页面完成；使用 `dsh plugin` 时不要同时让应用执行其他插件操作。

## 配置

profile 的 `cordis.patch.yml` 可以覆盖该行配置，字段都是可选的：

```yaml
- id: hareparse
  config:
    compiler: 'D:\tools\HareParse.exe'   # 默认使用插件自带 bin/HareParse.exe
    workspaceRoot: 'D:\work\demo'          # 没有 agent 工作目录时的兜底
    allowOutsideWorkspace: false           # 默认只允许写会话工作目录内
    checkOnWrite: true                     # write_grammar 默认立即检查
    python: 'python'                       # parse_text 用的解释器（缺省从 PATH 找）
    timeoutMs: 120000                      # 默认检查/生成超时
    maxOutputChars: 12000                  # 工具结果截断上限
```

## 典型用法

最小文法（插件模板的简化版）：

```gram
%lang python
%mode ielr
%start file
%define EQ "="
%define COMMA ","
%token regex NUMBER "[0-9]+"
%identifier "[A-Za-z_][A-Za-z0-9_]*"
%trivia "\s*#.*"
%tree

file  := pairs;
pairs := pairs COMMA pair | pair;
pair  := VARIABLE EQ value;
value := NUMBER | VARIABLE;
```

闭环：

1. `hareparse_write_grammar` 写入并 `--check`。
2. `hareparse_generate` 生成 Python 产物（`{"lang":"python","outDir":"out"}`）。
3. `hareparse_parse_text` 喂 `a=1,b=2`，核对 `tree` 与错误行列。
4. 结构稳定后加语义动作，再生成交付目标（C99 / C++17 / C# / Rust / Go / Lepus）。
5. `hareparse_run` 真实编译运行；`hareparse_clear` 清掉产物。

按场景：

| 场景 | 用法 |
| --- | --- |
| 日志 / DSL 分析 | `--lang python` + `%token NL "\n"` + `%space " \t"`；`hareparse_parse_text` 只喂 `head` 出来的样本，失败才打印行号。 |
| 表达式 / 运算符优先级 | `%left` / `%right` / `%nonassoc` + `%prec`；`hareparse_dump` 看冲突状态。 |
| 一次报多个错 | `error` 产生式 + `yyerrok`、`%sync`、`%max_errors`；生成后用坏样本验证。 |
| 交付解析库 | C99 / C++17 产物；`buildFiles` 打开（默认），`%copy file 路径` / `%copy dir 路径` 打包运行时依赖；API 见 `hareparse_man ['08']`。 |
| 二义 / 可空 / 左递归 | `--mode glr` / `earley` / `cyk`；注意 Java / JS 只有 `dd` / `earley` / `cyk` 且不执行动作。 |
| 只做词法分析 | `--mode lex`。 |
| 只要文法与分析报告 | `--lang gram`。 |
| 条件编译 / 多目标 | `%if` + `defines: ["DEBUG=1"]`，或按目标切换 `%lang`。 |

## 命令行功能对照

插件暴露生成器的绝大部分能力：

- 生成：`--out-dir`、`--lang`、`--mode`、`--lookahead`、`--namespace`、`--class`、`--prefix`、`--cpp-lexer`、`--define`、`--teach` / `--no-teach`、`--mermaid` / `--no-mermaid`、`--build-files` / `--no-build-files`、`--incremental` / `--no-incremental`、`--cache-dir`、`--rebuild`、`--report`、`--full-report`、`--syntax-only`、`--strict-regex`。
- 校验与调试：`--check`、`--dump-lr-automaton`、`--dump-slr-tables`、`--dump-mermaid`、`--dump-sections`、`--preprocess-only`。
- 维护与查询：`clear --out-dir`、`man …`。
- 未暴露：`zip`（生成器自身的发布打包）、`lsp`（编辑器语言服务器）、`--ui-lang`（工具输出跟随生成器自动选择；手册用 `hareparse_man` 的 `lang` 参数）。这些与“编写 / 生成 / 验证文法”无关。

## 开发与验证

```powershell
cd plugins/hareparse-grammar
node scripts/build.mjs        # 只重新生成 lib/skill-content.js
node scripts/package.mjs      # 重新生成 Skill + 复制生成器和模板
node --test test/plugin.test.mjs
```

测试覆盖：插件注册、生成器与模板确实在包内、`write_grammar` + `--check`、坏文法诊断、Python 生成与 `parse_text`、Python 演示入口运行、C 演示入口编译运行、内嵌手册、LR 自动机 dump、`clear`。需要 Python / C 编译器的用例在缺少工具链时自动跳过。

修改 `skills/hareparse-grammar/SKILL.md` 后必须重新运行 `node scripts/build.mjs`（或 `package.mjs`），并提交生成后的 `lib/skill-content.js`。`bin/HareParse.exe` 与 `bin/bundle.json` 随仓库提交，保证克隆后直接可用；重新打包后 SHA-256 变化时，把新的二进制一并提交。仓库 `.gitignore` 的 `*.exe` 只影响新增文件，已跟踪的生成器不受影响。

## 设计边界

- 插件只调用 HareParse 官方工具链，不重新实现文法分析、冲突消解或代码生成。
- 工具结果按 `maxOutputChars` 截断，保留头尾；完整输出不落盘。
- 写入默认限制在会话工作目录内；确需写外部路径时显式配置 `allowOutsideWorkspace: true`。
- 生成目录里的 `.hareparse-manifest` 是清理依据；不要手工伪造，否则 `hareparse_clear` 的行为不可预测。
- `hareparse_parse_text` 只支持 `--lang python` 产物；其他目标用 `hareparse_run` 的演示入口或目标语言自己的 API。
- Rust 产物依赖 `regex` crate，首次 `cargo run` 可能需要网络/镜像；C / C++17 / C# / Go / Java / JS 需要本机对应工具链。
- Lepus 目标只生成 `parser.lep`，编译需要 Lepus 工具链（见 [dsh-lepus](https://github.com/rabbitc999/Lepus) 插件）。
- 打包后的插件是平台相关的：`bin/HareParse.exe` 来自当前 Windows x64 构建。其他平台需要在对应构建上重新运行 `package.mjs`。
