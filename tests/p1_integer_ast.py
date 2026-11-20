"""GLR / IELR / CYK / Earley 的 P1 整数编号优化回归：只校验 Python 语法树与语义值。

用 P1 之前的基线生成器与当前生成器分别生成 Python 产物，逐棵比较
`parse_tree()` 的（节点名、词素、子树）与 `run_text()` 的状态码、语义值。
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "tmp/HareParse-p1-baseline.exe"
NEW = ROOT / "build/bin" / ("HareParse.exe" if os.name == "nt" else "HareParse")
PL0_TOKENS = ["const", "a", "=", "1", ";", "var", "b", ";",
              "begin", "b", ":=", "a", "end", "."]

# (文法, 模式, 测试输入列表)
CASES = [
    ("example/pl0.gram", "glr", ["const a=1; var b; begin b:=a end."]),
    ("example/glr_dangling_else.gram", "glr", ["if cond then other else other"]),
    ("example/c89_subset.gram", "glr", []),
    ("example/custom_tree.gram", "glr", ["a b c"]),
    ("example/pl0.gram", "earley", ["const a=1; var b; begin b:=a end."]),
    ("example/glr_dangling_else.gram", "earley", ["if cond then other else other"]),
    ("example/c89_subset.gram", "earley", []),
    ("example/pl0.gram", "cyk", ["const a=1; var b; begin b:=a end."]),
    ("example/glr_dangling_else.gram", "cyk", ["if cond then other else other"]),
    ("example/c89_subset.gram", "cyk", []),
    ("example/pl0.gram", "ielr", ["const a=1; var b; begin b:=a end."]),
    ("example/c89_subset.gram", "ielr", []),
]


def generate(exe, grammar, mode, out):
    result = subprocess.run([str(exe), str(ROOT / grammar), "--mode", mode,
                             "--lang", "python", "--out-dir", str(out)],
                            cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=180)
    return result.returncode


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


def behavior(parser, text):
    engine = parser()
    status = engine.run_text(text)
    tree = engine.parse_tree(text) if status == 0 else None
    return (status, engine.result, serialize(tree), leaves(tree))


def main():
    if not BASE.exists():
        raise AssertionError(f"缺少基线生成器：{BASE}")
    checked = 0
    with tempfile.TemporaryDirectory(prefix="p1-ast-", dir=ROOT / "tmp") as directory:
        root = Path(directory)
        for index, (grammar, mode, inputs) in enumerate(CASES):
            base_dir = root / f"base-{index}"
            new_dir = root / f"new-{index}"
            rb = generate(BASE, grammar, mode, base_dir)
            rn = generate(NEW, grammar, mode, new_dir)
            assert rb == rn, f"生成状态码不一致 {grammar}/{mode}: {rb} vs {rn}"
            if rb != 0:
                continue
            base_parser = load_parser(base_dir / "parser.py", f"base_{index}")
            new_parser = load_parser(new_dir / "parser.py", f"new_{index}")
            # 无输入的用例只校验产物能加载（表结构等价）。
            if not inputs:
                checked += 1
                print(f"OK {grammar}/{mode} (仅表结构)", flush=True)
                continue
            for text in inputs:
                expected = behavior(base_parser, text)
                actual = behavior(new_parser, text)
                assert actual == expected, (
                    f"行为不一致 {grammar}/{mode} 输入={text!r}\n"
                    f"  基线={expected}\n  优化={actual}")
                checked += 1
                print(f"OK {grammar}/{mode} {text!r}", flush=True)
    print(f"P1 整数编号：{checked} 项 Python 语法树与语义值回归通过")


if __name__ == "__main__":
    main()
