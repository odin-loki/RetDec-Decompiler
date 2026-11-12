#!/usr/bin/env python3
"""Golden end-to-end test for the qwen3 template: a random tiny Qwen3 (QK-norm) in HF and GGUF form,
a float64 numpy reference, and FORGE.

  1. HF import (f32, q8_0) -> prefill logits and greedy decode against the reference
  2. GGUF import (arch qwen3, F32) -> prefill logits
  3. Q6_K and Q5_K probe tensors: FORGE's block decoders against gguf-py, bit for bit
"""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tiny_llama as tl

CFG = dict(architectures=["Qwen3ForCausalLM"], model_type="qwen3", hidden_size=64, num_hidden_layers=2,
           num_attention_heads=4, num_key_value_heads=2, head_dim=32, intermediate_size=128, vocab_size=300,
           rms_norm_eps=1e-6, rope_theta=1000000.0, max_position_embeddings=128, bos_token_id=290,
           eos_token_id=291, tie_word_embeddings=True)

def make_model(d, seed=4321):
    rng = np.random.default_rng(seed)
    H, L, nh, nk, hd, F, V = (CFG[k] for k in ("hidden_size", "num_hidden_layers", "num_attention_heads",
                                                "num_key_value_heads", "head_dim", "intermediate_size", "vocab_size"))
    W = {"model.embed_tokens.weight": rng.normal(0, 1.0, (V, H)), "model.norm.weight": 1 + 0.1 * rng.normal(size=H)}
    for l in range(L):
        p = f"model.layers.{l}."
        W[p + "input_layernorm.weight"] = 1 + 0.1 * rng.normal(size=H)
        W[p + "post_attention_layernorm.weight"] = 1 + 0.1 * rng.normal(size=H)
        W[p + "self_attn.q_proj.weight"] = rng.normal(0, 1 / np.sqrt(H), (nh * hd, H))
        W[p + "self_attn.k_proj.weight"] = rng.normal(0, 1 / np.sqrt(H), (nk * hd, H))
        W[p + "self_attn.v_proj.weight"] = rng.normal(0, 1 / np.sqrt(H), (nk * hd, H))
        W[p + "self_attn.o_proj.weight"] = rng.normal(0, 1 / np.sqrt(nh * hd), (H, nh * hd))
        W[p + "self_attn.q_norm.weight"] = 1 + 0.3 * rng.normal(size=hd)
        W[p + "self_attn.k_norm.weight"] = 1 + 0.3 * rng.normal(size=hd)
        W[p + "mlp.gate_proj.weight"] = rng.normal(0, 1 / np.sqrt(H), (F, H))
        W[p + "mlp.up_proj.weight"] = rng.normal(0, 1 / np.sqrt(H), (F, H))
        W[p + "mlp.down_proj.weight"] = rng.normal(0, 1 / np.sqrt(F), (H, F))
    W = {k: v.astype(np.float32) for k, v in W.items()}
    saved = dict(tl.CFG); tl.CFG.update(CFG)
    try: tl.make_model(d)  # tokenizer.json (and a llama model we overwrite)
    finally: tl.CFG.clear(); tl.CFG.update(saved)
    from safetensors.numpy import save_file
    save_file(W, os.path.join(d, "model.safetensors"))  # tied embeddings: no lm_head.weight
    json.dump(CFG, open(os.path.join(d, "config.json"), "w"), indent=1)
    return {k: v.astype(np.float64) for k, v in W.items()}

def forward(W, ids):
    nh, nk, hd, eps = CFG["num_attention_heads"], CFG["num_key_value_heads"], CFG["head_dim"], CFG["rms_norm_eps"]
    s = len(ids); pos = np.arange(s, dtype=np.float64); rms = tl.rms
    x = W["model.embed_tokens.weight"][ids]
    for l in range(CFG["num_hidden_layers"]):
        p = f"model.layers.{l}."
        h = rms(x, W[p + "input_layernorm.weight"], eps)
        q = rms((h @ W[p + "self_attn.q_proj.weight"].T).reshape(s, nh, hd), W[p + "self_attn.q_norm.weight"], eps)
        k = rms((h @ W[p + "self_attn.k_proj.weight"].T).reshape(s, nk, hd), W[p + "self_attn.k_norm.weight"], eps)
        q = tl.rope(q, pos, CFG["rope_theta"]); k = tl.rope(k, pos, CFG["rope_theta"])
        v = (h @ W[p + "self_attn.v_proj.weight"].T).reshape(s, nk, hd)
        g = nh // nk; k = np.repeat(k, g, axis=1); v = np.repeat(v, g, axis=1)
        sc = np.einsum("shd,thd->hst", q, k) / np.sqrt(hd) + np.triu(np.full((s, s), -np.inf), 1)[None]
        sc = np.exp(sc - sc.max(-1, keepdims=True)); sc /= sc.sum(-1, keepdims=True)
        x = x + np.einsum("hst,thd->shd", sc, v).reshape(s, nh * hd) @ W[p + "self_attn.o_proj.weight"].T
        h = rms(x, W[p + "post_attention_layernorm.weight"], eps)
        gt = h @ W[p + "mlp.gate_proj.weight"].T
        x = x + ((gt / (1 + np.exp(-gt))) * (h @ W[p + "mlp.up_proj.weight"].T)) @ W[p + "mlp.down_proj.weight"].T
    return tl.rms(x, W["model.norm.weight"], eps) @ W["model.embed_tokens.weight"].T

def write_gguf(path, W, hf_dir):
    import gguf
    T = gguf.GGMLQuantizationType
    wr = gguf.GGUFWriter(path, "qwen3")
    wr.add_context_length(CFG["max_position_embeddings"]); wr.add_embedding_length(CFG["hidden_size"])
    wr.add_block_count(CFG["num_hidden_layers"]); wr.add_feed_forward_length(CFG["intermediate_size"])
    wr.add_head_count(CFG["num_attention_heads"]); wr.add_head_count_kv(CFG["num_key_value_heads"])
    wr.add_key_length(CFG["head_dim"]); wr.add_value_length(CFG["head_dim"])
    wr.add_layer_norm_rms_eps(CFG["rms_norm_eps"]); wr.add_rope_freq_base(CFG["rope_theta"])
    tok = json.load(open(os.path.join(hf_dir, "tokenizer.json")))
    inv = {i: t for t, i in tok["model"]["vocab"].items()}
    for a in tok["added_tokens"]: inv[a["id"]] = a["content"]
    toks = [inv.get(i, f"<unused{i}>") for i in range(CFG["vocab_size"])]
    wr.add_tokenizer_model("gpt2"); wr.add_token_list(toks); wr.add_token_types([3 if t in ("<s>", "</s>") else 1 for t in toks])
    wr.add_token_merges(tok["model"]["merges"]); wr.add_bos_token_id(CFG["bos_token_id"]); wr.add_eos_token_id(CFG["eos_token_id"])
    names = {"model.embed_tokens.weight": "token_embd.weight", "model.norm.weight": "output_norm.weight"}
    hf2g = {"input_layernorm": "attn_norm", "post_attention_layernorm": "ffn_norm", "self_attn.q_proj": "attn_q", "self_attn.k_proj": "attn_k",
            "self_attn.v_proj": "attn_v", "self_attn.o_proj": "attn_output", "self_attn.q_norm": "attn_q_norm", "self_attn.k_norm": "attn_k_norm",
            "mlp.gate_proj": "ffn_gate", "mlp.up_proj": "ffn_up", "mlp.down_proj": "ffn_down"}
    for k, v in W.items():  # qwen3 GGUFs keep HF's Q/K order (NeoX RoPE): no permute
        g = names.get(k) or f"blk.{k.split('.')[2]}.{hf2g['.'.join(k.split('.')[3:-1])]}.weight"
        wr.add_tensor(g, v.astype(np.float32))
    probes = {}
    for qt, dofs in (("Q6_K", 208), ("Q5_K", 0)):  # random blocks with sane f16 scales
        bs, bb = gguf.GGML_QUANT_SIZES[getattr(T, qt)]
        q = np.random.default_rng(len(qt) + dofs).integers(0, 256, size=(4, 512 // bs * bb), dtype=np.uint8)
        blk = q.reshape(-1, bb); blk[:, dofs:dofs + 2] = np.frombuffer(np.float16(0.01).tobytes(), np.uint8)
        if qt == "Q5_K": blk[:, 2:4] = np.frombuffer(np.float16(0.002).tobytes(), np.uint8)
        wr.add_tensor(f"probe_{qt}", q, raw_shape=q.shape, raw_dtype=getattr(T, qt))
        probes[qt] = gguf.quants.dequantize(q, getattr(T, qt)).reshape(-1)
    wr.write_header_to_file(); wr.write_kv_data_to_file(); wr.write_tensors_to_file(); wr.close()
    return probes

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--forge", required=True); ap.add_argument("--work", default="build/golden_qwen3")
    a = ap.parse_args(); run = tl.run
    hf = os.path.join(a.work, "hf"); W = make_model(hf)
    fails = 0
    prompt = [290, 72, 101, 108, 108, 111, 44, 32, 116, 104, 101, 32, 119]
    ref = forward(W, prompt)[-1]
    def check(fp, label, tol=None, cosmin=None, gen_n=1):
        nonlocal fails
        lf = fp + ".logits.f32"
        gen = run([a.forge, "run", fp, "--tokens", ",".join(map(str, prompt)), "-n", str(gen_n), "--ids", "--logits", lf])
        got = np.fromfile(lf, dtype=np.float32)
        err = np.abs(got - ref).max(); cos = got @ ref / np.linalg.norm(got) / np.linalg.norm(ref)
        ok = err < tol if tol else cos > cosmin
        print(f"[{'ok' if ok else 'FAIL'}] {label}: prefill logits max|err| {err:.2e}, cosine {cos:.6f}"); fails += not ok
        return gen
    for codec, tol, cosmin in (("f32", 2e-3, None), ("q8_0", None, 0.999)):
        out = os.path.join(a.work, f"qwen3_{codec}.fpk")
        run([a.forge, "import", hf, "-o", out, "--codec", codec])
        gen = check(out, f"hf {codec}", tol, cosmin, 20 if codec == "f32" else 1)
        if codec != "f32": continue
        ids = list(prompt); ref_gen = []
        for _ in range(20):
            t = int(np.argmax(forward(W, ids)[-1])); ref_gen.append(t); ids.append(t)
            if t == CFG["eos_token_id"]: break
        fg = [int(t) for t in gen.split()]; same = fg == ref_gen
        if not same:
            i = next(i for i in range(min(len(fg), len(ref_gen))) if fg[i] != ref_gen[i])
            lg = np.sort(forward(W, prompt + ref_gen[:i])[-1]); same = lg[-1] - lg[-2] < 1e-3
        print(f"[{'ok' if same else 'FAIL'}] greedy decode matches reference: forge {fg[:8]}... ref {ref_gen[:8]}..."); fails += not same
    try:
        import gguf  # noqa: F401
    except ImportError:
        print("[skip] gguf python package not installed"); gguf = None
    if gguf:
        gp = os.path.join(a.work, "qwen3.gguf"); fp = os.path.join(a.work, "qwen3_gguf.fpk")
        probes = write_gguf(gp, W, hf)
        run([a.forge, "import", gp, "-o", fp])
        check(fp, "gguf qwen3 F32", 1e-3)
        for qt, want in probes.items():
            pf = os.path.join(a.work, f"probe_{qt}.f32")
            run([a.forge, "dump", fp, f"extra.probe_{qt}", pf])
            d = np.abs(np.fromfile(pf, dtype=np.float32) - want).max()
            print(f"[{'ok' if d == 0 else 'FAIL'}] gguf {qt}: block decoder matches gguf-py exactly (max diff {d:g})"); fails += d != 0
    print("golden qwen3:", "PASS" if fails == 0 else f"{fails} FAILURE(S)")
    sys.exit(1 if fails else 0)

if __name__ == "__main__": main()
