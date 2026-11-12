#pragma once
// Hugging Face directory (config.json + *.safetensors + tokenizer.json) -> .fpk
#include <forge/import/safetensors.hpp>
#include <forge/import/quant_recipe.hpp>
#include <forge/modelfile/blueprint.hpp>
#include <forge/modelfile/fpk.hpp>
#include <forge/security/enforce.hpp>
#include <forge/os/fs.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace forge::import {

// FSS CAP-1: under an active forge::security policy every importer input must pass enforce(fs_read) and the output
// enforce(fs_write) (permission_denied, audited). No policy (profile none): no check.
inline void check_read(const std::filesystem::path& p) { security::enforce(security::capability::fs_read, os::fs::u8str(p)); }
inline void check_write(const std::string& p) { security::enforce(security::capability::fs_write, p); }

inline std::string read_file(const std::filesystem::path& p) {
    check_read(p);
    std::ifstream f(p, std::ios::binary);
    if (!f) raise(errc::io_error, "cannot read {}", p.string());
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

#ifndef FORGE_SOURCE_DIR
#define FORGE_SOURCE_DIR "."
#endif
inline std::filesystem::path template_dir() {
    if (auto* e = std::getenv("FORGE_TEMPLATES")) return e;
    return std::filesystem::path(FORGE_SOURCE_DIR) / "import" / "templates";
}

// Substitute {{key}} with config values (numbers formatted losslessly).
inline std::string instantiate(std::string tpl, const json::value& cfg) {
    std::size_t p;
    while ((p = tpl.find("{{")) != std::string::npos) {
        auto e = tpl.find("}}", p);
        auto key = tpl.substr(p + 2, e - p - 2);
        auto* v = cfg.find(key);
        if (!v) raise(errc::not_found, "template needs config key '{}'", key);
        std::string s;
        if (v->is_num()) { char b[40]; std::snprintf(b, 40, "%.17g", v->num()); s = b; } else s = v->str();
        tpl.replace(p, e - p + 2, s);
    }
    return tpl;
}

struct import_options {
    std::string codec = "f32"; std::string template_name;
    std::string emb_codec;   // tok_emb codec; empty = q8_0 when codec is quantised, else f32
    // Mixed precision (codec "q4_k_m"/"q5_k_m", or any override set): per-tensor types from quant_recipe.hpp.
    quant_recipe recipe;
    bool mixed() const { return is_mix_ftype(codec) || !recipe.output_type.empty() || !recipe.token_embedding_type.empty() || !recipe.tensor_types.empty() || recipe.head_bf16; }
};

struct import_report { std::string arch, template_name; std::size_t tensors = 0, bytes = 0; };

// The .fpk "experts" section (format minor 1): one entry per stacked expert tensor [n_experts, rows, k] (moe_ffn
// gate/up/down). Expert e of a tensor occupies bytes [offset + e * expert_bytes, + expert_bytes) of the data section
// (file offset = 64 + offset for stream_writer files); "aligned" says every expert slice starts on a 4 KiB file offset,
// so an expert can be read with one direct (O_DIRECT / unbuffered) read. Groundwork for streaming experts from disk.
struct expert_index {
    json::array ents;
    void add(const modelfile::tensor_entry& e, i64 layer, const char* proj) {
        if (e.shape.size() != 3 || e.shape[0] < 1) raise(errc::shape_mismatch, "expert tensor {} has shape {}", e.name, shape_str(e.shape));
        std::uint64_t ne = std::uint64_t(e.shape[0]), per = e.bytes / ne, foff = 64 + e.offset;
        ents.push_back(json::object{{"layer", double(layer)}, {"proj", std::string(proj)}, {"name", e.name}, {"codec", e.codec},
                                    {"n_experts", double(ne)}, {"offset", double(e.offset)}, {"file_offset", double(foff)},
                                    {"expert_bytes", double(per)}, {"aligned", foff % modelfile::expert_alignment == 0 && per % modelfile::expert_alignment == 0}});
    }
    bool empty() const { return ents.empty(); }
    std::string dump() const {
        return json::dump(json::value(json::object{{"version", 1.0}, {"align", double(modelfile::expert_alignment)}, {"tensors", ents}}));
    }
};

inline import_report import_hf(const std::filesystem::path& dir, const std::string& out, const import_options& opt = {}) {
    check_read(dir); check_write(out);
    auto cfg = json::parse(read_file(dir / "config.json"));
    std::string arch0 = cfg.find("architectures") ? cfg["architectures"][0].str() : "";
    if (cfg.find("text_config") && cfg["text_config"].is_obj()) {   // multimodal wrapper (Qwen3.5): import the text model
        json::object t = cfg["text_config"].obj();
        bool has_arch = false; for (auto& [k, v] : t) has_arch |= k == "architectures";
        if (!has_arch && !arch0.empty()) t.emplace_back("architectures", json::value(json::array{json::value(arch0)}));
        for (auto& [k, v] : cfg.obj()) if (k == "tie_word_embeddings" || k == "bos_token_id" || k == "eos_token_id") {
            bool dup = false; for (auto& [kk, vv] : t) dup |= kk == k; if (!dup) t.emplace_back(k, v);
        }
        cfg = json::value(t);
    }
    std::string arch = cfg.find("architectures") ? cfg["architectures"][0].str() : cfg.str_or("model_type", "unknown");
    std::string tname = opt.template_name;
    if (tname.empty()) {
        if (arch == "LlamaForCausalLM" || arch == "MistralForCausalLM" || arch == "Qwen2ForCausalLM" || cfg.str_or("model_type", "") == "llama")
            tname = "llama";
        else if (arch == "Qwen3ForCausalLM" || cfg.str_or("model_type", "") == "qwen3") tname = "qwen3";
        else if (arch == "Qwen3_5ForConditionalGeneration" || arch == "Qwen3_5ForCausalLM" || cfg.str_or("model_type", "") == "qwen3_5")
            tname = "qwen3_5";
        else if (cfg.str_or("model_type", "") == "qwen3_5_text") tname = "qwen3_5";
        else if (arch == "MixtralForCausalLM" || cfg.str_or("model_type", "") == "mixtral") tname = "mixtral";
        else if (arch == "DeepseekV2ForCausalLM" || arch == "DeepseekV3ForCausalLM" || cfg.str_or("model_type", "") == "deepseek_v2") tname = "deepseek_v2";
        else if (arch == "Qwen3MoeForCausalLM" || cfg.str_or("model_type", "") == "qwen3_moe") tname = "qwen3_moe";
        else raise(errc::unsupported, "no blueprint template for architecture {} (pass --template)", arch);
    }
    // normalise config with defaults the template needs
    json::object c = cfg.obj();
    auto num = [&](const char* k, double d) { return cfg.num_or(k, d); };
    double heads = num("num_attention_heads", 0), hidden = num("hidden_size", 0);
    auto set = [&](const std::string& k, json::value v) {
        for (auto& [kk, vv] : c) if (kk == k) { vv = v; return; }
        c.emplace_back(k, v);
    };
    set("head_dim", num("head_dim", hidden / heads));
    set("num_key_value_heads", num("num_key_value_heads", heads));
    set("rope_theta", num("rope_theta", 10000.0));
    set("rms_norm_eps", num("rms_norm_eps", 1e-6));
    set("rope_neox", 1.0);  // HF checkpoints use rotate_half (NeoX) RoPE
    if (tname == "deepseek_v2") {
        double nope = num("qk_nope_head_dim", 128), rd = num("qk_rope_head_dim", 64);
        set("qk_head_dim", nope + rd);
        set("attn_scale", 1.0 / std::sqrt(nope + rd));
        if (cfg.find("rope_scaling") && cfg["rope_scaling"].is_obj())
            raise(errc::unsupported, "deepseek_v2: rope_scaling (YaRN) is not supported yet");
        if (cfg.str_or("topk_method", "greedy") != "greedy" || cfg.str_or("scoring_func", "softmax") != "softmax")
            raise(errc::unsupported, "deepseek_v2: only greedy top-k with softmax scoring is supported");
        bool qlora = cfg.find("q_lora_rank") && cfg["q_lora_rank"].is_num();
        set("mla_q_expr", json::value(std::string(qlora ? "linear(rms_norm(linear(h, W.layer[l].wq_a), W.layer[l].q_norm, eps), W.layer[l].wq_b)"
                                                        : "linear(h, W.layer[l].wq)")));
        set("first_k_dense_replace", num("first_k_dense_replace", 0));
        set("norm_topk_prob", cfg.find("norm_topk_prob") && cfg["norm_topk_prob"].is_bool() ? json::value(cfg["norm_topk_prob"].b() ? 1.0 : 0.0) : json::value(0.0));
        set("routed_scaling_factor", num("routed_scaling_factor", 1.0));
        if (num("n_group", 1) > 1 && cfg.str_or("topk_method", "greedy") != "greedy") raise(errc::unsupported, "deepseek_v2: group-limited routing");
    }
    if (tname == "qwen3_moe") {
        if (num("decoder_sparse_step", 1) != 1) raise(errc::unsupported, "qwen3_moe: decoder_sparse_step != 1 (dense layers in between)");
        if (const auto* ml = cfg.find("mlp_only_layers"); ml && ml->is_arr() && !ml->arr().empty()) raise(errc::unsupported, "qwen3_moe: mlp_only_layers");
        set("norm_topk_prob", cfg.find("norm_topk_prob") && cfg["norm_topk_prob"].is_bool() ? json::value(cfg["norm_topk_prob"].b() ? 1.0 : 0.0) : json::value(0.0));
        set("routed_scaling_factor", 1.0);
    }
    if (tname == "qwen3_5") {
        const json::value* rp = cfg.find("rope_parameters");
        auto rnum = [&](const char* k, double d) { return rp && rp->is_obj() && rp->find(k) ? (*rp)[k].num() : cfg.num_or(k, d); };
        set("rope_theta", rnum("rope_theta", 10000.0));
        double hd = num("head_dim", hidden / heads);
        set("rotary_dim", std::floor(hd * rnum("partial_rotary_factor", 1.0) + 1e-9));
        if (rnum("partial_rotary_factor", 1.0) >= 1.0) raise(errc::unsupported, "qwen3_5: full-width RoPE is not supported by the template");
        double iv = num("full_attention_interval", 4), L0 = num("num_hidden_layers", 0);
        if (iv < 1 || std::fmod(L0, iv) != 0) raise(errc::unsupported, "qwen3_5: {} layers is not a multiple of full_attention_interval {}", L0, iv);
        if (const auto* lt = cfg.find("layer_types"); lt && lt->is_arr()) {
            const auto& a = lt->arr();
            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i].str() != ((i + 1) % std::size_t(iv) == 0 ? "full_attention" : "linear_attention"))
                    raise(errc::unsupported, "qwen3_5: layer_types[{}] = {} breaks the every-{}th-layer-full pattern", i, a[i].str(), iv);
        }
        if (cfg.find("num_experts") && cfg.num_or("num_experts", 0) > 0) raise(errc::unsupported, "qwen3_5: MoE variants are not supported yet");
        if (cfg.str_or("hidden_act", "silu") != "silu") raise(errc::unsupported, "qwen3_5: hidden_act must be silu");
        set("full_attention_interval", iv);
        set("n_blocks", L0 / iv);
        set("linear_conv_kernel_dim", num("linear_conv_kernel_dim", 4));
    }
    json::value cv(c);
    auto bp_text = instantiate(read_file(template_dir() / (tname + ".fbp")), cv);
    auto bp = modelfile::parse_blueprint(bp_text);  // validate before writing anything

    // gather safetensors shards
    std::vector<std::unique_ptr<safetensors>> shards;
    for (auto& f : std::filesystem::directory_iterator(dir))
        if (f.path().extension() == ".safetensors") shards.push_back(std::make_unique<safetensors>(f.path().string()));
    if (shards.empty()) raise(errc::not_found, "{}: no .safetensors files", dir.string());
    // Vision tower and multi-token-prediction heads are not part of the text model: never loaded.
    auto skipped = [](const std::string& n) {
        for (const char* p : {"visual.", "model.visual.", "mtp.", "model.mtp."}) if (n.starts_with(p)) return true;
        return false;
    };
    auto find = [&](const std::string& n) -> tensor {
        if (skipped(n)) raise(errc::not_found, "tensor {} belongs to a skipped (vision/mtp) tower", n);
        for (auto& s : shards) if (s->has(n)) return s->load(n);
        raise(errc::not_found, "missing tensor {}", n);
    };
    auto has = [&](const std::string& n) { if (skipped(n)) return false; for (auto& s : shards) if (s->has(n)) return true; return false; };
    // Qwen3.5 multimodal checkpoints keep the text model under model.language_model.
    std::string mp = has("model.language_model.embed_tokens.weight") ? "model.language_model." : "model.";

    modelfile::stream_writer w(out);   // each tensor is encoded, written and freed before the next is loaded
    import_report rep{arch, tname, 0, 0};
    i64 L = i64(bp.param("n_layers"));
    quant_recipe rc = opt.recipe;
    rc.ftype = opt.codec; rc.n_layer = int(L);
    if (rc.n_expert == 0 && bp.params.count("n_experts")) rc.n_expert = int(bp.param("n_experts"));
    std::vector<int> v_index(std::size_t(L), -1);
    for (i64 l = 0; l < L; ++l)
        if (has(std::format("{}layers.{}.self_attn.v_proj.weight", mp, l))) v_index[std::size_t(l)] = rc.n_attn_v++;
    bool in_deltanet = false;
    expert_index xi;
    auto put = [&](const std::string& n, const tensor& t, bool quantisable, std::size_t align = modelfile::alignment) {
        std::string c = "f32";
        if (quantisable) {
            if (opt.mixed()) {
                int il = recipe_layer(n);
                c = recipe_type(rc, n, t.shape.empty() ? 1 : t.shape.back(), in_deltanet, il >= 0 ? v_index[std::size_t(il)] : -1);
                if (c == "q4_k_m" || c == "q5_k_m") c = mix_base(c);
            } else c = opt.codec;
        }
        w.add_tensor(n, t.shape, t.data, c, align); ++rep.tensors;
    };
    // routed experts of one layer, stacked into [n_experts, rows, k] per projection (moe_ffn's layout), each tensor
    // starting on a 4 KiB file offset and listed in the "experts" section
    auto experts = [&](i64 l, const std::string& Pfmt, const std::string& Q, const char* g, const char* u, const char* d) {
        i64 ne = i64(bp.param("n_experts"));
        const char* src[3] = {g, u, d}; const char* proj[3] = {"gate", "up", "down"};
        for (int j = 0; j < 3; ++j) {
            tensor st;
            for (i64 e = 0; e < ne; ++e) {
                auto t = find(std::vformat(Pfmt, std::make_format_args(e)) + src[j] + ".weight");
                if (t.shape.size() != 2) raise(errc::shape_mismatch, "expert weight {} has shape {}", src[j], shape_str(t.shape));
                if (e == 0) { st = tensor(shape_t{ne, t.shape[0], t.shape[1]}); }
                else if (t.shape[0] != st.shape[1] || t.shape[1] != st.shape[2]) raise(errc::shape_mismatch, "experts of layer {} differ in shape", l);
                std::copy(t.data.begin(), t.data.end(), st.data.begin() + e * t.shape[0] * t.shape[1]);
            }
            put(Q + "experts.w_" + proj[j], st, true, modelfile::expert_alignment);
            xi.add(w.last(), l, proj[j]);
        }
    };
    {   // quantised embeddings: the runtime gathers rows straight from q8_0/q4_k blocks
        std::string ec = opt.emb_codec.empty() ? (opt.codec == "f32" || opt.codec == "f16" || opt.codec == "bf16" ? opt.codec : std::string("q8_0")) : opt.emb_codec;
        auto t = find(mp + "embed_tokens.weight");
        if (opt.mixed() && opt.emb_codec.empty()) ec = recipe_type(rc, "tok_emb", t.shape.back());
        w.add_tensor("tok_emb", t.shape, t.data, ec); ++rep.tensors;
    }
    auto mlp = [&](const std::string& P, const std::string& Q, const char* g, const char* u, const char* d) {
        put(Q + "w_gate", find(P + g + ".weight"), true);
        put(Q + "w_up", find(P + u + ".weight"), true);
        put(Q + "w_down", find(P + d + ".weight"), true);
    };
    bool q35 = tname == "qwen3_5";
    auto plus1 = [&](tensor t) { if (q35) for (auto& x : t.data) x += 1.f; return t; };   // Qwen3.5 RMSNorm: x * (1 + w)
    for (i64 l = 0; l < L; ++l) {
        auto P = std::format("{}layers.{}.", mp, l); auto Q = std::format("layer.{}.", l);
        in_deltanet = false;
        put(Q + "attn_norm", plus1(find(P + "input_layernorm.weight")), false);
        put(Q + "ffn_norm", plus1(find(P + "post_attention_layernorm.weight")), false);
        if (q35 && has(P + "linear_attn.in_proj_qkv.weight")) {
            auto A = P + "linear_attn.";
            in_deltanet = true;
            put(Q + "w_qkv", find(A + "in_proj_qkv.weight"), true);
            put(Q + "w_z", find(A + "in_proj_z.weight"), true);
            put(Q + "w_a", find(A + "in_proj_a.weight"), false);
            put(Q + "w_b", find(A + "in_proj_b.weight"), false);
            auto cw = find(A + "conv1d.weight");          // [C, 1, K] -> [C, K]
            if (cw.shape.size() == 3) cw.shape = {cw.shape[0], cw.shape[2]};
            put(Q + "conv_w", cw, false);
            put(Q + "dt_bias", find(A + "dt_bias"), false);
            auto an = find(A + "A_log"); for (auto& x : an.data) x = -std::exp(x);   // g = -exp(A_log) * softplus(..)
            put(Q + "a_neg", an, false);
            put(Q + "ssm_norm", find(A + "norm.weight"), false);   // gated norm: plain weight
            put(Q + "wo", find(A + "out_proj.weight"), true);
            mlp(P + "mlp.", Q, "gate_proj", "up_proj", "down_proj");
            continue;
        }
        put(Q + "wo", find(P + "self_attn.o_proj.weight"), true);
        if (tname == "deepseek_v2") {
            if (has(P + "self_attn.q_a_proj.weight")) {
                put(Q + "wq_a", find(P + "self_attn.q_a_proj.weight"), true);
                put(Q + "q_norm", find(P + "self_attn.q_a_layernorm.weight"), false);
                put(Q + "wq_b", find(P + "self_attn.q_b_proj.weight"), true);
            } else put(Q + "wq", find(P + "self_attn.q_proj.weight"), true);
            put(Q + "wkv_a", find(P + "self_attn.kv_a_proj_with_mqa.weight"), true);
            put(Q + "kv_norm", find(P + "self_attn.kv_a_layernorm.weight"), false);
            {   // split kv_b [h*(nope+v), r] into absorbed per-head W_uk [h, nope, r] and W_uv [h, r, v]
                auto kvb = find(P + "self_attn.kv_b_proj.weight");
                i64 h = i64(bp.param("n_heads")), nope = i64(bp.param("nope")), vd = i64(bp.param("v_dim")), r = i64(bp.param("kv_lora"));
                if (kvb.shape != shape_t{h * (nope + vd), r}) raise(errc::shape_mismatch, "kv_b_proj has shape {}", shape_str(kvb.shape));
                tensor uk(shape_t{h, nope, r}), uv(shape_t{h, r, vd});
                for (i64 hh = 0; hh < h; ++hh) for (i64 j = 0; j < nope + vd; ++j) for (i64 c = 0; c < r; ++c) {
                    float x = kvb.data[std::size_t((hh * (nope + vd) + j) * r + c)];
                    if (j < nope) uk.data[std::size_t((hh * nope + j) * r + c)] = x;
                    else uv.data[std::size_t((hh * r + c) * vd + (j - nope))] = x;
                }
                put(Q + "w_uk", uk, false); put(Q + "w_uv", uv, false);
            }
            if (l < i64(bp.param("first_dense"))) mlp(P + "mlp.", Q, "gate_proj", "up_proj", "down_proj");
            else {
                put(Q + "router", find(P + "mlp.gate.weight"), false);
                mlp(P + "mlp.shared_experts.", Q + "shared.", "gate_proj", "up_proj", "down_proj");
                experts(l, P + "mlp.experts.{}.", Q, "gate_proj", "up_proj", "down_proj");
            }
            continue;
        }
        put(Q + "wq", find(P + "self_attn.q_proj.weight"), true);
        put(Q + "wk", find(P + "self_attn.k_proj.weight"), true);
        put(Q + "wv", find(P + "self_attn.v_proj.weight"), true);
        if (tname == "qwen3" || tname == "qwen3_moe" || q35) {
            put(Q + "q_norm", plus1(find(P + "self_attn.q_norm.weight")), false);
            put(Q + "k_norm", plus1(find(P + "self_attn.k_norm.weight")), false);
        }
        if (tname == "mixtral") {
            put(Q + "router", find(P + "block_sparse_moe.gate.weight"), false);
            experts(l, P + "block_sparse_moe.experts.{}.", Q, "w1", "w3", "w2");
        } else if (tname == "qwen3_moe") {
            put(Q + "router", find(P + "mlp.gate.weight"), false);
            experts(l, P + "mlp.experts.{}.", Q, "gate_proj", "up_proj", "down_proj");
        } else mlp(P + "mlp.", Q, "gate_proj", "up_proj", "down_proj");
    }
    put("out_norm", plus1(find(mp + "norm.weight")), false);
    put("lm_head", has("lm_head.weight") ? find("lm_head.weight") : find(mp + "embed_tokens.weight"), true);

    json::object man{{"format", "fpk"}, {"forge_format_version", double(modelfile::format_version)}, {"architecture", arch},
                     {"template", tname}, {"codec", opt.codec}, {"session", bp.session},
                     {"bos_token_id", cfg.num_or("bos_token_id", -1)}, {"eos_token_id", cfg.num_or("eos_token_id", -1)},
                     {"max_position_embeddings", cfg.num_or("max_position_embeddings", 2048)}};
    // tokenizer_config.json's add_bos_token decides whether prompts start with BOS (absent: the old default, BOS added).
    if (std::filesystem::exists(dir / "tokenizer_config.json")) {
        auto tc = json::parse(read_file(dir / "tokenizer_config.json"));
        if (auto* ab = tc.find("add_bos_token"); ab && ab->is_bool()) man.emplace_back("add_bos", ab->b());
    }
    if (!xi.empty()) { man.emplace_back("forge_format_minor", double(modelfile::format_minor)); w.set_section("experts", xi.dump()); }
    w.set_manifest(json::value(man));
    w.set_section("blueprint", bp_text);
    if (std::filesystem::exists(dir / "tokenizer.json")) w.set_section("tokenizer", read_file(dir / "tokenizer.json"));
    w.finish();
    rep.bytes = std::filesystem::file_size(out);
    return rep;
}

} // namespace forge::import
