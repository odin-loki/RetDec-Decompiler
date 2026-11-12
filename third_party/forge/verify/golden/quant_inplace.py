#!/usr/bin/env python3
"""Quantised weights are read in place (matmul_t on encoded rows): logits must match the decode-everything path
(FORGE_DECODE_WEIGHTS=1) for every codec, on a tiny random Qwen3 model."""
import argparse, os, subprocess, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tiny_qwen3 as tq, tiny_llama as tl
# The in-place path is checked against exact f32 activations: the CPU integer matvec (default) also rounds the
# activations to int8, a second approximation on top of the weights. FORGE_CPU_INT8 set by the caller: kept.
os.environ.setdefault("FORGE_CPU_INT8", "0")

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--forge", required=True); ap.add_argument("--work", default="build/golden_quant_inplace")
    a = ap.parse_args()
    tq.CFG.update(hidden_size=256, intermediate_size=512, head_dim=64)  # k divisible by the 256-element k-quant blocks
    hf = os.path.join(a.work, "hf"); tq.make_model(hf)
    prompt = "290,72,101,108,108,111,44,32,116"
    fails = 0
    for codec in ("f32", "f16", "q8_0", "q4_k", "q8_g32"):
        fp = os.path.join(a.work, f"m_{codec}.fpk")
        tl.run([a.forge, "import", hf, "-o", fp, "--codec", codec])
        outs = {}
        for mode in ("0", "1"):
            env = dict(os.environ, FORGE_DECODE_WEIGHTS=mode); lf = fp + f".{mode}.f32"
            import subprocess
            ids = subprocess.run([a.forge, "run", fp, "--tokens", prompt, "-n", "8", "--ids", "--logits", lf], env=env,
                                 capture_output=True, text=True, check=True).stdout.split()
            outs[mode] = (np.fromfile(lf, dtype=np.float32), ids)
        (l0, i0), (l1, i1) = outs["0"], outs["1"]
        err = float(np.abs(l0 - l1).max()) / max(float(np.abs(l1).max()), 1e-9)
        ok = err < 1e-4 and i0 == i1
        print(f"[{'ok' if ok else 'FAIL'}] {codec}: in-place vs decoded logits rel err {err:.1e}, tokens {'equal' if i0 == i1 else 'DIFFER'}")
        fails += not ok
    # GGUF-only codecs: a qwen3 GGUF whose projection weights are random Q6_K (layer 0) and Q5_K (layer 1) blocks.
    try:
        import gguf
    except ImportError:
        gguf = None; print("[skip] gguf python package not installed")
    if gguf:
        W = tq.make_model(os.path.join(a.work, "hf_g"))
        gp = os.path.join(a.work, "mixed.gguf")
        rng = np.random.default_rng(5)
        def raw(shape, qt):
            T = getattr(gguf.GGMLQuantizationType, qt); bs, bb = gguf.GGML_QUANT_SIZES[T]
            q = rng.integers(0, 256, size=(shape[0], shape[1] // bs * bb), dtype=np.uint8); blk = q.reshape(-1, bb)
            f = lambda v: np.frombuffer(np.float16(v).tobytes(), np.uint8)
            if qt == "Q6_K": blk[:, 192:208] = rng.integers(-20, 20, size=(blk.shape[0], 16)).astype(np.int8).view(np.uint8); blk[:, 208:210] = f(2e-3)
            else: blk[:, 0:2] = f(2e-3); blk[:, 2:4] = f(1e-3)
            return q, T
        writer_add = gguf.GGUFWriter.add_tensor
        def add_tensor(self, name, t, raw_shape=None, raw_dtype=None, **kw):
            if raw_dtype is None and name.startswith("blk.") and t.ndim == 2 and "norm" not in name:
                q, T = raw(t.shape, "Q6_K" if name.startswith("blk.0.") else "Q5_K")
                return writer_add(self, name, q, raw_shape=q.shape, raw_dtype=T)
            return writer_add(self, name, t, raw_shape=raw_shape, raw_dtype=raw_dtype, **kw)
        gguf.GGUFWriter.add_tensor = add_tensor
        try: tq.write_gguf(gp, W, os.path.join(a.work, "hf_g"))
        finally: gguf.GGUFWriter.add_tensor = writer_add
        fp = os.path.join(a.work, "mixed_gguf.fpk"); tl.run([a.forge, "import", gp, "-o", fp])
        info = subprocess.run([a.forge, "inspect", fp, "--tensors"], capture_output=True, text=True).stdout
        have = all(c in info for c in ("q6_k", "q5_k"))
        res = {}
        for mode in ("0", "1"):
            lf = fp + f".{mode}.f32"
            ids = subprocess.run([a.forge, "run", fp, "--tokens", prompt, "-n", "8", "--ids", "--logits", lf],
                                 env=dict(os.environ, FORGE_DECODE_WEIGHTS=mode), capture_output=True, text=True, check=True).stdout.split()
            res[mode] = (np.fromfile(lf, dtype=np.float32), ids)
        (l0, i0), (l1, i1) = res["0"], res["1"]
        rel = float(np.abs(l0 - l1).max()) / max(float(np.abs(l1).max()), 1e-9)
        same = rel < 1e-4 and i0 == i1 and bool(np.isfinite(l0).all())
        ok = have and same
        print(f"[{'ok' if ok else 'FAIL'}] gguf Q6_K/Q5_K weights: in-place vs decoded rel err {rel:.1e}, tokens {'equal' if i0 == i1 else 'DIFFER'}"
              f"{'' if have else ' (q6_k/q5_k tensors missing from the import!)'}")
        fails += not ok
    print("quant in-place:", "PASS" if not fails else f"{fails} FAILURE(S)"); sys.exit(1 if fails else 0)

if __name__ == "__main__": main()
