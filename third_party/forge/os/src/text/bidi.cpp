// forge::os::text: Unicode Bidirectional Algorithm (UAX 9), single paragraph per call.
#include "internal.hpp"
#include <algorithm>

namespace forge::os::text {
using namespace detail;
namespace {
constexpr int max_depth = 125;
bool is_iso_init(int c) { return c == B_LRI || c == B_RLI || c == B_FSI; }
bool removed_x9(int c) { return c == B_RLE || c == B_LRE || c == B_RLO || c == B_LRO || c == B_PDF || c == B_BN; }
bool strong_rl(int c) { return c == B_R || c == B_AL; }
bool is_ni(int c) { return c == B_B || c == B_S || c == B_WS || c == B_ON || is_iso_init(c) || c == B_PDI; }

// P2/P3 over [b, e): first strong char skipping isolates; stop at a PDI matching an outer isolate when `stop_at_pdi`.
int first_strong_level(const std::vector<int>& cls, std::size_t b, std::size_t e, bool stop_at_pdi, int dflt) {
    int depth = 0;
    for (std::size_t i = b; i < e; ++i) {
        int c = cls[i];
        if (is_iso_init(c)) { ++depth; continue; }
        if (c == B_PDI) { if (depth) --depth; else if (stop_at_pdi) break; continue; }
        if (c == B_B) break;
        if (depth) continue;
        if (c == B_L) return 0;
        if (strong_rl(c)) return 1;
    }
    return dflt;
}
}  // namespace

bidi_result bidi_levels32(std::u32string_view s, direction base) {
    const std::size_t n = s.size();
    bidi_result R; R.levels.assign(n, 0);
    std::vector<int> orig(n), cls(n);
    for (std::size_t i = 0; i < n; ++i) orig[i] = cls[i] = prop(s[i]).bidi;
    // BD9: matching PDI for each isolate initiator.
    std::vector<long> match(n, -1);
    { std::vector<std::size_t> st; for (std::size_t i = 0; i < n; ++i) {
        if (is_iso_init(orig[i])) st.push_back(i);
        else if (orig[i] == B_PDI && !st.empty()) { match[st.back()] = long(i); st.pop_back(); }
        else if (orig[i] == B_B) st.clear(); } }
    int para = base == direction::ltr ? 0 : base == direction::rtl ? 1 : first_strong_level(orig, 0, n, false, 0);
    R.paragraph_level = para;
    std::vector<int> lev(n, para);
    // X1-X8
    struct entry { int level; int ovr; bool iso; };
    std::vector<entry> st; st.reserve(max_depth + 2); st.push_back({para, -1, false});
    int over_iso = 0, over_emb = 0, valid_iso = 0;
    for (std::size_t i = 0; i < n; ++i) {
        int c = orig[i];
        auto next_odd = [&] { int l = st.back().level; return (l + 1) | 1; };
        auto next_even = [&] { int l = st.back().level; return (l + 2) & ~1; };
        switch (c) {
        case B_RLE: case B_LRE: case B_RLO: case B_LRO: {
            int nl = (c == B_RLE || c == B_RLO) ? next_odd() : next_even();
            lev[i] = st.back().level;
            if (nl <= max_depth && !over_iso && !over_emb) st.push_back({nl, c == B_RLO ? B_R : c == B_LRO ? B_L : -1, false});
            else if (!over_iso) ++over_emb;
            break; }
        case B_RLI: case B_LRI: case B_FSI: {
            lev[i] = st.back().level;
            if (st.back().ovr >= 0) cls[i] = st.back().ovr;
            bool rtl = c == B_RLI;
            if (c == B_FSI) rtl = first_strong_level(orig, i + 1, match[i] >= 0 ? std::size_t(match[i]) : n, true, 0) == 1;
            int nl = rtl ? next_odd() : next_even();
            if (nl <= max_depth && !over_iso && !over_emb) { ++valid_iso; st.push_back({nl, -1, true}); }
            else ++over_iso;
            break; }
        case B_PDI:
            if (over_iso) --over_iso;
            else if (valid_iso) { over_emb = 0; while (!st.back().iso) st.pop_back(); st.pop_back(); --valid_iso; }
            lev[i] = st.back().level;
            if (st.back().ovr >= 0) cls[i] = st.back().ovr;
            break;
        case B_PDF:
            lev[i] = st.back().level;
            if (over_iso) {} else if (over_emb) --over_emb; else if (!st.back().iso && st.size() >= 2) st.pop_back();
            break;
        case B_B: lev[i] = para; break;
        case B_BN: lev[i] = st.back().level; break;
        default:
            lev[i] = st.back().level;
            if (st.back().ovr >= 0) cls[i] = st.back().ovr;
        }
    }
    // X9: removed characters are skipped from here on.
    std::vector<std::size_t> idx; idx.reserve(n);
    for (std::size_t i = 0; i < n; ++i) if (!removed_x9(orig[i])) idx.push_back(i);
    // X10: level runs, then isolating run sequences.
    std::vector<std::vector<std::size_t>> runs;
    for (std::size_t k = 0; k < idx.size(); ++k) {
        if (k == 0 || lev[idx[k]] != lev[idx[k - 1]]) runs.emplace_back();
        runs.back().push_back(idx[k]);
    }
    std::vector<long> run_of(n, -1);
    for (std::size_t r = 0; r < runs.size(); ++r) for (auto i : runs[r]) run_of[i] = long(r);
    std::vector<std::vector<std::size_t>> seqs;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        std::size_t first = runs[r].front();
        if (orig[first] == B_PDI) {   // starts with a PDI that matches an initiator -> already chained
            bool matched = false; for (std::size_t i = 0; i < n; ++i) if (match[i] == long(first)) { matched = !removed_x9(orig[i]); break; }
            if (matched) continue;
        }
        std::vector<std::size_t> seq; std::size_t cur = r;
        while (true) {
            seq.insert(seq.end(), runs[cur].begin(), runs[cur].end());
            std::size_t last = runs[cur].back();
            if (is_iso_init(orig[last]) && match[last] >= 0 && run_of[match[last]] >= 0) cur = std::size_t(run_of[match[last]]);
            else break;
        }
        seqs.push_back(std::move(seq));
    }
    const std::vector<int> emb = lev;   // explicit levels; sos/eos must not see I1/I2 results
    auto pos_of = [&](std::size_t i) { return std::lower_bound(idx.begin(), idx.end(), i) - idx.begin(); };
    for (auto& seq : seqs) {
        const std::size_t m = seq.size();
        int level = lev[seq[0]];
        auto pk = pos_of(seq.front());
        int prev_level = pk > 0 ? emb[idx[pk - 1]] : para;
        if (orig[seq.front()] == B_PDI) {} // prev_level as computed (the matching initiator's level is the same as ours)
        int sos = (std::max(prev_level, level) & 1) ? B_R : B_L;
        std::size_t lastc = seq.back(); int next_level;
        if (is_iso_init(orig[lastc]) && (match[lastc] < 0)) next_level = para;
        else if (is_iso_init(orig[lastc])) next_level = para;
        else { auto lk = pos_of(lastc); next_level = std::size_t(lk + 1) < idx.size() ? emb[idx[lk + 1]] : para; }
        int eos = (std::max(next_level, level) & 1) ? B_R : B_L;
        std::vector<int> t(m); for (std::size_t k = 0; k < m; ++k) t[k] = cls[seq[k]];
        // W1
        for (std::size_t k = 0; k < m; ++k) if (t[k] == B_NSM) t[k] = k == 0 ? sos : (is_iso_init(t[k - 1]) || t[k - 1] == B_PDI) ? B_ON : t[k - 1];
        // W2, W3
        { int last_strong = sos; for (std::size_t k = 0; k < m; ++k) {
            if (t[k] == B_L || t[k] == B_R || t[k] == B_AL) last_strong = t[k];
            else if (t[k] == B_EN && last_strong == B_AL) t[k] = B_AN; } }
        for (auto& x : t) if (x == B_AL) x = B_R;
        // W4
        for (std::size_t k = 1; k + 1 < m; ++k) {
            if (t[k] == B_ES && t[k - 1] == B_EN && t[k + 1] == B_EN) t[k] = B_EN;
            else if (t[k] == B_CS && t[k - 1] == B_EN && t[k + 1] == B_EN) t[k] = B_EN;
            else if (t[k] == B_CS && t[k - 1] == B_AN && t[k + 1] == B_AN) t[k] = B_AN;
        }
        // W5
        for (std::size_t k = 0; k < m; ++k) if (t[k] == B_ET) {
            std::size_t e = k; while (e < m && t[e] == B_ET) ++e;
            bool en = (k > 0 && t[k - 1] == B_EN) || (e < m && t[e] == B_EN);
            if (en) for (std::size_t q = k; q < e; ++q) t[q] = B_EN;
            k = e - 1;
        }
        // W6
        for (auto& x : t) if (x == B_ES || x == B_ET || x == B_CS) x = B_ON;
        // W7
        { int last_strong = sos; for (std::size_t k = 0; k < m; ++k) {
            if (t[k] == B_L || t[k] == B_R) last_strong = t[k];
            else if (t[k] == B_EN && last_strong == B_L) t[k] = B_L; } }
        // N0: bracket pairs
        int edir = (level & 1) ? B_R : B_L;
        {
            std::vector<std::pair<std::size_t, std::size_t>> pairs; std::vector<std::pair<cp_t, std::size_t>> bst; bool overflow = false;
            auto canon = [](cp_t c) { return c == 0x2329 ? cp_t(0x3008) : c == 0x232A ? cp_t(0x3009) : c; };
            for (std::size_t k = 0; k < m && !overflow; ++k) {
                if (t[k] != B_ON) continue;
                int ty; cp_t other = bracket_pair(s[seq[k]], ty);
                if (ty == 1) { if (bst.size() >= 63) { overflow = true; break; } bst.push_back({canon(other), k}); }
                else if (ty == 2) {
                    cp_t me = canon(s[seq[k]]);
                    for (std::size_t q = bst.size(); q-- > 0;) if (bst[q].first == me) { pairs.push_back({bst[q].second, k}); bst.resize(q); break; }
                }
            }
            if (overflow) pairs.clear();
            std::sort(pairs.begin(), pairs.end());
            auto sdir = [](int x) { return x == B_L ? B_L : (x == B_R || x == B_EN || x == B_AN) ? B_R : -1; };
            for (auto [o, c] : pairs) {
                bool found_e = false, found_o = false;
                for (std::size_t k = o + 1; k < c; ++k) { int d = sdir(t[k]); if (d == edir) found_e = true; else if (d >= 0) found_o = true; }
                int set = -1;
                if (found_e) set = edir;
                else if (found_o) {
                    int ctx = sos; for (std::size_t k = o; k-- > 0;) { int d = sdir(t[k]); if (d >= 0) { ctx = d; break; } }
                    set = ctx != edir ? ctx : edir;
                }
                if (set >= 0) {
                    t[o] = t[c] = set;
                    for (std::size_t k = o + 1; k < m && orig[seq[k]] == B_NSM; ++k) t[k] = set;
                    for (std::size_t k = c + 1; k < m && orig[seq[k]] == B_NSM; ++k) t[k] = set;
                }
            }
        }
        // N1, N2
        for (std::size_t k = 0; k < m; ++k) if (is_ni(t[k])) {
            std::size_t e = k; while (e < m && is_ni(t[e])) ++e;
            auto sd = [](int x) { return x == B_L ? B_L : B_R; };
            int before = k == 0 ? sos : sd(t[k - 1]);
            int after = e == m ? eos : sd(t[e]);
            int set = before == after ? before : edir;
            for (std::size_t q = k; q < e; ++q) t[q] = set;
            k = e - 1;
        }
        // I1, I2
        for (std::size_t k = 0; k < m; ++k) {
            int& L = lev[seq[k]];
            if (!(L & 1)) { if (t[k] == B_R) L += 1; else if (t[k] == B_AN || t[k] == B_EN) L += 2; }
            else if (t[k] == B_L || t[k] == B_EN || t[k] == B_AN) L += 1;
        }
    }
    // L1 (whole input = one line)
    {
        bool trailing = true;
        for (std::size_t i = n; i-- > 0;) {
            int c = orig[i];
            if (c == B_S || c == B_B) { lev[i] = para; trailing = true; continue; }
            if (removed_x9(c)) continue;
            if (trailing && (c == B_WS || is_iso_init(c) || c == B_PDI)) lev[i] = para;
            else trailing = false;
        }
    }
    for (std::size_t i = 0; i < n; ++i) R.levels[i] = removed_x9(orig[i]) ? 0xFF : std::uint8_t(lev[i]);
    return R;
}

std::vector<std::size_t> bidi_reorder32(std::u32string_view s, const bidi_result& r) {
    std::vector<std::size_t> ord; std::vector<int> lv;
    for (std::size_t i = 0; i < s.size() && i < r.levels.size(); ++i) if (r.levels[i] != 0xFF) { ord.push_back(i); lv.push_back(r.levels[i]); }
    if (lv.empty()) return ord;
    int hi = *std::max_element(lv.begin(), lv.end()), lo_odd = 255;
    for (int x : lv) if (x & 1) lo_odd = std::min(lo_odd, x);
    for (int L = hi; L >= lo_odd && L > 0; --L) {
        for (std::size_t k = 0; k < lv.size();) {
            if (lv[k] < L) { ++k; continue; }
            std::size_t e = k; while (e < lv.size() && lv[e] >= L) ++e;
            std::reverse(ord.begin() + k, ord.begin() + e); std::reverse(lv.begin() + k, lv.begin() + e);
            k = e;
        }
    }
    return ord;
}
std::string bidi_visual(std::string_view s, direction base) {
    std::string out;
    std::size_t start = 0;
    while (start <= s.size()) {   // line by line
        auto nl = s.find('\n', start); auto line = s.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        auto v = to32(line); auto r = bidi_levels32(v, base);
        for (auto i : bidi_reorder32(v, r)) append_utf8(out, (r.levels[i] & 1) ? mirror(v[i]) : v[i]);
        if (nl == std::string_view::npos) break;
        out += '\n'; start = nl + 1;
    }
    return out;
}
direction base_direction(std::string_view s) {
    auto v = to32(s); std::vector<int> c(v.size()); for (std::size_t i = 0; i < v.size(); ++i) c[i] = prop(v[i]).bidi;
    return first_strong_level(c, 0, c.size(), false, 0) ? direction::rtl : direction::ltr;
}
}  // namespace forge::os::text
