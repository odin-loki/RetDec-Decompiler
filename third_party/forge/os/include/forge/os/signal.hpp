#pragma once
// forge::os portable signal names and delivery. Guide: docs/os-wrapper.md.
#include <forge/os/os.hpp>
#include <limits>

namespace forge::os {

// Full portable signal set for delivery (os::sig in os.hpp covers handler installation).
enum class signal_id { interrupt, terminate, kill, hangup, quit, user1, user2 };

inline const char* sig_name(signal_id s) {
    switch (s) { case signal_id::interrupt: return "int"; case signal_id::terminate: return "term"; case signal_id::kill: return "kill";
                 case signal_id::hangup: return "hup"; case signal_id::quit: return "quit"; case signal_id::user1: return "usr1"; case signal_id::user2: return "usr2"; }
    return "?";
}
inline bool sig_from_name(std::string_view n, signal_id& out) {
    for (signal_id s : {signal_id::interrupt, signal_id::terminate, signal_id::kill, signal_id::hangup, signal_id::quit, signal_id::user1, signal_id::user2})
        if (n == sig_name(s)) { out = s; return true; }
    return false;
}
// Native number, or -1 where the OS has no equivalent.
inline int sig_native(signal_id s) {
#if defined(FORGE_OS_POSIX)
    switch (s) { case signal_id::interrupt: return SIGINT; case signal_id::terminate: return SIGTERM; case signal_id::kill: return SIGKILL;
                 case signal_id::hangup: return SIGHUP; case signal_id::quit: return SIGQUIT; case signal_id::user1: return SIGUSR1; case signal_id::user2: return SIGUSR2; }
    return -1;
#else
    return s == signal_id::kill || s == signal_id::terminate ? 9 : s == signal_id::interrupt ? 2 : -1;
#endif
}

// Deliver to a process (or, with group = true, its whole process group). Windows: kill/terminate only.
// Rejected with EINVAL / ERROR_INVALID_PARAMETER: pid 0 (kill(0) would signal our own group), a pid that does
// not fit pid_t / DWORD (it would be truncated to some other process), and group with pid <= 1 (kill(-1)
// signals every process we may signal).
inline status send_signal(std::uint64_t pid, signal_id s, bool group = false) {
#if defined(FORGE_OS_POSIX)
    if (pid == 0 || pid > std::uint64_t(std::numeric_limits<pid_t>::max()) || (group && pid <= 1))
        return {{}, {EINVAL, "send_signal: invalid pid " + std::to_string(pid) + (group ? " (group)" : "")}};
    int n = sig_native(s);
    pid_t p = pid_t(pid);
    if (::kill(group ? -p : p, n) != 0) return {{}, last_error("kill")};
    return {};
#else
    if (pid == 0 || pid > 0xFFFFFFFFull) return {{}, {ERROR_INVALID_PARAMETER, "send_signal: invalid pid " + std::to_string(pid)}};
    // Windows has no signals to send. GenerateConsoleCtrlEvent reaches only processes on our own console, cannot
    // target CTRL_C at one group, and broadcasts to the whole console when the id is not a group leader, so
    // interrupt/quit are not emulated with it.
    (void)group;
    if (s != signal_id::kill && s != signal_id::terminate) return {{}, {unsupported_code, "send_signal: only kill/term on Windows"}};
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!h) return {{}, last_error("OpenProcess")};
    BOOL ok = TerminateProcess(h, 1); CloseHandle(h);
    if (!ok) return {{}, last_error("TerminateProcess")};
    return {};
#endif
}

} // namespace forge::os
