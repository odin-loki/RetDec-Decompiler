#pragma once
// Generic inference: the .fpk's blueprint declares the session kind; the engine attaches services.
//
// model   : the immutable, shareable part (mapped file, blueprint, tokenizer, decoded weights, plans).
// session : one sequence's mutable state (KV state store, position). Many sessions may share a model;
//           runs are serialised on the model (plans own a scratch arena), state is per session.
#include <forge/graph/graph.hpp>
#include <forge/inference/sampler.hpp>
#include <forge/modelfile/blueprint.hpp>
#include <forge/modelfile/fpk.hpp>
#include <forge/modelfile/sign.hpp>
#include <forge/modelfile/tokenizer.hpp>
#include <algorithm>
#include <forge/security/enforce.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace forge::inference {

struct session_options {
    i64 context = 0;       // 0 = manifest max_position_embeddings
    bool verify = false;   // check BLAKE3 hashes on open
    i64 page_rows = 256;   // KV page size in rows; 0 = reserve the whole context up front
    std::string kv_codec;  // "", "f32", "int8", "fp8", "tbq4", "tbq3" or "k8v4"; empty = $FORGE_KV_CACHE_TYPE, $FORGE_KV_CODEC or f32
    int kv_offload = -1;   // 1 = keep KV on the host, drop device copies after each attention; -1 = $FORGE_KV_OFFLOAD
};

struct generate_stats { std::size_t prompt_tokens = 0, new_tokens = 0; double prefill_s = 0, decode_s = 0; };

// Whether prompts start with BOS: the manifest's "add_bos" (recorded by importers from GGUF
// tokenizer.ggml.add_bos_token / HF tokenizer_config.json). Files imported before that key existed: Qwen-family
// models never add BOS (their bos_token_id is <|endoftext|>, which derails generation); others keep BOS.
inline bool add_bos_default(const json::value& man) {
    if (auto* ab = man.find("add_bos"); ab && ab->is_bool()) return ab->b();
    return man.str_or("architecture", "").rfind("qwen", 0) != 0;
}

class model {
    // FSS CAP-1 (docs/security-api.md): with an active forge::security policy the model file must be readable under
    // policy fs.read before it is opened. No active policy: nothing is checked (and nothing changes).
    static const std::string& admit(const std::string& path) {
        if (auto p = security::active_policy(); p->active()) security::enforce(*p, security::capability::fs_read, path);
        return path;
    }
public:
    explicit model(const std::string& fpk_path, bool verify = false) : r_(admit(fpk_path)) {
        // Strict mode (FORGE_PLAN: "verify before load; refuse unsigned files"): FORGE_REQUIRE_SIGNED=1
        // and/or FORGE_TRUSTED_KEYS=<hex pubkey>[,<hex pubkey>...] require a valid (trusted) signature.
        std::vector<std::string> trusted;
        if (auto* e = std::getenv("FORGE_TRUSTED_KEYS")) {
            std::string s = e;
            for (std::size_t p = 0, q; p < s.size(); p = q + 1) { q = s.find(',', p); if (q == std::string::npos) q = s.size(); if (q > p) trusted.push_back(s.substr(p, q - p)); }
        }
        bool strict = !trusted.empty() || (std::getenv("FORGE_REQUIRE_SIGNED") && std::string(std::getenv("FORGE_REQUIRE_SIGNED")) == "1");
        // FSS INT-1 / INT-4: an active policy with integrity.models "required" or "optional" decides here, before the
        // blueprint is parsed or any tensor is decoded (the file is only mapped and hashed). Its trusted_keys join
        // FORGE_TRUSTED_KEYS (which lockdown() also sets); failures are security::integrity_error, audited.
        const auto pol = security::active_policy();
        const bool fss = pol->active() && pol->integrity.models != security::requirement::off;
        if (fss) {
            for (auto k : pol->integrity.trusted_keys) { for (auto& c : k) c = char(std::tolower(static_cast<unsigned char>(c))); trusted.push_back(std::move(k)); }
            if (pol->integrity.models == security::requirement::required) strict = true;
            const bool check = strict || r_.has_section("signature");
            auto fail = [&](const std::string& why) {
                security::audit("integrity", security::decision::fail, "INT-4", fpk_path, why);
                throw security::integrity_error(fpk_path + ": " + why, "INT-4");
            };
            if (check || verify) if (auto bad = r_.verify(); !bad.empty()) fail("hash mismatch in " + bad.front());
            if (check) {
                auto rep = modelfile::verify_signature(r_, trusted);
                if (!rep.trusted) fail("signature check failed (integrity.models " + std::string(security::name(pol->integrity.models)) + "): " + rep.message);
                security::audit("integrity", security::decision::allow, "INT-1", fpk_path, "model signature verified (key " + rep.valid_keys.front() + ")");
            } else security::audit("integrity", security::decision::weaken, "INT-1", fpk_path, "unsigned model loaded (integrity.models optional)");
        }
        if (!fss && (verify || strict)) {
            auto bad = r_.verify();
            if (!bad.empty()) raise(errc::integrity_error, "{}: hash mismatch in {}", fpk_path, bad.front());
        }
        if (!fss && (strict || (verify && r_.has_section("signature")))) {
            auto rep = modelfile::verify_signature(r_, trusted);
            if (!rep.trusted && (strict || rep.signed_)) raise(errc::integrity_error, "{}: signature check failed: {}", fpk_path, rep.message);
        }
        man_ = r_.manifest();
        bp_ = modelfile::parse_blueprint(r_.section_text("blueprint"));
        if (r_.has_section("tokenizer")) {
            tok_.emplace(r_.section_text("tokenizer"));
            tok_->set_special(int(man_.num_or("bos_token_id", -1)), int(man_.num_or("eos_token_id", -1)));
            tok_->set_add_bos(add_bos_default(man_));
        }
        vocab_ = i64(bp_.param_or("vocab", 0));
        if (auto* d = std::getenv("FORGE_DEVICE")) set_device(d);
    }
    // Select the execution device ("cpu", "vulkan"). Unavailable devices fall back to the CPU with a warning.
    // Returns the device actually used. Clears cached plans.
    std::string set_device(const std::string& dev) {
        std::lock_guard lk(run_mu_);
        std::string why;
        reg_ = graph::device_registry(dev, &why);
        if (!reg_ && !why.empty()) log::warn("{}; using cpu", why);
        device_ = reg_ ? dev : "cpu";
        cache_.clear();
        return device_;
    }
    const std::string& device() const { return device_; }
    static std::shared_ptr<model> open(const std::string& p, bool verify = false) { return std::make_shared<model>(p, verify); }

    const modelfile::blueprint& blueprint() const { return bp_; }
    const json::value& manifest() const { return man_; }
    const modelfile::reader& file() const { return r_; }
    const modelfile::bpe_tokenizer* tokenizer() const { return tok_ ? &*tok_ : nullptr; }
    i64 vocab() const { return vocab_; }
    i64 max_context() const { return i64(man_.num_or("max_position_embeddings", 2048)); }
    graph::plan_cache& plans() { return cache_; }
    std::mutex& run_mutex() { return run_mu_; }

    // LoRA adapters: W' = W + (alpha / r) * B @ A for each named weight (A: [r, in], B: [out, r], PEFT layout).
    // Only the targeted matrices are materialised per adapter; base weights stay mapped and shared, and each
    // adapter gets its own plans, so sessions switch adapters per request without reloading anything.
    struct lora { tensor a, b; };
    void add_adapter(const std::string& name, const std::map<std::string, lora>& deltas, float alpha) {
        if (name.empty() || adapters_.count(name)) raise(errc::invalid_argument, "adapter: empty or duplicate name '{}'", name);
        std::map<std::string, tensor> merged;
        for (auto& [w, d] : deltas) {
            tensor base = r_.load(w);
            if (d.a.shape.size() != 2 || d.b.shape.size() != 2 || base.shape.size() != 2 || d.b.shape[1] != d.a.shape[0] ||
                d.b.shape[0] != base.shape[0] || d.a.shape[1] != base.shape[1])
                raise(errc::invalid_argument, "adapter {}: shape mismatch for {}", name, w);
            i64 r = d.a.shape[0], O = base.shape[0], I = base.shape[1]; float sc = alpha / float(r);
            for (i64 o = 0; o < O; ++o) for (i64 k = 0; k < r; ++k) {
                float bk = sc * d.b.data[std::size_t(o * r + k)];
                for (i64 i = 0; i < I; ++i) base.data[std::size_t(o * I + i)] += bk * d.a.data[std::size_t(k * I + i)];
            }
            merged.emplace(w, std::move(base));
        }
        std::lock_guard lk(w_mu_);
        // Plans (possibly held by live sessions) point at the merged data, so an adapter name is immutable.
        if (!adapters_.emplace(name, std::move(merged)).second) raise(errc::invalid_argument, "adapter '{}' already loaded", name);
    }
    // Load an adapter from a .fpk holding "<weight>.lora_a" / "<weight>.lora_b" tensors; alpha from the manifest.
    void add_adapter_file(const std::string& name, const std::string& path) {
        modelfile::reader ar(path); std::map<std::string, lora> d;
        for (auto& [n, e] : ar.tensors()) {
            auto cut = n.rfind(".lora_"); if (cut == std::string::npos) continue;
            auto& l = d[n.substr(0, cut)]; (n.substr(cut) == ".lora_a" ? l.a : l.b) = ar.load(n);
        }
        add_adapter(name, d, float(ar.manifest().num_or("lora_alpha", 1)));
    }
    bool has_adapter(const std::string& n) const { return n.empty() || adapters_.count(n); }
    std::size_t loaded_weights() const { return weights_.size(); }

    // plan for `n` new tokens; all_rows keeps logits for every position
    graph::plan& plan_for(i64 n, bool all_rows, graph::state_store* st, const std::string& adapter = {}) {
        return cache_.get(std::format("{}:{}:{}:{}", all_rows ? "all" : "seq", n, adapter, st ? graph::kv_codec_pair_name({st->codec, st->codec_v == graph::kv_codec::f32 ? st->codec : st->codec_v}) : "f32"), [&] {
            modelfile::build_options bo;
            bo.dims["seq"] = n;
            bo.all_rows = all_rows;
            bo.weight = [&](const std::string& name) -> std::pair<shape_t, const float*> {
                std::lock_guard lk(w_mu_);
                if (!adapter.empty()) {
                    auto& m = adapters_.at(adapter);
                    if (auto a = m.find(name); a != m.end()) return {a->second.shape, a->second.data.data()};
                }
                auto it = weights_.find(name);
                if (it == weights_.end()) it = weights_.emplace(name, r_.load(name)).first;
                return {it->second.shape, it->second.data.data()};
            };
            // Weights stay in the mapped file: f32 tensors are used in place, other codecs are read in place by
            // quantised matmul (the plan decodes a shared f32 copy only for other consumers). FORGE_DECODE_WEIGHTS=1
            // restores the old behaviour (every weight decoded to f32 up front).
            static const bool decode_all = [] { auto* e = std::getenv("FORGE_DECODE_WEIGHTS"); return e && std::string(e) == "1"; }();
            if (!decode_all) bo.qweight = [&](const std::string& name) -> modelfile::build_options::encoded {
                if (!adapter.empty() && adapters_.at(adapter).count(name)) return {};
                const auto& e = r_.entry(name);
                if (e.codec == "f32") return {};  // handled by `weight` (zero-copy below)
                return {e.shape, r_.section_bytes("data").data() + e.offset, e.codec};
            };
            if (!decode_all) bo.weight = [&, inner = bo.weight](const std::string& name) -> std::pair<shape_t, const float*> {
                const auto& e = r_.entry(name);
                bool adapted = !adapter.empty() && adapters_.at(adapter).count(name);
                const auto* base = r_.section_bytes("data").data() + e.offset;
                if (!adapted && e.codec == "f32" && reinterpret_cast<std::uintptr_t>(base) % alignof(float) == 0)
                    return {e.shape, reinterpret_cast<const float*>(base)};
                return inner(name);
            };
            graph::pass_options po; po.lowerings = reg_.get();
            return graph::compile(modelfile::build_graph(bp_, bo), st, po);
        });
    }
private:
    modelfile::reader r_;
    json::value man_;
    modelfile::blueprint bp_;
    std::optional<modelfile::bpe_tokenizer> tok_;
    graph::plan_cache cache_;
    std::map<std::string, tensor> weights_;
    std::map<std::string, std::map<std::string, tensor>> adapters_;
    std::mutex w_mu_, run_mu_;
    std::unique_ptr<graph::registry> reg_;
    std::string device_ = "cpu";
    i64 vocab_ = 0;
};

// Header of session::save_prefix blobs (KV state of a token prefix).
inline constexpr char kv_blob_magic[9] = "FORGEKV1";

class session {
public:
    explicit session(const std::string& fpk_path, session_options o = {}) : session(model::open(fpk_path, o.verify), o) {}
    explicit session(std::shared_ptr<model> m, session_options o = {}) : m_(std::move(m)), opt_(o) {
        st_.capacity = o.context ? o.context : m_->max_context();
        st_.page_rows = o.page_rows;
        std::string kc = o.kv_codec;
        if (kc.empty()) if (const char* e = std::getenv("FORGE_KV_CACHE_TYPE")) kc = e;
        if (kc.empty()) if (const char* e = std::getenv("FORGE_KV_CODEC")) kc = e;
        auto kp = graph::kv_codec_pair_from_name(kc);
        st_.codec = kp.k; st_.codec_v = kp.v != kp.k ? kp.v : graph::kv_codec::f32;
        int off = o.kv_offload;
        if (off < 0) { const char* e = std::getenv("FORGE_KV_OFFLOAD"); off = e && *e && *e != '0'; }
        st_.offload = off != 0;
    }
    const std::shared_ptr<model>& shared_model() const { return m_; }
    const modelfile::blueprint& blueprint() const { return m_->blueprint(); }
    const json::value& manifest() const { return m_->manifest(); }
    const modelfile::reader& file() const { return m_->file(); }
    const modelfile::bpe_tokenizer* tokenizer() const { return m_->tokenizer(); }
    i64 vocab() const { return m_->vocab(); }
    i64 position() const { return pos_; }
    i64 context() const { return st_.capacity; }
    const graph::state_store& state() const { return st_; }
    void reset() { st_.reset(); pos_ = 0; }
    // Select a LoRA adapter registered on the model ("" = base). The KV cache is cleared, since cached
    // keys/values were computed with the previous weights.
    void set_adapter(const std::string& a) {
        if (!m_->has_adapter(a)) raise(errc::not_found, "adapter '{}' not loaded", a);
        if (a != adapter_) { adapter_ = a; execs_.clear(); reset(); }
    }
    const std::string& adapter() const { return adapter_; }
    // roll back to `pos` tokens (speculative rejection, prefix reuse). Recurrent state (e.g. Qwen3.5 DeltaNet,
    // conv windows) cannot be cut: this raises for such models unless pos is 0; use checkpoint() + rewind(pos, ck, tokens).
    void rewind(i64 pos) { if (pos < pos_) { st_.truncate(pos); pos_ = pos; } }
    // True when the state holds recurrent slots, which only make sense at their full length.
    bool has_recurrent_state() const { for (auto& [k, s] : st_.slots) if (s.fixed) return true; return false; }
    // Whether the first `n` tokens of this session can be shared (copy_prefix / save_prefix) or rewound to.
    bool can_share_prefix(i64 n) const {
        if (n > pos_) return false;
        for (auto& [k, s] : st_.slots) if (s.fixed && n != s.len) return false;
        return true;
    }
    // A snapshot of the recurrent slots at the current position (empty for pure KV-cache models), so a later
    // rewind can restore them and replay the tokens since, instead of cutting a state that cannot be cut.
    struct checkpoint { i64 pos = 0; std::map<std::string, graph::state_store::slot> fixed; };
    checkpoint make_checkpoint() const {
        checkpoint c; c.pos = pos_;
        if (!has_recurrent_state()) return c;
        st_.flush();   // device-resident rows back to the host copy
        for (auto& [k, s] : st_.slots) if (s.fixed) c.fixed.emplace(k, s);
        return c;
    }
    // Roll back to `pos` (ck.pos <= pos <= position()), where `since` holds the tokens fed after the checkpoint.
    // KV-only models just truncate; recurrent slots are restored from `ck` and since[0 .. pos - ck.pos) replayed.
    void rewind(i64 pos, const checkpoint& ck, std::span<const int> since) {
        if (pos >= pos_) return;
        if (ck.fixed.empty()) { rewind(pos); return; }
        if (pos < ck.pos || pos - ck.pos > i64(since.size())) raise(errc::invalid_argument, "rewind: position {} is outside the checkpoint range", pos);
        st_.forget();   // device copies are stale after the restore
        for (auto& [k, s] : ck.fixed) st_.slots[k] = s;
        st_.truncate(ck.pos); pos_ = ck.pos;
        if (pos > ck.pos) forward(since.subspan(0, std::size_t(pos - ck.pos)));
    }
    // ---- speculative verify with cheap recurrent rollback (used by speculative.hpp) ----
    // forward_spec(ids, keep) is forward_all(ids); on a model with recurrent state it also sets that state aside
    // after the first `keep` rows, inside the same pass (a device-to-device copy on GPU backends: no host transfer,
    // no extra pass). spec_rewind(pos) then rolls back to any pos in [mark, position()], mark = position before the
    // call + keep: KV-only models are cut at pos and it returns 0; recurrent models go back to the mark and it
    // returns pos - mark, the number of tokens (ids[keep ...]) the caller must feed again, e.g. at the head of its
    // next pass (they cost a few extra rows there, not a pass of their own).
    // The returned rows ([ids.size(), vocab]) are the executor's output buffer: valid until the next forward.
    std::span<const float> forward_spec(std::span<const int> ids, i64 keep) {
        keep = std::clamp<i64>(keep, 0, i64(ids.size()));
        spec_mark_ = pos_ + keep;
        if (has_recurrent_state()) st_.snap_row = keep;
        struct reset_snap { graph::state_store& s; ~reset_snap() { s.snap_row = -1; } } g{st_};
        return run_view(ids, true);
    }
    i64 spec_rewind(i64 pos) {
        if (pos >= pos_) return 0;
        if (!has_recurrent_state()) { rewind(pos); return 0; }
        if (pos < spec_mark_ || spec_mark_ > pos_ || !st_.restore_snapshots())
            raise(errc::invalid_argument, "spec_rewind: position {} is outside the last forward_spec range", pos);
        st_.truncate(spec_mark_); pos_ = spec_mark_;
        return pos - spec_mark_;
    }
    // plain-greedy step: feed `ids`, return the argmax of the last position's raw logits (on the device when it can)
    int forward_argmax(std::span<const int> ids) { return run_greedy(ids); }
    // ---- end speculative ----
    // copy the first `n` tokens of another session's state (prefix sharing across sequences)
    void copy_prefix(const session& o, i64 n) {
        if (o.m_ != m_) raise(errc::invalid_argument, "copy_prefix: sessions use different models");
        n = std::min(n, o.pos_);
        if (o.st_.codec != st_.codec) raise(errc::invalid_argument, "copy_prefix: sessions use different KV codecs");
        o.st_.flush(); st_.forget();
        st_.reset();
        for (auto& [k, s] : o.st_.slots) {
            auto& d = st_.slots[k];
            if (s.fixed) {   // recurrent state: only a full-length copy is meaningful
                if (n != s.len) raise(errc::unsupported, "copy_prefix: recurrent slot '{}' holds {} tokens, cannot share {}", k, s.len, n);
                d = s; continue;
            }
            if (!d.capacity) d.codec = s.codec;
            shape_t row = s.buf.shape;
            st_.ensure(d, row, std::max<i64>(n, 1));
            i64 rw = numel(s.buf.shape) / std::max<i64>(s.capacity, 1);
            std::copy_n(s.buf.data.data(), std::size_t(rw * n), d.buf.data.data());
            d.len = n;
        }
        pos_ = n;
    }
    // Serialise the KV state of the first `n` tokens (all, by default) to a blob, e.g. for mem::spill_store.
    // load_prefix() restores it into a session of the same model and KV codec: afterwards the session continues
    // exactly as copy_prefix() from the original would (identical logits).
    std::vector<std::byte> save_prefix(i64 n = -1) const {
        n = n < 0 ? pos_ : std::min(n, pos_);
        st_.flush();   // device-resident rows back to the host copy
        std::vector<std::byte> b;
        auto put = [&](const void* p, std::size_t k) { auto* c = static_cast<const std::byte*>(p); b.insert(b.end(), c, c + k); };
        auto u64 = [&](std::uint64_t v) { put(&v, 8); };
        put(kv_blob_magic, 8); u64(std::uint64_t(n)); u64(std::uint64_t(st_.codec)); u64(st_.slots.size());
        for (auto& [k, s] : st_.slots) {
            if (s.fixed && n != s.len) raise(errc::unsupported, "save_prefix: recurrent slot '{}' holds {} tokens, cannot save {}", k, s.len, n);
            u64(k.size()); put(k.data(), k.size());
            u64(s.fixed); u64(std::uint64_t(s.codec)); u64(std::uint64_t(s.len)); u64(std::uint64_t(s.capacity)); u64(s.buf.shape.size());
            for (auto d : s.buf.shape) u64(std::uint64_t(d));
            std::size_t nf = s.fixed ? s.buf.data.size() : std::size_t(numel(s.buf.shape) / std::max<i64>(s.capacity, 1) * n);
            u64(nf); put(s.buf.data.data(), nf * sizeof(float));
        }
        return b;
    }
    void load_prefix(std::span<const std::byte> b) {
        std::size_t at = 0;
        auto get = [&](void* p, std::size_t k) { if (at + k > b.size()) raise(errc::parse_error, "load_prefix: truncated KV blob"); std::memcpy(p, b.data() + at, k); at += k; };
        auto u64 = [&] { std::uint64_t v; get(&v, 8); return v; };
        char magic[8]; get(magic, 8);
        if (std::memcmp(magic, kv_blob_magic, 8)) raise(errc::parse_error, "load_prefix: not a KV blob");
        i64 n = i64(u64());
        if (graph::kv_codec(u64()) != st_.codec) raise(errc::invalid_argument, "load_prefix: the blob uses a different KV codec");
        if (n > st_.capacity) raise(errc::out_of_memory, "load_prefix: {} tokens exceed the context {}", n, st_.capacity);
        st_.forget(); st_.reset();
        for (std::uint64_t i = 0, ns = u64(); i < ns; ++i) {
            std::string k(u64(), '\0'); get(k.data(), k.size());
            bool fixed = u64() != 0; auto codec = graph::kv_codec(u64()); i64 len = i64(u64()), cap = i64(u64());
            shape_t sh(u64()); for (auto& d : sh) d = i64(u64());
            std::size_t nf = u64();
            auto& d = st_.slots[k];
            if (fixed) { d = {}; d.buf = tensor(sh); d.len = len; d.capacity = cap; d.codec = codec; d.fixed = true; }
            else { if (!d.capacity) d.codec = codec; st_.ensure(d, sh, std::max<i64>(n, 1)); d.len = n; }
            if (nf > d.buf.data.size()) raise(errc::parse_error, "load_prefix: slot '{}' does not fit its shape", k);
            get(d.buf.data.data(), nf * sizeof(float));
        }
        pos_ = n;
    }
    graph::plan_cache& plans() { return m_->plans(); }

    // Runs the graph on `ids` continuing from the current position; returns logits of the last position.
    std::vector<float> forward(std::span<const int> ids) { return run(ids, false); }
    // Same, but returns logits for every input position ([ids.size(), vocab] row-major).
    std::vector<float> forward_all(std::span<const int> ids) { return run(ids, true); }

    // generative_sequence: prefill + decode loop with a sampler chain. `on_token` returning false stops.
    std::vector<int> generate(std::span<const int> prompt, int max_new, sampler& s,
                              const std::function<bool(int)>& on_token = {}, generate_stats* stats = nullptr, bool stop_at_eos = true) {
        check_generative();
        std::vector<int> hist(prompt.begin(), prompt.end()), out;
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        // Plain greedy: the device picks the token and only its index comes back (no logits download per token).
        const bool greedy = s.plain_greedy();
        std::span<const float> logits; int next = -1;
        if (greedy) next = run_greedy(prompt); else logits = run_view(prompt, false);   // logits: the executor's output buffer, valid until the next run
        auto t1 = clk::now();
        int eos = tokenizer() ? tokenizer()->eos() : -1;
        for (int i = 0; i < max_new; ++i) {
            int t = greedy ? next : s.sample(logits, hist);
            out.push_back(t); hist.push_back(t);
            if (on_token && !on_token(t)) break;
            if (stop_at_eos && t == eos) break;
            if (i + 1 == max_new || pos_ >= st_.capacity) break;
            if (greedy) next = run_greedy(std::span<const int>(&t, 1)); else logits = run_view(std::span<const int>(&t, 1), false);
        }
        auto t2 = clk::now();
        if (stats) *stats = {prompt.size(), out.size(), std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count()};
        return out;
    }
    void check_generative() const {
        if (blueprint().session != "generative_sequence") raise(errc::unsupported, "session kind '{}' cannot generate", blueprint().session);
    }

    // one_shot graphs (e.g. imported ONNX): bind named f32 inputs, run once, return every declared output.
    std::map<std::string, std::vector<float>> infer(const std::map<std::string, std::vector<float>>& in, i64 seq = 1) {
        std::lock_guard lk(m_->run_mutex());
        auto& p = m_->plan_for(seq, false, &st_);
        auto it = execs_.find(&p);
        if (it == execs_.end()) it = execs_.emplace(&p, std::make_unique<graph::executor>(p, &st_)).first;
        auto& ex = *it->second;
        for (auto& d : blueprint().inputs)
            if (!in.count(d.name)) raise(errc::invalid_argument, "infer: missing input '{}'", d.name);
        for (auto& [k, v] : in) ex.set_input(k, v);
        ex.run();
        std::map<std::string, std::vector<float>> out;
        for (auto& o : blueprint().outputs) { auto v = ex.output_view(o.name); out[o.name].assign(v.data, v.data + v.size()); }
        return out;
    }
private:
    std::vector<float> run(std::span<const int> ids, bool all) { auto v = run_view(ids, all); return {v.begin(), v.end()}; }
    // Runs like run_view and returns the argmax of the last position's logits: on the device when the backend can
    // (one integer read back), else over the host view (ties: the lowest index, as std::max_element).
    int run_greedy(std::span<const int> ids) {
        greedy_next_ = true;
        auto v = run_view(ids, false);
        greedy_next_ = false;
        if (last_argmax_ >= 0) return int(last_argmax_);
        return int(std::max_element(v.begin(), v.end()) - v.begin());
    }
    std::span<const float> run_view(std::span<const int> ids, bool all) {
        if (ids.empty()) raise(errc::invalid_argument, "forward: no tokens");
        if (pos_ + i64(ids.size()) > st_.capacity) raise(errc::out_of_memory, "context {} exceeded", st_.capacity);
        i64 n = i64(ids.size());
        std::lock_guard lk(m_->run_mutex());
        auto& p = m_->plan_for(n, all, &st_, adapter_);
        auto it = execs_.find(&p);
        if (it == execs_.end()) it = execs_.emplace(&p, std::make_unique<graph::executor>(p, &st_)).first;
        auto& ex = *it->second;
        std::vector<float> tk(ids.begin(), ids.end()); std::vector<float> ps(static_cast<std::size_t>(n));
        for (i64 i = 0; i < n; ++i) ps[std::size_t(i)] = float(pos_ + i);
        ex.set_input("tokens", tk);
        ex.set_input("positions", ps);
        ex.want_argmax(greedy_next_);
        ex.run();
        last_argmax_ = greedy_next_ ? ex.last_argmax() : -1;
        pos_ += n;
        auto v = ex.output_view(blueprint().outputs.front().name);
        return {v.data, v.size()};
    }
    std::shared_ptr<model> m_;
    session_options opt_;
    graph::state_store st_;
    std::map<graph::plan*, std::unique_ptr<graph::executor>> execs_;
    i64 pos_ = 0;
    std::string adapter_;
    bool greedy_next_ = false; long long last_argmax_ = -1;
    i64 spec_mark_ = 0;   // forward_spec: position whose recurrent state was set aside
};

} // namespace forge::inference
