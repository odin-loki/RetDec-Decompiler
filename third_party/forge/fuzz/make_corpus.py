#!/usr/bin/env python3
"""Regenerate the binary seed corpus (tiny .fpk / .safetensors / .gguf / tokenizer.json). Usage: make_corpus.py <forge>"""
import json, os, subprocess, sys, tempfile
import numpy as np
from safetensors.numpy import save_file
forge = sys.argv[1]; here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus")
rng = np.random.default_rng(0)
d = tempfile.mkdtemp()
cfg = dict(architectures=["LlamaForCausalLM"], hidden_size=8, num_hidden_layers=1, num_attention_heads=2, num_key_value_heads=1,
           intermediate_size=8, vocab_size=20, rms_norm_eps=1e-5, max_position_embeddings=16, bos_token_id=1, eos_token_id=2)
W = {"model.embed_tokens.weight": rng.normal(size=(20, 8)), "model.norm.weight": np.ones(8), "lm_head.weight": rng.normal(size=(20, 8))}
p = "model.layers.0."
for n, s in [("input_layernorm", (8,)), ("post_attention_layernorm", (8,)), ("self_attn.q_proj", (8, 8)), ("self_attn.k_proj", (4, 8)),
             ("self_attn.v_proj", (4, 8)), ("self_attn.o_proj", (8, 8)), ("mlp.gate_proj", (8, 8)), ("mlp.up_proj", (8, 8)), ("mlp.down_proj", (8, 8))]:
    W[p + n + ".weight"] = rng.normal(size=s)
W = {k: v.astype(np.float32) for k, v in W.items()}
save_file(W, os.path.join(d, "model.safetensors")); json.dump(cfg, open(os.path.join(d, "config.json"), "w"))
tok = {"model": {"type": "BPE", "vocab": {c: i for i, c in enumerate("abcdefghijklmnopqrst")}, "merges": ["a b", "c d"]},
       "added_tokens": [{"id": 1, "content": "<s>", "special": True}]}
json.dump(tok, open(os.path.join(d, "tokenizer.json"), "w"))
open(os.path.join(here, "tokenizer", "tok.json"), "w").write(json.dumps(tok) + "\x01abcd ab")
save_file({"a": np.arange(6, dtype=np.float32).reshape(2, 3), "b": np.ones(4, dtype=np.float16)}, os.path.join(here, "safetensors", "small.safetensors"))
subprocess.run([forge, "import", d, "-o", os.path.join(here, "fpk", "tiny.fpk"), "--codec", "q8_0"], check=True, capture_output=True)
subprocess.run([forge, "keygen", "-o", os.path.join(d, "k")], check=True, capture_output=True)
subprocess.run([forge, "sign", os.path.join(here, "fpk", "tiny.fpk"), "--key", os.path.join(d, "k.key"), "-o", os.path.join(here, "fpk", "tiny_signed.fpk")], check=True, capture_output=True)
import gguf
w = gguf.GGUFWriter(os.path.join(here, "gguf", "small.gguf"), "llama")
w.add_block_count(1); w.add_token_list(["a", "b"]); w.add_tensor("t", np.ones((2, 32), np.float32))
w.write_header_to_file(); w.write_kv_data_to_file(); w.write_tensors_to_file(); w.close()
for k in os.listdir(here): print(k, os.listdir(os.path.join(here, k)))
