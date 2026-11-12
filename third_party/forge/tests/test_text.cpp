// forge::os::text unit tests (conformance suites live in test_text_conformance.cpp).
#include <forge/os/text.hpp>
#include <cstdio>
#include <string>
#include <vector>
namespace t = forge::os::text;
static int fails = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define EQ(a, b) do { ++checks; auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++fails; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #a " == " #b); } } while (0)

static void utf_props_case() {
    EQ(t::count_code_points("a\xC3\xA9\xF0\x9F\x98\x80"), 3u);
    CHECK(!t::is_valid_utf8("\xC0\x80"));
    EQ(t::sanitize_utf8("a\xFF" "b"), std::string("a\xEF\xBF\xBD" "b"));
    EQ(t::to_upper("stra\xC3\x9F" "e"), std::string("STRASSE"));
    EQ(t::to_upper("i", "tr"), std::string("\xC4\xB0"));
    EQ(t::to_title("hello wORLD"), std::string("Hello World"));
    CHECK(t::equals_ignore_case("Stra\xC3\x9F" "e", "STRASSE"));
    EQ(t::normalize("e\xCC\x81"), std::string("\xC3\xA9"));
    EQ(std::string(t::script(U'\u03A9')), std::string("Greek"));
    CHECK(t::has_property(U'\u0663', "Nd"));
    EQ(t::count_graphemes("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7" "e\xCC\x81"), 2u);
    EQ(t::display_width("\xE4\xB8\xAD\xE6\x96\x87" "ab"), 6);
    EQ(t::base_direction("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D abc"), t::direction::rtl);
}
static void collation() {
    std::vector<std::string> v{"\xC3\xA4pple", "zebra", "apple", "Apple"};
    t::sort(v, "sv");
    EQ(v.back(), std::string("\xC3\xA4pple"));
    t::sort(v);
    EQ(v[0], std::string("apple"));
    t::collator num({}, {.numeric = true});
    CHECK(num.compare("file9", "file10") < 0);
}
static void encodings() {
    auto sj = t::decode("\x93\xFA\x96\x7B", "shift_jis"); CHECK(sj); EQ(sj.value, std::string("\xE6\x97\xA5\xE6\x9C\xAC"));
    auto back = t::encode("\xE6\x97\xA5\xE6\x9C\xAC", "sjis"); CHECK(back); EQ(back.value, std::string("\x93\xFA\x96\x7B"));
    auto gbk = t::encode("\xE4\xB8\xAD", "gbk"); CHECK(gbk); EQ(gbk.value, std::string("\xD6\xD0"));
    EQ(t::decode(gbk.value, "gb2312").value, std::string("\xE4\xB8\xAD"));
    auto gb = t::encode("\xF0\x9F\x98\x80", "gb18030"); CHECK(gb); EQ(t::decode(gb.value, "gb18030").value, std::string("\xF0\x9F\x98\x80"));
    EQ(t::decode("\xE9", "latin1").value, std::string("\xC3\xA9"));
    EQ(t::decode("\x80", "windows-1252").value, std::string("\xE2\x82\xAC"));
    EQ(t::decode("\xC9", "cp437").value, std::string("\xE2\x95\x94"));
    EQ(t::decode("\xC1", "koi8-r").value, std::string("\xD0\xB0"));
    EQ(t::decode("\xD0\xFD", "iso-8859-9").value, std::string("\xC4\x9E\xC4\xB1"));   // WHATWG: Latin-5 is windows-1254
    EQ(t::decode("\xD0", "latin5").value, std::string("\xC4\x9E"));
    EQ(t::decode("\x80", "ISO8859-9").value, std::string("\xE2\x82\xAC"));
    EQ(t::decode("\xA4\xA4", "big5").value, std::string("\xE4\xB8\xAD"));
    EQ(t::decode("\xB0\xA1", "euc-kr").value, std::string("\xEA\xB0\x80"));
    EQ(t::decode("\xC6\xFC", "euc-jp").value, std::string("\xE6\x97\xA5"));
    EQ(t::decode("\x1B$B\x46\x7C\x1B(B", "iso-2022-jp").value, std::string("\xE6\x97\xA5"));
    EQ(t::encode("a\xE6\x97\xA5" "b", "iso-2022-jp").value, std::string("a\x1B$B\x46\x7C\x1B(Bb"));
    EQ(t::sniff_charset("\x1B$B\x46\x7C\x1B(B").charset, std::string("iso-2022-jp"));
    std::string gbt = t::encode("\xE4\xB8\xAD\xE5\x8D\x8E\xE4\xBA\xBA\xE6\xB0\x91\xE5\x85\xB1\xE5\x92\x8C\xE5\x9B\xBD\xE6\x88\x90\xE7\xAB\x8B\xE4\xBA\x86", "gbk").value;
    EQ(t::sniff_charset(gbt).charset, std::string("gbk"));
    CHECK(!t::encode("\xE6\x97\xA5", "latin1"));
    EQ(t::encode("\xE6\x97\xA5", "latin1", t::on_error::replace).value, std::string("?"));
    CHECK(!t::decode("x", "nonsense"));
    EQ(t::detect_bom("\xEF\xBB\xBFhi").charset, std::string("utf-8"));
    EQ(t::detect_bom(std::string("\xFF\xFEh\0", 4)).length, 2u);
    EQ(t::sniff_charset("plain ascii").charset, std::string("utf-8"));
    std::string sj_text = "\x82\xB1\x82\xEA\x82\xCD\x93\xFA\x96\x7B\x8C\xEA\x82\xCC\x83\x65\x83\x4C\x83\x58\x83\x67\x82\xC5\x82\xB7\x81\x42";
    EQ(t::sniff_charset(sj_text).charset, std::string("shift_jis"));
    std::string cyr = t::encode("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD0\xBA\xD0\xB0\xD0\xBA \xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB0? \xD0\xAD\xD1\x82\xD0\xBE \xD1\x82\xD0\xB5\xD1\x81\xD1\x82.", "windows-1251").value;
    EQ(t::sniff_charset(cyr).charset, std::string("windows-1251"));
    std::string used; EQ(t::to_utf8_auto("\xEF\xBB\xBFok", &used), std::string("ok"));
    EQ(t::decode(std::string("h\0i\0", 4), "utf-16le").value, std::string("hi"));
}
static void translit() {
    EQ(t::to_ascii("\xC3\x86r\xC3\xB8sk\xC3\xB8""bing Stra\xC3\x9F" "e"), std::string("AEroskobing Strasse"));
    EQ(t::to_ascii("\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0"), std::string("Moskva"));
    EQ(t::to_ascii("\xCE\x91\xCE\xB8\xCE\xAE\xCE\xBD\xCE\xB1"), std::string("Athina"));
    EQ(t::to_ascii("\xE2\x80\x9CQuote\xE2\x80\x9D \xE2\x80\x94 \xE2\x82\xAC" "5"), std::string("\"Quote\" - EUR5"));
    EQ(t::remove_marks("cr\xC3\xA8me br\xC3\xBBl\xC3\xA9" "e"), std::string("creme brulee"));
    EQ(t::slugify("Hello, World! \xC3\x87" "a va?"), std::string("hello-world-ca-va"));
    EQ(t::slugify("Don't Stop  Me", '_'), std::string("dont_stop_me"));
    EQ(t::slugify("one two three four", '-', 12), std::string("one-two"));
}
static void fuzzy() {
    EQ(t::levenshtein("kitten", "sitting"), 3u);
    EQ(t::levenshtein("", "abc"), 3u);
    EQ(t::levenshtein("caf\xC3\xA9", "cafe"), 1u);
    EQ(t::damerau_levenshtein("ca", "ac"), 1u);
    EQ(t::levenshtein("ca", "ac"), 2u);
    CHECK(t::jaro_winkler("MARTHA", "MARHTA") > 0.96 && t::jaro_winkler("MARTHA", "MARHTA") < 0.962);
    CHECK(t::similarity("abc", "abc") == 1.0);
    CHECK(!t::fuzzy_score("xyz", "abc"));
    std::vector<std::size_t> pos;
    CHECK(t::fuzzy_score("fb", "foo_bar", &pos)); EQ(pos.size(), 2u); EQ(pos[1], 4u);
    std::vector<std::string> items{"src/main.cpp", "include/forge/main_window.hpp", "docs/maintenance.md", "makefile"};
    auto hits = t::fuzzy_filter("main", items);
    EQ(hits.size(), 3u);
    EQ(hits[0].index, 0u);
    CHECK(*t::fuzzy_score("gc", "getConfig") > *t::fuzzy_score("gc", "bagicon"));
}
static t::regex rx(const char* p, const char* f = "") { auto r = t::regex::compile(p, f); if (!r) std::printf("compile %s: %s\n", p, r.err.what.c_str()); return r.value; }
static void regex() {
    CHECK(rx("a+b").match("aaab"));
    CHECK(!rx("a+b").match("aaabc"));
    CHECK(rx("(a|ab)(c|bcd)(d*)").match("abcd"));
    auto m = rx("(?<y>\\d{4})-(?<m>\\d\\d)").search("on 2026-10-31");
    CHECK(m); EQ(std::string(m->str("on 2026-10-31", *m->group_index("m"))), std::string("10"));
    EQ(rx("(\\w+)@(\\w+)").replace("me@host you@there", "$2:$1"), std::string("host:me there:you"));
    EQ(rx("\\s*,\\s*").split("a , b,c").size(), 3u);
    CHECK(rx("\\p{Greek}+").match("\xCE\xB1\xCE\xB2\xCE\xB3"));
    CHECK(rx("\\p{Lu}\\p{Ll}+").match("\xC3\x89t\xC3\xA9"));
    CHECK(rx("stra\\u00dfe", "i").match("STRA\xC3\x9F" "E"));
    CHECK(rx("[a-z]+", "i").match("HeLLo"));
    CHECK(rx("(\\w)\\1").search("hello"));
    CHECK(rx("foo(?=bar)").search("foobar") && !rx("foo(?!bar)").search("foobar"));
    EQ(rx("a*?").search("aaa")->end(), 0u);
    EQ(rx("^\\w+$", "m").find_all("one\ntwo\nthree").size(), 3u);
    EQ(rx("\\X").find_all("e\xCC\x81\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5").size(), 2u);
    CHECK(rx("(a*)*b").match("aaab"));
    auto evil = rx("(a*)*c"); evil.set_step_limit(100000);
    CHECK(!evil.match(std::string(40, 'a'))); CHECK(evil.step_limit_hit());
    CHECK(!t::regex::compile("a(b"));
    CHECK(!t::regex::compile("*a"));
    CHECK(!t::regex::compile("\\p{NoSuchThing}"));
    CHECK(rx(" a  b # comment", "x").match("ab"));
    CHECK(rx("[[:alpha:]]+\\d").match("abc1"));
    EQ(rx("").find_all("ab").size(), 3u);
    EQ(rx("x*").replace("abc", "-"), std::string("-a-b-c-"));
}
static void console() {
    t::text_style st; st.bold = true; st.fg = t::rgb{255, 0, 0};
    EQ(t::style("x", st), std::string("\x1b[1;38;2;255;0;0mx\x1b[0m"));
    EQ(t::style("x", st, t::color_level::ansi256), std::string("\x1b[1;38;5;196mx\x1b[0m"));
    EQ(t::style("x", st, t::color_level::basic), std::string("\x1b[1;91mx\x1b[0m"));
    EQ(t::style("x", st, t::color_level::none), std::string("x"));
    {   // OSC 8: nothing at color_level::none; controls in the URI cannot close the OSC string early
        t::text_style ln; ln.link = "http://a/\x1b\\\x07" "b\xC2\x9C" "c\xC3\xA9";
        EQ(t::style("x", ln, t::color_level::none), std::string("x"));
        EQ(t::style("x", ln, t::color_level::basic), std::string("\x1b]8;;http://a/\\bc\xC3\xA9\x1b\\x\x1b]8;;\x1b\\"));
    }
    auto ps = t::parse_style("bold red on #202020 underline"); CHECK(ps && ps->bold && ps->fg16 == 1 && ps->bg && ps->bg->r == 0x20 && ps->underline);
    CHECK(!t::parse_style("bold chartreuse-ish"));
    EQ(t::strip_ansi("\x1b[31mred\x1b[0m \x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\"), std::string("red link"));
    EQ(t::display_width_ansi("\x1b[1m\xE4\xB8\xAD\x1b[0m"), 2);
    EQ(t::truncate("hello world", 8), std::string("hello w\xE2\x80\xA6"));
    EQ(t::truncate("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97", 4, "."), std::string("\xE4\xB8\xAD."));
    EQ(t::truncate("short", 10), std::string("short"));
    EQ(t::pad("ab", 5, t::align::right), std::string("   ab"));
    EQ(t::pad("ab", 6, t::align::center, U'*'), std::string("**ab**"));
    t::wrap_options wo; wo.width = 10;
    auto ls = t::wrap_lines("The quick brown fox jumps over", wo);
    EQ(ls.size(), 3u); EQ(ls[0], std::string("The quick")); EQ(ls[2], std::string("jumps over"));
    wo.width = 4; EQ(t::wrap_lines("abcdefghij", wo).size(), 3u);
    wo.width = 20; EQ(t::wrap_lines("one\ntwo", wo).size(), 2u);
    wo.width = 12; wo.indent = "* "; wo.subsequent_indent = "  ";
    EQ(t::wrap("alpha beta gamma delta", wo), std::string("* alpha beta\n  gamma\n  delta"));
    auto sz = t::terminal_size(); CHECK(sz.cols > 0 && sz.rows > 0);
    if (t::is_terminal(1)) CHECK(t::enable_vt());   // Windows: no console mode when stdout is a pipe (ctest)
}
int main() {
    utf_props_case(); collation(); encodings(); translit(); fuzzy(); regex(); console();
    std::printf("test_text: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
