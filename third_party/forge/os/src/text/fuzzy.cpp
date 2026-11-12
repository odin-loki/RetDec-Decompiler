// forge::os::text: edit distances, Jaro-Winkler and an editor-style fuzzy matcher, all over graphemes.
#include "internal.hpp"
#include <algorithm>
#include <numeric>

namespace forge::os::text {
using namespace detail;
namespace {
std::vector<std::string_view> units(std::string_view s) {
    for (unsigned char c : s) if (c >= 0x80) return graphemes(s);
    std::vector<std::string_view> r; r.reserve(s.size()); for (std::size_t i = 0; i < s.size(); ++i) r.push_back(s.substr(i, 1)); return r;
}
}  // namespace

std::size_t levenshtein(std::string_view a, std::string_view b) {
    auto x = units(a), y = units(b);
    if (x.size() < y.size()) std::swap(x, y);
    std::vector<std::size_t> row(y.size() + 1); std::iota(row.begin(), row.end(), 0);
    for (std::size_t i = 1; i <= x.size(); ++i) {
        std::size_t diag = row[0]; row[0] = i;
        for (std::size_t j = 1; j <= y.size(); ++j) {
            std::size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (x[i - 1] == y[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[y.size()];
}
std::size_t damerau_levenshtein(std::string_view a, std::string_view b) {
    auto x = units(a), y = units(b); const std::size_t n = x.size(), m = y.size();
    std::vector<std::vector<std::size_t>> d(n + 1, std::vector<std::size_t>(m + 1));
    for (std::size_t i = 0; i <= n; ++i) d[i][0] = i;
    for (std::size_t j = 0; j <= m; ++j) d[0][j] = j;
    for (std::size_t i = 1; i <= n; ++i) for (std::size_t j = 1; j <= m; ++j) {
        std::size_t cost = x[i - 1] == y[j - 1] ? 0 : 1;
        d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
        if (i > 1 && j > 1 && x[i - 1] == y[j - 2] && x[i - 2] == y[j - 1]) d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
    }
    return d[n][m];
}
double similarity(std::string_view a, std::string_view b) {
    std::size_t la = units(a).size(), lb = units(b).size(), mx = std::max(la, lb);
    return mx ? 1.0 - double(levenshtein(a, b)) / double(mx) : 1.0;
}
double jaro_winkler(std::string_view a, std::string_view b) {
    auto x = units(a), y = units(b);
    if (x.empty() && y.empty()) return 1.0;
    if (x.empty() || y.empty()) return 0.0;
    std::size_t win = std::max(x.size(), y.size()) / 2; win = win ? win - 1 : 0;
    std::vector<bool> mx(x.size()), my(y.size()); std::size_t m = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        std::size_t lo = i > win ? i - win : 0, hi = std::min(y.size(), i + win + 1);
        for (std::size_t j = lo; j < hi; ++j) if (!my[j] && x[i] == y[j]) { mx[i] = my[j] = true; ++m; break; }
    }
    if (!m) return 0.0;
    std::size_t t = 0, k = 0;
    for (std::size_t i = 0; i < x.size(); ++i) if (mx[i]) { while (!my[k]) ++k; if (x[i] != y[k]) ++t; ++k; }
    double md = double(m);
    double jaro = (md / double(x.size()) + md / double(y.size()) + (md - double(t) / 2) / md) / 3;
    std::size_t prefix = 0; while (prefix < 4 && prefix < x.size() && prefix < y.size() && x[prefix] == y[prefix]) ++prefix;
    return jaro + double(prefix) * 0.1 * (1 - jaro);
}

std::optional<int> fuzzy_score(std::string_view pattern, std::string_view cand, std::vector<std::size_t>* positions) {
    // Code-point level, case-folded; scoring modelled on fzf v2: match 16, boundary bonus 8 (after separator or
    // camelCase hump), first-char x2, consecutive 4 per char, gap -3 start / -1 extension.
    std::u32string p, c; std::vector<std::size_t> off;
    { std::size_t i = 0; while (i < pattern.size()) { cp_t ch = decode_utf8(pattern, i); if (!is_space(ch)) p += simple_fold(ch); } }
    { std::size_t i = 0; while (i < cand.size()) { off.push_back(i); c += decode_utf8(cand, i); } }
    std::u32string cf(c); for (auto& ch : cf) ch = simple_fold(ch);
    const std::size_t n = p.size(), m = c.size();
    if (n == 0) return 0;
    if (n > m) return std::nullopt;
    { std::size_t k = 0; for (std::size_t j = 0; j < m && k < n; ++j) if (cf[j] == p[k]) ++k; if (k < n) return std::nullopt; }
    auto bonus = [&](std::size_t j) {
        if (j == 0) return 10;
        cp_t a = c[j - 1], b = c[j];
        if (!is_word_char(a) && is_word_char(b)) return 8;
        if (is_lower(a) && is_upper(b)) return 7;
        if (!is_digit(a) && is_digit(b)) return 4;
        return 0;
    };
    constexpr int NEG = -1000000;
    // H[i][j]: best score with p[0..i] matched and p[i] matched at c[j]. C[i][j]: consecutive run length there.
    std::vector<std::vector<int>> H(n, std::vector<int>(m, NEG)), C(n, std::vector<int>(m, 0));
    std::vector<std::vector<int>> from(n, std::vector<int>(m, -1));
    for (std::size_t i = 0; i < n; ++i) {
        int best = NEG; int bestj = -1;   // max over k < j of H[i-1][k] - gap(j - k - 1)
        for (std::size_t j = 0; j < m; ++j) {
            if (i > 0 && j > 0) {
                int prev = H[i - 1][j - 1];
                if (best > NEG) best -= 1;   // extend the gap of the running best
                if (prev > NEG && prev - 3 > best) { best = prev - 3; bestj = int(j - 1); }
            }
            if (cf[j] != p[i]) continue;
            int b = bonus(j);
            if (i == 0) { H[i][j] = 16 + b * 2 - int(std::min<std::size_t>(j, 15)); C[i][j] = 1; continue; }
            int cons = (j > 0 && H[i - 1][j - 1] > NEG) ? H[i - 1][j - 1] + 16 + std::max(b, 4) + 4 * C[i - 1][j - 1] : NEG;
            int gap = best > NEG ? best + 16 + b : NEG;
            if (cons >= gap && cons > NEG) { H[i][j] = cons; C[i][j] = C[i - 1][j - 1] + 1; from[i][j] = int(j - 1); }
            else if (gap > NEG) { H[i][j] = gap; C[i][j] = 1; from[i][j] = bestj; }
        }
    }
    int best = NEG; int bj = -1;
    for (std::size_t j = 0; j < m; ++j) if (H[n - 1][j] > best) { best = H[n - 1][j]; bj = int(j); }
    if (bj < 0) return std::nullopt;
    if (positions) {
        positions->assign(n, 0); int j = bj;
        for (std::size_t i = n; i-- > 0;) { (*positions)[i] = off[std::size_t(j)]; if (i) j = from[i][std::size_t(j)]; if (j < 0) break; }
    }
    return best;
}
std::vector<fuzzy_hit> fuzzy_filter(std::string_view pattern, std::span<const std::string> items, std::size_t limit) {
    std::vector<fuzzy_hit> r;
    for (std::size_t i = 0; i < items.size(); ++i) if (auto s = fuzzy_score(pattern, items[i])) r.push_back({i, *s});
    std::stable_sort(r.begin(), r.end(), [&](auto& a, auto& b) { return a.score != b.score ? a.score > b.score : items[a.index].size() < items[b.index].size(); });
    if (limit && r.size() > limit) r.resize(limit);
    return r;
}
}  // namespace forge::os::text
