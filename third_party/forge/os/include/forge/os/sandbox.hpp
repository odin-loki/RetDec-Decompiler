#pragma once
// Process sandboxing (one-way: once applied it cannot be lifted).
//   Linux/Android: no_new_privs + a seccomp-BPF filter returning EPERM for the denied syscalls (network sockets
//   other than AF_UNIX and i386 socketcall, execve/execveat, file writes via open flags / creat / unlink / rename /
//   mkdir / truncate / link / symlink / mknod / chmod / chown / utimes / xattrs; io_uring whenever network or
//   writes are denied). Another architecture's syscalls kill the process, and so do x86_64 x32 calls.
//   Landlock (Linux 5.13+, raw syscalls, ABI 1-6 detected) is stacked on top when the kernel has it: deny_fs_write
//   handles every write right of the ABI (write, remove, make-*, refer from 2, truncate from 3) with no rule
//   allowing it, so the whole tree is read-only; deny_exec handles EXECUTE; deny_network handles TCP bind and
//   connect from ABI 4. Without Landlock (older kernel, LSM disabled, Android's app seccomp policy refusing the
//   calls) the seccomp filter alone applies; sandbox_status() says which mechanisms are active.
//   run_sandboxed() uses Landlock to apply deny_exec to the target itself: EXECUTE is allowed only on the target
//   binary, its #! interpreter and its ELF interpreter (ld.so), and Landlock rules survive execve, so the target
//   starts but cannot start anything else (except by running a program through ld.so explicitly).
//   FreeBSD: Capsicum cap_enter() (capability mode: no global namespaces at all; coarser than requested).
//   run_sandboxed() there opens the binary, the run-time linker and the library directories first and starts
//   the target with fexecve of ld-elf.so.1 -f <fd> (LD_LIBRARY_PATH_FDS), since capability mode allows no
//   lookups by path: the target runs, but only on descriptors it inherited (no files, no network, no exec).
//   OpenBSD (bonus): pledge().  macOS: sandbox_init named profiles (no-network / no-write / pure-computation);
//   App Sandbox is detected from APP_SANDBOX_CONTAINER_ID.  iOS: apps are always sandboxed; apply() reports that.
//   Windows: the current process joins a job object (ActiveProcessLimit=1 blocks child processes; network and
//   filesystem denial cannot be added to a running process, so run_sandboxed() starts children in an
//   AppContainer (no network capability unless allowed; writes only to the container folder) inside a
//   kill-on-close job with UI limits, or, for an exec-only policy, with a restricted token in the job).
//   process_pool starts its workers in the same AppContainer profiles (detail::create_in_appcontainer), and
//   apply_sandbox() inside such a process accepts net/fs denial the token already enforces.
#include <forge/os/detail.hpp>
#include <forge/os/process.hpp>   // find_executable, detail_proc::win_cmdline
#if defined(_WIN32)
#include <userenv.h>   // CreateAppContainerProfile (link userenv)
#include <psapi.h>     // K32EnumProcessModules (kernel32): the DLLs a worker of this image needs
#include <aclapi.h>    // SetEntriesInAcl (advapi32): grant the container read+execute on a worker image
#if defined(_MSC_VER)
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")
#endif
#endif
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <elf.h>
#include <stddef.h>
#include <atomic>
#elif defined(FORGE_OS_FREEBSD)
#include <sys/capsicum.h>
#include <elf.h>
#endif

namespace forge::os {
struct sandbox_policy { bool deny_network = false, deny_exec = false, deny_fs_write = false; };
// mechanism: what confines this process ("seccomp-bpf", "seccomp-bpf+landlock", "capsicum", "appcontainer+job-object", ...).
// landlock_abi: the Landlock ABI the kernel offers (0: none / not usable), whether or not it is applied; landlock:
// this process applied a Landlock ruleset (through apply_sandbox; one inherited across exec is not visible).
struct sandbox_info { bool sandboxed = false; std::string mechanism; int landlock_abi = 0; bool landlock = false; };

#if defined(__linux__)
// Landlock through raw syscalls (numbers shared by every architecture; no libc wrapper needed).
namespace detail::ll {
constexpr long nr_create = 444, nr_add_rule = 445, nr_restrict = 446;
constexpr std::uint32_t create_version = 1u << 0;
constexpr int rule_path_beneath = 1;
enum : std::uint64_t {
    fs_execute = 1ull << 0, fs_write_file = 1ull << 1, fs_read_file = 1ull << 2, fs_read_dir = 1ull << 3,
    fs_remove_dir = 1ull << 4, fs_remove_file = 1ull << 5, fs_make_char = 1ull << 6, fs_make_dir = 1ull << 7,
    fs_make_reg = 1ull << 8, fs_make_sock = 1ull << 9, fs_make_fifo = 1ull << 10, fs_make_block = 1ull << 11,
    fs_make_sym = 1ull << 12, fs_refer = 1ull << 13 /*ABI 2*/, fs_truncate = 1ull << 14 /*ABI 3*/, fs_ioctl_dev = 1ull << 15 /*ABI 5*/,
    net_bind_tcp = 1ull << 0, net_connect_tcp = 1ull << 1 /*ABI 4*/,
};
struct ruleset_attr { std::uint64_t handled_fs = 0, handled_net = 0, scoped = 0; };   // scoped: ABI 6
struct __attribute__((packed)) path_beneath_attr { std::uint64_t allowed = 0; std::int32_t parent_fd = -1; };
inline std::atomic<bool>& applied() { static std::atomic<bool> a{false}; return a; }
inline int probe() noexcept {
    long v = ::syscall(nr_create, nullptr, 0, create_version);
    return v > 0 ? int(v) : 0;
}
// The kernel's Landlock ABI (0 when unavailable). Android: an app's seccomp policy may kill a process that makes
// an unlisted syscall, so the first probe there runs in a throwaway child. FORGE_LANDLOCK=0 turns it off (seccomp
// only), e.g. to test the fallback.
inline int abi() {
    static const int v = [] {
        if (const char* e = std::getenv("FORGE_LANDLOCK"); e && e[0] == '0') return 0;
#if defined(__ANDROID__)
        pid_t c = forge::os::serialized_fork();
        if (c < 0) return 0;
        if (c == 0) ::_exit(probe() & 0x7f);
        int st = 0; while (::waitpid(c, &st, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(st) || WEXITSTATUS(st) == 0) return 0;
#endif
        return probe();
    }();
    return v;
}
// Every right that changes the file system, for this ABI.
inline std::uint64_t write_rights(int abi) {
    std::uint64_t r = fs_write_file | fs_remove_dir | fs_remove_file | fs_make_char | fs_make_dir | fs_make_reg | fs_make_sock |
                      fs_make_fifo | fs_make_block | fs_make_sym;
    if (abi >= 2) r |= fs_refer;
    if (abi >= 3) r |= fs_truncate;
    return r;
}
// Build a ruleset for `p` (returns -1 when Landlock is unavailable or the policy needs nothing from it). EXECUTE is
// allowed only on `exec_ok` (files) when exec is denied. Errors adding a rule are returned in *err.
inline int ruleset(const sandbox_policy& p, const std::vector<std::string>& exec_ok, error* err) {
    const int a = abi(); if (!a) return -1;
    ruleset_attr at;
    if (p.deny_fs_write) at.handled_fs |= write_rights(a);
    if (p.deny_exec) at.handled_fs |= fs_execute;
    if (p.deny_network && a >= 4) at.handled_net = net_bind_tcp | net_connect_tcp;
    if (!at.handled_fs && !at.handled_net) return -1;
    const std::size_t sz = a >= 6 ? sizeof at : a >= 4 ? 16 : 8;
    int fd = int(::syscall(nr_create, &at, sz, 0u));
    if (fd < 0) { if (err) *err = last_error("landlock_create_ruleset"); return -1; }
    if (p.deny_exec)
        for (auto& path : exec_ok) {
            path_beneath_attr pb; pb.allowed = fs_execute;
            pb.parent_fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
            if (pb.parent_fd < 0 || ::syscall(nr_add_rule, fd, rule_path_beneath, &pb, 0u) != 0) {
                if (err) *err = last_error("landlock_add_rule " + path);
                if (pb.parent_fd >= 0) ::close(pb.parent_fd);
                ::close(fd); return -1;
            }
            ::close(pb.parent_fd);
        }
    return fd;
}
// Enforce a ruleset on the calling thread (and what it execs). Async-signal-safe: used in a forked child.
inline bool restrict_self(int fd) noexcept {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return false;
    return ::syscall(nr_restrict, fd, 0u) == 0;
}
// What a target needs EXECUTE on to start: the file itself, a #! interpreter (recursively), and the ELF program
// interpreter (PT_INTERP, the dynamic linker), which the kernel opens for execution too.
inline void exec_chain(const std::string& path, std::vector<std::string>& out, int depth = 0) {
    if (depth > 4 || path.empty()) return;
    out.push_back(path);
    int f = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); if (f < 0) return;
    char head[256] = {}; ssize_t n = ::pread(f, head, sizeof head - 1, 0);
    if (n > 2 && head[0] == '#' && head[1] == '!') {
        std::string s(head + 2, std::size_t(n - 2)); s = s.substr(0, s.find('\n'));
        auto b = s.find_first_not_of(" \t"); if (b != std::string::npos) s = s.substr(b);
        s = s.substr(0, s.find_first_of(" \t"));
        ::close(f); exec_chain(s, out, depth + 1); return;
    }
    if (n >= EI_NIDENT && std::memcmp(head, ELFMAG, SELFMAG) == 0) {
        const bool e64 = head[EI_CLASS] == ELFCLASS64;
        std::uint64_t phoff = 0; unsigned phnum = 0, phent = 0;
        if (e64 && n >= ssize_t(sizeof(Elf64_Ehdr))) { Elf64_Ehdr eh; std::memcpy(&eh, head, sizeof eh); phoff = eh.e_phoff; phnum = eh.e_phnum; phent = eh.e_phentsize; }
        else if (!e64 && n >= ssize_t(sizeof(Elf32_Ehdr))) { Elf32_Ehdr eh; std::memcpy(&eh, head, sizeof eh); phoff = eh.e_phoff; phnum = eh.e_phnum; phent = eh.e_phentsize; }
        for (unsigned i = 0; i < phnum && i < 256; ++i) {
            std::uint32_t type = 0; std::uint64_t off = 0, size = 0;
            if (e64) { Elf64_Phdr ph; if (::pread(f, &ph, sizeof ph, off_t(phoff + std::uint64_t(i) * phent)) != ssize_t(sizeof ph)) break; type = ph.p_type; off = ph.p_offset; size = ph.p_filesz; }
            else { Elf32_Phdr ph; if (::pread(f, &ph, sizeof ph, off_t(phoff + std::uint64_t(i) * phent)) != ssize_t(sizeof ph)) break; type = ph.p_type; off = ph.p_offset; size = ph.p_filesz; }
            if (type != PT_INTERP || size < 2 || size > 4096) continue;
            std::string ip(std::size_t(size), '\0');
            if (::pread(f, ip.data(), ip.size(), off_t(off)) == ssize_t(ip.size())) { ip.resize(::strnlen(ip.c_str(), ip.size())); ::close(f); exec_chain(ip, out, depth + 1); return; }
            break;
        }
    }
    ::close(f);
}
}
#elif defined(FORGE_OS_WINDOWS)
namespace detail::wsbx {
// True when this process runs in an AppContainer; *net: it holds a network capability (internetClient,
// internetClientServer or privateNetworkClientServer).
inline bool in_appcontainer(bool* net) {
    if (net) *net = false;
    HANDLE t = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    DWORD ac = 0, len = 0; bool in = GetTokenInformation(t, TokenIsAppContainer, &ac, sizeof ac, &len) && ac;
    if (in && net) {
        GetTokenInformation(t, TokenCapabilities, nullptr, 0, &len);
        std::vector<unsigned char> b(len ? len : 1);
        if (len && GetTokenInformation(t, TokenCapabilities, b.data(), len, &len)) {
            auto* g = reinterpret_cast<TOKEN_GROUPS*>(b.data());
            for (DWORD i = 0; i < g->GroupCount; ++i) {
                PSID s = g->Groups[i].Sid; auto* ia = GetSidIdentifierAuthority(s);
                const SID_IDENTIFIER_AUTHORITY app = SECURITY_APP_PACKAGE_AUTHORITY;
                if (std::memcmp(ia, &app, sizeof app) != 0 || *GetSidSubAuthorityCount(s) != SECURITY_BUILTIN_CAPABILITY_RID_COUNT) continue;
                DWORD rid = *GetSidSubAuthority(s, 1);
                if (*GetSidSubAuthority(s, 0) == SECURITY_CAPABILITY_BASE_RID &&
                    (rid == SECURITY_CAPABILITY_INTERNET_CLIENT || rid == SECURITY_CAPABILITY_INTERNET_CLIENT_SERVER || rid == SECURITY_CAPABILITY_PRIVATE_NETWORK_CLIENT_SERVER)) *net = true;
            }
        }
    }
    CloseHandle(t); return in;
}
// The AppContainer profile for a net/fs policy (created on first use, then reused; it persists per user like any
// package profile). *sid is freed with FreeSid.
inline const wchar_t* profile_name(const sandbox_policy& p) {
    return p.deny_network && p.deny_fs_write ? L"forge.sandbox.nonet.nofs" : p.deny_network ? L"forge.sandbox.nonet" : L"forge.sandbox.nofs";
}
inline error profile_sid(const sandbox_policy& p, PSID* sid) {
    const wchar_t* name = profile_name(p); *sid = nullptr;
    HRESULT hr = CreateAppContainerProfile(name, name, L"FORGE sandboxed child", nullptr, 0, sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) hr = DeriveAppContainerSidFromAppContainerName(name, sid);
    if (FAILED(hr) || !*sid) return {FAILED(hr) ? int(hr) : int(E_FAIL), "AppContainer profile failed"};
    return {};
}
// Give the container read + execute on a file outside the places it may read by default (the user's own
// directories are not readable by AppContainers; system directories are). An existing grant is left alone.
inline error grant_read_execute(const std::wstring& path, PSID sid) {
    PACL dacl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (DWORD e = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd))
        return {int(e), "GetNamedSecurityInfo " + narrow(path)};
    const DWORD want = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    bool have = false;
    for (DWORD i = 0; dacl && i < dacl->AceCount && !have; ++i) {
        void* a = nullptr; if (!GetAce(dacl, i, &a)) continue;
        auto* h = static_cast<ACE_HEADER*>(a);
        if (h->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto* aa = static_cast<ACCESS_ALLOWED_ACE*>(a);
        if (EqualSid(reinterpret_cast<PSID>(&aa->SidStart), sid) && (aa->Mask & want) == want) have = true;
    }
    error r{};
    if (!have) {
        EXPLICIT_ACCESS_W ea{}; ea.grfAccessPermissions = want; ea.grfAccessMode = GRANT_ACCESS; ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID; ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP; ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL nd = nullptr;
        if (DWORD e = SetEntriesInAclW(1, &ea, dacl, &nd)) r = {int(e), "SetEntriesInAcl " + narrow(path)};
        else if (DWORD e2 = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nd, nullptr))
            r = {int(e2), "SetNamedSecurityInfo " + narrow(path) + " (grant the AppContainer read+execute on the worker image)"};
        if (nd) LocalFree(nd);
    }
    LocalFree(sd); return r;
}
// CreateProcessW inside the policy's AppContainer (no network capability when deny_network, else internetClient).
// With STARTF_USESTDHANDLES the child inherits exactly si's std handles (inheritable duplicates through
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST, like create_process_std_only), otherwise no handles. `image`, when given, is
// granted read + execute for the container first.
inline error create_in_appcontainer(const sandbox_policy& p, const wchar_t* app, wchar_t* cmd, DWORD flags, const STARTUPINFOW& si,
                                    PROCESS_INFORMATION& pi, const std::wstring& image = {}) {
    PSID ac = nullptr;
    if (auto e = profile_sid(p, &ac)) return e;
    if (!image.empty()) {
        if (auto e = grant_read_execute(image, ac)) { FreeSid(ac); return e; }
        // The worker is usually this program: it also needs the non-system DLLs we loaded (a MinGW or MSVC
        // runtime next to the exe or on PATH), or it dies with STATUS_DLL_NOT_FOUND. Best effort per DLL.
        wchar_t sys[MAX_PATH] = {}, win[MAX_PATH] = {};
        UINT ns = GetSystemDirectoryW(sys, MAX_PATH), nw = GetWindowsDirectoryW(win, MAX_PATH);
        auto under = [](const std::wstring& f, const wchar_t* d, UINT n) { return n && f.size() > n && _wcsnicmp(f.c_str(), d, n) == 0 && (f[n] == L'\\' || f[n] == L'/'); };
        HMODULE mods[512]; DWORD need = 0;
        if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
            for (DWORD i = 1; i < std::min<DWORD>(need / sizeof(HMODULE), 512); ++i) {   // [0] is the exe itself
                wchar_t f[MAX_PATH * 2]; DWORD n = GetModuleFileNameW(mods[i], f, DWORD(sizeof f / sizeof f[0]));
                if (!n || n >= sizeof f / sizeof f[0]) continue;
                std::wstring w(f, n); if (under(w, sys, ns) || under(w, win, nw)) continue;
                (void)grant_read_execute(w, ac);
            }
    }
    SID_AND_ATTRIBUTES caps[1]{}; DWORD ncaps = 0; PSID net = nullptr;
    if (!p.deny_network) {   // fs denied only: keep outbound network
        SID_IDENTIFIER_AUTHORITY auth = SECURITY_APP_PACKAGE_AUTHORITY;
        if (AllocateAndInitializeSid(&auth, SECURITY_BUILTIN_CAPABILITY_RID_COUNT, SECURITY_CAPABILITY_BASE_RID, SECURITY_CAPABILITY_INTERNET_CLIENT, 0, 0, 0, 0, 0, 0, &net)) { caps[0].Sid = net; caps[0].Attributes = SE_GROUP_ENABLED; ncaps = 1; }
    }
    SECURITY_CAPABILITIES sc{}; sc.AppContainerSid = ac; sc.Capabilities = ncaps ? caps : nullptr; sc.CapabilityCount = ncaps;
    HANDLE self = GetCurrentProcess();
    STARTUPINFOEXW sx{}; sx.StartupInfo = si; sx.StartupInfo.cb = sizeof sx;
    HANDLE* std3[3] = {&sx.StartupInfo.hStdInput, &sx.StartupInfo.hStdOutput, &sx.StartupInfo.hStdError};
    HANDLE orig[3] = {}, dups[3] = {}; DWORD n = 0;
    if (si.dwFlags & STARTF_USESTDHANDLES)
        for (HANDLE* h : std3) {
            if (!*h || *h == INVALID_HANDLE_VALUE) continue;
            DWORD k = 0; while (k < n && orig[k] != *h) ++k;
            if (k == n) { if (!DuplicateHandle(self, *h, self, &dups[n], 0, TRUE, DUPLICATE_SAME_ACCESS)) { *h = nullptr; continue; } orig[n++] = *h; }
            *h = dups[k];
        }
    const DWORD nattr = n ? 2 : 1;
    SIZE_T sz = 0; InitializeProcThreadAttributeList(nullptr, nattr, 0, &sz);
    std::vector<unsigned char> buf(sz ? sz : 1); auto* al = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    bool init = InitializeProcThreadAttributeList(al, nattr, 0, &sz);
    bool ok = init && UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sc, sizeof sc, nullptr, nullptr) &&
              (!n || UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, dups, n * sizeof(HANDLE), nullptr, nullptr));
    sx.lpAttributeList = al;
    if (ok) ok = CreateProcessW(app, cmd, nullptr, nullptr, n ? TRUE : FALSE, flags | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &sx.StartupInfo, &pi);
    error e{}; if (!ok) e = last_error("CreateProcess (AppContainer)");
    if (init) DeleteProcThreadAttributeList(al);
    for (DWORD i = 0; i < n; ++i) CloseHandle(dups[i]);
    if (net) FreeSid(net);
    FreeSid(ac);
    return e;
}
}
#endif

inline sandbox_info sandbox_status() {
    sandbox_info i;
#if defined(__linux__)
    auto s = detail::slurp("/proc/self/status"); auto p = s.find("Seccomp:");
    if (p != std::string::npos && std::atoi(s.c_str() + p + 8) > 0) { i.sandboxed = true; i.mechanism = std::atoi(s.c_str() + p + 8) == 2 ? "seccomp-bpf" : "seccomp-strict"; }
    i.landlock_abi = detail::ll::abi(); i.landlock = detail::ll::applied().load();
    if (i.landlock) { i.sandboxed = true; i.mechanism += i.mechanism.empty() ? "landlock" : "+landlock"; }
#elif defined(FORGE_OS_FREEBSD)
    unsigned m = 0; if (::cap_getmode(&m) == 0 && m) { i.sandboxed = true; i.mechanism = "capsicum"; }
#elif defined(FORGE_OS_IOS)
    i.sandboxed = true; i.mechanism = "ios-app-sandbox";
#elif defined(FORGE_OS_MACOS)
    if (!getenv("APP_SANDBOX_CONTAINER_ID").empty()) { i.sandboxed = true; i.mechanism = "app-sandbox"; }
#elif defined(FORGE_OS_WINDOWS)
    if (detail::wsbx::in_appcontainer(nullptr)) { i.sandboxed = true; i.mechanism = "appcontainer"; }
    BOOL in = FALSE; if (IsProcessInJob(GetCurrentProcess(), nullptr, &in) && in) { i.sandboxed = true; i.mechanism += i.mechanism.empty() ? "job-object" : "+job-object"; }
#endif
    return i;
}

#if defined(__linux__)
namespace detail {
#if defined(__x86_64__)
constexpr std::uint32_t audit_arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t audit_arch = AUDIT_ARCH_AARCH64;
#elif defined(__i386__)
constexpr std::uint32_t audit_arch = AUDIT_ARCH_I386;
#elif defined(__arm__)
constexpr std::uint32_t audit_arch = AUDIT_ARCH_ARM;
#elif defined(__riscv) && __riscv_xlen == 64
constexpr std::uint32_t audit_arch = AUDIT_ARCH_RISCV64;
#else
constexpr std::uint32_t audit_arch = 0;
#endif
// The BPF program for `p` (built in the caller, so a forked child only has to install it).
inline std::vector<sock_filter> seccomp_filter(const sandbox_policy& p) {
    std::vector<sock_filter> f;
    auto st = [&](std::uint16_t code, std::uint32_t k, std::uint8_t jt = 0, std::uint8_t jf = 0) { f.push_back(sock_filter{code, jt, jf, k}); };
    const std::uint32_t deny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA), allow = SECCOMP_RET_ALLOW;
    st(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
    st(BPF_JMP | BPF_JEQ | BPF_K, audit_arch, 1, 0); st(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    auto load_nr = [&] { st(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)); };
    auto deny_nr = [&](long nr) { load_nr(); st(BPF_JMP | BPF_JEQ | BPF_K, std::uint32_t(nr), 0, 1); st(BPF_RET | BPF_K, deny); };
#if defined(__x86_64__)
    // x32 ABI calls share AUDIT_ARCH_X86_64 but carry bit 30 in nr (0x40000000 + the x32 number): every rule
    // below would miss them, so any such call kills the process.
    load_nr(); st(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1); st(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#endif
    if (p.deny_network || p.deny_fs_write) {
        // io_uring performs socket/connect/openat/unlinkat/... inside the kernel without those syscalls, so the
        // rules below never see them. Numbers 425-427 are shared by every architecture (since Linux 5.1). A ring
        // set up before the sandbox stops working too (io_uring_enter is denied): os::async_file reports EPERM.
        for (long nr : {425L /*io_uring_setup*/, 426L /*io_uring_enter*/, 427L /*io_uring_register*/}) deny_nr(nr);
    }
    // deny when (nr == NR) && (args[argi] & mask)
    auto deny_flags = [&](long nr, int argi, std::uint32_t mask) {
        load_nr(); st(BPF_JMP | BPF_JEQ | BPF_K, std::uint32_t(nr), 0, 3);
        st(BPF_LD | BPF_W | BPF_ABS, std::uint32_t(offsetof(seccomp_data, args) + std::size_t(argi) * 8));
        st(BPF_JMP | BPF_JSET | BPF_K, mask, 0, 1); st(BPF_RET | BPF_K, deny);
    };
    if (p.deny_network) {   // socket(domain != AF_UNIX)
        load_nr(); st(BPF_JMP | BPF_JEQ | BPF_K, std::uint32_t(__NR_socket), 0, 3);
        st(BPF_LD | BPF_W | BPF_ABS, std::uint32_t(offsetof(seccomp_data, args)));
        st(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0); st(BPF_RET | BPF_K, deny);
#ifdef __NR_socketcall
        deny_nr(__NR_socketcall);   // i386 multiplexer: its arguments sit in user memory the filter cannot read
#endif
    }
    if (p.deny_exec) {
        deny_nr(__NR_execve);
#ifdef __NR_execveat
        deny_nr(__NR_execveat);
#endif
    }
    if (p.deny_fs_write) {
        const std::uint32_t wr = O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
#ifdef __NR_open
        deny_flags(__NR_open, 1, wr);
#endif
#ifdef __NR_creat
        deny_nr(__NR_creat);
#endif
        deny_flags(__NR_openat, 2, wr);
#ifdef __NR_openat2
        deny_nr(__NR_openat2);   // flags live in a struct the filter cannot read
#endif
        for (long nr : {long(__NR_unlinkat), long(__NR_mkdirat), long(__NR_truncate), long(__NR_ftruncate)}) deny_nr(nr);
#ifdef __NR_renameat   // absent on riscv64 (renameat2 only)
        deny_nr(__NR_renameat);
#endif
#ifdef __NR_unlink
        deny_nr(__NR_unlink); deny_nr(__NR_rename); deny_nr(__NR_mkdir); deny_nr(__NR_rmdir);
#endif
#ifdef __NR_renameat2
        deny_nr(__NR_renameat2);
#endif
        // links, device nodes, permission / owner / time / extended-attribute changes
        for (long nr : {long(__NR_linkat), long(__NR_symlinkat), long(__NR_mknodat), long(__NR_fchmodat), long(__NR_fchmod), long(__NR_fchownat),
                        long(__NR_fchown), long(__NR_utimensat), long(__NR_setxattr), long(__NR_lsetxattr), long(__NR_fsetxattr),
                        long(__NR_removexattr), long(__NR_lremovexattr), long(__NR_fremovexattr)}) deny_nr(nr);
        deny_nr(452);   // fchmodat2 (Linux 6.6, same number everywhere; older headers lack __NR_fchmodat2)
#ifdef __NR_link   // legacy non-at calls (x86, arm; absent on aarch64 / riscv64)
        deny_nr(__NR_link); deny_nr(__NR_symlink); deny_nr(__NR_mknod); deny_nr(__NR_chmod); deny_nr(__NR_chown); deny_nr(__NR_lchown);
#endif
#ifdef __NR_utime
        deny_nr(__NR_utime);
#endif
#ifdef __NR_utimes
        deny_nr(__NR_utimes);
#endif
#ifdef __NR_futimesat
        deny_nr(__NR_futimesat);
#endif
#ifdef __NR_chown32   // i386 / arm 32-bit uid variants
        deny_nr(__NR_chown32); deny_nr(__NR_lchown32); deny_nr(__NR_fchown32);
#endif
#ifdef __NR_utimensat_time64
        deny_nr(__NR_utimensat_time64);
#endif
#ifdef __NR_truncate64
        deny_nr(__NR_truncate64); deny_nr(__NR_ftruncate64);
#endif
    }
    st(BPF_RET | BPF_K, allow);
    return f;
}
// Install a filter: two prctl calls, async-signal-safe (run_sandboxed builds the filter before fork).
inline bool install_seccomp(const std::vector<sock_filter>& f) noexcept {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return false;
    sock_fprog prog{static_cast<unsigned short>(f.size()), const_cast<sock_filter*>(f.data())};
    return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) == 0;
}
inline status apply_seccomp(const sandbox_policy& p) {
    if (!install_seccomp(seccomp_filter(p))) return {{}, last_error("seccomp (PR_SET_NO_NEW_PRIVS / PR_SET_SECCOMP)")};
    return {};
}
}
#endif

inline status apply_sandbox(const sandbox_policy& p) {
    if (!p.deny_network && !p.deny_exec && !p.deny_fs_write) return {};
#if defined(__linux__)
    if (!detail::audit_arch) return {{}, unsupported("seccomp on this architecture")};
    // Landlock first (best effort: the seccomp filter below is the baseline everywhere), then seccomp.
    if (int ll = detail::ll::ruleset(p, {}, nullptr); ll >= 0) {
        if (detail::ll::restrict_self(ll)) detail::ll::applied() = true;
        ::close(ll);
    }
    return detail::apply_seccomp(p);
#elif defined(FORGE_OS_FREEBSD)
    if (::cap_enter()) return {{}, last_error("cap_enter")}; return {};
#elif defined(__OpenBSD__)
    std::string pr = "stdio rpath flock"; if (!p.deny_fs_write) pr += " wpath cpath"; if (!p.deny_network) pr += " inet unix dns"; if (!p.deny_exec) pr += " proc exec";
    if (::pledge(pr.c_str(), nullptr)) return {{}, last_error("pledge")}; return {};
#elif defined(FORGE_OS_MACOS)
    auto init = reinterpret_cast<int (*)(const char*, std::uint64_t, char**)>(::dlsym(RTLD_DEFAULT, "sandbox_init"));
    if (!init) return {{}, unsupported("sandbox_init")};
    const char* prof = (p.deny_network && p.deny_fs_write) || p.deny_exec ? "pure-computation" : p.deny_network ? "no-internet" : "no-write";
    char* err = nullptr; if (init(prof, 1 /*SANDBOX_NAMED*/, &err)) { std::string e = err ? err : "sandbox_init failed"; return {{}, {1, e}}; }
    return {};
#elif defined(FORGE_OS_IOS)
    return {};   // already confined by the iOS app sandbox (no exec, container-only fs); nothing further to apply
#elif defined(FORGE_OS_WINDOWS)
    if (p.deny_network || p.deny_fs_write) {
        // Cannot be added to a running process; accepted only when this process already runs in an
        // AppContainer that enforces it (process_pool workers, run_sandboxed children): no network capability
        // when the network is denied.
        bool net = false;
        if (!detail::wsbx::in_appcontainer(&net) || (p.deny_network && net))
            return {{}, unsupported("in-process network/fs denial on Windows (use run_sandboxed or an AppContainer process_pool)")};
        if (!p.deny_exec) return {};
    }
    HANDLE j = CreateJobObjectW(nullptr, nullptr); if (!j) return {{}, last_error("CreateJobObject")};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS; li.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li) || !AssignProcessToJobObject(j, GetCurrentProcess())) { auto e = last_error("job object"); CloseHandle(j); return {{}, e}; }
    return {};   // the job handle is intentionally leaked: it must live as long as the process
#else
    return {{}, unsupported("sandbox")};
#endif
}

// Run a child under the policy; returns its exit code (POSIX: 126 = the policy could not be applied, 127 = the
// target could not be started, 128+n = killed by signal n). argv[0] is resolved like spawn_wait (PATH).
// deny_exec: Windows (ActiveProcessLimit = 1), FreeBSD (capability mode) and Linux with Landlock (EXECUTE only on
// the target's binary, #! interpreter and ld.so) keep the target itself from starting programs. Linux without
// Landlock and macOS cannot deny exec yet allow the one that starts the target, so there the target runs under the
// policy's other parts only (sandbox_status().landlock_abi says whether Landlock is there).
inline result<int> run_sandboxed(const std::vector<std::string>& argv, const sandbox_policy& p) {
    if (argv.empty()) return {-1, {22, "run_sandboxed: empty argv"}};
#if defined(FORGE_OS_WINDOWS)
    std::string exe = find_executable(argv[0]);
    if (exe.empty()) return {-1, {ERROR_FILE_NOT_FOUND, "run_sandboxed " + argv[0] + ": not found"}};
    error cmd_err{};
    std::wstring cmd = detail_proc::win_cmdline(argv, exe, &cmd_err), wexe = widen(exe);   // same quoting as spawn_wait
    if (cmd.empty()) return {-1, cmd_err};
    // Job object for every child: kill-on-close, no breakaway, UI limits; ActiveProcessLimit=1 when exec is denied.
    HANDLE j = CreateJobObjectW(nullptr, nullptr); if (!j) return {-1, last_error("CreateJobObject")};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | (p.deny_exec ? JOB_OBJECT_LIMIT_ACTIVE_PROCESS : 0);
    li.BasicLimitInformation.ActiveProcessLimit = 1;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{}; ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;
    if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li) || !SetInformationJobObject(j, JobObjectBasicUIRestrictions, &ui, sizeof ui)) {
        auto e = last_error("SetInformationJobObject"); CloseHandle(j); return {-1, e};
    }
    PROCESS_INFORMATION pi{};
    if (p.deny_fs_write || p.deny_network) {
        // AppContainer: the child gets a per-policy container SID. With no capabilities it has no network
        // (internetClient / privateNetworkClientServer are not granted) and can write only to its own
        // container folder (%LOCALAPPDATA%\Packages\<name>\AC); reads of user files are denied unless their ACL
        // grants ALL APPLICATION PACKAGES (system directories do, so programs still load).
        STARTUPINFOW si{}; si.cb = sizeof si;
        if (auto e = detail::wsbx::create_in_appcontainer(p, wexe.c_str(), cmd.data(), CREATE_SUSPENDED, si, pi)) { CloseHandle(j); return {-1, e}; }
    } else {
        // exec-only policy: restricted token (max privileges dropped) inside the job.
        HANDLE tok = nullptr, rt = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tok)) { CloseHandle(j); return {-1, last_error("OpenProcessToken")}; }
        BOOL ok = CreateRestrictedToken(tok, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &rt); CloseHandle(tok);
        if (!ok) { CloseHandle(j); return {-1, last_error("CreateRestrictedToken")}; }
        STARTUPINFOW si{}; si.cb = sizeof si;
        if (!CreateProcessAsUserW(rt, wexe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            auto e = last_error("CreateProcessAsUser"); CloseHandle(rt); CloseHandle(j); return {-1, e};
        }
        CloseHandle(rt);
    }
    if (!AssignProcessToJobObject(j, pi.hProcess)) { auto e = last_error("AssignProcessToJobObject"); TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(j); return {-1, e}; }
    ResumeThread(pi.hThread); WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(j); return {int(code), {}};
#elif defined(FORGE_OS_IOS)
    (void)p; return {-1, unsupported("child processes on iOS")};
#else
    // Everything is prepared before fork; the child makes only async-signal-safe calls (apart from macOS
    // sandbox_init, which has no such variant).
    const std::string exe = find_executable(argv[0]);
    if (exe.empty()) return {-1, {ENOENT, "run_sandboxed " + argv[0] + ": command not found"}};
    std::vector<char*> a; for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str())); a.push_back(nullptr);
    const bool confine = p.deny_network || p.deny_exec || p.deny_fs_write;
#if defined(__linux__)
    // deny_exec is left out of the seccomp filter (it would refuse the execve that starts the target); Landlock
    // carries it instead, with EXECUTE allowed on the target's own exec chain only.
    sandbox_policy q = p; q.deny_exec = false;
    if (confine && !detail::audit_arch) return {-1, unsupported("seccomp on this architecture")};
    const std::vector<sock_filter> filter = q.deny_network || q.deny_fs_write ? detail::seccomp_filter(q) : std::vector<sock_filter>{};
    std::vector<std::string> chain; if (p.deny_exec) detail::ll::exec_chain(exe, chain);
    error ll_err{};
    const int ll = confine ? detail::ll::ruleset(p, chain, &ll_err) : -1;
    if (ll_err) return {-1, ll_err};
#elif defined(FORGE_OS_FREEBSD)
    // Capability mode refuses every lookup by path, execve included, so the child opens nothing itself: the
    // binary (and for a dynamic one the run-time linker named by PT_INTERP plus the default library
    // directories) is opened here, the child calls cap_enter() and fexecve()s - a static binary directly, a
    // dynamic one as `ld-elf.so.1 -f <fd> -- argv...` with LD_LIBRARY_PATH_FDS (the kernel would otherwise
    // have to open the interpreter by path, and the exec fails past the point of no return). Scripts (#!)
    // cannot start this way; libraries outside /lib, /usr/lib, /usr/lib/compat, /usr/local/lib are not found.
    int bin = -1, interp = -1; std::vector<int> dirs; std::vector<std::string> xargs, xenv; std::vector<char*> xa, xe;
    auto close_all = [&] { for (int f : dirs) ::close(f); if (bin >= 0) ::close(bin); if (interp >= 0) ::close(interp); };
    if (confine) {
        if ((bin = ::open(exe.c_str(), O_RDONLY)) < 0) return {-1, last_error("open " + exe)};
        Elf_Ehdr eh{}; std::string interp_path;
        if (::pread(bin, &eh, sizeof eh, 0) != ssize_t(sizeof eh) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
            close_all(); return {-1, {ENOEXEC, "run_sandboxed " + exe + ": not an ELF binary (a script cannot start in capability mode)"}};
        }
        for (unsigned i = 0; i < eh.e_phnum; ++i) {
            Elf_Phdr ph{};
            if (::pread(bin, &ph, sizeof ph, off_t(eh.e_phoff + std::uint64_t(i) * eh.e_phentsize)) != ssize_t(sizeof ph)) break;
            if (ph.p_type != PT_INTERP || ph.p_filesz < 2 || ph.p_filesz > 4096) continue;
            interp_path.resize(std::size_t(ph.p_filesz));
            if (::pread(bin, interp_path.data(), interp_path.size(), off_t(ph.p_offset)) != ssize_t(interp_path.size())) interp_path.clear();
            interp_path.resize(::strnlen(interp_path.c_str(), interp_path.size()));
            break;
        }
        if (!interp_path.empty()) {
            if ((interp = ::open(interp_path.c_str(), O_RDONLY | O_CLOEXEC)) < 0) { auto e = last_error("open " + interp_path); close_all(); return {-1, e}; }
            std::string fds;
            for (const char* d : {"/lib", "/usr/lib", "/usr/lib/compat", "/usr/local/lib"})
                if (int f = ::open(d, O_RDONLY | O_DIRECTORY); f >= 0) { dirs.push_back(f); if (!fds.empty()) fds += ':'; fds += std::to_string(f); }
            xargs = {"ld-elf.so.1", "-f", std::to_string(bin), "--"}; xargs.insert(xargs.end(), argv.begin(), argv.end());
            for (char** e = environ; e && *e; ++e) if (std::strncmp(*e, "LD_LIBRARY_PATH_FDS=", 20) != 0) xenv.emplace_back(*e);
            xenv.push_back("LD_LIBRARY_PATH_FDS=" + fds);
            for (auto& s : xargs) xa.push_back(s.data()); xa.push_back(nullptr);
            for (auto& s : xenv) xe.push_back(s.data()); xe.push_back(nullptr);
        } else {
            ::fcntl(bin, F_SETFD, FD_CLOEXEC);   // static: the kernel needs no path, the target no descriptor
        }
    }
#elif defined(FORGE_OS_MACOS)
    sandbox_policy q = p; q.deny_exec = false;   // see above: the target must still be exec'd
    (void)confine;
#else
    (void)confine;
#endif
    pid_t c = forge::os::serialized_fork();
    if (c < 0) {
        auto e = last_error("fork");
#if defined(FORGE_OS_FREEBSD)
        close_all();
#elif defined(__linux__)
        if (ll >= 0) ::close(ll);
#endif
        return {-1, e};
    }
    if (c == 0) {
#if defined(__linux__)
        if (ll >= 0 && !detail::ll::restrict_self(ll)) ::_exit(126);
        if (!filter.empty() && !detail::install_seccomp(filter)) ::_exit(126);
#elif defined(FORGE_OS_FREEBSD)
        if (confine) {
            if (::cap_enter()) ::_exit(126);
            if (interp >= 0) ::fexecve(interp, xa.data(), xe.data()); else ::fexecve(bin, a.data(), environ);
            ::_exit(127);
        }
#elif defined(FORGE_OS_MACOS)
        if (!apply_sandbox(q)) ::_exit(126);
#else
        if (!apply_sandbox(p)) ::_exit(126);
#endif
        ::execv(exe.c_str(), a.data()); ::_exit(127);
    }
#if defined(FORGE_OS_FREEBSD)
    close_all();
#elif defined(__linux__)
    if (ll >= 0) ::close(ll);
#endif
    int st = 0; while (::waitpid(c, &st, 0) < 0 && errno == EINTR) {}
    return {WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), {}};
#endif
}
} // namespace forge::os
