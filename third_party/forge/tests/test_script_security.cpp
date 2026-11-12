// forge::security wired into FORGE (docs/security-api.md "Integration"): the script runtime's scope hook
// (enforce_scope: fs paths, net hosts, exec, env, plugins), the SecurityError script classes, model integrity at
// load (INT-1 / INT-4) and the no-policy path. The policy is made active with set_active_policy for the in-process
// cases; the `lockdown` child (this binary re-run with FORGE_SCRIPT_SECURITY_CHILD) applies a real lockdown and runs
// a script under it. The forge / forge-serve / rpc-serve command lines are covered by tests/security_cli_test.py.
#include <forge/script/script.hpp>
#include <forge/security/security.hpp>
#include <forge/inference/inference.hpp>
#include <forge/modelfile/sign.hpp>
#include <forge/os/os.hpp>
#include <forge/os/process.hpp>
#include "check.hpp"
#include "tiny_model.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
using namespace forge;
namespace sec = forge::security;
namespace fs = std::filesystem;
TEST_PROCESS_WORKER(script::run_process_worker_if_requested);   // sandbox_escape.fs starts a ProcessPool

namespace {
std::string scratch(const char* tag) {
    auto d = os::join(os::temp_dir(), std::string("forge_ssec_") + tag + "_" + std::to_string(os::pid()));
    os::remove_all(d); os::mkdirs(d);
    return os::absolute(d);
}
std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
// "!<ErrorType>" appended on failure, like the golden tests
std::string run(const std::string& src, script::options o = {}, bool vm = true) {
    std::string out; o.use_vm = vm; o.jit_threshold = 1; o.timeout_ms = 20000;
    o.out = [&](std::string_view s) { out += s; };
    script::engine e(o);
    auto r = e.run(src, "<sec>");
    if (!r.ok) out += "!" + r.error_type + (r.error_type == "PermissionError" || r.error_type == "IntegrityError" ? "" : ": " + r.error);
    return out;
}
struct active {   // a policy active for the scope of one test; profile none again afterwards
    explicit active(sec::policy p) { sec::set_active_policy(std::move(p)); }
    ~active() { sec::set_active_policy(sec::profile_defaults(sec::profile::none)); }
};
struct capture {
    std::mutex m; std::vector<sec::audit_record> recs; int id = 0;
    capture() { id = sec::add_audit_sink([this](const sec::audit_record& r) { std::lock_guard g(m); recs.push_back(r); }); }
    ~capture() { sec::remove_audit_sink(id); }
    bool has(std::string_view cat, sec::decision d, std::string_view target_part, std::string_view control = {}) {
        std::lock_guard g(m);
        for (auto& r : recs) if (r.category == cat && r.d == d && r.target.find(target_part) != std::string::npos && (control.empty() || r.control == control)) return true;
        return false;
    }
};
std::string quoted(std::string s) { std::string o = "\""; for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o + "\""; }
sec::policy confined(const std::string& root) {   // strict, with fs confined to `root` (reads and writes)
    auto p = sec::profile_defaults(sec::profile::strict);
    p.fs.read = {root}; p.fs.write = {root};
    p.audit.os = false;
    return p;
}
} // namespace

// ---------------------------------------------------------------- child: a real lockdown, then a script
static int child_main(const std::string& dir) {
    auto p = confined(dir);
    p.integrity = {sec::requirement::off, sec::requirement::off, sec::requirement::off, {}, false};
    p.allow_root = os::is_elevated();   // CI containers may run as root
    try { sec::lockdown(p); } catch (const std::exception& e) { std::printf("lockdown threw: %s\n", e.what()); return 2; }
    script::options o; o.allow_net = false; o.allow_proc = false; o.allow_env = false; o.os_sandbox = script::options::os_sandbox_mode::off;
    auto outside = os::join(os::temp_dir(), "forge_ssec_outside_" + std::to_string(os::pid()));
    const std::string src = "fs.write(" + quoted(os::join(dir, "in.txt")) + ", \"x\"); std::println(\"{}\", fs.read(" + quoted(os::join(dir, "in.txt")) + "));\n"
                            "try { fs.write(" + quoted(outside) + ", \"y\"); std::println(\"wrote\"); } catch (const PermissionError& e) { std::println(\"denied\"); }\n";
    auto got = run(src, o);
    std::printf("%s", got.c_str());
    if (got != "x\ndenied\n") std::printf("--- source\n%s", src.c_str());
    if (os::exists(outside)) { os::remove_all(outside); return 3; }
    return got == "x\ndenied\n" ? 0 : 4;
}
// Run as a test (not from a static initializer: the script runtime's own statics must be initialised first). The
// parent re-runs this binary with FORGE_TEST_ONLY=zz_lockdown_child and FORGE_SCRIPT_SECURITY_CHILD=<dir>; in a
// normal run the variable is unset and this does nothing.
TEST(zz_lockdown_child) {
    auto d = os::getenv("FORGE_SCRIPT_SECURITY_CHILD"); if (d.empty()) return;
    os::unsetenv("FORGE_SCRIPT_SECURITY_CHILD");
    std::fflush(stdout);
    std::exit(child_main(d));
}

// ---------------------------------------------------------------- no policy: unchanged
TEST(no_policy_is_profile_none) {
    // the test runs without a policy file or FORGE_SECURITY_* in the environment (ctest's working directory has none)
    CHECK(!sec::active_policy()->active());
    capture cap;
    auto d = scratch("none");
    auto got = run("fs.write(" + quoted(os::join(d, "a.txt")) + ", \"1\"); std::println(\"{}\", fs.read(" + quoted(os::join(d, "a.txt")) + "));");
    CHECK_MSG(got == "1\n", "%s", got.c_str());
    CHECK(cap.recs.empty());   // profile none audits nothing
    os::remove_all(d);
}

TEST(goldens_same_with_permissive_policy) {
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator("tests/script")) if (e.path().extension() == ".fs") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    CHECK(files.size() >= 8);
    auto p = sec::profile_defaults(sec::profile::permissive); p.audit.os = false;
    for (auto& f : files) {
        auto src = slurp(f); auto name = f.filename().string();
        auto runf = [&](bool vm) {
            std::string out; script::options o; o.use_vm = vm; o.jit_threshold = 1; o.timeout_ms = 20000;
            o.out = [&](std::string_view s) { out += s; };
            script::engine e(o); auto r = e.run(src, name);
            if (!r.ok) out += "!" + r.error_type + "\n";
            return out;
        };
        auto plain = runf(true);
        std::string with;
        { active a(p); with = runf(true); }
        auto want = slurp(fs::path(f).replace_extension(".out")); std::erase(want, '\r');
        CHECK_MSG(plain == want, "%s: no-policy output differs from the golden", name.c_str());
        CHECK_MSG(with == plain, "%s: output differs under a permissive policy\n--- none\n%s--- permissive\n%s", name.c_str(), plain.c_str(), with.c_str());
    }
}

// ---------------------------------------------------------------- strict: path- and host-scoped
TEST(strict_policy_scopes_fs) {
    auto root = scratch("root"), other = scratch("other");
    capture cap;
    active a(confined(root));
    auto in = os::join(root, "in.txt"), out = os::join(other, "out.txt");
    for (bool vm : {false, true}) {
        CHECK_MSG(run("fs.write(" + quoted(in) + ", \"ok\"); std::println(\"{}\", fs.read(" + quoted(in) + "));", {}, vm) == "ok\n", "write inside the root");
        CHECK(run("fs.write(" + quoted(out) + ", \"no\");", {}, vm) == "!PermissionError");
        CHECK(run("fs.read(" + quoted(os::join(other, "x")) + ");", {}, vm) == "!PermissionError");
        CHECK(run("fs.list(" + quoted(other) + ");", {}, vm) == "!PermissionError");
        CHECK(run("fs.exists(" + quoted(out) + ");", {}, vm) == "!PermissionError");
        CHECK(run("fs.mkdir(" + quoted(os::join(root, "sub")) + "); std::println(\"{}\", fs.is_dir(" + quoted(os::join(root, "sub")) + "));", {}, vm) == "true\n");
        CHECK(run("fs.copy(" + quoted(in) + ", " + quoted(out) + ");", {}, vm) == "!PermissionError");
        CHECK(run("fs.rename(" + quoted(in) + ", " + quoted(out) + ");", {}, vm) == "!PermissionError");
        CHECK(run("auto f = fs.open(" + quoted(out) + ", \"w\");", {}, vm) == "!PermissionError");
        CHECK(run("auto f = fs.open(" + quoted(in) + ", \"r\"); std::println(\"{}\", f.read()); f.close();", {}, vm) == "ok\n");
        // "../" out of the root, and a glob whose literal prefix leaves the root
        CHECK(run("fs.read(" + quoted(os::join(root, "../" + fs::path(other).filename().string() + "/x")) + ");", {}, vm) == "!PermissionError");
        CHECK(run("fs.glob(" + quoted(other + "/*") + ");", {}, vm) == "!PermissionError");
        CHECK(run("try { fs.write(" + quoted(out) + ", \"no\"); } catch (const OSError& e) { std::println(\"oserror\"); }", {}, vm) == "oserror\n");
    }
    CHECK(!os::exists(out));
    CHECK(cap.has("fs", sec::decision::deny, out, "CAP-1"));
    os::remove_all(root); os::remove_all(other);
}

TEST(strict_policy_scopes_net_exec_env) {
    auto root = scratch("net");
    active a(confined(root));
    // net.mode none: connect denied by the policy even when the Caps flag is on (an embedder that keeps caps)
    CHECK(run("net.port_open(\"127.0.0.1\", 9, 100);") == "!PermissionError");
    CHECK(run("net.http(\"http://example.com/\");") == "!PermissionError");
    CHECK(run("net.resolve(\"example.com\");") == "!PermissionError");
    CHECK(run("forge::remote(\"127.0.0.1:9\", \"1;\");") == "!PermissionError");
    CHECK(run("run(\"echo hi\");") == "!PermissionError");
    CHECK(run("os.env(\"PATH\");") == "!PermissionError");
    CHECK(run("std::getenv(\"PATH\");") == "!PermissionError");
    os::remove_all(root);
}

TEST(hosts_policy_allows_listed_hosts) {
    auto p = sec::profile_defaults(sec::profile::standard); p.audit.os = false;
    p.net.hosts = {"127.0.0.1:9"};
    active a(p);
    // loopback is allowed in mode hosts: the call reaches the OS (port 9 is normally closed -> false, not an error)
    auto got = run("std::println(\"{}\", net.port_open(\"127.0.0.1\", 9, 200));");
    CHECK_MSG(got == "false\n" || got == "true\n", "%s", got.c_str());
    CHECK(run("net.port_open(\"192.0.2.1\", 80, 100);") == "!PermissionError");   // not listed
}

// ---------------------------------------------------------------- errors and plugins
TEST(security_error_script_classes) {
    script::options o; std::string out; o.out = [&](std::string_view s) { out += s; };
    script::engine e(o);
    e.define("integrity", 0, 0, [](const std::string&) -> std::string { throw sec::integrity_error("bad signature"); });
    e.define("policy", 0, 0, [](const std::string&) -> std::string { throw sec::policy_violation("bad policy"); });
    e.define("denied", 0, 0, [](const std::string&) -> std::string { throw sec::permission_denied("no"); });
    e.define("sandbox", 0, 0, [](const std::string&) -> std::string { throw sec::sandbox_error("cannot confine"); });
    auto r = e.run(R"(
try { integrity(); } catch (const IntegrityError& x) { std::println("IntegrityError"); }
try { integrity(); } catch (const SecurityError& x) { std::println("SecurityError"); }
try { policy(); } catch (const PolicyViolation& x) { std::println("PolicyViolation"); }
try { policy(); } catch (const OSError& x) { std::println("OSError"); }
try { denied(); } catch (const PermissionError& x) { std::println("PermissionError"); }
try { sandbox(); } catch (const SandboxViolationError& x) { std::println("SandboxViolationError"); }
)", "<classes>");
    CHECK_MSG(r.ok, "%s", r.error.c_str());
    CHECK_MSG(out == "IntegrityError\nSecurityError\nPolicyViolation\nOSError\nPermissionError\nSandboxViolationError\n", "%s", out.c_str());
}

#ifdef FORGE_TEST_PLUGIN
TEST(plugins_follow_integrity_plugins) {
    const char* plug = std::getenv("FORGE_TEST_PLUGIN_PATH"); if (!plug || !*plug) plug = FORGE_TEST_PLUGIN;
    const std::string src = "auto h = forge::plugin(" + quoted(plug) + "); std::println(\"loaded\");";
    capture cap;
    {
        auto p = sec::profile_defaults(sec::profile::permissive); p.audit.os = false;   // plugins optional: allowed, audited
        active a(p);
        CHECK_MSG(run(src) == "loaded\n", "%s", run(src).c_str());
        CHECK(cap.has("plugin", sec::decision::allow, "hello_plugin", "CAP-1"));
    }
    {
        auto p = sec::profile_defaults(sec::profile::standard); p.audit.os = false;   // plugins required: no .sig -> refused
        active a(p);
        CHECK(run(src) == "!IntegrityError");
        CHECK(cap.has("integrity", sec::decision::fail, "hello_plugin", "INT-4"));
        CHECK(!sec::try_enforce(sec::capability::plugin_load, plug));               // the gate alone refuses too (fail closed)
        CHECK(cap.has("plugin", sec::decision::deny, "hello_plugin", "CAP-1"));
    }
}
#endif

// ---------------------------------------------------------------- model integrity (INT-1 / INT-4)
TEST(model_integrity_under_policy) {
    auto d = scratch("model");
    auto plain = os::join(d, "plain.fpk"), signed_ = os::join(d, "signed.fpk"), other = os::join(d, "other.fpk"), bad = os::join(d, "tampered.fpk");
    forge_test::make_tiny_model(plain);
    auto k = modelfile::generate_keypair(), k2 = modelfile::generate_keypair();
    modelfile::sign_file(plain, signed_, k);
    modelfile::sign_file(plain, other, k2);
    {   // tampered: flip one byte of tensor data after signing
        auto bytes = slurp(signed_);
        bytes[bytes.size() / 2] = char(bytes[bytes.size() / 2] ^ 0x5a);
        std::ofstream(bad, std::ios::binary) << bytes;
    }
    auto loads = [](const std::string& f) -> std::string {
        try { inference::model m(f); return "ok"; }
        catch (const sec::integrity_error&) { return "integrity"; }
        catch (const std::exception& e) { return std::string("other: ") + e.what(); }
    };
    // no policy: unsigned and tampered files load as before (no verification requested)
    CHECK(loads(plain) == "ok");
    CHECK(loads(signed_) == "ok");
    capture cap;
    auto p = sec::profile_defaults(sec::profile::permissive); p.audit.os = false;
    p.integrity.models = sec::requirement::required; p.integrity.trusted_keys = {modelfile::detail::to_hex(k.pub)};
    {
        active a(p);
        CHECK(loads(signed_) == "ok");
        CHECK(loads(plain) == "integrity");
        CHECK(loads(bad) == "integrity");
        CHECK(loads(other) == "integrity");   // valid signature, untrusted key
        CHECK(cap.has("integrity", sec::decision::fail, plain, "INT-4"));
        CHECK(cap.has("integrity", sec::decision::allow, signed_, "INT-1"));
    }
    p.integrity.models = sec::requirement::optional; p.integrity.trusted_keys.clear();
    {
        active a(p);
        CHECK(loads(plain) == "ok");          // unsigned: loaded, audited
        CHECK(cap.has("integrity", sec::decision::weaken, plain, "INT-1"));
        CHECK(loads(other) == "ok");          // signed, any valid signature (no trusted_keys)
        CHECK(loads(bad) == "integrity");     // signed but modified
    }
    {   // fs.read scope on the model path
        auto q = sec::profile_defaults(sec::profile::strict); q.audit.os = false; q.integrity.models = sec::requirement::off;
        q.fs.read = {os::join(d, "nowhere")}; q.fs.write = {};
        active a(q);
        bool denied = false;
        try { inference::model m(plain); } catch (const sec::permission_denied&) { denied = true; }
        CHECK(denied);
    }
    os::remove_all(d);
}

// ---------------------------------------------------------------- lockdown + script in a child process
TEST(lockdown_child_runs_script_confined) {
    auto d = scratch("child");
    os::setenv("FORGE_SCRIPT_SECURITY_CHILD", d); os::setenv("FORGE_TEST_ONLY", "zz_lockdown_child");
    auto r = os::run({os::exe_path()}, true);
    os::unsetenv("FORGE_SCRIPT_SECURITY_CHILD"); os::unsetenv("FORGE_TEST_ONLY");
    CHECK_MSG(r && r->exit_code == 0, "child: %s", r ? (std::to_string(r->exit_code) + ": " + r->out).c_str() : r.err.what.c_str());
    os::remove_all(d);
}
