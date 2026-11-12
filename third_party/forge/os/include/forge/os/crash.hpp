#pragma once
// Crash handling: stack traces and crash reports.
//   stack_trace(): _Unwind_Backtrace + dladdr on POSIX; CaptureStackBackTrace + DbgHelp SymFromAddr on Windows.
//   crash_handler::install({dir, app, minidump, after}): a text report <dir>/<app>-<pid>.crash on a crash (POSIX
//   signals on an alternate stack, then the previous handler or the default action, so the OS core dump /
//   tombstone / ReportCrash still happens; Windows SEH and the CRT abort / invalid-parameter / purecall paths,
//   plus a minidump <dir>/<app>-<pid>.dmp). Details at crash_handler below.
#include <forge/os/detail.hpp>
#if defined(FORGE_OS_POSIX)
#include <unwind.h>
#endif
#include <atomic>
#include <csignal>
#include <exception>

namespace forge::os {
struct stack_frame { void* addr = nullptr; std::string symbol, module; std::uintptr_t offset = 0; };
namespace detail {
#if defined(FORGE_OS_POSIX)
struct unwind_state { void** out; int n, max, skip; };
inline _Unwind_Reason_Code unwind_cb(struct _Unwind_Context* c, void* arg) {
    auto* s = static_cast<unwind_state*>(arg); auto ip = _Unwind_GetIP(c);
    if (!ip) return _URC_END_OF_STACK;
    if (s->skip > 0) { --s->skip; return _URC_NO_REASON; }
    if (s->n >= s->max) return _URC_END_OF_STACK;
    s->out[s->n++] = reinterpret_cast<void*>(ip); return _URC_NO_REASON;
}
#endif
inline int raw_backtrace(void** out, int max, int skip) {
#if defined(FORGE_OS_WINDOWS)
    return int(CaptureStackBackTrace(DWORD(skip + 1), DWORD(max), out, nullptr));
#else
    unwind_state s{out, 0, max, skip + 1}; _Unwind_Backtrace(unwind_cb, &s); return s.n;
#endif
}
#if defined(FORGE_OS_WINDOWS)
struct dbghelp {
    HMODULE h = nullptr; bool inited = false;
    BOOL(WINAPI* SymInitialize)(HANDLE, PCSTR, BOOL) = nullptr;
    BOOL(WINAPI* SymFromAddr)(HANDLE, DWORD64, PDWORD64, void*) = nullptr;
    DWORD(WINAPI* SymSetOptions)(DWORD) = nullptr;
    BOOL(WINAPI* MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, int, void*, void*, void*) = nullptr;
    static dbghelp& get() {
        static dbghelp d = [] { dbghelp x; x.h = LoadLibraryExW(L"dbghelp.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/); if (!x.h) return x;
            x.SymInitialize = reinterpret_cast<decltype(x.SymInitialize)>(reinterpret_cast<void*>(GetProcAddress(x.h, "SymInitialize")));
            x.SymFromAddr = reinterpret_cast<decltype(x.SymFromAddr)>(reinterpret_cast<void*>(GetProcAddress(x.h, "SymFromAddr")));
            x.SymSetOptions = reinterpret_cast<decltype(x.SymSetOptions)>(reinterpret_cast<void*>(GetProcAddress(x.h, "SymSetOptions")));
            x.MiniDumpWriteDump = reinterpret_cast<decltype(x.MiniDumpWriteDump)>(reinterpret_cast<void*>(GetProcAddress(x.h, "MiniDumpWriteDump")));
            if (x.SymSetOptions) x.SymSetOptions(0x2 | 0x10 /*UNDNAME|LOAD_LINES*/);
            if (x.SymInitialize) x.inited = x.SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0;
            return x; }();
        return d;
    }
};
#endif
inline void symbolize(stack_frame& f) {
#if defined(FORGE_OS_WINDOWS)
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(f.addr), &m)) {
        wchar_t b[MAX_PATH]; DWORD n = GetModuleFileNameW(m, b, MAX_PATH); f.module = narrow({b, n}); f.offset = reinterpret_cast<std::uintptr_t>(f.addr) - reinterpret_cast<std::uintptr_t>(m);
    }
    auto& d = dbghelp::get();
    if (d.inited && d.SymFromAddr) {
        struct { ULONG SizeOfStruct, TypeIndex; ULONG64 Reserved[2]; ULONG Index, Size; ULONG64 ModBase; ULONG Flags; ULONG64 Value, Address; ULONG Register, Scope, Tag, NameLen, MaxNameLen; CHAR Name[256]; } si{};
        si.SizeOfStruct = 88; si.MaxNameLen = 255; DWORD64 disp = 0;
        if (d.SymFromAddr(GetCurrentProcess(), DWORD64(reinterpret_cast<std::uintptr_t>(f.addr)), &disp, &si)) f.symbol = si.Name;
    }
#else
    Dl_info i{};
    if (::dladdr(f.addr, &i)) {
        if (i.dli_fname) f.module = i.dli_fname; if (i.dli_sname) f.symbol = i.dli_sname;
        f.offset = reinterpret_cast<std::uintptr_t>(f.addr) - reinterpret_cast<std::uintptr_t>(i.dli_fbase);
    }
#endif
}
}
inline std::vector<stack_frame> stack_trace(int skip = 0, int max = 64) {
    std::vector<void*> raw(std::size_t(max > 0 ? max : 64)); int n = detail::raw_backtrace(raw.data(), int(raw.size()), skip + 1);
    std::vector<stack_frame> v; for (int i = 0; i < n; ++i) { stack_frame f; f.addr = raw[std::size_t(i)]; detail::symbolize(f); v.push_back(std::move(f)); }
    return v;
}
inline std::string format_stack_trace(const std::vector<stack_frame>& v) {
    std::string s; char b[64];
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::snprintf(b, sizeof b, "#%-2zu %p ", i, v[i].addr); s += b;
        s += v[i].symbol.empty() ? "??" : v[i].symbol;
        if (!v[i].module.empty()) { std::snprintf(b, sizeof b, "+0x%llx", (unsigned long long)v[i].offset); s += " (" + basename(v[i].module) + b + ")"; }
        s += '\n';
    }
    return s;
}

namespace detail {
// Fixed-buffer text building with no allocation, locale or stdio: safe inside a signal handler.
struct sig_text {
    char* p; std::size_t cap, n = 0;
    sig_text& s(const char* t) { while (t && *t && n + 1 < cap) p[n++] = *t++; p[n] = 0; return *this; }
    sig_text& dec(std::uint64_t v) { char t[24]; int i = 0; do { t[i++] = char('0' + v % 10); v /= 10; } while (v); while (i && n + 1 < cap) p[n++] = t[--i]; p[n] = 0; return *this; }
    sig_text& sdec(long long v) { if (v < 0) { s("-"); return dec(std::uint64_t(-(v + 1)) + 1); } return dec(std::uint64_t(v)); }
    sig_text& hex(std::uint64_t v) { char t[16]; int i = 0; do { t[i++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); s("0x"); while (i && n + 1 < cap) p[n++] = t[--i]; p[n] = 0; return *this; }
    sig_text& ptr(const void* v) { return hex(std::uint64_t(reinterpret_cast<std::uintptr_t>(v))); }
};
}

// crash_handler::install({dir, app, minidump, after}) writes <dir>/<app>-<pid>.crash (signal or exception, fault
// address, backtrace; Linux/Android add the module map) when the process crashes; Windows also writes a minidump
// <dir>/<app>-<pid>.dmp (MiniDumpWriteDump). The pid is the crashing process's, so a forked child writes its own.
//   dir: empty = a private per-user directory: POSIX <temp>/forge-crash-<euid>, created 0700 and refused unless
//        it is a real directory owned by us (a planted symlink or another user's directory is not used);
//        Windows %TEMP% (already per user). The report is created O_CREAT|O_EXCL|O_NOFOLLOW (POSIX) and never
//        follows or overwrites an existing file; on a name clash -1 .. -9 is appended.
//   after: called with the report path once it is written. POSIX: it runs inside the signal handler, so it must
//        be async-signal-safe (a plain function pointer, no captures).
// POSIX: SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP, on an alternate signal stack so a stack overflow is
// caught. Alternate stacks are per thread: install() sets one up for the calling thread only; every other
// thread whose overflow should be reported calls crash_handler::install_thread_stack() once (other faults are
// caught on any thread). The handler formats with detail::sig_text (no stdio/malloc); the unwinder and dladdr
// are warmed up by install() but are not formally async-signal-safe (a crash inside the dynamic loader itself
// can still hang there). Then the previous disposition runs: a previous handler is called (chained), the
// default action re-raises, so the core dump / tombstone / ReportCrash still happens. std::terminate is
// reported too, then the previous terminate handler runs.
// Windows: SetUnhandledExceptionFilter (the previous filter is chained), plus the CRT paths that bypass it:
// abort() (SIGABRT), invalid CRT parameters (_set_invalid_parameter_handler, exits 0xC0000417) and pure virtual
// calls (_set_purecall_handler, exits 3). Paths are wide (CreateFileW).
class crash_handler {
public:
    struct config { std::string dir, app = "forge"; bool minidump = true; void (*after)(const char* report_path) = nullptr; };
    static status install(config c) {
        auto& s = st();
#if defined(FORGE_OS_WINDOWS)
        if (c.dir.empty()) c.dir = temp_dir();
        if (auto m = mkdirs(c.dir); !m) return m;
#else
        if (c.dir.empty()) { auto d = private_dir_(); if (!d) return {{}, d.err}; c.dir = d.value; }
        else if (auto m = mkdirs(c.dir); !m) return m;
#endif
        if (c.dir.size() + c.app.size() + 48 > sizeof s.report || c.app.find_first_of("/\\") != std::string::npos) return {{}, {22, "crash_handler: dir too long or app contains a path separator"}};
        s.cfg = std::move(c);
        std::memcpy(s.dir, s.cfg.dir.c_str(), s.cfg.dir.size() + 1);
        std::memcpy(s.app, s.cfg.app.c_str(), s.cfg.app.size() + 1);
        build_path_(s.report, ".crash", 0); build_path_(s.dump, ".dmp", 0);
#if defined(FORGE_OS_WINDOWS)
        s.wreport = widen(s.report); s.wdump = widen(s.dump);
        detail::dbghelp::get();
        if (!s.installed) {
            s.prev_filter = SetUnhandledExceptionFilter(&seh_);
            s.prev_abrt = std::signal(SIGABRT, &abrt_);
            _set_invalid_parameter_handler(&invalid_param_);
            _set_purecall_handler(&purecall_);
        }
#else
        if (auto a = install_thread_stack(); !a) return a;
        // warm-up: load libgcc_s / libunwind and resolve dladdr's lazy bindings now, not inside the handler
        { void* w[4]; (void)detail::raw_backtrace(w, 4, 0); Dl_info di{}; (void)::dladdr(reinterpret_cast<void*>(&sig_), &di); }
        if (!s.installed) {
            for (int i = 0; i < nsig; ++i) {
                struct sigaction sa{}; sa.sa_sigaction = &sig_; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
                ::sigaction(sigs()[i], &sa, &s.prev[i]);
            }
        }
#endif
        if (!s.installed) s.prev_terminate = std::set_terminate([] {
            if (begin_()) { write_report_("std::terminate", nullptr, -1); end_(); }
            if (auto h = st().prev_terminate) h();
            std::abort();
        });
        s.installed = true; return {};
    }
    // Alternate signal stack (64 KiB or SIGSTKSZ) for the calling thread, freed when the thread ends; a thread
    // that already has one keeps it. Windows: nothing to do (stack overflow arrives through SEH).
    static status install_thread_stack() {
#if defined(FORGE_OS_POSIX)
        struct alt { void* p = nullptr; std::size_t n = 0; ~alt() { if (p) { stack_t d{}; d.ss_flags = SS_DISABLE; ::sigaltstack(&d, nullptr); ::munmap(p, n); } } };
        thread_local alt a;
        if (a.p) return {};
        stack_t cur{}; const std::size_t n = std::max<std::size_t>(std::size_t(1) << 16, std::size_t(SIGSTKSZ));
        if (::sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_size >= n / 2) return {};
        void* m = ::mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) return {{}, last_error("mmap (signal stack)")};
        stack_t ss{}; ss.ss_sp = m; ss.ss_size = n;
        if (::sigaltstack(&ss, nullptr)) { auto e = last_error("sigaltstack"); ::munmap(m, n); return {{}, e}; }
        a.p = m; a.n = n;
#endif
        return {};
    }
    // The report path for this process (after a crash: the file actually written).
    static const char* report_path() { auto& s = st(); if (s.installed && !s.crashed) build_path_(s.report, ".crash", 0); return s.report; }
    static const char* dump_path() { return st().dump; }
    static bool installed() { return st().installed; }
    // Write a dump of the running process on demand (Windows minidump; elsewhere a text report).
    static status write_dump_now(const std::string& path) {
#if defined(FORGE_OS_WINDOWS)
        auto& d = detail::dbghelp::get(); if (!d.MiniDumpWriteDump) return {{}, unsupported("dbghelp MiniDumpWriteDump")};
        HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return {{}, last_error("create " + path)};
        BOOL ok = d.MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1 | 0x4 /*WithDataSegs|WithHandleData*/, nullptr, nullptr, nullptr);
        CloseHandle(f); if (!ok) return {{}, last_error("MiniDumpWriteDump")}; return {};
#else
        return write_file(path, "forge on-demand report\npid " + std::to_string(pid()) + "\n" + format_stack_trace(stack_trace(1)));
#endif
    }
private:
#if defined(FORGE_OS_POSIX)
    static constexpr int nsig = 6;
    static const int* sigs() { static const int v[nsig] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}; return v; }
#endif
    struct state {
        config cfg; char dir[1024] = {}; char app[256] = {}; char report[1400] = {}; char dump[1400] = {};
        bool installed = false; volatile bool crashed = false;
        std::atomic<int> phase{0};   // 0 idle, 1 a report is being written, 2 written
        std::terminate_handler prev_terminate = nullptr;
#if defined(FORGE_OS_WINDOWS)
        std::wstring wreport, wdump;
        LPTOP_LEVEL_EXCEPTION_FILTER prev_filter = nullptr;
        void (__cdecl* prev_abrt)(int) = nullptr;
#else
        struct sigaction prev[nsig] = {};
#endif
    };
    static state& st() { static state s; return s; }
    // <dir>/<app>-<pid>[-k]<ext>, pid read now (fork-safe); no allocation.
    static void build_path_(char* out, const char* ext, int k) {
        auto& s = st(); detail::sig_text t{out, sizeof s.report};
#if defined(FORGE_OS_WINDOWS)
        t.s(s.dir).s("\\").s(s.app).s("-").dec(GetCurrentProcessId());
#else
        t.s(s.dir).s("/").s(s.app).s("-").dec(std::uint64_t(::getpid()));
#endif
        if (k) t.s("-").dec(std::uint64_t(k));
        t.s(ext);
    }
    // One report per process: the first crashing thread writes it, later ones wait (up to 2 s) and then chain.
    static bool begin_() {
        int z = 0;
        if (st().phase.compare_exchange_strong(z, 1)) return true;
        for (int i = 0; i < 200 && st().phase.load() == 1; ++i) {
#if defined(FORGE_OS_WINDOWS)
            Sleep(10);
#else
            struct timespec ts{0, 10000000}; ::nanosleep(&ts, nullptr);
#endif
        }
        return false;
    }
    static void end_() { st().phase.store(2); }
#if defined(FORGE_OS_POSIX)
    static result<std::string> private_dir_() {
        std::string d = join(temp_dir(), "forge-crash-" + std::to_string(::geteuid()));
        if (::mkdir(d.c_str(), 0700) != 0 && errno != EEXIST) return {{}, last_error("mkdir " + d)};
        struct stat sb{};
        if (::lstat(d.c_str(), &sb) != 0) return {{}, last_error("lstat " + d)};
        if (!S_ISDIR(sb.st_mode) || sb.st_uid != ::geteuid()) return {{}, {EPERM, "crash_handler: " + d + " is not a directory owned by this user"}};
        if ((sb.st_mode & 077) && ::chmod(d.c_str(), 0700) != 0) return {{}, last_error("chmod " + d)};
        return {d, {}};
    }
    // Async-signal-safe: open/write/close, sig_text, plus the warmed-up unwinder and dladdr.
    static void write_report_(const char* what, const void* addr, int sig) {
        auto& s = st(); if (!s.dir[0]) return;
        char path[sizeof s.report]; int fd = -1;
        for (int k = 0; k < 10 && fd < 0; ++k) {
            build_path_(path, ".crash", k);
            fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0 && errno != EEXIST) return;
        }
        if (fd < 0) return;
        std::memcpy(s.report, path, sizeof path); s.crashed = true;
        char b[1024]; detail::sig_text t{b, sizeof b};
        auto flush = [&] { (void)!::write(fd, b, t.n); t.n = 0; b[0] = 0; };
        t.s("forge crash report\napp ").s(s.app).s("\npid ").dec(std::uint64_t(::getpid())).s("\nreason ").s(what).s("\naddress ").ptr(addr).s("\nsignal ").sdec(sig).s("\nbacktrace:\n"); flush();
        void* raw[64]; int n = detail::raw_backtrace(raw, 64, 1);
        for (int i = 0; i < n; ++i) {
            Dl_info di{}; const char* sym = "??"; const char* mod = "?"; std::uintptr_t off = 0;
            if (::dladdr(raw[i], &di)) { if (di.dli_sname) sym = di.dli_sname; if (di.dli_fname) mod = di.dli_fname; off = reinterpret_cast<std::uintptr_t>(raw[i]) - reinterpret_cast<std::uintptr_t>(di.dli_fbase); }
            t.s("#").dec(std::uint64_t(i)).s(i < 10 ? "  " : " ").ptr(raw[i]).s(" ").s(sym).s(" (").s(mod).s("+").hex(off).s(")\n"); flush();
        }
#if defined(__linux__)
        (void)!::write(fd, "maps:\n", 6); int m = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (m >= 0) { char mb[4096]; ssize_t r; while ((r = ::read(m, mb, sizeof mb)) > 0) (void)!::write(fd, mb, std::size_t(r)); ::close(m); }
#endif
        ::close(fd);
        if (s.cfg.after) s.cfg.after(s.report);
    }
    static void sig_(int sg, siginfo_t* si, void* ctx) {
        const char* w = sg == SIGSEGV ? "SIGSEGV" : sg == SIGBUS ? "SIGBUS" : sg == SIGILL ? "SIGILL" : sg == SIGFPE ? "SIGFPE" : sg == SIGABRT ? "SIGABRT" : "SIGTRAP";
        const int saved = errno;
        if (begin_()) { write_report_(w, si ? si->si_addr : nullptr, sg); end_(); }
        // Hand the signal on: restore the previous disposition (SIG_IGN becomes SIG_DFL: an ignored fault would
        // loop) and call a previous handler, or re-raise for the default action. sg is blocked while we run, so
        // the re-raise is delivered as soon as this handler returns; a hardware fault would recur anyway.
        auto& s = st(); int i = 0; while (i < nsig && sigs()[i] != sg) ++i;
        struct sigaction prev{};
        if (i < nsig) prev = s.prev[i]; else prev.sa_handler = SIG_DFL;
        if (!(prev.sa_flags & SA_SIGINFO) && prev.sa_handler == SIG_IGN) prev.sa_handler = SIG_DFL;
        ::sigaction(sg, &prev, nullptr);
        errno = saved;
        if (prev.sa_flags & SA_SIGINFO) { if (prev.sa_sigaction) prev.sa_sigaction(sg, si, ctx); return; }
        if (prev.sa_handler != SIG_DFL) { prev.sa_handler(sg); return; }
        ::raise(sg);
    }
#else
    static void write_dump_(EXCEPTION_POINTERS* ep) {
        auto& s = st();
        if (!s.cfg.minidump) return;
        auto& d = detail::dbghelp::get(); if (!d.MiniDumpWriteDump) return;
        HANDLE f = CreateFileW(s.wdump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        // MINIDUMP_EXCEPTION_INFORMATION is declared under #pragma pack(4) in dbghelp.h: the pointer sits at
        // offset 4, not 8. A naturally aligned copy makes MiniDumpWriteDump fail and leaves an empty .dmp.
#pragma pack(push, 4)
        struct mei_t { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; };
#pragma pack(pop)
        static_assert(sizeof(void*) != 8 || sizeof(mei_t) == 16, "MINIDUMP_EXCEPTION_INFORMATION layout");
        mei_t mei{GetCurrentThreadId(), ep, FALSE};
        d.MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1 | 0x4, ep ? &mei : nullptr, nullptr, nullptr); CloseHandle(f);
    }
    static void write_report_(const char* what, const void* addr, int sig) {
        auto& s = st(); if (s.wreport.empty()) return;
        HANDLE f = CreateFileW(s.wreport.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        s.crashed = true;
        auto put = [&](const char* t, std::size_t n) { DWORD w = 0; WriteFile(f, t, DWORD(n), &w, nullptr); };
        char b[1024];
        int k = std::snprintf(b, sizeof b, "forge crash report\napp %s\npid %lu\nreason %s\naddress %p\nsignal %d\nbacktrace:\n", s.app, (unsigned long)GetCurrentProcessId(), what, addr, sig);
        put(b, std::size_t(std::max(0, std::min(k, int(sizeof b) - 1))));
        void* raw[64]; int n = detail::raw_backtrace(raw, 64, 1);
        for (int i = 0; i < n; ++i) {
            stack_frame fr; fr.addr = raw[i]; detail::symbolize(fr);
            k = std::snprintf(b, sizeof b, "#%-2d %p %s (%s)\n", i, fr.addr, fr.symbol.c_str(), basename(fr.module).c_str());
            put(b, std::size_t(std::max(0, std::min(k, int(sizeof b) - 1))));
        }
        CloseHandle(f);
        if (s.cfg.after) s.cfg.after(s.report);
    }
    // A crash outside SEH (abort, CRT checks): dump with a captured context, then the report.
    static void report_here_(const char* what, DWORD code) {
        if (!begin_()) return;
        CONTEXT ctx{}; RtlCaptureContext(&ctx);
        EXCEPTION_RECORD er{}; er.ExceptionCode = code; er.ExceptionAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
#if defined(_M_X64) || defined(__x86_64__)
            ctx.Rip
#elif defined(_M_ARM64) || defined(__aarch64__)
            ctx.Pc
#else
            0
#endif
        ));
        EXCEPTION_POINTERS ep{&er, &ctx};
        write_dump_(&ep); write_report_(what, er.ExceptionAddress, -1); end_();
    }
    static LONG WINAPI seh_(EXCEPTION_POINTERS* ep) {
        if (begin_()) {
            write_dump_(ep);
            char what[64]; std::snprintf(what, sizeof what, "exception 0x%08lx", ep && ep->ExceptionRecord ? (unsigned long)ep->ExceptionRecord->ExceptionCode : 0ul);
            write_report_(what, ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr, -1);
            end_();
        }
        // Chain the previous filter; if it only passes the exception on (MinGW's CRT filter does, without a
        // signal handler), end the process here with its exception code instead of waking WER / a JIT debugger.
        if (auto prev = st().prev_filter) if (LONG r = prev(ep); r != EXCEPTION_CONTINUE_SEARCH) return r;
        return EXCEPTION_EXECUTE_HANDLER;
    }
    static void __cdecl abrt_(int sg) {
        report_here_("abort (SIGABRT)", 0x40000015 /*STATUS_FATAL_APP_EXIT*/);
        auto prev = st().prev_abrt;
        if (prev && prev != SIG_DFL && prev != SIG_IGN && prev != SIG_ERR) prev(sg);
        // returning lets abort() finish (exit code 3)
    }
    static void __cdecl invalid_param_(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {
        report_here_("invalid CRT parameter", 0xC0000417);
        TerminateProcess(GetCurrentProcess(), 0xC0000417);
    }
    static void __cdecl purecall_() {
        report_here_("pure virtual function call", 0xC0000025);
        TerminateProcess(GetCurrentProcess(), 3);
    }
#endif
};
} // namespace forge::os
