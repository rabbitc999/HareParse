"""LL / 递归下降优化后的 Python 产物回归。

用优化前的生成器（tmp/HareParse-ll-baseline.exe）与当前生成器各生成一份
Python 产物，逐例比较 ``run_text`` 的状态码、语义值与 ``parse_tree`` 的形状。
基线生成器不存在时直接跳过，避免误报。
"""

import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / "tmp" / "HareParse-ll-baseline.exe"
NEW = ROOT / "build" / "bin" / "HareParse.exe"

# (文法, 模式, 输入列表, LL(k) 的 k)
CASES = [
    ("example/ll0_demo.gram", "ll0", ["a b"], None),
    ("example/pl0_ll1.gram", "ll1", ["const a = 1; var b; begin b := a end."], None),
    ("example/calc.gram", "ll1", ["1+2*3", "1+2+3*4"], None),
    ("example/llk_demo.gram", "ll1", ["a c"], None),
    ("example/llk_demo.gram", "llk", ["a b c", "a b d", "a c"], 2),
    ("example/llk_demo.gram", "llk", ["a b c", "a c"], 3),
    ("example/dd_expr.gram", "dd", ["1 + 2 + 3", "(1 + 2)", "1",
                                    "1 +", "(", "1 2"], None),
    ("tmp/dd_nullable_check.gram", "dd", ["a", "b", "c", "ab"], None),
    ("example/ll_follow_recovery.gram", "ll1", ["1+2;", "1+;"], None),
    ("example/c89_subset.gram", "llk", [], 2),
    ("example/pl0.gram", "lalrk", ["const a=1; var b; begin b:=a end."], 2),
    ("example/pl0.gram", "slrk", ["const a=1; var b; begin b:=a end."], 2),
    ("example/calc.gram", "lrk", ["1+2*3"], 2),
]


def generate(exe, grammar, mode, lookahead, out):
    """调用生成器；返回 (returncode, stderr)。"""
    cmd = [str(exe), str(ROOT / grammar), "--mode", mode,
           "--lang", "python", "--out-dir", str(out)]
    if lookahead is not None:
        cmd += ["--lookahead", str(lookahead)]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    return result.returncode, result.stderr.strip()


def load(path, name):
    """以独立模块名加载产物里的 parser.py。"""
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def tree_of(text, module):
    """把 parse_tree 序列化成 (节点名, 词素, 子树...) 元组。"""
    tree = module.Parser().parse_tree(text)

    def walk(node):
        if node is None:
            return None
        children = tuple(walk(node.child(i)) for i in range(node.child_count))
        return (node.name, node.text, children)

    return walk(tree)


def run_text(text, module):
    """跑一次解析，返回 (状态码, 语义值 repr)。"""
    parser = module.Parser()
    status = parser.run_text(text)
    return status, repr(parser.result)


def main():
    if not BASE.exists():
        print("跳过：基线生成器不存在，请保留 tmp/HareParse-ll-baseline.exe")
        return 0
    total = 0
    for index, (grammar, mode, inputs, lookahead) in enumerate(CASES):
        base_out = ROOT / "tmp" / f"ll_ast_base_{index}"
        new_out = ROOT / "tmp" / f"ll_ast_new_{index}"
        base_rc, base_err = generate(BASE, grammar, mode, lookahead, base_out)
        new_rc, new_err = generate(NEW, grammar, mode, lookahead, new_out)
        label = f"{grammar}/{mode}" + (f"(k={lookahead})" if lookahead else "")
        if base_rc != new_rc:
            raise AssertionError(f"生成结果不一致 {label}: {base_rc} vs {new_rc}\n"
                                 f"基线: {base_err}\n新: {new_err}")
        if base_rc != 0:
            print(f"OK {label} 双方均生成失败（一致）")
            total += 1
            continue
        base_mod = load(base_out / "parser.py", f"ll_base_{index}")
        new_mod = load(new_out / "parser.py", f"ll_new_{index}")
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
