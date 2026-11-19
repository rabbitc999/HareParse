// hareparse-grammar — DeepSeek Harness plugin for HareParse grammars.
//
// The plugin registers one embedded skill (hareparse-grammar) and eight
// host-plane tools. Tools live on the global tools layer, so every agent
// preset inherits them; the skill lands in the global skills layer for the
// same reason. The package carries the real HareParse generator under
// bin/ (5.x MiB) and the starter grammar template, so an installed plugin
// never needs the repository checkout or a system HareParse.
//
// This module deliberately imports Node built-ins only: it can be loaded from
// a profile link, an absolute path, or an installed package without a peer
// resolution setup.

import { spawn } from 'node:child_process'
import { lstat, mkdir, mkdtemp, readdir, readFile, rm, stat, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import {
  basename,
  delimiter,
  dirname,
  isAbsolute,
  join,
  normalize,
  relative,
  resolve,
} from 'node:path'
import { fileURLToPath } from 'node:url'

import { skillContent, skillDescription } from './skill-content.js'

export const name = 'hareparse-grammar'
export const inject = ['tools', 'skills', 'workingDirectory']

const PLUGIN_ROOT = dirname(dirname(fileURLToPath(import.meta.url)))

const DEFAULT_TIMEOUT_MS = 120_000
const DEFAULT_BUILD_TIMEOUT_MS = 300_000
const MAX_TIMEOUT_MS = 900_000
const DEFAULT_MAX_OUTPUT_CHARS = 12_000

const LANGUAGES = ['c', 'cpp17', 'python', 'csharp', 'rust', 'go', 'java', 'js', 'lepus', 'gram']
const MODES = [
  'll0', 'll1', 'llk',
  'lr0', 'slr', 'slrk', 'lalr', 'lalrk', 'lr1', 'lrk', 'ielr',
  'glr', 'earley', 'cyk', 'dd', 'lex',
]
const DUMP_KINDS = {
  'lr-automaton': '--dump-lr-automaton',
  'slr-tables': '--dump-slr-tables',
  mermaid: '--dump-mermaid',
  sections: '--dump-sections',
  preprocess: '--preprocess-only',
}

/** Python 驱动：用生成产物解析整段文本，最后打印一行 JSON。 */
const PYTHON_DRIVER = `# -*- coding: utf-8 -*-
"""HareParse Python 解析器驱动：读整段标准输入，输出单行 JSON。"""
import importlib
import json
import sys


def _int(value, fallback=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def _error_dict(error):
    return {
        "code": _int(getattr(error, "code", 0)),
        "line": _int(getattr(error, "line", 0)),
        "column": _int(getattr(error, "column", 0)),
        "message": str(getattr(error, "message", error)),
    }


def main():
    out_dir = sys.argv[1]
    class_name = sys.argv[2] if len(sys.argv) > 2 else "Parser"
    text = sys.stdin.read()
    result = {"ok": False}
    try:
        sys.path.insert(0, out_dir)
        module = importlib.import_module("parser")
        cls = getattr(module, class_name, None)
        if cls is None:
            cls = getattr(module, "Parser", None)
        if cls is None:
            raise RuntimeError("生成的 parser.py 里找不到解析器类")
        engine = cls()
        status = engine.run_text(text)
        result["status"] = _int(status)
        result["statusName"] = str(module.status_name(status)) if hasattr(module, "status_name") else str(status)
        result["ok"] = status == module.STATUS_OK
        last = getattr(engine, "last_error", None)
        if last is not None:
            result["error"] = _error_dict(last)
        errors = []
        for error in getattr(engine, "errors", []) or []:
            errors.append(_error_dict(error))
        result["errors"] = errors
        value = getattr(engine, "result", None)
        if value is not None:
            result["result"] = repr(value)[:4000]
        commits = []
        for commit in getattr(engine, "commits", []) or []:
            if isinstance(commit, dict):
                commits.append({
                    "line": _int(commit.get("line")),
                    "type": str(commit.get("type", "")),
                    "text": str(commit.get("text", "")),
                })
            else:
                commits.append({
                    "line": _int(getattr(commit, "line", 0)),
                    "type": str(getattr(commit, "type", "")),
                    "text": str(getattr(commit, "text", "")),
                })
        if commits:
            result["commits"] = commits
        try:
            tree = engine.parse_tree(text)
        except Exception:
            tree = None
        if tree is not None:
            render = getattr(tree, "to_string", None)
            result["tree"] = (render() if callable(render) else str(tree))[:12000]
    except Exception as exc:
        result["ok"] = False
        result["error"] = {
            "code": 0,
            "line": 0,
            "column": 0,
            "message": "%s: %s" % (type(exc).__name__, exc),
        }
    print(json.dumps(result, ensure_ascii=True))


if __name__ == "__main__":
    main()
`

/**
 * Register the HareParse skill and tools on the calling (host) context.
 * @param ctx - Cordis context carrying `tools`, `skills`, and `workingDirectory`.
 * @param config - optional row configuration (compiler, workspaceRoot, limits, python).
 */
export function apply(ctx, config = {}) {
  const settings = normalizeSettings(config)

  ctx.skills.register({
    name: 'hareparse-grammar',
    description: skillDescription,
    content: skillContent,
    invocation: { modelInvocable: true, userInvocable: true },
    provider: name,
    source: 'runtime',
  })

  for (const definition of createTools(ctx, settings)) ctx.tools.register(definition)
}

/**
 * Validate row configuration once at mount time.
 * @param config - raw row configuration.
 * @returns normalized settings used by every tool call.
 */
function normalizeSettings(config) {
  const compiler =
    typeof config.compiler === 'string' && config.compiler.trim() !== ''
      ? config.compiler.trim()
      : undefined
  const workspaceRoot =
    typeof config.workspaceRoot === 'string' && config.workspaceRoot.trim() !== ''
      ? resolve(config.workspaceRoot.trim())
      : undefined
  return {
    compiler,
    workspaceRoot,
    allowOutsideWorkspace: config.allowOutsideWorkspace === true,
    checkOnWrite: config.checkOnWrite !== false,
    python:
      typeof config.python === 'string' && config.python.trim() !== ''
        ? config.python.trim()
        : undefined,
    timeoutMs: positiveInteger(config.timeoutMs, DEFAULT_TIMEOUT_MS, MAX_TIMEOUT_MS),
    maxOutputChars: positiveInteger(config.maxOutputChars, DEFAULT_MAX_OUTPUT_CHARS, 200_000),
  }
}

/**
 * Build the model-facing tool definitions.
 * @param ctx - host context.
 * @param settings - normalized row configuration.
 * @returns tool definitions to register.
 */
function createTools(ctx, settings) {
  return [
    writeGrammarTool(ctx, settings),
    checkTool(ctx, settings),
    generateTool(ctx, settings),
    runTool(ctx, settings),
    parseTextTool(ctx, settings),
    manTool(ctx, settings),
    dumpTool(ctx, settings),
    clearTool(ctx, settings),
  ]
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

function writeGrammarTool(ctx, settings) {
  return {
    name: 'hareparse_write_grammar',
    description:
      '写入完整 HareParse .gram 文法文件（UTF-8），默认立即用插件内置的 HareParse 原程序执行 --check：只做内存校验，报告冲突、缺失语义动作、正则兼容性等问题，不写任何产物。适合创建新文法或整体替换旧文法；改少量行用通用编辑工具即可。路径相对会话工作目录。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        path: {
          type: 'string',
          description: '目标 .gram 文件，相对会话工作目录或工作目录内的绝对路径。',
        },
        grammar: {
          type: 'string',
          description: '完整 .gram 原文。不要包含 Markdown 代码围栏。',
        },
        check: {
          type: 'boolean',
          description: '写入后执行 --check。默认 true；只在工具链暂不可用时传 false。',
        },
        mode: {
          type: 'string',
          enum: MODES,
          description: '校验用的生成模式；省略时用文法里的 %mode（再缺省是 ielr）。',
        },
        lang: {
          type: 'string',
          enum: LANGUAGES,
          description: '校验用的目标语言；省略时用 %lang（再缺省是 cpp17）。',
        },
        teach: {
          type: 'boolean',
          description: 'true 传 --teach，false 传 --no-teach；省略时不覆盖文法设置。',
        },
        defines: {
          type: 'array',
          items: { type: 'string' },
          description: '条件编译变量，形如 "DEBUG=1"；等价于重复传 --define。',
        },
        strictRegex: {
          type: 'boolean',
          description: '拒绝未证明共享语义的原生正则（--strict-regex）。',
        },
        timeoutMs: {
          type: 'integer',
          description: '校验超时毫秒数，上限 900000。',
        },
      },
      required: ['path', 'grammar'],
    },
    output: {
      schema: {
        type: 'object',
        additionalProperties: false,
        properties: {
          path: { type: 'string' },
          bytes: { type: 'integer' },
          compiler: { type: 'string' },
          checked: { type: 'boolean' },
          ok: { type: 'boolean' },
          exitCode: { type: 'integer' },
          timedOut: { type: 'boolean' },
          stdout: { type: 'string' },
          stderr: { type: 'string' },
        },
        required: ['path', 'bytes', 'compiler', 'checked', 'ok', 'exitCode'],
      },
      render: (_args, value) => [{ type: 'text', text: renderWriteResult(value) }],
    },
    async execute(args, exec) {
      const cwd = await sessionCwd(ctx, exec, settings)
      const file = targetPath(args.path, cwd, settings)
      const bytes = Buffer.byteLength(args.grammar, 'utf8')
      await mkdir(dirname(file), { recursive: true })
      await assertWritableFile(file)
      await writeFile(file, args.grammar, 'utf8')

      const shouldCheck = args.check !== false && settings.checkOnWrite !== false
      if (!shouldCheck) {
        return {
          path: file,
          bytes,
          compiler: '',
          checked: false,
          ok: false,
          exitCode: -1,
          timedOut: false,
          stdout: '',
          stderr: '',
        }
      }

      const check = await runCheck(file, cwd, settings, exec?.signal, args)
      return {
        path: file,
        bytes,
        compiler: check.compiler ?? '',
        checked: true,
        ok: check.ok,
        exitCode: check.exitCode,
        timedOut: check.timedOut,
        stdout: check.stdout,
        stderr: check.stderr,
      }
    },
  }
}

function checkTool(ctx, settings) {
  return {
    name: 'hareparse_check',
    description:
      '对已有 .gram 执行 HareParse 的 --check：在内存中完成生成校验，打印正则兼容性报告与冲突/缺动作诊断，不写任何产物。适合改完文法后快速定位错误，比 generate 更轻；返回 exitCode、stdout、stderr。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        path: {
          type: 'string',
          description: '.gram 文件，相对会话工作目录或工作目录内的绝对路径。',
        },
        mode: {
          type: 'string',
          enum: MODES,
          description: '校验用的生成模式；省略时用 %mode（再缺省是 ielr）。',
        },
        lang: {
          type: 'string',
          enum: LANGUAGES,
          description: '校验用的目标语言；省略时用 %lang（再缺省是 cpp17）。',
        },
        lookahead: {
          type: 'integer',
          description: 'LR(k)/LL(k) 的 k（0..INT_MAX；CLI 通常用 1..3）。',
        },
        teach: {
          type: 'boolean',
          description: 'true 传 --teach，false 传 --no-teach；省略时不覆盖文法设置。',
        },
        defines: {
          type: 'array',
          items: { type: 'string' },
          description: '条件编译变量，形如 "DEBUG=1"。',
        },
        strictRegex: {
          type: 'boolean',
          description: '拒绝未证明共享语义的原生正则（--strict-regex）。',
        },
        timeoutMs: {
          type: 'integer',
          description: '超时毫秒数，上限 900000。',
        },
      },
      required: ['path'],
    },
    output: {
      schema: {
        type: 'object',
        additionalProperties: false,
        properties: {
          path: { type: 'string' },
          compiler: { type: 'string' },
          ok: { type: 'boolean' },
          exitCode: { type: 'integer' },
          timedOut: { type: 'boolean' },
          stdout: { type: 'string' },
          stderr: { type: 'string' },
        },
        required: ['path', 'compiler', 'ok', 'exitCode'],
      },
      render: (args, value) => [{ type: 'text', text: renderCheckResult(args.path, value) }],
    },
    async execute(args, exec) {
      const cwd = await sessionCwd(ctx, exec, settings)
      const file = targetPath(args.path, cwd, settings)
      if (!(await isFile(file))) throw new Error(`找不到文法文件: ${file}`)
      const check = await runCheck(file, cwd, settings, exec?.signal, args)
      return { ...check, path: file, compiler: check.compiler ?? '' }
    },
  }
}

function generateTool(ctx, settings) {
  return {
    name: 'hareparse_generate',
    description:
      '用插件内置的 HareParse 从 .gram 生成解析器工程，支持全部 16 种模式与 10 个目标（c/cpp17/python/csharp/rust/go/java/js/lepus/gram）。产物写入 outDir，并生成 RESULT.md 与 .hareparse-manifest。可传 namespace/className/prefix/cppLexer/defines/teach/mermaid/buildFiles/incremental/cacheDir/rebuild/fullReport/report/syntaxOnly/strictRegex。不传 lang/mode 时尊重文法里的 %lang/%mode。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        path: {
          type: 'string',
          description: '.gram 文件，相对会话工作目录或工作目录内的绝对路径。',
        },
        outDir: {
          type: 'string',
          description: '产物目录（必填）；会递归创建，已存在的同名产物按 --out-dir 规则覆盖。',
        },
        lang: {
          type: 'string',
          enum: LANGUAGES,
          description: '目标语言；省略时用 %lang，再缺省是 cpp17。分析阶段建议先选 python。',
        },
        mode: {
          type: 'string',
          enum: MODES,
          description: '生成模式；省略时用 %mode，再缺省是 ielr。',
        },
        lookahead: {
          type: 'integer',
          description: 'LR(k)/LL(k) 的 k；只对 lrk/lalrk/slrk/llk/ll0 有意义。',
        },
        namespace: {
          type: 'string',
          description: 'C++17 命名空间名与文件名前缀（--namespace）。',
        },
        className: {
          type: 'string',
          description: 'Python 解析器类名（--class），缺省 Parser。',
        },
        prefix: {
          type: 'string',
          description: 'C99 公开符号前缀（--prefix）。',
        },
        cppLexer: {
          type: 'string',
          enum: ['std', 'dfa', 'vm'],
          description: 'C++17 词法引擎（--cpp-lexer）。',
        },
        defines: {
          type: 'array',
          items: { type: 'string' },
          description: '条件编译变量，形如 "DEBUG=1"；等价于重复传 --define。',
        },
        teach: {
          type: 'boolean',
          description: 'true 传 --teach，false 传 --no-teach；省略时不覆盖文法设置。',
        },
        mermaid: {
          type: 'boolean',
          description: 'true 生成 Mermaid 状态机图，false 关闭；省略时用 %mermaid。',
        },
        buildFiles: {
          type: 'boolean',
          description: '是否导出 Makefile/CMakeLists.txt/Cargo.toml 等构建文件。',
        },
        incremental: {
          type: 'boolean',
          description: 'true 复用持久化分析缓存（--incremental），false 关闭。',
        },
        cacheDir: {
          type: 'string',
          description: '指定增量缓存目录并启用缓存（--cache-dir）。',
        },
        rebuild: {
          type: 'boolean',
          description: '忽略旧缓存重新分析（--rebuild）。',
        },
        fullReport: {
          type: 'boolean',
          description: 'RESULT.md 里附上完整 FIRST/FOLLOW、ACTION/GOTO 报告（--full-report）。',
        },
        report: {
          type: 'string',
          description: '把文法分析报告另存为指定文件（--report）。',
        },
        syntaxOnly: {
          type: 'boolean',
          description: '预览生成：强制语法树并忽略语义动作/用户代码（--syntax-only）。',
        },
        strictRegex: {
          type: 'boolean',
          description: '拒绝未证明共享语义的原生正则（--strict-regex）。',
        },
        timeoutMs: {
          type: 'integer',
          description: '生成超时毫秒数，上限 900000。',
        },
      },
      required: ['path', 'outDir'],
    },
    output: {
      schema: {
        type: 'object',
        additionalProperties: false,
        properties: {
          grammar: { type: 'string' },
          outDir: { type: 'string' },
          compiler: { type: 'string' },
          ok: { type: 'boolean' },
          exitCode: { type: 'integer' },
          timedOut: { type: 'boolean' },
          artifacts: { type: 'array', items: { type: 'string' } },
          resultPath: { type: 'string' },
          reportPath: { type: 'string' },
          stdout: { type: 'string' },
          stderr: { type: 'string' },
        },
        required: ['grammar', 'outDir', 'compiler', 'ok', 'exitCode', 'artifacts'],
      },
      render: (_args, value) => [{ type: 'text', text: renderGenerateResult(value) }],
    },
    async execute(args, exec) {
      const cwd = await sessionCwd(ctx, exec, settings)
      const grammar = targetPath(args.path, cwd, settings)
      const outDir = targetPath(args.outDir, cwd, settings)
      const grammarInfo = await stat(grammar).catch(() => undefined)
      if (grammarInfo === undefined || !grammarInfo.isFile()) {
        throw new Error(`找不到文法文件: ${grammar}`)
      }
      await mkdir(outDir, { recursive: true })

      const compiler = await findCompiler(settings, cwd)
      if (compiler === undefined) throw new Error(compilerNotFoundMessage(cwd))

      const command = [grammar, '--out-dir', outDir, ...grammarFlags(args)]
      if (isText(args.namespace)) command.push('--namespace', args.namespace)
      if (isText(args.className)) command.push('--class', args.className)
      if (isText(args.prefix)) command.push('--prefix', args.prefix)
      if (isText(args.cppLexer)) command.push('--cpp-lexer', args.cppLexer)
      if (args.buildFiles === true) command.push('--build-files')
      if (args.buildFiles === false) command.push('--no-build-files')
      if (args.mermaid === true) command.push('--mermaid')
      if (args.mermaid === false) command.push('--no-mermaid')
      if (args.incremental === true) command.push('--incremental')
      if (args.incremental === false) command.push('--no-incremental')
      if (isText(args.cacheDir)) command.push('--cache-dir', targetPath(args.cacheDir, cwd, settings))
      if (args.rebuild === true) command.push('--rebuild')
      if (args.fullReport === true) command.push('--full-report')
      if (args.syntaxOnly === true) command.push('--syntax-only')

      let reportPath
      if (isText(args.report)) {
        reportPath = targetPath(args.report, cwd, settings)
        command.push('--report', reportPath)
      }

      const timeoutMs = clamp(
        positiveInteger(args.timeoutMs, settings.timeoutMs, MAX_TIMEOUT_MS),
        1_000,
        MAX_TIMEOUT_MS,
      )
      const result = await runProcess(compiler, command, {
        cwd,
        timeoutMs,
        signal: exec?.signal,
        maxOutputChars: settings.maxOutputChars,
      })
      const artifacts = await listArtifacts(outDir)
      const resultPath = (await isFile(join(outDir, 'RESULT.md'))) ? join(outDir, 'RESULT.md') : ''
      return {
        grammar,
        outDir,
        compiler,
        ok: result.exitCode === 0 && !result.timedOut,
        exitCode: result.exitCode,
        timedOut: result.timedOut,
        artifacts,
        resultPath,
        ...(reportPath === undefined ? {} : { reportPath }),
        stdout: result.stdout,
        stderr: result.stderr,
      }
    },
  }
}

function runTool(ctx, settings) {
  return {
    name: 'hareparse_run',
    description:
      '构建并运行 generate 产物目录里的演示程序：C/C++17 用本机编译器把产物编成 parser_demo 后运行，Python 运行 main.py，C# 用 dotnet run，Rust 用 cargo run，Go 用 go run，Java 用 javac+java，JS 用 node。输入文本写入子进程标准输入（演示程序默认只读一行，除非文法写了 %main）。Lepus 与 gram 档没有可运行演示程序，会明确说明。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        outDir: {
          type: 'string',
          description: 'hareparse_generate 的产物目录。',
        },
        language: {
          type: 'string',
          enum: LANGUAGES,
          description: '目标语言；省略时按产物文件自动识别。',
        },
        text: {
          type: 'string',
          description: '喂给演示程序标准输入的文本。与 inputFile 二选一；都不给时空输入。',
        },
        inputFile: {
          type: 'string',
          description: '从文件读输入（UTF-8），适合较大样本；text 优先。',
        },
        timeoutMs: {
          type: 'integer',
          description: '构建 + 运行的总超时毫秒数；缺省 300000，上限 900000。',
        },
      },
      required: ['outDir'],
    },
    output: {
      schema: {
        type: 'object',
        additionalProperties: false,
        properties: {
          language: { type: 'string' },
          compiler: { type: 'string' },
          ok: { type: 'boolean' },
          exitCode: { type: 'integer' },
          timedOut: { type: 'boolean' },
          stage: { type: 'string' },
          command: { type: 'string' },
          build: {
            type: 'object',
            additionalProperties: false,
            properties: {
              ok: { type: 'boolean' },
              exitCode: { type: 'integer' },
              timedOut: { type: 'boolean' },
              stdout: { type: 'string' },
              stderr: { type: 'string' },
            },
            required: ['ok', 'exitCode'],
          },
          stdout: { type: 'string' },
          stderr: { type: 'string' },
        },
        required: ['language', 'compiler', 'ok', 'exitCode', 'stage', 'command'],
      },
      render: (_args, value) => [{ type: 'text', text: renderRunResult(value) }],
    },
    async execute(args, exec) {
      const cwd = await sessionCwd(ctx, exec, settings)
      const outDir = targetPath(args.outDir, cwd, settings)
      const info = await stat(outDir).catch(() => undefined)
      if (info === undefined || !info.isDirectory()) throw new Error(`找不到产物目录: ${outDir}`)
      const compiler = await findCompiler(settings, cwd)
      if (compiler === undefined) throw new Error(compilerNotFoundMessage(cwd))

      const artifacts = await listArtifacts(outDir)
      const language = isText(args.language) ? args.language : detectLanguage(artifacts)
      if (language === undefined) {
        throw new Error(`无法从产物识别目标语言（${outDir}）；请显式传 language。`)
      }

      let input = ''
      if (typeof args.text === 'string') {
        input = args.text
      } else if (isText(args.inputFile)) {
        const inputPath = targetPath(args.inputFile, cwd, settings)
        input = await readFile(inputPath, 'utf8').catch(() => {
          throw new Error(`找不到输入文件: ${inputPath}`)
        })
      }

      const timeoutMs = clamp(
        positiveInteger(args.timeoutMs, DEFAULT_BUILD_TIMEOUT_MS, MAX_TIMEOUT_MS),
        1_000,
        MAX_TIMEOUT_MS,
      )
      const result = await runGenerated(language, outDir, input, {
        compiler,
        timeoutMs,
        signal: exec?.signal,
        settings,
      })
      return { language, compiler, ...result }
    },
  }
}

function parseTextTool(ctx, settings) {
  return {
    name: 'hareparse_parse_text',
    description:
      '用已生成的 Python 解析器解析整段文本（调用 run_text，不是演示入口的一行限制），返回状态码、错误行列与消息、语义值 repr、语法树文本和 %commit 片段。适合多行日志/DSL、回归样本和“改文法→重生成→喂样本”的收敛循环；outDir 必须是 --lang python 的产物目录。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        outDir: {
          type: 'string',
          description: 'Python 产物目录（含 parser.py / main.py）。',
        },
        text: {
          type: 'string',
          description: '要解析的完整文本。与 inputFile 二选一；都不给时按空输入解析。',
        },
        inputFile: {
          type: 'string',
          description: '从 UTF-8 文件读取输入；text 优先。',
        },
        className: {
          type: 'string',
          description: '解析器类名；省略时从 main.py 的 Parser = parser.X 自动识别，再缺省 Parser。',
        },
        timeoutMs: {
          type: 'integer',
          description: '超时毫秒数，上限 900000。',
        },
      },
      required: ['outDir'],
    },
    output: {
      schema: {
        type: 'object',
        additionalProperties: false,
        properties: {
          ok: { type: 'boolean' },
          status: { type: 'integer' },
          statusName: { type: 'string' },
          error: {
            type: 'object',
            additionalProperties: false,
            properties: {
              code: { type: 'integer' },
              line: { type: 'integer' },
              column: { type: 'integer' },
              message: { type: 'string' },
            },
            required: ['code', 'line', 'column', 'message'],
          },
          errors: {
            type: 'array',
            items: {
              type: 'object',
              additionalProperties: false,
              properties: {
                code: { type: 'integer' },
                line: { type: 'integer' },
                column: { type: 'integer' },
                message: { type: 'string' },
              },
              required: ['code', 'line', 'column', 'message'],
            },
          },
          result: { type: 'string' },
          tree: { type: 'string' },
          commits: {
            type: 'array',
            items: {
              type: 'object',
              additionalProperties: false,
              properties: {
                line: { type: 'integer' },
                type: { type: 'string' },
                text: { type: 'string' },
              },
              required: ['line', 'type', 'text'],
            },
          },
          exitCode: { type: 'integer' },
          timedOut: { type: 'boolean' },
          stdout: { type: 'string' },
          stderr: { type: 'string' },
        },
        required: ['ok', 'exitCode'],
      },
      render: (_args, value) => [{ type: 'text', text: renderParseResult(value) }],
    },
    async execute(args, exec) {
      const cwd = await sessionCwd(ctx, exec, settings)
      const outDir = targetPath(args.outDir, cwd, settings)
      const parserFile = join(outDir, 'parser.py')
      if (!(await isFile(parserFile))) {
        throw new Error(`不是 Python 产物目录（缺少 parser.py）: ${outDir}`)
      }

      let input = ''
      if (typeof args.text === 'string') {
        input = args.text
      } else if (isText(args.inputFile)) {
        const inputPath = targetPath(args.inputFile, cwd, settings)
        input = await readFile(inputPath, 'utf8').catch(() => {
          throw new Error(`找不到输入文件: ${inputPath}`)
        })
      }

      const className = isText(args.className)
        ? args.className.trim()
        : (await detectPythonClass(outDir)) ?? 'Parser'
      const python = await findPython(settings)
      if (python === undefined) throw new Error(pythonNotFoundMessage())

      const timeoutMs = clamp(
        positiveInteger(args.timeoutMs, settings.timeoutMs, MAX_TIMEOUT_MS),
        1_000,
        MAX_TIMEOUT_MS,
      )

      const tempDir = await mkdtemp(join(tmpdir(), 'hareparse-driver-'))
      try {
        const driver = join(tempDir, 'driver.py')
        await writeFile(driver, PYTHON_DRIVER, 'utf8')
        const result = await runProcess(python, ['-X', 'utf8', driver, outDir, className], {
          cwd,
          timeoutMs,
          signal: exec?.signal,
          input,
          maxOutputChars: settings.maxOutputChars,
        })
        const parsed = parseDriverOutput(result.stdout)
        if (parsed === undefined) {
          return {
            ok: false,
            exitCode: result.exitCode,
            timedOut: result.timedOut,
            stdout: result.stdout,
            stderr: result.stderr || '解析器驱动没有输出可解析的 JSON。',
          }
        }
        return {
          ...parsed,
          exitCode: result.exitCode,
          timedOut: result.timedOut,
          ...(result.stderr === '' ? {} : { stderr: result.stderr }),
        }
      } finally {
        await rm(tempDir, { recursive: true, force: true }).catch(() => {})
      }
    },
  }
}

function manTool(ctx, settings) {
  return {
    name: 'hareparse_man',
    description:
      '查询 HareParse 构建期内嵌手册（离线可用，不读仓库文件）。常用：留空列出全部条目；["directives"] 列文法指令；["modes"] 列 16 种模式；["languages"] 列 10 个目标；["examples"] 列示例文法；["05"]/["08"]/["12"]/["13"]/["14"]/["15"] 看具体章节；["%token"]/["lalr"]/["python"] 看单条；["--search","展望符"] 关键词搜索。加 json 输出 JSON。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        query: {
          type: 'array',
          items: { type: 'string' },
          description: '传给 "HareParse man" 的参数；留空等价于 "man"（列出全部条目）。',
        },
        json: {
          type: 'boolean',
          description: '加 --json，返回结构化手册数据。',
        },
        lang: {
          type: 'string',
          enum: ['zh', 'en', 'auto'],
          description: '手册正文语言（--lang），缺省自动。',
        },
      },
    },
    output: {
      schema: {
        type: 'object',
        additionalProperties: false,
        properties: {
          query: { type: 'string' },
          compiler: { type: 'string' },
          ok: { type: 'boolean' },
          exitCode: { type: 'integer' },
          timedOut: { type: 'boolean' },
          stdout: { type: 'string' },
          stderr: { type: 'string' },
        },
        required: ['query', 'compiler', 'ok', 'exitCode'],
      },
      render: (_args, value) => [{ type: 'text', text: renderManResult(value) }],
    },
    async execute(args, exec) {
      const cwd = await sessionCwd(ctx, exec, settings)
      const compiler = await findCompiler(settings, cwd)
      if (compiler === undefined) throw new Error(compilerNotFoundMessage(cwd))
      const query = Array.isArray(args.query) ? args.query.map(String) : []
      const command = ['man', ...query]
      if (args.json === true) command.push('--json')
      if (isText(args.lang)) command.push('--lang', args.lang)
      const result = await runProcess(compiler, command, {
        cwd,
        timeoutMs: clamp(settings.timeoutMs, 1_000, MAX_TIMEOUT_MS),
        signal: exec?.signal,
        maxOutputChars: settings.maxOutputChars,
      })
      return {
        query: query.join(' '),
        compiler,
        ok: result.exitCode === 0 && !result.timedOut,
        exitCode: result.exitCode,
        timedOut: result.timedOut,
        stdout: result.stdout,
        stderr: result.stderr,
      }
    },
  }
}

function dumpTool(ctx, settings) {
  return {
    name: 'hareparse_dump',
    description:
      '打印 HareParse 的调试视图：kind=lr-automaton（LR(0)/SLR/LALR/LR(1)/IELR 的项集与转移）、slr-tables（ACTION/GOTO 表）、mermaid（Mermaid 状态机）、sections（%epilogue/%main/%code 段与动作行号）、preprocess（%if 处理后的文本与行号映射）。在临时目录里运行，不污染工作区；输出按 maxOutputChars 截断。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        path: {
          type: 'string',
          description: '.gram 文件，相对会话工作目录或工作目录内的绝对路径。',
        },
        kind: {
          type: 'string',
          enum: Object.keys(DUMP_KINDS),
          description: '调试视图种类。lr-automaton / slr-tables 需要 LR 家族模式。',
        },
        mode: {
          type: 'string',
          enum: MODES,
          description: '生成模式；省略时用 %mode（再缺省 ielr）。',
        },
        lookahead: {
          type: 'integer',
          description: 'LR(k)/LL(k) 的 k。',
        },
        defines: {
          type: 'array',
          items: { type: 'string' },
          description: '条件编译变量，形如 "DEBUG=1"。',
        },
        timeoutMs: {
          type: 'integer',
          description: '超时毫秒数，上限 900000。',
        },
      },
      required: ['path', 'kind'],
    },
    output: {
      schema: {
        type: 'object',
        additionalProperties: false,
        properties: {
          kind: { type: 'string' },
          compiler: { type: 'string' },
          ok: { type: 'boolean' },
          exitCode: { type: 'integer' },
          timedOut: { type: 'boolean' },
          stdout: { type: 'string' },
          stderr: { type: 'string' },
        },
        required: ['kind', 'compiler', 'ok', 'exitCode'],
      },
      render: (_args, value) => [{ type: 'text', text: renderDumpResult(value) }],
    },
    async execute(args, exec) {
      const cwd = await sessionCwd(ctx, exec, settings)
      const grammar = targetPath(args.path, cwd, settings)
      if (!(await isFile(grammar))) throw new Error(`找不到文法文件: ${grammar}`)
      const compiler = await findCompiler(settings, cwd)
      if (compiler === undefined) throw new Error(compilerNotFoundMessage(cwd))

      const timeoutMs = clamp(
        positiveInteger(args.timeoutMs, settings.timeoutMs, MAX_TIMEOUT_MS),
        1_000,
        MAX_TIMEOUT_MS,
      )
      const tempDir = await mkdtemp(join(tmpdir(), 'hareparse-dump-'))
      try {
        const command = [
          grammar,
          '--out-dir',
          tempDir,
          DUMP_KINDS[args.kind],
          ...grammarFlags(args),
        ]
        const result = await runProcess(compiler, command, {
          cwd,
          timeoutMs,
          signal: exec?.signal,
          maxOutputChars: settings.maxOutputChars,
        })
        return {
          kind: args.kind,
          compiler,
          ok: result.exitCode === 0 && !result.timedOut,
          exitCode: result.exitCode,
          timedOut: result.timedOut,
          stdout: result.stdout,
          stderr: result.stderr,
        }
      } finally {
        await rm(tempDir, { recursive: true, force: true }).catch(() => {})
      }
    },
  }
}

function clearTool(ctx, settings) {
  return {
    name: 'hareparse_clear',
    description:
      '用 HareParse 的 clear 模式按 .hareparse-manifest 清单删除生成目录里的产物（不会误删用户放进目录的其他文件）。返回删除数量；目录不存在或没有清单时也是安全的空操作。',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        outDir: {
          type: 'string',
          description: '要清理的生成目录。',
        },
      },
      required: ['outDir'],
    },
    output: {
      schema: {
        type: 'object',
        additionalProperties: false,
        properties: {
          outDir: { type: 'string' },
          compiler: { type: 'string' },
          ok: { type: 'boolean' },
          exitCode: { type: 'integer' },
          removed: { type: 'integer' },
          stdout: { type: 'string' },
          stderr: { type: 'string' },
        },
        required: ['outDir', 'compiler', 'ok', 'exitCode', 'removed'],
      },
      render: (_args, value) => [{ type: 'text', text: renderClearResult(value) }],
    },
    async execute(args, exec) {
      const cwd = await sessionCwd(ctx, exec, settings)
      const outDir = targetPath(args.outDir, cwd, settings)
      const compiler = await findCompiler(settings, cwd)
      if (compiler === undefined) throw new Error(compilerNotFoundMessage(cwd))
      const result = await runProcess(compiler, ['clear', '--out-dir', outDir], {
        cwd,
        timeoutMs: clamp(settings.timeoutMs, 1_000, MAX_TIMEOUT_MS),
        signal: exec?.signal,
        maxOutputChars: settings.maxOutputChars,
      })
      const match = /共删除\s*(\d+)\s*个生成文件/.exec(result.stdout)
      return {
        outDir,
        compiler,
        ok: result.exitCode === 0 && !result.timedOut,
        exitCode: result.exitCode,
        removed: match === null ? 0 : Number.parseInt(match[1], 10),
        stdout: result.stdout,
        stderr: result.stderr,
      }
    },
  }
}

// ---------------------------------------------------------------------------
// Grammar / toolchain helpers
// ---------------------------------------------------------------------------

/**
 * Run `HareParse <file> --check` and normalize the result.
 * @param file - absolute grammar path.
 * @param cwd - session working directory.
 * @param settings - normalized row configuration.
 * @param signal - optional abort signal.
 * @param args - check-related tool arguments.
 * @returns check result with compiler, ok, exitCode, timedOut, stdout, stderr.
 */
async function runCheck(file, cwd, settings, signal, args) {
  const compiler = await findCompiler(settings, cwd)
  if (compiler === undefined) {
    return {
      compiler: undefined,
      ok: false,
      exitCode: -1,
      timedOut: false,
      stdout: '',
      stderr: compilerNotFoundMessage(cwd),
    }
  }
  const timeoutMs = clamp(
    positiveInteger(args.timeoutMs, settings.timeoutMs, MAX_TIMEOUT_MS),
    1_000,
    MAX_TIMEOUT_MS,
  )
  const result = await runProcess(compiler, [file, '--check', ...grammarFlags(args)], {
    cwd,
    timeoutMs,
    signal,
    maxOutputChars: settings.maxOutputChars,
  })
  return {
    compiler,
    ok: result.exitCode === 0 && !result.timedOut,
    exitCode: result.exitCode,
    timedOut: result.timedOut,
    stdout: result.stdout,
    stderr: result.stderr,
  }
}

/**
 * Translate shared grammar flags into CLI arguments.
 * @param args - tool arguments carrying mode/lang/lookahead/teach/defines/strictRegex.
 * @returns argument list.
 */
function grammarFlags(args) {
  const flags = []
  if (isText(args.mode)) flags.push('--mode', args.mode)
  if (isText(args.lang)) flags.push('--lang', args.lang)
  if (Number.isInteger(args.lookahead)) flags.push('--lookahead', String(args.lookahead))
  if (args.teach === true) flags.push('--teach')
  if (args.teach === false) flags.push('--no-teach')
  if (args.strictRegex === true) flags.push('--strict-regex')
  for (const define of normalizeDefines(args.defines)) flags.push('--define', define)
  return flags
}

function normalizeDefines(defines) {
  if (Array.isArray(defines)) {
    return defines.map((value) => String(value).trim()).filter((value) => value !== '')
  }
  if (defines !== null && typeof defines === 'object') {
    return Object.entries(defines)
      .filter(([, value]) => value !== undefined && value !== null)
      .map(([key, value]) => (String(value) === '' ? String(key) : `${key}=${value}`))
  }
  return []
}

async function findCompiler(settings, cwd) {
  const explicit = settings.compiler ?? process.env.HAREPARSE_BIN ?? process.env.HAREPARSE_COMPILER
  if (isText(explicit)) {
    const candidate = isAbsolute(explicit) ? explicit : resolve(cwd, explicit)
    if (await isFile(candidate)) return candidate
    throw new Error(`配置指定的 HareParse 不存在: ${candidate}`)
  }

  // The package carries the real generator under bin/; prefer it so the plugin
  // keeps working outside this repository and without a system HareParse.
  const bundled = bundledCompilers()
  for (const candidate of bundled) if (await isFile(candidate)) return candidate

  for (const root of ancestorRoots(cwd)) {
    for (const candidate of [
      join(root, 'build', 'bin', executableName('HareParse')),
      join(root, executableName('HareParse')),
    ]) {
      if (await isFile(candidate)) return candidate
    }
  }

  for (const candidate of [executableName('HareParse'), executableName('hareparse')]) {
    const found = await findExecutable(candidate)
    if (found !== undefined) return found
  }
  return undefined
}

function bundledCompilers() {
  const directory = join(PLUGIN_ROOT, 'bin')
  return [
    join(directory, executableName('HareParse')),
    join(directory, 'HareParse.exe'),
    join(directory, 'HareParse'),
  ]
}

function executableName(base) {
  return process.platform === 'win32' ? `${base}.exe` : base
}

function ancestorRoots(cwd) {
  const roots = []
  let current = resolve(cwd)
  for (let depth = 0; depth < 6; depth += 1) {
    roots.push(current)
    const parent = dirname(current)
    if (parent === current) break
    current = parent
  }
  return roots
}

function compilerNotFoundMessage(cwd) {
  return (
    '找不到 HareParse 原程序。在插件目录运行 "node scripts/package.mjs" 把生成器复制进 bin/，' +
    '或设置 HAREPARSE_BIN / 在插件行里配置 compiler 字段。' +
    `搜索起点: ${cwd}`
  )
}

async function findPython(settings) {
  const explicit = settings.python ?? process.env.HAREPARSE_PYTHON ?? process.env.PYTHON
  if (isText(explicit)) {
    const found = await findExecutable(explicit)
    if (found !== undefined) return found
    throw new Error(`配置指定的 Python 不存在: ${explicit}`)
  }
  for (const candidate of ['python', 'python3', 'py']) {
    const found = await findExecutable(candidate)
    if (found !== undefined) return found
  }
  return undefined
}

function pythonNotFoundMessage() {
  return (
    '找不到 Python 解释器。hareparse_parse_text 需要 Python 3（生成目录里的 parser.py）；' +
    '安装 Python 后把 python 放进 PATH，或在插件行里配置 python 字段。'
  )
}

async function findExecutable(name) {
  if (!isText(name)) return undefined
  const value = name.trim()
  if (isAbsolute(value) || value.includes('/') || value.includes('\\')) {
    return (await isFile(value)) ? value : undefined
  }
  const extensions =
    process.platform === 'win32'
      ? (process.env.PATHEXT ?? '.EXE;.CMD;.BAT;.COM')
          .split(';')
          .filter((extension) => extension !== '')
          .map((extension) => extension.toLowerCase())
      : ['']
  for (const directory of (process.env.PATH ?? '').split(delimiter)) {
    if (directory === '') continue
    const base = join(directory, value)
    if (await isFile(base)) return base
    for (const extension of extensions) {
      const candidate = `${base}${extension}`
      if (await isFile(candidate)) return candidate
    }
  }
  return undefined
}

async function firstExecutable(candidates) {
  for (const candidate of candidates) {
    const found = await findExecutable(candidate)
    if (found !== undefined) return found
  }
  return undefined
}

// ---------------------------------------------------------------------------
// Generated artifacts / runners
// ---------------------------------------------------------------------------

async function listArtifacts(outDir) {
  const manifest = join(outDir, '.hareparse-manifest')
  try {
    const text = await readFile(manifest, 'utf8')
    return text
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line !== '' && !line.startsWith('#'))
  } catch {
    // No manifest (hand-made directory or plain `gram` export): scan the tree.
  }
  try {
    return await scanArtifacts(outDir)
  } catch {
    return []
  }
}

async function scanArtifacts(outDir) {
  const found = []
  async function walk(directory) {
    const entries = await readdir(directory, { withFileTypes: true })
    for (const entry of entries) {
      if (entry.name === '.hareparse-manifest') continue
      const absolute = join(directory, entry.name)
      if (entry.isDirectory()) {
        await walk(absolute)
      } else if (entry.isFile()) {
        found.push(relative(outDir, absolute))
      }
    }
  }
  await walk(outDir)
  return found
}

function detectLanguage(artifacts) {
  const names = artifacts.map((file) => basename(file).toLowerCase())
  if (names.includes('parser.py') || names.includes('main.py')) return 'python'
  if (names.includes('cargo.toml') || names.includes('parser.rs')) return 'rust'
  if (names.includes('go.mod') || names.includes('parser.go')) return 'go'
  if (names.some((name) => name.endsWith('.csproj')) || names.includes('parser.cs')) return 'csharp'
  if (names.includes('parser.java')) return 'java'
  if (names.includes('parser.ts') || names.includes('parser.js')) return 'js'
  if (names.includes('parser.lep')) return 'lepus'
  // %namespace / %prefix 会改源文件名，所以 C 家族按扩展名识别。
  if (artifacts.some((file) => /\.(cpp|cc|cxx)$/i.test(file))) return 'cpp17'
  if (artifacts.some((file) => /\.c$/i.test(file))) return 'c'
  if (artifacts.some((file) => file.toLowerCase().endsWith('.gram'))) return 'gram'
  return undefined
}

async function detectPythonClass(outDir) {
  try {
    const text = await readFile(join(outDir, 'main.py'), 'utf8')
    const match = /^\s*Parser\s*=\s*parser\.([A-Za-z_][A-Za-z0-9_]*)\s*$/m.exec(text)
    return match === null ? undefined : match[1]
  } catch {
    return undefined
  }
}

/**
 * Build and run the generated demo for one target language.
 * @param language - detected or requested target language.
 * @param outDir - generated directory.
 * @param input - text written to the demo's stdin.
 * @param options - compiler path, timeout, abort signal and plugin settings.
 * @returns stage/command/exitCode/stdout/stderr (plus optional build result).
 */
async function runGenerated(language, outDir, input, options) {
  const { timeoutMs, signal, settings } = options
  const artifacts = await listArtifacts(outDir)
  const processOptions = {
    cwd: outDir,
    timeoutMs,
    signal,
    input,
    maxOutputChars: settings.maxOutputChars,
  }

  if (language === 'python') {
    const python = await findPython(settings)
    if (python === undefined) return notRunnable(language, pythonNotFoundMessage())
    if (!(await isFile(join(outDir, 'main.py')))) {
      return notRunnable(language, 'Python 产物目录里没有 main.py。')
    }
    const command = formatCommand(python, ['-X', 'utf8', 'main.py'])
    const result = await runProcess(python, ['-X', 'utf8', 'main.py'], processOptions)
    return { ...result, stage: 'run', command, ok: result.exitCode === 0 && !result.timedOut }
  }

  if (language === 'c' || language === 'cpp17') {
    const sources = artifacts.filter((file) => new RegExp(`\\.${language === 'c' ? 'c' : 'cpp'}$`, 'i').test(file))
    if (sources.length === 0) {
      return notRunnable(language, `产物清单里没有 .${language === 'c' ? 'c' : 'cpp'} 源文件。`)
    }
    const candidates =
      language === 'c'
        ? [process.env.CC, 'gcc', 'cc', 'clang']
        : [process.env.CXX, 'g++', 'c++', 'clang++']
    const compiler = await firstExecutable(candidates)
    if (compiler === undefined) {
      return notRunnable(
        language,
        `找不到 ${language === 'c' ? 'C' : 'C++'} 编译器（试过 ${candidates.filter(Boolean).join(' / ')}）。`,
      )
    }
    const executable = join(outDir, executableName('parser_demo'))
    const compileArgs = [
      language === 'c' ? '-std=c99' : '-std=c++17',
      '-O2',
      ...sources,
      '-lm',
      '-o',
      executable,
    ]
    const build = await runProcess(compiler, compileArgs, {
      cwd: outDir,
      timeoutMs,
      signal,
      maxOutputChars: settings.maxOutputChars,
    })
    const buildSummary = summarizeBuild(build)
    if (buildSummary.ok !== true) {
      return {
        stage: 'compile',
        command: formatCommand(compiler, compileArgs),
        ok: false,
        exitCode: build.exitCode,
        timedOut: build.timedOut,
        build: buildSummary,
        stdout: build.stdout,
        stderr: build.stderr,
      }
    }
    const run = await runProcess(executable, [], processOptions)
    return {
      stage: 'run',
      command: formatCommand(executable, []),
      ok: run.exitCode === 0 && !run.timedOut,
      exitCode: run.exitCode,
      timedOut: run.timedOut,
      build: buildSummary,
      stdout: run.stdout,
      stderr: run.stderr,
    }
  }

  if (language === 'csharp') {
    const dotnet = await findExecutable('dotnet')
    if (dotnet === undefined) return notRunnable(language, '找不到 dotnet。')
    const args = ['run', '--nologo']
    const result = await runProcess(dotnet, args, processOptions)
    return { ...result, stage: 'run', command: formatCommand(dotnet, args), ok: result.exitCode === 0 && !result.timedOut }
  }

  if (language === 'rust') {
    const cargo = await findExecutable('cargo')
    if (cargo === undefined) return notRunnable(language, '找不到 cargo。')
    const args = ['run', '--quiet']
    const result = await runProcess(cargo, args, processOptions)
    return { ...result, stage: 'run', command: formatCommand(cargo, args), ok: result.exitCode === 0 && !result.timedOut }
  }

  if (language === 'go') {
    const go = await findExecutable('go')
    if (go === undefined) return notRunnable(language, '找不到 go。')
    const args = ['run', '.']
    const result = await runProcess(go, args, processOptions)
    return { ...result, stage: 'run', command: formatCommand(go, args), ok: result.exitCode === 0 && !result.timedOut }
  }

  if (language === 'java') {
    const javaFile = artifacts.find((file) => file.toLowerCase().endsWith('.java'))
    if (javaFile === undefined) return notRunnable(language, '产物清单里没有 .java 文件。')
    const javac = await findExecutable('javac')
    const java = await findExecutable('java')
    if (javac === undefined || java === undefined) {
      return notRunnable(language, '找不到 javac / java。')
    }
    const className = basename(javaFile).replace(/\.java$/i, '')
    const compileArgs = ['-encoding', 'UTF-8', javaFile]
    const build = await runProcess(javac, compileArgs, {
      cwd: outDir,
      timeoutMs,
      signal,
      maxOutputChars: settings.maxOutputChars,
    })
    const buildSummary = summarizeBuild(build)
    if (buildSummary.ok !== true) {
      return {
        stage: 'compile',
        command: formatCommand(javac, compileArgs),
        ok: false,
        exitCode: build.exitCode,
        timedOut: build.timedOut,
        build: buildSummary,
        stdout: build.stdout,
        stderr: build.stderr,
      }
    }
    const runArgs = ['-cp', '.', className]
    const run = await runProcess(java, runArgs, processOptions)
    return {
      stage: 'run',
      command: formatCommand(java, runArgs),
      ok: run.exitCode === 0 && !run.timedOut,
      exitCode: run.exitCode,
      timedOut: run.timedOut,
      build: buildSummary,
      stdout: run.stdout,
      stderr: run.stderr,
    }
  }

  if (language === 'js') {
    const node = await findExecutable('node')
    if (node === undefined) return notRunnable(language, '找不到 node（JS 产物需要 Node.js 22+）。')
    const entry = artifacts.find((file) => /\.(ts|js)$/i.test(file))
    if (entry === undefined) return notRunnable(language, '产物清单里没有 .ts/.js 入口。')
    const args = [entry]
    const result = await runProcess(node, args, processOptions)
    return { ...result, stage: 'run', command: formatCommand(node, args), ok: result.exitCode === 0 && !result.timedOut }
  }

  if (language === 'lepus') {
    return notRunnable(
      language,
      'Lepus 产物是自包含的 parser.lep，需要 Lepus 工具链编译；本插件只负责生成文法、检查与产物。',
    )
  }

  return notRunnable(
    language,
    'gram 档只导出文法原文与分析报告，没有可运行的解析器。用 --lang python / c / cpp17 等生成可运行产物。',
  )
}

function summarizeBuild(build) {
  return {
    ok: build.exitCode === 0 && !build.timedOut,
    exitCode: build.exitCode,
    timedOut: build.timedOut,
    stdout: build.stdout,
    stderr: build.stderr,
  }
}

function notRunnable(language, message) {
  return {
    stage: 'none',
    command: '',
    ok: false,
    exitCode: -1,
    timedOut: false,
    stdout: '',
    stderr: `${language}: ${message}`,
  }
}

function formatCommand(command, args) {
  return [command, ...args].map((part) => (/\s/.test(part) ? JSON.stringify(part) : part)).join(' ')
}

function parseDriverOutput(stdout) {
  const lines = stdout
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line !== '')
  for (let index = lines.length - 1; index >= 0; index -= 1) {
    if (!lines[index].startsWith('{')) continue
    try {
      return JSON.parse(lines[index])
    } catch {
      // Keep looking: warnings may be printed before the JSON line.
    }
  }
  return undefined
}

// ---------------------------------------------------------------------------
// Paths / processes
// ---------------------------------------------------------------------------

async function sessionCwd(ctx, exec, settings) {
  if (exec?.agent !== undefined) {
    return await ctx.workingDirectory.ensure(exec.agent, exec.signal)
  }
  return settings.workspaceRoot ?? process.cwd()
}

function targetPath(raw, cwd, settings) {
  if (!isText(raw)) throw new Error('path 必须是非空字符串')
  const absolute = isAbsolute(raw) ? normalize(raw) : resolve(cwd, raw)
  if (!settings.allowOutsideWorkspace && !isInside(cwd, absolute)) {
    throw new Error(
      `路径必须位于会话工作目录内 (${cwd}): ${absolute}。` +
        '确需访问目录外的路径时，在插件行配置 allowOutsideWorkspace: true。',
    )
  }
  return absolute
}

function isInside(root, target) {
  const rel = relative(lower(root), lower(target))
  return rel === '' || (!rel.startsWith('..') && !isAbsolute(rel))
}

function lower(value) {
  return process.platform === 'win32' ? value.toLowerCase() : value
}

async function assertWritableFile(file) {
  try {
    const info = await lstat(file)
    if (info.isSymbolicLink()) throw new Error(`拒绝覆盖符号链接: ${file}`)
    if (info.isDirectory()) throw new Error(`目标是一个目录: ${file}`)
  } catch (error) {
    if (error?.code !== 'ENOENT') throw error
  }
}

async function isFile(file) {
  try {
    return (await stat(file)).isFile()
  } catch {
    return false
  }
}

function isText(value) {
  return typeof value === 'string' && value.trim() !== ''
}

function positiveInteger(value, fallback, max) {
  if (!Number.isInteger(value) || value <= 0) return fallback
  return Math.min(value, max)
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max)
}

/**
 * Spawn a child process, capture UTF-8 stdout/stderr, and enforce a timeout.
 * @param command - executable path or name.
 * @param args - argument list.
 * @param options - cwd, timeoutMs, signal, stdin text, env, maxOutputChars.
 * @returns exitCode/stdout/stderr/timedOut; spawn failures resolve with exitCode -1.
 */
async function runProcess(command, args, options) {
  return await new Promise((resolvePromise, rejectPromise) => {
    const child = spawn(command, args, {
      cwd: options.cwd,
      env: {
        ...process.env,
        PYTHONIOENCODING: 'utf-8',
        PYTHONUTF8: '1',
        DOTNET_CLI_TELEMETRY_OPTOUT: process.env.DOTNET_CLI_TELEMETRY_OPTOUT ?? '1',
        ...options.env,
      },
      windowsHide: true,
      stdio: ['pipe', 'pipe', 'pipe'],
      signal: options.signal,
    })

    let stdout = ''
    let stderr = ''
    let timedOut = false
    let settled = false

    const finish = (result) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      resolvePromise({
        ...result,
        stdout: clip(result.stdout, options.maxOutputChars),
        stderr: clip(result.stderr, options.maxOutputChars),
      })
    }
    const fail = (error) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      rejectPromise(error)
    }
    const timer = setTimeout(() => {
      timedOut = true
      killTree(child)
    }, options.timeoutMs)

    child.stdout?.setEncoding('utf8')
    child.stderr?.setEncoding('utf8')
    child.stdout?.on('data', (chunk) => {
      stdout += chunk
    })
    child.stderr?.on('data', (chunk) => {
      stderr += chunk
    })
    // A missing executable reports ENOENT through 'error'; surface it as a
    // normal failed result so callers can render a helpful message.
    child.on('error', (error) => {
      if (error?.code === 'ABORT_ERR') {
        fail(error)
        return
      }
      finish({ exitCode: -1, stdout, stderr: `${stderr}${String(error?.message ?? error)}`, timedOut })
    })
    child.on('close', (exitCode) => {
      finish({
        exitCode: Number.isInteger(exitCode) ? exitCode : -1,
        stdout,
        stderr,
        timedOut,
      })
    })

    child.stdin?.on('error', () => {})
    if (typeof options.input === 'string') child.stdin?.end(options.input)
    else child.stdin?.end()
  })
}

function killTree(child) {
  try {
    child.kill('SIGKILL')
  } catch {
    // The child may already be gone; taskkill below covers descendants.
  }
  if (process.platform === 'win32' && Number.isInteger(child.pid)) {
    try {
      const killer = spawn('taskkill', ['/pid', String(child.pid), '/T', '/F'], {
        windowsHide: true,
        stdio: 'ignore',
      })
      killer.on('error', () => {})
    } catch {
      // Best-effort cleanup only.
    }
  }
}

function clip(text, maxChars) {
  if (typeof text !== 'string' || text.length <= maxChars) return text
  const head = Math.floor(maxChars * 0.6)
  const tail = maxChars - head
  return `${text.slice(0, head)}\n...[truncated ${text.length - maxChars} characters]...\n${text.slice(-tail)}`
}

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------

function renderWriteResult(value) {
  const lines = [`Wrote ${value.path} (${value.bytes} bytes).`]
  if (value.checked) {
    lines.push(
      `Check: ${value.ok ? 'passed' : 'failed'} (exit ${value.exitCode}${value.timedOut ? ', timed out' : ''})`,
    )
  } else {
    lines.push('Check: skipped.')
  }
  if (value.compiler) lines.push(`Compiler: ${value.compiler}`)
  if (value.stdout) lines.push(`stdout:\n${value.stdout}`)
  if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
  return lines.join('\n')
}

function renderCheckResult(path, value) {
  const lines = [
    `check: ${value.ok ? 'passed' : 'failed'} (exit ${value.exitCode}${value.timedOut ? ', timed out' : ''})`,
    `Source: ${path}`,
  ]
  if (value.compiler) lines.push(`Compiler: ${value.compiler}`)
  if (value.stdout) lines.push(`stdout:\n${value.stdout}`)
  if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
  return lines.join('\n')
}

function renderGenerateResult(value) {
  const lines = [
    `generate: ${value.ok ? 'ok' : 'failed'} (exit ${value.exitCode}${value.timedOut ? ', timed out' : ''})`,
    `Grammar: ${value.grammar}`,
    `OutDir: ${value.outDir}`,
    `Artifacts (${value.artifacts.length}): ${value.artifacts.join(', ') || '(none)'}`,
  ]
  if (value.resultPath) lines.push(`Result: ${value.resultPath}`)
  if (value.reportPath) lines.push(`Report: ${value.reportPath}`)
  if (value.compiler) lines.push(`Compiler: ${value.compiler}`)
  if (value.stdout) lines.push(`stdout:\n${value.stdout}`)
  if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
  return lines.join('\n')
}

function renderRunResult(value) {
  const lines = [
    `run: ${value.ok ? 'ok' : 'failed'} (exit ${value.exitCode}${value.timedOut ? ', timed out' : ''})`,
    `Language: ${value.language}`,
    `Stage: ${value.stage}`,
  ]
  if (value.command) lines.push(`Command: ${value.command}`)
  if (value.build) {
    lines.push(
      `Build: ${value.build.ok ? 'ok' : 'failed'} (exit ${value.build.exitCode})`,
    )
    if (value.build.stdout) lines.push(`build stdout:\n${value.build.stdout}`)
    if (value.build.stderr) lines.push(`build stderr:\n${value.build.stderr}`)
  }
  if (value.stdout) lines.push(`stdout:\n${value.stdout}`)
  if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
  return lines.join('\n')
}

function renderParseResult(value) {
  if (value.status === undefined) {
    const lines = [`parse: failed (exit ${value.exitCode}${value.timedOut ? ', timed out' : ''})`]
    if (value.error) lines.push(`error: ${value.error.message}`)
    if (value.stdout) lines.push(`stdout:\n${value.stdout}`)
    if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
    return lines.join('\n')
  }
  const lines = [
    `parse: ${value.ok ? 'ok' : 'failed'} (status ${value.status} ${value.statusName}, exit ${value.exitCode})`,
  ]
  if (value.error) {
    lines.push(`error: line ${value.error.line} column ${value.error.column}: ${value.error.message}`)
  }
  if (Array.isArray(value.errors) && value.errors.length > 1) {
    lines.push(`errors (${value.errors.length}):`)
    for (const error of value.errors) {
      lines.push(`  line ${error.line} column ${error.column}: ${error.message}`)
    }
  }
  if (value.result !== undefined) lines.push(`result: ${value.result}`)
  if (Array.isArray(value.commits) && value.commits.length > 0) {
    lines.push(`commits (${value.commits.length}):`)
    for (const commit of value.commits) {
      lines.push(`  line ${commit.line} [${commit.type}] ${commit.text}`)
    }
  }
  if (value.tree) lines.push(`tree:\n${value.tree}`)
  if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
  return lines.join('\n')
}

function renderManResult(value) {
  const lines = [`man ${value.query} (exit ${value.exitCode})`.trim()]
  if (value.stdout) lines.push(value.stdout)
  if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
  return lines.join('\n')
}

function renderDumpResult(value) {
  const lines = [`dump ${value.kind} (exit ${value.exitCode}${value.timedOut ? ', timed out' : ''})`]
  if (value.stdout) lines.push(value.stdout)
  if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
  return lines.join('\n')
}

function renderClearResult(value) {
  const lines = [
    `clear: ${value.ok ? 'ok' : 'failed'} (exit ${value.exitCode}, removed ${value.removed})`,
    `OutDir: ${value.outDir}`,
  ]
  if (value.stdout) lines.push(`stdout:\n${value.stdout}`)
  if (value.stderr) lines.push(`stderr:\n${value.stderr}`)
  return lines.join('\n')
}
