// Unicode conformance: NormalizationTest, Grapheme/Word/Sentence/LineBreakTest, BidiCharacterTest, CollationTest.
// Data: $FORGE_UCD_DIR or tests/data/ucd (cmake -DFORGE_FETCH_UCD=ON downloads them). Exit 77 when absent.
#include <forge/os/text.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
namespace t = forge::os::text;
namespace fs = std::filesystem;

static fs::path dir;
static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> r; std::string cur; for (char c : s) { if (c == d) { r.push_back(cur); cur.clear(); } else cur += c; } r.push_back(cur); return r;
}
static std::u32string hexseq(const std::string& s) {
    std::u32string r; std::istringstream in(s); std::string h; while (in >> h) r += char32_t(std::stoul(h, nullptr, 16)); return r;
}
static std::string strip_comment(std::string l) { if (auto p = l.find('#'); p != std::string::npos) l.resize(p); return l; }
struct tally { const char* name; long pass = 0, fail = 0; int shown = 0;
    void report() const { std::printf("%-28s %7ld / %7ld  (%.3f%%)\n", name, pass, pass + fail, 100.0 * pass / std::max(1L, pass + fail)); } };
static std::vector<tally> results;

static void normalization() {
    std::ifstream f(dir / "NormalizationTest.txt"); if (!f) return;
    tally T{"NormalizationTest"}; std::string l;
    while (std::getline(f, l)) {
        l = strip_comment(l); if (l.empty() || l[0] == '@') continue;
        auto p = split(l, ';'); if (p.size() < 5) continue;
        std::u32string c[5]; for (int i = 0; i < 5; ++i) c[i] = hexseq(p[i]);
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            ok &= t::normalize32(c[i], t::norm_form::nfc) == c[1];
            ok &= t::normalize32(c[i], t::norm_form::nfd) == c[2];
        }
        for (int i = 0; i < 5; ++i) { ok &= t::normalize32(c[i], t::norm_form::nfkc) == c[3]; ok &= t::normalize32(c[i], t::norm_form::nfkd) == c[4]; }
        if (ok) ++T.pass; else { ++T.fail; if (T.shown++ < 5) std::printf("  norm fail: %s\n", l.c_str()); }
    }
    results.push_back(T);
}
template <class F> static void breaks(const char* file, const char* name, F&& fn) {
    std::ifstream f(dir / file); if (!f) return;
    tally T{name}; std::string l;
    while (std::getline(f, l)) {
        auto orig = l; l = strip_comment(l); if (l.find_first_not_of(" \t") == std::string::npos) continue;
        std::istringstream in(l); std::string tok; std::u32string s; std::vector<std::size_t> want;
        while (in >> tok) {
            if (tok == "\xC3\xB7") want.push_back(s.size());          // ÷
            else if (tok == "\xC3\x97") {}                             // ×
            else s += char32_t(std::stoul(tok, nullptr, 16));
        }
        auto got = fn(s);
        if (got == want) ++T.pass; else { ++T.fail; if (T.shown++ < 8) { std::printf("  %s fail: %s\n   got:", name, orig.substr(0, 160).c_str()); for (auto x : got) std::printf(" %zu", x); std::printf("\n"); } }
    }
    results.push_back(T);
}
static void bidi() {
    std::ifstream f(dir / "BidiCharacterTest.txt"); if (!f) return;
    tally T{"BidiCharacterTest"}; std::string l;
    while (std::getline(f, l)) {
        l = strip_comment(l); if (l.empty()) continue;
        auto p = split(l, ';'); if (p.size() < 5) continue;
        auto s = hexseq(p[0]); int dir = std::stoi(p[1]);
        auto r = t::bidi_levels32(s, dir == 0 ? t::direction::ltr : dir == 1 ? t::direction::rtl : t::direction::auto_);
        bool ok = r.paragraph_level == std::stoi(p[2]);
        std::istringstream lv(p[3]); std::string x; std::size_t i = 0;
        while (lv >> x) { if (i >= r.levels.size()) { ok = false; break; } if (x == "x") ok &= r.levels[i] == 0xFF; else ok &= r.levels[i] == std::stoi(x); ++i; }
        if (ok) { auto ord = t::bidi_reorder32(s, r); std::istringstream o(p[4]); std::vector<std::size_t> want; std::size_t k; while (o >> k) want.push_back(k); ok = ord == want; }
        if (ok) ++T.pass; else { ++T.fail; if (T.shown++ < 5) std::printf("  bidi fail: %s\n", l.substr(0, 200).c_str()); }
    }
    results.push_back(T);
}
static void collation(const char* file, bool shifted) {
    std::ifstream f(dir / file); if (!f) f.open(dir / "CollationTest" / file); if (!f) return;
    tally T{shifted ? "CollationTest_SHIFTED" : "CollationTest_NON_IGNORABLE"}; std::string l;
    t::collation_options o; o.level = t::strength::identical; o.shifted = shifted;
    t::collator c("", o); std::string prev; bool have = false; std::u32string prevs;
    while (std::getline(f, l)) {
        l = strip_comment(l); if (l.find_first_not_of(" \t") == std::string::npos) continue;
        auto sc = l.find(';'); auto s = hexseq(l.substr(0, sc));
        auto k = c.sort_key32(s);
        if (have) { if (prev <= k) ++T.pass; else { ++T.fail; if (T.shown++ < 5) std::printf("  coll fail: %s\n", l.c_str()); } }
        prev = std::move(k); have = true; prevs = s;
    }
    results.push_back(T);
}
int main() {
    if (auto e = std::getenv("FORGE_UCD_DIR")) dir = e; else dir = "tests/data/ucd";
    if (!fs::exists(dir / "NormalizationTest.txt") && !fs::exists(dir / "auxiliary/GraphemeBreakTest.txt") && !fs::exists(dir / "GraphemeBreakTest.txt")) {
        std::printf("no UCD test data in %s (set FORGE_UCD_DIR or -DFORGE_FETCH_UCD=ON); skipping\n", dir.string().c_str()); return 77;
    }
    auto aux = [&](const char* f) { return fs::exists(dir / f) ? std::string(f) : std::string("auxiliary/") + f; };
    normalization();
    breaks(aux("GraphemeBreakTest.txt").c_str(), "GraphemeBreakTest", [](auto& s) { return t::grapheme_breaks32(s); });
    breaks(aux("WordBreakTest.txt").c_str(), "WordBreakTest", [](auto& s) { return t::word_breaks32(s); });
    breaks(aux("SentenceBreakTest.txt").c_str(), "SentenceBreakTest", [](auto& s) { return t::sentence_breaks32(s); });
    breaks(aux("LineBreakTest.txt").c_str(), "LineBreakTest", [](auto& s) {
        std::vector<std::size_t> r; for (auto& b : t::line_breaks32(s)) r.push_back(b.pos); return r; });
    bidi();
    collation("CollationTest_NON_IGNORABLE_SHORT.txt", false);
    collation("CollationTest_SHIFTED_SHORT.txt", true);
    bool all = true;
    for (auto& r : results) { r.report(); if (r.fail) all = false; }
    // Required floors: these must stay at 100%; the others are reported (see docs/STATUS.md).
    for (auto& r : results) {
        std::string n = r.name;
        double rate = double(r.pass) / std::max(1L, r.pass + r.fail);
        double floor = 1.0;   // every suite passes in full (Unicode 16.0.0); any regression fails the test
        if (rate < floor) { std::printf("FAIL: %s below %.0f%%\n", r.name, floor * 100); return 1; }
    }
    (void)all;
    return 0;
}
