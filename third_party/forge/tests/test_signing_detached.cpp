// FSS INT-2 detached signatures (security/integrity.hpp), the integrity.plugins / integrity.scripts decisions
// (verify_artefact, security::open_plugin, engine::run_file and imports) and INT-3 os_code_signature.
// The forge sign / verify / script command lines are covered by tests/security_cli_test.py.
#include <forge/script/script.hpp>
#include <forge/security/security.hpp>
#include <forge/modelfile/sign.hpp>
#include <forge/os/os.hpp>
#include "check.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
using namespace forge;
namespace sec = forge::security;
namespace fs = std::filesystem;
TEST_PROCESS_WORKER(script::run_process_worker_if_requested);

namespace {
std::string scratch(const char* tag) {
    auto d = os::join(os::temp_dir(), std::string("forge_dsig_") + tag + "_" + std::to_string(os::pid()));
    os::remove_all(d); os::mkdirs(d);
    return os::absolute(d);
}
std::span<const std::uint8_t> B(const std::string& s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }
std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
void put(const std::string& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }
std::string hexpub(const modelfile::keypair& k) { return modelfile::detail::to_hex(k.pub); }
void sign_file(const std::string& f, const modelfile::keypair& k, const std::string& as = {}) {
    put(sec::detached_sig_path(f), sec::detached_sign(B(slurp(f)), as.empty() ? sec::signed_name(f) : as, k.seed));
}
struct active {
    explicit active(sec::policy p) { sec::set_active_policy(std::move(p)); }
    ~active() { sec::set_active_policy(sec::profile_defaults(sec::profile::none)); }
};
struct capture {
    std::mutex m; std::vector<sec::audit_record> recs; int id = 0;
    capture() { id = sec::add_audit_sink([this](const sec::audit_record& r) { std::lock_guard g(m); recs.push_back(r); }); }
    ~capture() { sec::remove_audit_sink(id); }
    bool has(sec::decision d, std::string_view control, std::string_view target_part) {
        std::lock_guard g(m);
        for (auto& r : recs) if (r.d == d && r.control == control && r.target.find(target_part) != std::string::npos) return true;
        return false;
    }
};
sec::policy pol(sec::requirement plugins, sec::requirement scripts, std::vector<std::string> keys) {
    auto p = sec::profile_defaults(sec::profile::permissive);
    p.audit.os = false; p.integrity.plugins = plugins; p.integrity.scripts = scripts; p.integrity.trusted_keys = std::move(keys);
    p.integrity.os_code_signing = false;
    return p;
}
std::string quoted(std::string s) { std::string o = "\""; for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o + "\""; }
} // namespace

// ---------------------------------------------------------------- the format
TEST(detached_round_trip) {
    auto k = modelfile::generate_keypair(), k2 = modelfile::generate_keypair();
    const std::string data = "int main() { return 0; }\n";
    auto sig = sec::detached_sign(B(data), "a.fs", k.seed);
    auto j = json::parse(sig);
    CHECK(j["format"].str() == "forge-detached-sig" && j["version"].num() == 1 && j["file"].str() == "a.fs" && j["size"].num() == double(data.size()));
    using S = sec::detached_report::status;
    auto r = sec::detached_verify(B(data), "a.fs", sig, {hexpub(k)});
    CHECK_MSG(r.st == S::ok && r.trusted && r.valid_keys.size() == 1, "%s", r.message.c_str());
    CHECK(sec::detached_verify(B(data), "a.fs", sig).trusted);                         // no trusted keys: any valid signature
    auto up = hexpub(k); for (auto& c : up) c = char(std::toupper((unsigned char)c));
    CHECK(sec::detached_verify(B(data), "a.fs", sig, {up}).trusted);                   // hex case does not matter
    // wrong key
    r = sec::detached_verify(B(data), "a.fs", sig, {hexpub(k2)});
    CHECK(r.st == S::untrusted && !r.trusted && r.valid_keys.size() == 1);
    // tampered content (same length, and a different length)
    auto bad = data; bad[3] ^= 1;
    CHECK(sec::detached_verify(B(bad), "a.fs", sig, {hexpub(k)}).st == S::content_mismatch);
    CHECK(sec::detached_verify(B(data + " "), "a.fs", sig).st == S::content_mismatch);
    // a signature for another file name
    r = sec::detached_verify(B(data), "b.fs", sig, {hexpub(k)});
    CHECK(r.st == S::name_mismatch && !r.trusted);
    // a forged "file" field: the signature binds the name, so rewriting it breaks the signature
    auto forged = sig; forged.replace(forged.find("\"a.fs\""), 6, "\"b.fs\"");
    r = sec::detached_verify(B(data), "b.fs", forged, {hexpub(k)});
    CHECK(r.st == S::no_valid_signature && !r.trusted);
    // a flipped signature byte
    auto flip = sig; auto pos = flip.find("\"sig\":\"") + 7; flip[pos] = flip[pos] == '0' ? '1' : '0';
    CHECK(sec::detached_verify(B(data), "a.fs", flip).st == S::no_valid_signature);
    // malformed / unsigned
    CHECK(sec::detached_verify(B(data), "a.fs", "{").st == S::malformed);
    CHECK(sec::detached_verify(B(data), "a.fs", "{\"format\":\"other\"}").st == S::malformed);
    CHECK(sec::detached_verify(B(data), "a.fs", "").st == S::unsigned_);
    // a second signer extends the file; both verify, each key alone is trusted
    auto both = sec::detached_sign(B(data), "a.fs", k2.seed, sig);
    r = sec::detached_verify(B(data), "a.fs", both, {hexpub(k2)});
    CHECK(r.trusted && r.valid_keys.size() == 2);
    CHECK(sec::detached_verify(B(data), "a.fs", both, {hexpub(k)}).trusted);
    // re-signing with the same key replaces its entry; a .sig for other content is not extended
    CHECK(json::parse(sec::detached_sign(B(data), "a.fs", k2.seed, both))["signatures"].arr().size() == 2);
    CHECK(json::parse(sec::detached_sign(B(bad), "a.fs", k2.seed, both))["signatures"].arr().size() == 1);
#if defined(_WIN32) || defined(__APPLE__)
    CHECK(sec::detached_verify(B(data), "A.FS", sig).trusted);   // case-insensitive file systems
#else
    CHECK(sec::detached_verify(B(data), "A.FS", sig).st == S::name_mismatch);
#endif
}

TEST(detached_swap_two_signed_files) {
    // two files signed by the same key: putting b's bytes and .sig in place of a must fail (the name is bound)
    auto d = scratch("swap"); auto k = modelfile::generate_keypair();
    auto a = os::join(d, "a.fs"), b = os::join(d, "b.fs");
    put(a, "std::println(\"a\");\n"); put(b, "std::println(\"b\");\n");
    sign_file(a, k); sign_file(b, k);
    CHECK(sec::detached_verify_file(a, {hexpub(k)}).trusted && sec::detached_verify_file(b, {hexpub(k)}).trusted);
    put(a, slurp(b)); put(sec::detached_sig_path(a), slurp(sec::detached_sig_path(b)));
    auto r = sec::detached_verify_file(a, {hexpub(k)});
    CHECK_MSG(r.st == sec::detached_report::status::name_mismatch, "%s", r.message.c_str());
    os::remove_all(d);
}

// ---------------------------------------------------------------- the policy decision
TEST(verify_artefact_required_and_optional) {
    auto d = scratch("pol"); auto k = modelfile::generate_keypair(), k2 = modelfile::generate_keypair();
    auto f = os::join(d, "x.fs"); put(f, "1;\n");
    capture cap;
    auto check = [&](const sec::policy& p) -> std::string {
        try { auto b = slurp(f); sec::verify_artefact(p, sec::artefact::script, f, B(b)); return "ok"; }
        catch (const sec::integrity_error& e) { return std::string("integrity:") + e.control; }
    };
    auto req = pol(sec::requirement::off, sec::requirement::required, {hexpub(k)});
    auto opt = pol(sec::requirement::off, sec::requirement::optional, {hexpub(k)});
    CHECK(check(sec::profile_defaults(sec::profile::none)) == "ok");             // no policy: nothing
    CHECK(check(pol(sec::requirement::off, sec::requirement::off, {})) == "ok");  // off
    CHECK(check(req) == "integrity:INT-4");                                        // missing .sig, required
    CHECK(cap.has(sec::decision::fail, "INT-4", "x.fs"));
    CHECK(check(opt) == "ok");                                                     // missing .sig, optional
    CHECK(cap.has(sec::decision::weaken, "INT-1", "x.fs"));
    sign_file(f, k);
    CHECK(check(req) == "ok" && check(opt) == "ok");
    CHECK(cap.has(sec::decision::allow, "INT-2", "x.fs"));
    sign_file(f, k2);                                                              // signed, but not by a trusted key
    CHECK(check(req) == "integrity:INT-4" && check(opt) == "integrity:INT-4");
    CHECK(check(pol(sec::requirement::off, sec::requirement::required, {})) == "ok");   // empty list: any valid signature
    sign_file(f, k); put(f, "2;\n");                                               // tampered after signing
    CHECK(check(req) == "integrity:INT-4" && check(opt) == "integrity:INT-4");
    os::remove_all(d);
}

// ---------------------------------------------------------------- scripts (engine::run_file and imports)
TEST(script_file_under_required) {
    auto d = scratch("script"); auto k = modelfile::generate_keypair(), k2 = modelfile::generate_keypair();
    auto main_fs = os::join(d, "main.fs"), mod_fs = os::join(d, "util.fs");
    put(mod_fs, "int twice(int x) { return 2 * x; }\n");
    put(main_fs, "import \"util\" as u;\nstd::println(\"{}\", u.twice(21));\n");
    auto run = [&](const std::string& f) -> std::string {
        std::string out; script::options o; o.out = [&](std::string_view s) { out += s; }; o.timeout_ms = 20000;
        script::engine e(o);
        auto r = e.run_file(f);
        return r.ok ? out : "!" + r.error_type;
    };
    CHECK_MSG(run(main_fs) == "42\n", "%s", run(main_fs).c_str());   // no policy
    capture cap;
    {
        active a(pol(sec::requirement::off, sec::requirement::required, {hexpub(k)}));
        CHECK(run(main_fs) == "!IntegrityError");                     // unsigned main
        sign_file(main_fs, k);
        CHECK(run(main_fs) == "!IntegrityError");                     // signed main, unsigned import
        CHECK(cap.has(sec::decision::fail, "INT-4", "util.fs"));
        sign_file(mod_fs, k);
        CHECK_MSG(run(main_fs) == "42\n", "%s", run(main_fs).c_str());
        sign_file(mod_fs, k2);                                        // import signed by an untrusted key
        CHECK(run(main_fs) == "!IntegrityError");
        sign_file(mod_fs, k);
        put(main_fs, slurp(main_fs) + "\n");                          // main modified after signing
        CHECK(run(main_fs) == "!IntegrityError");
        sign_file(main_fs, k);
        CHECK(run(main_fs) == "42\n");
    }
    {
        os::remove(sec::detached_sig_path(mod_fs));
        active a(pol(sec::requirement::off, sec::requirement::optional, {hexpub(k)}));
        CHECK(run(main_fs) == "42\n");                                // optional: unsigned import runs, audited
        CHECK(cap.has(sec::decision::weaken, "INT-1", "util.fs"));
    }
    os::remove_all(d);
}

// ---------------------------------------------------------------- plugins (security::open_plugin, forge::plugin)
#ifdef FORGE_TEST_PLUGIN
TEST(plugin_load_under_policy) {
    const char* src_plug = std::getenv("FORGE_TEST_PLUGIN_PATH"); if (!src_plug || !*src_plug) src_plug = FORGE_TEST_PLUGIN;
    auto d = scratch("plug"); auto k = modelfile::generate_keypair(), k2 = modelfile::generate_keypair();
    const auto plug = os::join(d, fs::path(src_plug).filename().string());
    fs::copy_file(src_plug, plug, fs::copy_options::overwrite_existing);
    const std::string code = "auto h = forge::plugin(" + quoted(plug) + "); std::println(\"loaded {}\", h.greet(\"x\"));";
    auto run = [&]() -> std::string {
        std::string out; script::options o; o.out = [&](std::string_view s) { out += s; }; o.timeout_ms = 20000;
        script::engine e(o);
        auto r = e.run(code, "<plug>");
        return r.ok ? out : "!" + r.error_type + ": " + r.error;
    };
    auto expect = [&](const std::string& got, const std::string& want) { CHECK_MSG(got.rfind(want, 0) == 0, "want %s, got %s", want.c_str(), got.c_str()); };
    capture cap;
    {   // required, unsigned: refused; enforce(plugin_load) also refuses (fail closed for callers that only gate)
        active a(pol(sec::requirement::required, sec::requirement::off, {hexpub(k)}));
        expect(run(), "!IntegrityError");
        CHECK(cap.has(sec::decision::fail, "INT-4", "hello_plugin"));
        CHECK(!sec::try_enforce(sec::capability::plugin_load, plug));
    }
    sign_file(plug, k);
    {
        active a(pol(sec::requirement::required, sec::requirement::off, {hexpub(k)}));
        CHECK(sec::try_enforce(sec::capability::plugin_load, plug));
        auto lib = sec::open_plugin(plug);
        CHECK_MSG(lib && lib.value.symbol("forge_script_plugin_init"), "%s", lib ? "no symbol" : lib.err.what.c_str());
        CHECK(cap.has(sec::decision::allow, "INT-2", "hello_plugin"));
    }
    {   // a fresh engine: the verified plugin runs
        active a(pol(sec::requirement::required, sec::requirement::off, {hexpub(k)}));
        expect(run(), "loaded");
    }
    {   // signed by another key
        active a(pol(sec::requirement::required, sec::requirement::off, {hexpub(k2)}));
        CHECK_THROWS(sec::open_plugin(plug));
    }
    {   // a signature for another name: copy the plugin + its .sig under a different name
        auto other = os::join(d, "renamed_" + fs::path(plug).filename().string());
        fs::copy_file(plug, other); fs::copy_file(sec::detached_sig_path(plug), sec::detached_sig_path(other));
        active a(pol(sec::requirement::required, sec::requirement::off, {hexpub(k)}));
        bool threw = false; try { (void)sec::open_plugin(other); } catch (const sec::integrity_error& e) { threw = std::string(e.what()).find("not '") != std::string::npos; }
        CHECK(threw);
    }
    {   // tampered: one byte appended
        auto t = os::join(d, "t"); fs::create_directories(t);
        auto tp = os::join(t, fs::path(plug).filename().string());
        put(tp, slurp(plug) + std::string(1, '\0')); fs::copy_file(sec::detached_sig_path(plug), sec::detached_sig_path(tp));
        active a(pol(sec::requirement::required, sec::requirement::off, {hexpub(k)}));
        CHECK_THROWS(sec::open_plugin(tp));
    }
    {   // optional: unsigned loads, audited weaken
        auto u = os::join(d, "u"); fs::create_directories(u);
        auto up = os::join(u, fs::path(plug).filename().string()); fs::copy_file(src_plug, up);
        active a(pol(sec::requirement::optional, sec::requirement::off, {hexpub(k)}));
        auto lib = sec::open_plugin(up);
        CHECK(lib.err.what.empty() && lib.value.valid());
        CHECK(cap.has(sec::decision::weaken, "INT-1", up));
    }
    {   // os_code_signing: our test plugin is not OS-signed -> refused where the OS has a mechanism, audited n/a elsewhere
        auto p = pol(sec::requirement::required, sec::requirement::off, {hexpub(k)}); p.integrity.os_code_signing = true;
        active a(p);
#if defined(_WIN32) || defined(__APPLE__)
        bool threw = false; try { (void)sec::open_plugin(plug); } catch (const sec::integrity_error& e) { threw = e.control == "INT-3"; }
        CHECK(threw);
        CHECK(cap.has(sec::decision::fail, "INT-3", "hello_plugin"));
#else
        auto lib = sec::open_plugin(plug);
        CHECK(lib.err.what.empty() && lib.value.valid());
        CHECK(cap.has(sec::decision::weaken, "INT-3", "hello_plugin"));
#endif
    }
#if defined(_WIN32)
    {   // Windows: while another handle may write the file, a verified load is refused (no TOCTOU window)
        HANDLE w = CreateFileW(os::widen(plug).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(w != INVALID_HANDLE_VALUE);
        active a(pol(sec::requirement::required, sec::requirement::off, {hexpub(k)}));
        auto lib = sec::open_plugin(plug);
        CHECK_MSG(!lib && lib.err.what.find("open for writing") != std::string::npos, "%s", lib.err.what.c_str());
        CloseHandle(w);
    }
#endif
    os::remove_all(d);
}
#endif

// ---------------------------------------------------------------- INT-3: OS code signatures
TEST(os_code_signature_states) {
    using S = sec::code_signature::state;
    auto self = sec::os_code_signature(os::exe_path());
    std::fprintf(stderr, "  this test binary: %s (%s) %s\n", sec::name(self.st), self.backend.c_str(), self.detail.c_str());
#if defined(_WIN32)
    CHECK_MSG(self.st == S::unsigned_, "%s", self.detail.c_str());                  // our build is not Authenticode-signed
    auto win = os::getenv("SystemRoot"); if (win.empty()) win = "C:\\Windows";
    auto np = sec::os_code_signature(win + "\\System32\\notepad.exe");                // catalog-signed
    std::fprintf(stderr, "  notepad.exe: %s [%s] %s (%s)\n", sec::name(np.st), np.signer.c_str(), np.detail.c_str(), np.backend.c_str());
    CHECK(np.st == S::signed_ && np.signer.find("Microsoft") != std::string::npos);
    auto emb = win + "\\System32\\MpSigStub.exe";                                       // embedded Authenticode, when present
    if (os::stat(emb).exists) {
        auto e = sec::os_code_signature(emb);
        std::fprintf(stderr, "  MpSigStub.exe: %s [%s] %s\n", sec::name(e.st), e.signer.c_str(), e.backend.c_str());
        CHECK(e.st == S::signed_ && e.backend.find("embedded") != std::string::npos);
    }
    CHECK(sec::os_code_signature(win + "\\no-such-file.dll").st == S::error);
#elif defined(__APPLE__)
    CHECK(self.st == S::unsigned_);                                                    // unsigned or ad-hoc (linker) only
    auto ls = sec::os_code_signature("/bin/ls");
    std::fprintf(stderr, "  /bin/ls: %s [%s] %s\n", sec::name(ls.st), ls.signer.c_str(), ls.detail.c_str());
    CHECK(ls.st == S::signed_);
#else
    CHECK(self.st == S::not_applicable);
#endif
    // forge security check reports it
    auto p = sec::profile_defaults(sec::profile::strict); p.audit.os = false;
    auto r = sec::check(p);
    const sec::control* c = nullptr; for (auto& x : r.controls) if (x.id == "INT-3") c = &x;
    CHECK(c != nullptr);
    if (c) std::fprintf(stderr, "  check INT-3: %s %s %s | %s\n", c->state.c_str(), c->level.c_str(), c->verdict.c_str(), c->detail.c_str());
#if !defined(_WIN32) && !defined(__APPLE__)
    if (c) CHECK(c->state == "missing" && c->verdict == "ok");                         // n/a is reported, not failed
#else
    if (c) CHECK(c->verdict == "ok");
#endif
}
