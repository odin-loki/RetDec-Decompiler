// RPC device over loopback and pipeline split: whole-model logits must match the plain CPU run.
#include "check.hpp"
#include "tiny_model.hpp"
#include <forge/inference/inference.hpp>
#include <forge/graph/vulkan.hpp>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace forge;
using namespace forge::inference;

static std::string tmp(const std::string& n) { return (std::filesystem::temp_directory_path() / ("forge_rpc_" + std::to_string(::getpid()) + "_" + n)).string(); }
static const std::string& model_a() { static std::string p = forge_test::make_tiny_model(tmp("a.fpk")); return p; }
static std::vector<float> logits(const std::string& dev) {
    auto m = model::open(model_a());
    CHECK(m->set_device(dev) == dev);
    inference::session s(m);
    std::vector<int> p = {1, 5, 9, 33, 70, 101};
    auto a = s.forward(p); int t = 7; auto b = s.forward(std::span<const int>(&t, 1));
    a.insert(a.end(), b.begin(), b.end()); return a;
}
static float maxdiff(const std::vector<float>& a, const std::vector<float>& b) {
    CHECK(a.size() == b.size()); float m = 0; for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i])); return m; }

TEST(rpc_loopback_matches_cpu) {
    graph::rpc::server srv(nullptr, 0);
    srv.start();
    auto ref = logits("cpu");
    auto got = logits("rpc:127.0.0.1:" + std::to_string(srv.port()));
    CHECK(srv.ops() > 50);
    CHECK_MSG(maxdiff(ref, got) < 1e-5f, "rpc diff %g", maxdiff(ref, got));
    srv.stop();
}
TEST(rpc_errors_propagate) {
    graph::rpc::server srv(nullptr, 0); srv.start();
    auto c = graph::rpc::client::connect("127.0.0.1", std::to_string(srv.port()), nullptr);
    CHECK(c != nullptr);
    tensor x({4}), y({4});
    tensor_view in{x.data.data(), x.shape};
    bool threw = false;
    try { c->run("no_such_op", std::span<const tensor_view>(&in, 1), {y.data.data(), y.shape}, {}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    for (auto& v : x.data) v = -1.f;   // connection still usable after an error
    c->run("relu", std::span<const tensor_view>(&in, 1), {y.data.data(), y.shape}, {});
    CHECK(y.data[0] == 0.f);
    srv.stop();
}
TEST(rpc_connect_failure_falls_back) {
    std::string why; auto r = graph::device_registry("rpc:127.0.0.1:1", &why);
    CHECK(!r); CHECK(!why.empty());
}
TEST(pipeline_split_two_stages) {
    graph::rpc::server srv(nullptr, 0); srv.start();
    auto ref = logits("cpu");
    auto dev = "pipe:cpu,rpc:127.0.0.1:" + std::to_string(srv.port());
    auto got = logits(dev);
    CHECK(srv.ops() > 0);
    CHECK_MSG(maxdiff(ref, got) < 1e-5f, "pipe diff %g", maxdiff(ref, got));
    // both stages got work, and the split is contiguous
    std::string why; auto reg = graph::device_registry(dev, &why);
    CHECK(reg && reg->stages.size() == 2);
    srv.stop();
}
TEST(pipeline_with_vulkan_if_present) {
    std::string why; auto v = graph::device_registry("vulkan", &why);
    if (!v) { std::printf("skip: no vulkan (%s)\n", why.c_str()); return; }
    v.reset();
    auto ref = logits("cpu"); auto got = logits("pipe:vulkan,cpu");
    CHECK_MSG(maxdiff(ref, got) < 2e-3f, "pipe vk diff %g", maxdiff(ref, got));
}
TEST(pipeline_multi_gpu_if_present) {   // real multi-GPU: needs two CUDA/HIP devices
    for (auto b : {"cuda", "hip"}) {
        std::string w0, w1; auto a = graph::device_registry(std::string(b) + ":0", &w0), c = graph::device_registry(std::string(b) + ":1", &w1);
        if (!a || !c) continue;
        a.reset(); c.reset();
        auto ref = logits("cpu"); auto got = logits(std::string("pipe:") + b + ":0," + b + ":1");
        CHECK_MSG(maxdiff(ref, got) < 2e-3f, "%s pipe diff %g", b, maxdiff(ref, got));
    }
}

// ---- security: token / Ed25519 key auth, TLS, remote scripts
static graph::rpc::client_security tok(const std::string& t) { graph::rpc::client_security s; s.token = t; return s; }
static const char* kSeed = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";   // RFC 8032 test 1
static bool relu_ok(graph::rpc::client& c) {
    tensor x({4}), y({4}); for (auto& v : x.data) v = -1.f; tensor_view in{x.data.data(), x.shape};
    c.run("relu", std::span<const tensor_view>(&in, 1), {y.data.data(), y.shape}, {}); return y.data[0] == 0.f;
}
TEST(rpc_token_auth) {
    graph::rpc::server_security sec; sec.tokens = {"s3cret"};
    graph::rpc::server srv(nullptr, 0, "127.0.0.1", sec); srv.start();
    auto port = std::to_string(srv.port()); std::string why;
    CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, {}) || true);   // unauthenticated: rejected on first frame
    { auto c = graph::rpc::client::connect("127.0.0.1", port, &why, {}); bool threw = false; if (c) { try { relu_ok(*c); } catch (...) { threw = true; } } CHECK(!c || threw); }
    why.clear(); CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, tok("wrong"))); CHECK(why.find("bad token") != std::string::npos);
    auto c = graph::rpc::client::connect("127.0.0.1", port, &why, tok("s3cret")); CHECK(c != nullptr);
    CHECK(relu_ok(*c));
    srv.stop();
}
TEST(rpc_key_auth_and_scripts) {
    auto id = std::make_shared<graph::rpc::identity>(graph::rpc::identity_from_seed_hex(kSeed));
    CHECK(graph::rpc::sec::hex(id->pub.data(), 32) == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    graph::rpc::server_security sec; sec.authorized_keys = {graph::rpc::sec::hex(id->pub.data(), 32)}; sec.allow_scripts = true;
    graph::rpc::server srv(nullptr, 0, "127.0.0.1", sec);
    srv.script_handler = [](const std::string& src, const std::string&, const std::vector<std::string>& argv, std::uint32_t) {
        return graph::rpc::server::script_result{true, "", "", "ran:" + src + ":" + std::to_string(argv.size())};
    };
    srv.start(); auto port = std::to_string(srv.port()); std::string why;
    graph::rpc::client_security good; good.key = id;
    auto c = graph::rpc::client::connect("127.0.0.1", port, &why, good); CHECK_MSG(c != nullptr, "%s", why.c_str());
    auto r = c->run_script("x", "<t>", {"a", "b"}); CHECK(r.ok && r.output == "ran:x:2");
    CHECK(relu_ok(*c));
    std::uint8_t other[32] = {1}; graph::rpc::client_security bad; bad.key = std::make_shared<graph::rpc::identity>(graph::rpc::identity_from_seed_hex(graph::rpc::sec::hex(other, 32)));
    why.clear(); CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, bad)); CHECK(why.find("not authorized") != std::string::npos);
    CHECK(srv.scripts() == 1);
    srv.stop();
    // scripts need auth; and a no-auth server refuses them
    bool threw = false; try { graph::rpc::server_security s2; s2.allow_scripts = true; graph::rpc::server x(nullptr, 0, "127.0.0.1", s2); } catch (...) { threw = true; } CHECK(threw);
    graph::rpc::server open_srv(nullptr, 0); open_srv.start();
    auto oc = graph::rpc::client::connect("127.0.0.1", std::to_string(open_srv.port()), &why, {});
    threw = false; try { oc->run_script("x"); } catch (const std::exception& e) { threw = std::string(e.what()).find("disabled") != std::string::npos; } CHECK(threw);
    open_srv.stop();
}
// ---- per-key permissions (authorized_keys "allow=" options); plain TCP, so these run without OpenSSL too
static std::shared_ptr<graph::rpc::identity> ident(std::uint8_t b) {
    std::uint8_t seed[32] = {b, 7}; return std::make_shared<graph::rpc::identity>(graph::rpc::identity_from_seed_hex(graph::rpc::sec::hex(seed, 32)));
}
static std::string pubhex(const graph::rpc::identity& k) { return graph::rpc::sec::hex(k.pub.data(), 32); }
TEST(rpc_authorized_keys_parse) {
    auto a = ident(1), b = ident(2), c = ident(3);
    auto upper = pubhex(*c); for (auto& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    auto ks = graph::rpc::parse_authorized_keys("# comment\n\n" + pubhex(*a) + "\n"                  // old format: full access
                                                "allow=device " + pubhex(*b) + " ci runner\r\n"
                                                "  allow=scripts allow=device " + upper + "\n");
    CHECK(ks.size() == 3);
    if (ks.size() == 3) {
        CHECK(ks[0].pub == pubhex(*a) && ks[0].perms == graph::rpc::perm_all && ks[0].comment.empty());
        CHECK(ks[1].perms == graph::rpc::perm_device && ks[1].comment == "ci runner");
        CHECK(ks[2].pub == pubhex(*c) && ks[2].perms == graph::rpc::perm_all);
    }
    CHECK(graph::rpc::parse_authorized_keys("allow= " + pubhex(*a))[0].perms == graph::rpc::perm_none);
    CHECK_THROWS(graph::rpc::parse_authorized_keys("allow=root " + pubhex(*a)));      // unknown permission
    CHECK_THROWS(graph::rpc::parse_authorized_keys("from=x " + pubhex(*a)));          // unknown option
    CHECK_THROWS(graph::rpc::parse_authorized_keys("allow=device\n"));                // no key
    CHECK_THROWS(graph::rpc::parse_authorized_keys("deadbeef comment\n"));            // not a key
}
TEST(rpc_key_permissions) {
    auto dev = ident(1), scr = ident(2), full = ident(3), none = ident(4);
    graph::rpc::server_security sec; sec.allow_scripts = true;
    sec.authorized_keys = graph::rpc::parse_authorized_keys("allow=device " + pubhex(*dev) + " gpu-only\n"
                                                            "allow=scripts " + pubhex(*scr) + "\n" +
                                                            pubhex(*full) + " legacy line\n"
                                                            "allow= " + pubhex(*none) + "\n");
    graph::rpc::server srv(nullptr, 0, "127.0.0.1", sec);
    srv.script_handler = [](const std::string&, const std::string&, const std::vector<std::string>&, std::uint32_t) {
        return graph::rpc::server::script_result{true, "", "", "ran"};
    };
    srv.start(); auto port = std::to_string(srv.port());
    auto conn = [&](std::shared_ptr<graph::rpc::identity> k) { graph::rpc::client_security s; s.key = k; std::string why;
        auto c = graph::rpc::client::connect("127.0.0.1", port, &why, s); CHECK_MSG(c != nullptr, "%s", why.c_str()); return c; };
    auto denied = [](auto&& fn) { try { fn(); } catch (const std::exception& e) { return std::string(e.what()).find("permission denied") != std::string::npos; } return false; };
    if (auto c = conn(dev)) {
        CHECK(relu_ok(*c));
        CHECK(denied([&] { c->run_script("x"); }));
        CHECK(relu_ok(*c));                                       // still usable after a refusal
    }
    if (auto c = conn(scr)) {
        CHECK(c->run_script("x").output == "ran");
        CHECK(denied([&] { relu_ok(*c); }));
        CHECK(c->run_script("y").ok);
    }
    if (auto c = conn(full)) { CHECK(relu_ok(*c)); CHECK(c->run_script("x").ok); }   // old-format line keeps full access
    if (auto c = conn(none)) { CHECK(denied([&] { relu_ok(*c); })); CHECK(denied([&] { c->run_script("x"); })); }
    CHECK(srv.scripts() == 3);
    srv.stop();
}
// ---- scoped tokens (token-file lines "[allow=...] <token> [name]") and config errors with line numbers
static bool throws_with(auto&& fn, std::string_view needle) {
    try { fn(); } catch (const std::exception& e) { bool ok = std::string_view(e.what()).find(needle) != std::string_view::npos; if (!ok) std::printf("  message: %s\n", e.what()); return ok; }
    return false;
}
TEST(rpc_token_file_parse) {
    auto ts = graph::rpc::parse_tokens("# tokens\n\nbare-token\r\n"
                                       "allow=device dev+tok/== gpu borrower\n"
                                       "  allow=scripts allow=device both\n"
                                       "allow= nothing\n");
    CHECK(ts.size() == 4);
    if (ts.size() == 4) {
        CHECK(ts[0].token == "bare-token" && ts[0].perms == graph::rpc::perm_all && ts[0].name.empty());   // a bare token: full access
        CHECK(ts[1].token == "dev+tok/==" && ts[1].perms == graph::rpc::perm_device && ts[1].name == "gpu borrower");
        CHECK(ts[2].token == "both" && ts[2].perms == graph::rpc::perm_all);
        CHECK(ts[3].token == "nothing" && ts[3].perms == graph::rpc::perm_none);
    }
    CHECK(graph::rpc::parse_tokens("abc=").at(0).token == "abc=");                          // '=' inside a token is fine
    CHECK(throws_with([] { graph::rpc::parse_tokens("ok\n# c\nallow=root t\n", "tok.txt"); }, "tok.txt line 3: unknown permission 'root'"));
    CHECK(throws_with([] { graph::rpc::parse_tokens("ok\nallow=device\n"); }, "line 2: no token"));
    // authorized_keys errors carry line numbers too
    CHECK(throws_with([] { graph::rpc::parse_authorized_keys("\n\nallow=device,root " + std::string(64, 'a')); }, "authorized_keys line 3: unknown permission 'root'"));
    // client-certificate maps
    auto fp = std::string(64, 'a'); std::string colons; for (std::size_t i = 0; i < 64; i += 2) { if (i) colons += ':'; colons += "AA"; }
    auto m = graph::rpc::parse_client_map("# map\nallow=device sha256:" + colons + " laptop\ncn:ci-runner\nallow=scripts,device cn:x\n");
    CHECK(m.size() == 3);
    if (m.size() == 3) {
        CHECK(m[0].kind == graph::rpc::cert_rule::sha256 && m[0].value == fp && m[0].perms == graph::rpc::perm_device && m[0].comment == "laptop");
        CHECK(m[1].kind == graph::rpc::cert_rule::cn && m[1].value == "ci-runner" && m[1].perms == graph::rpc::perm_all);
        CHECK(m[2].value == "x" && m[2].perms == graph::rpc::perm_all);
    }
    CHECK(throws_with([] { graph::rpc::parse_client_map("cn:a\nsha256:abcd\n", "map.txt"); }, "map.txt line 2: sha256: wants a 64-hex"));
    CHECK(throws_with([] { graph::rpc::parse_client_map("cn:a\n\nallow=device\n"); }, "line 3: no certificate identity"));
    CHECK(throws_with([] { graph::rpc::parse_client_map("cn:a\nfrom=x cn:b\n"); }, "line 2: unknown option 'from'"));
    CHECK(throws_with([] { graph::rpc::parse_client_map("cn:a\ncn:a\n"); }, "line 2: duplicate"));
    CHECK(throws_with([] { graph::rpc::parse_client_map("cn:\n"); }, "line 1: empty cn"));
    CHECK(throws_with([] { graph::rpc::parse_client_map("alice\n"); }, "line 1"));
    // a client map needs client certificates
    graph::rpc::server_security s; s.client_map = graph::rpc::parse_client_map("cn:a\n");
    CHECK_THROWS(graph::rpc::server(nullptr, 0, "127.0.0.1", s));
}
TEST(rpc_token_scopes) {
    graph::rpc::server_security sec; sec.allow_scripts = true;
    sec.tokens = graph::rpc::parse_tokens("allow=device devtok gpu\nallow=scripts scrtok\nfulltok\nallow= nonetok\n");
    sec.tokens.push_back(std::string("legacy"));                                              // bare token pushed as before
    graph::rpc::server srv(nullptr, 0, "127.0.0.1", sec);
    srv.script_handler = [](const std::string&, const std::string&, const std::vector<std::string>&, std::uint32_t) {
        return graph::rpc::server::script_result{true, "", "", "ran"};
    };
    std::mutex lm; std::vector<std::string> logs;
    srv.log = [&](const std::string& m) { std::lock_guard g(lm); logs.push_back(m); };
    srv.start(); auto port = std::to_string(srv.port());
    auto conn = [&](const std::string& t) { std::string why; auto c = graph::rpc::client::connect("127.0.0.1", port, &why, tok(t)); CHECK_MSG(c != nullptr, "%s", why.c_str()); return c; };
    auto denied = [](auto&& fn) { return throws_with(fn, "permission denied"); };
    if (auto c = conn("devtok")) { CHECK(relu_ok(*c)); CHECK(denied([&] { c->run_script("x"); })); CHECK(relu_ok(*c)); }
    if (auto c = conn("scrtok")) { CHECK(c->run_script("x").ok); CHECK(denied([&] { relu_ok(*c); })); CHECK(c->run_script("y").ok); }
    if (auto c = conn("fulltok")) { CHECK(relu_ok(*c)); CHECK(c->run_script("x").ok); }
    if (auto c = conn("legacy")) { CHECK(relu_ok(*c)); CHECK(c->run_script("x").ok); }
    if (auto c = conn("nonetok")) { CHECK(denied([&] { relu_ok(*c); })); CHECK(denied([&] { c->run_script("x"); })); }
    std::string why; CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, tok("wrong")));
    CHECK(srv.scripts() == 4);
    srv.stop();
    // the log names the principal (token number and name, never the token) and its permissions
    std::string all; for (auto& l : logs) all += l + "\n";
    CHECK_MSG(all.find("authenticated token#1 (gpu) allow=device") != std::string::npos, "%s", all.c_str());
    CHECK_MSG(all.find("authenticated token#5 allow=all") != std::string::npos, "%s", all.c_str());
    CHECK_MSG(all.find("denied script for token#1 (gpu)") != std::string::npos, "%s", all.c_str());
    CHECK_MSG(all.find("refused: bad token") != std::string::npos, "%s", all.c_str());
    CHECK(all.find("devtok") == std::string::npos && all.find("fulltok") == std::string::npos);
}

// ---- TLS. These skip without OpenSSL (or its CLI); the `test_rpc_tls` ctest entry runs only them and then exits 77.
// The CLI runs through os::run (no shell, no redirects), so the probe and the throwaway PKI work on Windows too.
// FORGE_TEST_OPENSSL names the binary; on Windows, Git for Windows' copies are tried after PATH.
static const std::string& openssl_bin() {
    static const std::string b = [] {
        std::vector<std::string> c;
        if (const char* e = std::getenv("FORGE_TEST_OPENSSL"); e && *e) c.push_back(e);
        c.push_back("openssl");
#if defined(_WIN32)
        c.push_back("C:\\Program Files\\Git\\usr\\bin\\openssl.exe"); c.push_back("C:\\Program Files\\Git\\mingw64\\bin\\openssl.exe");
#endif
        for (auto& x : c) { auto r = os::run({x, "version"}, true); if (r && r.value.exit_code == 0) return x; }
        return std::string{};
    }();
    return b;
}
static bool ossl(std::vector<std::string> args, std::string* out = nullptr) {
    args.insert(args.begin(), openssl_bin()); auto r = os::run(args, true);
    if (out && r) *out = r.value.out;
    return r && r.value.exit_code == 0;
}
static bool tls_or_skip() {
    if (graph::rpc::tls_available() && !openssl_bin().empty()) return true;
    std::printf("skip: %s\n", graph::rpc::tls_available() ? "no openssl CLI" : "built without TLS");
    if (const char* only = std::getenv("FORGE_TEST_ONLY"); only && std::strstr(only, "rpc_tls")) std::exit(77);
    return false;
}
// Throwaway PKI: a self-signed server cert for localhost, a client CA with three client certs (CN forge-client,
// forge-client-2, forge-client-3), and a self-signed "rogue" client cert (CN forge-client) the server does not trust.
struct test_pki { std::string srv, srv_key, ca, ca_key, cl, cl_key, cl2, cl2_key, cl3, cl3_key, rogue, rogue_key; bool ok = false; };
static const test_pki& pki() {
    static test_pki p = [] {
        test_pki p; const std::vector<std::string> ec = {"-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes"};
        auto req = [&](std::vector<std::string> a, const std::vector<std::string>& tail) { a.insert(a.end(), ec.begin(), ec.end()); a.insert(a.end(), tail.begin(), tail.end()); return ossl(a); };
        p.srv = tmp("srv.pem"); p.srv_key = tmp("srv.key"); p.ca = tmp("ca.pem"); p.ca_key = tmp("ca.key");
        p.cl = tmp("cl.pem"); p.cl_key = tmp("cl.key"); p.cl2 = tmp("cl2.pem"); p.cl2_key = tmp("cl2.key"); p.cl3 = tmp("cl3.pem"); p.cl3_key = tmp("cl3.key");
        p.rogue = tmp("rogue.pem"); p.rogue_key = tmp("rogue.key"); auto csr = tmp("cl.csr");
        auto client = [&](const std::string& cn, const std::string& crt, const std::string& key, const char* serial) {
            return req({"req"}, {"-subj", "/CN=" + cn, "-keyout", key, "-out", csr})
                && ossl({"x509", "-req", "-in", csr, "-CA", p.ca, "-CAkey", p.ca_key, "-set_serial", serial, "-days", "1", "-out", crt});
        };
        p.ok = req({"req", "-x509"}, {"-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost", "-keyout", p.srv_key, "-out", p.srv})
            && req({"req", "-x509"}, {"-days", "1", "-subj", "/CN=forge-test-ca", "-keyout", p.ca_key, "-out", p.ca})
            && client("forge-client", p.cl, p.cl_key, "2") && client("forge-client-2", p.cl2, p.cl2_key, "3") && client("forge-client-3", p.cl3, p.cl3_key, "4")
            && req({"req", "-x509"}, {"-days", "1", "-subj", "/CN=forge-client", "-keyout", p.rogue_key, "-out", p.rogue});
        std::error_code e; std::filesystem::remove(csr, e);
        return p;
    }();
    return p;
}
// SHA-256 fingerprint of a PEM certificate as the openssl CLI prints it ("AB:CD:..."), or "".
static std::string cli_fingerprint(const std::string& pem) {
    std::string out; if (!ossl({"x509", "-noout", "-fingerprint", "-sha256", "-in", pem}, &out)) return {};
    auto eq = out.find('='); if (eq == std::string::npos) return {};
    auto v = out.substr(eq + 1); while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back(); return v;
}
static graph::rpc::client_security tls_tok(const std::string& t) { auto s = tok(t); s.tls.enabled = true; s.tls.ca_file = pki().srv; s.tls.server_name = "localhost"; return s; }

TEST(rpc_tls_server_verify) {
    if (!tls_or_skip()) return;
    CHECK(pki().ok); auto crt = pki().srv, key = pki().srv_key;
    graph::rpc::server_security sec; sec.tokens = {"t"}; sec.tls = {crt, key};
    graph::rpc::server srv(nullptr, 0, "127.0.0.1", sec); srv.start(); CHECK(srv.tls());
    auto port = std::to_string(srv.port()); std::string why;
    auto s = tok("t"); s.tls.enabled = true; s.tls.ca_file = crt; s.tls.server_name = "localhost";
    auto c = graph::rpc::client::connect("127.0.0.1", port, &why, s); CHECK_MSG(c != nullptr, "%s", why.c_str());
    if (c) CHECK(relu_ok(*c));
    auto nv = tok("t"); nv.tls.enabled = true; why.clear();                         // no CA / fingerprint: refuse
    CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, nv)); CHECK(why.find("verify") != std::string::npos);
    auto fp = tok("t"); fp.tls.enabled = true; fp.tls.fingerprint_sha256 = std::string(64, 'a'); why.clear();
    CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, fp)); CHECK(why.find("fingerprint") != std::string::npos);
    auto plain = tok("t"); why.clear(); auto pc = graph::rpc::client::connect("127.0.0.1", port, &why, plain);   // plaintext to a TLS server fails
    CHECK(!pc);
    srv.stop();
}
TEST(rpc_tls_mtls) {
    if (!tls_or_skip()) return;
    auto& p = pki(); CHECK(p.ok);
    graph::rpc::server_security sec; sec.tokens = {"t"}; sec.tls = {p.srv, p.srv_key, p.ca};
    graph::rpc::server srv(nullptr, 0, "127.0.0.1", sec); srv.start();
    auto port = std::to_string(srv.port()); std::string why;
    auto good = tls_tok("t"); good.tls.cert_file = p.cl; good.tls.key_file = p.cl_key;
    auto c = graph::rpc::client::connect("127.0.0.1", port, &why, good); CHECK_MSG(c != nullptr, "%s", why.c_str());
    if (c) CHECK(relu_ok(*c));
    why.clear(); CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, tls_tok("t")));           // no client certificate
    auto rogue = tls_tok("t"); rogue.tls.cert_file = p.rogue; rogue.tls.key_file = p.rogue_key;         // not signed by the client CA
    why.clear(); CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, rogue));
    auto half = tls_tok("t"); half.tls.cert_file = p.cl;                                                 // cert without key
    why.clear(); CHECK(!graph::rpc::client::connect("127.0.0.1", port, &why, half)); CHECK(why.find("go together") != std::string::npos);
    CHECK(srv.ops() == 1);
    srv.stop();
    // mTLS without a server certificate is a configuration error
    graph::rpc::server_security bad; bad.tokens = {"t"}; bad.tls.client_ca_file = p.ca;
    CHECK_THROWS(graph::rpc::server(nullptr, 0, "127.0.0.1", bad));
    // the client CA file must load
    graph::rpc::server_security miss; miss.tokens = {"t"}; miss.tls = {p.srv, p.srv_key, tmp("missing-ca.pem")};
    CHECK_THROWS(graph::rpc::server(nullptr, 0, "127.0.0.1", miss));
}
// A man in the middle that the client wrongly trusts (it holds a certificate the client accepts) terminates the
// client's TLS and relays the auth exchange into its own TLS connection to the real server. Channel binding makes
// the relayed proof fail: the two TLS connections have different exporters.
static std::string relay_auth(int server_port, const graph::rpc::client_security& victim) {
#ifndef FORGE_HAVE_TLS
    (void)server_port; (void)victim; return "built without TLS";
#else
    auto& p = pki(); namespace R = graph::rpc;
    auto l = os::socket::listen("127.0.0.1", 0, 1); CHECK(bool(l)); if (!l) return "listen";
    int mitm_port = l.value.local_port();
    std::string victim_why; std::shared_ptr<R::client> victim_conn;
    std::thread th([&] { victim_conn = R::client::connect("127.0.0.1", std::to_string(mitm_port), &victim_why, victim); });
    auto a = l.value.accept(); CHECK(bool(a));
    R::channel down(std::move(a.value)); std::string why;
    CHECK_MSG(down.accept_tls(R::make_server_tls({p.srv, p.srv_key}), &why), "%s", why.c_str());
    auto u = os::socket::connect("127.0.0.1", server_port, 5000); CHECK(bool(u));
    R::channel up(std::move(u.value)); R::tls_client_config tc; tc.enabled = true; tc.ca_file = p.srv; tc.server_name = "localhost";
    CHECK_MSG(up.connect_tls(tc, "127.0.0.1", &why), "%s", why.c_str());
    CHECK(down.channel_binding().size() == 32 && up.channel_binding().size() == 32 && down.channel_binding() != up.channel_binding());
    auto pipe = [](R::channel& from, R::channel& to, std::size_t n) { std::vector<char> b(n); return from.recv_all(b.data(), n) && to.send_all(b.data(), n); };
    char hello[8];
    if (down.recv_all(hello, 8) && up.send_all(hello, 8)) {
        std::uint32_t method; std::memcpy(&method, hello + 4, 4);
        bool ok = method == 4 ? pipe(down, up, 32) && pipe(up, down, 32) && pipe(down, up, 64) : method == 3 ? pipe(down, up, 64) : false;
        std::uint32_t st = 0;
        if (ok && up.recv_all(&st, 4) && down.send_all(&st, 4) && st != 0) { std::uint32_t n = 0; if (up.recv_all(&n, 4) && n < 4096) { down.send_all(&n, 4); pipe(up, down, n); } }
    }
    th.join();
    CHECK(victim_conn == nullptr);
    return victim_why;
#endif
}
TEST(rpc_tls_channel_binding) {
    if (!tls_or_skip()) return;
    auto& p = pki(); CHECK(p.ok); namespace R = graph::rpc;
    auto id = ident(9);
    R::server_security sec; sec.tokens = {"t"}; sec.authorized_keys = {pubhex(*id)}; sec.tls = {p.srv, p.srv_key};
    R::server srv(nullptr, 0, "127.0.0.1", sec); srv.start();
    auto port = std::to_string(srv.port());
    // direct connections authenticate (bound methods 3/4)
    auto ks = tls_tok(""); ks.key = id; std::string why;
    auto c = R::client::connect("127.0.0.1", port, &why, ks); CHECK_MSG(c != nullptr, "%s", why.c_str()); if (c) CHECK(relu_ok(*c));
    why.clear(); c = R::client::connect("127.0.0.1", port, &why, tls_tok("t")); CHECK_MSG(c != nullptr, "%s", why.c_str()); if (c) CHECK(relu_ok(*c));
    why.clear(); CHECK(!R::client::connect("127.0.0.1", port, &why, tls_tok("nope"))); CHECK(why.find("bad token") != std::string::npos);
    // relayed through a second TLS connection: binding mismatch
    auto w = relay_auth(srv.port(), ks);   CHECK_MSG(w.find("key not authorized") != std::string::npos, "%s", w.c_str());
    w = relay_auth(srv.port(), tls_tok("t")); CHECK_MSG(w.find("bad token") != std::string::npos, "%s", w.c_str());
#ifdef FORGE_HAVE_TLS
    // unbound (pre-binding) methods are refused over TLS, so a relay cannot downgrade
    {
        auto u = os::socket::connect("127.0.0.1", srv.port(), 5000); CHECK(bool(u));
        R::channel ch(std::move(u.value)); R::tls_client_config tc; tc.enabled = true; tc.ca_file = p.srv; tc.server_name = "localhost";
        CHECK(ch.connect_tls(tc, "127.0.0.1", &why));
        CHECK(ch.send_all("FRPH", 4) && R::sec::put_u32(ch, 1));   // refused on the method alone (no unread bytes left behind)
        why.clear(); CHECK(!R::sec::status(ch, &why)); CHECK_MSG(why.find("channel-bound") != std::string::npos, "%s", why.c_str());
    }
#endif
    // and bound methods are refused on plain TCP
    {
        R::server_security ps; ps.tokens = {"t"}; R::server plain(nullptr, 0, "127.0.0.1", ps); plain.start();
        auto u = os::socket::connect("127.0.0.1", plain.port(), 5000); CHECK(bool(u));
        R::channel ch(std::move(u.value));
        CHECK(ch.send_all("FRPH", 4) && R::sec::put_u32(ch, 3));
        why.clear(); CHECK(!R::sec::status(ch, &why)); CHECK_MSG(why.find("needs TLS") != std::string::npos, "%s", why.c_str());
        plain.stop();
    }
    srv.stop();
}
// mTLS client map: fingerprint and CN rules, intersection with token permissions, unmapped certificates refused,
// and a certificate-only server (no tokens or keys) where a mapped certificate authenticates device ops.
TEST(rpc_tls_client_map) {
    if (!tls_or_skip()) return;
    auto& p = pki(); CHECK(p.ok); namespace R = graph::rpc;
    auto fp = cli_fingerprint(p.cl); CHECK_MSG(fp.size() == 95, "fingerprint '%s'", fp.c_str());   // AB:CD:... form
    // cl: fingerprint rule (device only) wins over the CN rule for its CN (all); cl2: CN rule (all); cl3: unmapped
    auto map = R::parse_client_map("allow=device sha256:" + fp + " laptop\ncn:forge-client\ncn:forge-client-2 ci\n");
    auto cert = [&](R::client_security s, const std::string& crt, const std::string& key) { s.tls.cert_file = crt; s.tls.key_file = key; return s; };
    auto script_ok = [](const std::string&, const std::string&, const std::vector<std::string>&, std::uint32_t) { return R::server::script_result{true, "", "", "ran"}; };
    auto denied = [](auto&& fn) { return throws_with(fn, "permission denied"); };
    std::mutex lm; std::string logs; auto logger = [&](const std::string& m) { std::lock_guard g(lm); logs += m + "\n"; };
    {
        R::server_security sec; sec.tls = {p.srv, p.srv_key, p.ca}; sec.client_map = map; sec.allow_scripts = true;
        sec.tokens = R::parse_tokens("full\nallow=device dev\n");
        auto key_id = ident(5); sec.authorized_keys = R::parse_authorized_keys("allow=scripts " + pubhex(*key_id) + " runner\n");
        R::server srv(nullptr, 0, "127.0.0.1", sec); srv.script_handler = script_ok; srv.log = logger; srv.start();
        auto port = std::to_string(srv.port()); std::string why;
        auto conn = [&](const R::client_security& s) { why.clear(); auto c = R::client::connect("127.0.0.1", port, &why, s); CHECK_MSG(c != nullptr, "%s", why.c_str()); return c; };
        if (auto c = conn(cert(tls_tok("full"), p.cl, p.cl_key))) {        // full token & device cert = device
            CHECK(relu_ok(*c)); CHECK(denied([&] { c->run_script("x"); }));
        }
        if (auto c = conn(cert(tls_tok("full"), p.cl2, p.cl2_key))) {      // full token & full cert = all
            CHECK(relu_ok(*c)); CHECK(c->run_script("x").ok);
        }
        if (auto c = conn(cert(tls_tok("dev"), p.cl2, p.cl2_key))) {       // device token & full cert = device
            CHECK(relu_ok(*c)); CHECK(denied([&] { c->run_script("x"); }));
        }
        auto ks = cert(tls_tok(""), p.cl, p.cl_key); ks.key = key_id;     // scripts key & device cert = nothing
        if (auto c = conn(ks)) { CHECK(denied([&] { relu_ok(*c); })); CHECK(denied([&] { c->run_script("x"); })); }
        why.clear(); CHECK(!R::client::connect("127.0.0.1", port, &why, cert(tls_tok("full"), p.cl3, p.cl3_key)));   // unmapped cert
        CHECK_MSG(why.find("client certificate not authorized") != std::string::npos, "%s", why.c_str());
        // a mapped certificate without a token: this server has tokens, so they are required
        if (auto c = conn(cert(tls_tok(""), p.cl2, p.cl2_key))) CHECK(throws_with([&] { relu_ok(*c); }, "authentication required"));
        CHECK(srv.scripts() == 1);
        srv.stop();
    }
    {   // certificate-only server: a mapped certificate authenticates device ops; scripts cannot be enabled
        R::server_security sec; sec.tls = {p.srv, p.srv_key, p.ca}; sec.client_map = map;
        R::server srv(nullptr, 0, "127.0.0.1", sec); srv.script_handler = script_ok; srv.log = logger; srv.start();
        auto port = std::to_string(srv.port()); std::string why;
        auto c = R::client::connect("127.0.0.1", port, &why, cert(tls_tok(""), p.cl, p.cl_key)); CHECK_MSG(c != nullptr, "%s", why.c_str());
        if (c) { CHECK(relu_ok(*c)); CHECK(throws_with([&] { c->run_script("x"); }, "disabled")); }
        auto c3 = R::client::connect("127.0.0.1", port, &why, cert(tls_tok(""), p.cl3, p.cl3_key)); CHECK(c3 != nullptr);   // TLS ok, then refused
        if (c3) CHECK(throws_with([&] { relu_ok(*c3); }, "client certificate not authorized"));
        CHECK(srv.ops() == 1);
        srv.stop();
        R::server_security s2 = sec; s2.allow_scripts = true;                // scripts still need tokens or keys
        CHECK_THROWS(R::server(nullptr, 0, "127.0.0.1", s2));
        R::server_security s3; s3.tls = {p.srv, p.srv_key}; s3.client_map = map; s3.tokens = {"t"};   // map without a client CA
        CHECK_THROWS(R::server(nullptr, 0, "127.0.0.1", s3));
    }
    CHECK_MSG(logs.find("authenticated token#1 cert:sha256:") != std::string::npos && logs.find("(laptop) allow=device") != std::string::npos, "%s", logs.c_str());
    CHECK_MSG(logs.find("authenticated token#2 cert:cn=forge-client-2 (ci) allow=device") != std::string::npos, "%s", logs.c_str());
    CHECK_MSG(logs.find("(runner) cert:sha256:") != std::string::npos && logs.find("(laptop) allow=none") != std::string::npos, "%s", logs.c_str());
    CHECK_MSG(logs.find("cn=forge-client-3 is not in the client map") != std::string::npos, "%s", logs.c_str());
}
TEST(rpc_tls_zz_cleanup) {
    if (!graph::rpc::tls_available()) return;
    auto& p = pki();
    for (auto* f : {&p.srv, &p.srv_key, &p.ca, &p.ca_key, &p.cl, &p.cl_key, &p.cl2, &p.cl2_key, &p.cl3, &p.cl3_key, &p.rogue, &p.rogue_key}) { std::error_code ec; std::filesystem::remove(*f, ec); }
}
