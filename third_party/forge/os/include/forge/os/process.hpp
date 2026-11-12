#pragma once
// forge::os process control: spawn with argv/env/cwd, separate stdout/stderr capture, stdin data, timeouts,
// whole-tree kill (process group / Job Object) and resource limits applied at spawn. Guide: docs/os-wrapper.md.
// Like the rest of forge::os, calls return os::result<T>; nothing throws.
#include <forge/os/os.hpp>
#include <forge/os/pipe.hpp>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#if defined(FORGE_OS_POSIX)
#include <sys/time.h>
#endif
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#if defined(FORGE_OS_MACOS)
#include <libproc.h>
#include <sys/proc_info.h>
#endif
#if defined(FORGE_OS_FREEBSD)
#include <sys/sysctl.h>
#include <sys/user.h>
#endif
#if defined(FORGE_OS_WINDOWS)
#include <psapi.h>
#include <tlhelp32.h>
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#endif

namespace forge::os {

struct proc_limits {
    std::uint64_t memory_bytes = 0;   // address-space cap (RLIMIT_AS / Job Object process memory; macOS and Android:
                                      // resident-size watchdog that SIGKILLs the group, see detail_proc::memory_watchdog); 0 = none
    std::uint64_t cpu_seconds = 0;    // RLIMIT_CPU / Job Object user time; 0 = none
    std::uint64_t open_files = 0;     // RLIMIT_NOFILE; 0 = inherit. POSIX only: Windows has no per-process handle cap (ignored)
    std::uint64_t file_size_bytes = 0;// RLIMIT_FSIZE (largest file the process may write); 0 = none. POSIX only (ignored on Windows)
    bool no_core = false;             // RLIMIT_CORE = 0: no core dumps. Windows: the Job Object's DIE_ON_UNHANDLED_EXCEPTION
                                      // (a crash ends the process at once, without Windows Error Reporting or a dump)
};

#if defined(FORGE_OS_POSIX)
// Apply `lim` to the calling process (POSIX; async-signal-safe, so it may run in a forked child before exec).
// A request above an existing hard limit is lowered to it (an unprivileged process cannot raise its hard limit,
// and setrlimit would otherwise fail and leave no limit at all); soft never exceeds hard. CPU: soft = cpu_seconds,
// hard = cpu_seconds + 1 (SIGXCPU first, then SIGKILL). Returns false if any setrlimit failed.
inline bool apply_limits(const proc_limits& lim) noexcept {
    bool ok = true;
    auto set = [&](int which, rlim_t soft, rlim_t hard) {
        struct rlimit cur{};
        if (::getrlimit(which, &cur) == 0 && cur.rlim_max != RLIM_INFINITY) {
            if (hard == RLIM_INFINITY || hard > cur.rlim_max) hard = cur.rlim_max;
        }
        if (soft > hard) soft = hard;
        struct rlimit r{soft, hard};
        if (::setrlimit(which, &r) != 0) ok = false;
    };
#if !defined(FORGE_OS_MACOS) && !defined(__ANDROID__)
    // macOS: RLIMIT_AS aliases RLIMIT_RSS, which the kernel ignores. Android: bionic's linker and allocator reserve
    // far more address space than they use, so a realistic RLIMIT_AS stops even `sh` from linking. Both keep
    // memory_bytes with the resident-size memory_watchdog instead.
    if (lim.memory_bytes) set(RLIMIT_AS, rlim_t(lim.memory_bytes), rlim_t(lim.memory_bytes));
#endif
    if (lim.cpu_seconds) set(RLIMIT_CPU, rlim_t(lim.cpu_seconds), rlim_t(lim.cpu_seconds + 1));
    if (lim.open_files) set(RLIMIT_NOFILE, rlim_t(lim.open_files), rlim_t(lim.open_files));
    if (lim.file_size_bytes) set(RLIMIT_FSIZE, rlim_t(lim.file_size_bytes), rlim_t(lim.file_size_bytes));
    if (lim.no_core) set(RLIMIT_CORE, 0, 0);
    return ok;
}
#endif

struct spawn_spec {
    std::vector<std::string> argv;
    std::string cwd;                                   // empty = inherit
    std::map<std::string, std::string> env_set;        // added/overridden variables
    std::vector<std::string> env_unset;
    bool clear_env = false;                            // start from an empty environment
    std::optional<std::string> stdin_data;             // nullopt = /dev/null
    bool capture_stdout = true, capture_stderr = true; // false = inherit the parent's stream
    bool merge_stderr = false;                         // stderr goes into out
    std::uint64_t timeout_ms = 0;                      // 0 = wait forever; on expiry the process tree is killed
    std::uint64_t max_output = 64ull << 20;            // per stream; extra output is discarded, truncated = true
    proc_limits limits;
    // Redirect a stream to a file (empty = not used). stdin_file wins over stdin_data; stdout_file/stderr_file
    // win over capture_* (and merge_stderr sends stderr to stdout_file). Files are created/truncated, or
    // appended to with append_output.
    std::string stdin_file, stdout_file, stderr_file;
    bool append_output = false;
    // Cancel: when stop is requested the whole process tree is killed, cancelled = true, exit_code = 125.
    std::stop_token stop{};
    // POSIX: setsid() (new session, no controlling terminal) instead of setpgid(0,0); the session leader's
    // group is still killed as a whole. Windows: CREATE_NEW_PROCESS_GROUP (the Job Object kills the tree).
    bool new_session = false;
    // Memory watchdog: kill the tree once its memory passes this many bytes (0 = off); memory_killed = true,
    // exit_code = 137. Windows: Job Object committed memory of every process in the job; Linux/Android: RSS of
    // the process group (/proc); other POSIX: the child's RSS is not readable, use limits.memory_bytes.
    std::uint64_t watch_memory_bytes = 0;
};

#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
// Signal every process in group `pgid` (spawned children lead their own group). True on success.
inline bool kill_group(std::uint64_t pgid, int sig = SIGKILL) { return pgid > 1 && pgid <= std::uint64_t(std::numeric_limits<pid_t>::max()) && ::kill(-pid_t(pgid), sig) == 0; }
#endif
namespace detail_proc {
#if defined(__linux__) || defined(__ANDROID__)
// Calls fn(pid, pgrp, sid, rss_pages) for every process in /proc.
template <class F> inline void for_each_proc(F&& fn) {
    DIR* d = ::opendir("/proc"); if (!d) return;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64]; std::snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        FILE* f = std::fopen(path, "r"); if (!f) continue;
        char buf[1024]; std::size_t n = std::fread(buf, 1, sizeof buf - 1, f); std::fclose(f); buf[n] = 0;
        const char* q = std::strrchr(buf, ')'); if (!q) continue;
        // fields after ')': state ppid pgrp session ... rss is field 24 overall (22nd after the comm)
        char st = 0; long long ppid = 0, pgrp = 0, sid = 0;
        if (std::sscanf(q + 2, "%c %lld %lld %lld", &st, &ppid, &pgrp, &sid) != 4) continue;
        const char* r = q + 2; int field = 0;
        while (*r && field < 21) { if (*r == ' ') ++field; ++r; }
        fn(pid_t(std::atoll(e->d_name)), pid_t(pgrp), pid_t(sid), std::uint64_t(std::strtoull(r, nullptr, 10)));
    }
    ::closedir(d);
}
// Sum of RSS (bytes) over processes whose process group is pgid.
inline std::uint64_t group_rss(pid_t pgid) {
    std::uint64_t total = 0; const std::uint64_t pg = std::uint64_t(::sysconf(_SC_PAGESIZE));
    for_each_proc([&](pid_t, pid_t pgrp, pid_t, std::uint64_t rss) { if (pgrp == pgid) total += rss * pg; });
    return total;
}
// Every process whose session id is sid.
inline std::vector<pid_t> session_pids(pid_t sid) {
    std::vector<pid_t> v;
    for_each_proc([&](pid_t pid, pid_t, pid_t s, std::uint64_t) { if (s == sid) v.push_back(pid); });
    return v;
}
#define FORGE_PROC_GROUP_RSS 1
#if defined(__ANDROID__)
// Android keeps proc_limits::memory_bytes with a watchdog (see apply_limits): a double-forked grandchild polls the
// resident size of process group `pgid` every 2 ms through /proc and SIGKILLs the group once it passes `cap`; it
// exits when the group is empty. Raw syscalls only (open/read/getdents64, stack buffers): the caller may be
// multithreaded. Inherited descriptors are closed first so it never holds a child's pipe open.
inline std::uint64_t raw_dec(const char*& p) { std::uint64_t v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + std::uint64_t(*p++ - '0'); return v; }
inline void memory_watchdog(pid_t pgid, std::uint64_t cap) {
    pid_t m = ::fork();
    if (m < 0) return;
    if (m == 0) {
        if (::fork() != 0) ::_exit(0);
        struct rlimit nf{}; const int top = ::getrlimit(RLIMIT_NOFILE, &nf) == 0 && nf.rlim_cur != RLIM_INFINITY && nf.rlim_cur < 65536 ? int(nf.rlim_cur) : 1024;
        for (int fd = 0; fd < top; ++fd) ::close(fd);
        const std::uint64_t pg = std::uint64_t(::sysconf(_SC_PAGESIZE));
        for (;;) {
            int dfd = ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dfd < 0) ::_exit(0);
            std::uint64_t total = 0; int live = 0;
            alignas(8) char dbuf[8192];
            for (;;) {
                long n = ::syscall(SYS_getdents64, dfd, dbuf, sizeof dbuf);
                if (n <= 0) break;
                for (long off = 0; off < n;) {
                    const char* rec = dbuf + off;
                    unsigned short reclen; std::memcpy(&reclen, rec + 16, sizeof reclen);
                    const char* name = rec + 19; off += reclen;
                    if (name[0] < '0' || name[0] > '9') continue;
                    char path[48] = "/proc/"; std::size_t k = 6;
                    for (const char* q = name; *q && k < 36; ++q) path[k++] = *q;
                    std::memcpy(path + k, "/stat", 6);
                    int f = ::open(path, O_RDONLY | O_CLOEXEC); if (f < 0) continue;
                    char buf[1024]; long r = ::read(f, buf, sizeof buf - 1); ::close(f);
                    if (r <= 0) continue;
                    buf[r] = 0;
                    const char* q = nullptr; for (long i = r - 1; i >= 0; --i) if (buf[i] == ')') { q = buf + i; break; }
                    if (!q || q[1] != ' ') continue;
                    // after ") ": state ppid pgrp ...; rss is the 22nd field after the comm
                    const char* t = q + 2; int field = 0;
                    std::uint64_t pgrp = 0;
                    while (*t && field < 21) { if (*t == ' ') { ++field; ++t; if (field == 2) pgrp = raw_dec(t); continue; } ++t; }
                    if (pid_t(pgrp) != pgid) continue;
                    if (q[2] == 'Z') continue;
                    total += raw_dec(t) * pg; ++live;
                }
            }
            ::close(dfd);
            if (!live) ::_exit(0);
            if (total > cap) { ::kill(-pgid, SIGKILL); ::_exit(0); }
            struct timespec ts{0, 2000000}; ::nanosleep(&ts, nullptr);
        }
    }
    int st; while (::waitpid(m, &st, 0) < 0 && errno == EINTR) {}
}
#define FORGE_PROC_MEMORY_WATCHDOG 1
#endif
#elif defined(FORGE_OS_MACOS)
inline std::vector<pid_t> list_pids(std::uint32_t type, std::uint32_t arg) {
    int n = ::proc_listpids(type, arg, nullptr, 0); if (n <= 0) return {};
    std::vector<pid_t> v(std::size_t(n) / sizeof(pid_t) + 16);
    n = ::proc_listpids(type, arg, v.data(), int(v.size() * sizeof(pid_t))); if (n <= 0) return {};
    v.resize(std::size_t(n) / sizeof(pid_t)); std::erase(v, 0); return v;
}
inline std::uint64_t group_rss(pid_t pgid) {
    std::uint64_t total = 0;
    for (pid_t pid : list_pids(PROC_PGRP_ONLY, std::uint32_t(pgid))) {
        rusage_info_v2 ri{};
        if (::proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) == 0) total += ri.ri_resident_size;
    }
    return total;
}
inline std::vector<pid_t> session_pids(pid_t sid) {
    std::vector<pid_t> v;
    for (pid_t pid : list_pids(PROC_ALL_PIDS, 0)) if (::getsid(pid) == sid) v.push_back(pid);
    return v;
}
// macOS enforces no address-space or resident-size rlimit, so proc_limits::memory_bytes is kept by a watchdog:
// a double-forked grandchild (reparented to launchd, nothing to reap) polls the resident size of process group
// `pgid` every 2 ms and SIGKILLs the group once it passes `cap`; it exits as soon as the group is empty. It
// closes every inherited descriptor first, so it never holds a child's stdin/stdout pipe open. Only
// async-signal-safe calls after fork (the caller may be multithreaded).
inline void memory_watchdog(pid_t pgid, std::uint64_t cap) {
    pid_t m = ::fork();
    if (m < 0) return;
    if (m == 0) {
        if (::fork() != 0) ::_exit(0);
        const int top = ::getdtablesize();
        for (int fd = 0; fd < top; ++fd) ::close(fd);
        int nul = ::open("/dev/null", O_RDWR); if (nul >= 0) { ::dup2(nul, 1); ::dup2(nul, 2); }
        pid_t pids[512];
        for (;;) {
            int n = ::proc_listpids(PROC_PGRP_ONLY, std::uint32_t(pgid), pids, int(sizeof pids));
            if (n <= 0) ::_exit(0);
            std::uint64_t total = 0; int live = 0;
            for (int i = 0; i < n / int(sizeof(pid_t)); ++i) {
                if (pids[i] <= 0) continue;
                rusage_info_v2 ri{};
                if (::proc_pid_rusage(pids[i], RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) == 0) { total += ri.ri_resident_size; ++live; }
            }
            if (!live) ::_exit(0);
            if (total > cap) { ::kill(-pgid, SIGKILL); ::_exit(0); }
            struct timespec ts{0, 2000000}; ::nanosleep(&ts, nullptr);
        }
    }
    int st; while (::waitpid(m, &st, 0) < 0 && errno == EINTR) {}
}
#define FORGE_PROC_MEMORY_WATCHDOG 1
#define FORGE_PROC_GROUP_RSS 1
#elif defined(FORGE_OS_FREEBSD)
inline std::vector<kinfo_proc> procs(int op, int arg) {
    int mib[4] = {CTL_KERN, KERN_PROC, op, arg}; std::size_t len = 0;
    if (::sysctl(mib, op == KERN_PROC_PROC ? 3u : 4u, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    len += len / 4;
    std::vector<kinfo_proc> v(len / sizeof(kinfo_proc) + 1);
    len = v.size() * sizeof(kinfo_proc);
    if (::sysctl(mib, op == KERN_PROC_PROC ? 3u : 4u, v.data(), &len, nullptr, 0) != 0) return {};
    v.resize(len / sizeof(kinfo_proc)); return v;
}
inline std::uint64_t group_rss(pid_t pgid) {
    std::uint64_t total = 0; const std::uint64_t pg = std::uint64_t(::getpagesize());
    for (auto& k : procs(KERN_PROC_PGRP, int(pgid))) total += std::uint64_t(k.ki_rssize) * pg;
    return total;
}
inline std::vector<pid_t> session_pids(pid_t sid) {
    std::vector<pid_t> v;
    for (auto& k : procs(KERN_PROC_SESSION, int(sid))) v.push_back(k.ki_pid);
    return v;
}
#define FORGE_PROC_GROUP_RSS 1
#endif
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
// SIGKILL the group pgid and, for a session leader, every other member of its session (a descendant that
// moved to a process group of its own is still in the session). Members in other sessions are out of reach.
inline void kill_tree(pid_t p, bool session) {
    ::kill(-p, SIGKILL);
#if defined(FORGE_PROC_GROUP_RSS)
    if (session) for (pid_t m : session_pids(p)) if (m != p) ::kill(m, SIGKILL);
#else
    (void)session;
#endif
}
// rusage ru_maxrss in bytes (Linux/FreeBSD/Android report KiB, macOS bytes).
inline std::uint64_t maxrss_bytes(const struct rusage& ru) {
#if defined(FORGE_OS_MACOS)
    return std::uint64_t(ru.ru_maxrss);
#else
    return std::uint64_t(ru.ru_maxrss) * 1024ull;
#endif
}
#endif
}

struct proc_result {
    bool cancelled = false, memory_killed = false;
    // Killed (timeout / stop / watchdog) but not reaped within 2 s: a detached thread reaps it later.
    bool unreaped = false;
    // Peak resident memory of the child in bytes (POSIX: wait4 rusage of the direct child; Windows: peak
    // process memory of the Job Object, i.e. of the largest process in the tree). 0 = not available.
    std::uint64_t peak_rss_bytes = 0;
    int exit_code = -1;      // 128+signal when killed by a signal (POSIX convention)
    int term_signal = 0;
    bool timed_out = false, truncated = false;
    std::string out, err;
    std::uint64_t wall_ms = 0;
};

// Capability names this module contributes to os::caps()-style feature checks.
inline std::vector<std::string> process_caps() {
#if defined(FORGE_OS_IOS)
    return {};
#elif defined(FORGE_OS_WINDOWS)
    return {"spawn", "process.limits.memory", "process.limits.cpu", "process.tree_kill", "process.timeout"};
#else
    return {"spawn", "process.limits.memory", "process.limits.cpu", "process.limits.files", "process.tree_kill", "process.timeout", "signals"};
#endif
}

// Locate an executable on PATH (or return the name if it contains a separator and exists).
inline std::string find_executable(const std::string& name) {
    if (name.empty()) return {};
#if defined(FORGE_OS_WINDOWS)
    const char sep = ';';
    std::vector<std::string> exts = {"", ".exe", ".cmd", ".bat", ".com"};
    auto is_file = [](const std::string& p) { DWORD a = GetFileAttributesW(widen(p).c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); };
    if (name.find_first_of("/\\") != std::string::npos) { for (auto& e : exts) if (is_file(name + e)) return name + e; return {}; }
    // A bare name ("cmd") resolves like CreateProcess: by extension only, so an extensionless file on PATH
    // (an MSYS2 / Git shell script, which CreateProcess cannot start) never shadows cmd.exe.
    if (name.find('.') == std::string::npos) exts.erase(exts.begin());
#else
    const char sep = ':';
    std::vector<std::string> exts = {""};
    auto is_file = [](const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0; };
    if (name.find('/') != std::string::npos) return is_file(name) ? name : std::string{};
#endif
    std::string path = getenv("PATH");
    std::size_t i = 0;
    while (i <= path.size()) {
        auto j = path.find(sep, i); if (j == std::string::npos) j = path.size();
        std::string d = path.substr(i, j - i); if (d.empty()) d = ".";
#if defined(FORGE_OS_WINDOWS)
        std::string base = d + "\\" + name;
#else
        std::string base = d + "/" + name;
#endif
        for (auto& e : exts) if (is_file(base + e)) return base + e;
        i = j + 1;
    }
    return {};
}

#if defined(FORGE_OS_WINDOWS)
namespace detail_proc {
inline std::wstring system_dir() {
    wchar_t b[MAX_PATH]; UINT n = GetSystemDirectoryW(b, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(b, n) : std::wstring(L"C:\\Windows\\System32");
}
inline bool is_batch(const std::string& exe) {
    if (exe.size() < 5) return false;
    std::string e = exe.substr(exe.size() - 4); for (auto& c : e) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return e == ".bat" || e == ".cmd";
}
// One argument with MSVCRT / CommandLineToArgvW quoting.
inline void quote_arg(std::wstring& cmd, const std::wstring& w) {
    if (!w.empty() && w.find_first_of(L" \t\n\v\"") == std::wstring::npos) { cmd += w; return; }
    cmd += L'"'; std::size_t bs = 0;
    for (wchar_t c : w) { if (c == L'\\') { ++bs; continue; } if (c == L'"') cmd.append(bs * 2 + 1, L'\\'); else cmd.append(bs, L'\\'); bs = 0; cmd += c; }
    cmd.append(bs * 2, L'\\'); cmd += L'"';
}
// One argument of a batch file. cmd.exe parses the line itself (MSVCRT's \" means nothing to it, so
// `"&calc&"` would run calc: BatBadBut, CVE-2024-24576); this is the scheme Rust's std uses: quote anything
// that is not plainly safe ASCII, double an embedded quote (cmd toggles its quote state twice and stays
// inside), double backslashes only before a quote, and turn % into %%cd:~,% so no %VAR% is expanded (an
// empty substring of the always-defined %cd%). Delayed !VAR! expansion is off (/v:OFF). CR/LF are rejected
// by the caller. What the script does with %~1 (which strips the quotes again) is beyond our reach.
inline void quote_bat_arg(std::wstring& cmd, const std::wstring& w) {
    bool quote = w.empty() || w.back() == L'\\';
    for (wchar_t c : w) {
        const bool safe = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                          (c != 0 && std::wcschr(L"#$*+-./:?@\\_", c)) || (c >= 0xa0);
        if (!safe) quote = true;
    }
    if (quote) cmd += L'"';
    std::size_t bs = 0;
    for (wchar_t c : w) {
        if (c == L'\\') { ++bs; cmd += c; continue; }
        if (c == L'"') { cmd.append(bs, L'\\'); cmd += L'"'; }
        else if (c == L'%') cmd += L"%%cd:~,";
        bs = 0; cmd += c;
    }
    if (quote) { cmd.append(bs, L'\\'); cmd += L'"'; }
}
// argv -> CreateProcess command line. exe is argv[0] resolved by find_executable. A .cmd/.bat runs as
// `cmd.exe /e:ON /v:OFF /d /c ""script" args"` with cmd.exe from the system directory (exe is switched to it)
// and quote_bat_arg escaping; an argument with CR, LF or NUL (which would end the cmd line) or a script name
// with a quote is refused: the result is empty and *err says why.
inline std::wstring win_cmdline(std::vector<std::string> av, std::string& exe, error* err = nullptr) {
    std::wstring cmd;
    av[0] = exe;
    if (is_batch(exe)) {
        const std::wstring script = widen(exe);
        auto refuse = [&](const char* why) { if (err) *err = {ERROR_BAD_ARGUMENTS, "spawn " + exe + ": " + why}; return std::wstring(); };
        if (script.find(L'"') != std::wstring::npos || script.back() == L'\\') return refuse("batch file name cannot be quoted");
        cmd = L"cmd.exe /e:ON /v:OFF /d /c \"\"" + script + L"\"";
        for (std::size_t i = 1; i < av.size(); ++i) {
            if (av[i].find_first_of(std::string("\r\n\0", 3)) != std::string::npos) return refuse("a batch file argument cannot contain CR, LF or NUL");
            cmd += L' '; quote_bat_arg(cmd, widen(av[i]));
        }
        cmd += L'"';
        exe = narrow(system_dir() + L"\\cmd.exe");
        return cmd;
    }
    for (auto& a : av) { if (!cmd.empty()) cmd += L' '; quote_arg(cmd, widen(a)); }
    return cmd;
}
// Windows variable names are case-insensitive: "Path" and "PATH" are one variable.
struct env_less {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
// CREATE_UNICODE_ENVIRONMENT block from the parent's environment (unless clear_env) plus env_unset/env_set,
// matching names case-insensitively (env_unset "path" removes "Path"; env_set "PATH" replaces its value).
inline std::wstring win_env_block(const spawn_spec& sp) {
    std::map<std::wstring, std::wstring, env_less> envm;
    if (!sp.clear_env) {
        if (LPWCH blk = GetEnvironmentStringsW()) {
            for (LPWCH p = blk; *p; p += wcslen(p) + 1) { std::wstring e(p); auto eq = e.find(L'=', 1); if (eq != std::wstring::npos) envm.emplace(e.substr(0, eq), e.substr(eq + 1)); }
            FreeEnvironmentStringsW(blk);
        }
    }
    for (auto& u : sp.env_unset) envm.erase(widen(u));
    for (auto& [k, v] : sp.env_set) envm[widen(k)] = widen(v);
    std::wstring envb; for (auto& [k, v] : envm) { envb += k; envb += L'='; envb += v; envb += L'\0'; } envb += L'\0';
    return envb;
}
// Job Object carrying the memory / CPU limits, and for no_core JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION (an
// unhandled exception ends the process at once: no Windows Error Reporting dialog or dump). kill_on_close also
// kills whatever is left when it is closed. Null on failure, with GetLastError set.
inline HANDLE win_job(const proc_limits& lim, bool kill_on_close) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    auto& fl = li.BasicLimitInformation.LimitFlags;
    if (kill_on_close) fl |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (lim.no_core) fl |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (lim.memory_bytes) { fl |= JOB_OBJECT_LIMIT_PROCESS_MEMORY; li.ProcessMemoryLimit = SIZE_T(lim.memory_bytes); }
    if (lim.cpu_seconds) { fl |= JOB_OBJECT_LIMIT_PROCESS_TIME; li.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = LONGLONG(lim.cpu_seconds) * 10000000LL; }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) { DWORD e = GetLastError(); CloseHandle(job); SetLastError(e); return nullptr; }
    return job;
}
// Put a suspended process into its job; on failure (a parent job that forbids nesting or breakaway) it is
// terminated: running it without its limits and tree kill would be silently weaker than asked.
inline error win_assign(HANDLE job, const PROCESS_INFORMATION& pi) {
    if (AssignProcessToJobObject(job, pi.hProcess)) return {};
    error e = last_error("AssignProcessToJobObject");
    TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000);
    return e;
}
}
#endif

#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
namespace detail_proc {
inline void set_nonblock(int fd) { int fl = ::fcntl(fd, F_GETFL); ::fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
inline void cloexec(int fd) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }
// In a forked child: make `from` the descriptor `to` (0/1/2), inheritable across exec. Pipes are close-on-exec
// (detail::pipe_cloexec); dup2 clears the flag on the copy, but dup2(fd, fd) is a no-op that would leave it set.
inline void dup_to(int from, int to) { if (from == to) ::fcntl(to, F_SETFD, 0); else ::dup2(from, to); }
}
#endif

// Spawn, feed stdin, collect output, wait (with optional timeout). The child leads its own process group,
// so a timeout kills grandchildren too.
inline result<proc_result> spawn_wait(const spawn_spec& sp) {
    proc_result res;
    if (sp.argv.empty()) return {{}, {-1, "spawn: empty argv"}};
    auto t0 = monotonic_ns();
#if defined(FORGE_OS_IOS)
    return {{}, {unsupported_code, "spawn: process spawning is not available on iOS"}};
#elif defined(FORGE_OS_WINDOWS)
    std::string exe = find_executable(sp.argv[0]);
    if (exe.empty()) return {{}, {ERROR_FILE_NOT_FOUND, "spawn " + sp.argv[0] + ": not found"}};
    error cmd_err{};
    std::wstring cmd = detail_proc::win_cmdline(sp.argv, exe, &cmd_err), envb = detail_proc::win_env_block(sp);
    if (cmd.empty()) return {{}, cmd_err};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    auto closeh = [](HANDLE& h) { if (h) { CloseHandle(h); h = nullptr; } };
    // Every handle is created non-inheritable: create_process_std_only hands the child inheritable duplicates of
    // exactly its three std handles, so a CreateProcess on another thread never picks one up.
    auto open_file = [&](const std::string& f, bool write) -> HANDLE {
        if (f.empty()) return nullptr;
        return CreateFileW(widen(f).c_str(), write ? (sp.append_output ? FILE_APPEND_DATA : GENERIC_WRITE) : GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, write ? (sp.append_output ? OPEN_ALWAYS : CREATE_ALWAYS) : OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    HANDLE fin = open_file(sp.stdin_file, false), fout = open_file(sp.stdout_file, true), ferr = sp.merge_stderr ? nullptr : open_file(sp.stderr_file, true);
    for (auto [h, f] : {std::pair{fin, &sp.stdin_file}, {fout, &sp.stdout_file}, {ferr, &sp.stderr_file}})
        if (h == INVALID_HANDLE_VALUE) { auto e = last_error("open " + *f); for (HANDLE x : {fin, fout, ferr}) if (x && x != INVALID_HANDLE_VALUE) CloseHandle(x); return {{}, e}; }
    auto fail = [&](error e) { for (HANDLE* h : {&in_r, &in_w, &out_r, &out_w, &err_r, &err_w, &fin, &fout, &ferr}) closeh(*h); return result<proc_result>{{}, std::move(e)}; };
    if (!CreatePipe(&in_r, &in_w, nullptr, 0)) return fail(last_error("CreatePipe"));
    if (!fout && (sp.capture_stdout || sp.merge_stderr) && !CreatePipe(&out_r, &out_w, nullptr, 0)) return fail(last_error("CreatePipe"));
    if (!ferr && sp.capture_stderr && !sp.merge_stderr && !CreatePipe(&err_r, &err_w, nullptr, 0)) return fail(last_error("CreatePipe"));
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = fin ? fin : in_r;
    HANDLE o1 = fout ? fout : out_w;
    si.hStdOutput = o1 ? o1 : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = sp.merge_stderr ? si.hStdOutput : (ferr ? ferr : (err_w ? err_w : GetStdHandle(STD_ERROR_HANDLE)));
    HANDLE job = detail_proc::win_job(sp.limits, true);
    if (!job) return fail(last_error("CreateJobObject"));
    PROCESS_INFORMATION pi{};
    std::wstring wcwd = widen(sp.cwd);
    BOOL ok = create_process_std_only(widen(exe).c_str(), cmd.data(), CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (sp.new_session ? CREATE_NEW_PROCESS_GROUP : 0),
                                      envb.data(), sp.cwd.empty() ? nullptr : wcwd.c_str(), si, pi);
    closeh(in_r); closeh(out_w); closeh(err_w); closeh(fin); closeh(fout); closeh(ferr);
    if (!ok) { auto e = last_error("CreateProcess " + sp.argv[0]); CloseHandle(job); return fail(e); }
    if (auto e = detail_proc::win_assign(job, pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(job); return fail(e); }
    ResumeThread(pi.hThread);
    // The stdin writer and the stdout/stderr readers run on threads of their own: a child that never reads its
    // stdin cannot stall the wait loop below, which keeps the timeout, stop token and watchdog live.
    const std::string in = sp.stdin_file.empty() ? sp.stdin_data.value_or("") : std::string{};
    struct io { HANDLE h; std::string* s; const std::string* in; std::uint64_t cap; bool trunc = false; std::atomic<std::uint64_t> drain_end{0}; };
    io ro{out_r, &res.out, nullptr, sp.max_output}, re{err_r, &res.err, nullptr, sp.max_output}, wi{in_w, nullptr, &in, 0};
    auto reader = [](void* p) -> unsigned {
        auto* r = static_cast<io*>(p); char buf[4096];
        for (;;) {
            DWORD n = 0, avail = 0;
            if (const std::uint64_t de = r->drain_end.load()) {   // draining: only what is already buffered, never block
                if (monotonic_ns() > de || !PeekNamedPipe(r->h, nullptr, 0, nullptr, &avail, nullptr) || !avail) break;
            }
            if (!ReadFile(r->h, buf, avail ? std::min<DWORD>(avail, DWORD(sizeof buf)) : DWORD(sizeof buf), &n, nullptr) || !n) {
                if (GetLastError() == ERROR_OPERATION_ABORTED && r->drain_end.load()) continue;   // cancelled: drain
                break;
            }
            if (r->s->size() + n <= r->cap) r->s->append(buf, n); else r->trunc = true;
        }
        return 0; };
    auto writer = [](void* p) -> unsigned {
        auto* w = static_cast<io*>(p); std::size_t off = 0; DWORD n = 0;
        while (off < w->in->size() && !w->drain_end.load() && WriteFile(w->h, w->in->data() + off, DWORD(std::min<std::size_t>(w->in->size() - off, 1 << 16)), &n, nullptr)) off += n;
        CloseHandle(w->h); w->h = nullptr;   // EOF for the child
        return 0; };
    HANDLE th[3] = {nullptr, nullptr, nullptr};
    if (out_r && !(th[0] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, reader, &ro, 0, nullptr)))) closeh(out_r);
    if (err_r && !(th[1] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, reader, &re, 0, nullptr)))) closeh(err_r);
    if (in.empty() || !(th[2] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, writer, &wi, 0, nullptr)))) closeh(in_w);
    // Wait in 50 ms slices: timeout, stop token and the memory watchdog (Job Object committed memory of the tree).
    const std::uint64_t deadline = sp.timeout_ms ? t0 + sp.timeout_ms * 1000000ull : 0;
    for (;;) {
        DWORD slice = INFINITE;
        if (deadline || sp.stop.stop_possible() || sp.watch_memory_bytes) {
            std::uint64_t now = monotonic_ns();
            if (deadline && now >= deadline) { res.timed_out = true; break; }
            slice = 50; if (deadline) slice = DWORD(std::min<std::uint64_t>(50, (deadline - now) / 1000000 + 1));
        }
        if (WaitForSingleObject(pi.hProcess, slice) == WAIT_OBJECT_0) break;
        if (sp.stop.stop_requested()) { res.cancelled = true; break; }
        if (sp.watch_memory_bytes) {
            std::uint64_t used = 0;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION xi{};
            if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &xi, sizeof xi, nullptr)) used = std::uint64_t(xi.PeakJobMemoryUsed);
            if (used > sp.watch_memory_bytes) { res.memory_killed = true; break; }
        }
    }
    if (res.timed_out || res.cancelled || res.memory_killed) { TerminateJobObject(job, 1); if (WaitForSingleObject(pi.hProcess, 2000) != WAIT_OBJECT_0) res.unreaped = true; }
    // The direct child is gone. Normally every pipe reports EOF at once, but a grandchild that inherited one can
    // hold it open indefinitely: after a short grace the threads drain only what is already buffered and their
    // blocking ReadFile/WriteFile is cancelled (CancelSynchronousIo), as the POSIX loop stops once the direct
    // child is reaped. The kill-on-close job ends such leftovers when it is closed below.
    HANDLE live[3]; DWORD nlive = 0; for (HANDLE t : th) if (t) live[nlive++] = t;
    if (nlive && WaitForMultipleObjects(nlive, live, TRUE, 200) == WAIT_TIMEOUT) {
        const std::uint64_t end = monotonic_ns() + 200000000ull;
        for (io* x : {&ro, &re, &wi}) x->drain_end.store(end);
        for (DWORD i = 0; i < nlive; ++i) while (WaitForSingleObject(live[i], 10) == WAIT_TIMEOUT) CancelSynchronousIo(live[i]);
    }
    for (DWORD i = 0; i < nlive; ++i) CloseHandle(live[i]);
    if (!th[2]) closeh(in_w);
    { JOBOBJECT_EXTENDED_LIMIT_INFORMATION xi{}; if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &xi, sizeof xi, nullptr)) res.peak_rss_bytes = std::uint64_t(xi.PeakProcessMemoryUsed); }
    if (!res.peak_rss_bytes) { PROCESS_MEMORY_COUNTERS pmc{}; if (GetProcessMemoryInfo(pi.hProcess, &pmc, sizeof pmc)) res.peak_rss_bytes = std::uint64_t(pmc.PeakWorkingSetSize); }
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); res.exit_code = res.timed_out ? 124 : res.cancelled ? 125 : res.memory_killed ? 137 : int(code);
    res.truncated = ro.trunc || re.trunc;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); closeh(out_r); closeh(err_r); CloseHandle(job);
    res.wall_ms = (monotonic_ns() - t0) / 1000000;
    return {std::move(res), {}};
#else
    std::string exe = find_executable(sp.argv[0]);
    if (exe.empty()) return {{}, {ENOENT, "spawn " + sp.argv[0] + ": command not found"}};
    // Everything the child needs is prepared before fork: after fork only async-signal-safe calls are made.
    std::vector<std::string> envs;
    if (!sp.clear_env) for (char** e = environ; e && *e; ++e) {
        std::string s(*e); auto eq = s.find('='); std::string k = s.substr(0, eq);
        bool drop = sp.env_set.count(k) > 0;
        for (auto& u : sp.env_unset) if (u == k) drop = true;
        if (!drop) envs.push_back(std::move(s));
    }
    for (auto& [k, v] : sp.env_set) envs.push_back(k + "=" + v);
    std::vector<char*> av, ev;
    for (auto& a : sp.argv) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
    for (auto& e : envs) ev.push_back(const_cast<char*>(e.c_str())); ev.push_back(nullptr);
    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1}, pexec[2] = {-1, -1};
    auto cl = [](int& fd) { if (fd >= 0) { ::close(fd); fd = -1; } };
    if (detail::pipe_cloexec(pin)) return {{}, last_error("pipe")};
    if (detail::pipe_cloexec(pexec)) { auto e = last_error("pipe"); cl(pin[0]); cl(pin[1]); return {{}, e}; }
    const int oflags = O_WRONLY | O_CREAT | O_CLOEXEC | (sp.append_output ? O_APPEND : O_TRUNC);
    int fin = -1, fout = -1, ferr = -1;
    auto open_fail = [&](const std::string& f) { auto e = last_error("open " + f); for (int* x : {&fin, &fout, &ferr, &pin[0], &pin[1], &pexec[0], &pexec[1]}) cl(*x); return e; };
    if (!sp.stdin_file.empty() && (fin = ::open(sp.stdin_file.c_str(), O_RDONLY | O_CLOEXEC)) < 0) return {{}, open_fail(sp.stdin_file)};
    if (!sp.stdout_file.empty() && (fout = ::open(sp.stdout_file.c_str(), oflags, 0644)) < 0) return {{}, open_fail(sp.stdout_file)};
    if (!sp.stderr_file.empty() && !sp.merge_stderr && (ferr = ::open(sp.stderr_file.c_str(), oflags, 0644)) < 0) return {{}, open_fail(sp.stderr_file)};
    // every end is close-on-exec: the child moves the ones it needs onto 0/1/2 (dup_to)
    if (fout < 0 && (sp.capture_stdout || sp.merge_stderr) && detail::pipe_cloexec(pout)) return {{}, open_fail("pipe")};
    if (ferr < 0 && sp.capture_stderr && !sp.merge_stderr && detail::pipe_cloexec(perr)) { auto e = open_fail("pipe"); cl(pout[0]); cl(pout[1]); return {{}, e}; }
    const bool use_stdin = sp.stdin_data.has_value() && fin < 0;
    int devnull = (use_stdin || fin >= 0) ? -1 : ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    const bool new_session = sp.new_session;
    const char* cwd = sp.cwd.empty() ? nullptr : sp.cwd.c_str();
    const proc_limits lim = sp.limits;
    pid_t p = forge::os::serialized_fork();
    if (p < 0) { auto e = last_error("fork"); for (int* f : {&pin[0], &pin[1], &pout[0], &pout[1], &perr[0], &perr[1], &pexec[0], &pexec[1]}) cl(*f); return {{}, e}; }
    if (p == 0) {
        if (new_session) ::setsid(); else ::setpgid(0, 0);
        ::signal(SIGPIPE, SIG_DFL);
        using detail_proc::dup_to;
        if (fin >= 0) dup_to(fin, 0); else if (use_stdin) dup_to(pin[0], 0); else if (devnull >= 0) dup_to(devnull, 0);
        const int o1 = fout >= 0 ? fout : pout[1];
        if (o1 >= 0) dup_to(o1, 1);
        if (sp.merge_stderr && o1 >= 0) dup_to(o1, 2); else if (ferr >= 0) dup_to(ferr, 2); else if (perr[1] >= 0) dup_to(perr[1], 2);
        (void)apply_limits(lim);
        if (cwd && ::chdir(cwd) != 0) { int e = errno; (void)!::write(pexec[1], &e, sizeof e); ::_exit(127); }
        ::execve(exe.c_str(), av.data(), ev.data());
        int e = errno; (void)!::write(pexec[1], &e, sizeof e); ::_exit(127);
    }
    if (!new_session) ::setpgid(p, p);   // also in the parent, to close the race (setsid makes p the group id too)
#if defined(FORGE_PROC_MEMORY_WATCHDOG)
    if (lim.memory_bytes) detail_proc::memory_watchdog(p, lim.memory_bytes);
#endif
    cl(pin[0]); cl(pout[1]); cl(perr[1]); cl(pexec[1]); cl(fin); cl(fout); cl(ferr); if (devnull >= 0) ::close(devnull);
    int child_errno = 0;
    { auto n = ::read(pexec[0], &child_errno, sizeof child_errno); if (n != sizeof child_errno) child_errno = 0; cl(pexec[0]); }
    if (child_errno) {
        cl(pin[1]); cl(pout[0]); cl(perr[0]); int st; ::waitpid(p, &st, 0);
        return {{}, {child_errno, "spawn " + sp.argv[0] + ": " + error_text(child_errno)}};
    }
    std::string in = use_stdin ? *sp.stdin_data : std::string{};
    std::size_t in_off = 0;
    // stop / memory watchdog: checked at least every 50 ms
    const bool watching = sp.stop.stop_possible() || sp.watch_memory_bytes;
    std::uint64_t next_mem_check = 0;
    auto check_watch = [&]() -> bool {
        if (sp.stop.stop_requested()) { res.cancelled = true; return true; }
#if defined(FORGE_PROC_GROUP_RSS)
        const std::uint64_t now = monotonic_ns();
        if (sp.watch_memory_bytes && now >= next_mem_check && (next_mem_check = now + 50000000ull, true) && detail_proc::group_rss(p) > sp.watch_memory_bytes && detail_proc::group_rss(p) > sp.watch_memory_bytes) { res.memory_killed = true; return true; }
#endif
        return false;
    };
    if (in.empty()) cl(pin[1]); else detail_proc::set_nonblock(pin[1]);
    if (pout[0] >= 0) detail_proc::set_nonblock(pout[0]);
    if (perr[0] >= 0) detail_proc::set_nonblock(perr[0]);
    std::uint64_t deadline = sp.timeout_ms ? t0 + sp.timeout_ms * 1000000ull : 0;
    char buf[16384];
    auto drain = [&](int& fd, std::string& dst) {
        for (;;) { auto n = ::read(fd, buf, sizeof buf);
            if (n > 0) { if (dst.size() + std::size_t(n) <= sp.max_output) dst.append(buf, std::size_t(n)); else res.truncated = true; continue; }
            if (n == 0) { cl(fd); } else if (errno == EINTR) continue;
            break; }
    };
    // The direct child is polled too: once it exits, what is in the pipes is drained and the loop ends,
    // even if a background grandchild still holds a pipe open (it is not waited for).
    int st = 0;
    struct rusage ru{};
    bool reaped = false;
    while (pout[0] >= 0 || perr[0] >= 0 || pin[1] >= 0) {
        if (!reaped) { pid_t w = ::wait4(p, &st, WNOHANG, &ru); if (w == p) reaped = true; }
        if (reaped) {
            if (pout[0] >= 0) drain(pout[0], res.out);
            if (perr[0] >= 0) drain(perr[0], res.err);
            break;
        }
        struct pollfd fds[3]; int nf = 0, io = -1, ie = -1, ii = -1;
        if (pout[0] >= 0) { io = nf; fds[nf++] = {pout[0], POLLIN, 0}; }
        if (perr[0] >= 0) { ie = nf; fds[nf++] = {perr[0], POLLIN, 0}; }
        if (pin[1] >= 0) { ii = nf; fds[nf++] = {pin[1], POLLOUT, 0}; }
        int to = -1;
        if (deadline) { auto now = monotonic_ns(); if (now >= deadline) { res.timed_out = true; break; } to = int(std::min<std::uint64_t>((deadline - now) / 1000000 + 1, 1000)); }
        if (watching) { if (check_watch()) break; }
        to = to < 0 ? 50 : std::min(to, 50);
        int r = ::poll(fds, nfds_t(nf), to);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (io >= 0 && fds[io].revents) drain(pout[0], res.out);
        if (ie >= 0 && fds[ie].revents) drain(perr[0], res.err);
        if (ii >= 0 && fds[ii].revents) {
            if (fds[ii].revents & (POLLERR | POLLHUP)) cl(pin[1]);
            else { auto n = ::write(pin[1], in.data() + in_off, std::min<std::size_t>(in.size() - in_off, 65536));
                   if (n > 0) in_off += std::size_t(n); else if (n < 0 && errno != EAGAIN && errno != EINTR) cl(pin[1]);
                   if (in_off >= in.size()) cl(pin[1]); }
        }
    }
    auto killed = [&] { return res.timed_out || res.cancelled || res.memory_killed; };
    if (killed()) detail_proc::kill_tree(p, new_session);
    cl(pin[1]); cl(pout[0]); cl(perr[0]);
    std::uint64_t kill_at = killed() ? monotonic_ns() : 0;
    while (!reaped) {
        // once killed, never block: a child stuck in uninterruptible sleep is left to a detached reaper
        pid_t w = ::wait4(p, &st, ((deadline || watching) && !killed()) || killed() ? WNOHANG : 0, &ru);
        if (w == p) { reaped = true; break; }
        if (w < 0 && errno != EINTR) { reaped = true; ru = {}; break; }
        if (w == 0) {
            if (!killed()) { if (deadline && monotonic_ns() >= deadline) res.timed_out = true; else if (watching) (void)check_watch(); }
            if (killed()) {
                if (!kill_at) kill_at = monotonic_ns();
                detail_proc::kill_tree(p, new_session);
                if (monotonic_ns() - kill_at > 2000000000ull) break;
            }
            sleep_ns(1000000);
        }
    }
    // A new session owns everything it started: after a normal exit, sweep leftovers (background
    // children, members that moved to their own group) too. The pgid/sid p cannot be reused while any member lives.
    if (reaped && new_session && !killed()) detail_proc::kill_tree(p, true);
    if (!reaped) {
        res.unreaped = true;
        std::thread([p] { int s2 = 0; while (::waitpid(p, &s2, 0) < 0 && errno == EINTR) {} }).detach();
    } else {
        res.peak_rss_bytes = detail_proc::maxrss_bytes(ru);
    }
    if (WIFEXITED(st)) res.exit_code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) { res.term_signal = WTERMSIG(st); res.exit_code = 128 + res.term_signal; }
    if (res.timed_out) res.exit_code = 124;
    else if (res.cancelled) res.exit_code = 125;
    else if (res.memory_killed) res.exit_code = 137;
    res.wall_ms = (monotonic_ns() - t0) / 1000000;
    return {std::move(res), {}};
#endif
}

// Live process for streaming use: spawn without waiting; read/wait/kill later. RAII: the destructor kills
// and reaps a child that is still running, so a forgotten handle never leaves a zombie.
// Windows: the child runs in a Job Object (which carries limits.memory_bytes / cpu_seconds); kill() and the
// destructor terminate the whole job, i.e. the child and everything it started. There are no signals: any
// kill(sig) with sig != 0 terminates with exit code 128+sig, so wait() reports what POSIX would.
class child {
public:
    child() = default;
    child(const child&) = delete; child& operator=(const child&) = delete;
    child(child&& o) noexcept { swap(o); }
    child& operator=(child&& o) noexcept { if (this != &o) { reset(); swap(o); } return *this; }
    ~child() { reset(); }
    std::uint64_t pid() const { return std::uint64_t(pid_); }
    bool running() const { return pid_ > 0 && !done_; }
    // Start argv with stdout (and stderr merged when asked) readable through read().
    static result<child> start(const std::vector<std::string>& argv, bool merge_stderr = false, const std::string& cwd = {}) {
        child c;
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
        std::string exe = find_executable(argv.empty() ? std::string{} : argv[0]);
        if (exe.empty()) return {{}, {ENOENT, "spawn: command not found"}};
        std::vector<char*> av; for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
        int po[2]; if (detail::pipe_cloexec(po)) return {{}, last_error("pipe")};
        const char* wd = cwd.empty() ? nullptr : cwd.c_str();
        pid_t p = forge::os::serialized_fork();
        if (p < 0) { ::close(po[0]); ::close(po[1]); return {{}, last_error("fork")}; }
        if (p == 0) { ::setpgid(0, 0); detail_proc::dup_to(po[1], 1); if (merge_stderr) detail_proc::dup_to(po[1], 2);
                      if (wd && ::chdir(wd) != 0) ::_exit(127); ::execve(exe.c_str(), av.data(), environ); ::_exit(127); }
        ::setpgid(p, p); ::close(po[1]);
        c.pid_ = p; c.out_ = po[0];
        return {std::move(c), {}};
#elif defined(FORGE_OS_WINDOWS)
        (void)c;
        if (argv.empty()) return {{}, {ERROR_FILE_NOT_FOUND, "spawn: command not found"}};
        spawn_spec sp; sp.argv = argv; sp.merge_stderr = merge_stderr; sp.cwd = cwd;
        return start_win(sp, true);
#else
        (void)c; (void)argv; (void)merge_stderr; (void)cwd;
        return {{}, {unsupported_code, "child::start: process spawning is not available on iOS"}};
#endif
    }
    // Start from a full spawn_spec: argv, cwd, env_set/env_unset/clear_env, limits (memory/cpu/files) and
    // merge_stderr. stdout (plus stderr when merged) is readable through read(); stdin is /dev/null and an
    // unmerged stderr is inherited. An exec failure (e.g. command not found) is reported here, not at wait().
    static result<child> start(const spawn_spec& sp) {
        child c;
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
        if (sp.argv.empty()) return {{}, {EINVAL, "spawn: empty argv"}};
        std::string exe = find_executable(sp.argv[0]);
        if (exe.empty()) return {{}, {ENOENT, "spawn " + sp.argv[0] + ": command not found"}};
        std::vector<std::string> envs;
        if (!sp.clear_env) for (char** e = environ; e && *e; ++e) {
            std::string s(*e); auto eq = s.find('='); std::string k = s.substr(0, eq);
            bool drop = sp.env_set.count(k) > 0;
            for (auto& u : sp.env_unset) if (u == k) drop = true;
            if (!drop) envs.push_back(std::move(s));
        }
        for (auto& [k, v] : sp.env_set) envs.push_back(k + "=" + v);
        std::vector<char*> av, ev;
        for (auto& a : sp.argv) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
        for (auto& e : envs) ev.push_back(const_cast<char*>(e.c_str())); ev.push_back(nullptr);
        int po[2], px[2];
        if (detail::pipe_cloexec(po)) return {{}, last_error("pipe")};
        if (detail::pipe_cloexec(px)) { auto e = last_error("pipe"); ::close(po[0]); ::close(po[1]); return {{}, e}; }
        int devnull = sp.stdin_file.empty() ? ::open("/dev/null", O_RDONLY | O_CLOEXEC) : ::open(sp.stdin_file.c_str(), O_RDONLY | O_CLOEXEC);
        int ferr = -1;
        if (!sp.stderr_file.empty() && !sp.merge_stderr)
            ferr = ::open(sp.stderr_file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (sp.append_output ? O_APPEND : O_TRUNC), 0644);
        if ((!sp.stdin_file.empty() && devnull < 0) || (!sp.stderr_file.empty() && !sp.merge_stderr && ferr < 0)) {
            auto e = last_error("open"); for (int fd : {po[0], po[1], px[0], px[1], devnull, ferr}) if (fd >= 0) ::close(fd); return {{}, e}; }
        const char* wd = sp.cwd.empty() ? nullptr : sp.cwd.c_str();
        const proc_limits lim = sp.limits; const bool merge = sp.merge_stderr; const bool new_session = sp.new_session;
        pid_t p = forge::os::serialized_fork();
        if (p < 0) { auto e = last_error("fork"); for (int fd : {po[0], po[1], px[0], px[1], devnull}) if (fd >= 0) ::close(fd); return {{}, e}; }
        if (p == 0) {
            if (new_session) ::setsid(); else ::setpgid(0, 0);
            ::signal(SIGPIPE, SIG_DFL);
            using detail_proc::dup_to;
            if (devnull >= 0) dup_to(devnull, 0);
            dup_to(po[1], 1); if (merge) dup_to(po[1], 2); else if (ferr >= 0) dup_to(ferr, 2);
            (void)apply_limits(lim);
            if (wd && ::chdir(wd) != 0) { int e = errno; (void)!::write(px[1], &e, sizeof e); ::_exit(127); }
            ::execve(exe.c_str(), av.data(), ev.data());
            int e = errno; (void)!::write(px[1], &e, sizeof e); ::_exit(127);
        }
        if (!new_session) ::setpgid(p, p);
#if defined(FORGE_PROC_MEMORY_WATCHDOG)
        if (lim.memory_bytes) detail_proc::memory_watchdog(p, lim.memory_bytes);
#endif
        ::close(po[1]); ::close(px[1]); if (devnull >= 0) ::close(devnull); if (ferr >= 0) ::close(ferr);
        int child_errno = 0;
        { auto n = ::read(px[0], &child_errno, sizeof child_errno); if (n != sizeof child_errno) child_errno = 0; ::close(px[0]); }
        if (child_errno) { int st; while (::waitpid(p, &st, 0) < 0 && errno == EINTR) {} ::close(po[0]); errno = child_errno; return {{}, last_error("exec " + sp.argv[0])}; }
        c.pid_ = p; c.out_ = po[0];
        return {std::move(c), {}};
#elif defined(FORGE_OS_WINDOWS)
        (void)c;
        if (sp.argv.empty()) return {{}, {ERROR_INVALID_PARAMETER, "spawn: empty argv"}};
        return start_win(sp, false);
#else
        (void)c; (void)sp;
        return {{}, {unsupported_code, "child::start: process spawning is not available on iOS"}};
#endif
    }
    // Non-blocking: the exit code (128+signal) if the child has exited, else -1.
    int try_wait() {
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
        if (done_) return code_;
        if (pid_ <= 0) return -1;
        int st = 0; pid_t w;
        do { w = ::waitpid(pid_, &st, WNOHANG); } while (w < 0 && errno == EINTR);
        if (w != pid_) return -1;
        done_ = true; code_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        return code_;
#elif defined(FORGE_OS_WINDOWS)
        if (done_) return code_;
        if (!proc_ || WaitForSingleObject(proc_, 0) != WAIT_OBJECT_0) return -1;
        return reap();
#else
        return -1;
#endif
    }
    bool exited() const { return done_; }
    int exit_code() const { return done_ ? code_ : -1; }
    // Read up to `max` bytes of output; empty string at end of stream. Waits at most timeout_ms (0 = block).
    std::string read(std::size_t max = 65536, std::uint64_t timeout_ms = 0) {
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
        if (out_ < 0) return {};
        if (timeout_ms) { struct pollfd f{out_, POLLIN, 0}; if (::poll(&f, 1, int(timeout_ms)) <= 0) return {}; }
        std::string s(max, '\0');
        auto n = ::read(out_, s.data(), max);
        if (n <= 0) { ::close(out_); out_ = -1; return {}; }
        s.resize(std::size_t(n)); return s;
#elif defined(FORGE_OS_WINDOWS)
        if (!hout_) return {};
        if (timeout_ms) {   // anonymous pipes cannot be waited on: peek with a short back-off (1 ms rising to 5 ms)
            const std::uint64_t end = monotonic_ns() + timeout_ms * 1000000ull; std::uint64_t nap = 1000000ull;
            for (;;) {
                DWORD avail = 0;
                if (!PeekNamedPipe(hout_, nullptr, 0, nullptr, &avail, nullptr) || avail) break;   // data, or EOF: ReadFile tells
                if (monotonic_ns() >= end) return {};
                sleep_ns(nap); nap = std::min<std::uint64_t>(nap * 2, 5000000ull);
            }
        }
        std::string s(max, '\0'); DWORD n = 0;
        if (!ReadFile(hout_, s.data(), DWORD(std::min<std::size_t>(max, 1u << 30)), &n, nullptr) || !n) { CloseHandle(hout_); hout_ = nullptr; return {}; }
        s.resize(n); return s;
#else
        (void)max; (void)timeout_ms; return {};
#endif
    }
    // Signal the whole process group (Windows: terminate the Job Object with exit code 128+sig; sig 0 only
    // reports whether the child is still running).
    bool kill(int sig = 9) {
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
        return pid_ > 0 && !done_ && ::kill(-pid_, sig) == 0;
#elif defined(FORGE_OS_WINDOWS)
        if (pid_ <= 0 || done_ || !proc_) return false;
        if (sig == 0) return WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT;
        return job_ ? TerminateJobObject(job_, UINT(128 + sig)) != 0 : TerminateProcess(proc_, UINT(128 + sig)) != 0;
#else
        (void)sig; return false;
#endif
    }
    // Wait for exit; returns the exit code (128+signal), or -1 on timeout.
    int wait(std::uint64_t timeout_ms = 0) {
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
        if (done_) return code_;
        if (pid_ <= 0) return -1;
        std::uint64_t end = timeout_ms ? monotonic_ns() + timeout_ms * 1000000ull : 0;
        int st = 0;
        for (;;) {
            pid_t w = ::waitpid(pid_, &st, end ? WNOHANG : 0);
            if (w == pid_) break;
            if (w < 0 && errno != EINTR) return -1;
            if (w == 0) { if (monotonic_ns() >= end) return -1; sleep_ns(1000000); }
        }
        done_ = true; code_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        return code_;
#elif defined(FORGE_OS_WINDOWS)
        if (done_) return code_;
        if (!proc_) return -1;
        const std::uint64_t end = timeout_ms ? monotonic_ns() + timeout_ms * 1000000ull : 0;
        for (;;) {
            DWORD slice = INFINITE;
            if (end) { std::uint64_t now = monotonic_ns(); if (now >= end) return -1; slice = DWORD(std::min<std::uint64_t>((end - now + 999999) / 1000000, 0x7fffffffu)); }
            DWORD w = WaitForSingleObject(proc_, slice);
            if (w == WAIT_OBJECT_0) return reap();
            if (w != WAIT_TIMEOUT) return -1;
        }
#else
        (void)timeout_ms; return -1;
#endif
    }
private:
    void swap(child& o) noexcept {
        std::swap(pid_, o.pid_); std::swap(out_, o.out_); std::swap(done_, o.done_); std::swap(code_, o.code_);
#if defined(FORGE_OS_WINDOWS)
        std::swap(proc_, o.proc_); std::swap(job_, o.job_); std::swap(hout_, o.hout_);
#endif
    }
    void reset() {
#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
        if (out_ >= 0) { ::close(out_); out_ = -1; }
        if (pid_ > 0 && !done_) { ::kill(-pid_, SIGKILL); int st; while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {} }
#elif defined(FORGE_OS_WINDOWS)
        if (hout_) { CloseHandle(hout_); hout_ = nullptr; }
        if (proc_) {
            if (!done_) { if (!job_ || !TerminateJobObject(job_, 128 + 9)) TerminateProcess(proc_, 128 + 9); WaitForSingleObject(proc_, INFINITE); }
            CloseHandle(proc_); proc_ = nullptr;
        }
        if (job_) { CloseHandle(job_); job_ = nullptr; }
#endif
        pid_ = -1; done_ = false; code_ = -1;
    }
#if defined(FORGE_OS_WINDOWS)
    int reap() { DWORD code = 0; GetExitCodeProcess(proc_, &code); done_ = true; code_ = int(code); return code_; }
    // stdout (plus stderr when merged) is a pipe read through read(); stdin is the parent's (inherit_stdin, the
    // argv overload) or stdin_file / NUL; an unmerged stderr goes to stderr_file or is inherited. The process
    // starts suspended, joins its Job Object (no kill-on-close: like POSIX, what outlives the child is not
    // swept), then resumes. CreateProcess failure (not found, bad cwd) is reported here, and so is a job the
    // process cannot join (it is terminated rather than run without its limits and tree kill).
    static result<child> start_win(const spawn_spec& sp, bool inherit_stdin) {
        child c;
        std::string exe = find_executable(sp.argv[0]);
        if (exe.empty()) return {{}, {ERROR_FILE_NOT_FOUND, "spawn " + sp.argv[0] + ": not found"}};
        error cmd_err{};
        std::wstring cmd = detail_proc::win_cmdline(sp.argv, exe, &cmd_err), envb = detail_proc::win_env_block(sp);
        if (cmd.empty()) return {{}, cmd_err};
        auto closeh = [](HANDLE& h) { if (h) { CloseHandle(h); h = nullptr; } };
        HANDLE fin = nullptr, ferr = nullptr, out_r = nullptr, out_w = nullptr;
        if (!inherit_stdin) {
            const std::string f = sp.stdin_file.empty() ? std::string("NUL") : sp.stdin_file;
            fin = CreateFileW(widen(f).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (fin == INVALID_HANDLE_VALUE) return {{}, last_error("open " + f)};
        }
        if (!sp.stderr_file.empty() && !sp.merge_stderr) {
            ferr = CreateFileW(widen(sp.stderr_file).c_str(), sp.append_output ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, sp.append_output ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (ferr == INVALID_HANDLE_VALUE) { auto e = last_error("open " + sp.stderr_file); closeh(fin); return {{}, e}; }
        }
        // not inheritable: create_process_std_only hands the child inheritable duplicates of exactly these
        if (!CreatePipe(&out_r, &out_w, nullptr, 0)) { auto e = last_error("CreatePipe"); closeh(fin); closeh(ferr); return {{}, e}; }
        STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inherit_stdin ? GetStdHandle(STD_INPUT_HANDLE) : fin;
        si.hStdOutput = out_w;
        si.hStdError = sp.merge_stderr ? out_w : ferr ? ferr : GetStdHandle(STD_ERROR_HANDLE);
        HANDLE job = detail_proc::win_job(sp.limits, false);
        if (!job) { auto e = last_error("CreateJobObject"); closeh(out_r); closeh(out_w); closeh(fin); closeh(ferr); return {{}, e}; }
        PROCESS_INFORMATION pi{};
        std::wstring wcwd = widen(sp.cwd);
        BOOL ok = create_process_std_only(widen(exe).c_str(), cmd.data(), CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (sp.new_session ? CREATE_NEW_PROCESS_GROUP : 0),
                                          envb.data(), sp.cwd.empty() ? nullptr : wcwd.c_str(), si, pi);
        closeh(out_w); closeh(fin); closeh(ferr);
        if (!ok) { auto e = last_error("CreateProcess " + sp.argv[0]); closeh(out_r); closeh(job); return {{}, e}; }
        if (auto e = detail_proc::win_assign(job, pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); closeh(out_r); closeh(job); return {{}, e}; }
        ResumeThread(pi.hThread); CloseHandle(pi.hThread);
        c.pid_ = static_cast<long long>(pi.dwProcessId); c.proc_ = pi.hProcess; c.job_ = job; c.hout_ = out_r;
        return {std::move(c), {}};
    }
    HANDLE proc_ = nullptr, job_ = nullptr, hout_ = nullptr;
#endif
#if defined(FORGE_OS_POSIX)
    pid_t pid_ = -1;
#else
    long long pid_ = -1;
#endif
    int out_ = -1; bool done_ = false; int code_ = -1;
};

// Wait for whichever of several children exits first. Returns its index into `cs` (that child is reaped,
// so exit_code() is valid), or nullopt when timeout_ms (0 = forever) passes with all still running, or when
// every child has already been reaped. Null entries and finished children are skipped. Polls with a short
// back-off (1 ms rising to 10 ms) so it is portable and never steals another waiter's child.
inline std::optional<std::size_t> wait_any(std::span<child* const> cs, std::uint64_t timeout_ms = 0) {
    std::uint64_t end = timeout_ms ? monotonic_ns() + timeout_ms * 1000000ull : 0;
    std::uint64_t nap = 1000000ull;
    for (;;) {
        bool any_live = false;
        for (std::size_t i = 0; i < cs.size(); ++i) {
            child* c = cs[i];
            if (!c || c->exited() || !c->running()) continue;
            any_live = true;
            if (c->try_wait() != -1) return i;
        }
        if (!any_live) return std::nullopt;
        if (end && monotonic_ns() >= end) return std::nullopt;
        sleep_ns(nap); nap = std::min<std::uint64_t>(nap * 2, 10000000ull);
    }
}
// Wait until every child has exited; false on timeout (the ones that finished are reaped either way).
inline bool wait_all(std::span<child* const> cs, std::uint64_t timeout_ms = 0) {
    std::uint64_t end = timeout_ms ? monotonic_ns() + timeout_ms * 1000000ull : 0;
    for (;;) {
        auto i = wait_any(cs, end ? std::max<std::uint64_t>(1, (end - std::min(end, monotonic_ns())) / 1000000ull) : 0);
        if (!i) break;
    }
    for (auto* c : cs) if (c && c->running()) return false;
    return true;
}

// One running process as seen by list_processes()/process_info(). name is the short command name (Linux,
// Android, macOS, FreeBSD: the kernel's comm, at most 15-19 chars; Windows: the image file name, "foo.exe");
// cmdline is argv joined by spaces (Windows: the raw command line). rss_bytes is the resident set (Windows: the
// working set). Fields the caller may not read stay empty / 0: another user's argv or RSS on macOS, protected
// and other users' processes on Windows (rss and cmdline), kernel threads everywhere (cmdline).
struct process_entry { std::uint64_t pid = 0, ppid = 0, rss_bytes = 0; std::string name, cmdline; };

#if defined(FORGE_OS_MACOS) || defined(FORGE_OS_FREEBSD)
namespace detail_proc {
// NUL-separated strings in [q, e) -> at most `count` of them joined by spaces.
inline std::string join_args(const char* q, const char* e, std::size_t count = ~std::size_t(0)) {
    std::string s;
    for (std::size_t i = 0; i < count && q < e; ++i) {
        std::size_t l = ::strnlen(q, std::size_t(e - q));
        if (i) s += ' ';
        s.append(q, l); q += l + 1;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
inline std::size_t arg_max() {
    int mib[2] = {CTL_KERN, KERN_ARGMAX}, v = 0; std::size_t sz = sizeof v;
    return ::sysctl(mib, 2, &v, &sz, nullptr, 0) == 0 && v > 0 ? std::size_t(v) : std::size_t(1) << 20;
}
}
#endif

inline result<std::vector<process_entry>> list_processes() {
    std::vector<process_entry> v;
#if defined(FORGE_OS_LINUX) || defined(FORGE_OS_ANDROID)
    auto ls = list_dir("/proc");
    if (!ls) return {{}, ls.err};
    long page = ::sysconf(_SC_PAGESIZE);
    for (auto& e : ls.value) {
        if (e.name.empty() || e.name.find_first_not_of("0123456789") != std::string::npos) continue;
        process_entry p; p.pid = std::stoull(e.name);
        auto st = read_file("/proc/" + e.name + "/stat");
        if (!st) continue;
        auto& s = st.value; auto l = s.find('('), r = s.rfind(')');
        if (l == std::string::npos || r == std::string::npos) continue;
        p.name = s.substr(l + 1, r - l - 1);
        std::vector<std::string> f; std::size_t i = r + 2;
        while (i < s.size()) { auto j = s.find(' ', i); if (j == std::string::npos) j = s.size(); f.push_back(s.substr(i, j - i)); i = j + 1; }
        if (f.size() > 21) { p.ppid = std::stoull(f[1]); p.rss_bytes = std::stoull(f[21]) * std::uint64_t(page); }
        if (auto c = read_file("/proc/" + e.name + "/cmdline")) { p.cmdline = c.value; for (auto& ch : p.cmdline) if (ch == '\0') ch = ' '; while (!p.cmdline.empty() && p.cmdline.back() == ' ') p.cmdline.pop_back(); }
        v.push_back(std::move(p));
    }
    return {std::move(v), {}};
#elif defined(FORGE_OS_WINDOWS)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {{}, last_error("CreateToolhelp32Snapshot")};
    // Another process's command line: NtQueryInformationProcess(ProcessCommandLineInformation = 60), Windows 8.1+,
    // needs only PROCESS_QUERY_LIMITED_INFORMATION. ntdll is always loaded; resolved at run time (no import lib).
    using nqip_t = LONG (WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto nqip = ntdll ? reinterpret_cast<nqip_t>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationProcess"))) : nullptr;
    struct ustr { USHORT len, max; PWSTR buf; };   // UNICODE_STRING
    std::vector<unsigned char> buf(8192);
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        process_entry p; p.pid = pe.th32ProcessID; p.ppid = pe.th32ParentProcessID; p.name = narrow(pe.szExeFile);
        if (HANDLE h = pe.th32ProcessID ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID) : nullptr) {
            PROCESS_MEMORY_COUNTERS pmc{};
            if (GetProcessMemoryInfo(h, &pmc, sizeof pmc)) p.rss_bytes = std::uint64_t(pmc.WorkingSetSize);
            if (nqip) {
                ULONG need = 0;
                LONG st = nqip(h, 60, buf.data(), ULONG(buf.size()), &need);
                if (st < 0 && need > buf.size()) { buf.resize(need); st = nqip(h, 60, buf.data(), ULONG(buf.size()), &need); }
                if (st >= 0) {
                    ustr u{}; std::memcpy(&u, buf.data(), sizeof u);
                    if (u.buf && u.len) p.cmdline = narrow(std::wstring_view(u.buf, u.len / sizeof(wchar_t)));
                    while (!p.cmdline.empty() && p.cmdline.back() == ' ') p.cmdline.pop_back();
                }
            }
            CloseHandle(h);
        }
        v.push_back(std::move(p));
    }
    CloseHandle(snap);
    return {std::move(v), {}};
#elif defined(FORGE_OS_MACOS)
    auto pids = detail_proc::list_pids(PROC_ALL_PIDS, 0);
    if (pids.empty()) return {{}, last_error("proc_listpids")};
    std::vector<char> args(detail_proc::arg_max());
    for (pid_t pid : pids) {
        // PROC_PIDT_SHORTBSDINFO needs no privilege; the resident size and argv are readable for own processes
        // (all of them as root).
        proc_bsdshortinfo bi{};
        if (::proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &bi, int(sizeof bi)) != int(sizeof bi)) continue;   // exited meanwhile
        process_entry p; p.pid = std::uint64_t(pid); p.ppid = std::uint64_t(bi.pbsi_ppid);
        p.name.assign(bi.pbsi_comm, ::strnlen(bi.pbsi_comm, sizeof bi.pbsi_comm));
        rusage_info_v2 ri{};
        if (::proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) == 0) p.rss_bytes = ri.ri_resident_size;
        // KERN_PROCARGS2: int argc, the exec path, NUL padding, then argv[0..argc) (then the environment)
        int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid}; std::size_t sz = args.size();
        if (::sysctl(mib, 3, args.data(), &sz, nullptr, 0) == 0 && sz > sizeof(int)) {
            int argc = 0; std::memcpy(&argc, args.data(), sizeof argc);
            const char* q = args.data() + sizeof(int); const char* e = args.data() + sz;
            while (q < e && *q) ++q;
            while (q < e && !*q) ++q;
            if (argc > 0) p.cmdline = detail_proc::join_args(q, e, std::size_t(argc));
        }
        v.push_back(std::move(p));
    }
    return {std::move(v), {}};
#elif defined(FORGE_OS_FREEBSD)
    auto ks = detail_proc::procs(KERN_PROC_PROC, 0);
    if (ks.empty()) return {{}, last_error("sysctl kern.proc.proc")};
    const std::uint64_t pg = std::uint64_t(::getpagesize());
    std::vector<char> args(detail_proc::arg_max());
    for (auto& k : ks) {
        process_entry p; p.pid = std::uint64_t(k.ki_pid); p.ppid = std::uint64_t(k.ki_ppid);
        p.rss_bytes = std::uint64_t(k.ki_rssize) * pg; p.name = k.ki_comm;
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, k.ki_pid}; std::size_t sz = args.size();
        if (::sysctl(mib, 4, args.data(), &sz, nullptr, 0) == 0 && sz) p.cmdline = detail_proc::join_args(args.data(), args.data() + sz);
        v.push_back(std::move(p));
    }
    return {std::move(v), {}};
#else
    return {{}, {unsupported_code, "list_processes: other processes are not visible on this platform"}};
#endif
}

inline result<process_entry> process_info(std::uint64_t pid) {
    auto all = list_processes();
    if (!all) return {{}, all.err};
    for (auto& p : all.value) if (p.pid == pid) return {p, {}};
    return {{}, {-1, "process_info: no such process"}};
}

} // namespace forge::os
