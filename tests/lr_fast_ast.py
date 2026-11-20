"""LR 建表快速路径的 Python 产物回归：只校验语法树与语义值。"""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build/bin" / ("HareParse.exe" if os.name == "nt" else "HareParse")
PL0_TOKENS = ["const", "a", "=", "1", ";", "var", "b", ";",
              "begin", "b", ":=", "a", "end", "."]
SPLIT_TREE = ("s", "", (("A", "a", ()), ("x", "", (("C", "c", ()),)),
                       ("D", "d", ())))


def generate(grammar, mode, out):
    result = subprocess.run([str(EXE), str(grammar), "--mode", mode,
                             "--lang", "python", "--out-dir", str(out)],
                            cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if result.returncode:
        raise AssertionError(f"{grammar} / {mode}\n{result.stdout}{result.stderr}")


def load_parser(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module.Parser


def serialize(tree):
    if tree is None:
        return None
    return (tree.name, tree.text,
            tuple(serialize(tree.child(i)) for i in range(tree.child_count)))


def leaves(tree):
    if tree is None:
        return []
    if tree.child_count == 0:
        return [] if not tree.text else [tree.text]
    result = []
    for index in range(tree.child_count):
        result.extend(leaves(tree.child(index)))
    return result


def main():
    with tempfile.TemporaryDirectory(prefix="lr-fast-ast-", dir=ROOT / "tmp") as directory:
        root = Path(directory)
        split = root / "split.gram"
        split.write_text(
            '%tree\n%start s\n%token A "a"\n%token B "b"\n%token C "c"\n'
            '%token D "d"\n%token E "e"\n'
            's := A x D { $$ = $2*10+1; } | B y D { $$ = $2*10+1; }\n'
            '   | A y E { $$ = $2*10+2; } | B x E { $$ = $2*10+2; };\n'
            'x := C { $$ = 1; };\ny := C { $$ = 2; };\n', encoding="utf-8")

        for mode in ("lalr", "ielr", "lr1", "glr"):
            out = root / ("split-" + mode)
            generate(split, mode, out)
            parser = load_parser(out / "parser.py", f"split_{mode}")
            tree = parser().parse_tree("a c d")
            assert serialize(tree) == SPLIT_TREE, (mode, serialize(tree))
            engine = parser()
            assert engine.run_text("a c d") == 0 and engine.result == 11, mode
            engine = parser()
            status = engine.run_text("a c e")
            if mode == "lalr":
                # LALR 合并丢失上下文：这条输入本来就解析不了（基线行为）。
                assert status != 0, mode
            else:
                assert status == 0 and engine.result == 22, mode

        for mode in ("lalr", "ielr", "lr1"):
            out = root / ("pl0-" + mode)
            generate(ROOT / "example/pl0.gram", mode, out)
            parser = load_parser(out / "parser.py", f"pl0_{mode}")
            tree = parser().parse_tree("const a=1; var b; begin b:=a end.")
            assert tree is not None and tree.name == "program", mode
            assert leaves(tree) == PL0_TOKENS, (mode, leaves(tree))

    print("LR 快速建表：Python 语法树与语义值回归通过")


if __name__ == "__main__":
    main()
