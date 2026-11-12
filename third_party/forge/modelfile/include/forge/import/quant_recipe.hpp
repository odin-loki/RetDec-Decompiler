#pragma once
// Per-tensor quantisation type choice for mixed-precision imports (Q4_K_M / Q5_K_M and FORGE's recipe).
//
// The "llama" recipe is a port of llama_tensor_get_type() from llama.cpp
//   https://github.com/ggml-org/llama.cpp/blob/bc428e407a52bd3e21b5985cabeef57cdff9bc4a/src/llama-quant.cpp
//   Copyright (c) 2023-2026 The ggml authors. MIT License (https://github.com/ggml-org/llama.cpp/blob/master/LICENSE).
// Only the rules that apply to the K-quant mixes FORGE can encode are ported:
//   use_more_bits (L490-492), base types (L950-952), output.weight (L539-562), token_embd (L571-588),
//   attn_v (L613-650), ffn_down (L670-714), attn_output (L715-734), attn_qkv (L735-741), override order (L763-819).
// The "forge" recipe applies the same rules and then adds FORGE's extensions: Gated DeltaNet in/out projections and
// attn_output at q6_k, and the head and token embeddings at q8_0 (or bf16 with head_bf16). Norms, conv, A/dt and the
// a/b gates are never quantised (the importer stores them as f32).
#include <forge/core/error.hpp>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace forge::import {

struct quant_recipe {
    std::string ftype;               // "q4_k_m", "q5_k_m", or a plain codec name (no mixture)
    std::string recipe = "forge";    // "llama" (exact port) or "forge"
    std::string output_type;         // --output-tensor-type
    std::string token_embedding_type;// --token-embedding-type
    std::vector<std::pair<std::string, std::string>> tensor_types;   // --tensor-type REGEX=TYPE, first match wins
    bool pure = false;               // --pure: base type everywhere except explicit overrides
    bool head_bf16 = false;          // FORGE: lm_head and tok_emb in bf16 (CUDA runs them on BF16 tensor cores)
    int n_layer = 0, n_attn_v = 0, n_expert = 0;   // model facts (n_attn_v: layers that have a V projection)
};

inline bool is_mix_ftype(const std::string& f) { return f == "q4_k_m" || f == "q5_k_m"; }
inline std::string mix_base(const std::string& f) { return f == "q4_k_m" ? "q4_k" : f == "q5_k_m" ? "q5_k" : f; }

// llama.cpp use_more_bits: first and last eighth of the layers, and every third layer in between.
inline bool use_more_bits(int i, int n) { return i < n / 8 || i >= 7 * n / 8 || (i - n / 8) % 3 == 2; }

// FORGE tensor names ("layer.N.wv", "lm_head", "tok_emb", "layer.N.expert.E.w_down") -> llama.cpp role.
inline int recipe_layer(const std::string& n) {
    if (!n.starts_with("layer.")) return -1;
    return std::stoi(n.substr(6, n.find('.', 6) - 6));
}
inline std::string recipe_leaf(const std::string& n) { auto p = n.rfind('.'); return p == std::string::npos ? n : n.substr(p + 1); }

// Type for one quantisable tensor. `cols` is the row length (the block axis). `deltanet` marks Gated DeltaNet layers.
// `attn_v_index` is the running index of this V projection among the n_attn_v of the model (llama's i_attention_wv).
inline std::string recipe_type(const quant_recipe& r, const std::string& name, long long cols, bool deltanet = false, int attn_v_index = -1) {
    const std::string base = mix_base(r.ftype);
    const std::string leaf = recipe_leaf(name);
    const int il = recipe_layer(name);
    const bool forge = r.recipe == "forge";
    const bool is_out = name == "lm_head", is_emb = name == "tok_emb";
    std::string t = base;
    if (!r.pure && is_mix_ftype(r.ftype)) {
        if (is_out) t = cols % 256 ? "q8_0" : "q6_k";
        else if (is_emb) t = base;
        else if (leaf == "wv") {
            if (r.n_expert == 8) t = "q8_0";
            else if (use_more_bits(attn_v_index < 0 ? il : attn_v_index, r.n_attn_v > 0 ? r.n_attn_v : r.n_layer)) t = "q6_k";
        } else if (leaf == "w_down") {
            if (use_more_bits(il, r.n_layer)) t = "q6_k";
        } else if (leaf == "wo" && !deltanet) {
            if (r.n_expert == 8) t = "q5_k";
        } else if (leaf == "w_qkv") {       // fused QKV (llama attn_qkv; Qwen3.5 DeltaNet in_proj_qkv)
            t = r.ftype == "q4_k_m" ? "q5_k" : "q6_k";
        }
        if (forge) {
            if (is_out || is_emb) t = "q8_0";
            if (leaf == "wo" || (deltanet && (leaf == "w_qkv" || leaf == "w_z"))) t = "q6_k";
        }
    }
    if (forge && r.head_bf16 && (is_out || is_emb)) t = "bf16";
    if (is_emb && !r.token_embedding_type.empty()) t = r.token_embedding_type;
    if (is_out && !r.output_type.empty()) t = r.output_type;
    for (auto& [re, ty] : r.tensor_types)
        if (std::regex_search(name, std::regex(re))) { t = ty; break; }
    // shape fallback: k-quants need whole 256-wide super-blocks
    if ((t == "q4_k" || t == "q5_k" || t == "q6_k") && cols % 256) t = cols % 32 ? "f32" : "q8_0";
    if (t == "q8_0" && cols % 32) t = "f32";
    return t;
}

inline std::pair<std::string, std::string> parse_tensor_type(const std::string& s) {
    auto p = s.find('=');
    if (p == std::string::npos || p == 0 || p + 1 == s.size()) raise(errc::invalid_argument, "--tensor-type wants REGEX=TYPE, got '{}'", s);
    return {s.substr(0, p), s.substr(p + 1)};
}

} // namespace forge::import
