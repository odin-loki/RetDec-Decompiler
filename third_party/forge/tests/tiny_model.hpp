#pragma once
// Builds a random tiny Llama .fpk in-process (byte-level tokenizer, vocab 260) for engine tests.
#include <forge/import/hf.hpp>
#include <forge/modelfile/fpk.hpp>
#include <cmath>
#include <random>

namespace forge_test {

inline std::string utf8(unsigned cp) {
    std::string s;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 63)); }
    else { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 63)); s += char(0x80 | (cp & 63)); }
    return s;
}
inline std::string byte_tokenizer_json() {
    std::vector<int> bs;
    for (int b = '!'; b <= '~'; ++b) bs.push_back(b);
    for (int b = 0xA1; b <= 0xAC; ++b) bs.push_back(b);
    for (int b = 0xAE; b <= 0xFF; ++b) bs.push_back(b);
    std::vector<unsigned> cs(bs.begin(), bs.end());
    int n = 0;
    for (int b = 0; b < 256; ++b) if (std::find(bs.begin(), bs.end(), b) == bs.end()) { bs.push_back(b); cs.push_back(256 + unsigned(n++)); }
    std::string v = "{";
    for (std::size_t i = 0; i < bs.size(); ++i) {
        std::string t = utf8(cs[i]);
        if (t == "\"" || t == "\\") t = "\\" + t;
        v += std::format("{}\"{}\":{}", i ? "," : "", t, bs[i]);
    }
    v += "}";
    return std::format(R"({{"model":{{"type":"BPE","vocab":{},"merges":[]}},"added_tokens":[{{"id":256,"content":"<s>","special":true}},{{"id":257,"content":"</s>","special":true}}]}})", v);
}

struct tiny_cfg { int d = 32, layers = 2, heads = 4, kv = 2, ffn = 64, vocab = 260; unsigned seed = 7; std::string codec = "f32"; float logit_scale = 3.f; };

inline std::string make_tiny_model(const std::string& path, tiny_cfg c = {}) {
    using namespace forge;
    json::object cfg{{"hidden_size", double(c.d)}, {"num_hidden_layers", double(c.layers)}, {"num_attention_heads", double(c.heads)},
                     {"num_key_value_heads", double(c.kv)}, {"head_dim", double(c.d / c.heads)}, {"intermediate_size", double(c.ffn)},
                     {"vocab_size", double(c.vocab)}, {"rms_norm_eps", 1e-5}, {"rope_theta", 10000.0}, {"rope_neox", 1.0}};
    auto bp = import::instantiate(import::read_file(import::template_dir() / "llama.fbp"), json::value(cfg));
    std::mt19937 rng(c.seed); std::normal_distribution<float> N(0, 1);
    modelfile::writer w;
    auto put = [&](const std::string& n, shape_t s, float sd, float mean = 0) {
        std::vector<float> d(std::size_t(numel(s))); for (auto& x : d) x = mean + sd * N(rng);
        w.add_tensor(n, s, d, n.find("norm") != std::string::npos || n == "tok_emb" ? std::string("f32") : c.codec);
    };
    int hd = c.d / c.heads;
    put("tok_emb", {c.vocab, c.d}, 1.f);
    for (int l = 0; l < c.layers; ++l) {
        auto Q = std::format("layer.{}.", l);
        put(Q + "attn_norm", {c.d}, .1f, 1); put(Q + "ffn_norm", {c.d}, .1f, 1);
        float s = 1.f / std::sqrt(float(c.d));
        put(Q + "wq", {c.heads * hd, c.d}, s); put(Q + "wk", {c.kv * hd, c.d}, s); put(Q + "wv", {c.kv * hd, c.d}, s);
        put(Q + "wo", {c.d, c.heads * hd}, s);
        put(Q + "w_gate", {c.ffn, c.d}, s); put(Q + "w_up", {c.ffn, c.d}, s); put(Q + "w_down", {c.d, c.ffn}, 1.f / std::sqrt(float(c.ffn)));
    }
    put("out_norm", {c.d}, .1f, 1);
    put("lm_head", {c.vocab, c.d}, c.logit_scale / std::sqrt(float(c.d)));
    w.set_manifest(json::value(json::object{{"format", "fpk"}, {"session", "generative_sequence"}, {"bos_token_id", 256.0},
                                            {"eos_token_id", 257.0}, {"max_position_embeddings", 512.0}}));
    w.set_section("blueprint", bp);
    w.set_section("tokenizer", byte_tokenizer_json());
    w.write(path);
    return path;
}

// A tiny random Qwen3.5-style hybrid (qwen3_5.fbp): 4 layers, every 2nd one gated full attention, the others
// Gated DeltaNet with a causal conv window. Its state has recurrent slots that cannot be cut to a shorter prefix.
inline std::string make_tiny_qwen35_model(const std::string& path, unsigned seed = 5) {
    using namespace forge;
    const int d = 32, L = 4, iv = 2, heads = 2, kv = 1, hd = 16, ffn = 64, vocab = 260, lkh = 2, lkd = 8, lvh = 2, lvd = 8, K = 4;
    json::object cfg{{"hidden_size", double(d)}, {"num_hidden_layers", double(L)}, {"num_attention_heads", double(heads)},
                     {"num_key_value_heads", double(kv)}, {"head_dim", double(hd)}, {"intermediate_size", double(ffn)},
                     {"vocab_size", double(vocab)}, {"rms_norm_eps", 1e-6}, {"rotary_dim", 4.0}, {"rope_theta", 10000.0},
                     {"full_attention_interval", double(iv)}, {"n_blocks", double(L / iv)}, {"linear_conv_kernel_dim", double(K)},
                     {"linear_num_key_heads", double(lkh)}, {"linear_key_head_dim", double(lkd)},
                     {"linear_num_value_heads", double(lvh)}, {"linear_value_head_dim", double(lvd)}};
    auto bp = import::instantiate(import::read_file(import::template_dir() / "qwen3_5.fbp"), json::value(cfg));
    std::mt19937 rng(seed); std::normal_distribution<float> N(0, 1);
    modelfile::writer w;
    auto put = [&](const std::string& n, shape_t s, float sd, float mean = 0) {
        std::vector<float> v(std::size_t(numel(s))); for (auto& x : v) x = mean + sd * N(rng);
        w.add_tensor(n, s, v, "f32");
    };
    auto lin = [&](const std::string& n, int out, int in) { put(n, {out, in}, 0.8f / std::sqrt(float(in))); };
    put("tok_emb", {vocab, d}, 1.f);
    const int conv_c = 2 * lkh * lkd + lvh * lvd;
    for (int l = 0; l < L; ++l) {
        auto Q = std::format("layer.{}.", l);
        put(Q + "attn_norm", {d}, .1f, 1); put(Q + "ffn_norm", {d}, .1f, 1);
        lin(Q + "w_gate", ffn, d); lin(Q + "w_up", ffn, d); lin(Q + "w_down", d, ffn);
        if ((l + 1) % iv) {   // Gated DeltaNet
            lin(Q + "w_qkv", conv_c, d); lin(Q + "w_z", lvh * lvd, d); lin(Q + "w_a", lvh, d); lin(Q + "w_b", lvh, d);
            put(Q + "conv_w", {conv_c, K}, .5f);
            put(Q + "dt_bias", {lvh}, .5f);
            std::vector<float> an(static_cast<std::size_t>(lvh)); for (auto& x : an) x = -(0.1f + std::fabs(N(rng)));   // -exp(A_log) < 0
            w.add_tensor(Q + "a_neg", {lvh}, an, "f32");
            put(Q + "ssm_norm", {lvd}, .1f, 1);
            lin(Q + "wo", d, lvh * lvd);
        } else {               // gated full attention
            lin(Q + "wq", heads * 2 * hd, d); lin(Q + "wk", kv * hd, d); lin(Q + "wv", kv * hd, d); lin(Q + "wo", d, heads * hd);
            put(Q + "q_norm", {hd}, .1f, 1); put(Q + "k_norm", {hd}, .1f, 1);
        }
    }
    put("out_norm", {d}, .1f, 1);
    put("lm_head", {vocab, d}, 3.f / std::sqrt(float(d)));
    w.set_manifest(json::value(json::object{{"format", "fpk"}, {"session", "generative_sequence"}, {"bos_token_id", 256.0},
                                            {"eos_token_id", 257.0}, {"max_position_embeddings", 512.0}}));
    w.set_section("blueprint", bp);
    w.set_section("tokenizer", byte_tokenizer_json());
    w.write(path);
    return path;
}

} // namespace forge_test
