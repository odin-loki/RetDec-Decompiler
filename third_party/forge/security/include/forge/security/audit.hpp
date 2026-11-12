#pragma once
// FSS-AUD: security audit events.
//
//   security::audit("fs", decision::deny, "CAP-1", "/etc/passwd", "write outside policy.fs.write");
//
// Every event carries UTC time, pid, the active profile, category, decision, control ID and target (AUD-2). It goes to
//   * the callback sinks (add_audit_sink),
//   * the OS audit facility when enabled (set_audit_os(true): syslog(3) / journald on POSIX, the Windows event log),
//     called directly, so a FORGE_LOG level never hides a security event,
//   * the FORGE logger (module "security"; deny/fail/weaken at warn, allow at info) when that level is enabled,
//   * a JSON-lines file (open_audit_file: created 0600, append-only, O_NOFOLLOW; owner + SYSTEM only on Windows).
// AUD-3: audit() never throws and never changes a decision. A sink that throws or a write that fails is counted in
// audit_failures(); the caller's deny stays a deny.
#include <forge/core/log.hpp>
#include <forge/os/os.hpp>
#include <forge/os/time.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN   // no <rpc.h>: its <rpcndr.h> defines `small` as char, breaking unrelated code
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#ifdef small
#undef small
#endif
#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace forge::security {

enum class decision { allow, deny, weaken, fail };
inline const char* name(decision d) {
    switch (d) { case decision::allow: return "allow"; case decision::deny: return "deny"; case decision::weaken: return "weaken"; case decision::fail: return "fail"; }
    return "?";
}

struct audit_record {
    std::string time;          // ISO 8601 UTC, milliseconds: 2026-11-05T10:20:30.123Z
    std::uint64_t pid = 0;
    std::string profile;       // "strict", "standard", "permissive", "none"
    std::string category;      // fs, net, exec, env, dynamic_code, plugin, policy, sandbox, integrity, audit
    decision d = decision::deny;
    std::string control;       // FSS control ID
    std::string target;        // path, host:port, program, variable ...
    std::string detail;        // free text (never a secret: ERR-4)
};
using audit_sink = std::function<void(const audit_record&)>;

namespace detail {
inline void json_str(std::string& o, std::string_view s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    o += '"';
}
inline std::string utc_now() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::time_t t = std::time_t(ms / 1000); std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[64];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, int(ms % 1000));
    return b;
}
struct audit_state {
    std::mutex m;
    std::vector<std::pair<int, audit_sink>> sinks;
    int next_id = 1;
    std::string profile = "none";
    log::sink os_sink;   // syslog / event log, called directly
    bool os_on = false;
    std::string file_path;
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
#else
    int file = -1;
#endif
    std::atomic<std::uint64_t> failures{0}, events{0};
    ~audit_state() {
#if defined(_WIN32)
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#else
        if (file >= 0) ::close(file);
#endif
    }
};
inline audit_state& ast() { static audit_state s; return s; }
} // namespace detail

inline std::string to_json(const audit_record& r) {
    std::string o = "{\"time\":"; detail::json_str(o, r.time);
    o += ",\"pid\":" + std::to_string(r.pid);
    o += ",\"profile\":"; detail::json_str(o, r.profile);
    o += ",\"category\":"; detail::json_str(o, r.category);
    o += ",\"decision\":"; detail::json_str(o, name(r.d));
    o += ",\"control\":"; detail::json_str(o, r.control);
    o += ",\"target\":"; detail::json_str(o, r.target);
    if (!r.detail.empty()) { o += ",\"detail\":"; detail::json_str(o, r.detail); }
    o += '}';
    return o;
}

inline int add_audit_sink(audit_sink k) {
    auto& s = detail::ast(); std::lock_guard g(s.m); int id = s.next_id++; s.sinks.emplace_back(id, std::move(k)); return id;
}
inline void remove_audit_sink(int id) {
    auto& s = detail::ast(); std::lock_guard g(s.m); std::erase_if(s.sinks, [&](auto& p) { return p.first == id; });
}
inline void set_audit_profile(std::string p) { auto& s = detail::ast(); std::lock_guard g(s.m); s.profile = std::move(p); }
inline std::string audit_profile() { auto& s = detail::ast(); std::lock_guard g(s.m); return s.profile; }
inline std::uint64_t audit_failures() { return detail::ast().failures.load(); }
inline std::uint64_t audit_events() { return detail::ast().events.load(); }

// OS audit facility: syslog(3) on POSIX (journald collects it), the Windows event log (source "forge-security").
// Returns false when no OS facility could be opened (Android logcat / Apple os_log are not wired yet).
inline bool set_audit_os(bool on) {
    auto& s = detail::ast(); std::lock_guard g(s.m);
    if (!on) { s.os_on = false; s.os_sink = nullptr; return true; }
    if (s.os_on) return true;
#if defined(_WIN32)
    s.os_sink = log::make_eventlog_sink("forge-security");
#elif defined(__unix__) || defined(__APPLE__)
    s.os_sink = log::make_syslog_sink("forge-security");
#endif
    s.os_on = bool(s.os_sink);
    return s.os_on;
}
inline bool audit_os_active() { auto& s = detail::ast(); std::lock_guard g(s.m); return s.os_on; }

// JSON-lines audit file: created 0600 (owner + SYSTEM on Windows), opened append-only, a symlink (reparse point on
// Windows) at `path` is refused. An existing file owned by this user is tightened to 0600; one owned by someone else
// is refused. Empty path closes the file.
inline os::status open_audit_file(const std::string& path) {
    auto& s = detail::ast(); std::lock_guard g(s.m);
#if defined(_WIN32)
    if (s.file != INVALID_HANDLE_VALUE) { CloseHandle(s.file); s.file = INVALID_HANDLE_VALUE; }
    s.file_path.clear();
    if (path.empty()) return {};
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE}; PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)", SDDL_REVISION_1, &sd, nullptr)) sa.lpSecurityDescriptor = sd;
    HANDLE h = CreateFileW(os::widen(path).c_str(), FILE_APPEND_DATA | SYNCHRONIZE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (sd) LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) return {{}, os::last_error("audit file " + path)};
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi) || (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(h); return {{}, {ERROR_ACCESS_DENIED, "audit file " + path + ": a link or directory is not accepted"}};
    }
    s.file = h;
#else
    if (s.file >= 0) { ::close(s.file); s.file = -1; }
    s.file_path.clear();
    if (path.empty()) return {};
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return {{}, os::last_error("audit file " + path)};
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); return {{}, {EACCES, "audit file " + path + ": not a regular file"}}; }
    if (st.st_uid != ::geteuid()) { ::close(fd); return {{}, {EACCES, "audit file " + path + ": owned by another user"}}; }
    if ((st.st_mode & 077) && ::fchmod(fd, 0600) != 0) { auto e = os::last_error("audit file chmod " + path); ::close(fd); return {{}, e}; }
    s.file = fd;
#endif
    s.file_path = path;
    return {};
}
inline std::string audit_file() { auto& s = detail::ast(); std::lock_guard g(s.m); return s.file_path; }

// Record one event. Never throws (AUD-3).
// `profile` overrides the active profile name (enforce() against an explicit policy passes that policy's profile).
inline void audit(std::string_view category, decision d, std::string_view control, std::string_view target, std::string_view detail_text = {},
                  std::string_view profile = {}) noexcept {
    try {
        auto& s = detail::ast();
        audit_record r;
        r.time = detail::utc_now(); r.pid = os::pid(); r.category = category; r.d = d; r.control = control; r.target = target; r.detail = detail_text;
        std::string line;
        std::vector<audit_sink> sinks;
        {
            std::lock_guard g(s.m);
            r.profile = profile.empty() ? s.profile : std::string(profile);
            line = to_json(r);
            for (auto& [_, k] : s.sinks) sinks.push_back(k);
            const log::level lv = d == decision::allow ? log::level::info : d == decision::fail ? log::level::error : log::level::warn;
            if (s.os_on && s.os_sink) {
                try { log::record lr{lv, "security", line, os::unix_time_ns(), os::thread_id()}; s.os_sink(lr, line); } catch (...) { ++s.failures; }
            }
            bool file_open =
#if defined(_WIN32)
                s.file != INVALID_HANDLE_VALUE;
#else
                s.file >= 0;
#endif
            if (file_open) {
                std::string nl = line + '\n';
#if defined(_WIN32)
                DWORD w = 0;
                if (!WriteFile(s.file, nl.data(), DWORD(nl.size()), &w, nullptr) || w != nl.size()) ++s.failures;
#else
                if (::write(s.file, nl.data(), nl.size()) != ssize_t(nl.size())) ++s.failures;
#endif
            }
        }
        ++s.events;
        // Callbacks and the logger run outside the audit lock (a callback may itself call audit_profile()).
        for (auto& k : sinks) { try { k(r); } catch (...) { ++s.failures; } }
        const log::level lv = d == decision::allow ? log::level::info : d == decision::fail ? log::level::error : log::level::warn;
        if (log::enabled(lv, "security")) log::emit(lv, "security", line);
    } catch (...) {
        ++detail::ast().failures;
    }
}

} // namespace forge::security
