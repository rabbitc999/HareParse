"""P2/P3 整数编号与索引优化的 Python 行为回归。

用 P2 之前的基线生成器与当前生成器分别生成 Python 产物，比较
`run_text()` 状态码、语义结果、语法树结构与 GLR 结果个数；对小型
字母表还做 token 串穷举，确保拆表/索引改动没有改变语言本身。
"""
import importlib.util
import itertools
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "tmp/HareParse-p2-baseline.exe"
NEW = ROOT / "build/bin" / ("HareParse.exe" if os.name == "nt" else "HareParse")
PL0 = "x := 1 + 2 * 3 ."

# (文法, 模式, 固定输入列表, 穷举字母表, 穷举最大长度)
CASES = [
    ("tmp/ielr_split_py.gram", "ielr",
     ["acd", "bcd", "ace", "bce", "fjh", "gjh", "fji", "gji"],
     list("abcdefghij"), 3),
    ("example/glr_dangling_else.gram", "glr",
     ["if cond then other else other", "if cond then other"],
     ["if", "cond", "then", "other", "else"], 3),
    ("example/pl0.gram", "glr", [PL0], None, 0),
    ("example/pl0.gram", "earley", [PL0], None, 0),
    ("example/pl0.gram", "cyk", [PL0], None, 0),
    ("example/custom_tree.gram", "glr", ["a b c"], None, 0),
    ("example/glr_expr.gram", "glr", ["1+2*3"], None, 0),
]


def generate(exe, grammar, mode, out):
    run = subprocess.run(
        [str(exe), str(ROOT / grammar), "--mode", mode,
         "--lang", "python", "--out-dir", str(out)],
        cwd=ROOT, capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=180)
    return run.returncode


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


def behavior(parser, text):
    engine = parser()
    status = engine.run_text(text)
    tree = engine.parse_tree(text) if status == 0 else None
    extra = ()
    if hasattr(engine, "glr_result_count"):
        extra = (engine.glr_result_count(),
                 tuple(engine.glr_result(i) for i in range(
                     engine.glr_result_count())),
                 tuple(serialize(engine.glr_tree(i)) for i in range(
                     engine.glr_result_count())))
    return (status, engine.result, serialize(tree), extra)


def compare(base_parser, new_parser, text, label):
    expected = behavior(base_parser, text)
    actual = behavior(new_parser, text)
    assert actual == expected, (
        f"行为不一致 {label} 输入={text!r}\n"
        f"  基线={expected}\n  优化={actual}")


def main():
    if not BASE.exists():
        raise AssertionError(f"缺少 P2 基线生成器：{BASE}")
    checked = 0
    with tempfile.TemporaryDirectory(prefix="p23-ast-", dir=ROOT / "tmp") as folder:
        root = Path(folder)
        for index, (grammar, mode, inputs, alphabet, max_len) in enumerate(CASES):
            base_dir = root / f"base-{index}"
            new_dir = root / f"new-{index}"
            rb = generate(BASE, grammar, mode, base_dir)
            rn = generate(NEW, grammar, mode, new_dir)
            assert rb == rn, f"生成状态码不一致 {grammar}/{mode}: {rb} vs {rn}"
            if rb != 0:
                continue
            base_parser = load_parser(base_dir / "parser.py", f"b{index}")
            new_parser = load_parser(new_dir / "parser.py", f"n{index}")
            for text in inputs:
                compare(base_parser, new_parser, text, f"{grammar}/{mode}")
                checked += 1
            if alphabet:
                for length in range(1, max_len + 1):
                    for parts in itertools.product(alphabet, repeat=length):
                        compare(base_parser, new_parser, " ".join(parts),
                                f"{grammar}/{mode}[穷举]")
                        checked += 1
            print(f"OK {grammar}/{mode}", flush=True)
    print(f"P2/P3 整数编号：{checked} 项 Python 行为与语法树回归通过")


if __name__ == "__main__":
    main()
