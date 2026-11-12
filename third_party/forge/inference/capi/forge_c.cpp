#include <cstddef>
#include <forge.h>
#include <forge/inference/inference.hpp>
#include <cstring>

namespace { thread_local std::string g_err; }
struct forge_model { forge::inference::session s; explicit forge_model(const char* p, forge::inference::session_options o) : s(p, o) {} };

#define FORGE_TRY(fail) catch (const std::exception& e) { g_err = e.what(); return fail; } catch (...) { g_err = "unknown error"; return fail; }

extern "C" {
uint32_t forge_abi_version(void) { return FORGE_ABI_VERSION; }
const char* forge_version(void) { return "0.1.0"; }
const char* forge_last_error(void) { return g_err.c_str(); }

forge_model* forge_open(const char* path, int64_t ctx, int verify) {
    try { return new forge_model(path, {.context = ctx, .verify = verify != 0}); } FORGE_TRY(nullptr)
}
void forge_close(forge_model* m) { delete m; }
void forge_reset(forge_model* m) { if (m) m->s.reset(); }
int64_t forge_vocab_size(const forge_model* m) {
    try { return int64_t(m->s.blueprint().param("vocab")); } FORGE_TRY(-1)
}
int64_t forge_tokenize(const forge_model* m, const char* text, int add_bos, int32_t* out, int64_t cap) {
    try {
        auto* t = m->s.tokenizer();
        if (!t) { g_err = "model has no tokenizer"; return -1; }
        auto ids = t->encode(text);
        if (add_bos < 0 ? t->add_bos() : (add_bos && t->bos() >= 0)) ids.insert(ids.begin(), t->bos());   // -1: the model's default
        for (int64_t i = 0; i < int64_t(ids.size()) && i < cap; ++i) out[i] = ids[std::size_t(i)];
        return int64_t(ids.size());
    } FORGE_TRY(-1)
}
int64_t forge_detokenize(const forge_model* m, const int32_t* ids, int64_t n, char* out, int64_t cap) {
    try {
        auto* t = m->s.tokenizer();
        if (!t) { g_err = "model has no tokenizer"; return -1; }
        std::string s = t->decode(std::vector<int>(ids, ids + n));
        if (out && cap > 0) { auto k = std::min<std::size_t>(s.size(), std::size_t(cap - 1)); std::memcpy(out, s.data(), k); out[k] = 0; }
        return int64_t(s.size());
    } FORGE_TRY(-1)
}
int forge_forward(forge_model* m, const int32_t* ids, int64_t n, float* logits, int64_t cap) {
    try {
        auto l = m->s.forward(std::span<const int>(ids, std::size_t(n)));
        std::memcpy(logits, l.data(), std::min<std::size_t>(l.size(), std::size_t(cap)) * 4);
        return 0;
    } FORGE_TRY(-1)
}
int64_t forge_generate(forge_model* m, const int32_t* prompt, int64_t np, int32_t max_new, const forge_sampling* s,
                       int32_t* out, int64_t cap, forge_token_cb cb, void* user) {
    return forge_generate_ex(m, prompt, np, max_new, s, nullptr, out, cap, cb, user);
}
int64_t forge_position(const forge_model* m) { return m ? m->s.position() : -1; }
int forge_rewind(forge_model* m, int64_t p) { try { m->s.rewind(p); return 0; } FORGE_TRY(-1) }
int forge_forward_all(forge_model* m, const int32_t* ids, int64_t n, float* logits, int64_t cap) {
    try {
        auto l = m->s.forward_all(std::span<const int>(ids, std::size_t(n)));
        std::memcpy(logits, l.data(), std::min<std::size_t>(l.size(), std::size_t(cap)) * 4);
        return 0;
    } FORGE_TRY(-1)
}
int64_t forge_generate_ex(forge_model* m, const int32_t* prompt, int64_t np, int32_t max_new, const forge_sampling* s,
                          const forge_generate_options* o, int32_t* out, int64_t cap, forge_token_cb cb, void* user) {
    return forge_generate_ex2(m, prompt, np, max_new, s, o, nullptr, out, cap, cb, user);
}
int64_t forge_generate_ex2(forge_model* m, const int32_t* prompt, int64_t np, int32_t max_new, const forge_sampling* s,
                           const forge_generate_options* o, const forge_sampling_ext* ext, int32_t* out, int64_t cap,
                           forge_token_cb cb, void* user) {
    try {
        forge::inference::sampler_config c;
        if (s) { c.temperature = s->temperature; c.top_k = s->top_k; c.top_p = s->top_p > 0 ? s->top_p : 1.f; c.min_p = s->min_p;
                 c.repetition_penalty = s->repetition_penalty > 0 ? s->repetition_penalty : 1.f; c.seed = s->seed; }
        if (ext) {
            auto has = [&](std::size_t end) { return ext->size >= end; };
            if (has(offsetof(forge_sampling_ext, typical_p) + sizeof(float)) && ext->typical_p > 0) c.typical_p = ext->typical_p;
            if (has(offsetof(forge_sampling_ext, mirostat_tau) + sizeof(float))) c.mirostat_tau = ext->mirostat_tau;
            if (has(offsetof(forge_sampling_ext, mirostat_eta) + sizeof(float)) && ext->mirostat_eta > 0) c.mirostat_eta = ext->mirostat_eta;
        }
        forge::inference::sampler smp(c);
        auto* tok = m->s.tokenizer();
        std::unique_ptr<forge::inference::grammar_constraint> gc;
        if (o && o->grammar) {
            if (!tok) { g_err = "grammar needs a tokenizer"; return -1; }
            gc = std::make_unique<forge::inference::grammar_constraint>(std::make_shared<forge::inference::grammar>(o->grammar), *tok, int(m->s.vocab()), tok->eos());
            smp.filter = [&](std::span<float> l) { gc->apply(l); }; smp.allow = [&](int t) { return gc->allows(t); };
        }
        std::vector<int> r;
        auto on = [&](int t) {
            if (gc && !gc->accept(t)) return false;
            r.push_back(t);
            if (cb) { std::string txt = tok ? tok->decode_one(t) : std::string(); if (cb(t, txt.c_str(), user) != 0) return false; }
            return !(gc && gc->finished());
        };
        std::span<const int> pr(prompt, std::size_t(np));
        if (o && o->draft) forge::inference::speculative_generate(m->s, o->draft->s, pr, max_new, smp, o->spec_k > 0 ? o->spec_k : 4, on);
        else m->s.generate(pr, max_new, smp, on);
        for (int64_t i = 0; i < int64_t(r.size()) && i < cap; ++i) out[i] = r[std::size_t(i)];
        return int64_t(r.size());
    } FORGE_TRY(-1)
}
}
