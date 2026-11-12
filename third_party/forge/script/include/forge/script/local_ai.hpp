#pragma once
// In-process model backend for ForgeScript's AI commands (forge::ask, forge::ask_json, ...; docs/scripting.md).
// Header-only so the forge_script library does not depend on the inference module: a host that links
// forge::inference includes this file and calls install_local_ai() once (apps/forge.cpp does). Scripts then use
// the model named by options::ask_model or $FORGE_ASK_MODEL instead of an HTTP endpoint.
//
// Each request opens a fresh session on a cached model (one per path, loaded on first use), formats the
// conversation the way forge-serve's /v1/chat/completions does ("system: ...\nuser: ...\nassistant:"), and decodes
// greedily up to max_tokens or end of sequence. json = constrain the reply to a JSON value with the grammar sampler
// (json_schema_to_gbnf of the empty schema); a reply cut off by max_tokens raises.
#include <forge/inference/grammar.hpp>
#include <forge/inference/json_schema.hpp>
#include <forge/inference/session.hpp>
#include <forge/script/script.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace forge::script {

inline std::shared_ptr<inference::model> local_ai_model(const std::string& path) {
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<inference::model>> cache;
    std::lock_guard lk(mu);
    auto& m = cache[path];
    if (!m) m = inference::model::open(path);
    return m;
}

inline std::string local_ai_generate(const local_ai_request& r) {
    auto m = local_ai_model(r.model);
    const auto* tok = m->tokenizer();
    if (!tok) throw std::runtime_error("the model has no tokenizer");
    std::string text;
    if (!r.system.empty()) text += "system: " + r.system + "\n";
    text += "user: " + r.prompt + "\nassistant:";
    std::vector<int> prompt = tok->encode(text);
    if (tok->add_bos()) prompt.insert(prompt.begin(), tok->bos());
    int max_new = r.max_tokens > 0 ? r.max_tokens : 256;
    inference::session_options so;
    so.context = std::min<i64>(m->max_context(), i64(prompt.size()) + max_new + 1);
    if (i64(prompt.size()) >= so.context) throw std::runtime_error("prompt (" + std::to_string(prompt.size()) + " tokens) does not fit the model's context");
    inference::session s(m, so);
    inference::sampler_config sc; sc.temperature = 0.f;   // greedy: the same prompt gives the same reply
    inference::sampler smp(sc);
    std::unique_ptr<inference::grammar_constraint> gc;
    if (r.json) {
        static const auto g = std::make_shared<const inference::grammar>(inference::json_schema_to_gbnf(json::value(json::object{})));
        gc = std::make_unique<inference::grammar_constraint>(g, *tok, int(m->vocab()), tok->eos());
        smp.filter = [&gc](std::span<float> l) { gc->apply(l); }; smp.allow = [&gc](int t) { return gc->allows(t); };
    }
    bool ended = false;
    auto out = s.generate(prompt, max_new, smp, [&](int t) {
        if (t == tok->eos()) { ended = true; return false; }
        if (gc && !gc->accept(t)) return false;
        if (gc && gc->finished()) { ended = true; return false; }   // a complete value: nothing else may follow
        return true;
    });
    if (!out.empty() && out.back() == tok->eos()) out.pop_back();
    if (gc && !ended && !gc->can_end()) throw std::runtime_error("the reply was cut off at " + std::to_string(max_new) + " tokens before the JSON value was complete (raise FORGE_ASK_MAX_TOKENS)");
    return tok->decode(out);
}

inline void install_local_ai() { set_local_ai(local_ai_generate); }

} // namespace forge::script
