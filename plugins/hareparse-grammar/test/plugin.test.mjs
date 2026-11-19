// Registration and toolchain smoke tests for hareparse-grammar.
//
// Run from the plugin directory: node --test test/plugin.test.mjs
// The toolchain cases call the bundled generator (bin/HareParse.exe), so they
// also prove the package is self-contained after `npm run package`. Cases that
// need Python are skipped when no interpreter is available.

import assert from 'node:assert/strict'
import { readFile, mkdtemp, rm, stat } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import test from 'node:test'
import { fileURLToPath, pathToFileURL } from 'node:url'

const pluginRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const plugin = await import(pathToFileURL(join(pluginRoot, 'lib', 'index.js')).href)

function fakeContext(workspaceRoot) {
  const skills = []
  const tools = []
  return {
    skills: { register: (definition) => skills.push(definition) },
    tools: { register: (definition) => tools.push(definition) },
    workingDirectory: { ensure: async () => workspaceRoot },
    skillsList: skills,
    toolsList: tools,
  }
}

function tool(ctx, name) {
  const found = ctx.toolsList.find((definition) => definition.name === name)
  assert.ok(found, `missing ${name}`)
  return found
}

async function makeWorkdir() {
  return await mkdtemp(join(tmpdir(), 'hareparse-plugin-test-'))
}

async function withWorkdir(run) {
  const workDir = await makeWorkdir()
  try {
    return await run(workDir)
  } finally {
    await rm(workDir, { recursive: true, force: true })
  }
}

async function hasPython() {
  const { spawnSync } = await import('node:child_process')
  for (const candidate of ['python', 'python3']) {
    const probe = spawnSync(candidate, ['-c', 'import sys; sys.exit(0)'], { stdio: 'ignore' })
    if (probe.status === 0) return true
  }
  return false
}

async function hasCCompiler() {
  const { spawnSync } = await import('node:child_process')
  for (const candidate of ['gcc', 'cc', 'clang']) {
    const probe = spawnSync(candidate, ['--version'], { stdio: 'ignore' })
    if (probe.status === 0) return true
  }
  return false
}

test('registers the skill and eight tools on the host layer', () => {
  const ctx = fakeContext(tmpdir())
  plugin.apply(ctx, { workspaceRoot: tmpdir() })

  assert.equal(plugin.name, 'hareparse-grammar')
  assert.deepEqual(plugin.inject, ['tools', 'skills', 'workingDirectory'])
  assert.equal(ctx.skillsList.length, 1)
  assert.equal(ctx.skillsList[0].name, 'hareparse-grammar')
  assert.match(ctx.skillsList[0].content, /hareparse_generate/)
  assert.deepEqual(
    ctx.toolsList.map((definition) => definition.name),
    [
      'hareparse_write_grammar',
      'hareparse_check',
      'hareparse_generate',
      'hareparse_run',
      'hareparse_parse_text',
      'hareparse_man',
      'hareparse_dump',
      'hareparse_clear',
    ],
  )
})

test('bundles the real generator and its metadata', async () => {
  const binary = join(pluginRoot, 'bin', process.platform === 'win32' ? 'HareParse.exe' : 'HareParse')
  const info = await stat(binary)
  assert.ok(info.size > 1024 * 1024, `generator too small: ${info.size}`)

  const metadata = JSON.parse(await readFile(join(pluginRoot, 'bin', 'bundle.json'), 'utf8'))
  assert.equal(metadata.file, `bin/${process.platform === 'win32' ? 'HareParse.exe' : 'HareParse'}`)
  assert.equal(metadata.bytes, info.size)
  assert.match(metadata.sha256, /^[0-9a-f]{64}$/)

  const template = await readFile(join(pluginRoot, 'assets', 'grammar-template.gram'), 'utf8')
  assert.match(template, /%tree/)
  assert.equal(await readFile(join(pluginRoot, 'bin', 'grammar-template.gram'), 'utf8'), template)
})

test('writes a grammar and checks it with the bundled generator', async () => {
  await withWorkdir(async (workDir) => {
    const ctx = fakeContext(workDir)
    plugin.apply(ctx, { workspaceRoot: workDir })
    const template = await readFile(join(pluginRoot, 'assets', 'grammar-template.gram'), 'utf8')

    const write = tool(ctx, 'hareparse_write_grammar')
    const result = await write.execute(
      { path: 'demo.gram', grammar: template },
      { agent: undefined, signal: undefined },
    )
    assert.equal(result.checked, true)
    assert.equal(result.ok, true, result.stderr)
    assert.equal(result.bytes, Buffer.byteLength(template))
    assert.equal(result.path, join(workDir, 'demo.gram'))
    assert.match(result.compiler, /HareParse(?:\.exe)?$/)
  })
})

test('checks a broken grammar and reports diagnostics', async () => {
  await withWorkdir(async (workDir) => {
    const ctx = fakeContext(workDir)
    plugin.apply(ctx, { workspaceRoot: workDir })
    const check = tool(ctx, 'hareparse_check')

    const result = await check.execute(
      { path: 'broken.gram', mode: 'lalr' },
      { agent: undefined, signal: undefined },
    ).catch((error) => error)

    // The file does not exist yet: the tool must fail before spawning.
    assert.ok(result instanceof Error)
    assert.match(result.message, /找不到文法文件/)

    const write = tool(ctx, 'hareparse_write_grammar')
    await write.execute(
      { path: 'broken.gram', grammar: '%token A "a"\nS := A;\n', check: false },
      { agent: undefined, signal: undefined },
    )
    const checked = await check.execute(
      { path: 'broken.gram', mode: 'lalr' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(checked.ok, false)
    assert.notEqual(checked.exitCode, 0)
    assert.match(checked.stderr, /错误/)
  })
})

test('generates a Python parser and parses text through the generated API', async (t) => {
  if (!(await hasPython())) {
    t.skip('python is not available')
    return
  }
  await withWorkdir(async (workDir) => {
    const ctx = fakeContext(workDir)
    plugin.apply(ctx, { workspaceRoot: workDir })
    const template = await readFile(join(pluginRoot, 'assets', 'grammar-template.gram'), 'utf8')
    await tool(ctx, 'hareparse_write_grammar').execute(
      { path: 'demo.gram', grammar: template },
      { agent: undefined, signal: undefined },
    )

    const generate = tool(ctx, 'hareparse_generate')
    const generated = await generate.execute(
      { path: 'demo.gram', outDir: 'out', lang: 'python' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(generated.ok, true, generated.stderr)
    assert.ok(generated.artifacts.includes('parser.py'))
    assert.ok(generated.artifacts.includes('main.py'))
    assert.equal(generated.resultPath, join(workDir, 'out', 'RESULT.md'))

    const parse = tool(ctx, 'hareparse_parse_text')
    const good = await parse.execute(
      { outDir: 'out', text: 'a=1,b=2' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(good.ok, true, good.stderr)
    assert.equal(good.status, 0)
    assert.match(good.tree, /pair/)

    const bad = await parse.execute(
      { outDir: 'out', text: 'a=' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(bad.ok, false)
    assert.ok(bad.error.line >= 1)
    assert.ok(bad.error.message.length > 0)
  })
})

test('runs the generated Python demo with stdin', async (t) => {
  if (!(await hasPython())) {
    t.skip('python is not available')
    return
  }
  await withWorkdir(async (workDir) => {
    const ctx = fakeContext(workDir)
    plugin.apply(ctx, { workspaceRoot: workDir })
    const template = await readFile(join(pluginRoot, 'assets', 'grammar-template.gram'), 'utf8')
    await tool(ctx, 'hareparse_write_grammar').execute(
      { path: 'demo.gram', grammar: template },
      { agent: undefined, signal: undefined },
    )
    await tool(ctx, 'hareparse_generate').execute(
      { path: 'demo.gram', outDir: 'out', lang: 'python' },
      { agent: undefined, signal: undefined },
    )
    const run = tool(ctx, 'hareparse_run')
    const result = await run.execute(
      { outDir: 'out', text: 'a=1' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(result.language, 'python')
    assert.equal(result.ok, true, result.stderr)
    assert.match(result.stdout, /解析成功/)
  })
})

test('runs a generated C demo when a C compiler is available', async (t) => {
  if (!(await hasCCompiler())) {
    t.skip('no C compiler is available')
    return
  }
  await withWorkdir(async (workDir) => {
    const ctx = fakeContext(workDir)
    plugin.apply(ctx, { workspaceRoot: workDir })
    const template = await readFile(join(pluginRoot, 'assets', 'grammar-template.gram'), 'utf8')
    await tool(ctx, 'hareparse_write_grammar').execute(
      { path: 'demo.gram', grammar: template },
      { agent: undefined, signal: undefined },
    )
    const generated = await tool(ctx, 'hareparse_generate').execute(
      { path: 'demo.gram', outDir: 'out-c', lang: 'c' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(generated.ok, true, generated.stderr)
    const result = await tool(ctx, 'hareparse_run').execute(
      { outDir: 'out-c', text: 'a=1,b=2' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(result.language, 'c')
    assert.equal(result.ok, true, result.stderr)
    assert.match(result.stdout, /解析成功/)
  })
})

test('queries the embedded manual', async () => {
  await withWorkdir(async (workDir) => {
    const ctx = fakeContext(workDir)
    plugin.apply(ctx, { workspaceRoot: workDir })
    const man = tool(ctx, 'hareparse_man')
    const result = await man.execute(
      { query: ['directives'] },
      { agent: undefined, signal: undefined },
    )
    assert.equal(result.ok, true, result.stderr)
    assert.match(result.stdout, /%token/)
    assert.match(result.stdout, /%mode/)
  })
})

test('dumps the LR automaton without polluting the workspace', async () => {
  await withWorkdir(async (workDir) => {
    const ctx = fakeContext(workDir)
    plugin.apply(ctx, { workspaceRoot: workDir })
    const template = await readFile(join(pluginRoot, 'assets', 'grammar-template.gram'), 'utf8')
    await tool(ctx, 'hareparse_write_grammar').execute(
      { path: 'demo.gram', grammar: template },
      { agent: undefined, signal: undefined },
    )
    const dump = tool(ctx, 'hareparse_dump')
    const result = await dump.execute(
      { path: 'demo.gram', kind: 'lr-automaton', mode: 'lalr' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(result.ok, true, result.stderr)
    assert.match(result.stdout, /I0:/)
  })
})

test('clears generated artifacts by manifest', async () => {
  await withWorkdir(async (workDir) => {
    const ctx = fakeContext(workDir)
    plugin.apply(ctx, { workspaceRoot: workDir })
    const template = await readFile(join(pluginRoot, 'assets', 'grammar-template.gram'), 'utf8')
    await tool(ctx, 'hareparse_write_grammar').execute(
      { path: 'demo.gram', grammar: template },
      { agent: undefined, signal: undefined },
    )
    await tool(ctx, 'hareparse_generate').execute(
      { path: 'demo.gram', outDir: 'out', lang: 'python' },
      { agent: undefined, signal: undefined },
    )
    const clear = tool(ctx, 'hareparse_clear')
    const result = await clear.execute(
      { outDir: 'out' },
      { agent: undefined, signal: undefined },
    )
    assert.equal(result.ok, true, result.stderr)
    assert.ok(result.removed >= 3, `removed ${result.removed}`)
    await assert.rejects(() => stat(join(workDir, 'out', 'parser.py')))
  })
})
