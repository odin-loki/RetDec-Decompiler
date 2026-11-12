// forge::security (docs/security-api.md): policy parsing, profiles, load order, paths, enforce + audit, errors,
// check() and lockdown(). Lockdown is one-way, so those cases re-run this binary with FORGE_SECURITY_TEST_CHILD=<mode>
// (dispatched from a static initializer before the test runner starts).
#include "check.hpp"
#include <forge/security/security.hpp>
#include <forge/core/memory.hpp>
#include <forge/graph/rpc_secure.hpp>
#include <forge/import/gguf.hpp>
#include <forge/import/onnx.hpp>
#include <forge/modelfile/sign.hpp>
#include <forge/json/json.hpp>
#include <forge/os/os.hpp>
#include <forge/os/fs.hpp>
#include <atomic>
#include <mutex>
#include <type_traits>
#if !defined(_WIN32)
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace forge;
namespace sec = forge::security;

static std::string scratch(const char* tag) {
    auto d = os::join(os::temp_dir(), std::string("forge_sec_") + tag + "_" + std::to_string(os::pid()));
    os::remove_all(d);
    // FIO-2: a strict policy's audit directory must be private (0700; owner + SYSTEM + Administrators on Windows,
    // where a temp dir can inherit a DACL that lets every user write, as on the MSYS2 CI runner).
    if (auto s = os::ensure_private_dir(d); !s) std::printf("  scratch %s: %s\n", d.c_str(), s.err.what.c_str());
    return os::absolute(d);
}
template <class E, class F> static std::string throws_msg(F&& f) {
    try { f(); } catch (const E& e) { return std::string("!") + e.what(); } catch (const std::exception& e) { return std::string("?wrong type: ") + e.what(); }
    return "";
}
struct capture {
    std::mutex m; std::vector<sec::audit_record> recs; int id = 0;
    capture() { id = sec::add_audit_sink([this](const sec::audit_record& r) { std::lock_guard g(m); recs.push_back(r); }); }
    ~capture() { sec::remove_audit_sink(id); }
    bool has(std::string_view cat, sec::decision d, std::string_view target_part) {
        std::lock_guard g(m);
        for (auto& r : recs) if (r.category == cat && r.d == d && r.target.find(target_part) != std::string::npos) return true;
        return false;
    }
};

// ---------------------------------------------------------------- child modes (lockdown is irreversible)
static int child_main(const std::string& mode, const std::string& dir) {
    if (mode == "lockdown") {
        auto p = sec::profile_defaults(sec::profile::strict);
        p.fs.read = {"*"}; p.fs.write = {dir};
        p.integrity.models = sec::requirement::off; p.integrity.plugins = sec::requirement::off; p.integrity.os_code_signing = false;
        p.limits.core = 0; p.limits.nofile = 256;
        p.audit.os = false; p.audit.file = os::join(dir, "audit.jsonl");
        p.allow_root = os::is_elevated();   // CI containers may run as root
#if !defined(_WIN32)
        { struct rlimit c0{}; if (::getrlimit(RLIMIT_CORE, &c0) == 0 && c0.rlim_max > 0) { c0.rlim_cur = c0.rlim_max; ::setrlimit(RLIMIT_CORE, &c0); } }
#endif
        sec::report r;
        try { r = sec::lockdown(p); }
        catch (const std::exception& e) { std::printf("lockdown threw: %s\n", e.what()); return 2; }
        std::printf("%s", r.text().c_str());
        if (!r.applied || !sec::locked_down()) return 3;
        auto* core = r.find("limit: core dumps");
        if (!core || core->state != "active") return 16;
        // in-process gate
        auto outside = os::join(os::temp_dir(), "forge_sec_outside_" + std::to_string(os::pid()));
        bool denied = false;
        try { sec::enforce(sec::capability::fs_write, outside); } catch (const sec::permission_denied&) { denied = true; }
        if (!denied) return 4;
        if (!sec::try_enforce(sec::capability::fs_write, os::join(dir, "ok.txt"))) return 5;
        // the kernel side
        if (!os::write_file(os::join(dir, "inside.txt"), "x")) { std::printf("write inside failed\n"); return 6; }
        auto* fw = r.find("fs: write confinement");
        if (fw && fw->state == "active") {
            if (os::write_file(outside, "x")) { os::remove_all(outside); std::printf("write outside allowed\n"); return 7; }
        } else std::printf("fs write confinement not active here (%s): kernel write check skipped\n", fw ? fw->state.c_str() : "?");
        auto* ex = r.find("exec denial");
        if (ex && ex->state == "active") {
            auto rr = os::run({os::exe_path()});
            if (rr && rr->exit_code == 0) { std::printf("exec allowed\n"); return 8; }
        }
#if !defined(_WIN32)
        struct rlimit rl{};
        if (::getrlimit(RLIMIT_CORE, &rl) || rl.rlim_cur != 0) return 9;
        if (::getrlimit(RLIMIT_NOFILE, &rl) || rl.rlim_cur > 256) return 10;
#endif
#if defined(__linux__)
        auto st = os::read_file("/proc/self/status");
        if (!st || st.value.find("NoNewPrivs:\t1") == std::string::npos) return 11;
#endif
#if defined(_WIN32)
        auto* dyn = r.find("dynamic code (W^X)");
        if (dyn && dyn->state == "active") {
            void* m = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (m) { VirtualFree(m, 0, MEM_RELEASE); std::printf("RWX allocation allowed under ACG\n"); return 12; }
        }
#endif
        // FORGE_REQUIRE_SIGNED is set only when integrity.models is required (it is off here)
        if (os::getenv("FORGE_REQUIRE_SIGNED") == "1") return 13;
        // a second lockdown is refused
        bool again = false;
        try { sec::lockdown(p); } catch (const sec::sandbox_error&) { again = true; }
        if (!again) return 14;
        // check() after lockdown reports the applied state
        if (!sec::check(p).applied) return 15;
        sec::open_audit_file("");   // flush/close before the parent reads it
        return 0;
    }
    if (mode == "refuse") {   // strict + emulation deny: fs read confinement is emulated everywhere -> refused up front
        auto p = sec::profile_defaults(sec::profile::strict);
        p.emulation = sec::emulation_mode::deny; p.audit.os = false;
        p.allow_root = true;
        try { sec::lockdown(p); std::printf("lockdown did not refuse\n"); return 2; }
        catch (const sec::sandbox_error& e) {
            std::printf("refused: %s\n", e.what());
            if (std::string(e.what()).find("fs: read confinement") == std::string::npos) return 3;
        }
        if (sec::locked_down()) return 4;
#if defined(__linux__)
        auto st = os::read_file("/proc/self/status");   // nothing was applied
        if (st && st.value.find("NoNewPrivs:\t1") != std::string::npos) return 5;
#endif
        // the process is still free: a write anywhere works
        auto f = os::join(dir, "still_free.txt");
        if (!os::write_file(f, "x")) return 6;
        return 0;
    }
    if (mode == "models") {   // integrity.models required -> FORGE_REQUIRE_SIGNED / FORGE_TRUSTED_KEYS for inference::model
        auto p = sec::profile_defaults(sec::profile::permissive);
        p.integrity.models = sec::requirement::required; p.integrity.trusted_keys = {std::string(64, 'a')};
        p.audit.os = false;
        auto r = sec::lockdown(p);
        if (os::getenv("FORGE_REQUIRE_SIGNED") != "1" || os::getenv("FORGE_TRUSTED_KEYS") != std::string(64, 'a')) return 2;
        auto* m = r.find("integrity: models");
        return m && m->state == "active" ? 0 : 3;
    }
    return 99;
}
static const int child_dispatch = [] {
    auto m = os::getenv("FORGE_SECURITY_TEST_CHILD"); if (m.empty()) return 0;
    os::unsetenv("FORGE_SECURITY_TEST_CHILD");
    auto c = m.find(':');
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::exit(child_main(m.substr(0, c), c == std::string::npos ? "" : m.substr(c + 1)));
}();
static int run_child(const std::string& mode, const std::string& dir) {
    os::setenv("FORGE_SECURITY_TEST_CHILD", mode + ":" + dir);
    auto r = os::run({os::exe_path()}, true);
    os::unsetenv("FORGE_SECURITY_TEST_CHILD");
    if (!r) { std::printf("  run child failed: %s\n", r.err.what.c_str()); return -1; }
    if (!r->out.empty()) { std::printf("  child[%s]:\n", mode.c_str()); std::fputs(r->out.c_str(), stdout); }
    return r->exit_code;
}

// ---------------------------------------------------------------- profiles and parsing
TEST(profile_defaults_match_table) {
    auto s = sec::profile_defaults(sec::profile::strict);
    CHECK(s.prof == sec::profile::strict && s.net.mode == sec::net_mode::none && !s.exec && !s.dynamic_code && !s.env && !s.allow_root);
    CHECK(s.integrity.models == sec::requirement::required && s.integrity.plugins == sec::requirement::required && s.integrity.os_code_signing);
    CHECK(s.limits.core && *s.limits.core == 0 && s.limits.nofile == 1024);
    CHECK(s.fs.write.size() == 2 && s.fs.write[0] == "." && s.fs.write[1] == "$TMP");
    auto d = sec::profile_defaults(sec::profile::standard);
    CHECK(d.exec && d.net.mode == sec::net_mode::hosts && d.fs.read == std::vector<std::string>{"*"} && d.integrity.models == sec::requirement::optional);
    auto p = sec::profile_defaults(sec::profile::permissive);
    CHECK(p.net.mode == sec::net_mode::any && p.fs.write == std::vector<std::string>{"*"} && !p.limits.core);
    auto n = sec::profile_defaults(sec::profile::none);
    CHECK(!n.active() && n.exec && n.net.mode == sec::net_mode::any);
}

TEST(parse_full_document) {
    const char* doc = R"({
  "profile": "strict",
  "fs": { "read": ["."], "write": ["."] },
  "net": { "mode": "none", "hosts": [] },
  "exec": false, "dynamic_code": false, "env": false, "allow_root": false,
  "integrity": { "models": "required", "plugins": "required", "scripts": "optional",
                 "trusted_keys": ["0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"], "os_code_signing": true },
  "limits": { "core": 0, "nofile": 1024, "address_space_mb": 0, "cpu_seconds": 0, "nproc": 0 },
  "audit": { "os": true, "file": "" },
  "emulation": "deny"
})";
    auto p = sec::parse_policy(doc, "doc.json");
    CHECK(p.prof == sec::profile::strict && p.fs.read.size() == 1 && p.emulation == sec::emulation_mode::deny);
    CHECK(p.integrity.trusted_keys.size() == 1 && p.source == "doc.json");
    // round trip through to_json
    auto q = sec::parse_policy([&] { auto j = json::parse(sec::to_json(p)); auto& o = j.obj_mut(); std::erase_if(o, [](auto& kv) { return kv.first == "source"; }); return json::dump(j); }());
    CHECK(q.prof == p.prof && q.fs.read == p.fs.read && q.limits.nofile == p.limits.nofile && q.emulation == p.emulation && q.integrity.trusted_keys == p.integrity.trusted_keys);
}

TEST(parse_omitted_fields_take_profile) {
    auto p = sec::parse_policy(R"({"profile":"standard","exec":false})");
    CHECK(p.prof == sec::profile::standard && !p.exec && p.net.mode == sec::net_mode::hosts && p.env);
    auto q = sec::parse_policy(R"({"net":{"mode":"loopback"}})");   // no profile: strict base
    CHECK(q.prof == sec::profile::strict && q.net.mode == sec::net_mode::loopback && !q.exec);
    auto r = sec::parse_policy(R"({"env":false})", "x", sec::profile::permissive);   // base from FORGE_SECURITY_PROFILE
    CHECK(r.prof == sec::profile::permissive && !r.env && r.exec);
    auto c = sec::parse_policy(R"({"limits":{"core":null}})");
    CHECK(!c.limits.core);
}

TEST(parse_errors_fail_closed) {
    auto m = throws_msg<sec::policy_violation>([] { sec::parse_policy("{\n  \"profile\": \"strict\",\n  \"fs\": { \"reed\": [] }\n}", "p.json"); });
    CHECK_MSG(m.find("!p.json:3:") == 0 && m.find("unknown key 'reed'") != std::string::npos, "%s", m.c_str());
    m = throws_msg<sec::policy_violation>([] { sec::parse_policy("{\n  \"exec\": false,\n  \"bogus\": 1\n}", "p.json"); });
    CHECK_MSG(m.find("!p.json:3:3:") == 0, "%s", m.c_str());
    m = throws_msg<sec::policy_violation>([] { sec::parse_policy("{\n  \"exec\": false,\n  \"env\": tru\n}", "p.json"); });
    CHECK_MSG(m.find("!p.json:3:") == 0 && m.find("syntax") != std::string::npos, "%s", m.c_str());
    m = throws_msg<sec::policy_violation>([] { sec::parse_policy(R"({"exec": "no"})"); });
    CHECK_MSG(m.find("expected true or false") != std::string::npos, "%s", m.c_str());
    m = throws_msg<sec::policy_violation>([] { sec::parse_policy(R"({"exec": true, "exec": false})"); });
    CHECK_MSG(m.find("duplicate key") != std::string::npos, "%s", m.c_str());
    for (const char* bad : {R"({"profile":"lax"})", R"({"net":{"mode":"some"}})", R"({"limits":{"nofile":-1}})", R"({"limits":{"nofile":1.5}})",
                            R"({"integrity":{"trusted_keys":["abc"]}})", R"({"integrity":{"models":"yes"}})", R"({"fs":{"write":["$HOME"]}})",
                            R"({"fs":{"write":"."}})", R"({"emulation":"maybe"})", R"([])", R"({"audit":{"os":true,"syslog":1}})", ""})
        CHECK_MSG(throws_msg<sec::policy_violation>([&] { sec::parse_policy(bad); })[0] == '!', "accepted: %s", bad);
}

TEST(load_policy_search_order_and_env) {
    auto dir = scratch("load");
    auto old = os::cwd();
    os::chdir(dir);
    os::unsetenv("FORGE_SECURITY_POLICY"); os::unsetenv("FORGE_SECURITY_PROFILE");
    auto saved_xdg = os::getenv("XDG_CONFIG_HOME");
    os::setenv("XDG_CONFIG_HOME", os::join(dir, "cfg"));   // keep the user's own config dir out of it (POSIX)
    {
        auto p = sec::load_policy();
        CHECK_MSG(p.prof == sec::profile::none && p.source == "none", "profile %s from %s", sec::name(p.prof), p.source.c_str());
    }
    {
        capture cap;
        os::setenv("FORGE_SECURITY_PROFILE", "standard");
        auto p = sec::load_policy();
        CHECK(p.prof == sec::profile::standard && p.source == "env:FORGE_SECURITY_PROFILE");
        CHECK(cap.has("policy", sec::decision::weaken, "FORGE_SECURITY_PROFILE"));
        os::setenv("FORGE_SECURITY_PROFILE", "bogus");
        CHECK(throws_msg<sec::policy_violation>([] { sec::load_policy(); })[0] == '!');
        os::unsetenv("FORGE_SECURITY_PROFILE");
    }
    os::write_file(os::join(dir, "forge-security.json"), R"({"profile":"permissive","exec":false})");
    {
        auto p = sec::load_policy();
        CHECK(p.prof == sec::profile::permissive && !p.exec && p.source == "forge-security.json");
        os::setenv("FORGE_SECURITY_PROFILE", "standard");   // the file's explicit profile wins over the env base
        CHECK(sec::load_policy().prof == sec::profile::permissive);
        os::unsetenv("FORGE_SECURITY_PROFILE");
    }
    auto other = os::join(dir, "other.json");
    os::write_file(other, R"({"profile":"standard"})");
    os::setenv("FORGE_SECURITY_POLICY", other);
    CHECK(sec::load_policy().prof == sec::profile::standard);
    CHECK(sec::load_policy(os::join(dir, "forge-security.json")).prof == sec::profile::permissive);   // --policy first
    os::setenv("FORGE_SECURITY_POLICY", os::join(dir, "missing.json"));
    CHECK(throws_msg<sec::policy_violation>([] { sec::load_policy(); })[0] == '!');   // named but missing: an error
    os::unsetenv("FORGE_SECURITY_POLICY");
    CHECK(throws_msg<sec::policy_violation>([&] { sec::load_policy(os::join(dir, "missing.json")); })[0] == '!');
    os::write_file(os::join(dir, "forge-security.json"), "{ \"profile\": ");
    CHECK(throws_msg<sec::policy_violation>([] { sec::load_policy(); }).find("forge-security.json:1:") != std::string::npos);
    if (saved_xdg.empty()) os::unsetenv("XDG_CONFIG_HOME"); else os::setenv("XDG_CONFIG_HOME", saved_xdg);
    os::chdir(old);
    os::remove_all(dir);
}

// ---------------------------------------------------------------- paths
TEST(check_relative_attack_strings) {
    const char* bad[] = {"", "/etc/passwd", "\\windows\\system32", "\\\\?\\C:\\x", "\\\\.\\PhysicalDrive0", "//?/C:/x", "\\\\server\\share\\x",
                         "C:\\x", "C:x", "c:/x", "a/b:stream", "file.txt::$DATA", "..", "../x", "a/../../x", "a\\..\\..\\x", "./../x",
                         "CON", "con.txt", "a/NUL", "aux.tar.gz", "COM1", "lpt9.log", "COM\xC2\xB9", "CONIN$", "PRN ", "nul .txt",
                         "a./b", "a /b", "...", "a/.../b", "x\ty", "x\x7fy"};
    for (auto* s : bad) CHECK_MSG(!sec::check_relative(s), "accepted '%s'", s);
    CHECK(!sec::check_relative(std::string_view("a\0b", 3)));
    auto ok = sec::check_relative("a/./b//c/../d.txt");
    CHECK(ok && ok.value == (std::vector<std::string>{"a", "b", "d.txt"}));
    CHECK(sec::check_relative("a\\b") && sec::check_relative("a\\b").value.size() == 2);
    CHECK(sec::check_relative("console.txt") && sec::check_relative("COM10") && sec::check_relative("nul_x") && sec::check_relative(".hidden"));
    CHECK(sec::check_relative(".") && sec::check_relative(".").value.empty());
}

TEST(safe_join_and_canonical_within) {
    auto root = scratch("paths");
    os::mkdirs(os::join(root, "sub"));
    auto j = sec::safe_join(root, "sub/f.txt");
    CHECK(sec::canonical_within(j, {root}).has_value());
    CHECK(throws_msg<sec::permission_denied>([&] { sec::safe_join(root, "../x"); })[0] == '!');
    CHECK(throws_msg<sec::permission_denied>([&] { sec::safe_join(root, "/etc/passwd"); })[0] == '!');
    CHECK(throws_msg<sec::permission_denied>([&] { sec::safe_join(root, "sub\\..\\..\\x"); })[0] == '!');
    try { sec::safe_join(root, "CON"); CHECK(false); } catch (const sec::permission_denied& e) { CHECK(e.control == "CAP-3"); }
    CHECK(!sec::canonical_within(os::join(root, "sub/../../x"), {root}));
    CHECK(sec::canonical_within(os::join(root, "sub/../y"), {root}).has_value());
    CHECK(!sec::canonical_within(root + "x", {root}));                       // sibling with a common prefix
    CHECK(!sec::canonical_within("\\\\?\\C:\\Windows", {"\\\\?\\C:\\"}));
    CHECK(!sec::canonical_within(std::string_view("a\0b", 3), {root}));
    CHECK(sec::canonical_within(os::join(root, "new/deeper/file"), {root}).has_value());   // need not exist
    // a symlink inside root that points outside is refused
    auto outside = scratch("paths_out");
    if (os::fs::symlink(outside, os::join(root, "link"))) {
        CHECK(!sec::try_safe_join(root, "link/x"));
        CHECK(!sec::canonical_within(os::join(root, "link/x"), {root}));
        CHECK(sec::canonical_within(os::join(root, "link/x"), {outside}).has_value());
    } else std::printf("  skip symlink case: cannot create symlinks here\n");
#if defined(_WIN32)
    std::string up = root; for (auto& c : up) c = char(std::toupper((unsigned char)c));
    CHECK(sec::canonical_within(os::join(up, "a"), {root}).has_value());   // case-insensitive on Windows
#endif
    os::remove_all(root); os::remove_all(outside);
}

// ---------------------------------------------------------------- enforce + audit
TEST(enforce_allow_deny_and_audit) {
    auto root = scratch("enf");
    auto p = sec::profile_defaults(sec::profile::strict);
    p.fs.read = {root}; p.fs.write = {os::join(root, "w")};
    p.net.mode = sec::net_mode::hosts; p.net.hosts = {"api.example.com:443", "*.models.example", "[2001:db8::1]:8080"};
    os::mkdirs(os::join(root, "w"));
    capture cap;
    CHECK(sec::try_enforce(p, sec::capability::fs_read, os::join(root, "a.txt")));
    CHECK(sec::try_enforce(p, sec::capability::fs_read, os::join(root, "w/b.txt")));
    CHECK(sec::try_enforce(p, sec::capability::fs_write, os::join(root, "w/b.txt")));
    auto s = sec::try_enforce(p, sec::capability::fs_write, os::join(root, "a.txt"));
    CHECK(!s && s.err.code == int(sec::sec_code::permission) && sec::classify(s.err) == os::os_errc::permission_denied);
    CHECK(cap.has("fs", sec::decision::deny, "a.txt"));
    CHECK(!sec::try_enforce(p, sec::capability::fs_read, os::join(root, "w/../../escape")));
    {
        std::lock_guard g(cap.m);
        auto& r = cap.recs.front();
        CHECK(r.control == "CAP-1" && r.profile == "strict" && r.pid == os::pid() && r.time.size() == 24 && r.time.back() == 'Z');
        auto j = json::parse(sec::to_json(r));
        for (auto* k : {"time", "pid", "profile", "category", "decision", "control", "target"}) CHECK_MSG(j.find(k), "missing %s", k);
        CHECK(j["decision"].str() == "deny");
    }
    bool threw = false;
    try { sec::enforce(p, sec::capability::fs_write, "/etc/forge-should-not-exist"); }
    catch (const sec::security_error& e) { threw = true; CHECK(e.sc == sec::sec_code::permission && e.kind == os::os_errc::permission_denied && std::string(e.script_type()) == "PermissionError"); }
    CHECK(threw);
    // network
    CHECK(sec::try_enforce(p, sec::capability::net_connect, "api.example.com:443"));
    CHECK(!sec::try_enforce(p, sec::capability::net_connect, "api.example.com:80"));
    CHECK(sec::try_enforce(p, sec::capability::net_connect, "cdn.models.example:443"));
    CHECK(!sec::try_enforce(p, sec::capability::net_connect, "models.example:443"));
    CHECK(!sec::try_enforce(p, sec::capability::net_connect, "evil.com:443"));
    CHECK(sec::try_enforce(p, sec::capability::net_connect, "[2001:db8::1]:8080"));
    CHECK(sec::try_enforce(p, sec::capability::net_connect, "127.0.0.1:9000") && sec::try_enforce(p, sec::capability::net_connect, "[::1]:9000"));
    CHECK(!sec::try_enforce(p, sec::capability::net_connect, "host:99999") && !sec::try_enforce(p, sec::capability::net_connect, "[::1"));
    CHECK(sec::try_enforce(p, sec::capability::net_listen, "127.0.0.1:8080") && !sec::try_enforce(p, sec::capability::net_listen, "0.0.0.0:8080"));
    p.net.mode = sec::net_mode::none;
    CHECK(!sec::try_enforce(p, sec::capability::net_connect, "127.0.0.1:9000"));
    CHECK(sec::try_enforce(p, sec::capability::net_listen, "localhost:8080"));   // "none (loopback for servers)"
    p.net.mode = sec::net_mode::loopback;
    CHECK(sec::try_enforce(p, sec::capability::net_connect, "localhost:1") && !sec::try_enforce(p, sec::capability::net_connect, "api.example.com:443"));
    // flags
    CHECK(!sec::try_enforce(p, sec::capability::exec, "/bin/sh") && !sec::try_enforce(p, sec::capability::env, "HOME"));
    CHECK(!sec::try_enforce(p, sec::capability::dynamic_code, "jit") && !sec::try_enforce(p, sec::capability::plugin_load, "x.so"));
    CHECK(cap.has("exec", sec::decision::deny, "/bin/sh") && cap.has("plugin", sec::decision::deny, "x.so"));
    auto st = sec::profile_defaults(sec::profile::standard);
    CHECK(sec::try_enforce(st, sec::capability::exec, "/bin/true") && cap.has("exec", sec::decision::allow, "/bin/true"));
    CHECK(sec::try_enforce(st, sec::capability::fs_read, "/anything") && !sec::try_enforce(st, sec::capability::plugin_load, "y.so"));
    // profile none: everything allowed, nothing audited
    auto n = sec::profile_defaults(sec::profile::none);
    std::size_t before; { std::lock_guard g(cap.m); before = cap.recs.size(); }
    CHECK(sec::try_enforce(n, sec::capability::exec, "x") && sec::try_enforce(n, sec::capability::fs_write, "/etc/x"));
    { std::lock_guard g(cap.m); CHECK(cap.recs.size() == before); }
    // the process-wide active policy starts as none
    CHECK(sec::active_policy()->prof == sec::profile::none && sec::try_enforce(sec::capability::exec, "x"));
    // a throwing sink never turns a deny into an allow (AUD-3)
    int bad = sec::add_audit_sink([](const sec::audit_record&) { throw std::runtime_error("sink down"); });
    auto f0 = sec::audit_failures();
    CHECK(!sec::try_enforce(p, sec::capability::exec, "/bin/sh"));
    CHECK(sec::audit_failures() > f0);
    sec::remove_audit_sink(bad);
    os::remove_all(root);
}

TEST(error_hierarchy_and_codes) {
    CHECK(sec::is_security_code(0x5EC01) && sec::is_security_code(0x5EC04) && !sec::is_security_code(os::unsupported_code));
    auto chk = [](auto ex, sec::sec_code c, const char* fss) {
        try { throw ex; }
        catch (const sec::security_error& e) { CHECK(e.sc == c && e.code == int(c) && std::string(e.fss_name()) == fss); }
        try { throw ex; } catch (const os::os_exception&) {} catch (...) { CHECK(false); }
    };
    chk(sec::policy_violation("x"), sec::sec_code::policy, "PolicyViolation");
    chk(sec::integrity_error("x"), sec::sec_code::integrity, "IntegrityError");
    chk(sec::permission_denied("x"), sec::sec_code::permission, "PermissionDenied");
    chk(sec::sandbox_error("x"), sec::sec_code::sandbox, "SandboxError");
    CHECK(std::string(sec::sandbox_error("x").script_type()) == "SandboxViolationError");
    CHECK(std::string(sec::permission_denied("x").script_type()) == "PermissionError");
    CHECK(throws_msg<sec::integrity_error>([] { sec::throw_if(sec::make_error(sec::sec_code::integrity, "bad sig")); }) == "!bad sig");
    CHECK(throws_msg<sec::sandbox_error>([] { sec::throw_if(sec::make_error(sec::sec_code::sandbox, "no")); })[0] == '!');
}

TEST(audit_file_permissions) {
    auto dir = scratch("audit");
    auto f = os::join(dir, "audit.jsonl");
    CHECK(sec::open_audit_file(f));
    CHECK(sec::audit_file() == f);
    sec::audit("test", sec::decision::deny, "AUD-1", "target-1", "detail \"quoted\"\n");
    sec::audit("test", sec::decision::weaken, "AUD-1", "target-2");
    CHECK(sec::open_audit_file(""));
    auto txt = os::read_file(f);
    CHECK(txt);
    std::size_t lines = 0, at = 0;
    while (txt && (at = txt.value.find('\n', at)) != std::string::npos) { ++lines; ++at; }
    CHECK(lines == 2);
    auto first = json::parse(txt.value.substr(0, txt.value.find('\n')));
    CHECK(first["target"].str() == "target-1" && first["detail"].str() == "detail \"quoted\"\n" && first["decision"].str() == "deny");
#if !defined(_WIN32)
    struct stat st{};
    CHECK(::stat(f.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    // an existing loose file is tightened
    auto g = os::join(dir, "loose.jsonl"); os::write_file(g, ""); ::chmod(g.c_str(), 0644);
    CHECK(sec::open_audit_file(g)); sec::open_audit_file("");
    CHECK(::stat(g.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    // a symlink is refused (O_NOFOLLOW)
    auto l = os::join(dir, "link.jsonl");
    if (::symlink(f.c_str(), l.c_str()) == 0) CHECK(!sec::open_audit_file(l));
    CHECK(!sec::open_audit_file(dir));   // a directory is refused
#else
    auto l = os::join(dir, "link.jsonl");
    if (os::fs::symlink(f, l)) CHECK(!sec::open_audit_file(l)); else std::printf("  skip symlink case\n");
    CHECK(!sec::open_audit_file(dir));
#endif
    sec::open_audit_file("");
    os::remove_all(dir);
}

// ---------------------------------------------------------------- check / lockdown
TEST(check_report_shape) {
    for (auto pr : {sec::profile::none, sec::profile::strict, sec::profile::standard, sec::profile::permissive}) {
        auto p = sec::profile_defaults(pr); p.audit.os = false;
        auto r = sec::check(p);
        CHECK(!r.applied && r.profile == sec::name(pr));
        auto j = json::parse(r.json());
        for (auto* k : {"profile", "source", "platform", "applied", "ok", "controls", "policy"}) CHECK_MSG(j.find(k), "missing %s", k);
        CHECK(j["controls"].arr().size() == r.controls.size() && r.controls.size() > 15);
        for (auto& c : j["controls"].arr()) {
            for (auto* k : {"id", "name", "state", "backend", "level", "verdict", "detail"}) CHECK_MSG(c.find(k), "control missing %s", k);
            auto st = c["state"].str();
            CHECK_MSG(st == "active" || st == "available" || st == "emulated" || st == "missing" || st == "n/a" || st == "failed", "state %s", st.c_str());
            auto v = c["verdict"].str(); CHECK(v == "ok" || v == "warn" || v == "fail");
        }
        bool any_fail = false; for (auto& c : r.controls) if (c.verdict == "fail") any_fail = true;
        CHECK(r.ok == !any_fail);
        CHECK(r.text().find("verdict:") != std::string::npos);
        if (pr == sec::profile::none) CHECK(r.ok);
    }
    auto p = sec::profile_defaults(sec::profile::strict); p.audit.os = false;
    p.emulation = sec::emulation_mode::deny;
    auto r = sec::check(p);
    auto* fr = r.find("fs: read confinement");
    CHECK(fr && fr->state == "emulated" && fr->verdict == "fail" && !r.ok);
    auto* sb = r.find("no root / Administrator");
    CHECK(sb && (os::is_elevated() ? sb->state == "failed" : sb->state == "available"));
    auto* bl = r.find("build hardening");
    CHECK(bl && !bl->backend.empty());
}

TEST(lockdown_child_applies_policy) {
    auto dir = scratch("lock");
    int rc = run_child("lockdown", dir);
    CHECK_MSG(rc == 0, "lockdown child exit %d", rc);
    auto a = os::read_file(os::join(dir, "audit.jsonl"));
    CHECK_MSG(a && a.value.find("\"control\":\"SBX-1\"") != std::string::npos && a.value.find("\"decision\":\"deny\"") != std::string::npos,
              "audit file: %s", a ? a.value.c_str() : "(missing)");
    os::remove_all(dir);
}

TEST(lockdown_refuses_before_applying) {
    auto dir = scratch("refuse");
    int rc = run_child("refuse", dir);
    CHECK_MSG(rc == 0, "refuse child exit %d", rc);
    os::remove_all(dir);
}

TEST(lockdown_sets_model_integrity) {
    int rc = run_child("models", "");
    CHECK_MSG(rc == 0, "models child exit %d", rc);
}

// ---------------------------------------------------------------- CRY-2: secure_buffer
static std::atomic<int> g_released{-1};   // OR of every byte handed to the release hook (after the wipe); -1 = not called
static void on_release(const std::uint8_t* p, std::size_t n) { int v = 0; for (std::size_t i = 0; i < n; ++i) v |= p[i]; g_released = v; }

TEST(secure_buffer_wipes_and_reports_state) {
    static_assert(!std::is_copy_constructible_v<sec::secure_buffer> && std::is_nothrow_move_constructible_v<sec::secure_buffer>);
    static_assert(!std::is_copy_constructible_v<sec::secure_bytes<32>>);
    os::locked_buffer::set_release_hook(&on_release);
    {
        sec::secure_buffer b(64);
        CHECK(b.size() == 64 && b.data() != nullptr);
        bool zero = true; for (std::size_t i = 0; i < b.size(); ++i) zero = zero && b[i] == 0; CHECK(zero);
        std::memset(b.data(), 0xA5, b.size());
        std::printf("  secure_buffer: %s\n", sec::memory_state(b).c_str());
#if defined(__linux__)
        CHECK(b.dump_excluded());   // MADV_DONTDUMP
        struct rlimit ml{};
        if (::getrlimit(RLIMIT_MEMLOCK, &ml) == 0 && (ml.rlim_cur == RLIM_INFINITY || ml.rlim_cur >= 64 * 1024)) CHECK(b.locked());
#elif defined(_WIN32)
        CHECK(b.locked());          // VirtualLock of one page fits the default working set
#endif
        g_released = -1;
    }
    CHECK_MSG(g_released == 0, "released bytes not wiped (%d)", g_released.load());
    {   // a move leaves the source empty; the moved-to buffer is wiped when it goes
        sec::secure_buffer a(16); std::memset(a.data(), 0x5A, 16);
        sec::secure_buffer b(std::move(a));
        CHECK(a.empty() && a.data() == nullptr && b[0] == 0x5A);
        g_released = -1;
        sec::secure_buffer c(8); std::memset(c.data(), 0x33, 8);
        c = std::move(b);           // c's old bytes are wiped first
        CHECK(g_released == 0 && c.size() == 16 && c[15] == 0x5A);
        g_released = -1;
    }
    CHECK(g_released == 0);
    {
        sec::secure_buffer g(8, true);
        std::memset(g.data(), 1, 8);
        std::printf("  guarded: %s\n", sec::memory_state(g).c_str());
        CHECK(g.guarded());
    }
    {
        sec::secure_bytes<32> k; CHECK(k.size() == 32 && k[0] == 0);
        k[0] = 7; auto c = k.clone(); CHECK(c[0] == 7 && c.data() != k.data());
        sec::secure_string s = "tok", t = s;
        CHECK(t == "tok" && s.view() == "tok" && t.data() != s.data());
        s = std::string("x"); CHECK(s == "x" && t == "tok");
        sec::secure_string e; CHECK(e.empty() && e.view().empty());
        std::string_view v = t; CHECK(v == "tok");
    }
    {   // keys loaded for signing / rpc live in it
        std::uint8_t seed[32] = {9};
        auto kp = modelfile::keypair_from_seed(seed);
        CHECK(kp.seed[0] == 9 && kp.seed.buffer().size() == 32);
        auto id = graph::rpc::identity_from_seed_hex(graph::rpc::sec::hex(seed, 32));
        CHECK(id.seed[0] == 9 && std::memcmp(id.pub.data(), kp.pub.data(), 32) == 0);
        graph::rpc::token_entry te("s3cret"); CHECK(te.token == "s3cret");
    }
    os::locked_buffer::set_release_hook(nullptr);
}

// ---------------------------------------------------------------- FIO-1 / FIO-2
#if defined(_WIN32)
// Adds an access-allowed ACE for Everyone (read) to `path`'s DACL.
static bool grant_everyone(const std::string& path) {
    auto w = os::widen(path);
    PACL old = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(w.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old, nullptr, &sd)) return false;
    BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD n = sizeof sid;
    CreateWellKnownSid(WinWorldSid, nullptr, sid, &n);
    EXPLICIT_ACCESSW ea{}; ea.grfAccessPermissions = GENERIC_READ; ea.grfAccessMode = GRANT_ACCESS; ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID; ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP; ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    PACL nw = nullptr;
    bool ok = SetEntriesInAclW(1, &ea, old, &nw) == ERROR_SUCCESS &&
              SetNamedSecurityInfoW(w.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nw, nullptr) == ERROR_SUCCESS;
    if (nw) LocalFree(nw);
    LocalFree(sd); return ok;
}
#endif

TEST(temp_files_are_private) {
    auto d = os::fs::make_temp_dir("forge_sec_tmp_");
    CHECK(d);
    if (!d) return;
    CHECK_MSG(sec::check_private_dir(d.value, sec::file_kind::secret), "%s", sec::check_private_dir(d.value, sec::file_kind::secret).err.what.c_str());
    auto f = os::fs::make_temp_file("k", ".bin", d.value);
    CHECK(f);
    if (f) CHECK_MSG(sec::check_private_file(f.value), "%s", sec::check_private_file(f.value).err.what.c_str());
    auto g = os::join(d.value, "spill.bin");
    CHECK(os::fs::write_new_private_file(g, "abc"));
    CHECK(sec::check_private_file(g));
    CHECK(!os::fs::write_new_private_file(g, "again"));   // exclusive: never an existing file
#if !defined(_WIN32)
    auto l = os::join(d.value, "link.bin");
    CHECK(::symlink(g.c_str(), l.c_str()) == 0);
    CHECK(!os::fs::write_new_private_file(l, "x"));      // never through a symlink
#endif
    os::remove_all(d.value);
}

TEST(check_private_file_rules) {
    auto d = scratch("priv");
    auto f = os::join(d, "k.key");
#if !defined(_WIN32)
    os::write_file(f, "secret");
    ::chmod(f.c_str(), 0644);
    auto s = sec::check_private_file(f);
    CHECK_MSG(!s && s.err.what.find("0644") != std::string::npos, "%s", s.err.what.c_str());
    CHECK(sec::check_private_file(f, sec::file_kind::trusted));        // readable by others is fine for a trusted file
    ::chmod(f.c_str(), 0600);
    CHECK(sec::check_private_file(f));
    ::chmod(f.c_str(), 0620);
    CHECK(!sec::check_private_file(f, sec::file_kind::trusted));       // group-writable
    ::chmod(f.c_str(), 0600);
    auto l = os::join(d, "link.key");
    CHECK(::symlink(f.c_str(), l.c_str()) == 0);
    s = sec::check_private_file(l);
    CHECK_MSG(!s && s.err.what.find("symbolic link") != std::string::npos, "%s", s.err.what.c_str());
    CHECK(!sec::check_private_file(l, sec::file_kind::trusted));
    CHECK(!sec::check_private_file(d));                                  // a directory is not a regular file
    CHECK(sec::check_private_dir(d, sec::file_kind::secret));            // scratch() made it 0700
    ::chmod(d.c_str(), 0777);
    CHECK(!sec::check_private_dir(d));
    ::chmod(d.c_str(), 0700);
    if (::geteuid() == 0) {
        CHECK(::chown(f.c_str(), 65534, 65534) == 0);
        CHECK(!sec::check_private_file(f));
        CHECK(!sec::check_private_file(f, sec::file_kind::trusted));
    } else std::printf("  (other-owner case skipped: needs root to chown)\n");
#else
    CHECK(os::fs::write_new_private_file(f, "secret"));
    auto s = sec::check_private_file(f);
    CHECK_MSG(s, "%s", s.err.what.c_str());
    CHECK(grant_everyone(f));
    s = sec::check_private_file(f);
    CHECK_MSG(!s && s.err.what.find("S-1-1-0") != std::string::npos, "%s", s.err.what.c_str());
    CHECK(sec::check_private_file(f, sec::file_kind::trusted));        // Everyone may read, not write
    CHECK(!sec::check_private_file(d));
#endif
    os::remove_all(d);
}

TEST(strict_policy_refuses_world_readable_key) {
    auto d = scratch("fio2");
    auto k = os::join(d, "id.key");
    const std::string hex = "0909090909090909090909090909090909090909090909090909090909090909";
#if !defined(_WIN32)
    os::write_file(k, hex + "\n");
    ::chmod(k.c_str(), 0644);
#else
    CHECK(os::fs::write_new_private_file(k, hex + "\n"));
    CHECK(grant_everyone(k));
#endif
    capture cap;
    auto strict = sec::profile_defaults(sec::profile::strict);
    auto m = throws_msg<sec::permission_denied>([&] { sec::require_private_file(strict, "--tls-key", k); });
    CHECK_MSG(m.size() > 1 && m[0] == '!' && m.find("FIO-2") != std::string::npos, "%s", m.c_str());
    CHECK(cap.has("fs", sec::decision::deny, "id.key"));
    auto standard = sec::profile_defaults(sec::profile::standard);
    CHECK(throws_msg<sec::permission_denied>([&] { sec::require_private_file(standard, "--tls-key", k); }).empty());
    CHECK(cap.has("fs", sec::decision::weaken, "id.key"));
    CHECK(throws_msg<sec::permission_denied>([&] { sec::require_private_file("--tls-key", k); }).empty());   // profile none: warn only
    // the forge sign / rpc key loaders under an active strict policy
    auto p = strict; p.audit.os = false; p.audit.file.clear();
    sec::set_active_policy(p);
    CHECK(throws_msg<sec::permission_denied>([&] { (void)modelfile::load_secret_key(k); })[0] == '!');
    CHECK(throws_msg<sec::permission_denied>([&] { (void)graph::rpc::read_key_file(k); })[0] == '!');
    sec::set_active_policy(sec::profile_defaults(sec::profile::none));
    CHECK(modelfile::load_secret_key(k).seed[0] == 9);   // profile none: loads (with a warning)
#if !defined(_WIN32)
    ::chmod(k.c_str(), 0600);
    sec::set_active_policy(p);
    CHECK(modelfile::load_secret_key(k).seed[0] == 9);
    sec::set_active_policy(sec::profile_defaults(sec::profile::none));
    // a strict policy file that others can write is not loaded
    auto pf = os::join(d, "forge-security.json");
    os::write_file(pf, R"({"profile":"strict"})");
    ::chmod(pf.c_str(), 0666);
    m = throws_msg<sec::policy_violation>([&] { sec::load_policy(pf); });
    CHECK_MSG(m.size() > 1 && m[0] == '!' && m.find("FIO-2") != std::string::npos, "%s", m.c_str());
    ::chmod(pf.c_str(), 0644);
    CHECK(sec::load_policy(pf).prof == sec::profile::strict);
    os::write_file(pf, R"({"profile":"standard"})");
    ::chmod(pf.c_str(), 0666);
    CHECK(sec::load_policy(pf).prof == sec::profile::standard);           // standard: used, audited as weaken
    CHECK(cap.has("policy", sec::decision::weaken, "forge-security.json"));
#endif
    os::remove_all(d);
}

// ---------------------------------------------------------------- CAP-1 routes: importers, .fpk reader, KV spill
TEST(enforce_routes_importers_reader_spill) {
    auto in = scratch("routes_in"), out = scratch("routes_out");
    auto p = sec::profile_defaults(sec::profile::strict);
    p.fs.read = {in}; p.fs.write = {in}; p.audit.os = false; p.audit.file.clear();
    os::write_file(os::join(in, "m.safetensors"), "x");
    os::write_file(os::join(out, "m.safetensors"), "x");
    os::write_file(os::join(out, "m.gguf"), "x");
    os::write_file(os::join(out, "m.fpk"), "x");
    sec::set_active_policy(p);
    auto denied = [](auto&& f) { auto m = throws_msg<sec::permission_denied>(f); return m.size() > 1 && m[0] == '!'; };
    CHECK(denied([&] { import::safetensors s(os::join(out, "m.safetensors")); }));
    CHECK(denied([&] { import::gguf_file g(os::join(out, "m.gguf")); }));
    CHECK(denied([&] { import::import_onnx(os::join(out, "m.onnx"), os::join(in, "o.fpk")); }));   // input outside
    CHECK(denied([&] { import::import_onnx(os::join(in, "m.onnx"), os::join(out, "o.fpk")); }));   // output outside
    CHECK(denied([&] { import::import_hf(out, os::join(in, "o.fpk")); }));
    CHECK(denied([&] { modelfile::reader r(os::join(out, "m.fpk")); }));
    // inside the roots the policy lets them through to their own errors (a 1-byte file is no safetensors)
    auto m = throws_msg<sec::permission_denied>([&] { import::safetensors s(os::join(in, "m.safetensors")); });
    CHECK_MSG(m.size() > 1 && m[0] == '?', "%s", m.c_str());
    // KV spill directory: fs_write, private
    CHECK_THROWS(mem::spill_store(std::filesystem::path(os::join(out, "spill"))));
    CHECK(!os::exists(os::join(out, "spill")));
    {
        mem::spill_store s(std::filesystem::path(os::join(in, "spill")));
        int v[4] = {1, 2, 3, 4};
        auto key = s.put(v, sizeof v); CHECK(key);
        auto dir = os::fs::u8str(s.dir());
        CHECK_MSG(sec::check_private_dir(dir, sec::file_kind::secret), "%s", sec::check_private_dir(dir, sec::file_kind::secret).err.what.c_str());
        auto f = os::join(dir, std::to_string(key) + ".bin");
        CHECK_MSG(sec::check_private_file(f), "%s", sec::check_private_file(f).err.what.c_str());
        auto back = s.take(key); CHECK(back && back->size() == sizeof v);
    }
    sec::set_active_policy(sec::profile_defaults(sec::profile::none));
    {   // profile none: unchanged (anywhere), still private
        mem::spill_store s(std::filesystem::path(os::join(out, "spill")));
        CHECK(s.put("ab", 2));
        CHECK(sec::check_private_dir(os::fs::u8str(s.dir()), sec::file_kind::secret));
    }
    {   // default directory: a fresh private one under temp, removed afterwards
        std::string dir;
        { mem::spill_store s; dir = os::fs::u8str(s.dir()); CHECK(sec::check_private_dir(dir, sec::file_kind::secret)); }
        CHECK(!os::exists(dir));
    }
    os::remove_all(in); os::remove_all(out);
}
