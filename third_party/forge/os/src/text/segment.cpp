// forge::os::text: grapheme, word and sentence boundaries (UAX 29) and line breaking (UAX 14), Unicode 16.
#include "internal.hpp"

namespace forge::os::text {
using namespace detail;
namespace {
template <class F> std::vector<std::size_t> to_bytes(std::string_view s, F&& f) {
    auto v = to32(s); auto b = f(std::u32string_view(v)); auto off = cp_byte_offsets(s);
    for (auto& x : b) x = off[x];
    return b;
}
std::vector<std::string_view> pieces(std::string_view s, const std::vector<std::size_t>& b) {
    std::vector<std::string_view> r; for (std::size_t k = 1; k < b.size(); ++k) r.push_back(s.substr(b[k - 1], b[k] - b[k - 1])); return r;
}
}  // namespace

// ---------------------------------------------------------------- graphemes
std::vector<std::size_t> grapheme_breaks32(std::u32string_view s) {
    std::vector<std::size_t> r{0};
    if (s.empty()) return r;
    int ri = 0, pict = 0, incb = 0;   // pict: 1 = ExtPict Extend*, 2 = ... ZWJ ; incb: 1 consonant (ext)*, 2 ... linker seen
    auto upd = [&](cp_t c) {
        auto& p = prop(c);
        ri = p.gcb == GB_RI ? ri + 1 : 0;
        if (p.flags & F_ExtPict) pict = 1; else if (pict == 1 && p.gcb == GB_Extend) pict = 1; else if (pict == 1 && p.gcb == GB_ZWJ) pict = 2; else pict = 0;
        if (p.incb == InCB_Consonant) incb = 1; else if (incb && p.incb == InCB_Linker) incb = 2; else if (incb && (p.incb == InCB_Extend)) {} else incb = 0;
    };
    upd(s[0]);
    for (std::size_t i = 1; i < s.size(); ++i) {
        auto& a = prop(s[i - 1]); auto& b = prop(s[i]); bool brk;
        int A = a.gcb, B = b.gcb;
        if (A == GB_CR && B == GB_LF) brk = false;
        else if (A == GB_Control || A == GB_CR || A == GB_LF) brk = true;
        else if (B == GB_Control || B == GB_CR || B == GB_LF) brk = true;
        else if (A == GB_L && (B == GB_L || B == GB_V || B == GB_LV || B == GB_LVT)) brk = false;
        else if ((A == GB_LV || A == GB_V) && (B == GB_V || B == GB_T)) brk = false;
        else if ((A == GB_LVT || A == GB_T) && B == GB_T) brk = false;
        else if (B == GB_Extend || B == GB_ZWJ || B == GB_SpacingMark || A == GB_Prepend) brk = false;
        else if (incb == 2 && b.incb == InCB_Consonant) brk = false;
        else if (pict == 2 && (b.flags & F_ExtPict)) brk = false;
        else if (A == GB_RI && B == GB_RI) brk = ri % 2 == 0;
        else brk = true;
        if (brk) r.push_back(i);
        upd(s[i]);
    }
    r.push_back(s.size());
    return r;
}

// ---------------------------------------------------------------- words
std::vector<std::size_t> word_breaks32(std::u32string_view s) {
    std::vector<std::size_t> r{0};
    const std::size_t n = s.size();
    if (!n) return r;
    std::vector<int> w(n); for (std::size_t i = 0; i < n; ++i) w[i] = prop(s[i]).wb;
    auto ign = [&](std::size_t i) { return w[i] == WB_Extend || w[i] == WB_Format || w[i] == WB_ZWJ; };
    auto ahl = [](int x) { return x == WB_ALetter || x == WB_Hebrew_Letter; };
    auto midnl = [](int x) { return x == WB_MidNumLet || x == WB_Single_Quote; };
    // effective previous non-ignorable index before position i (i.e. at or before i-1)
    auto prev_idx = [&](long i) -> long { long j = i; while (j > 0 && ign(std::size_t(j))) --j; return j; };   // j >= 0
    auto prev_of = [&](long i) -> long { if (i <= 0) return -1; long j = i - 1; while (j > 0 && ign(std::size_t(j))) --j; return j; };
    auto next_of = [&](std::size_t i) -> long { std::size_t j = i + 1; while (j < n && ign(j)) ++j; return j < n ? long(j) : -1; };
    for (std::size_t i = 1; i < n; ++i) {
        int A = w[i - 1], B = w[i]; bool brk = true;
        auto nl = [](int x) { return x == WB_Newline || x == WB_CR || x == WB_LF; };
        if (A == WB_CR && B == WB_LF) brk = false;
        else if (nl(A) || nl(B)) brk = true;
        else if (A == WB_ZWJ && (prop(s[i]).flags & F_ExtPict)) brk = false;
        else if (A == WB_WSegSpace && B == WB_WSegSpace) brk = false;
        else if (ign(i)) brk = false;
        else {
            long pi = prev_idx(long(i) - 1); int P = w[pi];
            long ppi = prev_of(pi); int PP = ppi >= 0 ? w[ppi] : -1;
            long ni = next_of(i); int N = ni >= 0 ? w[ni] : -1;
            if (ahl(P) && ahl(B)) brk = false;                                                   // WB5
            else if (ahl(P) && (B == WB_MidLetter || midnl(B)) && ahl(N)) brk = false;          // WB6
            else if (ahl(PP) && (P == WB_MidLetter || midnl(P)) && ahl(B)) brk = false;         // WB7
            else if (P == WB_Hebrew_Letter && B == WB_Single_Quote) brk = false;                 // WB7a
            else if (P == WB_Hebrew_Letter && B == WB_Double_Quote && N == WB_Hebrew_Letter) brk = false;
            else if (PP == WB_Hebrew_Letter && P == WB_Double_Quote && B == WB_Hebrew_Letter) brk = false;
            else if (P == WB_Numeric && B == WB_Numeric) brk = false;                            // WB8
            else if (ahl(P) && B == WB_Numeric) brk = false;
            else if (P == WB_Numeric && ahl(B)) brk = false;
            else if (PP == WB_Numeric && (P == WB_MidNum || midnl(P)) && B == WB_Numeric) brk = false;   // WB11
            else if (P == WB_Numeric && (B == WB_MidNum || midnl(B)) && N == WB_Numeric) brk = false;    // WB12
            else if (P == WB_Katakana && B == WB_Katakana) brk = false;
            else if ((ahl(P) || P == WB_Numeric || P == WB_Katakana || P == WB_ExtendNumLet) && B == WB_ExtendNumLet) brk = false;
            else if (P == WB_ExtendNumLet && (ahl(B) || B == WB_Numeric || B == WB_Katakana)) brk = false;
            else if (P == WB_RI && B == WB_RI) {
                int cnt = 0; long j = pi; while (j >= 0 && w[j] == WB_RI) { ++cnt; j = prev_of(j); if (j >= 0 && ign(std::size_t(j))) break; }
                brk = cnt % 2 == 0;
            }
        }
        if (brk) r.push_back(i);
    }
    r.push_back(n);
    return r;
}

// ---------------------------------------------------------------- sentences
std::vector<std::size_t> sentence_breaks32(std::u32string_view s) {
    std::vector<std::size_t> r{0};
    const std::size_t n = s.size();
    if (!n) return r;
    std::vector<int> t(n); for (std::size_t i = 0; i < n; ++i) t[i] = prop(s[i]).sb;
    auto ign = [&](std::size_t i) { return t[i] == SB_Extend || t[i] == SB_Format; };
    auto para = [](int x) { return x == SB_Sep || x == SB_CR || x == SB_LF; };
    auto saterm = [](int x) { return x == SB_STerm || x == SB_ATerm; };
    auto prev_of = [&](long i) -> long { if (i <= 0) return -1; long j = i - 1; while (j > 0 && ign(std::size_t(j)) && !para(t[j - 1])) --j; return j; };
    auto base_at = [&](long i) -> long { long j = i; while (j > 0 && ign(std::size_t(j)) && !para(t[j - 1])) --j; return j; };
    for (std::size_t i = 1; i < n; ++i) {
        int A = t[i - 1], B = t[i]; bool brk = false;
        if (A == SB_CR && B == SB_LF) { continue; }
        if (para(A)) { r.push_back(i); continue; }
        if (ign(i)) continue;
        long p = base_at(long(i) - 1); int P = t[p];
        long pp = prev_of(p); int PP = pp >= 0 ? t[pp] : -1;
        // Parse SATerm Close* Sp* ending at p.
        long j = p; int sp = 0, cl = 0;
        while (j >= 0 && t[j] == SB_Sp) { ++sp; j = prev_of(j); }
        while (j >= 0 && t[j] == SB_Close) { ++cl; j = prev_of(j); }
        bool term = j >= 0 && saterm(t[j]); int termT = term ? t[j] : -1;
        if (P == SB_ATerm && B == SB_Numeric) brk = false;
        else if (P == SB_ATerm && (PP == SB_Upper || PP == SB_Lower) && B == SB_Upper) brk = false;
        else if (term && termT == SB_ATerm && [&] {
                     for (std::size_t k = i; k < n; ++k) {
                         int x = t[k]; if (ign(k)) continue;
                         if (x == SB_Lower) return true;
                         if (x == SB_OLetter || x == SB_Upper || para(x) || saterm(x)) return false;
                     }
                     return false; }()) brk = false;
        else if (term && (B == SB_SContinue || saterm(B))) brk = false;
        else if (term && sp == 0 && (B == SB_Close || B == SB_Sp || para(B))) brk = false;
        else if (term && (B == SB_Sp || para(B))) brk = false;
        else if (term) brk = true;
        if (brk) r.push_back(i);
    }
    r.push_back(n);
    return r;
}

// ---------------------------------------------------------------- lines
std::vector<line_break> line_breaks32(std::u32string_view s) {
    std::vector<line_break> r;
    const long n = long(s.size());
    if (!n) return r;
    std::vector<int> c(n), e(n); std::vector<long> start(n);
    for (long i = 0; i < n; ++i) {
        auto& p = prop(s[i]); int x = p.lb;
        if (x == LB_AI || x == LB_SG || x == LB_XX) x = LB_AL;
        else if (x == LB_SA) x = (p.gc == 6 || p.gc == 7) ? LB_CM : LB_AL;   // Mn, Mc
        else if (x == LB_CJ) x = LB_NS;
        c[i] = x;
    }
    auto hard = [](int x) { return x == LB_BK || x == LB_CR || x == LB_LF || x == LB_NL || x == LB_SP || x == LB_ZW; };
    for (long i = 0; i < n; ++i) {
        if ((c[i] == LB_CM || c[i] == LB_ZWJ) && i > 0 && !hard(e[i - 1])) { e[i] = e[i - 1]; start[i] = start[i - 1]; }
        else { e[i] = (c[i] == LB_CM || c[i] == LB_ZWJ) ? LB_AL : c[i]; start[i] = i; }
    }
    auto base_cp = [&](long u) -> cp_t { long b = start[u]; return (c[b] == LB_CM || c[b] == LB_ZWJ) ? cp_t('A') : s[b]; };
    auto ea = [&](long u) { if (u < 0) return false; auto w = prop(base_cp(u)).eaw; return w == EA_F || w == EA_W || w == EA_H; };
    auto prevu = [&](long u) -> long { return u < 0 ? -1 : start[u] - 1; };   // index of last char of previous unit (or -1)
    auto nextu = [&](long i) -> long { long j = i + 1; while (j < n && start[j] != j) ++j; return j < n ? j : -1; };
    auto E = [&](long u) { return u < 0 ? -1 : e[u]; };
    auto gcat = [&](long u) { return int(prop(base_cp(u)).gc); };
    constexpr int Pi = 16, Pf = 17;
    for (long i = 1; i < n; ++i) {
        int A = c[i - 1], B = c[i];
        auto add = [&](bool mand) { r.push_back({std::size_t(i), mand}); };
        if (A == LB_BK) { add(true); continue; }
        if (A == LB_CR && B == LB_LF) continue;
        if (A == LB_CR || A == LB_LF || A == LB_NL) { add(true); continue; }
        if (B == LB_BK || B == LB_CR || B == LB_LF || B == LB_NL) continue;
        if (B == LB_SP || B == LB_ZW) continue;
        { long j = i - 1; while (j >= 0 && c[j] == LB_SP) --j; if (j >= 0 && c[j] == LB_ZW) { add(false); continue; } }
        if (A == LB_ZWJ) continue;
        if ((B == LB_CM || B == LB_ZWJ) && !hard(e[i - 1])) continue;   // LB9
        long P = i - 1;                       // last char of previous unit; E(P) is its effective class
        int Pc = e[P], Bc = e[i];
        long Nn = nextu(i); int Nc = E(Nn);
        auto skip_sp = [&](long u) { while (u >= 0 && e[u] == LB_SP) u = prevu(u); return u; };
        bool brk = true;
        if (Bc == LB_WJ || Pc == LB_WJ) brk = false;
        else if (Pc == LB_GL) brk = false;
        else if (Bc == LB_GL && Pc != LB_SP && Pc != LB_BA && Pc != LB_HY) brk = false;
        else if (Bc == LB_CL || Bc == LB_CP || Bc == LB_EX || Bc == LB_SY) brk = false;
        else if (E(skip_sp(P)) == LB_OP) brk = false;
        else if ([&] { long q = skip_sp(P); if (q < 0 || e[q] != LB_QU || gcat(q) != Pi) return false; long b = prevu(q);
                       int bc = E(b); return b < 0 || bc == LB_BK || bc == LB_CR || bc == LB_LF || bc == LB_NL || bc == LB_OP || bc == LB_QU || bc == LB_GL || bc == LB_SP || bc == LB_ZW; }()) brk = false;
        else if (Bc == LB_QU && gcat(i) == Pf && (Nn < 0 || Nc == LB_SP || Nc == LB_GL || Nc == LB_WJ || Nc == LB_CL || Nc == LB_QU || Nc == LB_CP || Nc == LB_EX ||
                 Nc == LB_IS || Nc == LB_SY || Nc == LB_BK || Nc == LB_CR || Nc == LB_LF || Nc == LB_NL || Nc == LB_ZW)) brk = false;
        else if (Pc == LB_SP && Bc == LB_IS && Nc == LB_NU) brk = true;
        else if (Bc == LB_IS) brk = false;
        else if (Bc == LB_NS && (E(skip_sp(P)) == LB_CL || E(skip_sp(P)) == LB_CP)) brk = false;
        else if (Bc == LB_B2 && E(skip_sp(P)) == LB_B2) brk = false;
        else if (Pc == LB_SP) brk = true;
        else if (Bc == LB_QU && gcat(i) != Pi) brk = false;
        else if (Pc == LB_QU && gcat(P) != Pf) brk = false;
        else if (Bc == LB_QU && !ea(P)) brk = false;
        else if (Bc == LB_QU && (Nn < 0 || !ea(Nn))) brk = false;
        else if (Pc == LB_QU && !ea(i)) brk = false;
        else if (Pc == LB_QU && (prevu(P) < 0 || !ea(prevu(P)))) brk = false;
        else if (Bc == LB_CB || Pc == LB_CB) brk = true;
        else if ((Pc == LB_HY || base_cp(P) == 0x2010) && Bc == LB_AL && [&] { long b = prevu(P); int bc = E(b);
                     return b < 0 || bc == LB_BK || bc == LB_CR || bc == LB_LF || bc == LB_NL || bc == LB_SP || bc == LB_ZW || bc == LB_CB || bc == LB_GL; }()) brk = false;
        else if (Bc == LB_BA || Bc == LB_HY || Bc == LB_NS || Pc == LB_BB) brk = false;
        else if (E(prevu(P)) == LB_HL && (Pc == LB_HY || (Pc == LB_BA && !ea(P))) && Bc != LB_HL) brk = false;
        else if (Pc == LB_SY && Bc == LB_HL) brk = false;
        else if (Bc == LB_IN) brk = false;
        else if ((Pc == LB_AL || Pc == LB_HL) && Bc == LB_NU) brk = false;
        else if (Pc == LB_NU && (Bc == LB_AL || Bc == LB_HL)) brk = false;
        else if (Pc == LB_PR && (Bc == LB_ID || Bc == LB_EB || Bc == LB_EM)) brk = false;
        else if ((Pc == LB_ID || Pc == LB_EB || Pc == LB_EM) && Bc == LB_PO) brk = false;
        else if ((Pc == LB_PR || Pc == LB_PO) && (Bc == LB_AL || Bc == LB_HL)) brk = false;
        else if ((Pc == LB_AL || Pc == LB_HL) && (Bc == LB_PR || Bc == LB_PO)) brk = false;
        else if ((Bc == LB_PO || Bc == LB_PR) && [&] { long q = P; if (e[q] == LB_CL || e[q] == LB_CP) q = prevu(q);
                     while (q >= 0 && (e[q] == LB_SY || e[q] == LB_IS)) q = prevu(q); return E(q) == LB_NU; }()) brk = false;
        else if ((Pc == LB_PO || Pc == LB_PR) && Bc == LB_OP && (Nc == LB_NU || (Nc == LB_IS && E(nextu(Nn)) == LB_NU))) brk = false;
        else if ((Pc == LB_PO || Pc == LB_PR || Pc == LB_HY || Pc == LB_IS) && Bc == LB_NU) brk = false;
        else if (Bc == LB_NU && [&] { long q = P; while (q >= 0 && (e[q] == LB_SY || e[q] == LB_IS)) q = prevu(q); return E(q) == LB_NU; }()) brk = false;
        else if (Pc == LB_JL && (Bc == LB_JL || Bc == LB_JV || Bc == LB_H2 || Bc == LB_H3)) brk = false;
        else if ((Pc == LB_JV || Pc == LB_H2) && (Bc == LB_JV || Bc == LB_JT)) brk = false;
        else if ((Pc == LB_JT || Pc == LB_H3) && Bc == LB_JT) brk = false;
        else if ((Pc == LB_JL || Pc == LB_JV || Pc == LB_JT || Pc == LB_H2 || Pc == LB_H3) && Bc == LB_PO) brk = false;
        else if (Pc == LB_PR && (Bc == LB_JL || Bc == LB_JV || Bc == LB_JT || Bc == LB_H2 || Bc == LB_H3)) brk = false;
        else if ((Pc == LB_AL || Pc == LB_HL) && (Bc == LB_AL || Bc == LB_HL)) brk = false;
        else if ([&] {
                     auto ak = [&](long u) { return u >= 0 && (e[u] == LB_AK || e[u] == LB_AS || base_cp(u) == 0x25CC); };
                     auto ak2 = [&](long u) { return u >= 0 && (e[u] == LB_AK || base_cp(u) == 0x25CC); };
                     if (Pc == LB_AP && ak(i)) return true;
                     if (ak(P) && (Bc == LB_VF || Bc == LB_VI)) return true;
                     if (Pc == LB_VI && ak(prevu(P)) && ak2(i)) return true;
                     if (ak(P) && ak(i) && Nc == LB_VF) return true;
                     return false; }()) brk = false;
        else if (Pc == LB_IS && (Bc == LB_AL || Bc == LB_HL)) brk = false;
        else if ((Pc == LB_AL || Pc == LB_HL || Pc == LB_NU) && Bc == LB_OP && !ea(i)) brk = false;
        else if (Pc == LB_CP && !ea(P) && (Bc == LB_AL || Bc == LB_HL || Bc == LB_NU)) brk = false;
        else if (Pc == LB_RI && Bc == LB_RI) { int k = 0; long q = P; while (q >= 0 && e[q] == LB_RI) { ++k; q = prevu(q); } brk = k % 2 == 0; }
        else if (Pc == LB_EB && Bc == LB_EM) brk = false;
        else if (Bc == LB_EM && (prop(base_cp(P)).flags & F_ExtPict) && prop(base_cp(P)).gc == 0) brk = false;
        if (brk) add(false);
    }
    r.push_back({std::size_t(n), false});
    return r;
}

// ---------------------------------------------------------------- UTF-8 front ends
std::vector<std::size_t> grapheme_breaks(std::string_view s) { return to_bytes(s, grapheme_breaks32); }
std::vector<std::size_t> word_breaks(std::string_view s) { return to_bytes(s, word_breaks32); }
std::vector<std::size_t> sentence_breaks(std::string_view s) { return to_bytes(s, sentence_breaks32); }
std::vector<line_break> line_breaks(std::string_view s) {
    auto v = to32(s); auto b = line_breaks32(v); auto off = cp_byte_offsets(s);
    for (auto& x : b) x.pos = off[x.pos];
    return b;
}
std::vector<std::string_view> graphemes(std::string_view s) { return pieces(s, grapheme_breaks(s)); }
std::vector<std::string_view> sentences(std::string_view s) { return pieces(s, sentence_breaks(s)); }
std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> r;
    for (auto w : pieces(s, word_breaks(s))) {
        std::size_t i = 0; bool keep = false;
        while (i < w.size()) { cp_t c = decode_utf8(w, i); if (is_alpha(c) || is_number(c) || is_ideographic(c)) { keep = true; break; } }
        if (keep) r.push_back(w);
    }
    return r;
}
std::size_t next_grapheme(std::string_view s, std::size_t pos) {
    if (pos >= s.size()) return s.size();
    // Rules are local except RI and emoji/InCB state; scan a bounded window starting at a safe point.
    auto b = grapheme_breaks(s.substr(pos));
    return pos + (b.size() > 1 ? b[1] : s.size() - pos);
}
std::size_t prev_grapheme(std::string_view s, std::size_t pos) {
    if (pos == 0) return 0;
    auto b = grapheme_breaks(s.substr(0, std::min(pos, s.size())));
    return b.size() >= 2 ? b[b.size() - 2] : 0;
}
std::size_t count_graphemes(std::string_view s) { return s.empty() ? 0 : grapheme_breaks(s).size() - 1; }
}  // namespace forge::os::text
