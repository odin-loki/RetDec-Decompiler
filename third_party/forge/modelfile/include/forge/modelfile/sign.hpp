#pragma once
#include "forge/modelfile/entropy.hpp"
// .fpk signing (FORGE_PLAN Part E: "[SIGN] signatures over manifest + hashes").
//
// Ed25519 (RFC 8032) via vendored Monocypher -- no home-grown crypto. A signature covers a
// domain-separated BLAKE3 digest of the section table: for every section except "signature",
// in name order, its name, size and BLAKE3 hash. The "tensors" section carries per-tensor hashes
// and "data" is hashed whole, so the digest pins the manifest, blueprint, tokenizer and all weights.
// Verification recomputes every section hash from the bytes on disk; it never trusts the stored table.
//
//   "signature" section (JSON): {"alg":"ed25519","scheme":"forge-fpk-v1","digest":hex,
//                                "signatures":[{"key_id":hex8,"public_key":hex,"sig":hex}]}
#include <forge/os/os.hpp>
#include <forge/modelfile/fpk.hpp>
#include <forge/security/files.hpp>
#include <forge/security/secure_buffer.hpp>
#include <fstream>

extern "C" {
#include "monocypher-ed25519.h"
}

namespace forge::modelfile {

inline constexpr std::string_view sign_scheme = "forge-fpk-v1";

namespace detail {
inline std::string to_hex(std::span<const std::uint8_t> b) {
    static const char* d = "0123456789abcdef"; std::string s;
    for (auto c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}
inline std::vector<std::uint8_t> from_hex(std::string_view s) {
    auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    if (s.size() % 2) raise(errc::parse_error, "odd-length hex string");
    std::vector<std::uint8_t> o;
    for (std::size_t i = 0; i < s.size(); i += 2) {
        int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) raise(errc::parse_error, "bad hex string");
        o.push_back(std::uint8_t(a * 16 + b));
    }
    return o;
}
} // namespace detail

// seed: the Ed25519 secret, in locked, non-dumpable memory wiped on destruction (FSS-CRY-2). Move-only.
struct keypair { security::secure_bytes<32> seed; std::array<std::uint8_t, 32> pub{}; };

inline keypair keypair_from_seed(std::span<const std::uint8_t> seed) {
    if (seed.size() != 32) raise(errc::invalid_argument, "ed25519 seed must be 32 bytes");
    keypair k; std::copy(seed.begin(), seed.end(), k.seed.begin());
    std::uint8_t sk[64], s2[32]; std::memcpy(s2, k.seed.data(), 32);
    crypto_ed25519_key_pair(sk, k.pub.data(), s2);  // wipes s2
    crypto_wipe(sk, 64);
    return k;
}
inline keypair generate_keypair() {
    std::array<std::uint8_t, 32> seed{};
    if (forge::modelfile::os_entropy(seed.data(), seed.size()) != 0) raise(errc::io_error, "getrandom failed");
    auto k = keypair_from_seed(seed); crypto_wipe(seed.data(), 32); return k;
}
inline std::string key_id(std::span<const std::uint8_t> pub) {
    auto h = hash::blake3::of(pub); return detail::to_hex({h.data(), 4});
}

// Digest over (name, size, BLAKE3(bytes)) of every non-signature section, recomputed from the file.
inline hash::blake3::digest signing_digest(const reader& r) {
    hash::blake3 h;
    auto put = [&](const void* p, std::size_t n) { h.update(static_cast<const std::uint8_t*>(p), n); };
    put(sign_scheme.data(), sign_scheme.size());
    for (auto& [name, s] : r.sections()) {
        if (name == "signature") continue;
        auto d = hash::blake3::of(r.raw_section_bytes(name));
        std::uint64_t ln = name.size(), sz = s.size;
        put(&ln, 8); put(name.data(), name.size()); put(&sz, 8); put(d.data(), d.size());
    }
    return h.final();
}

inline std::string sign_digest_hex(const hash::blake3::digest& d, const keypair& k) {
    std::uint8_t sk[64], pub[32], seed[32], sig[64];
    std::memcpy(seed, k.seed.data(), 32);
    crypto_ed25519_key_pair(sk, pub, seed);
    crypto_ed25519_sign(sig, sk, d.data(), d.size());
    crypto_wipe(sk, 64);
    return detail::to_hex({sig, 64});
}

// Rewrites `in` to `out` with all sections copied verbatim plus a "signature" section.
// An existing signature is extended (multi-signer) when its digest still matches.
inline void sign_file(const std::string& in, const std::string& out, const keypair& k) {
    std::vector<std::pair<std::string, std::string>> secs;
    json::array sigs;
    std::string dig;
    {
        reader r(in);
        if (auto bad = r.verify(); !bad.empty()) raise(errc::format_error, "{}: refusing to sign a corrupt file ({})", in, bad.front());
        auto d = signing_digest(r); dig = detail::to_hex(d);
        if (r.has_section("signature")) {
            auto old = json::parse(r.section_text("signature"));
            if (old["digest"].str() == dig) for (auto& s : old["signatures"].arr()) if (s["public_key"].str() != detail::to_hex(k.pub)) sigs.push_back(s);
        }
        sigs.push_back(json::object{{"key_id", key_id(k.pub)}, {"public_key", detail::to_hex(k.pub)}, {"sig", sign_digest_hex(d, k)}});
        for (auto& [name, s] : r.sections()) if (name != "signature") { auto b = r.raw_section_bytes(name); secs.emplace_back(name, std::string(reinterpret_cast<const char*>(b.data()), b.size())); }
    }
    secs.emplace_back("signature", json::dump(json::object{{"alg", "ed25519"}, {"scheme", std::string(sign_scheme)}, {"digest", dig}, {"signatures", sigs}}));
    std::sort(secs.begin(), secs.end());
    // raw container write (same layout as writer::write, sections verbatim)
    std::vector<std::uint8_t> o(64, 0);
    struct ent { std::string name; std::uint64_t off, size; hash::blake3::digest h; };
    std::vector<ent> table;
    for (auto& [nm, s] : secs) {
        while (o.size() % alignment) o.push_back(0);
        table.push_back({nm, o.size(), s.size(), hash::blake3::of({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()})});
        o.insert(o.end(), s.begin(), s.end());
    }
    while (o.size() % alignment) o.push_back(0);
    std::uint64_t toff = o.size();
    for (auto& e : table) {
        std::uint8_t rec[96] = {};
        std::memcpy(rec, e.name.data(), e.name.size());
        std::memcpy(rec + 32, &e.off, 8); std::memcpy(rec + 40, &e.size, 8); std::memcpy(rec + 48, e.h.data(), 32);
        o.insert(o.end(), rec, rec + 96);
    }
    std::uint32_t n = std::uint32_t(table.size()); std::uint64_t fsz = o.size();
    std::memcpy(o.data(), magic, 8); std::memcpy(o.data() + 8, &format_version, 4); std::memcpy(o.data() + 12, &n, 4);
    std::memcpy(o.data() + 16, &toff, 8); std::memcpy(o.data() + 24, &fsz, 8);
    auto tmp = out + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); f.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size())); if (!f) raise(errc::io_error, "cannot write {}", tmp); }
    if (auto rs = forge::os::rename(tmp, out); !rs) { forge::os::remove(tmp); raise(errc::io_error, "cannot replace {}: {}", out, rs.err.what); }
}

struct signature_report {
    bool signed_ = false, digest_ok = false;
    std::vector<std::string> valid_keys, invalid_keys;  // key ids
    bool trusted = false;                               // a valid signature from a trusted key
    std::string message;
};

// `trusted` = hex public keys; empty means "any valid signature" (integrity only, not provenance).
inline signature_report verify_signature(const reader& r, const std::vector<std::string>& trusted = {}) {
    signature_report rep;
    if (!r.has_section("signature")) { rep.message = "unsigned"; return rep; }
    rep.signed_ = true;
    json::value s;
    try { s = json::parse(r.section_text("signature")); } catch (const std::exception& e) { rep.message = std::string("bad signature section: ") + e.what(); return rep; }
    if (s.str_or("alg", "") != "ed25519" || s.str_or("scheme", "") != sign_scheme) { rep.message = "unsupported signature scheme"; return rep; }
    auto d = signing_digest(r);
    rep.digest_ok = s.str_or("digest", "") == detail::to_hex(d);
    if (!rep.digest_ok) { rep.message = "content does not match signed digest (file modified after signing)"; return rep; }
    auto* arr = s.find("signatures");
    if (!arr || !arr->is_arr()) { rep.message = "no signatures"; return rep; }
    for (auto& e : arr->arr()) {
        std::vector<std::uint8_t> pub, sig;
        try { pub = detail::from_hex(e["public_key"].str()); sig = detail::from_hex(e["sig"].str()); } catch (...) { rep.invalid_keys.push_back("?"); continue; }
        if (pub.size() != 32 || sig.size() != 64) { rep.invalid_keys.push_back("?"); continue; }
        bool ok = crypto_ed25519_check(sig.data(), pub.data(), d.data(), d.size()) == 0;
        (ok ? rep.valid_keys : rep.invalid_keys).push_back(key_id(pub));
        if (ok && (trusted.empty() || std::find(trusted.begin(), trusted.end(), detail::to_hex(pub)) != trusted.end())) rep.trusted = true;
    }
    rep.message = rep.trusted ? "ok" : rep.valid_keys.empty() ? "no valid signature" : "valid signature but no trusted key";
    return rep;
}

// Key files: secret = 64 hex chars (seed) on one line; public = 64 hex chars.
// The secret key file must be private under a strict security policy (FSS-FIO-2: owner-only, ours, no symlink);
// other profiles audit a warning.
inline keypair load_secret_key(const std::string& path) {
    security::require_private_file("secret key", path, security::file_kind::secret);
    std::ifstream f(path); std::string h; f >> h;
    if (!f) { if (!h.empty()) crypto_wipe(h.data(), h.size()); raise(errc::io_error, "cannot read key {}", path); }
    auto b = detail::from_hex(h); crypto_wipe(h.data(), h.size());
    auto k = keypair_from_seed(b); crypto_wipe(b.data(), b.size()); return k;
}
inline std::string load_public_key(const std::string& path) {
    std::ifstream f(path); std::string h; f >> h;
    if (!f) raise(errc::io_error, "cannot read key {}", path);
    if (detail::from_hex(h).size() != 32) raise(errc::parse_error, "{}: not an ed25519 public key", path);
    return h;
}

} // namespace forge::modelfile
