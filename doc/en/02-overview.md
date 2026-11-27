# 02 Repository Documentation Overview

> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../../doc/README.md)。

> **Lepus's glorious history**: This language grew up in this repository in its early days — the generator, StackVM, standard library, glue and reflection were all polished together here. It has since been split into an independent repository at <https://github.com/rabbitc999/Lepus>; links to `lepus/` in this document now point to the corresponding documents in the new repository.
>
> **The RustView line**: It likewise grew up in this repository — the minimal Rust grammar, borrow analysis and zero-dependency SVG / HTML all took their final shape here. It has since been split into an independent repository at <https://github.com/rabbitc999/rust-view>; links to `rust-view/` in this document now point to the corresponding documents in the new repository.

This page describes where documentation across the HareParse repository lives. The repository's main entry is the [root README](../../README.md); the generator, Lepus, MetroLang and RustView each have their own project documentation.

## Independent Projects

| Project | Documentation | Content |
| --- | --- | --- |
| HareParse parser generator | [Usage entry](../../source/README.md) · [Topic document index](../README.md) | `.gram` grammar, analysis modes, eight target languages, output artifacts and implementation |
| Lepus | [Language entry](https://github.com/rabbitc999/Lepus/blob/master/README.md) · [EBNF grammar reference](https://github.com/rabbitc999/Lepus/blob/master/doc/14-grammar-ebnf.md) · [Topic documents](https://github.com/rabbitc999/Lepus/blob/master/doc/README.md) · Lepus exporter | Language syntax baseline, standard library, StackVM, glue and export |
| MetroLang | [Toolchain entry](https://github.com/rabbitc999/metro-lang/blob/master/README.md) · [Language reference](https://github.com/rabbitc999/metro-lang/blob/master/doc/01-metrolang-reference.md) · [JSON format](https://github.com/rabbitc999/metro-lang/blob/master/doc/02-json-format.md) · [Interactive viewer](https://github.com/rabbitc999/metro-lang/blob/master/map_view/README.md) | A metro network data language: grammar, compilation / decompilation, SVG maps and route queries (**independent repository**) |
| RustView | [Project entry](https://github.com/rabbitc999/rust-view/blob/master/README.md) · [Preview page and demo](https://github.com/rabbitc999/rust-view/blob/master/doc/preview.html) · [Grammar](https://github.com/rabbitc999/rust-view/blob/master/rust_subset.gram) | Borrow-semantics visualization for minimal Rust pseudocode: HareParse-generated frontend, scope stack + borrow state table analysis, hand-written SVG / HTML; **1.0 feature freeze** (**independent repository**) |

## Parser Generator Documentation

The generator's implementation and manual live in this directory alongside the rest of the repository's documents; the numbering is in the table below.

| No. | Document | Content |
| --- | --- | --- |
| 03 | [Architecture](../03-architecture.md) | Modules and data flow of the generator |
| 04 | [Build and Test](../04-build-and-test.md) | Generator build, template rebuild and regression |
| 05 | [Grammar](../05-grammar.md) | The `.gram` specification and validation |
| 06 | [Analysis](../06-analysis.md) | Algorithms such as LL, LR and IELR |
| 07 | [Code Generation](../07-codegen.md) | Templates and language backends |
| 08 | [Generated Code](../08-generated-code.md) | Artifact structure and APIs |
| 09 | [Example Grammars](../09-examples.md) | A guided tour of the `example/` grammars |
| 10 | [Project Examples](../10-project-examples.md) | A guided tour of the `project/` projects |
| 11 | [Grammar Usage Manual](../11-grammar-usage.md) | Directives and semantic actions |
| 12 | [Generation Modes](../12-modes.md) | Sixteen analysis and generation modes |
| 13 | [Target Languages and Output Artifacts](../13-languages-and-products.md) | Backend selection and artifact forms |
| 14 | [Support Matrix](../14-language-mode-support.md) | Current scope of languages, modes and incremental APIs |
| 15 | [Tri-language, Embedded Assets and Release](../15-i18n-and-packaging.md) | Display options, embedded manual and language server, Unicode console, standalone Python packaging scripts |
| 16 | [Extended Regular Expressions](../16-regex-extensions.md) | Lookahead, word boundaries, case folding and backreferences |
| 17 | [Unicode Regex Modes](../17-regex-unicode.md) | UTF-8 code points, Unicode properties and simple case folding |
| 18 | [Custom Error Messages](../18-custom-error-messages.md) | Error-message callbacks and context across target languages |
| 19 | [Regex Runtime Budgets](../19-regex-runtime-budgets.md) | Steps, working memory, cancellation and structured failure states |
| 20 | [Regex Compatibility and Check-Only Validation](../20-regex-compatibility.md) | Per-rule engines and validation that writes no output |
| 21 | [Incremental Binary Generation](../21-incremental-generation.md) | Analysis caches, invalidation and corrupt-cache recovery |
| 22 | [VS Code Workflow](../22-vscode-workflow.md) | Large-grammar sync, incremental generation and safe rename |
| 23 | [Lepus Backend](../23-lepus-backend.md) | How Lepus output is generated, its API and regressions |

## Other Projects and Tools

| Category | Documentation |
| --- | --- |
| Example projects | [project main entry](../../project/README.md) · [course lab experiments](../../project/lab_report_nju/README.md) |
| Standalone libraries | [library directory](../../library/); each library maintains its own `README.md` and technical reports |
| Editor | [VS Code extension](../../vscode/README.md) |
| Documentation maintenance | [unified conventions, templates and documentation generator](../../project/document/README.md) |
| Repository collaboration | [handover notes](../../AGENTS.md) · [grammar AI plugin](../../plugins/hareparse-grammar/README.md) |
| Cross-project narratives | [01 Max-Level Grammar](../01-gram-legend.md) |

When changing any project, defer to that project's source, tests and local documentation; for the unified scope of the parser's languages and modes, see the [support matrix](../14-language-mode-support.md).

## Custom Allocator Library

[`library/allocator`](../../library/allocator/README.md) provides a linked-list pool, a linear pool and C++17 container adapters, under the namespace `Memory`. Build artifacts live in `build/allocator/`.

## Lepus Binary Stream Parsing

`std::binary::BinaryReader` reads little/big-endian primitive numbers, fixed-length strings and arrays through C++ glue.

## Lepus JSON and TOML

The JSON/TOML parsing interfaces plug copies of `library/json` and `library/toml` into Lepus, providing parsing, nested reads, traversal, diagnostics and serialization.

