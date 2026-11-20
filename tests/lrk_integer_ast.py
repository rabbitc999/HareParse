"""LR(k) 项集族整数编号后的 Python 产物回归。

用优化前的生成器（tmp/HareParse-ll-baseline.exe）与当前生成器各生成一份
Python 产物，逐例比较 ``run_text`` 的状态码、语义值与 ``parse_tree`` 的形状。
覆盖 lrk / lalrk / slrk 在 k=2、3 下的多份文法。基线不存在时直接跳过。
"""

import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / "tmp" / "HareParse-ll-baseline.exe"
NEW = ROOT / "build" / "bin" / "HareParse.exe"

PL0 = "const a=1; var b; begin b:=a end."
# (文法, 模式, 输入列表, 展望长度)
CASES = [
    ("example/pl0.gram", "lalrk", [PL0], 2),
    ("example/pl0.gram", "slrk", [PL0], 2),
    ("example/pl0.gram", "slrk", [PL0], 3),
    ("example/llk_demo.gram", "lrk", ["a b c", "a b d", "a c"], 2),
    ("example/llk_demo.gram", "lrk", ["a b c", "a b d", "a c"], 3),
    ("example/llk_demo.gram", "lalrk", ["a b c", "a b d", "a c"], 2),
    ("example/llk_demo.gram", "lalrk", ["a b c", "a b d", "a c"], 3),
    ("example/llk_demo.gram", "slrk", ["a b c", "a b d", "a c"], 2),
    ("example/llk_demo.gram", "slrk", ["a b c", "a b d", "a c"], 3),
    ("example/dd_expr.gram", "lrk", ["1 + 2 + 3", "(1 + 2)", "1", "1 +"], 2),
    ("example/dd_expr.gram", "lrk", ["1 + 2 + 3", "(1 + 2)", "1", "1 +"], 3),
    ("example/dd_expr.gram", "lalrk", ["1 + 2 + 3", "(1 + 2)", "1"], 2),
    ("example/dd_expr.gram", "lalrk", ["1 + 2 + 3", "(1 + 2)", "1"], 3),
    ("example/dd_expr.gram", "slrk", ["1 + 2 + 3", "(1 + 2)", "1"], 2),
    ("example/dd_expr.gram", "slrk", ["1 + 2 + 3", "(1 + 2)", "1"], 3),
    ("example/glr_dangling_else.gram", "lrk",
     ["if cond then other", "if cond then if cond then other else other"], 2),
    ("example/glr_dangling_else.gram", "lrk",
     ["if cond then other", "if cond then if cond then other else other"], 3),
    ("example/custom_tree.gram", "lrk", ["a + b * c", "(a + b) * c"], 2),
    ("example/custom_tree.gram", "lrk", ["a + b * c", "(a + b) * c"], 3),
    ("example/custom_tree.gram", "lalrk", ["a + b * c", "(a + b) * c"], 2),
    ("example/custom_tree.gram", "lalrk", ["a + b * c", "(a + b) * c"], 3),
    ("example/custom_tree.gram", "slrk", ["a + b * c", "(a + b) * c"], 2),
    ("example/custom_tree.gram", "slrk", ["a + b * c", "(a + b) * c"], 3),
]


def generate(exe, grammar, mode, lookahead, out):
    cmd = [str(exe), str(ROOT / grammar), "--mode", mode,
           "--lang", "python", "--out-dir", str(out),
           "--lookahead", str(lookahead)]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    return result.returncode, result.stderr.strip()


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def tree_of(text, module):
    tree = module.Parser().parse_tree(text)

    def walk(node):
        if node is None:
            return None
        children = tuple(walk(node.child(i)) for i in range(node.child_count))
        return (node.name, node.text, children)

    return walk(tree)


def run_text(text, module):
    parser = module.Parser()
    status = parser.run_text(text)
    return status, repr(parser.result)


def main():
    if not BASE.exists():
        print("跳过：基线生成器不存在，请保留 tmp/HareParse-ll-baseline.exe")
        return 0
    total = 0
    for index, (grammar, mode, inputs, k) in enumerate(CASES):
        base_out = ROOT / "tmp" / f"lrk_int_base_{index}"
        new_out = ROOT / "tmp" / f"lrk_int_new_{index}"
        base_rc, base_err = generate(BASE, grammar, mode, k, base_out)
        new_rc, new_err = generate(NEW, grammar, mode, k, new_out)
        label = f"{grammar}/{mode}(k={k})"
        if base_rc != new_rc:
            raise AssertionError(f"生成结果不一致 {label}: {base_rc} vs {new_rc}\n"
                                 f"基线: {base_err}\n新: {new_err}")
        if base_rc != 0:
            print(f"OK {label} 双方均生成失败（一致）")
            total += 1
            continue
        base_mod = load(base_out / "parser.py", f"lrk_base_{index}")
        new_mod = load(new_out / "parser.py", f"lrk_new_{index}")
        for text in inputs:
            expected = (run_text(text, base_mod), tree_of(text, base_mod))
            actual = (run_text(text, new_mod), tree_of(text, new_mod))
            if expected != actual:
                raise AssertionError(f"AST/语义值不一致 {label} 输入={text!r}\n"
                                     f"基线: {expected}\n新: {actual}")
            total += 1
        print(f"OK {label} {len(inputs)} 个输入")
    print(f"全部 {total} 项一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
