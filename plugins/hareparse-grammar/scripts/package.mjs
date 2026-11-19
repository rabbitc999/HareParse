#!/usr/bin/env node
// Prepare the installable plugin: regenerate the embedded skill and copy the
// real HareParse generator plus the starter grammar template into bin/, so
// the installed package carries its own toolchain. Run from the plugin
// directory:
//   node scripts/package.mjs
//
// Source search order: HAREPARSE_BIN / HAREPARSE_COMPILER -> repository
// build/bin/HareParse(.exe) -> repository root -> PATH. The binary is large;
// bin/compiler.json style metadata is written as bin/bundle.json with the size
// and SHA-256 of what was copied. The metadata is stable when the binary does
// not change, so repeated packaging does not dirty the working tree.

import { createHash } from 'node:crypto'
import { createReadStream } from 'node:fs'
import { chmod, copyFile, mkdir, readFile, stat, writeFile } from 'node:fs/promises'
import { delimiter, dirname, isAbsolute, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawnSync } from 'node:child_process'

const here = dirname(fileURLToPath(import.meta.url))
const pluginRoot = resolve(here, '..')
const repoRoot = resolve(pluginRoot, '..', '..')

const build = spawnSync(process.execPath, [join(here, 'build.mjs')], { stdio: 'inherit' })
if (build.status !== 0) process.exit(build.status ?? 1)

const source = await locateGenerator()
if (source === undefined) {
  console.error(
    'hareparse-grammar: 找不到 HareParse 原程序。先构建生成器（make -C source）或设置 HAREPARSE_BIN。',
  )
  process.exit(1)
}

const binDir = join(pluginRoot, 'bin')
await mkdir(binDir, { recursive: true })
const filename = process.platform === 'win32' ? 'HareParse.exe' : 'HareParse'
const destination = join(binDir, filename)

const sourceInfo = await stat(source)
const existing = await stat(destination).catch(() => undefined)
if (existing === undefined || existing.size !== sourceInfo.size || existing.mtimeMs < sourceInfo.mtimeMs) {
  await copyFile(source, destination)
  if (process.platform !== 'win32') await chmod(destination, 0o755)
  console.log(`copied ${source} -> ${destination} (${formatBytes(sourceInfo.size)})`)
} else {
  console.log(`kept ${destination} (${formatBytes(existing.size)})`)
}

await copyFile(
  join(pluginRoot, 'assets', 'grammar-template.gram'),
  join(binDir, 'grammar-template.gram'),
)

const destinationInfo = await stat(destination)
const sha256 = await hashFile(destination)
const previous = await readBundleMetadata()
const unchanged =
  previous !== undefined &&
  previous.file === `bin/${filename}` &&
  previous.bytes === destinationInfo.size &&
  previous.sha256 === sha256

const metadata = {
  name: 'HareParse',
  file: `bin/${filename}`,
  source: isAbsolute(source) && source.startsWith(repoRoot)
    ? relative(repoRoot, source).replaceAll('\\', '/')
    : source,
  bytes: destinationInfo.size,
  sha256,
  builtAt: sourceInfo.mtime.toISOString(),
  packagedAt: unchanged ? previous.packagedAt : new Date().toISOString(),
}
await writeFile(join(binDir, 'bundle.json'), `${JSON.stringify(metadata, undefined, 2)}\n`, 'utf8')
console.log(
  unchanged
    ? `bundle metadata unchanged (HareParse ${formatBytes(metadata.bytes)}, sha256 ${sha256})`
    : `bundled HareParse ${formatBytes(metadata.bytes)} (sha256 ${sha256})`,
)

async function locateGenerator() {
  for (const name of ['HAREPARSE_BIN', 'HAREPARSE_COMPILER']) {
    const value = process.env[name]
    if (typeof value === 'string' && value.trim() !== '') {
      const candidate = isAbsolute(value) ? value : resolve(process.cwd(), value)
      if (await isFile(candidate)) return candidate
      throw new Error(`${name} 指向的文件不存在: ${candidate}`)
    }
  }

  const direct = [
    join(repoRoot, 'build', 'bin', process.platform === 'win32' ? 'HareParse.exe' : 'HareParse'),
    join(repoRoot, process.platform === 'win32' ? 'HareParse.exe' : 'HareParse'),
  ]
  for (const candidate of direct) if (await isFile(candidate)) return candidate

  const names = process.platform === 'win32' ? ['HareParse.exe'] : ['HareParse']
  for (const directory of (process.env.PATH ?? '').split(delimiter)) {
    if (directory === '') continue
    for (const name of names) {
      const candidate = join(directory, name)
      if (await isFile(candidate)) return candidate
    }
  }
  return undefined
}

async function isFile(file) {
  try {
    return (await stat(file)).isFile()
  } catch {
    return false
  }
}

async function hashFile(file) {
  const hash = createHash('sha256')
  for await (const chunk of createReadStream(file, { highWaterMark: 1024 * 1024 })) hash.update(chunk)
  return hash.digest('hex')
}

async function readBundleMetadata() {
  try {
    return JSON.parse(await readFile(join(binDir, 'bundle.json'), 'utf8'))
  } catch {
    return undefined
  }
}

function formatBytes(bytes) {
  return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`
}
