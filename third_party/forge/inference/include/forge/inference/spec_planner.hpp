#pragma once
// Draft-length planner for speculative decoding: each pass verifies the number of drafts d that maximises the
// expected tokens per second,
//     (1 + sum_{i<d} P[drafts 0..i all accepted]) / (t(rows + d) + d * draft_cost),
// from numbers measured on this machine: t(n) is a moving average of the wall time of a pass over n rows, and the
// acceptance chance of draft position i is the share of drafts tried there that were accepted (counts halved now
// and then so the estimate follows the current text; a prior of 1/2 worth two trials). d = 0 (a plain pass) wins
// unless some d beats it, so drafts never make decoding slower than the measurements say; after `probe_every`
// passes in a row without drafts one 1-draft pass is forced, so a transient slowdown cannot switch drafts off for
// good. Costs unmeasured so far are interpolated (or extrapolated at +30 % per row).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace forge::inference {

class spec_planner {
public:
    static constexpr int max_rows = 32;     // pass sizes tracked (pending replay rows + 1 + drafts)
    static constexpr int max_drafts = 16;   // draft positions tracked
    int kmax = 8;              // most drafts per pass
    int probe_every = 16;      // force a 1-draft pass after this many draft-less passes in a row
    double ema = 0.25;         // weight of a new timing
    double halve_at = 192;     // halve the counts when position 0 has this many trials

    // a pass over `rows` rows took `s` seconds. The first pass of each size is not counted: it usually includes
    // compiling that size's plan and loading its kernels.
    void observe_pass(int rows, double s) {
        if (rows < 1 || rows > max_rows || !(s > 0)) return;
        if (!seen_[std::size_t(rows)]) { seen_[std::size_t(rows)] = true; return; }
        auto& t = t_[std::size_t(rows)];
        t = n_[std::size_t(rows)]++ ? (1 - ema) * t + ema * s : s;
    }
    // `n` drafts were verified, the first `acc` accepted (drafts after the first rejection are not counted as tried)
    void observe_drafts(int n, int acc) {
        for (int i = 0; i < std::min(n, max_drafts); ++i) {
            tried_[std::size_t(i)] += 1;
            if (i < acc) won_[std::size_t(i)] += 1; else break;
        }
        if (tried_[0] >= halve_at) for (int i = 0; i < max_drafts; ++i) { tried_[std::size_t(i)] /= 2; won_[std::size_t(i)] /= 2; }
    }
    // moving-average cost of a single draft pass (draft models; lookup drafts cost nothing)
    void observe_draft_cost(double s) { if (s > 0 && draft_n_++) draft_ = draft_n_ > 2 ? (1 - ema) * draft_ + ema * s : s; }   // first one skipped
    double draft_cost() const { return draft_; }
    // forget the pass timings (e.g. a new turn), keep the acceptance counts
    void reset_costs() { t_.fill(0); n_.fill(0); refused_ = 0; }

    // P[draft at position i accepted | drafts before it accepted]
    double accept(int i) const { return i < max_drafts ? (won_[std::size_t(i)] + 1) / (tried_[std::size_t(i)] + 2) : 0.5; }
    // estimated seconds for a pass over `rows` rows
    double cost(int rows) const {
        rows = std::clamp(rows, 1, max_rows);
        if (n_[std::size_t(rows)]) return t_[std::size_t(rows)];
        int lo = rows - 1, hi = rows + 1;
        while (lo >= 1 && !n_[std::size_t(lo)]) --lo;
        while (hi <= max_rows && !n_[std::size_t(hi)]) ++hi;
        bool hl = lo >= 1, hh = hi <= max_rows;
        if (hl && hh) return t_[std::size_t(lo)] + (t_[std::size_t(hi)] - t_[std::size_t(lo)]) * double(rows - lo) / double(hi - lo);
        if (hl) return t_[std::size_t(lo)] * (1 + 0.3 * (rows - lo));
        if (hh) return t_[std::size_t(hi)] / (1 + 0.3 * (hi - rows));
        return 1 + 0.3 * (rows - 1);   // nothing measured: relative units
    }
    // Number of drafts to verify now: `avail` drafts can be had, `base_rows` rows go into the pass anyway (replay
    // rows + the current token), each draft costs `per_draft` seconds to make. Counts the forced probes.
    int choose(int avail, int base_rows = 1) {
        avail = std::min({avail, kmax, max_drafts, max_rows - base_rows});
        if (avail <= 0) return 0;
        double best = 1.0 / cost(base_rows), chance = 1, gain = 1; int bd = 0;
        for (int d = 1; d <= avail; ++d) {
            chance *= accept(d - 1); gain += chance;
            double r = gain / (cost(base_rows + d) + d * draft_);
            if (r > best) { best = r; bd = d; }
        }
        if (bd == 0 && ++refused_ >= probe_every) { refused_ = 0; ++probes_; return 1; }
        if (bd > 0) refused_ = 0;
        return bd;
    }
    std::size_t probes() const { return probes_; }
private:
    std::array<double, max_rows + 1> t_{};
    std::array<std::size_t, max_rows + 1> n_{};
    std::array<bool, max_rows + 1> seen_{};
    std::array<double, max_drafts> tried_{}, won_{};
    double draft_ = 0; std::size_t draft_n_ = 0;
    int refused_ = 0; std::size_t probes_ = 0;
};

} // namespace forge::inference
