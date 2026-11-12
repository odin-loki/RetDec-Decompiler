#!/usr/bin/env python3
"""Golden tests for the MoE (Mixtral) and MLA+MoE (DeepSeek-V2) templates.

Random tiny HF checkpoints + float64 numpy references written from the HF modelling code's maths;
checks prefill logits and KV/latent-cached greedy decode against full recompute."""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import tiny_llama as T  # tokenizer writer, rms, rope (neox), run()

def silu(x): return x / (1 + np.exp(-x))
def softmax(x): e = np.exp(x - x.max(-1, keepdims=True)); return e / e.sum(-1, keepdims=True)

def rope_interleaved(x, pos, base):  # GPT-J pairs (2i, 2i+1); == HF DeepseekV2 permuted rotate_half
    d = x.shape[-1]; inv = base ** (-np.arange(d // 2) * 2.0 / d)
    th = pos[:, None] * inv[None, :]; c, s = np.cos(th)[:, None, :], np.sin(th)[:, None, :]
    x1, x2 = x[..., 0::2], x[..., 1::2]
    o = np.empty_like(x); o[..., 0::2] = x1 * c - x2 * s; o[..., 1::2] = x1 * s + x2 * c
    return o

def attend(q, k, v, scale):  # q [s,h,dq] k [s,h,dq] v [s,h,dv]
    s = q.shape[0]
    sc = np.einsum("shd,thd->hst", q, k) * scale + np.triu(np.full((s, s), -np.inf), 1)[None]
    return np.einsum("hst,thd->shd", softmax(sc), v)

def moe(h, W, p, n_exp, k, norm, scale_f, names):
    g = softmax(h @ W[p + names[0]].T)
    out = np.zeros_like(h)
    for r in range(h.shape[0]):
        top = np.argsort(-g[r], kind="stable")[:k]
        w = g[r, top] / (g[r, top].sum() if norm else 1.0) * scale_f
        for e, we in zip(top, w):
            q = p + names[1].format(e)
            out[r] += we * ((silu(h[r] @ W[q + names[2]].T) * (h[r] @ W[q + names[3]].T)) @ W[q + names[4]].T)
    return out

def rnd(rng, *s, scale=None): return rng.normal(0, scale if scale else 1 / np.sqrt(s[-1]), s)

# ---------------------------------------------------------------- Mixtral
MCFG = dict(architectures=["MixtralForCausalLM"], model_type="mixtral", hidden_size=48, num_hidden_layers=2, num_attention_heads=4,
            num_key_value_heads=2, intermediate_size=64, vocab_size=300, rms_norm_eps=1e-5, rope_theta=10000.0,
            max_position_embeddings=128, bos_token_id=290, eos_token_id=291, num_local_experts=4, num_experts_per_tok=2)

def mixtral_weights(rng):
    C = MCFG; H, F, E = C["hidden_size"], C["intermediate_size"], C["num_local_experts"]; hd = H // C["num_attention_heads"]
    W = {"model.embed_tokens.weight": rnd(rng, 300, H, scale=1.0), "model.norm.weight": 1 + 0.1 * rng.normal(size=H), "lm_head.weight": rnd(rng, 300, H)}
    for l in range(C["num_hidden_layers"]):
        p = f"model.layers.{l}."
        W[p + "input_layernorm.weight"] = 1 + 0.1 * rng.normal(size=H); W[p + "post_attention_layernorm.weight"] = 1 + 0.1 * rng.normal(size=H)
        W[p + "self_attn.q_proj.weight"] = rnd(rng, 4 * hd, H); W[p + "self_attn.k_proj.weight"] = rnd(rng, 2 * hd, H)
        W[p + "self_attn.v_proj.weight"] = rnd(rng, 2 * hd, H); W[p + "self_attn.o_proj.weight"] = rnd(rng, H, 4 * hd)
        W[p + "block_sparse_moe.gate.weight"] = rnd(rng, E, H, scale=1.0)
        for e in range(E):
            q = p + f"block_sparse_moe.experts.{e}."
            W[q + "w1.weight"] = rnd(rng, F, H); W[q + "w3.weight"] = rnd(rng, F, H); W[q + "w2.weight"] = rnd(rng, H, F)
    return W

def mixtral_forward(W, ids):
    C = MCFG; H, nh, nk = C["hidden_size"], C["num_attention_heads"], C["num_key_value_heads"]; hd = H // nh; eps = C["rms_norm_eps"]
    s = len(ids); pos = np.arange(s, dtype=np.float64); x = W["model.embed_tokens.weight"][ids]
    for l in range(C["num_hidden_layers"]):
        p = f"model.layers.{l}."
        h = T.rms(x, W[p + "input_layernorm.weight"], eps)
        q = T.rope((h @ W[p + "self_attn.q_proj.weight"].T).reshape(s, nh, hd), pos, C["rope_theta"])
        k = np.repeat(T.rope((h @ W[p + "self_attn.k_proj.weight"].T).reshape(s, nk, hd), pos, C["rope_theta"]), nh // nk, 1)
        v = np.repeat((h @ W[p + "self_attn.v_proj.weight"].T).reshape(s, nk, hd), nh // nk, 1)
        x = x + attend(q, k, v, 1 / np.sqrt(hd)).reshape(s, H) @ W[p + "self_attn.o_proj.weight"].T
        h = T.rms(x, W[p + "post_attention_layernorm.weight"], eps)
        x = x + moe(h, W, p, C["num_local_experts"], C["num_experts_per_tok"], True, 1.0,
                    ("block_sparse_moe.gate.weight", "block_sparse_moe.experts.{}.", "w1.weight", "w3.weight", "w2.weight"))
    return T.rms(x, W["model.norm.weight"], eps) @ W["lm_head.weight"].T

# ---------------------------------------------------------------- DeepSeek-V2 (MLA + MoE)
DCFG = dict(architectures=["DeepseekV2ForCausalLM"], model_type="deepseek_v2", hidden_size=48, num_hidden_layers=3, num_attention_heads=4,
            intermediate_size=64, moe_intermediate_size=32, vocab_size=300, rms_norm_eps=1e-6, rope_theta=10000.0,
            max_position_embeddings=128, bos_token_id=290, eos_token_id=291, q_lora_rank=24, kv_lora_rank=16,
            qk_nope_head_dim=8, qk_rope_head_dim=4, v_head_dim=6, n_routed_experts=4, n_shared_experts=1, num_experts_per_tok=2,
            first_k_dense_replace=1, norm_topk_prob=False, routed_scaling_factor=1.5, topk_method="greedy", scoring_func="softmax",
            moe_layer_freq=1, n_group=1, topk_group=1, rope_scaling=None)

def deepseek_weights(rng, q_lora=True):
    C = DCFG; H, nh = C["hidden_size"], C["num_attention_heads"]
    nope, rd, vd, r, ql = C["qk_nope_head_dim"], C["qk_rope_head_dim"], C["v_head_dim"], C["kv_lora_rank"], C["q_lora_rank"]
    W = {"model.embed_tokens.weight": rnd(rng, 300, H, scale=1.0), "model.norm.weight": 1 + 0.1 * rng.normal(size=H), "lm_head.weight": rnd(rng, 300, H)}
    for l in range(C["num_hidden_layers"]):
        p = f"model.layers.{l}."
        W[p + "input_layernorm.weight"] = 1 + 0.1 * rng.normal(size=H); W[p + "post_attention_layernorm.weight"] = 1 + 0.1 * rng.normal(size=H)
        if q_lora:
            W[p + "self_attn.q_a_proj.weight"] = rnd(rng, ql, H); W[p + "self_attn.q_a_layernorm.weight"] = 1 + 0.1 * rng.normal(size=ql)
            W[p + "self_attn.q_b_proj.weight"] = rnd(rng, nh * (nope + rd), ql)
        else: W[p + "self_attn.q_proj.weight"] = rnd(rng, nh * (nope + rd), H)
        W[p + "self_attn.kv_a_proj_with_mqa.weight"] = rnd(rng, r + rd, H); W[p + "self_attn.kv_a_layernorm.weight"] = 1 + 0.1 * rng.normal(size=r)
        W[p + "self_attn.kv_b_proj.weight"] = rnd(rng, nh * (nope + vd), r); W[p + "self_attn.o_proj.weight"] = rnd(rng, H, nh * vd)
        def mlp(q, F): W[q + "gate_proj.weight"] = rnd(rng, F, H); W[q + "up_proj.weight"] = rnd(rng, F, H); W[q + "down_proj.weight"] = rnd(rng, H, F)
        if l < C["first_k_dense_replace"]: mlp(p + "mlp.", C["intermediate_size"])
        else:
            W[p + "mlp.gate.weight"] = rnd(rng, C["n_routed_experts"], H, scale=1.0)
            mlp(p + "mlp.shared_experts.", C["moe_intermediate_size"] * C["n_shared_experts"])
            for e in range(C["n_routed_experts"]): mlp(p + f"mlp.experts.{e}.", C["moe_intermediate_size"])
    return W

def deepseek_forward(W, ids):
    C = DCFG; H, nh, eps = C["hidden_size"], C["num_attention_heads"], C["rms_norm_eps"]
    nope, rd, vd, r = C["qk_nope_head_dim"], C["qk_rope_head_dim"], C["v_head_dim"], C["kv_lora_rank"]
    s = len(ids); pos = np.arange(s, dtype=np.float64); x = W["model.embed_tokens.weight"][ids]
    def dense(h, q): return (silu(h @ W[q + "gate_proj.weight"].T) * (h @ W[q + "up_proj.weight"].T)) @ W[q + "down_proj.weight"].T
    for l in range(C["num_hidden_layers"]):
        p = f"model.layers.{l}."; a = p + "self_attn."
        h = T.rms(x, W[p + "input_layernorm.weight"], eps)
        if a + "q_a_proj.weight" in W: q = T.rms(h @ W[a + "q_a_proj.weight"].T, W[a + "q_a_layernorm.weight"], eps) @ W[a + "q_b_proj.weight"].T
        else: q = h @ W[a + "q_proj.weight"].T
        q = q.reshape(s, nh, nope + rd)
        q = np.concatenate([q[..., :nope], rope_interleaved(q[..., nope:], pos, C["rope_theta"])], -1)
        ckv = h @ W[a + "kv_a_proj_with_mqa.weight"].T
        kv = (T.rms(ckv[:, :r], W[a + "kv_a_layernorm.weight"], eps) @ W[a + "kv_b_proj.weight"].T).reshape(s, nh, nope + vd)
        kpe = rope_interleaved(ckv[:, None, r:], pos, C["rope_theta"])
        k = np.concatenate([kv[..., :nope], np.repeat(kpe, nh, 1)], -1)
        x = x + attend(q, k, kv[..., nope:], 1 / np.sqrt(nope + rd)).reshape(s, nh * vd) @ W[a + "o_proj.weight"].T
        h = T.rms(x, W[p + "post_attention_layernorm.weight"], eps)
        if l < C["first_k_dense_replace"]: x = x + dense(h, p + "mlp.")
        else:
            x = x + dense(h, p + "mlp.shared_experts.") + moe(h, W, p, C["n_routed_experts"], C["num_experts_per_tok"], C["norm_topk_prob"],
                                                              C["routed_scaling_factor"], ("mlp.gate.weight", "mlp.experts.{}.", "gate_proj.weight", "up_proj.weight", "down_proj.weight"))
    return T.rms(x, W["model.norm.weight"], eps) @ W["lm_head.weight"].T

def check(name, forge, work, cfg, W, fwd):
    from safetensors.numpy import save_file
    hf = os.path.join(work, name); T.make_model(hf)  # tokenizer.json (and a llama checkpoint we overwrite)
    save_file({k: v.astype(np.float32) for k, v in W.items()}, os.path.join(hf, "model.safetensors"))
    json.dump(cfg, open(os.path.join(hf, "config.json"), "w"), indent=1)
    W = {k: v.astype(np.float32).astype(np.float64) for k, v in W.items()}
    out = os.path.join(work, name + ".fpk"); T.run([forge, "import", hf, "-o", out])
    prompt = [290, 72, 101, 108, 108, 111, 44, 32, 116, 104, 101]
    lf = os.path.join(work, name + "_logits.f32")
    gen = T.run([forge, "run", out, "--tokens", ",".join(map(str, prompt)), "-n", "12", "--ids", "--logits", lf])
    ref = fwd(W, prompt)[-1]; got = np.fromfile(lf, dtype=np.float32)
    err = np.abs(got - ref).max(); fails = 0
    ok = err < 2e-3; fails += not ok
    print(f"[{'ok' if ok else 'FAIL'}] {name}: prefill logits max|err| {err:.2e}")
    ids = list(prompt); rg = []
    for _ in range(12):
        t = int(np.argmax(fwd(W, ids)[-1])); rg.append(t); ids.append(t)
        if t == cfg["eos_token_id"]: break
    fg = [int(t) for t in gen.split()]
    same = fg == rg
    if not same:
        i = next((i for i in range(min(len(fg), len(rg))) if fg[i] != rg[i]), None)
        if i is not None: lg = np.sort(fwd(W, prompt + rg[:i])[-1]); same = lg[-1] - lg[-2] < 1e-3
    print(f"[{'ok' if same else 'FAIL'}] {name}: cached greedy decode matches full recompute: {fg[:8]} vs {rg[:8]}")
    return fails + (not same)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--forge", required=True); ap.add_argument("--work", default="build/golden_moe")
    a = ap.parse_args(); os.makedirs(a.work, exist_ok=True)
    rng = np.random.default_rng(7)
    fails = check("mixtral", a.forge, a.work, MCFG, mixtral_weights(rng), mixtral_forward)
    fails += check("deepseek_v2", a.forge, a.work, DCFG, deepseek_weights(rng, True), deepseek_forward)
    c2 = dict(DCFG, q_lora_rank=None)
    global_cfg = DCFG.copy(); DCFG.update(c2)
    fails += check("deepseek_v2_lite", a.forge, a.work, c2, deepseek_weights(rng, False), deepseek_forward)
    DCFG.update(global_cfg)
    print("golden moe:", "PASS" if fails == 0 else f"{fails} FAILURE(S)")
    sys.exit(1 if fails else 0)

if __name__ == "__main__": main()
