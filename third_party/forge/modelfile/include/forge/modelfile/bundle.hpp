#pragma once
// Ed25519 signing for arbitrary artefacts: detached signatures over a byte string, and signed
// bundles -- a directory of files pinned by a manifest of (path, size, BLAKE3) entries.
// Same primitives and key files as .fpk signing (sign.hpp): Monocypher Ed25519 over a
// domain-separated BLAKE3 digest. Verification always re-hashes the files on disk.
//
//   manifest file (JSON, default "FORGE-SIGNATURE.json" in the bundle root):
//   {"alg":"ed25519","scheme":"forge-bundle-v1","files":[{"path":"a/b.txt","size":N,"blake3":hex}],
//    "digest":hex,"signatures":[{"key_id":hex8,"public_key":hex,"sig":hex}]}
//
//   auto k = modelfile::generate_keypair();
//   modelfile::sign_bundle("results/", k);                       // every regular file under results/
//   auto rep = modelfile::verify_bundle("results/", {modelfile::detail::to_hex(k.pub)});
//   if (!rep.ok()) ...                                     // rep.modified / missing / extra / message
#include <forge/modelfile/sign.hpp>
#include <forge/os/fs.hpp>
#include <filesystem>

namespace forge::modelfile {

inline constexpr std::string_view bundle_scheme = "forge-bundle-v1";
inline constexpr std::string_view bundle_manifest_name = "FORGE-SIGNATURE.json";

namespace detail {
inline void put_lp(hash::blake3& h, std::string_view s) { std::uint64_t n = s.size(); h.update(&n, 8); h.update(s.data(), s.size()); }
inline hash::blake3::digest file_blake3(const std::filesystem::path& p, std::uint64_t& size) {
    std::ifstream f(p, std::ios::binary);
    if (!f) raise(errc::io_error, "cannot read {}", p.string());
    hash::blake3 h; size = 0; std::vector<char> buf(1 << 16);
    while (f) { f.read(buf.data(), std::streamsize(buf.size())); auto n = std::size_t(f.gcount()); h.update(buf.data(), n); size += n; }
    return h.final();
}
// Manifest paths are relative, '/'-separated, and may not escape the root.
inline bool safe_rel_path(std::string_view p) {
    if (p.empty() || p.front() == '/' || p.find('\\') != p.npos || p.find(':') != p.npos) return false;
    std::size_t b = 0;
    while (b <= p.size()) {
        auto e = p.find('/', b); if (e == p.npos) e = p.size();
        auto seg = p.substr(b, e - b);
        if (seg.empty() || seg == "." || seg == "..") return false;
        b = e + 1;
    }
    return true;
}
} // namespace detail

// ---- detached signatures over bytes -------------------------------------------------------------
// `context` domain-separates uses (e.g. "prism-cert-v1"), so a signature for one purpose never
// verifies for another. Returns 128 hex chars.
inline std::string sign_bytes(std::span<const std::uint8_t> data, const keypair& k, std::string_view context = "forge-bytes-v1") {
    hash::blake3 h; detail::put_lp(h, context); h.update(data.data(), data.size());
    return sign_digest_hex(h.final(), k);
}
inline std::string sign_bytes(std::string_view data, const keypair& k, std::string_view context = "forge-bytes-v1") {
    return sign_bytes({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()}, k, context);
}
inline bool verify_bytes(std::span<const std::uint8_t> data, std::string_view sig_hex, std::string_view pub_hex, std::string_view context = "forge-bytes-v1") {
    std::vector<std::uint8_t> sig, pub;
    try { sig = detail::from_hex(sig_hex); pub = detail::from_hex(pub_hex); } catch (const forge_exception&) { return false; }
    if (sig.size() != 64 || pub.size() != 32) return false;
    hash::blake3 h; detail::put_lp(h, context); h.update(data.data(), data.size());
    auto d = h.final();
    return crypto_ed25519_check(sig.data(), pub.data(), d.data(), d.size()) == 0;
}
inline bool verify_bytes(std::string_view data, std::string_view sig_hex, std::string_view pub_hex, std::string_view context = "forge-bytes-v1") {
    return verify_bytes({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()}, sig_hex, pub_hex, context);
}

// ---- bundles ------------------------------------------------------------------------------------
struct bundle_entry { std::string path; std::uint64_t size = 0; std::string blake3; };

// Entries for `files` (relative to root), or every regular file under root when empty. The manifest
// file itself is always excluded. Sorted by path; symlinks are rejected (they could point anywhere).
inline std::vector<bundle_entry> bundle_entries(const std::string& root, std::vector<std::string> files = {},
                                                std::string_view manifest = bundle_manifest_name) {
    namespace sfs = std::filesystem;
    auto base = os::fs::u8path(root);
    if (files.empty()) {
        for (auto it = sfs::recursive_directory_iterator(base); it != sfs::recursive_directory_iterator(); ++it) {
            if (it->is_symlink()) raise(errc::invalid_argument, "{}: symlinks are not allowed in a signed bundle", it->path().string());
            if (!it->is_regular_file()) continue;
            auto rel = sfs::relative(it->path(), base).generic_string();
            if (rel != manifest) files.push_back(rel);
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    std::vector<bundle_entry> out;
    for (auto& rel : files) {
        if (!detail::safe_rel_path(rel)) raise(errc::invalid_argument, "bundle path '{}' must be relative and stay inside the bundle", rel);
        if (rel == manifest) continue;
        auto p = base / os::fs::u8path(rel);
        if (sfs::is_symlink(p)) raise(errc::invalid_argument, "{}: symlinks are not allowed in a signed bundle", rel);
        bundle_entry e; e.path = rel;
        e.blake3 = detail::to_hex(detail::file_blake3(p, e.size));
        out.push_back(std::move(e));
    }
    return out;
}

inline hash::blake3::digest bundle_digest(const std::vector<bundle_entry>& es) {
    hash::blake3 h; detail::put_lp(h, bundle_scheme);
    std::uint64_t n = es.size(); h.update(&n, 8);
    for (auto& e : es) { detail::put_lp(h, e.path); h.update(&e.size, 8); detail::put_lp(h, e.blake3); }
    return h.final();
}

// Writes (or extends, for multiple signers, when the content is unchanged) the manifest in root.
inline void sign_bundle(const std::string& root, const keypair& k, std::vector<std::string> files = {},
                        std::string_view manifest = bundle_manifest_name) {
    auto es = bundle_entries(root, std::move(files), manifest);
    auto d = bundle_digest(es); auto dig = detail::to_hex(d); auto pub = detail::to_hex(k.pub);
    auto mpath = (os::fs::u8path(root) / os::fs::u8path(std::string(manifest))).string();
    json::array sigs;
    if (auto old = os::read_file(mpath)) {
        try {
            auto o = json::parse(*old);
            if (o.str_or("digest", "") == dig)
                if (auto* a = o.find("signatures"); a && a->is_arr())
                    for (auto& s : a->arr()) if (s.str_or("public_key", "") != pub) sigs.push_back(s);
        } catch (const std::exception&) {}   // unreadable old manifest: replace it
    }
    sigs.push_back(json::object{{"key_id", key_id(k.pub)}, {"public_key", pub}, {"sig", sign_digest_hex(d, k)}});
    json::array fl;
    for (auto& e : es) fl.push_back(json::object{{"path", e.path}, {"size", double(e.size)}, {"blake3", e.blake3}});
    auto text = json::dump(json::object{{"alg", "ed25519"}, {"scheme", std::string(bundle_scheme)}, {"files", fl}, {"digest", dig}, {"signatures", sigs}});
    if (auto w = os::fs::write_file_atomic(mpath, text); !w) raise(errc::io_error, "cannot write {}: {}", mpath, w.err.what);
}

struct bundle_report {
    bool signed_ = false, digest_ok = false, trusted = false;
    std::vector<std::string> modified, missing, extra;   // paths
    std::vector<std::string> valid_keys, invalid_keys;   // key ids
    std::string message;
    bool ok() const { return trusted && digest_ok && modified.empty() && missing.empty() && extra.empty(); }
};

// `trusted` = hex public keys; empty means any valid signature (integrity, not provenance).
// `allow_extra` = files present under root but absent from the manifest are not an error.
inline bundle_report verify_bundle(const std::string& root, const std::vector<std::string>& trusted = {},
                                   bool allow_extra = false, std::string_view manifest = bundle_manifest_name) {
    namespace sfs = std::filesystem;
    bundle_report rep;
    auto mpath = (os::fs::u8path(root) / os::fs::u8path(std::string(manifest))).string();
    auto txt = os::read_file(mpath);
    if (!txt) { rep.message = "unsigned (no " + std::string(manifest) + ")"; return rep; }
    rep.signed_ = true;
    json::value m;
    try { m = json::parse(*txt); } catch (const std::exception& e) { rep.message = std::string("bad manifest: ") + e.what(); return rep; }
    if (!m.is_obj() || m.str_or("alg", "") != "ed25519" || m.str_or("scheme", "") != bundle_scheme) { rep.message = "unsupported signature scheme"; return rep; }
    auto* fl = m.find("files");
    if (!fl || !fl->is_arr()) { rep.message = "manifest has no file list"; return rep; }
    // Recompute everything from disk; the manifest's sizes/hashes only say which file differs.
    std::vector<bundle_entry> claimed, actual;
    for (auto& f : fl->arr()) {
        if (!f.is_obj()) { rep.message = "bad manifest entry"; return rep; }
        bundle_entry e{f.str_or("path", ""), std::uint64_t(f.num_or("size", -1)), f.str_or("blake3", "")};
        if (!detail::safe_rel_path(e.path) || e.path == manifest) { rep.message = "unsafe path in manifest: '" + e.path + "'"; return rep; }
        claimed.push_back(std::move(e));
    }
    std::sort(claimed.begin(), claimed.end(), [](auto& a, auto& b) { return a.path < b.path; });
    for (std::size_t i = 1; i < claimed.size(); ++i) if (claimed[i].path == claimed[i - 1].path) { rep.message = "duplicate path in manifest: " + claimed[i].path; return rep; }
    auto base = os::fs::u8path(root);
    for (auto& c : claimed) {
        auto p = base / os::fs::u8path(c.path);
        std::error_code ec;
        if (sfs::is_symlink(p, ec) || !sfs::is_regular_file(p, ec)) { rep.missing.push_back(c.path); continue; }
        bundle_entry a; a.path = c.path; a.blake3 = detail::to_hex(detail::file_blake3(p, a.size));
        if (a.size != c.size || a.blake3 != c.blake3) rep.modified.push_back(c.path);
        actual.push_back(std::move(a));
    }
    if (!allow_extra) {
        std::vector<std::string> listed; for (auto& c : claimed) listed.push_back(c.path);
        std::error_code ec;
        for (auto it = sfs::recursive_directory_iterator(base, ec); !ec && it != sfs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory()) continue;
            auto rel = sfs::relative(it->path(), base).generic_string();
            if (rel != manifest && !std::binary_search(listed.begin(), listed.end(), rel)) rep.extra.push_back(rel);
        }
    }
    // The signature covers the manifest as written (claimed); file checks above cover the disk.
    auto d = bundle_digest(claimed);
    rep.digest_ok = m.str_or("digest", "") == detail::to_hex(d);
    if (!rep.digest_ok) { rep.message = "manifest does not match its signed digest"; return rep; }
    if (auto* sigs = m.find("signatures"); sigs && sigs->is_arr()) {
        for (auto& e : sigs->arr()) {
            std::vector<std::uint8_t> pub, sig;
            try { pub = detail::from_hex(e.str_or("public_key", "")); sig = detail::from_hex(e.str_or("sig", "")); } catch (const forge_exception&) { rep.invalid_keys.push_back("?"); continue; }
            if (pub.size() != 32 || sig.size() != 64) { rep.invalid_keys.push_back("?"); continue; }
            bool ok = crypto_ed25519_check(sig.data(), pub.data(), d.data(), d.size()) == 0;
            (ok ? rep.valid_keys : rep.invalid_keys).push_back(key_id(pub));
            if (ok && (trusted.empty() || std::find(trusted.begin(), trusted.end(), detail::to_hex(pub)) != trusted.end())) rep.trusted = true;
        }
    }
    if (!rep.modified.empty()) rep.message = std::to_string(rep.modified.size()) + " file(s) modified since signing";
    else if (!rep.missing.empty()) rep.message = std::to_string(rep.missing.size()) + " file(s) missing";
    else if (!rep.extra.empty()) rep.message = std::to_string(rep.extra.size()) + " unsigned file(s) present";
    else rep.message = rep.trusted ? "ok" : rep.valid_keys.empty() ? "no valid signature" : "valid signature but no trusted key";
    return rep;
}

} // namespace forge::modelfile
