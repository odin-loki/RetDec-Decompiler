#pragma once
// Speculative decoding. Drafts come from a cheap draft model (Leviathan et al. / Chen et al.) or from the text so far
// (prompt lookup / n-gram drafting); the target scores the current token and every draft in one pass.
//
// Exactness. One-hot drafts (n-gram drafts, greedy draft models) are verified by sample-and-compare: each row of
// the verify pass is sampled exactly as plain decoding would sample it (same sampler, same history, same random
// draws in the same order) and the draft is kept while it equals the sample. The output is then the target's own
// output: identical to plain decoding for greedy, and for a fixed seed whenever the pass's logits equal the
// single-token ones. Sampled draft models use modified rejection sampling, which keeps the target distribution.
//
// Draft length. k > 0 verifies min(k, available) drafts per pass; k = 0 lets a spec_planner choose from measured
// pass costs and acceptance (never slower than plain decoding by its measurements, with a periodic probe).
//
// Recurrent state (Qwen3.5 DeltaNet / conv windows). session::forward_spec sets that state aside inside the verify
// pass, right after the tokens that are certainly kept; a rejection restores it (one device-to-device copy) and the
// accepted drafts are fed again at the head of the next pass ("replay rows"), so no pass is ever repeated.
#include <forge/inference/session.hpp>
#include <forge/inference/spec_planner.hpp>
#include <array>
#include <unordered_map>

namespace forge::inference {

struct speculative_stats {
    std::size_t rounds = 0, drafted = 0, accepted = 0, new_tokens = 0;   // rounds: passes that verified drafts
    std::size_t passes = 0, replayed = 0, probes = 0;                   // all target passes; replay rows fed again
    std::array<std::size_t, 16> pos_tried{}, pos_won{};                 // per draft position
    double seconds = 0, prefill_seconds = 0;   // seconds: the whole call; prefill_seconds: the prompt (all but its last token)
    double decode_tps() const { return seconds > prefill_seconds ? double(new_tokens) / (seconds - prefill_seconds) : 0; }
    double acceptance() const { return drafted ? double(accepted) / double(drafted) : 0; }
    double tokens_per_pass() const { return passes ? double(new_tokens) / double(passes) : 0; }
};

// Prompt-lookup draft: the continuation of the most recent earlier occurrence of the last `ngram` tokens of
// `hist` (trying shorter n-grams down to 1). Kept for callers and tests; generation uses ngram_index.
inline std::vector<int> lookup_draft(std::span<const int> hist, int ngram, int k) {
    std::vector<int> d;
    for (int n = ngram; n >= 1 && d.empty(); --n) {
        if (hist.size() <= std::size_t(n)) continue;
        auto tail = hist.subspan(hist.size() - std::size_t(n));
        for (std::size_t s = hist.size() - std::size_t(n); s-- > 0;) {
            if (!std::equal(tail.begin(), tail.end(), hist.begin() + std::ptrdiff_t(s))) continue;
            for (std::size_t j = s + std::size_t(n); j < hist.size() && d.size() < std::size_t(k); ++j) d.push_back(hist[j]);
            break;
        }
    }
    return d;
}

// Incremental n-gram index over a token stream (the conversation so far): draft(k) finds the most recent earlier
// occurrence of the longest tail n-gram (nmax down to nmin tokens) in O(nmax) hash lookups and returns what
// followed it. A continuation that runs into the tail repeats with the match's period ("a b a b" -> "a b ...").
class ngram_index {
public:
    explicit ngram_index(int nmax = 3, int nmin = 1) : nmax_(std::max(nmax, 1)), nmin_(std::clamp(nmin, 1, std::max(nmax, 1))), at_(std::size_t(nmax_) + 1) {}
    void push(int t) {
        h_.push_back(t);
        std::size_t m = h_.size();   // n-grams ending just before the new token now have a continuation (at m - 1)
        for (int n = nmin_; n <= nmax_; ++n) if (m - 1 >= std::size_t(n)) at_[std::size_t(n)][key(m - 1 - std::size_t(n), n)] = m - 1;
    }
    void push(std::span<const int> ts) { for (int t : ts) push(t); }
    std::size_t size() const { return h_.size(); }
    const std::vector<int>& tokens() const { return h_; }
    // up to k drafts; *matched gets the n-gram length that matched (0: none)
    std::vector<int> draft(int k, int* matched = nullptr) const {
        std::vector<int> d; std::size_t m = h_.size();
        if (matched) *matched = 0;
        for (int n = nmax_; n >= nmin_ && k > 0; --n) {
            if (m < std::size_t(n) + 1) continue;
            auto& mp = at_[std::size_t(n)];
            auto it = mp.find(key(m - std::size_t(n), n));
            if (it == mp.end()) continue;
            std::size_t e = it->second;   // continuation start; verify (hash collisions)
            if (!std::equal(h_.begin() + std::ptrdiff_t(e - std::size_t(n)), h_.begin() + std::ptrdiff_t(e), h_.end() - n)) continue;
            std::size_t period = m - e;
            for (int j = 0; j < k; ++j) d.push_back(h_[e + std::size_t(j) % period]);
            if (matched) *matched = n;
            break;
        }
        return d;
    }
private:
    std::uint64_t key(std::size_t s, int n) const {
        std::uint64_t x = 0x9E3779B97F4A7C15ull * std::uint64_t(n);
        for (int i = 0; i < n; ++i) { x ^= std::uint64_t(std::uint32_t(h_[s + std::size_t(i)])) + 0x9E3779B97F4A7C15ull + (x << 6) + (x >> 2); }
        return x;
    }
    int nmax_, nmin_;
    std::vector<int> h_;
    std::vector<std::unordered_map<std::uint64_t, std::size_t>> at_;
};

// Target-side state of a speculative decode. Invariant between rounds: target.position() + pending.size() + 1 ==
// hist.size(), and hist.back() is the current token (sampled, not yet fed); `pending` are kept tokens whose
// recurrent state was rolled back and must be fed again (they lead the next pass).
struct spec_target {
    session& t; sampler& s; spec_planner& plan; speculative_stats& st;
    std::vector<int> hist, pending;
    int cur() const { return hist.back(); }

    // One pass: feed pending + current token + drafts `d`, emit every kept token (emit returns false to stop).
    // `q` (optional): the draft distribution of each draft, for modified rejection sampling; without it the
    // drafts are one-hot and verified by sample-and-compare. Returns the number of drafts accepted.
    template<class Emit> int round(std::span<const int> d, Emit&& emit, const std::vector<std::vector<double>>* q = nullptr) {
        using clk = std::chrono::steady_clock;
        std::vector<int> batch(pending); batch.push_back(cur()); batch.insert(batch.end(), d.begin(), d.end());
        const i64 base = i64(pending.size()) + 1, P0 = t.position();
        st.replayed += pending.size(); ++st.passes;
        auto t0 = clk::now();
        if (d.empty()) {   // plain pass
            int nx;
            if (s.plain_greedy()) nx = t.forward_argmax(batch);
            else { auto l = t.forward(batch); nx = s.sample(l, hist); }
            plan.observe_pass(int(batch.size()), std::chrono::duration<double>(clk::now() - t0).count());
            pending.clear(); hist.push_back(nx); emit(nx);
            return 0;
        }
        auto L = t.forward_spec(batch, base);
        plan.observe_pass(int(batch.size()), std::chrono::duration<double>(clk::now() - t0).count());
        ++st.rounds; st.drafted += d.size();
        const std::size_t V = std::size_t(t.vocab());
        auto row = [&](std::size_t j) { return L.subspan((std::size_t(base) - 1 + j) * V, V); };
        int acc = 0;
        for (std::size_t j = 0; j <= d.size(); ++j) {
            int tok; bool match;
            if (!q || j == d.size()) {
                tok = j < d.size() || !q ? s.sample(row(j), hist) : s.draw(s.probs(row(j), hist));
                match = j < d.size() && tok == d[j];
            } else {   // rejection sampling against the draft distribution
                auto p = s.probs(row(j), hist);
                const auto& qj = (*q)[j];
                double pi = p[std::size_t(d[j])], qi = qj[std::size_t(d[j])];
                match = qi > 0 && (pi >= qi || s.uniform() < pi / qi);
                if (match) tok = d[j];
                else { for (std::size_t i = 0; i < p.size(); ++i) p[i] = std::max(0.0, p[i] - qj[i]); tok = s.draw(p); }
            }
            if (match) ++acc;
            hist.push_back(tok);
            if (!emit(tok) || !match) break;
        }
        st.accepted += std::size_t(acc);
        for (std::size_t i = 0; i < std::min<std::size_t>(d.size(), st.pos_tried.size()); ++i) {
            ++st.pos_tried[i]; if (int(i) < acc) ++st.pos_won[i]; else break;
        }
        plan.observe_drafts(int(d.size()), acc);
        pending.clear();
        if (i64 keep = P0 + base + acc; keep < t.position()) {
            i64 n = t.spec_rewind(keep);   // recurrent: back to the mark (after the current token); n drafts to feed again
            pending.assign(d.begin(), d.begin() + n);
        }
        return acc;
    }
    // drafts that fit: context room and the tokens still wanted
    int room(int k, std::size_t produced, int max_new) const {
        i64 r = std::min<i64>(i64(k), std::min<i64>(i64(max_new) - i64(produced) - 1, t.context() - t.position() - i64(pending.size()) - 1));
        return int(std::max<i64>(r, 0));
    }
};

// Shared prefill / loop scaffolding for the generators below.
namespace detail {
struct spec_run {
    std::vector<int> out; bool done = false; int max_new; int eos; std::function<bool(int)> on_token;
    bool emit(int t) {
        out.push_back(t);
        if ((on_token && !on_token(t)) || t == eos || int(out.size()) >= max_new) done = true;
        return !done;
    }
};
} // namespace detail

// Draft-model speculation. k > 0: fixed draft length; k = 0: chosen by `planner` (a fresh one when null).
inline std::vector<int> speculative_generate(session& target, session& draft, std::span<const int> prompt, int max_new,
                                             sampler& s, int k = 4, const std::function<bool(int)>& on_token = {},
                                             speculative_stats* stats = nullptr, spec_planner* planner = nullptr) {
    target.check_generative(); draft.check_generative();
    if (prompt.empty()) raise(errc::invalid_argument, "speculative_generate: empty prompt");
    if (target.vocab() != draft.vocab()) raise(errc::invalid_argument, "draft vocab {} != target vocab {}", draft.vocab(), target.vocab());
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    speculative_stats st; spec_planner own; spec_planner& pl = planner ? *planner : own;
    target.reset(); draft.reset();
    detail::spec_run run{{}, false, max_new, target.tokenizer() ? target.tokenizer()->eos() : -1, on_token};
    spec_target T{target, s, pl, st, {prompt.begin(), prompt.end()}, {}};
    if (prompt.size() > 1) target.forward(prompt.subspan(0, prompt.size() - 1));
    st.prefill_seconds = std::chrono::duration<double>(clk::now() - t0).count();
    if (prompt.size() > 1) draft.forward(prompt.subspan(0, prompt.size() - 1));   // (a recurrent draft needs state to checkpoint)
    st.prefill_seconds = std::chrono::duration<double>(clk::now() - t0).count();
    std::vector<int> dpend;   // kept tokens the draft has not been fed yet
    const bool greedy = s.config().temperature <= 0.f, onehot = greedy && s.plain_greedy();
    auto emit = [&](int t) { return run.emit(t); };
    while (!run.done && max_new > 0) {
        if (target.position() + i64(T.pending.size()) + 1 > target.context()) break;
        int avail = T.room(k > 0 ? k : pl.kmax, run.out.size(), max_new);
        avail = int(std::min<i64>(avail, draft.context() - draft.position() - i64(dpend.size()) - 1));
        int nd = k > 0 ? avail : pl.choose(avail, int(T.pending.size()) + 1);
        int cur = T.cur();
        if (nd <= 0) { T.round({}, emit); dpend.push_back(cur); continue; }
        // 1. the draft proposes nd tokens (catching up on the kept tokens it has not seen first)
        std::vector<int> feed(dpend.begin(), dpend.end()); feed.push_back(cur);
        const i64 dp = draft.position(), dc = dp + i64(feed.size());   // dc: draft position once it has seen cur
        std::vector<int> since = feed;
        auto ck = draft.has_recurrent_state() ? draft.make_checkpoint() : session::checkpoint{};
        std::vector<int> d; std::vector<std::vector<double>> q; std::vector<int> dh = T.hist;
        for (int i = 0; i < nd; ++i) {
            auto a = clk::now(); int tok;
            if (onehot) tok = draft.forward_argmax(feed);
            else if (greedy) tok = s.sample(draft.forward(feed), dh);
            else { q.push_back(s.probs(draft.forward(feed), dh)); tok = s.draw(q.back()); }
            if (feed.size() == 1) pl.observe_draft_cost(std::chrono::duration<double>(clk::now() - a).count());
            d.push_back(tok); dh.push_back(tok); feed.assign(1, tok);
            if (i + 1 < nd) since.push_back(tok);
        }
        dpend.clear();
        // 2. the target verifies them in one pass
        int acc = T.round(d, emit, greedy ? nullptr : &q);
        // 3. the draft keeps cur + d[0, acc): it was fed cur + d[0, nd - 1)
        int fed_kept = std::min(acc, nd - 1);
        if (draft.position() > dc + fed_kept) {
            if (ck.fixed.empty()) draft.rewind(dc + fed_kept); else draft.rewind(dc + fed_kept, ck, since);
        }
        dpend.assign(d.begin() + fed_kept, d.begin() + acc);
    }
    st.probes = pl.probes();
    st.new_tokens = run.out.size();
    st.seconds = std::chrono::duration<double>(clk::now() - t0).count();
    if (stats) *stats = st;
    return run.out;
}

// Prompt-lookup ("n-gram") speculation: no draft model; drafts continue the most recent earlier occurrence of the
// last `ngram` tokens (down to `ngram_min`) of the prompt + output. k > 0: at most k drafts per pass; k = 0:
// planner-chosen.
inline std::vector<int> lookup_generate(session& target, std::span<const int> prompt, int max_new, sampler& s, int k = 4, int ngram = 3,
                                        const std::function<bool(int)>& on_token = {}, speculative_stats* stats = nullptr,
                                        spec_planner* planner = nullptr, int ngram_min = 1) {
    target.check_generative();
    if (prompt.empty()) raise(errc::invalid_argument, "lookup_generate: empty prompt");
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    speculative_stats st; spec_planner own; spec_planner& pl = planner ? *planner : own;
    target.reset();
    detail::spec_run run{{}, false, max_new, target.tokenizer() ? target.tokenizer()->eos() : -1, on_token};
    spec_target T{target, s, pl, st, {prompt.begin(), prompt.end()}, {}};
    ngram_index idx(ngram, ngram_min); idx.push(prompt);
    if (prompt.size() > 1) target.forward(prompt.subspan(0, prompt.size() - 1));
    st.prefill_seconds = std::chrono::duration<double>(clk::now() - t0).count();
    auto emit = [&](int t) { idx.push(t); return run.emit(t); };
    while (!run.done && max_new > 0) {
        if (target.position() + i64(T.pending.size()) + 1 > target.context()) break;
        int avail = T.room(k > 0 ? k : pl.kmax, run.out.size(), max_new);
        auto d = avail > 0 ? idx.draft(avail) : std::vector<int>{};
        int nd = k > 0 ? int(d.size()) : pl.choose(int(d.size()), int(T.pending.size()) + 1);
        d.resize(std::size_t(nd));
        T.round(d, emit);
    }
    st.probes = pl.probes();
    st.new_tokens = run.out.size();
    st.seconds = std::chrono::duration<double>(clk::now() - t0).count();
    if (stats) *stats = st;
    return run.out;
}

} // namespace forge::inference
