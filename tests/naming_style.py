"""验证 C99 / C++17 命名选项、兼容默认值及生成产物的编译运行。"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(*args, input_text=None):
    completed = subprocess.run(
        args, input=input_text, capture_output=True, text=True, encoding="utf-8",
        errors="replace", check=False,
    )
    if completed.returncode:
        raise AssertionError(f"{args}\n{completed.stdout}\n{completed.stderr}")
    return completed.stdout


def check(generator, root, grammar, lang, mode, expected, extra=(), header=None, input_text="1\n"):
    target = root / f"{lang}-{mode}-{'-'.join(extra[1:])}"
    run(generator, grammar, "--lang", lang, "--mode", mode,
        "--naming-style=snake_case", "--out-dir", target, *extra)
    is_cpp = lang == "cpp17"
    if header is None:
        stem = "lexer" if mode == "lex" else "parser"
        header = stem + (".hpp" if is_cpp else ".h")
    generated = (target / header).read_text(encoding="utf-8")
    for identifier in expected:
        assert identifier in generated, (target, identifier)
    if mode == "lex":
        source = target / ("lexer.cpp" if is_cpp else "lexer.c")
        assert "int tokenText = 0;" in source.read_text(encoding="utf-8")
    executable = target / "demo.exe"
    run("g++" if is_cpp else "gcc", "-std=c++17" if is_cpp else "-std=c99",
        *sorted(target.glob("*.cpp" if is_cpp else "*.c")), "-lm", "-o", executable)
    return run(executable, input_text="hello\n" if mode == "lex" else input_text)


def main():
    args = argparse.ArgumentParser(description=__doc__)
    repository = Path(__file__).resolve().parents[1]
    binary_name = "HareParse.exe" if os.name == "nt" else "HareParse"
    args.add_argument("generator", type=Path, nargs="?",
                      default=repository / "build" / "bin" / binary_name)
    generator = args.parse_args().generator.resolve()
    examples = repository / "example"
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        lexer = root / "lex.gram"
        lexer.write_text(
            '%mode lex\n%token HELLO "hello"\n'
            '%postlex\nint tokenText = 0; (void)tokenText;\n%endpostlex\n',
            encoding="utf-8",
        )
        for lang, header in [("c", "parser.h"), ("cpp17", "parser.hpp")]:
            default, legacy = root / f"{lang}-default", root / f"{lang}-legacy"
            command = [generator, examples / "calc.gram", "--lang", lang]
            run(*command, "--out-dir", default)
            run(*command, "--out-dir", legacy, "--naming-style", "legacy")
            for file in default.iterdir():
                if file.suffix in {".c", ".cpp", ".h", ".hpp"}:
                    assert file.read_bytes() == (legacy / file.name).read_bytes(), file
            original = (default / header).read_text(encoding="utf-8")
            assert ("Parser_RunText(" if lang == "c" else "runText(") in original

        parser = root / "parser.gram"
        parser.write_text("%start expr\nexpr := NUMBER { $$ = $1; };\n",
                          encoding="utf-8")
        directive = root / "directive.gram"
        grammar_body = parser.read_text(encoding="utf-8")
        for lang, header, modern, legacy_name in [
            ("c", "parser.h", "parser_run_text(", "Parser_RunText("),
            ("cpp17", "parser.hpp", "run_text(", "runText("),
        ]:
            target = root / f"{lang}-directive"
            command = [generator, directive, "--lang", lang, "--out-dir", target]
            for style, expected in [("snake_case", modern), ("legacy", legacy_name)]:
                directive.write_text(f"%naming_style {style}\n" + grammar_body,
                                     encoding="utf-8")
                run(*command)
                assert expected in (target / header).read_text(encoding="utf-8")
            run(*command, "--naming-style", "snake_case")
            assert modern in (target / header).read_text(encoding="utf-8")
            directive.write_text("%naming_style snake_case\n" + grammar_body,
                                 encoding="utf-8")
            run(*command, "--naming-style", "legacy")
            assert legacy_name in (target / header).read_text(encoding="utf-8")
        for invalid in ["%naming_style", "%naming_style unknown",
                        "%naming_style legacy extra",
                        "%naming_style legacy\n%naming_style snake_case"]:
            directive.write_text(invalid + "\n" + grammar_body, encoding="utf-8")
            result = subprocess.run([generator, directive, "--check"],
                                    capture_output=True, check=False)
            assert result.returncode and b"%naming_style" in result.stderr, invalid

        check(generator, root, parser, "c", "ielr",
              ["parser_run_text(", "parser_commit_list"])
        check(generator, root, parser, "c", "ielr",
              ["calc_api_run_text("], ["--prefix", "CalcApi"])
        check(generator, root, parser, "cpp17", "ielr",
              ["class Engine", "struct ParserLocation", "run_text("],
              ["--namespace", "CalcApi"], "CalcApi_parser.hpp")
        check(generator, root, examples / "glr_expr.gram", "cpp17", "glr",
              ["struct GlrIncrementalCache", "run_glr_incremental("])
        check(generator, root, examples / "dd_expr.gram", "c", "dd",
              ["parser_run_text("])
        check(generator, root, examples / "custom_tree.gram", "c", "lalr",
              ["parser_tree_child_count("], input_text="a+b\n")
        check(generator, root, examples / "custom_tree.gram", "cpp17", "lalr",
              ["class Engine", "parse_tree("], input_text="a+b\n")
        union = root / "union.gram"
        union.write_text(
            "%union { int tokenText; }\n%token <tokenText> NUMBER\n"
            "%type <tokenText> expr\n%start expr\nexpr := NUMBER { $$ = $1; };\n",
            encoding="utf-8",
        )
        check(generator, root, union, "cpp17", "ll1",
              ["union ParserValue", "int tokenText;", "run_text("])
        assert "HELLO" in check(generator, root, lexer, "c", "lex",
                                ["parser_next_token(", "parser_lexer_t"])
        assert "HELLO" in check(generator, root, lexer, "cpp17", "lex",
                                ["class Engine", "token_name("])
    print("C/C++ naming styles passed")


if __name__ == "__main__":
    main()
