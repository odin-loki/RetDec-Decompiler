// forge::os::text: terminal queries, ANSI styling, and width-aware wrap / truncate / pad.
#include "internal.hpp"
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

namespace forge::os::text {
using namespace detail;

namespace {
int env_int(const char* n) { const char* v = std::getenv(n); return v ? std::atoi(v) : 0; }
#if defined(_WIN32)
HANDLE handle_for(int fd) { return GetStdHandle(fd == 2 ? STD_ERROR_HANDLE : fd == 0 ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE); }
// mintty (MSYS2, Cygwin and the Windows shells built on them) gives programs named pipes, not consoles:
// "\\msys-<hash>-pty0-to-master" or "\\cygwin-<hash>-pty0-from-master".
bool is_mintty_pipe(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE || !h || GetFileType(h) != FILE_TYPE_PIPE) return false;
    struct { DWORD len; WCHAR name[MAX_PATH]; } info{};
    if (!GetFileInformationByHandleEx(h, FileNameInfo, &info, sizeof info)) return false;
    std::wstring n(info.name, std::min<std::size_t>(info.len / sizeof(WCHAR), MAX_PATH));
    bool cyg = n.find(L"msys-") != std::wstring::npos || n.find(L"cygwin-") != std::wstring::npos;
    return cyg && n.find(L"-pty") != std::wstring::npos && (n.find(L"-from-master") != std::wstring::npos || n.find(L"-to-master") != std::wstring::npos);
}
HANDLE crt_handle(int fd) { auto h = reinterpret_cast<HANDLE>(_get_osfhandle(fd)); return h == INVALID_HANDLE_VALUE ? handle_for(fd) : h; }
UINT g_saved_cp = 0;
void restore_cp() { if (g_saved_cp) SetConsoleOutputCP(g_saved_cp); }
#endif
}  // namespace

bool is_terminal(int fd) {
#if defined(_WIN32)
    return _isatty(fd) != 0 || is_mintty_pipe(crt_handle(fd));
#else
    return ::isatty(fd) == 1;
#endif
}
term_size terminal_size(int fd) {
    term_size t;
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (GetConsoleScreenBufferInfo(handle_for(fd), &i)) { t.cols = i.srWindow.Right - i.srWindow.Left + 1; t.rows = i.srWindow.Bottom - i.srWindow.Top + 1; return t; }
#else
    struct winsize w {};
    if (::ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_col) { t.cols = w.ws_col; t.rows = w.ws_row ? w.ws_row : 24; return t; }
#endif
    if (int c = env_int("COLUMNS"); c > 0) t.cols = c;
    if (int r = env_int("LINES"); r > 0) t.rows = r;
    return t;
}
status enable_vt(int fd) {
    status s;
#if defined(_WIN32)
    HANDLE h = handle_for(fd); DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) { s.err = last_error("enable_vt"); return s; }
    if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) && !SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) { s.err = last_error("enable_vt"); return s; }
    // The output code page belongs to the console (every process attached to it), not to this process: switch it to
    // UTF-8 once and put the previous one back at exit so the parent shell is left as it was.
    if (UINT cp = GetConsoleOutputCP(); cp && cp != CP_UTF8) {
        static std::once_flag once;
        std::call_once(once, [cp] { if (SetConsoleOutputCP(CP_UTF8)) { g_saved_cp = cp; std::atexit(restore_cp); } });
    }
#else
    (void)fd;
#endif
    return s;
}
color_level color_support(int fd) {
    auto env = [](const char* n) -> std::string_view { const char* v = std::getenv(n); return v ? v : ""; };
    if (!env("NO_COLOR").empty()) return color_level::none;
    auto force = env("FORCE_COLOR");
    if (!force.empty()) {
        if (force == "0" || force == "false") return color_level::none;
        if (force == "2") return color_level::ansi256;
        if (force == "3") return color_level::truecolor;
        if (force == "1" || force == "true") return color_level::basic;
    }
    bool forced = !force.empty() || (!env("CLICOLOR_FORCE").empty() && env("CLICOLOR_FORCE") != "0");
    if (!forced && !is_terminal(fd)) return color_level::none;
    auto ct = env("COLORTERM");
    if (ct == "truecolor" || ct == "24bit") return color_level::truecolor;
    auto term = env("TERM");
    if (term == "dumb") return forced ? color_level::basic : color_level::none;
#if defined(_WIN32)
    if (env("TERM_PROGRAM") == "mintty" || is_mintty_pipe(crt_handle(fd))) return color_level::truecolor;   // mintty 2.x+
    if (term.empty()) {
        // A console renders escapes only with VT processing on (enable_vt); Windows Terminal always has it.
        if (!env("WT_SESSION").empty()) return color_level::truecolor;
        DWORD mode = 0;
        if (GetConsoleMode(crt_handle(fd), &mode)) return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) ? color_level::truecolor : (forced ? color_level::basic : color_level::none);
        return forced ? color_level::basic : color_level::none;
    }
#endif
    if (env("TERM_PROGRAM") == "iTerm.app" || env("TERM_PROGRAM") == "WezTerm" || term.find("kitty") != std::string_view::npos || term.find("direct") != std::string_view::npos) return color_level::truecolor;
    if (term.find("256") != std::string_view::npos) return color_level::ansi256;
    if (!term.empty() || forced) return color_level::basic;
    return color_level::none;
}

namespace {
int rgb_to_256(rgb c) {
    auto q = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    int r = q(c.r), g = q(c.g), b = q(c.b);
    static const int lv[] = {0, 95, 135, 175, 215, 255};
    int cube = 16 + 36 * r + 6 * g + b;
    int cd = (lv[r] - c.r) * (lv[r] - c.r) + (lv[g] - c.g) * (lv[g] - c.g) + (lv[b] - c.b) * (lv[b] - c.b);
    int avg = (c.r + c.g + c.b) / 3, gi = avg > 238 ? 23 : std::max(0, (avg - 3) / 10), gv = 8 + 10 * gi;
    int gd = (gv - c.r) * (gv - c.r) + (gv - c.g) * (gv - c.g) + (gv - c.b) * (gv - c.b);
    return gd < cd ? 232 + gi : cube;
}
constexpr rgb pal16[16] = {{0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0}, {0, 0, 238}, {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
                           {127, 127, 127}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
int rgb_to_16(rgb c) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; ++i) { int d = (pal16[i].r - c.r) * (pal16[i].r - c.r) + (pal16[i].g - c.g) * (pal16[i].g - c.g) + (pal16[i].b - c.b) * (pal16[i].b - c.b); if (d < bd) { bd = d; best = i; } }
    return best;
}
void color_code(std::string& o, bool bg, const std::optional<rgb>& c, int c16, color_level lv) {
    if (c) {
        if (lv == color_level::truecolor) { o += (bg ? "48;2;" : "38;2;") + std::to_string(c->r) + ';' + std::to_string(c->g) + ';' + std::to_string(c->b) + ';'; return; }
        if (lv == color_level::ansi256) { o += (bg ? "48;5;" : "38;5;") + std::to_string(rgb_to_256(*c)) + ';'; return; }
        c16 = rgb_to_16(*c);
    }
    if (c16 >= 0) o += std::to_string((c16 < 8 ? (bg ? 40 : 30) : (bg ? 100 : 90)) + (c16 & 7)) + ';';
}
}  // namespace

std::string style(std::string_view s, const text_style& st, color_level lv) {
    std::string o;
    if (lv == color_level::none) return std::string(s);   // no escapes at all, hyperlinks included
    // The URI goes inside an OSC string: C0/C1 controls (ESC, BEL, the 8-bit ST) would end it early and let the
    // rest of the link run as terminal commands, so they are dropped (UTF-8 continuation bytes 0x80-0x9F stay).
    std::string link;
    for (std::size_t i = 0; i < st.link.size(); ++i) {
        unsigned char c = (unsigned char)st.link[i];
        if (c < 0x20 || c == 0x7F) continue;
        if (c == 0xC2 && i + 1 < st.link.size() && (unsigned char)st.link[i + 1] >= 0x80 && (unsigned char)st.link[i + 1] <= 0x9F) { ++i; continue; }   // U+0080-U+009F
        link += char(c);
    }
    std::string sgr;
    if (st.bold) sgr += "1;"; if (st.dim) sgr += "2;"; if (st.italic) sgr += "3;"; if (st.underline) sgr += "4;";
    if (st.blink) sgr += "5;"; if (st.reverse) sgr += "7;"; if (st.strike) sgr += "9;";
    color_code(sgr, false, st.fg, st.fg16, lv); color_code(sgr, true, st.bg, st.bg16, lv);
    if (!link.empty()) o += "\x1b]8;;" + link + "\x1b\\";
    if (!sgr.empty()) { sgr.pop_back(); o += "\x1b[" + sgr + "m"; }
    o.append(s);
    if (!sgr.empty()) o += "\x1b[0m";
    if (!link.empty()) o += "\x1b]8;;\x1b\\";
    return o;
}

std::optional<text_style> parse_style(std::string_view spec) {
    static const char* names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
    text_style st; bool on = false;
    auto color = [&](std::string_view w) -> bool {
        auto set = [&](int c16, std::optional<rgb> c) { if (on) { st.bg16 = c16; st.bg = c; } else { st.fg16 = c16; st.fg = c; } };
        if (w.size() == 7 && w[0] == '#') {
            unsigned v = 0; for (char c : w.substr(1)) { if (!std::isxdigit((unsigned char)c)) return false; v = v * 16 + unsigned(std::isdigit((unsigned char)c) ? c - '0' : (c | 0x20) - 'a' + 10); }
            set(-1, rgb{std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)}); return true;
        }
        if (!w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; }) && w.size() <= 3) {
            int n = std::atoi(std::string(w).c_str()); if (n > 255) return false;
            if (n < 16) { set(n, std::nullopt); return true; }
            rgb c;
            if (n >= 232) { auto g = std::uint8_t(8 + 10 * (n - 232)); c = {g, g, g}; }
            else { n -= 16; static const int lv[] = {0, 95, 135, 175, 215, 255}; c = {std::uint8_t(lv[n / 36]), std::uint8_t(lv[(n / 6) % 6]), std::uint8_t(lv[n % 6])}; }
            set(-1, c); return true;
        }
        bool bright = false; std::string_view base = w;
        if (base.starts_with("bright_")) { bright = true; base.remove_prefix(7); } else if (base.starts_with("bright")) { bright = true; base.remove_prefix(6); }
        if (base == "gray" || base == "grey") { set(8, std::nullopt); return true; }
        for (int i = 0; i < 8; ++i) if (base == names[i]) { set(i + (bright ? 8 : 0), std::nullopt); return true; }
        return false;
    };
    std::size_t i = 0;
    while (i < spec.size()) {
        while (i < spec.size() && (spec[i] == ' ' || spec[i] == ',')) ++i;
        std::size_t j = i; while (j < spec.size() && spec[j] != ' ' && spec[j] != ',') ++j;
        if (j == i) break;
        std::string w(spec.substr(i, j - i)); for (auto& c : w) c = char(std::tolower((unsigned char)c));
        i = j;
        if (w == "on") { on = true; continue; }
        if (w == "bold") st.bold = true; else if (w == "dim" || w == "faint") st.dim = true; else if (w == "italic") st.italic = true;
        else if (w == "underline") st.underline = true; else if (w == "blink") st.blink = true; else if (w == "reverse" || w == "inverse") st.reverse = true;
        else if (w == "strike" || w == "strikethrough") st.strike = true;
        else if (w.starts_with("link=")) st.link = w.substr(5);
        else if (!color(w)) return std::nullopt;
        else on = false;
    }
    return st;
}

namespace {
// Length of an escape sequence starting at s[i] (ESC or C1 CSI), 0 if none.
std::size_t esc_len(std::string_view s, std::size_t i) {
    if (s[i] != '\x1b' || i + 1 >= s.size()) return 0;
    char k = s[i + 1]; std::size_t j = i + 2;
    if (k == '[') { while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7E)) ++j; return std::min(j + 1, s.size()) - i; }
    if (k == ']' || k == 'P' || k == '_' || k == '^') {   // OSC/DCS/APC/PM until BEL or ST
        while (j < s.size()) { if (s[j] == '\x07') return j + 1 - i; if (s[j] == '\x1b' && j + 1 < s.size() && s[j + 1] == '\\') return j + 2 - i; ++j; }
        return s.size() - i;
    }
    return 2;
}
}  // namespace

std::string strip_ansi(std::string_view s) {
    std::string o; o.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) { if (auto n = esc_len(s, i)) { i += n; continue; } o += s[i++]; }
    return o;
}
int display_width_ansi(std::string_view s, width_options o) { return display_width(strip_ansi(s), o); }

namespace {
// Splits s into graphemes and zero-width escape tokens, preserving order.
struct tok { std::string_view t; int w; bool esc; };
std::vector<tok> tokens(std::string_view s, width_options o) {
    std::vector<tok> r; std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = i; while (j < s.size() && !esc_len(s, j)) ++j;
        if (j > i) for (auto g : graphemes(s.substr(i, j - i))) r.push_back({g, display_width(g, o), false});
        if (j < s.size()) { auto n = esc_len(s, j); r.push_back({s.substr(j, n), 0, true}); j += n; }
        i = j;
    }
    return r;
}
}  // namespace

std::string truncate(std::string_view s, int max_width, std::string_view ellipsis, width_options o) {
    if (display_width_ansi(s, o) <= max_width) return std::string(s);
    int ew = display_width(ellipsis, o);
    if (ew > max_width) { ellipsis = ""; ew = 0; }
    int budget = max_width - ew, used = 0; std::string out; bool had_esc = false, cut = false;
    for (auto& t : tokens(s, o)) {
        if (t.esc) { if (!cut) { out.append(t.t); had_esc = true; } continue; }
        if (cut) continue;
        if (used + t.w > budget) { cut = true; continue; }
        out.append(t.t); used += t.w;
    }
    out.append(ellipsis);
    if (had_esc) out += "\x1b[0m";
    return out;
}
std::string pad(std::string_view s, int w, align a, char32_t fill, width_options o) {
    int cur = display_width_ansi(s, o), fw = std::max(1, width(fill, o)); if (cur >= w) return std::string(s);
    int n = (w - cur) / fw, left = a == align::left ? 0 : a == align::right ? n : n / 2, right = n - left;
    std::string f; append_utf8(f, fill);
    std::string out; for (int k = 0; k < left; ++k) out += f; out.append(s); for (int k = 0; k < right; ++k) out += f;
    return out;
}

std::vector<std::string> wrap_lines(std::string_view s, const wrap_options& o) {
    std::vector<std::string> lines;
    std::string cur = o.indent; int curw = display_width_ansi(o.indent, o.w);
    bool line_has_text = false;
    auto newline = [&] {
        while (!cur.empty() && cur.back() == ' ') { cur.pop_back(); --curw; }
        lines.push_back(std::move(cur)); cur = o.subsequent_indent; curw = display_width_ansi(cur, o.w); line_has_text = false;
    };
    // Split on UAX #14 opportunities; each segment is a word with its trailing spaces.
    auto plain = strip_ansi(s);
    bool has_esc = plain.size() != s.size();
    std::string_view src = has_esc ? std::string_view(plain) : s;   // escapes are dropped when wrapping (they would span lines)
    std::size_t last = 0;
    for (auto& b : line_breaks(src)) {
        auto seg = src.substr(last, b.pos - last); last = b.pos;
        // Separate trailing whitespace / newline.
        std::size_t e = seg.size(); while (e > 0 && (seg[e - 1] == ' ' || seg[e - 1] == '\n' || seg[e - 1] == '\r' || seg[e - 1] == '\t')) --e;
        auto word = seg.substr(0, e); auto trail = seg.substr(e);
        int ww = display_width(word, o.w);
        if (line_has_text && curw + ww > o.width) newline();
        if (ww > o.width - curw && o.break_long_words) {
            for (auto g : graphemes(word)) {
                int gw = display_width(g, o.w);
                if (curw + gw > o.width && line_has_text) newline();
                cur.append(g); curw += gw; line_has_text = true;
            }
        } else if (!word.empty()) { cur.append(word); curw += ww; line_has_text = true; }
        bool hard = b.mandatory && trail.find_first_of("\n\r") != std::string_view::npos;
        if (hard) { newline(); continue; }
        if (!trail.empty() && line_has_text) {
            int sp = 0; for (char c : trail) sp += c == '\t' ? 4 : 1;
            if (curw + sp <= o.width) { cur.append(std::string(std::size_t(sp), ' ')); curw += sp; }
            else newline();
        }
    }
    if (line_has_text || lines.empty()) newline();
    return lines;
}
std::string wrap(std::string_view s, const wrap_options& o) {
    std::string r; auto ls = wrap_lines(s, o);
    for (std::size_t i = 0; i < ls.size(); ++i) { if (i) r += '\n'; r += ls[i]; }
    return r;
}
}  // namespace forge::os::text
