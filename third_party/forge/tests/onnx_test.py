#!/usr/bin/env python3
"""ONNX import: build small graphs with the `onnx` package, import with `forge import`, run with
`forge run --input`, and compare against numpy. Also checks the error message for an unsupported op."""
import os, subprocess, sys, tempfile, math
import numpy as np
import onnx
from onnx import helper as H, TensorProto as TP, numpy_helper as NH
forge = sys.argv[1]
runner = os.environ.get("FORGE_RUNNER", "").split()
d = tempfile.mkdtemp()
rng = np.random.default_rng(7)
def run(*a, ok=True):
    r = subprocess.run(runner + [forge, *a], capture_output=True, text=True, errors="replace")
    if (r.returncode == 0) != ok: sys.exit(f"unexpected exit {r.returncode}: {a}\n{r.stdout}{r.stderr}")
    return r.stdout + r.stderr
def W(name, *shape): return NH.from_array(rng.standard_normal(shape).astype(np.float32) * 0.5, name)
def check(name, graph, feeds, ref, opset=17, tol=1e-4):
    m = H.make_model(graph, opset_imports=[H.make_opsetid("", opset)], producer_name="forge-test")
    onnx.checker.check_model(m)
    p = os.path.join(d, name + ".onnx"); onnx.save(m, p)
    out = os.path.join(d, name + ".fpk")
    run("import", p, "-o", out)
    args = []
    for k, v in feeds.items():
        f = os.path.join(d, f"{name}.{k}.bin"); v.astype("<f4").tofile(f); args += ["--input", f"{k}={f}"]
    run("run", out, *args, "--out", os.path.join(d, name))
    for i, r in enumerate(ref):
        oname = graph.output[i].name
        got = np.fromfile(os.path.join(d, f"{name}.out_{oname}.f32"), "<f4")
        err = float(np.max(np.abs(got - r.ravel()))) if got.size == r.size else math.inf
        print(f"{name}.{oname}: {got.size} values, max err {err:.2e}")
        if err > tol: sys.exit(f"{name}: mismatch on {oname}")

# 1. MLP classifier with a symbolic batch dim: Gemm(transB) Relu MatMul Add Mul(scalar) Softmax, plus Tanh/Sigmoid branch
w1, b1, w2, b2 = W("w1", 16, 8), W("b1", 16), W("w2", 16, 4), W("b2", 4)
half = NH.from_array(np.array(0.5, np.float32), "half")
g = H.make_graph([
    H.make_node("Gemm", ["x", "w1", "b1"], ["h"], transB=1),
    H.make_node("Relu", ["h"], ["hr"]),
    H.make_node("MatMul", ["hr", "w2"], ["z"]),
    H.make_node("Add", ["z", "b2"], ["z2"]),
    H.make_node("Mul", ["z2", "half"], ["z3"]),
    H.make_node("Softmax", ["z3"], ["probs"], axis=-1),
    H.make_node("Tanh", ["z2"], ["t"]), H.make_node("Sigmoid", ["t"], ["sg"]),
], "mlp", [H.make_tensor_value_info("x", TP.FLOAT, ["batch", 8])],
   [H.make_tensor_value_info("probs", TP.FLOAT, ["batch", 4]), H.make_tensor_value_info("sg", TP.FLOAT, ["batch", 4])],
   [w1, b1, w2, b2, half])
for batch in (3, 5):
    x = rng.standard_normal((batch, 8)).astype(np.float32)
    A = lambda t: NH.to_array(t)
    z2 = np.maximum(x @ A(w1).T + A(b1), 0) @ A(w2) + A(b2)
    e = np.exp(z2 * 0.5 - (z2 * 0.5).max(-1, keepdims=True)); probs = e / e.sum(-1, keepdims=True)
    check(f"mlp_b{batch}", g, {"x": x}, [probs, 1 / (1 + np.exp(-np.tanh(z2)))])

# 2. Encoder-style block: LayerNormalization Transpose Reshape, erf-GELU, ReduceMean, Sub, Div, Constant, Flatten, Sqrt
gam, bet = W("gamma", 8), W("beta", 8)
shape = NH.from_array(np.array([3, 16], np.int64), "shape")
nodes = [
    H.make_node("LayerNormalization", ["x", "gamma", "beta"], ["ln"], axis=-1, epsilon=1e-5),
    H.make_node("Transpose", ["ln"], ["tr"], perm=[1, 0, 2]),
    H.make_node("Reshape", ["tr", "shape"], ["r"]),
    H.make_node("Constant", [], ["rt2"], value=NH.from_array(np.array(math.sqrt(2), np.float32))),
    H.make_node("Div", ["r", "rt2"], ["u"]), H.make_node("Erf", ["u"], ["eu"]),
    H.make_node("Constant", [], ["one"], value=NH.from_array(np.array([1.0], np.float32))),
    H.make_node("Add", ["eu", "one"], ["eu1"]), H.make_node("Mul", ["r", "eu1"], ["g2"]),
    H.make_node("Constant", [], ["hf"], value=NH.from_array(np.array(0.5, np.float32))),
    H.make_node("Mul", ["g2", "hf"], ["gelu"]),
    H.make_node("ReduceMean", ["gelu"], ["mu"], axes=[-1], keepdims=1),
    H.make_node("Sub", ["gelu", "mu"], ["c"]),
    H.make_node("Mul", ["c", "c"], ["c2"]), H.make_node("Abs", ["c2"], ["ac"]), H.make_node("Sqrt", ["ac"], ["out"]),
    H.make_node("Flatten", ["out"], ["flat"], axis=0),
]
g = H.make_graph(nodes, "enc", [H.make_tensor_value_info("x", TP.FLOAT, [2, 3, 8])],
                 [H.make_tensor_value_info("flat", TP.FLOAT, [1, 48])], [gam, bet, shape])
x = rng.standard_normal((2, 3, 8)).astype(np.float32)
mu, var = x.mean(-1, keepdims=True), x.var(-1, keepdims=True)
ln = (x - mu) / np.sqrt(var + 1e-5) * NH.to_array(gam) + NH.to_array(bet)
r = ln.transpose(1, 0, 2).reshape(3, 16)
erf = np.vectorize(math.erf)
gl = 0.5 * r * (1 + erf(r / math.sqrt(2)))
check("encoder", g, {"x": x}, [np.abs(gl - gl.mean(-1, keepdims=True)).reshape(1, 48)], opset=17, tol=2e-4)

# 3. unsupported op: clear error naming the operator
g = H.make_graph([H.make_node("Conv", ["x", "w"], ["y"])], "conv", [H.make_tensor_value_info("x", TP.FLOAT, [1, 1, 4, 4])],
                 [H.make_tensor_value_info("y", TP.FLOAT, [1, 1, 3, 3])], [W("w", 1, 1, 2, 2)])
p = os.path.join(d, "conv.onnx"); onnx.save(H.make_model(g, opset_imports=[H.make_opsetid("", 17)]), p)
msg = run("import", p, "-o", os.path.join(d, "conv.fpk"), ok=False)
if "Conv" not in msg: sys.exit("unsupported-op error does not name the op: " + msg)
print("unsupported op rejected:", msg.strip().splitlines()[-1])
print("onnx import OK")
