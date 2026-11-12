#pragma once
// forge::os::clipboard: get / set plain UTF-8 text. Windows uses the Win32 clipboard (CF_UNICODETEXT); macOS
// runs pbcopy / pbpaste; Linux/FreeBSD run wl-copy / wl-paste under Wayland, else xclip or xsel under X11.
// No display or helper → unsupported(). Mobile: unsupported. Guide: docs/os-wrapper.md.
#include <forge/os/process.hpp>

namespace forge::os::clipboard {
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS) && !defined(FORGE_OS_ANDROID)
// The helper command lines for reading and writing, or empty when none is usable.
inline std::pair<std::vector<std::string>, std::vector<std::string>> helpers_() {
#if defined(__APPLE__)
    return {{"pbpaste"}, {"pbcopy"}};
#else
    if (!getenv("WAYLAND_DISPLAY").empty() && !find_executable("wl-copy").empty()) return {{"wl-paste", "--no-newline"}, {"wl-copy"}};
    if (!getenv("DISPLAY").empty()) {
        if (!find_executable("xclip").empty()) return {{"xclip", "-selection", "clipboard", "-o"}, {"xclip", "-selection", "clipboard", "-i"}};
        if (!find_executable("xsel").empty()) return {{"xsel", "--clipboard", "--output"}, {"xsel", "--clipboard", "--input"}};
    }
    return {};
#endif
}
#endif
inline bool available() {
#if defined(FORGE_OS_WINDOWS)
    return true;
#elif defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS) && !defined(FORGE_OS_ANDROID)
    return !helpers_().first.empty();
#else
    return false;
#endif
}
#if defined(FORGE_OS_WINDOWS)
// Another application can hold the clipboard open for a moment: retry for ~100 ms before giving up.
inline bool open_clipboard_() {
    for (int i = 0; i < 10; ++i) { if (OpenClipboard(nullptr)) return true; Sleep(10); }
    return OpenClipboard(nullptr) != 0;
}
#elif defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS) && !defined(FORGE_OS_ANDROID)
// pbcopy/pbpaste pick their text encoding from the locale: force UTF-8 even when the caller's LANG is unset (launchd).
inline void helper_env_(spawn_spec& s) {
#if defined(__APPLE__)
    s.env_set["LANG"] = "en_US.UTF-8"; s.env_set["LC_ALL"] = "en_US.UTF-8";
#else
    (void)s;
#endif
}
// An empty clipboard is "" rather than an error: xclip "target ... not available", wl-paste "No selection" /
// "Nothing is copied", xsel exits 0 with nothing.
inline bool empty_selection_(const std::string& err) {
    for (const char* k : {"not available", "No selection", "Nothing is copied", "no selection"}) if (err.find(k) != std::string::npos) return true;
    return false;
}
#endif
inline result<std::string> get() {
#if defined(FORGE_OS_WINDOWS)
    if (!open_clipboard_()) return {{}, last_error("OpenClipboard")};
    std::string out; HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) if (auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
        // Foreign data need not be NUL-terminated: bound the text by the allocation.
        std::size_t cap = GlobalSize(h) / sizeof(wchar_t), len = 0; while (len < cap && w[len]) ++len;
        int n = len ? WideCharToMultiByte(CP_UTF8, 0, w, int(len), nullptr, 0, nullptr, nullptr) : 0;
        if (n > 0) { out.resize(std::size_t(n)); WideCharToMultiByte(CP_UTF8, 0, w, int(len), out.data(), n, nullptr, nullptr); }
        GlobalUnlock(h);
    }
    CloseClipboard(); return {out, {}};
#elif defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS) && !defined(FORGE_OS_ANDROID)
    auto h = helpers_(); if (h.first.empty()) return {{}, unsupported("clipboard (no display or clipboard helper)")};
    spawn_spec s; s.argv = h.first; s.timeout_ms = 5000; helper_env_(s); auto r = spawn_wait(s);
    if (!r) return {{}, r.err};
    if (r->exit_code) { if (empty_selection_(r->err)) return {std::string(), {}}; return {{}, {r->exit_code, "clipboard read: " + r->err}}; }
    return {r->out, {}};
#else
    return {{}, unsupported("clipboard")};
#endif
}
inline status set(const std::string& text) {
#if defined(FORGE_OS_WINDOWS)
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, std::size_t(n) * sizeof(wchar_t)); if (!g) return {{}, last_error("GlobalAlloc")};
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, static_cast<wchar_t*>(GlobalLock(g)), n); GlobalUnlock(g);
    if (!open_clipboard_()) { GlobalFree(g); return {{}, last_error("OpenClipboard")}; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, g)) { auto e = last_error("SetClipboardData"); CloseClipboard(); GlobalFree(g); return {{}, e}; }
    CloseClipboard(); return {};
#elif defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS) && !defined(FORGE_OS_ANDROID)
    auto h = helpers_(); if (h.second.empty()) return {{}, unsupported("clipboard (no display or clipboard helper)")};
    spawn_spec s; s.argv = h.second; s.stdin_data = text; s.timeout_ms = 5000; helper_env_(s);
    s.capture_stdout = false; s.capture_stderr = false;   // xclip/wl-copy fork a server that keeps the clipboard alive
    auto r = spawn_wait(s); if (!r) return {{}, r.err}; if (r->exit_code) return {{}, {r->exit_code, "clipboard write failed"}};
    return {};
#else
    (void)text; return {{}, unsupported("clipboard")};
#endif
}
} // namespace forge::os::clipboard
