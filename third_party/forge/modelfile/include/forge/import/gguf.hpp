#pragma once
#include <forge/core/mapped.hpp>
// GGUF (v2/v3) reader and GGUF -> .fpk importer. Quantised blocks (Q8_0, Q4_K) are copied verbatim:
// FORGE's codecs use the GGUF block layouts, so no re-quantisation error is introduced.
#include <forge/import/hf.hpp>
#include <set>

namespace forge::import {

class gguf_file {
public:
    struct tinfo { std::string name; shape_t shape; std::uint32_t type = 0; std::uint64_t offset = 0; };
    explicit gguf_file(const std::string& path) : path_(path) {
        check_read(path);   // FSS CAP-1 (hf.hpp)
        map_ = forge::map_readonly(path, size_);
        try {
            if (!map_) raise(errc::format_error, "{}: empty file", path);
            if (size_ < 24 || std::memcmp(map_, "GGUF", 4)) raise(errc::format_error, "{}: not a GGUF file", path);
            auto ver = rd<std::uint32_t>();
            if (ver < 2 || ver > 3) raise(errc::unsupported, "{}: GGUF version {} unsupported", path, ver);
            auto nt = rd<std::uint64_t>(), nkv = rd<std::uint64_t>();
            for (std::uint64_t i = 0; i < nkv; ++i) { auto k = rstr(); auto t = rd<std::uint32_t>(); kv_.emplace_back(k, rval(t)); }
            for (std::uint64_t i = 0; i < nt; ++i) {
                tinfo ti; ti.name = rstr();
                auto nd = rd<std::uint32_t>(); if (nd > 8) raise(errc::format_error, "{}: tensor {} has {} dims", path_, ti.name, nd);
                std::vector<i64> ne(nd); for (auto& d : ne) d = i64(rd<std::uint64_t>());
                ti.shape.assign(ne.rbegin(), ne.rend());   // ggml ne0 is the innermost dimension
                ti.type = rd<std::uint32_t>(); ti.offset = rd<std::uint64_t>();
                tensors_.push_back(ti);
            }
            std::size_t align = std::size_t(get("general.alignment") ? get("general.alignment")->num() : 32);
            data_ = (p_ + align - 1) / align * align;
        } catch (...) { forge::unmap_readonly(map_, size_); map_ = nullptr; throw; }   // no dtor on a throwing ctor
    }
    gguf_file(const gguf_file&) = delete;
    ~gguf_file() { forge::unmap_readonly(map_, size_); }
    const json::value* get(std::string_view k) const { for (auto& [kk, v] : kv_) if (kk == k) return &v; return nullptr; }
    const json::value& need(std::string_view k) const { if (auto* v = get(k)) return *v; raise(errc::not_found, "{}: missing GGUF key {}", path_, k); }
    const std::vector<tinfo>& tensors() const { return tensors_; }
    const tinfo* find(const std::string& n) const { for (auto& t : tensors_) if (t.name == n) return &t; return nullptr; }
    static const char* codec_of(std::uint32_t t) {
        switch (t) { case 0: return "f32"; case 1: return "f16"; case 8: return "q8_0"; case 12: return "q4_k"; case 13: return "q5_k"; case 14: return "q6_k"; case 30: return "bf16"; default: return nullptr; }
    }
    std::span<const std::uint8_t> bytes(const tinfo& t) const {
        auto* c = codec_of(t.type);
        if (!c) raise(errc::unsupported, "tensor {}: ggml type {} not supported (supported: F32 F16 BF16 Q8_0 Q4_K Q5_K Q6_K)", t.name, t.type);
        for (auto d : t.shape) if (d < 0 || d > (i64(1) << 40)) raise(errc::format_error, "tensor {}: bad shape", t.name);
        std::size_t n = graph::codecs::find(c)->bytes(std::size_t(numel(t.shape)));
        if (t.offset > size_ || n > size_ || data_ + t.offset + n > size_) raise(errc::format_error, "tensor {} out of bounds", t.name);
        return {map_ + data_ + t.offset, n};
    }
private:
    template <class T> T rd() { if (p_ + sizeof(T) > size_) raise(errc::format_error, "{}: truncated", path_); T v; std::memcpy(&v, map_ + p_, sizeof(T)); p_ += sizeof(T); return v; }
    std::string rstr() { auto n = rd<std::uint64_t>(); if (n > size_ - p_) raise(errc::format_error, "{}: truncated", path_); std::string s(reinterpret_cast<const char*>(map_ + p_), n); p_ += n; return s; }
    json::value rval(std::uint32_t t) {
        switch (t) {
        case 0: return double(rd<std::uint8_t>()); case 1: return double(rd<std::int8_t>());
        case 2: return double(rd<std::uint16_t>()); case 3: return double(rd<std::int16_t>());
        case 4: return double(rd<std::uint32_t>()); case 5: return double(rd<std::int32_t>());
        case 6: return double(rd<float>()); case 7: return rd<std::uint8_t>() != 0;
        case 8: return rstr();
        case 9: { auto et = rd<std::uint32_t>(); auto n = rd<std::uint64_t>(); if (n > size_ - p_ || ++adepth_ > 8) raise(errc::format_error, "{}: bad GGUF array", path_); json::array a; a.reserve(n); for (std::uint64_t i = 0; i < n; ++i) a.push_back(rval(et)); --adepth_; return a; }
        case 10: return double(rd<std::uint64_t>()); case 11: return double(rd<std::int64_t>());
        case 12: return rd<double>();
        default: raise(errc::format_error, "{}: bad GGUF value type {}", path_, t);
        }
    }
    std::string path_;
    const std::uint8_t* map_ = nullptr;
    std::size_t size_ = 0, p_ = 4, data_ = 0; int adepth_ = 0;
    std::vector<std::pair<std::string, json::value>> kv_;
    std::vector<tinfo> tensors_;
};

inline import_report import_gguf(const std::string& path, const std::string& out, const import_options& opt = {}) {
    check_write(out);
    gguf_file g(path);
    auto arch = g.need("general.architecture").str();
    if (arch != "llama" && arch != "qwen2" && arch != "qwen3" && arch != "mistral" && arch != "qwen35" && arch != "qwen3moe" && arch != "qwen35moe")
        raise(errc::unsupported, "GGUF architecture '{}' has no template", arch);
    bool q35 = arch == "qwen35" || arch == "qwen35moe";
    bool moe = arch == "qwen3moe" || arch == "qwen35moe";   // sparse MoE FFNs (moe_ffn over stacked *_exps tensors)
    auto K = [&](const std::string& k) { return g.need(arch + "." + k).num(); };
    // per-layer keys (qwen35 head_count_kv: 0 on linear-attention layers) collapse to their largest entry
    auto Kor = [&](const std::string& k, double d) {
        auto* v = g.get(arch + "." + k);
        if (!v) return d;
        if (!v->is_arr()) return v->num();
        double m = d; bool any = false;
        for (auto& e : v->arr()) if (!any || e.num() > m) { m = e.num(); any = true; }
        return m;
    };
    double heads = K("attention.head_count"), hidden = K("embedding_length");
    const auto& toks = g.need("tokenizer.ggml.tokens").arr();
    json::object cfg{{"hidden_size", hidden}, {"num_hidden_layers", K("block_count")}, {"num_attention_heads", heads},
                     {"num_key_value_heads", Kor("attention.head_count_kv", heads)}, {"head_dim", Kor("attention.key_length", hidden / heads)},
                     {"intermediate_size", moe ? Kor("feed_forward_length", 0) : K("feed_forward_length")}, {"vocab_size", double(toks.size())},
                     {"rms_norm_eps", Kor("attention.layer_norm_rms_epsilon", 1e-6)}, {"rope_theta", Kor("rope.freq_base", 10000.0)},
                     // llama.cpp's converter permutes Q/K for llama so it can use interleaved (GPT-J) RoPE
                     {"rope_neox", arch == "llama" || arch == "mistral" ? 0.0 : 1.0}};
    // Qwen3.5 (llama.cpp / Ollama arch "qwen35"): the hybrid Gated DeltaNet + gated attention decoder of the HF
    // qwen3_5 template. The GGUF converter already applies the (1 + w) RMSNorm offset, A = -exp(A_log) and the conv
    // squeeze; it may also reorder the V heads from grouped-by-K-head to tiled order (ssm.v_head_reordered), which
    // the import undoes below.
    i64 lk_heads = 0, lk_dim = 0, lv_heads = 0, lv_dim = 0;
    bool v_tiled = false;
    if (q35) {
        double L0 = K("block_count"), iv = Kor("full_attention_interval", 4);
        if (iv < 1 || std::fmod(L0, iv) != 0) raise(errc::unsupported, "qwen35: {} layers is not a multiple of full_attention_interval {}", L0, iv);
        if (!moe && Kor("expert_count", 0) > 0) raise(errc::unsupported, "qwen35: expert_count set on a dense architecture (expected qwen35moe)");
        lk_heads = i64(K("ssm.group_count")); lk_dim = i64(K("ssm.state_size")); lv_heads = i64(K("ssm.time_step_rank"));
        if (lk_heads <= 0 || lv_heads <= 0 || lv_heads % lk_heads) raise(errc::format_error, "qwen35: bad ssm head counts {} / {}", lk_heads, lv_heads);
        lv_dim = i64(K("ssm.inner_size")) / lv_heads;
        double hd = Kor("attention.key_length", hidden / heads), rot = Kor("rope.dimension_count", hd);
        if (rot >= hd) raise(errc::unsupported, "qwen35: full-width RoPE is not supported by the template");
        auto* vr = g.get("qwen35.ssm.v_head_reordered");
        v_tiled = vr && (vr->is_bool() ? vr->b() : vr->num() != 0) && lv_heads != lk_heads;
        cfg.emplace_back("rotary_dim", rot); cfg.emplace_back("full_attention_interval", iv); cfg.emplace_back("n_blocks", L0 / iv);
        cfg.emplace_back("linear_num_key_heads", double(lk_heads)); cfg.emplace_back("linear_key_head_dim", double(lk_dim));
        cfg.emplace_back("linear_num_value_heads", double(lv_heads)); cfg.emplace_back("linear_value_head_dim", double(lv_dim));
        cfg.emplace_back("linear_conv_kernel_dim", Kor("ssm.conv_kernel", 4));
    }
    if (moe) {   // llama.cpp's qwen3moe / qwen35moe: softmax gating, weights renormalised over the k (norm_w = true)
        double ne = K("expert_count"), nk = K("expert_used_count"), ws = Kor("expert_weights_scale", 0);
        if (ne < 1 || nk < 1 || nk > ne) raise(errc::format_error, "{}: bad expert counts {} / {}", arch, nk, ne);
        if (Kor("expert_gating_func", 1) != 1) raise(errc::unsupported, "{}: only softmax expert gating is supported", arch);
        cfg.emplace_back("num_experts", ne); cfg.emplace_back("num_experts_per_tok", nk);
        cfg.emplace_back("moe_intermediate_size", K("expert_feed_forward_length"));
        cfg.emplace_back("norm_topk_prob", 1.0); cfg.emplace_back("routed_scaling_factor", ws > 0 ? ws : 1.0);
    }
    std::string tname = !opt.template_name.empty() ? opt.template_name : arch == "qwen3" ? "qwen3" : arch == "qwen3moe" ? "qwen3_moe"
                      : arch == "qwen35moe" ? "qwen3_5_moe" : q35 ? "qwen3_5" : "llama";
    auto bp_text = instantiate(read_file(template_dir() / (tname + ".fbp")), json::value(cfg));
    auto bp = modelfile::parse_blueprint(bp_text);

    // streamed: tensors go straight from the mapped GGUF to disk, so a 20 GB model never sits in memory twice
    modelfile::stream_writer w(out);
    expert_index xi;
    import_report rep{arch, tname, 0, 0};
    std::set<std::string> used;
    auto put = [&](const std::string& dst, const std::string& src) {
        used.insert(src);
        auto* t = g.find(src);
        if (!t) raise(errc::not_found, "{}: missing tensor {}", path, src);
        w.add_raw(dst, t->shape, gguf_file::codec_of(t->type) ? gguf_file::codec_of(t->type) : "?", g.bytes(*t));
        ++rep.tensors;
    };
    auto need = [&](const std::string& src) -> const gguf_file::tinfo& {
        auto* t = g.find(src);
        if (!t) raise(errc::not_found, "{}: missing tensor {}", path, src);
        used.insert(src);
        return *t;
    };
    // tiled -> grouped V-head order: grouped head k*per + v sits at tiled head v*lk_heads + k
    auto vhead = [&](i64 h) { i64 per = lv_heads / lk_heads; return (h % per) * lk_heads + h / per; };
    // copy src with the rows [row0, row0 + lv_heads*rows_per_head) put back in grouped V-head order (raw blocks)
    auto put_vrows = [&](const std::string& dst, const std::string& src, i64 row0, i64 rows_per_head) {
        auto& t = need(src);
        auto* codec = gguf_file::codec_of(t.type);
        auto b = g.bytes(t);
        if (!v_tiled) { w.add_raw(dst, t.shape, codec, b); ++rep.tensors; return; }
        i64 rows = t.shape.empty() ? 1 : t.shape[0], cols = t.shape.size() > 1 ? numel(t.shape) / rows : 1;
        if (t.shape.size() == 1) { rows = t.shape[0]; cols = 1; }
        auto* c = graph::codecs::find(codec);
        if (c->block > 1 && cols % i64(c->block)) raise(errc::unsupported, "tensor {}: rows are not whole {} blocks", src, codec);
        std::size_t rb = c->bytes(std::size_t(cols));
        if (rows < row0 + lv_heads * rows_per_head) raise(errc::shape_mismatch, "tensor {} has shape {}", src, shape_str(t.shape));
        std::vector<std::uint8_t> o(b.begin(), b.end());
        for (i64 h = 0; h < lv_heads; ++h)
            std::memcpy(o.data() + std::size_t(row0 + h * rows_per_head) * rb, b.data() + std::size_t(row0 + vhead(h) * rows_per_head) * rb,
                        rb * std::size_t(rows_per_head));
        w.add_raw(dst, t.shape, codec, o); ++rep.tensors;
    };
    // the out projection consumes V heads along its input (column) dimension: blocks straddle heads, so decode,
    // permute and re-encode (q8_0 for quantised sources, which keeps the extra rounding negligible)
    auto put_vcols = [&](const std::string& dst, const std::string& src) {
        auto& t = need(src);
        auto* codec = gguf_file::codec_of(t.type);
        if (!v_tiled) { w.add_raw(dst, t.shape, codec, g.bytes(t)); ++rep.tensors; return; }
        auto* c = graph::codecs::find(codec);
        if (t.shape.size() != 2 || t.shape[1] != lv_heads * lv_dim) raise(errc::shape_mismatch, "tensor {} has shape {}", src, shape_str(t.shape));
        std::size_t n = std::size_t(numel(t.shape));
        std::vector<float> x(n), y(n);
        c->decode(g.bytes(t).data(), n, x.data());
        for (i64 r = 0; r < t.shape[0]; ++r)
            for (i64 h = 0; h < lv_heads; ++h)
                std::memcpy(&y[std::size_t(r * t.shape[1] + h * lv_dim)], &x[std::size_t(r * t.shape[1] + vhead(h) * lv_dim)], std::size_t(lv_dim) * 4);
        std::string out_codec = c->block > 1 ? "q8_0" : std::string(codec);
        w.add_tensor(dst, t.shape, y, out_codec); ++rep.tensors;
    };
    // MoE FFN: the stacked *_exps tensors ([n_experts, rows, k]; one expert = a contiguous run of whole rows) are copied
    // verbatim, each starting on a 4 KiB file offset and listed in the "experts" section; plus the router and, for
    // qwen35moe, the sigmoid-gated shared expert.
    auto put_moe = [&](i64 l, const std::string& P, const std::string& Q) {
        if (g.find(P + "ffn_gate_up_exps.weight")) raise(errc::unsupported, "{}: fused ffn_gate_up_exps tensors are not supported", path);
        put(Q + "router", P + "ffn_gate_inp.weight");
        const char* proj[3][2] = {{"gate", "ffn_gate_exps.weight"}, {"up", "ffn_up_exps.weight"}, {"down", "ffn_down_exps.weight"}};
        for (auto& pj : proj) {
            auto& t = need(P + pj[1]);
            if (t.shape.size() != 3) raise(errc::format_error, "{}: {} has shape {}", path, t.name, shape_str(t.shape));
            w.add_raw(Q + "experts.w_" + pj[0], t.shape, gguf_file::codec_of(t.type) ? gguf_file::codec_of(t.type) : "?", g.bytes(t), modelfile::expert_alignment);
            xi.add(w.last(), l, pj[0]); ++rep.tensors;
        }
        if (g.find(P + "ffn_up_shexp.weight")) {
            put(Q + "shared.w_gate", P + "ffn_gate_shexp.weight"); put(Q + "shared.w_up", P + "ffn_up_shexp.weight");
            put(Q + "shared.w_down", P + "ffn_down_shexp.weight");
            auto& gt = need(P + "ffn_gate_inp_shexp.weight");   // [d] -> a one-row linear [1, d]
            shape_t gs = gt.shape; if (gs.size() == 1) gs = {1, gs[0]};
            w.add_raw(Q + "shared.gate", gs, gguf_file::codec_of(gt.type) ? gguf_file::codec_of(gt.type) : "?", g.bytes(gt)); ++rep.tensors;
        } else if (q35) raise(errc::not_found, "{}: layer {} has no shared expert (ffn_up_shexp)", path, l);
    };
    put("tok_emb", "token_embd.weight");
    for (i64 l = 0, L = i64(bp.param("n_layers")); l < L; ++l) {
        auto P = std::format("blk.{}.", l), Q = std::format("layer.{}.", l);
        if (q35) {
            put(Q + "attn_norm", P + "attn_norm.weight"); put(Q + "ffn_norm", P + "post_attention_norm.weight");
            if (moe) put_moe(l, P, Q);
            else { put(Q + "w_gate", P + "ffn_gate.weight"); put(Q + "w_up", P + "ffn_up.weight"); put(Q + "w_down", P + "ffn_down.weight"); }
            if (g.find(P + "attn_qkv.weight")) {   // Gated DeltaNet layer
                put_vrows(Q + "w_qkv", P + "attn_qkv.weight", 2 * lk_heads * lk_dim, lv_dim);
                put_vrows(Q + "w_z", P + "attn_gate.weight", 0, lv_dim);
                put_vrows(Q + "w_a", P + "ssm_alpha.weight", 0, 1);
                put_vrows(Q + "w_b", P + "ssm_beta.weight", 0, 1);
                put_vrows(Q + "conv_w", P + "ssm_conv1d.weight", 2 * lk_heads * lk_dim, lv_dim);
                put_vrows(Q + "dt_bias", g.find(P + "ssm_dt.bias") ? P + "ssm_dt.bias" : P + "ssm_dt", 0, 1);
                put_vrows(Q + "a_neg", P + "ssm_a", 0, 1);
                put(Q + "ssm_norm", P + "ssm_norm.weight");
                put_vcols(Q + "wo", P + "ssm_out.weight");
            } else {                                // gated full-attention layer
                put(Q + "wq", P + "attn_q.weight"); put(Q + "q_norm", P + "attn_q_norm.weight");
                put(Q + "wk", P + "attn_k.weight"); put(Q + "k_norm", P + "attn_k_norm.weight");
                put(Q + "wv", P + "attn_v.weight"); put(Q + "wo", P + "attn_output.weight");
            }
            continue;
        }
        put(Q + "attn_norm", P + "attn_norm.weight"); put(Q + "ffn_norm", P + "ffn_norm.weight");
        put(Q + "wq", P + "attn_q.weight"); put(Q + "wk", P + "attn_k.weight"); put(Q + "wv", P + "attn_v.weight");
        if (tname == "qwen3" || arch == "qwen3moe") { put(Q + "q_norm", P + "attn_q_norm.weight"); put(Q + "k_norm", P + "attn_k_norm.weight"); }
        put(Q + "wo", P + "attn_output.weight");
        if (moe) { put_moe(l, P, Q); continue; }
        put(Q + "w_gate", P + "ffn_gate.weight");
        put(Q + "w_up", P + "ffn_up.weight"); put(Q + "w_down", P + "ffn_down.weight");
    }
    put("out_norm", "output_norm.weight");
    put("lm_head", g.find("output.weight") ? "output.weight" : "token_embd.weight");

    // vision tower and multi-token-prediction heads are not part of the text model: never copied
    auto skipped = [&](const std::string& n) { return q35 && (n.starts_with("v.") || n.starts_with("mm.") || n.starts_with("mtp.")); };
    for (auto& t : g.tensors())  // keep everything else (rope freqs, extra heads, ...) addressable
        if (!used.count(t.name) && !skipped(t.name) && gguf_file::codec_of(t.type)) { w.add_raw("extra." + t.name, t.shape, gguf_file::codec_of(t.type), g.bytes(t)); ++rep.tensors; }

    // tokenizer.json synthesised from the GGUF vocabulary (gpt2-style byte-level BPE)
    auto model = g.get("tokenizer.ggml.model") ? g.need("tokenizer.ggml.model").str() : std::string("gpt2");
    if (model == "gpt2") {
        json::object vocab; json::array added;
        const json::array* types = g.get("tokenizer.ggml.token_type") ? &g.need("tokenizer.ggml.token_type").arr() : nullptr;
        for (std::size_t i = 0; i < toks.size(); ++i) {
            vocab.emplace_back(toks[i].str(), double(i));
            if (types && (*types)[i].num() == 3) added.push_back(json::object{{"id", double(i)}, {"content", toks[i].str()}});
        }
        json::array merges; if (auto* m = g.get("tokenizer.ggml.merges")) merges = m->arr();
        json::object tj{{"added_tokens", added}, {"model", json::object{{"type", "BPE"}, {"vocab", vocab}, {"merges", merges}}}};
        w.set_section("tokenizer", json::dump(json::value(tj)));
    }
    auto num_or = [&](const char* k, double d) { auto* v = g.get(k); return v ? v->num() : d; };
    json::object man{{"format", "fpk"}, {"forge_format_version", double(modelfile::format_version)}, {"architecture", arch},
                     {"source", "gguf"}, {"template", tname}, {"session", bp.session},
                     {"bos_token_id", num_or("tokenizer.ggml.bos_token_id", -1)}, {"eos_token_id", num_or("tokenizer.ggml.eos_token_id", -1)},
                     {"max_position_embeddings", Kor("context_length", 2048)}};
    // Whether prompts start with BOS: GGUF says so explicitly (Qwen-family files set it to false).
    if (auto* ab = g.get("tokenizer.ggml.add_bos_token"); ab && ab->is_bool()) man.emplace_back("add_bos", ab->b());
    if (!xi.empty()) { man.emplace_back("forge_format_minor", double(modelfile::format_minor)); w.set_section("experts", xi.dump()); }
    w.set_manifest(json::value(man));
    w.set_section("blueprint", bp_text);
    w.finish();
    rep.bytes = std::filesystem::file_size(out);
    return rep;
}

} // namespace forge::import
