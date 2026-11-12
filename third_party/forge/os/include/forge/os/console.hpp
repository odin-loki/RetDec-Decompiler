#pragma once
// forge::os console: is-a-terminal checks, terminal size, ANSI/VT colour enablement (Windows 10+ needs a mode
// flag), raw / no-echo input modes as RAII guards, password prompts, and colour helpers that respect NO_COLOR
// and FORCE_COLOR. Guide: docs/os-wrapper.md.
#include <forge/os/os.hpp>
#include <cctype>
#include <iostream>
#if defined(FORGE_OS_POSIX)
#include <sys/ioctl.h>
#include <termios.h>
#endif

namespace forge::os::console {
enum class stream { in = 0, out = 1, err = 2 };
#if defined(FORGE_OS_WINDOWS)
inline HANDLE std_handle(stream s) { return GetStdHandle(s == stream::in ? STD_INPUT_HANDLE : s == stream::err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE); }
// mintty / MSYS2 / Cygwin terminals are not consoles: the program sees a named pipe called
// \msys-<hex>-ptyN-{from,to}-master or \cygwin-<hex>-ptyN-...
inline bool is_mintty_pipe(HANDLE h) {
    if (GetFileType(h) != FILE_TYPE_PIPE) return false;
    alignas(FILE_NAME_INFO) unsigned char b[sizeof(FILE_NAME_INFO) + MAX_PATH * sizeof(wchar_t)] = {};
    auto* fi = reinterpret_cast<FILE_NAME_INFO*>(b);
    if (!GetFileInformationByHandleEx(h, FileNameInfo, fi, sizeof b - sizeof(wchar_t))) return false;
    std::wstring n(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
    return (n.find(L"\\msys-") == 0 || n.find(L"\\cygwin-") == 0) && n.find(L"-pty") != std::wstring::npos && n.find(L"-master") != std::wstring::npos;
}
#endif
// A terminal: a console on Windows (not merely a character device: NUL is one too) or a mintty/MSYS pty pipe.
inline bool is_tty(stream s) {
#if defined(FORGE_OS_POSIX)
    return ::isatty(int(s)) == 1;
#else
    HANDLE h = std_handle(s); if (!h || h == INVALID_HANDLE_VALUE) return false;
    DWORD m = 0; if (GetFileType(h) == FILE_TYPE_CHAR && GetConsoleMode(h, &m)) return true;
    return is_mintty_pipe(h);
#endif
}
struct size_t2 { int cols = 80, rows = 24; };
// Terminal size; when there is no terminal to ask, COLUMNS / LINES (else 80x24), with an error only if `s` is a
// terminal that would not say.
inline result<size_t2> size(stream s = stream::out) {
    auto fallback = [](size_t2 d = {}) {
        auto c = getenv("COLUMNS"), l = getenv("LINES"); int v = 0;
        if (!c.empty() && (v = std::atoi(c.c_str())) > 0) d.cols = v;
        if (!l.empty() && (v = std::atoi(l.c_str())) > 0) d.rows = v;
        return d;
    };
#if defined(FORGE_OS_POSIX)
    winsize w{}; if (::ioctl(int(s), TIOCGWINSZ, &w) || !w.ws_col) return {fallback(), is_tty(s) ? last_error("TIOCGWINSZ") : error{}};
    return {{int(w.ws_col), int(w.ws_row)}, {}};
#else
    CONSOLE_SCREEN_BUFFER_INFO i{};
    HANDLE h = std_handle(s == stream::in ? stream::out : s);
    // Redirected, or mintty (a pipe: only the pty knows the size, and it exports COLUMNS / LINES to shells).
    if (!GetConsoleScreenBufferInfo(h, &i)) return {fallback(), {}};
    return {{i.srWindow.Right - i.srWindow.Left + 1, i.srWindow.Bottom - i.srWindow.Top + 1}, {}};
#endif
}
// Turn on ANSI escape processing (a no-op on POSIX terminals).
inline status enable_vt(stream s = stream::out) {
#if defined(FORGE_OS_WINDOWS)
    HANDLE h = GetStdHandle(s == stream::err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE); DWORD m = 0;
    if (!GetConsoleMode(h, &m)) return {{}, last_error("GetConsoleMode")};
    if (!SetConsoleMode(h, m | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/)) return {{}, last_error("SetConsoleMode")};
#else
    (void)s;
#endif
    return {};
}
// Should we emit colour on `s`? NO_COLOR disables, FORCE_COLOR / CLICOLOR_FORCE enable, TERM=dumb disables.
inline bool color_enabled(stream s = stream::out) {
    if (has_env("NO_COLOR")) return false;
    if (has_env("FORCE_COLOR") || getenv("CLICOLOR_FORCE") == "1") return true;
    if (getenv("TERM") == "dumb") return false;
    return is_tty(s);
}
enum class color { none = 0, red = 31, green = 32, yellow = 33, blue = 34, magenta = 35, cyan = 36, gray = 90, bold = 1 };
inline std::string paint(std::string_view text, color c, bool enabled) {
    if (!enabled || c == color::none) return std::string(text);
    return "\x1b[" + std::to_string(int(c)) + "m" + std::string(text) + "\x1b[0m";
}

// RAII: the terminal stays in the given mode until the guard is destroyed.
class mode_guard {
public:
    enum kind { raw, no_echo };
    explicit mode_guard(kind k) {
#if defined(FORGE_OS_POSIX)
        if (::tcgetattr(0, &old_) != 0) return;
        termios t = old_;
        if (k == raw) { t.c_lflag &= tcflag_t(~(ICANON | ECHO | ISIG | IEXTEN)); t.c_iflag &= tcflag_t(~(IXON | ICRNL)); t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0; }
        else t.c_lflag &= tcflag_t(~ECHO);
        active_ = ::tcsetattr(0, TCSAFLUSH, &t) == 0;
#else
        // raw also turns on VT input (Windows 10+), so arrow and function keys arrive as the same escape sequences
        // as on a POSIX terminal; older consoles refuse that flag and get raw mode without it.
        h_ = GetStdHandle(STD_INPUT_HANDLE); if (!GetConsoleMode(h_, &old_)) return;
        DWORD m = old_ & ~DWORD(ENABLE_ECHO_INPUT); if (k == raw) m &= ~DWORD(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        active_ = (k == raw && SetConsoleMode(h_, m | 0x0200 /*ENABLE_VIRTUAL_TERMINAL_INPUT*/)) || SetConsoleMode(h_, m);
#endif
    }
    ~mode_guard() {
        if (!active_) return;
#if defined(FORGE_OS_POSIX)
        ::tcsetattr(0, TCSAFLUSH, &old_);
#else
        SetConsoleMode(h_, old_);
#endif
    }
    mode_guard(const mode_guard&) = delete; mode_guard& operator=(const mode_guard&) = delete;
    bool active() const { return active_; }
private:
    bool active_ = false;
#if defined(FORGE_OS_POSIX)
    termios old_{};
#else
    HANDLE h_ = nullptr; DWORD old_ = 0;
#endif
};
// Prompt on stderr and read a line from stdin without echo (falls back to a plain read when not a terminal).
// Windows consoles: read as UTF-16 (ReadConsoleW) and return UTF-8, so non-ASCII passwords survive any code page.
inline std::string read_password(const std::string& prompt) {
    std::cerr << prompt << std::flush;
    std::string line;
#if defined(FORGE_OS_WINDOWS)
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE); DWORD m = 0;
    if (in && in != INVALID_HANDLE_VALUE && GetFileType(in) == FILE_TYPE_CHAR && GetConsoleMode(in, &m)) {
        mode_guard g(mode_guard::no_echo);
        std::wstring w; wchar_t b[256];
        for (;;) {
            DWORD n = 0; if (!ReadConsoleW(in, b, 256, &n, nullptr) || !n) break;
            w.append(b, n);
            if (w.find(L'\n') != std::wstring::npos || w.find(L'\r') != std::wstring::npos) break;
        }
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        std::cerr << "\n"; return narrow(w);
    }
#endif
    { mode_guard g(mode_guard::no_echo); std::getline(std::cin, line); }
    std::cerr << "\n";
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}
// Ask a yes/no question; anything but y/yes (case-insensitive) is "no". Non-interactive stdin answers `dflt`.
inline bool confirm(const std::string& question, bool dflt = false) {
    if (!is_tty(stream::in)) return dflt;
    std::cerr << question << (dflt ? " [Y/n] " : " [y/N] ") << std::flush;
    std::string a; if (!std::getline(std::cin, a)) return dflt;
    for (auto& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (a.empty()) return dflt;
    return a == "y" || a == "yes";
}
} // namespace forge::os::console
