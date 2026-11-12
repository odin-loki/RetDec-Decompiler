// Ed25519 detached signatures over bytes, and signed bundles (directory + manifest).
#include <forge/modelfile/bundle.hpp>
#include "check.hpp"
#include <filesystem>
#include <fstream>
#include <forge/os/os.hpp>
using namespace forge;
namespace sfs = std::filesystem;

namespace {
modelfile::keypair key(std::uint8_t b) { std::array<std::uint8_t, 32> s{}; s.fill(b); return modelfile::keypair_from_seed(s); }
std::string hexpub(const modelfile::keypair& k) { return modelfile::detail::to_hex(k.pub); }
void put(const sfs::path& p, const std::string& s) { sfs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
sfs::path fresh(const char* tag) {
    auto d = sfs::temp_directory_path() / ("forge_bundle_" + std::string(tag) + "_" + std::to_string(forge::os::pid()));
    sfs::remove_all(d); sfs::create_directories(d);
    put(d / "result.json", "{\"verdict\":\"true\"}"); put(d / "proofs/a.cert", "cert-a"); put(d / "proofs/b.cert", std::string(200000, 'x'));
    return d;
}
}

TEST(sign_bytes_roundtrip_and_context) {
    auto k = key(1), other = key(2);
    auto sig = modelfile::sign_bytes("hello", k, "prism-cert-v1");
    CHECK(sig.size() == 128);
    CHECK(modelfile::verify_bytes("hello", sig, hexpub(k), "prism-cert-v1"));
    CHECK(!modelfile::verify_bytes("hellO", sig, hexpub(k), "prism-cert-v1"));
    CHECK(!modelfile::verify_bytes("hello", sig, hexpub(other), "prism-cert-v1"));
    CHECK(!modelfile::verify_bytes("hello", sig, hexpub(k), "other-context"));   // domain separation
    CHECK(!modelfile::verify_bytes("hello", "zz", hexpub(k), "prism-cert-v1"));  // malformed, not a throw
    CHECK(modelfile::sign_bytes("hello", k, "prism-cert-v1") == sig);            // Ed25519 is deterministic
}

TEST(bundle_sign_verify_ok) {
    auto d = fresh("ok"); auto k = key(3);
    modelfile::sign_bundle(d.string(), k);
    CHECK(sfs::exists(d / "FORGE-SIGNATURE.json"));
    auto r = modelfile::verify_bundle(d.string(), {hexpub(k)});
    CHECK_MSG(r.ok(), "%s", r.message.c_str());
    CHECK(r.valid_keys.size() == 1 && r.valid_keys[0] == modelfile::key_id(k.pub));
    auto any = modelfile::verify_bundle(d.string());   // integrity only
    CHECK(any.ok());
    auto untrusted = modelfile::verify_bundle(d.string(), {hexpub(key(9))});
    CHECK(!untrusted.ok() && untrusted.digest_ok && !untrusted.trusted);
    sfs::remove_all(d);
}

TEST(bundle_detects_modified_missing_extra) {
    auto d = fresh("tamper"); auto k = key(4);
    modelfile::sign_bundle(d.string(), k);
    put(d / "proofs/a.cert", "cert-A");
    auto r = modelfile::verify_bundle(d.string(), {hexpub(k)});
    CHECK(!r.ok() && r.modified.size() == 1 && r.modified[0] == "proofs/a.cert");
    put(d / "proofs/a.cert", "cert-a");
    CHECK(modelfile::verify_bundle(d.string(), {hexpub(k)}).ok());
    sfs::remove(d / "result.json");
    r = modelfile::verify_bundle(d.string(), {hexpub(k)});
    CHECK(!r.ok() && r.missing.size() == 1 && r.missing[0] == "result.json");
    put(d / "result.json", "{\"verdict\":\"true\"}");
    put(d / "sneaky.txt", "added later");
    r = modelfile::verify_bundle(d.string(), {hexpub(k)});
    CHECK(!r.ok() && r.extra.size() == 1 && r.extra[0] == "sneaky.txt");
    CHECK(modelfile::verify_bundle(d.string(), {hexpub(k)}, /*allow_extra=*/true).ok());
    sfs::remove_all(d);
}

TEST(bundle_manifest_tamper_and_unsafe_paths) {
    auto d = fresh("manifest"); auto k = key(5);
    modelfile::sign_bundle(d.string(), k);
    auto mp = d / "FORGE-SIGNATURE.json";
    std::string m; { std::ifstream f(mp); m.assign(std::istreambuf_iterator<char>(f), {}); }
    // Rewriting a hash in the manifest to match a tampered file breaks the signed digest.
    put(d / "proofs/a.cert", "evil");
    std::uint64_t sz = 0; auto h = modelfile::detail::to_hex(modelfile::detail::file_blake3(d / "proofs/a.cert", sz));
    auto fake = m; auto at = fake.find(modelfile::detail::to_hex(hash::blake3::of({reinterpret_cast<const std::uint8_t*>("cert-a"), 6})));
    CHECK(at != std::string::npos);
    fake.replace(at, 64, h);
    auto sp = fake.find("\"size\":6"); CHECK(sp != std::string::npos); fake.replace(sp, 8, "\"size\":4");
    put(mp, fake);
    auto r = modelfile::verify_bundle(d.string(), {hexpub(k)});
    CHECK(!r.ok() && !r.digest_ok);
    // A manifest naming a path outside the bundle is rejected outright.
    auto esc = m; auto p = esc.find("result.json"); esc.replace(p, 11, "../etc/pass");
    put(mp, esc);
    r = modelfile::verify_bundle(d.string(), {hexpub(k)});
    CHECK(!r.ok() && r.message.find("unsafe path") != std::string::npos);
    put(mp, "not json");
    CHECK(!modelfile::verify_bundle(d.string()).ok());
    sfs::remove(mp);
    r = modelfile::verify_bundle(d.string());
    CHECK(!r.signed_ && !r.ok());
    CHECK(!modelfile::detail::safe_rel_path("/abs") && !modelfile::detail::safe_rel_path("a/../b") && !modelfile::detail::safe_rel_path("a//b")
          && !modelfile::detail::safe_rel_path("c:\\x") && modelfile::detail::safe_rel_path("a/b.c"));
    sfs::remove_all(d);
}

TEST(bundle_multi_signer_and_explicit_files) {
    auto d = fresh("multi"); auto a = key(6), b = key(7);
    modelfile::sign_bundle(d.string(), a);
    modelfile::sign_bundle(d.string(), b);   // unchanged content: second signature is appended
    auto r = modelfile::verify_bundle(d.string(), {hexpub(b)});
    CHECK(r.ok() && r.valid_keys.size() == 2);
    CHECK(modelfile::verify_bundle(d.string(), {hexpub(a)}).ok());
    put(d / "result.json", "{\"verdict\":\"false\"}");
    modelfile::sign_bundle(d.string(), b);   // content changed: stale signature from a is dropped
    r = modelfile::verify_bundle(d.string(), {hexpub(a)});
    CHECK(!r.ok() && r.valid_keys.size() == 1);
    // Only selected files; the rest are allowed as extras.
    modelfile::sign_bundle(d.string(), a, {"result.json"});
    CHECK(modelfile::verify_bundle(d.string(), {hexpub(a)}, true).ok());
    CHECK(!modelfile::verify_bundle(d.string(), {hexpub(a)}, false).ok());
    CHECK_THROWS(modelfile::sign_bundle(d.string(), a, {"../outside"}));
    sfs::remove_all(d);
}
