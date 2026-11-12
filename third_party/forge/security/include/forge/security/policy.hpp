#pragma once
// FSS policy: profiles, the forge-security.json file and where it is looked for.
//
//   auto p = security::load_policy(cli_path);      // --policy FILE, $FORGE_SECURITY_POLICY, ./forge-security.json,
//                                                  // <config dir>/forge/forge-security.json; $FORGE_SECURITY_PROFILE
//   auto s = security::profile_defaults(security::profile::strict);
//   auto q = security::parse_policy(text, "x.json");
//
// No file and no $FORGE_SECURITY_PROFILE: profile::none ("no FSS profile active"; FORGE behaves as before the
// standard: nothing is confined, enforce() allows everything and audits nothing). This deviates from the draft's
// "no file => strict" on purpose; making strict the default for every FORGE program is a separate decision.
//
// Parsing fails closed: a syntax error, an unknown key, a duplicate key, a wrong type or a bad value is a
// policy_violation naming the file and line:col. A file named by --policy or $FORGE_SECURITY_POLICY that cannot
// be read is a policy_violation too (never a silent fallback).
//
// fs.read / fs.write entries: a path (relative ones are taken against the working directory when the policy is
// used), "*" (anything), or one of the tokens "." (working directory), "$TMP" (os::temp_dir()), "$CACHE"
// ($FORGE_CACHE, else ~/.cache/forge: where `forge run` puts imported models), "$CONFIG" (<config dir>/forge),
// "~" / "~/..." (home directory).
// net.mode: "none" (no sockets but AF_UNIX), "loopback", "hosts" (loopback + net.hosts), "any".
// net.hosts entries: "host", "host:port", "*.example.com[:port]", "[v6addr]:port".
// integrity.models / plugins / scripts: "required", "optional" (checked when signed; unsigned audited), "off".
// limits: core (null = unchanged, 0 = no core dumps), nofile, address_space_mb, cpu_seconds, nproc (0 = unchanged).
#include <forge/security/audit.hpp>
#include <forge/security/errors.hpp>
#include <forge/json/json.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>
#include <forge/os/users.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge::security {

enum class profile { none, strict, standard, permissive };
enum class net_mode { none, loopback, hosts, any };
enum class requirement { off, optional, required };
enum class emulation_mode { allow, deny };

inline const char* name(profile p) {
    switch (p) { case profile::none: return "none"; case profile::strict: return "strict"; case profile::standard: return "standard"; case profile::permissive: return "permissive"; }
    return "?";
}
inline const char* name(net_mode m) {
    switch (m) { case net_mode::none: return "none"; case net_mode::loopback: return "loopback"; case net_mode::hosts: return "hosts"; case net_mode::any: return "any"; }
    return "?";
}
inline const char* name(requirement r) {
    switch (r) { case requirement::off: return "off"; case requirement::optional: return "optional"; case requirement::required: return "required"; }
    return "?";
}
inline const char* name(emulation_mode e) { return e == emulation_mode::deny ? "deny" : "allow"; }
inline std::optional<profile> parse_profile(std::string_view s) {
    if (s == "strict") return profile::strict;
    if (s == "standard") return profile::standard;
    if (s == "permissive") return profile::permissive;
    return std::nullopt;
}

struct policy {
    profile prof = profile::none;
    struct fs_t { std::vector<std::string> read, write; } fs;
    struct net_t { net_mode mode = net_mode::any; std::vector<std::string> hosts; } net;
    bool exec = true, dynamic_code = true, env = true, allow_root = true;
    struct integrity_t {
        requirement models = requirement::off, plugins = requirement::off, scripts = requirement::off;
        std::vector<std::string> trusted_keys;   // 64 hex chars each (Ed25519 public keys)
        bool os_code_signing = false;
    } integrity;
    struct limits_t {
        std::optional<std::uint64_t> core;        // nullopt: unchanged; 0: no core dumps
        std::uint64_t nofile = 0, address_space_mb = 0, cpu_seconds = 0, nproc = 0;
    } limits;
    struct audit_t { bool os = false; std::string file; } audit;
    emulation_mode emulation = emulation_mode::allow;
    std::string source = "none";   // file path, "env:FORGE_SECURITY_PROFILE" or "none"
    bool active() const { return prof != profile::none; }
};

// The table in docs/security-standard.md "Profiles". profile::none: everything allowed, nothing audited.
inline policy profile_defaults(profile p) {
    policy r; r.prof = p; r.source = p == profile::none ? "none" : std::string("defaults:") + name(p);
    switch (p) {
    case profile::none:
        r.fs.read = {"*"}; r.fs.write = {"*"};
        break;
    case profile::strict:
        r.fs.read = {".", "$CACHE"}; r.fs.write = {".", "$TMP"};
        r.net.mode = net_mode::none;
        r.exec = false; r.dynamic_code = false; r.env = false; r.allow_root = false;
        r.integrity = {requirement::required, requirement::required, requirement::optional, {}, true};
        r.limits.core = 0; r.limits.nofile = 1024;
        r.audit.os = true;
        break;
    case profile::standard:
        r.fs.read = {"*"}; r.fs.write = {".", "$TMP"};
        r.net.mode = net_mode::hosts;
        r.exec = true; r.dynamic_code = true; r.env = true; r.allow_root = true;
        r.integrity = {requirement::optional, requirement::required, requirement::optional, {}, false};
        r.limits.core = 0;
        r.audit.os = true;
        break;
    case profile::permissive:
        r.fs.read = {"*"}; r.fs.write = {"*"};
        r.net.mode = net_mode::any;
        r.integrity = {requirement::off, requirement::optional, requirement::off, {}, false};
        r.audit.os = true;
        break;
    }
    return r;
}

namespace detail {
// 1-based line:col of byte offset `off` in `text`.
inline std::string line_col(std::string_view text, std::size_t off) {
    if (off > text.size()) off = text.size();
    std::size_t line = 1, col = 1;
    for (std::size_t i = 0; i < off; ++i) { if (text[i] == '\n') { ++line; col = 1; } else ++col; }
    return std::to_string(line) + ":" + std::to_string(col);
}
struct policy_parser {
    std::string_view text, origin;
    std::size_t hint = 0;   // search start for key positions (keys are located in document order)
    [[noreturn]] void fail_at(std::size_t off, const std::string& msg) const {
        throw policy_violation(std::string(origin) + ":" + line_col(text, off) + ": " + msg);
    }
    // Position of the quoted key `k` (best effort: the first occurrence after the previous key).
    std::size_t key_pos(std::string_view k) {
        std::string q = "\"" + std::string(k) + "\"";
        auto p = text.find(q, hint);
        if (p == text.npos) p = text.find(q);
        if (p == text.npos) return hint;
        hint = p + 1;
        return p;
    }
    [[noreturn]] void fail_key(std::string_view key, std::string_view path, const std::string& msg) {
        fail_at(key_pos(key), std::string(path) + ": " + msg);
    }
    void no_dupes(const json::value& o, std::string_view path) {
        auto& ob = o.obj();
        for (std::size_t i = 0; i < ob.size(); ++i)
            for (std::size_t j = i + 1; j < ob.size(); ++j)
                if (ob[i].first == ob[j].first) fail_key(ob[j].first, path, "duplicate key '" + ob[j].first + "'");
    }
    void only_keys(const json::value& o, std::string_view path, std::initializer_list<std::string_view> allowed) {
        no_dupes(o, path);
        for (auto& [k, v] : o.obj()) {
            bool ok = false; for (auto a : allowed) if (a == k) ok = true;
            if (!ok) fail_key(k, path, "unknown key '" + k + "'");
        }
    }
    const json::value& object_at(const json::value& v, std::string_view key, std::string_view path) {
        if (!v.is_obj()) fail_key(key, path, "expected an object");
        return v;
    }
    bool boolean(const json::value& v, std::string_view key, std::string_view path) {
        if (!v.is_bool()) fail_key(key, path, "expected true or false");
        return v.b();
    }
    std::string string(const json::value& v, std::string_view key, std::string_view path) {
        if (!v.is_str()) fail_key(key, path, "expected a string");
        if (v.str().find('\0') != std::string::npos) fail_key(key, path, "NUL in string");
        return v.str();
    }
    std::uint64_t count(const json::value& v, std::string_view key, std::string_view path) {
        if (!v.is_num()) fail_key(key, path, "expected a non-negative integer");
        double d = v.num();
        if (!(d >= 0) || d != double(std::uint64_t(d)) || d > 9007199254740992.0) fail_key(key, path, "expected a non-negative integer");
        return std::uint64_t(d);
    }
    std::vector<std::string> strings(const json::value& v, std::string_view key, std::string_view path) {
        if (!v.is_arr()) fail_key(key, path, "expected an array of strings");
        std::vector<std::string> out;
        for (auto& e : v.arr()) {
            if (!e.is_str()) fail_key(key, path, "expected an array of strings");
            if (e.str().empty() || e.str().find('\0') != std::string::npos) fail_key(key, path, "empty string or NUL in entry");
            out.push_back(e.str());
        }
        return out;
    }
    void check_fs_entry(const std::string& s, std::string_view key, std::string_view path) {
        if (s[0] == '$' && s != "$TMP" && s != "$CACHE" && s != "$CONFIG")
            fail_key(key, path, "unknown token '" + s + "' (use $TMP, $CACHE or $CONFIG)");
    }
    requirement req(const json::value& v, std::string_view key, std::string_view path) {
        auto s = string(v, key, path);
        if (s == "required") return requirement::required;
        if (s == "optional") return requirement::optional;
        if (s == "off") return requirement::off;
        fail_key(key, path, "expected \"required\", \"optional\" or \"off\"");
    }
};
} // namespace detail

// Parse a policy document. Omitted fields take the values of the profile named by "profile", else `base`
// (default strict). Throws policy_violation ("origin:line:col: message").
inline policy parse_policy(std::string_view text, std::string_view origin = "<policy>", std::optional<profile> base = std::nullopt) {
    detail::policy_parser ps{text, origin};
    json::value doc;
    try { doc = json::parse(text); }
    catch (const std::exception& e) {
        std::string m = e.what(); std::size_t off = 0;
        if (auto p = m.rfind(" at offset "); p != std::string::npos) { off = std::size_t(std::strtoull(m.c_str() + p + 11, nullptr, 10)); m.resize(p); }
        if (m.rfind("json: ", 0) == 0) m = m.substr(6);
        ps.fail_at(off, "JSON syntax error: " + m);
    }
    if (!doc.is_obj()) ps.fail_at(0, "the policy must be a JSON object");
    ps.only_keys(doc, "policy", {"profile", "fs", "net", "exec", "dynamic_code", "env", "allow_root", "integrity", "limits", "audit", "emulation"});
    profile pr = base.value_or(profile::strict);
    if (auto* v = doc.find("profile")) {
        auto s = ps.string(*v, "profile", "profile");
        auto p = parse_profile(s); if (!p) ps.fail_key("profile", "profile", "expected \"strict\", \"standard\" or \"permissive\"");
        pr = *p;
    }
    policy r = profile_defaults(pr);
    r.source = std::string(origin);
    if (auto* v = doc.find("fs")) {
        ps.object_at(*v, "fs", "fs"); ps.only_keys(*v, "fs", {"read", "write"});
        if (auto* x = v->find("read")) { r.fs.read = ps.strings(*x, "read", "fs.read"); for (auto& s : r.fs.read) ps.check_fs_entry(s, "read", "fs.read"); }
        if (auto* x = v->find("write")) { r.fs.write = ps.strings(*x, "write", "fs.write"); for (auto& s : r.fs.write) ps.check_fs_entry(s, "write", "fs.write"); }
    }
    if (auto* v = doc.find("net")) {
        ps.object_at(*v, "net", "net"); ps.only_keys(*v, "net", {"mode", "hosts"});
        if (auto* x = v->find("mode")) {
            auto s = ps.string(*x, "mode", "net.mode");
            if (s == "none") r.net.mode = net_mode::none; else if (s == "loopback") r.net.mode = net_mode::loopback;
            else if (s == "hosts") r.net.mode = net_mode::hosts; else if (s == "any") r.net.mode = net_mode::any;
            else ps.fail_key("mode", "net.mode", "expected \"none\", \"loopback\", \"hosts\" or \"any\"");
        }
        if (auto* x = v->find("hosts")) r.net.hosts = ps.strings(*x, "hosts", "net.hosts");
    }
    if (auto* v = doc.find("exec")) r.exec = ps.boolean(*v, "exec", "exec");
    if (auto* v = doc.find("dynamic_code")) r.dynamic_code = ps.boolean(*v, "dynamic_code", "dynamic_code");
    if (auto* v = doc.find("env")) r.env = ps.boolean(*v, "env", "env");
    if (auto* v = doc.find("allow_root")) r.allow_root = ps.boolean(*v, "allow_root", "allow_root");
    if (auto* v = doc.find("integrity")) {
        ps.object_at(*v, "integrity", "integrity");
        ps.only_keys(*v, "integrity", {"models", "plugins", "scripts", "trusted_keys", "os_code_signing"});
        if (auto* x = v->find("models")) r.integrity.models = ps.req(*x, "models", "integrity.models");
        if (auto* x = v->find("plugins")) r.integrity.plugins = ps.req(*x, "plugins", "integrity.plugins");
        if (auto* x = v->find("scripts")) r.integrity.scripts = ps.req(*x, "scripts", "integrity.scripts");
        if (auto* x = v->find("trusted_keys")) {
            r.integrity.trusted_keys = ps.strings(*x, "trusted_keys", "integrity.trusted_keys");
            for (auto& k : r.integrity.trusted_keys)
                if (k.size() != 64 || k.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    ps.fail_key("trusted_keys", "integrity.trusted_keys", "expected 64 hex characters (an Ed25519 public key)");
        }
        if (auto* x = v->find("os_code_signing")) r.integrity.os_code_signing = ps.boolean(*x, "os_code_signing", "integrity.os_code_signing");
    }
    if (auto* v = doc.find("limits")) {
        ps.object_at(*v, "limits", "limits");
        ps.only_keys(*v, "limits", {"core", "nofile", "address_space_mb", "cpu_seconds", "nproc"});
        if (auto* x = v->find("core")) { if (x->is_null()) r.limits.core.reset(); else r.limits.core = ps.count(*x, "core", "limits.core"); }
        if (auto* x = v->find("nofile")) r.limits.nofile = ps.count(*x, "nofile", "limits.nofile");
        if (auto* x = v->find("address_space_mb")) r.limits.address_space_mb = ps.count(*x, "address_space_mb", "limits.address_space_mb");
        if (auto* x = v->find("cpu_seconds")) r.limits.cpu_seconds = ps.count(*x, "cpu_seconds", "limits.cpu_seconds");
        if (auto* x = v->find("nproc")) r.limits.nproc = ps.count(*x, "nproc", "limits.nproc");
    }
    if (auto* v = doc.find("audit")) {
        ps.object_at(*v, "audit", "audit"); ps.only_keys(*v, "audit", {"os", "file"});
        if (auto* x = v->find("os")) r.audit.os = ps.boolean(*x, "os", "audit.os");
        if (auto* x = v->find("file")) r.audit.file = ps.string(*x, "file", "audit.file");
    }
    if (auto* v = doc.find("emulation")) {
        auto s = ps.string(*v, "emulation", "emulation");
        if (s == "allow") r.emulation = emulation_mode::allow; else if (s == "deny") r.emulation = emulation_mode::deny;
        else ps.fail_key("emulation", "emulation", "expected \"allow\" or \"deny\"");
    }
    return r;
}

// The policy as JSON (the same shape parse_policy reads, plus "source").
inline std::string to_json(const policy& p) {
    auto str = [](std::string& o, std::string_view s) { detail::json_str(o, s); };
    auto list = [&](std::string& o, const std::vector<std::string>& v) {
        o += '['; for (std::size_t i = 0; i < v.size(); ++i) { if (i) o += ','; str(o, v[i]); } o += ']';
    };
    auto b = [](bool x) { return x ? "true" : "false"; };
    std::string o = "{\"profile\":"; str(o, name(p.prof));
    o += ",\"source\":"; str(o, p.source);
    o += ",\"fs\":{\"read\":"; list(o, p.fs.read); o += ",\"write\":"; list(o, p.fs.write); o += '}';
    o += ",\"net\":{\"mode\":"; str(o, name(p.net.mode)); o += ",\"hosts\":"; list(o, p.net.hosts); o += '}';
    o += std::string(",\"exec\":") + b(p.exec) + ",\"dynamic_code\":" + b(p.dynamic_code) + ",\"env\":" + b(p.env) + ",\"allow_root\":" + b(p.allow_root);
    o += ",\"integrity\":{\"models\":"; str(o, name(p.integrity.models));
    o += ",\"plugins\":"; str(o, name(p.integrity.plugins));
    o += ",\"scripts\":"; str(o, name(p.integrity.scripts));
    o += ",\"trusted_keys\":"; list(o, p.integrity.trusted_keys);
    o += std::string(",\"os_code_signing\":") + b(p.integrity.os_code_signing) + '}';
    o += ",\"limits\":{\"core\":" + (p.limits.core ? std::to_string(*p.limits.core) : std::string("null"));
    o += ",\"nofile\":" + std::to_string(p.limits.nofile) + ",\"address_space_mb\":" + std::to_string(p.limits.address_space_mb);
    o += ",\"cpu_seconds\":" + std::to_string(p.limits.cpu_seconds) + ",\"nproc\":" + std::to_string(p.limits.nproc) + '}';
    o += std::string(",\"audit\":{\"os\":") + b(p.audit.os) + ",\"file\":"; str(o, p.audit.file); o += '}';
    o += ",\"emulation\":"; str(o, name(p.emulation));
    o += '}';
    return o;
}

// Where load_policy looks, in order (the first that exists wins; an explicit --policy / $FORGE_SECURITY_POLICY that
// does not exist is an error).
inline std::vector<std::string> policy_search_path(std::string_view cli_path = {}) {
    std::vector<std::string> v;
    if (!cli_path.empty()) v.emplace_back(cli_path);
    if (auto e = os::getenv("FORGE_SECURITY_POLICY"); !e.empty()) v.push_back(e);
    v.push_back("forge-security.json");
    if (auto c = os::config_dir(); !c.empty()) v.push_back(os::join(os::join(c, "forge"), "forge-security.json"));
    return v;
}

inline policy load_policy(std::string_view cli_path = {}) {
    std::optional<profile> env_profile;
    const std::string env_p = os::getenv("FORGE_SECURITY_PROFILE");
    if (!env_p.empty()) {
        env_profile = parse_profile(env_p);
        if (!env_profile) throw policy_violation("FORGE_SECURITY_PROFILE: expected strict, standard or permissive, got '" + env_p + "'");
    }
    const std::string env_f = os::getenv("FORGE_SECURITY_POLICY");
    auto read_named = [&](const std::string& path, const char* what) -> std::string {
        auto r = os::read_file(path);
        if (!r) throw policy_violation(std::string(what) + " " + path + ": cannot be read: " + r.err.what);
        return r.value;
    };
    std::optional<policy> out;
    std::string file;
    if (!cli_path.empty()) out = parse_policy(read_named(file = std::string(cli_path), "--policy"), cli_path, env_profile);
    else if (!env_f.empty()) out = parse_policy(read_named(file = env_f, "FORGE_SECURITY_POLICY"), env_f, env_profile);
    else {
        auto search = policy_search_path();
        for (std::size_t i = 0; i < search.size() && !out; ++i) {
            if (!os::exists(search[i])) continue;
            out = parse_policy(read_named(file = search[i], "policy"), search[i], env_profile);
        }
    }
    // FIO-2: the policy decides everything else, so nobody but its owner (or root / Administrators) may change it.
    // Profile strict refuses a file that fails the check; other profiles use it and audit a warning.
    if (out && !file.empty()) {
        if (auto s = os::fs::check_private(file, os::fs::private_kind::trusted); !s) {
            std::string msg = "policy " + s.err.what;
            if (out->prof == profile::strict) {
                audit("policy", decision::deny, "FIO-2", file, msg, "strict");
                throw policy_violation(msg + " (FIO-2: a strict policy must not be a symlink or writable by others)", "FIO-2");
            }
            audit("policy", decision::weaken, "FIO-2", file, msg + "; used anyway", name(out->prof));
        }
    }
    if (!out) {
        if (!env_profile) return profile_defaults(profile::none);   // no FSS profile active (see the header comment)
        out = profile_defaults(*env_profile);
        out->source = "env:FORGE_SECURITY_PROFILE";
    }
    if (env_profile) audit("policy", *env_profile == profile::strict ? decision::allow : decision::weaken, "POL", "FORGE_SECURITY_PROFILE",
                           "base profile " + env_p + " from the environment; effective profile " + name(out->prof));
    return *out;
}

} // namespace forge::security
