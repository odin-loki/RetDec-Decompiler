#pragma once
// Service-manager integration.
//   Linux: systemd sd_notify protocol over $NOTIFY_SOCKET (READY=1, STATUS=, WATCHDOG=1, STOPPING=1, RELOADING=1;
//   no libsystemd) and journald's native socket (/run/systemd/journal/socket) when $JOURNAL_STREAM is set, else
//   syslog.  Android: liblog (__android_log_write).  Windows: Service Control Manager (run_as_service: dispatcher,
//   status, stop/shutdown control) and the Event Log (ReportEventW).  macOS/iOS: launchd detection
//   (XPC_SERVICE_NAME / launchd as parent) and os_log through the unified logging bridge (syslog() lands in
//   os_log on macOS 10.12+; os_log_impl itself is a private-ABI macro target).  FreeBSD: syslog + rc.d detection.
#include <forge/os/detail.hpp>
#if defined(FORGE_OS_POSIX)
#include <set>
#include <sys/un.h>
#include <syslog.h>
#endif

namespace forge::os {
enum class log_level { debug, info, notice, warning, error, critical };
namespace service {
#if defined(FORGE_OS_WINDOWS)
inline bool& is_windows_service_ref_() { static bool b = false; return b; }
inline bool is_windows_service_() { return is_windows_service_ref_(); }
#endif
inline std::string manager() {
#if defined(__linux__) && !defined(__ANDROID__)
    if (!getenv("NOTIFY_SOCKET").empty() || !getenv("INVOCATION_ID").empty()) return "systemd";
    return "";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
    if (!getenv("XPC_SERVICE_NAME").empty() && getenv("XPC_SERVICE_NAME") != "0") return "launchd";
    return ::getppid() == 1 ? "launchd" : "";
#elif defined(FORGE_OS_FREEBSD)
    return ::getppid() == 1 ? "rc.d" : "";
#elif defined(FORGE_OS_WINDOWS)
    return is_windows_service_() ? "scm" : "";
#else
    return "";
#endif
}

// sd_notify(3). Returns ok (no-op) when not under systemd.
inline status notify(const std::string& state) {
#if defined(__linux__)
    auto ns = getenv("NOTIFY_SOCKET"); if (ns.empty()) return {};
    sockaddr_un a{}; a.sun_family = AF_UNIX; if (ns.size() >= sizeof a.sun_path) return {{}, {36, "NOTIFY_SOCKET too long"}};
    std::memcpy(a.sun_path, ns.data(), ns.size()); if (a.sun_path[0] == '@') a.sun_path[0] = 0;
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return {{}, last_error("socket")};
    auto n = ::sendto(fd, state.data(), state.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&a), socklen_t(offsetof(sockaddr_un, sun_path) + ns.size()));
    ::close(fd); if (n < 0) return {{}, last_error("sd_notify")}; return {};
#else
    (void)state; return {};
#endif
}
inline status ready(const std::string& status_text = {}) { return notify("READY=1" + (status_text.empty() ? std::string() : "\nSTATUS=" + status_text)); }
inline status set_status(const std::string& s) { return notify("STATUS=" + s); }
inline status watchdog() { return notify("WATCHDOG=1"); }
inline status stopping() { return notify("STOPPING=1"); }
inline status reloading() { return notify("RELOADING=1"); }
// Interval at which watchdog() should be called (half of WATCHDOG_USEC), 0 if no watchdog.
inline std::uint64_t watchdog_interval_us() { auto w = getenv("WATCHDOG_USEC"); return w.empty() ? 0 : std::strtoull(w.c_str(), nullptr, 10) / 2; }

inline status log(log_level l, const std::string& msg, const std::string& ident = "forge") {
#if defined(__linux__) && !defined(__ANDROID__)
    if (!getenv("JOURNAL_STREAM").empty() || exists("/run/systemd/journal/socket")) {
        int pr = l == log_level::debug ? 7 : l == log_level::info ? 6 : l == log_level::notice ? 5 : l == log_level::warning ? 4 : l == log_level::error ? 3 : 2;
        std::string m = "PRIORITY=" + std::to_string(pr) + "\nSYSLOG_IDENTIFIER=" + ident + "\n";
        if (msg.find('\n') == std::string::npos) m += "MESSAGE=" + msg + "\n";
        else { m += "MESSAGE\n"; std::uint64_t n = msg.size(); char b[8]; std::memcpy(b, &n, 8); m.append(b, 8); m += msg; m += '\n'; }
        sockaddr_un a{}; a.sun_family = AF_UNIX; std::strcpy(a.sun_path, "/run/systemd/journal/socket");
        int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) { auto n = ::sendto(fd, m.data(), m.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&a), sizeof a); ::close(fd); if (n >= 0) return {}; }
    }
#endif
#if defined(__ANDROID__)
    using wfn = int (*)(int, const char*, const char*);
    static wfn w = [] { void* h = ::dlopen("liblog.so", RTLD_LAZY); return h ? reinterpret_cast<wfn>(::dlsym(h, "__android_log_write")) : wfn(nullptr); }();
    if (!w) return {{}, unsupported("liblog")};
    w(l == log_level::debug ? 3 : l == log_level::info || l == log_level::notice ? 4 : l == log_level::warning ? 5 : l == log_level::error ? 6 : 7, ident.c_str(), msg.c_str()); return {};
#elif defined(FORGE_OS_WINDOWS)
    HANDLE h = RegisterEventSourceW(nullptr, widen(ident).c_str()); if (!h) return {{}, last_error("RegisterEventSource")};
    auto wm = widen(msg); LPCWSTR s[1] = {wm.c_str()};
    WORD ty = l >= log_level::error ? EVENTLOG_ERROR_TYPE : l == log_level::warning ? EVENTLOG_WARNING_TYPE : EVENTLOG_INFORMATION_TYPE;
    BOOL ok = ReportEventW(h, ty, 0, 0x1000, nullptr, 1, 0, s, nullptr); DeregisterEventSource(h);
    if (!ok) return {{}, last_error("ReportEvent")}; return {};
#elif defined(FORGE_OS_POSIX)
    int pr = l == log_level::debug ? LOG_DEBUG : l == log_level::info ? LOG_INFO : l == log_level::notice ? LOG_NOTICE : l == log_level::warning ? LOG_WARNING : l == log_level::error ? LOG_ERR : LOG_CRIT;
    // openlog keeps the ident pointer: idents live in a never-shrinking set (stable nodes), switched under a lock.
    static std::mutex mu; static std::set<std::string> idents; static const char* cur = nullptr;
    std::lock_guard<std::mutex> g(mu);
    const char* id = idents.insert(ident).first->c_str();
    if (id != cur) { ::openlog(id, LOG_PID, LOG_USER); cur = id; }
    ::syslog(pr, "%s", msg.c_str()); return {};
#else
    (void)l; (void)msg; (void)ident; return {{}, unsupported("service log")};
#endif
}

// Windows: run `body` as a service named `name` (blocks until stopped). on_stop is called from the SCM thread.
// When started by the SCM it first changes to $FORGE_SERVICE_DIR if set (service_spec::working_dir).
// Elsewhere (or when not started by the SCM) body runs directly: daemons are managed by systemd/launchd/rc.d.
inline status run_as_service(const std::string& name, std::function<int()> body, std::function<void()> on_stop) {
#if defined(FORGE_OS_WINDOWS)
    struct ctx_t { std::wstring name; std::function<int()> body; std::function<void()> stop; SERVICE_STATUS_HANDLE h = nullptr; SERVICE_STATUS st{}; };
    static ctx_t* ctx = nullptr; ctx_t c{widen(name), std::move(body), std::move(on_stop)}; ctx = &c;
    auto set = [](DWORD state, DWORD code = 0) { ctx->st.dwServiceType = SERVICE_WIN32_OWN_PROCESS; ctx->st.dwCurrentState = state; ctx->st.dwWin32ExitCode = code;
        ctx->st.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0; SetServiceStatus(ctx->h, &ctx->st); };
    static void (*setp)(DWORD, DWORD) = set;
    SERVICE_TABLE_ENTRYW tbl[2] = {{const_cast<LPWSTR>(c.name.c_str()), [](DWORD, LPWSTR*) {
        ctx->h = RegisterServiceCtrlHandlerExW(ctx->name.c_str(), [](DWORD ctl, DWORD, LPVOID, LPVOID) -> DWORD {
            if (ctl == SERVICE_CONTROL_STOP || ctl == SERVICE_CONTROL_SHUTDOWN) { setp(SERVICE_STOP_PENDING, 0); if (ctx->stop) ctx->stop(); return NO_ERROR; }
            return ctl == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED; }, nullptr);
        if (!ctx->h) return;
        // Services start in System32 and have no working directory setting: service_control passes it in
        // FORGE_SERVICE_DIR (the service's registry Environment).
        if (DWORD n = GetEnvironmentVariableW(L"FORGE_SERVICE_DIR", nullptr, 0)) { std::wstring d(n, L'\0'); d.resize(GetEnvironmentVariableW(L"FORGE_SERVICE_DIR", d.data(), n)); if (!d.empty()) SetCurrentDirectoryW(d.c_str()); }
        is_windows_service_ref_() = true; setp(SERVICE_RUNNING, 0); int rc = ctx->body(); setp(SERVICE_STOPPED, DWORD(rc)); }}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(tbl)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) { int rc = c.body(); ctx = nullptr; return rc ? status{{}, {rc, "service body failed"}} : status{}; }
        auto e = last_error("StartServiceCtrlDispatcher"); ctx = nullptr; return {{}, e};
    }
    ctx = nullptr; return {};
#else
    (void)name; (void)on_stop; ready(); int rc = body(); stopping(); return rc ? status{{}, {rc, "service body failed"}} : status{};
#endif
}
} // namespace service
} // namespace forge::os
