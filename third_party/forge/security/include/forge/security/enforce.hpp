#pragma once
// FSS-CAP-1: the capability gate.
//
//   security::set_active_policy(p);                                  // lockdown() does this too
//   security::enforce(capability::fs_write, path);                   // throws permission_denied (audited)
//   if (auto s = security::try_enforce(capability::net_connect, "example.com:443"); !s) ...   // os::status
//   security::enforce(p, capability::exec, "/bin/sh");               // against an explicit policy
//
// Rules (profile::none allows everything and audits nothing):
//   fs_read      the canonical path (symlinks resolved) lies under an fs.read or fs.write root, or a list has "*"
//   fs_write     the canonical path lies under an fs.write root, or fs.write has "*"
//   net_connect  target "host[:port]" / "[v6]:port": mode any; hosts: loopback or a net.hosts match; loopback:
//                loopback only; none: nothing
//   net_listen   bind address: any for mode any; loopback for every other mode ("none (loopback for servers)") and
//                net.hosts matches in mode hosts
//   exec, env, dynamic_code   the policy flag
//   plugin_load  integrity.plugins "required": denied unless `<path>.sig` verifies against integrity.trusted_keys
//                (INT-2, checked on the file as it is now). Load plugins with security::open_plugin (plugin.hpp),
//                which runs this gate and then verifies the bytes it actually loads.
// Denials are audited (category, "CAP-1", target). exec and dynamic_code allowed by a non-strict profile are audited
// as allow ("allowed, audited" in the profile table).
// This is an in-process check: it covers code that calls it. lockdown() adds the kernel-level confinement.
#include <forge/security/audit.hpp>
#include <forge/security/errors.hpp>
#include <forge/security/integrity.hpp>
#include <forge/security/path.hpp>
#include <forge/security/policy.hpp>
#include <forge/os/os.hpp>
#include <forge/os/users.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace forge::security {

enum class capability { fs_read, fs_write, net_connect, net_listen, exec, env, dynamic_code, plugin_load };
inline const char* name(capability c) {
    switch (c) {
    case capability::fs_read: return "fs_read"; case capability::fs_write: return "fs_write";
    case capability::net_connect: return "net_connect"; case capability::net_listen: return "net_listen";
    case capability::exec: return "exec"; case capability::env: return "env";
    case capability::dynamic_code: return "dynamic_code"; case capability::plugin_load: return "plugin_load";
    }
    return "?";
}
inline const char* category(capability c) {
    switch (c) {
    case capability::fs_read: case capability::fs_write: return "fs";
    case capability::net_connect: case capability::net_listen: return "net";
    case capability::exec: return "exec"; case capability::env: return "env";
    case capability::dynamic_code: return "dynamic_code"; case capability::plugin_load: return "plugin";
    }
    return "?";
}

namespace detail {
struct active_state { std::mutex m; std::shared_ptr<const policy> p = std::make_shared<const policy>(profile_defaults(profile::none)); };
inline active_state& act() { static active_state s; return s; }
} // namespace detail

inline std::string expand_root(const std::string& e);
inline os::status try_enforce(capability c, std::string_view target);
namespace detail {
// os::check_access (code below forge::security: core's KV spill store) asks the active policy through this gate.
inline os::status access_gate(os::access_kind k, std::string_view path) {
    return try_enforce(k == os::access_kind::write ? capability::fs_write : capability::fs_read, path);
}
} // namespace detail
// The active policy's fs roots are made absolute when it becomes active ("." is the working directory at that moment),
// so a later chdir() cannot move them.
inline void set_active_policy(policy p) {
    for (auto* v : {&p.fs.read, &p.fs.write}) for (auto& e : *v) if (e != "*") e = expand_root(e);
    auto sp = std::make_shared<const policy>(std::move(p));
    set_audit_profile(name(sp->prof));
    // The policy's audit sinks start with the policy, not only at lockdown(): programs that never lock down
    // (forge run) still record integrity failures. A sink that cannot open is reported by check()/lockdown().
    if (sp->active()) {
        if (!sp->audit.file.empty() && audit_file() != sp->audit.file) (void)open_audit_file(sp->audit.file);
        if (sp->audit.os) (void)set_audit_os(true);
    }
    const bool on = sp->active();
    { auto& s = detail::act(); std::lock_guard g(s.m); s.p = std::move(sp); }
    os::set_access_gate(on ? &detail::access_gate : nullptr);
}
inline std::shared_ptr<const policy> active_policy() { auto& s = detail::act(); std::lock_guard g(s.m); return s.p; }

// fs.read / fs.write entries made absolute: "." -> cwd, "$TMP", "$CACHE", "$CONFIG", "~"; "*" is kept as is.
inline std::string expand_root(const std::string& e) {
    if (e == "*") return e;
    if (e == ".") return os::cwd();
    if (e == "$TMP") return os::temp_dir();
    if (e == "$CACHE") {
        if (auto c = os::getenv("FORGE_CACHE"); !c.empty()) return os::absolute(c);
        return os::join(os::join(os::home_dir(), ".cache"), "forge");
    }
    if (e == "$CONFIG") return os::join(os::config_dir(), "forge");
    if (e == "~") return os::home_dir();
    if (e.size() > 1 && e[0] == '~' && (e[1] == '/' || e[1] == '\\')) return os::join(os::home_dir(), e.substr(2));
    return os::absolute(e);
}
inline std::vector<std::string> expand_roots(const std::vector<std::string>& v) {
    std::vector<std::string> o; o.reserve(v.size());
    for (auto& e : v) o.push_back(expand_root(e));
    return o;
}

namespace detail {
struct host_port { std::string host; int port = -1; bool ok = false; };
inline std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }
inline host_port split_host(std::string_view t) {
    host_port r;
    if (t.empty()) return r;
    std::string_view h = t, port;
    if (t.front() == '[') {   // [v6]:port
        auto e = t.find(']'); if (e == t.npos) return r;
        h = t.substr(1, e - 1);
        auto rest = t.substr(e + 1);
        if (!rest.empty()) { if (rest.front() != ':') return r; port = rest.substr(1); }
    } else if (auto c = t.rfind(':'); c != t.npos && t.find(':') == c) {   // exactly one ':' -> host:port
        h = t.substr(0, c); port = t.substr(c + 1);
    }
    if (h.empty()) return r;
    if (!port.empty()) {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != port.npos) return r;
        r.port = std::atoi(std::string(port).c_str()); if (r.port > 65535) return r;
    }
    r.host = lower(std::string(h)); r.ok = true;
    return r;
}
inline bool is_loopback(const std::string& h) {
    return h == "localhost" || h == "::1" || h == "0:0:0:0:0:0:0:1" || h.rfind("127.", 0) == 0 || h.ends_with(".localhost");
}
inline bool host_match(const std::string& pattern, const host_port& t) {
    auto p = split_host(pattern); if (!p.ok) return false;
    if (p.port >= 0 && p.port != t.port) return false;
    if (p.host == "*") return true;
    if (p.host.rfind("*.", 0) == 0) { auto suf = p.host.substr(1); return t.host.size() > suf.size() && t.host.ends_with(suf); }
    return p.host == t.host;
}
} // namespace detail

// The decision for `c` on `target` under `p`, audited. Never throws.
inline os::status try_enforce(const policy& p, capability c, std::string_view target) {
    if (!p.active()) return {};
    std::string why;
    switch (c) {
    case capability::fs_read: case capability::fs_write: {
        bool any = false;
        std::vector<std::string> roots;
        auto add = [&](const std::vector<std::string>& v) { for (auto& e : v) { if (e == "*") any = true; else roots.push_back(expand_root(e)); } };
        add(p.fs.write);
        if (c == capability::fs_read) add(p.fs.read);
        if (target.empty() || target.find('\0') != target.npos) { why = "invalid path"; break; }
        if (any) {
            // "*" still refuses device namespaces and NUL (CAP-2)
            if (target.size() >= 4 && (target[0] == '\\' || target[0] == '/') && (target[1] == '\\' || target[1] == '/') && (target[2] == '?' || target[2] == '.'))
                why = "device namespace path";
            break;
        }
        if (!canonical_within(target, roots)) why = c == capability::fs_write ? "outside policy fs.write" : "outside policy fs.read";
        break;
    }
    case capability::net_connect: case capability::net_listen: {
        if (p.net.mode == net_mode::any) break;
        auto t = detail::split_host(target);
        if (!t.ok) { why = "unparsable host"; break; }
        bool lo = detail::is_loopback(t.host);
        bool listed = false; for (auto& h : p.net.hosts) if (detail::host_match(h, t)) listed = true;
        if (c == capability::net_listen) {
            if (lo || (p.net.mode == net_mode::hosts && listed)) break;
            why = "listen address not allowed by net.mode " + std::string(name(p.net.mode));
        } else {
            if (p.net.mode == net_mode::none) why = "net.mode none";
            else if (lo) break;
            else if (p.net.mode == net_mode::hosts && listed) break;
            else why = p.net.mode == net_mode::loopback ? "net.mode loopback" : "host not in net.hosts";
        }
        break;
    }
    case capability::exec: if (!p.exec) why = "exec denied by policy"; break;
    case capability::env: if (!p.env) why = "environment access denied by policy"; break;
    case capability::dynamic_code: if (!p.dynamic_code) why = "dynamic code denied by policy (W^X)"; break;
    case capability::plugin_load:
        if (p.integrity.plugins == requirement::required) {
            const std::string f(target);
            auto rep = detached_verify_file(f, p.integrity.trusted_keys);
            if (!rep.trusted) why = "integrity.plugins is required and " + detached_sig_path(signed_name(f)) + " does not verify: " + rep.message;
        }
        break;
    }
    if (!why.empty()) {
        audit(category(c), decision::deny, "CAP-1", target, std::string(name(c)) + ": " + why, name(p.prof));
        return {{}, make_error(sec_code::permission, std::string(name(c)) + " '" + std::string(target) + "' denied: " + why)};
    }
    if (p.prof != profile::strict && (c == capability::exec || c == capability::dynamic_code || c == capability::plugin_load))
        audit(category(c), decision::allow, "CAP-1", target, name(c), name(p.prof));
    return {};
}
inline os::status try_enforce(capability c, std::string_view target) { return try_enforce(*active_policy(), c, target); }
inline void enforce(const policy& p, capability c, std::string_view target) {
    auto s = try_enforce(p, c, target);
    if (!s) throw permission_denied(s.err.what, "CAP-1");
}
inline void enforce(capability c, std::string_view target) { enforce(*active_policy(), c, target); }

} // namespace forge::security
