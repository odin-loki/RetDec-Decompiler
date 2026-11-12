#pragma once
// FSS-SBX / Verification: lockdown(policy) and check(policy).
//
//   auto rep = security::check(p);      // what this process could apply, per control; nothing changes
//   std::puts(rep.json().c_str());
//   auto applied = security::lockdown(p);   // apply, one-way; throws sandbox_error when a required control cannot be met
//
// Each control is reported with
//   state    active (applied in this process) | available (can be applied) | emulated (FORGE's own in-process
//            checks, enforce(), stand in for a missing kernel mechanism) | missing | n/a (the policy does not ask
//            for it) | failed (applying it failed)
//   backend  the mechanism (seccomp-bpf, landlock, job object, ...)
//   level    required (strict) | should (standard) | may (permissive) | info (report only, or profile none)
//   verdict  ok | warn | fail. missing/failed: fail when required, warn when should. emulated: fail only when required
//            and the policy says "emulation": "deny".
//
// lockdown order (SBX-2): audit sinks (the audit file is opened before the file system is confined) -> resource
// limits -> no-new-privs -> privilege check (SBX-5: root / an elevated token is refused unless allow_root; FORGE
// never changes uid itself) -> file-system confinement (Landlock write rules) -> syscall confinement
// (os::apply_sandbox: seccomp-bpf / pledge / sandbox_init / job object) -> Windows mitigation policies ->
// integrity settings (FORGE_REQUIRE_SIGNED / FORGE_TRUSTED_KEYS for inference::model) -> the policy becomes the
// active one for enforce(). The plan is checked first: if any required control would fail, lockdown throws before
// changing anything. With profile::none lockdown does nothing and returns the report.
#include <forge/security/audit.hpp>
#include <forge/security/enforce.hpp>
#include <forge/security/errors.hpp>
#include <forge/security/files.hpp>
#include <forge/security/policy.hpp>
#include <forge/security/secure_buffer.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/keystore.hpp>
#include <forge/os/oserror.hpp>
#include <forge/os/os.hpp>
#include <forge/os/process.hpp>
#include <forge/os/sandbox.hpp>
#include <forge/os/users.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#if __has_include(<forge/core/build_info.hpp>)
#include <forge/core/build_info.hpp>   // forge::build_hardening() (FSS-BLD-1), when the build provides it
#define FORGE_SECURITY_HAVE_BUILD_INFO 1
#endif
#if defined(FORGE_OS_POSIX)
#include <sys/resource.h>
#endif
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/stat.h>
#endif
#if defined(FORGE_OS_FREEBSD)
#include <sys/procctl.h>
#endif

namespace forge::security {

struct control {
    std::string id, name, state, backend, level, verdict, detail;
};
struct report {
    std::string profile, source, platform;
    bool applied = false;   // produced by lockdown() (true) or check() (false)
    bool ok = true;         // no control has verdict "fail"
    std::vector<control> controls;
    std::string policy_json;
    const control* find(std::string_view name) const {
        for (auto& c : controls) if (c.name == name) return &c;
        return nullptr;
    }
    std::string json() const {
        std::string o = "{\"profile\":"; detail::json_str(o, profile);
        o += ",\"source\":"; detail::json_str(o, source);
        o += ",\"platform\":"; detail::json_str(o, platform);
        o += std::string(",\"applied\":") + (applied ? "true" : "false") + ",\"ok\":" + (ok ? "true" : "false") + ",\"controls\":[";
        for (std::size_t i = 0; i < controls.size(); ++i) {
            auto& c = controls[i];
            if (i) o += ',';
            o += "{\"id\":"; detail::json_str(o, c.id);
            o += ",\"name\":"; detail::json_str(o, c.name);
            o += ",\"state\":"; detail::json_str(o, c.state);
            o += ",\"backend\":"; detail::json_str(o, c.backend);
            o += ",\"level\":"; detail::json_str(o, c.level);
            o += ",\"verdict\":"; detail::json_str(o, c.verdict);
            o += ",\"detail\":"; detail::json_str(o, c.detail);
            o += '}';
        }
        o += "],\"policy\":" + (policy_json.empty() ? std::string("null") : policy_json) + "}";
        return o;
    }
    std::string text() const {
        std::string o = "FORGE security " + std::string(applied ? "lockdown" : "check") + ": profile " + profile + " (" + source + "), " + platform + "\n";
        if (profile == "none") o += "  no FSS profile active: nothing is confined (set FORGE_SECURITY_PROFILE or provide forge-security.json)\n";
        for (auto& c : controls) {
            char b[160];
            std::snprintf(b, sizeof b, "  %-6s %-26s %-9s %-8s %-4s ", c.id.c_str(), c.name.c_str(), c.state.c_str(), c.level.c_str(), c.verdict.c_str());
            o += b; o += c.backend;
            if (!c.detail.empty()) o += " (" + c.detail + ")";
            o += '\n';
        }
        o += ok ? "verdict: ok\n" : "verdict: FAIL (a required control cannot be met)\n";
        return o;
    }
};

namespace detail {
inline const char* platform_name() {
#if defined(FORGE_OS_WINDOWS)
    return "windows";
#elif defined(FORGE_OS_ANDROID)
    return "android";
#elif defined(__linux__)
    return "linux";
#elif defined(FORGE_OS_IOS)
    return "ios";
#elif defined(FORGE_OS_MACOS)
    return "macos";
#elif defined(FORGE_OS_FREEBSD)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#else
    return "other";
#endif
}
inline const char* level_for(profile p) {
    switch (p) { case profile::strict: return "required"; case profile::standard: return "should"; case profile::permissive: return "may"; default: return "info"; }
}
inline std::string verdict_for(const std::string& state, const std::string& level, emulation_mode em) {
    if (state == "active" || state == "available" || state == "n/a") return "ok";
    if (state == "emulated") return level == "required" && em == emulation_mode::deny ? "fail" : "ok";
    if (level == "required") return "fail";
    if (level == "should") return "warn";
    return state == "failed" ? "warn" : "ok";
}
struct lockdown_state { std::mutex m; bool applied = false; report last; };
inline lockdown_state& lds() { static lockdown_state s; return s; }

#if defined(FORGE_OS_POSIX)
inline bool set_rlimit(int which, rlim_t v) {
    struct rlimit cur{};
    if (::getrlimit(which, &cur) == 0 && cur.rlim_max != RLIM_INFINITY && v > cur.rlim_max) v = cur.rlim_max;
    struct rlimit r{v, v};
    return ::setrlimit(which, &r) == 0;
}
#endif
#if defined(__linux__)
inline bool no_new_privs_set() {
    auto s = os::detail::slurp("/proc/self/status"); auto p = s.find("NoNewPrivs:");
    return p != std::string::npos && std::atoi(s.c_str() + p + 11) == 1;
}
// fs write confinement with Landlock: every write right handled, allowed beneath `roots` (directories: all write
// rights; files: write + truncate) and on /dev/null. Roots that do not exist are skipped (reported in *skipped).
inline os::status landlock_write_roots(const std::vector<std::string>& roots, std::string* skipped) {
    namespace ll = os::detail::ll;
    const int a = ll::abi();
    if (!a) return {{}, os::unsupported("landlock")};
    ll::ruleset_attr at; at.handled_fs = ll::write_rights(a);
    const std::size_t sz = a >= 6 ? sizeof at : a >= 4 ? 16 : 8;
    int fd = int(::syscall(ll::nr_create, &at, sz, 0u));
    if (fd < 0) return {{}, os::last_error("landlock_create_ruleset")};
    auto add = [&](const std::string& path) -> bool {
        int pf = ::open(path.c_str(), O_PATH | O_CLOEXEC);
        if (pf < 0) { if (skipped) *skipped += (skipped->empty() ? "" : ", ") + path; return true; }
        struct stat st{}; ::fstat(pf, &st);
        ll::path_beneath_attr pb; pb.parent_fd = pf;
        pb.allowed = S_ISDIR(st.st_mode) ? ll::write_rights(a) : (ll::fs_write_file | (a >= 3 ? std::uint64_t(ll::fs_truncate) : 0));
        bool ok = ::syscall(ll::nr_add_rule, fd, ll::rule_path_beneath, &pb, 0u) == 0;
        ::close(pf);
        return ok;
    };
    for (auto& r : roots) if (!add(r)) { auto e = os::last_error("landlock_add_rule " + r); ::close(fd); return {{}, e}; }
    add("/dev/null");
    if (!ll::restrict_self(fd)) { auto e = os::last_error("landlock_restrict_self"); ::close(fd); return {{}, e}; }
    ::close(fd);
    ll::applied() = true;
    return {};
}
inline std::string mac_label() {
    auto s = os::detail::slurp("/proc/self/attr/current");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\0')) s.pop_back();
    return s;
}
#endif
#if defined(FORGE_OS_WINDOWS)
// SetProcessMitigationPolicy (Windows 8+), looked up at run time. Policy numbers and flag bits from winnt.h.
inline bool win_mitigate(int policy_id, DWORD flags) {
    using fn_t = BOOL(WINAPI*)(int, PVOID, SIZE_T);
    static auto fn = reinterpret_cast<fn_t>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessMitigationPolicy")));
    if (!fn) return false;
    return fn(policy_id, &flags, sizeof flags) != 0;
}
constexpr int mp_dynamic_code = 2, mp_strict_handle = 3, mp_extension_points = 6, mp_image_load = 10;
#endif

// Hardening of this build (FSS-BLD). Uses forge::build_hardening() when core/build_info.hpp exists; otherwise the
// compiler's own macros for the translation unit that includes this header.
template <class T> std::string hardening_text(const T& h) {
    if constexpr (requires { std::string(h); }) return std::string(h);
    else if constexpr (requires { std::string(h.summary()); }) return std::string(h.summary());
    else if constexpr (requires { std::string(h.to_string()); }) return std::string(h.to_string());
    else if constexpr (requires { std::string(h.str()); }) return std::string(h.str());
    else if constexpr (requires { std::string(h.json()); }) return std::string(h.json());
    else return "present";
}
inline void hardening(control& c) {
#if defined(FORGE_SECURITY_HAVE_BUILD_INFO)
    c.backend = "forge::build_hardening()";
    c.detail = hardening_text(forge::build_hardening());
    c.state = "active";
#else
    std::string d;
    auto item = [&](const char* n, bool on) { d += (d.empty() ? "" : ", ") + std::string(n) + (on ? "=yes" : "=no"); };
#if defined(_MSC_VER) && !defined(__clang__)
    item("/GS", true);
#else
#if defined(__SSP_STRONG__) || defined(__SSP_ALL__) || defined(__SSP__)
    item("stack-protector", true);
#else
    item("stack-protector", false);
#endif
#if defined(_FORTIFY_SOURCE)
    item(("fortify=" + std::to_string(_FORTIFY_SOURCE)).c_str(), _FORTIFY_SOURCE > 0);
#else
    item("fortify", false);
#endif
#if defined(__PIE__) || defined(__pie__)
    item("pie", true);
#else
    item("pie", false);
#endif
#if defined(__CET__)
    item("cet", true);
#endif
#endif
    c.backend = "compiler macros (core/build_info.hpp not present)";
    c.detail = d;
    c.state = d.find("=no") == std::string::npos ? "active" : "missing";
#endif
}

inline report evaluate(const policy& p, bool apply) {
    report r;
    r.profile = name(p.prof); r.source = p.source; r.platform = platform_name(); r.applied = apply; r.policy_json = to_json(p);
    const std::string L = level_for(p.prof);
    r.controls.reserve(64);   // add() hands out references: no reallocation while they are live
    auto add = [&](const char* id, const char* nm, std::string state, std::string backend, std::string level, std::string det = {}) -> control& {
        r.controls.push_back({id, nm, std::move(state), std::move(backend), std::move(level), "", std::move(det)});
        return r.controls.back();
    };
    const bool on = p.active();
    auto when = [&](bool requested) { return on && requested; };

    // ---- SBX-1
    add("SBX-1", "lockdown", apply ? "active" : (lds().applied ? "active" : "available"), "forge::security::lockdown", "info");

    // ---- AUD-1 (configured first: the audit file is opened before the file system is confined)
    {
        auto& c = add("AUD-1", "audit: OS facility", "n/a",
#if defined(_WIN32)
                      "Windows event log (source forge-security)",
#else
                      "syslog(3) / journald",
#endif
                      L);
        if (when(p.audit.os)) {
            c.state = apply ? (set_audit_os(true) ? "active" : "failed") : (audit_os_active() ? "active" : "available");
        }
        auto& f = add("AUD-1", "audit: JSON-lines file", "n/a", "file, 0600, O_APPEND|O_NOFOLLOW", L, p.audit.file);
        if (when(!p.audit.file.empty())) {
            if (apply) { auto s = open_audit_file(p.audit.file); f.state = s ? "active" : "failed"; if (!s) f.detail = s.err.what; }
            else {
                auto dir = os::fs::u8str(os::fs::u8path(os::absolute(p.audit.file)).parent_path());
                f.state = audit_file() == p.audit.file ? "active" : os::fs::is_dir(dir) ? "available" : "missing";
            }
        }
    }

    // ---- SBX-2 resource limits
    {
#if defined(FORGE_OS_POSIX)
        const char* be = "setrlimit";
#else
        const char* be = "job object";
#endif
        auto& core = add("SBX-2", "limit: core dumps", "n/a", be, L, p.limits.core ? "core=" + std::to_string(*p.limits.core) : "");
        auto& nofile = add("SBX-2", "limit: open files", "n/a", be, L, p.limits.nofile ? "nofile=" + std::to_string(p.limits.nofile) : "");
        auto& as = add("SBX-2", "limit: address space", "n/a", be, L, p.limits.address_space_mb ? std::to_string(p.limits.address_space_mb) + " MiB" : "");
        auto& cpu = add("SBX-2", "limit: cpu time", "n/a", be, L, p.limits.cpu_seconds ? std::to_string(p.limits.cpu_seconds) + " s" : "");
        auto& np = add("SBX-2", "limit: processes", "n/a", be, L, p.limits.nproc ? "nproc=" + std::to_string(p.limits.nproc) : "");
        if (when(p.limits.core.has_value())) core.state = "available";
        if (when(p.limits.nofile > 0)) nofile.state = "available";
        if (when(p.limits.address_space_mb > 0)) as.state = "available";
        if (when(p.limits.cpu_seconds > 0)) cpu.state = "available";
        if (when(p.limits.nproc > 0)) np.state = "available";
#if defined(FORGE_OS_WINDOWS)
        if (nofile.state == "available") { nofile.state = "n/a"; nofile.backend = "none"; nofile.detail += ": Windows has no per-process handle limit to set"; }
        if (core.state == "available") core.backend = "job object DIE_ON_UNHANDLED_EXCEPTION";
        if (np.state == "available") np.backend = "job object ActiveProcessLimit";
        if (apply && (core.state == "available" || as.state == "available" || cpu.state == "available" || np.state == "available")) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            auto& f = li.BasicLimitInformation.LimitFlags;
            if (core.state == "available") f |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
            if (as.state == "available") { f |= JOB_OBJECT_LIMIT_PROCESS_MEMORY; li.ProcessMemoryLimit = SIZE_T(p.limits.address_space_mb) << 20; }
            if (cpu.state == "available") { f |= JOB_OBJECT_LIMIT_PROCESS_TIME; li.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = LONGLONG(p.limits.cpu_seconds) * 10000000LL; }
            if (np.state == "available") { f |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS; li.BasicLimitInformation.ActiveProcessLimit = DWORD(p.limits.nproc); }
            HANDLE j = CreateJobObjectW(nullptr, nullptr);
            bool ok = j && SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li) && AssignProcessToJobObject(j, GetCurrentProcess());
            if (!ok && j) CloseHandle(j);   // on success the handle is kept for the life of the process
            for (auto* c : {&core, &as, &cpu, &np}) if (c->state == "available") { c->state = ok ? "active" : "failed"; if (!ok) c->detail += " " + os::last_error("job object").what; }
        }
#elif defined(FORGE_OS_POSIX)
#if defined(FORGE_OS_MACOS) || defined(FORGE_OS_ANDROID)
        if (as.state == "available") { as.state = "missing"; as.backend = "none (RLIMIT_AS is not usable here; os::spawn_spec has a watchdog for children)"; }
#endif
        if (apply) {
            auto run = [&](control& c, int which, std::uint64_t v) { if (c.state == "available") c.state = set_rlimit(which, rlim_t(v)) ? "active" : "failed"; };
            run(core, RLIMIT_CORE, p.limits.core.value_or(0));
            run(nofile, RLIMIT_NOFILE, p.limits.nofile);
            run(as, RLIMIT_AS, p.limits.address_space_mb << 20);
            if (cpu.state == "available") {   // soft = limit (SIGXCPU), hard = limit + 1 (SIGKILL), as os::apply_limits
                struct rlimit cur{}; rlim_t s = rlim_t(p.limits.cpu_seconds), h = s + 1;
                if (::getrlimit(RLIMIT_CPU, &cur) == 0 && cur.rlim_max != RLIM_INFINITY) { if (h > cur.rlim_max) h = cur.rlim_max; if (s > h) s = h; }
                struct rlimit rl{s, h}; cpu.state = ::setrlimit(RLIMIT_CPU, &rl) == 0 ? "active" : "failed";
            }
#if defined(RLIMIT_NPROC)
            run(np, RLIMIT_NPROC, p.limits.nproc);
#endif
        }
#if !defined(RLIMIT_NPROC)
        if (np.state == "available") np.state = "missing";
#endif
#else
        for (auto* c : {&core, &nofile, &as, &cpu, &np}) if (c->state == "available") c->state = "missing";
#endif
    }

    // ---- SBX-2 no-new-privs
    {
        auto& c = add("SBX-2", "no-new-privs", on ? "available" : "n/a", "", L);
#if defined(__linux__)
        c.backend = "prctl(PR_SET_NO_NEW_PRIVS)";
        if (no_new_privs_set()) c.state = on ? "active" : c.state;
        if (apply && on) c.state = ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 ? "active" : "failed";
#elif defined(FORGE_OS_FREEBSD) && defined(PROC_NO_NEW_PRIVS_CTL)
        c.backend = "procctl(PROC_NO_NEW_PRIVS_CTL)";
        if (apply && on) { int arg = PROC_NO_NEW_PRIVS_ENABLE; c.state = ::procctl(P_PID, ::getpid(), PROC_NO_NEW_PRIVS_CTL, &arg) == 0 ? "active" : "failed"; }
#else
        c.backend = "none"; c.level = "info";
        if (on) { c.state = "n/a"; c.detail = "no such mechanism on this OS (set-id programs are refused by the sandbox where one exists)"; }
#endif
    }

    // ---- SBX-5 root / Administrator
    {
        const bool elevated = os::is_elevated();
#if defined(_WIN32)
        auto& c = add("SBX-5", "no root / Administrator", "n/a", "elevated token check", L);
#else
        auto& c = add("SBX-5", "no root / Administrator", "n/a", "effective uid check", L);
#endif
        if (on) {
            if (!elevated) c.state = apply ? "active" : "available";
            else if (p.allow_root) { c.state = "n/a"; c.detail = "running elevated; allow_root is set"; if (apply) audit("sandbox", decision::weaken, "SBX-5", "root", "running elevated with allow_root"); }
            else { c.state = "failed"; c.detail = "running as root / Administrator and allow_root is false"; }
        }
    }

    // ---- SBX-3 confinement plan
    auto has_star = [](const std::vector<std::string>& v) { for (auto& e : v) if (e == "*") return true; return false; };
    const bool write_any = has_star(p.fs.write), read_any = write_any || has_star(p.fs.read);
    const bool write_none = p.fs.write.empty();
    os::sandbox_policy sp;   // what goes to os::apply_sandbox
#if defined(__linux__)
    const int ll_abi = os::detail::ll::abi();
    const std::string sc = std::string("seccomp-bpf") + (ll_abi ? "+landlock" : "");
#endif
    auto& fsw = add("SBX-3", "fs: write confinement", "n/a", "", L);
    auto& fsr = add("SBX-3", "fs: read confinement", "n/a", "", L);
    auto& ex = add("SBX-3", "exec denial", "n/a", "", L);
    auto& net = add("SBX-3", "network confinement", "n/a", "", L, on ? std::string("net.mode ") + name(p.net.mode) : "");
    auto& dyn = add("SBX-3", "dynamic code (W^X)", "n/a", "", L);
    auto& mit = add("SBX-3", "process mitigations", "n/a", "", p.prof == profile::strict ? "should" : L);
    const char* emu = "enforce() in-process checks";
    bool landlock_writes = false;
    if (on && !write_any) {
        if (write_none) {
#if defined(__linux__)
            fsw.state = "available"; fsw.backend = sc; sp.deny_fs_write = true;
#elif defined(__OpenBSD__)
            fsw.state = "available"; fsw.backend = "pledge"; sp.deny_fs_write = true;
#elif defined(FORGE_OS_MACOS)
            fsw.state = "available"; fsw.backend = "sandbox_init"; sp.deny_fs_write = true;
#elif defined(FORGE_OS_IOS)
            fsw.state = "active"; fsw.backend = "ios-app-sandbox";
#else
            fsw.state = "emulated"; fsw.backend = emu;
#endif
        } else {
#if defined(__linux__)
            if (ll_abi) { fsw.state = "available"; fsw.backend = "landlock (ABI " + std::to_string(ll_abi) + ") write rules"; landlock_writes = true; }
            else { fsw.state = "emulated"; fsw.backend = emu; fsw.detail = "Landlock unavailable: no kernel path rules"; }
#elif defined(FORGE_OS_IOS)
            fsw.state = "active"; fsw.backend = "ios-app-sandbox";
#else
            fsw.state = "emulated"; fsw.backend = emu; fsw.detail = "no kernel per-path write rules on this OS";
#endif
        }
    }
    if (on && !read_any) { fsr.state = "emulated"; fsr.backend = emu; fsr.detail = "kernel read confinement would break late library loading"; }
    if (on && !p.exec) {
#if defined(__linux__)
        ex.state = "available"; ex.backend = sc; sp.deny_exec = true;
#elif defined(__OpenBSD__)
        ex.state = "available"; ex.backend = "pledge"; sp.deny_exec = true;
#elif defined(FORGE_OS_MACOS)
        if (write_none) { ex.state = "available"; ex.backend = "sandbox_init pure-computation"; sp.deny_exec = true; }
        else { ex.state = "emulated"; ex.backend = emu; ex.detail = "sandbox_init cannot deny exec without denying all writes"; }
#elif defined(FORGE_OS_IOS)
        ex.state = "active"; ex.backend = "ios-app-sandbox";
#elif defined(FORGE_OS_WINDOWS)
        ex.state = "available"; ex.backend = "job object (ActiveProcessLimit=1)"; sp.deny_exec = true;
#else
        ex.state = "emulated"; ex.backend = emu; ex.detail = "Capsicum is all-or-nothing; use os::run_sandboxed for children";
#endif
    }
    if (on && p.net.mode != net_mode::any) {
        if (p.net.mode == net_mode::none) {
#if defined(__linux__)
            net.state = "available"; net.backend = "seccomp-bpf (no sockets but AF_UNIX)" + std::string(ll_abi >= 4 ? "+landlock TCP" : ""); sp.deny_network = true;
#elif defined(__OpenBSD__)
            net.state = "available"; net.backend = "pledge"; sp.deny_network = true;
#elif defined(FORGE_OS_MACOS)
            net.state = "available"; net.backend = "sandbox_init no-internet"; sp.deny_network = true;
#elif defined(FORGE_OS_WINDOWS)
            bool has_net = true;
            if (os::detail::wsbx::in_appcontainer(&has_net) && !has_net) { net.state = "active"; net.backend = "AppContainer without network capabilities"; }
            else { net.state = "emulated"; net.backend = emu; net.detail = "a running process cannot drop network access (os::run_sandboxed children can)"; }
#else
            net.state = "emulated"; net.backend = emu;
#endif
        } else {
            net.state = "emulated"; net.backend = emu; net.detail += ": no kernel filter by address";
        }
    }
    if (on && !p.dynamic_code) {
#if defined(FORGE_OS_WINDOWS)
        dyn.state = "available"; dyn.backend = "ProcessDynamicCodePolicy (ACG)";
#else
        dyn.state = "emulated"; dyn.backend = emu; dyn.detail = "the kernel does not enforce W^X for this process";
#endif
    }
#if defined(FORGE_OS_WINDOWS)
    if (on) { mit.state = "available"; mit.backend = std::string("extension points off, no remote / low-label images") + (p.prof == profile::strict ? ", strict handle checks" : ""); }
#else
    mit.backend = "Windows only";
#endif

    // ---- apply SBX-3 (fs confinement, then syscall confinement, then mitigations)
    if (apply && on) {
#if defined(__linux__)
        if (landlock_writes) {
            std::string skipped;
            // The audit file stays writable (a file rule: append to it, nothing else there): child processes
            // inherit these rules and reopen it, e.g. rpc-serve's script children running their own lockdown.
            auto roots = expand_roots(p.fs.write);
            if (!p.audit.file.empty()) roots.push_back(os::absolute(p.audit.file));
            auto s = landlock_write_roots(roots, &skipped);
            fsw.state = s ? "active" : "failed";
            if (!s) fsw.detail = s.err.what; else if (!skipped.empty()) fsw.detail = "skipped (missing): " + skipped;
            if (s) fsw.detail += (fsw.detail.empty() ? "" : "; ") + std::string("Landlock binds this thread and threads started after lockdown (earlier threads: enforce() only)");
        }
#endif
        if (sp.deny_exec || sp.deny_network || sp.deny_fs_write) {
            auto s = os::apply_sandbox(sp);
#if defined(__linux__) && defined(__NR_seccomp)
            // seccomp and Landlock confine the calling thread and the threads it starts later. A server has threads
            // already (schedulers, pools, watchers): SECCOMP_FILTER_FLAG_TSYNC puts the same filter on every thread of
            // the process. Landlock has no equivalent on the kernels FORGE targets (see fs write confinement below).
            if (s && os::detail::audit_arch) {
                auto f = os::detail::seccomp_filter(sp);
                sock_fprog prog{static_cast<unsigned short>(f.size()), f.data()};
                if (::syscall(__NR_seccomp, 1 /*SECCOMP_SET_MODE_FILTER*/, 1u /*SECCOMP_FILTER_FLAG_TSYNC*/, &prog) != 0)
                    for (auto* c : {&ex, &net}) if (c->state == "available") c->detail += (c->detail.empty() ? "" : "; ") + std::string("seccomp TSYNC failed: threads started before lockdown are not filtered");
            }
#endif
            for (auto* c : {&fsw, &ex, &net}) {
                if (c->state != "available") continue;
                c->state = s ? "active" : "failed";
                if (!s) c->detail = s.err.what;
            }
            if (s) os::note_sandbox_policy(sp.deny_network, sp.deny_exec, sp.deny_fs_write || landlock_writes);
        } else if (landlock_writes && fsw.state == "active") os::note_sandbox_policy(false, false, true);
#if defined(FORGE_OS_WINDOWS)
        if (dyn.state == "available") dyn.state = win_mitigate(mp_dynamic_code, 1u) ? "active" : "failed";
        if (mit.state == "available") {
            bool ok = win_mitigate(mp_extension_points, 1u) && win_mitigate(mp_image_load, 1u | 2u);
            if (p.prof == profile::strict) ok = win_mitigate(mp_strict_handle, 1u | 2u) && ok;
            mit.state = ok ? "active" : "failed";
        }
#endif
    }

    // ---- SBX-6 MAC label (report only)
    {
        auto& c = add("SBX-6", "MAC label", "n/a", "", "info");
#if defined(__linux__)
        auto lab = mac_label();
        c.backend = "/proc/self/attr/current";
        if (lab.empty() || lab.find("unconfined") != std::string::npos) { c.state = "missing"; c.detail = lab.empty() ? "no SELinux / AppArmor label" : lab; }
        else { c.state = "active"; c.detail = lab; }
#else
        c.backend = "not checked on this OS";
#endif
    }

    // ---- CAP
    add("CAP-1", "capability gate", on ? (apply || lds().applied ? "active" : "available") : "n/a", "forge::security::enforce", "info",
        "covers callers of enforce(); FORGE subsystems are not all routed through it yet");
    add("CAP-2", "path canonicalisation", "active", "canonical_within / safe_join", "info");

    // ---- INT
    {
        auto& m = add("INT-1", "integrity: models", "n/a", "inference::model (active policy; FORGE_REQUIRE_SIGNED / FORGE_TRUSTED_KEYS)", L);
        if (on && p.integrity.models == requirement::required) {
            m.state = apply || lds().applied ? "active" : "available";
            m.detail = p.integrity.trusted_keys.empty() ? "no trusted_keys: any valid signature passes (integrity, not provenance)"
                                                        : std::to_string(p.integrity.trusted_keys.size()) + " trusted key(s)";
            if (apply) {
                os::setenv("FORGE_REQUIRE_SIGNED", "1");
                std::string keys; for (auto& k : p.integrity.trusted_keys) keys += (keys.empty() ? "" : ",") + k;
                if (!keys.empty()) os::setenv("FORGE_TRUSTED_KEYS", keys);
            }
        } else if (on && p.integrity.models == requirement::optional) {
            m.state = apply || lds().applied ? "active" : "available"; m.level = p.prof == profile::strict ? "should" : "info";
            m.detail = "signed models are verified at load, unsigned ones audited (inference::model under the active policy)";
        }
        const std::string keys = p.integrity.trusted_keys.empty() ? "no trusted_keys: any valid signature passes (integrity, not provenance)"
                                                                  : std::to_string(p.integrity.trusted_keys.size()) + " trusted key(s)";
        const std::string st_on = apply || lds().applied ? "active" : "available";
        auto& pl = add("INT-1", "integrity: native plugins", "n/a", "", L);
        if (on && p.integrity.plugins != requirement::off) {
            pl.state = st_on; pl.backend = "detached <plugin>.sig (security::open_plugin, INT-2)";
            if (p.integrity.plugins == requirement::optional) { pl.level = p.prof == profile::strict ? "should" : "info"; pl.detail = "signed plugins verified, unsigned ones audited; " + keys; }
            else pl.detail = keys;
        }
        auto& sc2 = add("INT-1", "integrity: scripts", "n/a", "", L);
        if (on && p.integrity.scripts != requirement::off) {
            sc2.state = st_on; sc2.backend = "detached <script>.sig (engine::run_file, imports, rpc --rpc-sig; INT-2)";
            if (p.integrity.scripts == requirement::optional) { sc2.level = p.prof == profile::strict ? "should" : "info"; sc2.detail = "signed scripts verified, unsigned ones audited; " + keys; }
            else sc2.detail = keys + "; -e / REPL code is refused";
        }
        auto& cs = add("INT-3", "OS code signing (plugins)", "n/a", "", L);
        {
            // the running executable's own signature, as a sample of what the OS mechanism reports here
            auto self = os::exe_path();
            auto sig = self.empty() ? code_signature{} : os_code_signature(self);
            cs.backend = sig.backend;
            const std::string me = self.empty() ? std::string() : signed_name(self) + ": " + name(sig.st) + (sig.signer.empty() ? "" : " (" + sig.signer + ")");
            if (!on || !p.integrity.os_code_signing) cs.detail = me;
            else if (sig.st == code_signature::state::not_applicable) {
                // no OS mechanism for shared objects: reported, not failed; plugins rely on detached signatures
                cs.state = "missing"; cs.level = "info"; cs.detail = sig.detail;
            } else if (sig.st == code_signature::state::error) { cs.state = "failed"; cs.detail = sig.detail; }
            else { cs.state = st_on; cs.detail = "plugins must be OS-signed; " + me; }
        }
    }

    // ---- FIO-1 / FIO-2
    {
#if defined(_WIN32)
        const char* tb = "CREATE_NEW / CreateDirectoryW with an owner + SYSTEM + Administrators DACL";
#else
        const char* tb = "O_CREAT|O_EXCL|O_NOFOLLOW 0600, mkdir 0700";
#endif
        add("FIO-1", "temp files", "active", tb, "info", "os::fs::make_temp_file / make_temp_dir / write_new_private_file");
        // The audit file's directory: whoever can write it can replace the audit file between runs.
        auto& ad = add("FIO-2", "private files: audit directory", "n/a", "check_private_dir (trusted)", L);
        if (on && !p.audit.file.empty()) {
            auto dir = os::fs::u8str(os::fs::u8path(os::absolute(p.audit.file)).parent_path());
            ad.detail = dir;
            if (!os::fs::is_dir(dir)) ad.state = "missing";
            else if (auto s = check_private_dir(dir, file_kind::trusted); s) ad.state = "active";
            else { ad.state = "failed"; ad.detail = s.err.what; }
        }
        // The file keystore (os::keystore file / DPAPI backends): its directory and every file in it are secret.
        auto& ks = add("FIO-2", "private files: keystore", "n/a", "check_private_dir / check_private_file (secret)", L);
        if (on) {
            auto d = os::keystore::directory();
            if (!d || !os::fs::is_dir(d.value)) { ks.detail = d ? d.value + " (absent)" : d.err.what; }
            else {
                ks.detail = d.value; ks.state = "active";
                auto s = check_private_dir(d.value, file_kind::secret);
                if (s) if (auto l = os::list_dir(d.value))
                    for (auto& e : l.value) {
                        if (e.name == "." || e.name == "..") continue;
                        if (!(s = check_private_file(os::join(d.value, e.name), file_kind::secret))) break;
                    }
                if (!s) { ks.state = "failed"; ks.detail = s.err.what; }
            }
        }
        // Policy file: checked by load_policy() (a strict policy that fails it is not loaded at all).
        add("FIO-2", "private files: policy, keys, tokens", on ? "active" : "n/a", "load_policy; require_private_file", "info",
            "the policy file (load_policy), forge sign / rpc keys and token files, rpc --tls-key, --authorized-keys");
    }

    // ---- CRY-2
    {
        auto& c = add("CRY-2", "key memory", "missing", "os::locked_buffer", on && p.prof == profile::strict ? "should" : "info");
        try {
            secure_buffer probe(32);
            c.state = probe.locked() ? "active" : "missing";
            c.detail = memory_state(probe);
#if defined(_WIN32)
            c.backend = "VirtualLock + WerRegisterExcludedMemoryBlock";
#elif defined(__linux__)
            c.backend = "mlock + MADV_DONTDUMP + MADV_WIPEONFORK";
#elif defined(__FreeBSD__)
            c.backend = "mlock + MADV_NOCORE";
#else
            c.backend = "mlock (no per-range dump exclusion: set limits.core = 0)";
#endif
            if (!probe.locked()) c.detail += "; raise RLIMIT_MEMLOCK / the working-set quota";
        } catch (...) { c.state = "failed"; c.detail = "allocation failed"; }
    }

    // ---- CRY-1
    {
        unsigned char b[16]; bool ok = os::entropy(b, sizeof b);
#if defined(_WIN32)
        const char* be = "BCryptGenRandom";
#elif defined(__linux__)
        const char* be = "getrandom";
#else
        const char* be = "arc4random_buf";
#endif
        add("CRY-1", "OS CSPRNG", ok ? "active" : "failed", be, on ? "required" : "info");
    }

    // ---- BLD-1/2
    {
        auto& c = add("BLD-1", "build hardening", "missing", "", p.prof == profile::strict ? "should" : "info");
        hardening(c);
    }

    for (auto& c : r.controls) { c.verdict = verdict_for(c.state, c.level, p.emulation); if (c.verdict == "fail") r.ok = false; }
    return r;
}
inline std::string failures(const report& r, std::string* first_id = nullptr) {
    std::string s;
    for (auto& c : r.controls)
        if (c.verdict == "fail") {
            if (first_id && first_id->empty()) *first_id = c.id;
            s += (s.empty() ? "" : "; ") + c.id + " " + c.name + ": " + c.state + (c.detail.empty() ? "" : " (" + c.detail + ")");
        }
    return s;
}
} // namespace detail

// What this process could apply under `p`, per control. Changes nothing. After lockdown() it returns lockdown's report.
inline report check(const policy& p) {
    {
        auto& s = detail::lds(); std::lock_guard g(s.m);
        if (s.applied) return s.last;
    }
    return detail::evaluate(p, false);
}

// Apply `p` to this process (one-way). Throws sandbox_error (or policy_violation) if a required control cannot be met:
// before changing anything when the plan already fails, after the fact if applying a step fails.
inline report lockdown(const policy& p) {
    auto& s = detail::lds();
    std::lock_guard g(s.m);
    if (s.applied) throw sandbox_error("lockdown() was already applied in this process", "SBX-1");
    if (!p.active()) { auto r = detail::evaluate(p, false); r.applied = false; return r; }
    set_audit_profile(name(p.prof));
    auto plan = detail::evaluate(p, false);
    if (!plan.ok) {
        std::string id; auto why = detail::failures(plan, &id);
        audit("sandbox", decision::fail, id, "lockdown", why);
        throw sandbox_error("lockdown refused: " + why, id);
    }
    auto r = detail::evaluate(p, true);
    set_active_policy(p);
    s.applied = true; s.last = r;
    for (auto& c : r.controls)
        if (c.state == "emulated" || ((c.state == "missing" || c.state == "failed") && c.level != "info"))
            audit("sandbox", c.verdict == "fail" ? decision::fail : decision::weaken, c.id, c.name, c.state + (c.detail.empty() ? "" : ": " + c.detail));
    if (!r.ok) {
        std::string id; auto why = detail::failures(r, &id);
        throw sandbox_error("lockdown failed: " + why, id);
    }
    audit("sandbox", decision::allow, "SBX-1", "lockdown", std::string("profile ") + name(p.prof) + " applied");
    return r;
}
inline bool locked_down() { auto& s = detail::lds(); std::lock_guard g(s.m); return s.applied; }

} // namespace forge::security
