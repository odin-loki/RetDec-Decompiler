#pragma once
// FSS INT-1 / INT-2 / INT-3 / INT-4 for files that are not .fpk models: detached signatures, the policy decision for
// plugins and scripts, and the OS code-signature check.
//
//   auto sig = security::detached_sign(bytes, "hello_plugin.so", seed);         // JSON text for hello_plugin.so.sig
//   auto rep = security::detached_verify(bytes, "hello_plugin.so", sig, {hex_pub});   // rep.trusted
//   security::verify_artefact(policy, artefact::script, path, bytes);          // integrity.scripts decides; throws
//   auto cs = security::os_code_signature("C:/x/plugin.dll");                  // {state, signer, backend, detail}
//
// Detached signature (`<file>.sig`, JSON, UTF-8, one object):
//   {"format":"forge-detached-sig","version":1,"alg":"ed25519","hash":"blake3-256",
//    "file":"<basename>","size":<bytes>,"blake3":"<64 hex>",
//    "signatures":[{"key_id":"<8 hex>","public_key":"<64 hex>","sig":"<128 hex>"}]}
// Each Ed25519 signature (RFC 8032, Monocypher) is over the 32-byte message digest
//   BLAKE3("forge-detached-sig-v1" 0x00 || u64le(len(name)) || name || u64le(size) || BLAKE3(file bytes))
// so it binds the file's content, its length and its base name. Binding the name means a signed plugin or script
// cannot be swapped for a different file signed by the same key (b.so renamed to a.so fails); the cost is that a
// signed file cannot be renamed. Names compare exactly on Linux / BSD and ASCII-case-insensitively on Windows and
// macOS (case-insensitive file systems). "file", "size" and "blake3" are informative: verification recomputes all of
// them from the bytes and the name it is given and never trusts the stored values. Several signers may sign the same
// file ("signatures" is extended, like the .fpk signature section).
//
// Trusted keys: integrity.trusted_keys of the policy; an empty list accepts any valid signature (integrity, not
// provenance), as for models.
#include <forge/security/audit.hpp>
#include <forge/security/errors.hpp>
#include <forge/security/policy.hpp>
#include <forge/hash/blake3.hpp>
#include <forge/json/json.hpp>
#include <forge/os/os.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
extern "C" {
#include "monocypher-ed25519.h"
}
#if defined(FORGE_OS_WINDOWS)
#include <wincrypt.h>
#include <wintrust.h>
#elif defined(__APPLE__)
#include <forge/os/detail.hpp>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

namespace forge::security {

inline constexpr std::string_view detached_format = "forge-detached-sig";
inline constexpr std::string_view detached_scheme = "forge-detached-sig-v1";
inline constexpr int detached_version = 1;
inline constexpr std::size_t detached_max_sig_bytes = 64 * 1024;

namespace detail {
inline std::string hex(std::span<const std::uint8_t> b) {
    static const char* d = "0123456789abcdef"; std::string s; s.reserve(b.size() * 2);
    for (auto c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}
inline bool unhex(std::string_view s, std::vector<std::uint8_t>& o) {
    auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    if (s.size() % 2) return false;
    o.clear(); o.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return false;
        o.push_back(std::uint8_t(a * 16 + b));
    }
    return true;
}
inline std::string lower_ascii(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
inline void put_u64(hash::blake3& h, std::uint64_t v) {
    std::uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = std::uint8_t(v >> (8 * i));
    h.update(b, 8);
}
inline bool same_name(std::string_view a, std::string_view b) {
#if defined(FORGE_OS_WINDOWS) || defined(__APPLE__)
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
#else
    return a == b;
#endif
}
inline std::string key_id(std::span<const std::uint8_t> pub) { auto h = hash::blake3::of(pub); return hex({h.data(), 4}); }
inline std::span<const std::uint8_t> bytes_of(std::string_view s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }
} // namespace detail

// The base name a detached signature binds: the last path component ('/' everywhere, also '\\' and ':' on Windows).
inline std::string signed_name(std::string_view path) {
#if defined(FORGE_OS_WINDOWS)
    auto p = path.find_last_of("/\\:");
#else
    auto p = path.find_last_of('/');
#endif
    return std::string(p == std::string_view::npos ? path : path.substr(p + 1));
}
inline std::string detached_sig_path(std::string_view file) { return std::string(file) + ".sig"; }

// The 32-byte message the Ed25519 signatures cover.
inline hash::blake3::digest detached_message(std::string_view name, std::uint64_t size, const hash::blake3::digest& file_digest) {
    hash::blake3 h;
    h.update(detached_scheme.data(), detached_scheme.size());
    const std::uint8_t zero = 0; h.update(&zero, 1);
    detail::put_u64(h, name.size()); h.update(name.data(), name.size());
    detail::put_u64(h, size); h.update(file_digest.data(), file_digest.size());
    return h.final();
}

// Signs `bytes` as `name` with a 32-byte Ed25519 seed and returns the .sig JSON text. `existing` (the current .sig, if
// any) is extended when it is for the same name and content; a previous signature by the same key is replaced.
inline std::string detached_sign(std::span<const std::uint8_t> bytes, std::string_view name, std::span<const std::uint8_t> seed, std::string_view existing = {}) {
    if (seed.size() != 32) throw integrity_error("detached_sign: an Ed25519 seed is 32 bytes", "INT-2");
    if (name.empty()) throw integrity_error("detached_sign: empty file name", "INT-2");
    auto fd = hash::blake3::of(bytes);
    auto msg = detached_message(name, bytes.size(), fd);
    std::uint8_t sk[64], pub[32], s2[32], sig[64];
    std::copy(seed.begin(), seed.end(), s2);
    crypto_ed25519_key_pair(sk, pub, s2);   // wipes s2
    crypto_ed25519_sign(sig, sk, msg.data(), msg.size());
    crypto_wipe(sk, sizeof sk);
    const std::string pub_hex = detail::hex(pub), fd_hex = detail::hex(fd);
    json::array sigs;
    if (!existing.empty()) {
        try {
            auto old = json::parse(existing);
            if (old.str_or("format", "") == detached_format && old.str_or("blake3", "") == fd_hex && old.str_or("file", "") == name)
                if (auto* a = old.find("signatures"); a && a->is_arr())
                    for (auto& e : a->arr()) if (e.str_or("public_key", "") != pub_hex) sigs.push_back(e);
        } catch (const std::exception&) {}   // an unreadable old .sig is replaced
    }
    sigs.push_back(json::object{{"key_id", detail::key_id(pub)}, {"public_key", pub_hex}, {"sig", detail::hex(sig)}});
    return json::dump(json::object{{"format", std::string(detached_format)}, {"version", detached_version}, {"alg", "ed25519"},
                                   {"hash", "blake3-256"}, {"file", std::string(name)}, {"size", static_cast<long long>(bytes.size())},
                                   {"blake3", fd_hex}, {"signatures", std::move(sigs)}}) + "\n";
}

struct detached_report {
    enum class status { ok, unsigned_, malformed, name_mismatch, content_mismatch, no_valid_signature, untrusted };
    status st = status::unsigned_;
    bool trusted = false;                                  // a valid signature from a trusted key (any key if none given)
    std::string signed_file;                               // the name recorded in the .sig
    std::vector<std::string> valid_keys, invalid_keys;     // key ids
    std::string message;
};
inline const char* name(detached_report::status s) {
    using S = detached_report::status;
    switch (s) {
    case S::ok: return "ok"; case S::unsigned_: return "unsigned"; case S::malformed: return "malformed";
    case S::name_mismatch: return "name mismatch"; case S::content_mismatch: return "content mismatch";
    case S::no_valid_signature: return "no valid signature"; case S::untrusted: return "untrusted";
    }
    return "?";
}

// Verifies the .sig text `sig_text` for `bytes` loaded under the base name `name`. `trusted` = hex public keys; empty
// accepts any valid signature. Never throws.
inline detached_report detached_verify(std::span<const std::uint8_t> bytes, std::string_view name, std::string_view sig_text, const std::vector<std::string>& trusted = {}) {
    using S = detached_report::status;
    detached_report r;
    if (sig_text.empty()) { r.message = "unsigned (no detached signature)"; return r; }
    json::value s;
    try { s = json::parse(sig_text); } catch (const std::exception& e) { r.st = S::malformed; r.message = std::string("unreadable signature: ") + e.what(); return r; }
    if (!s.is_obj() || s.str_or("format", "") != detached_format || s.num_or("version", 0) != detached_version || s.str_or("alg", "") != "ed25519" || s.str_or("hash", "") != "blake3-256") {
        r.st = S::malformed; r.message = "not a forge-detached-sig v1 (ed25519, blake3-256) signature"; return r;
    }
    r.signed_file = s.str_or("file", "");
    if (r.signed_file.empty() || !detail::same_name(r.signed_file, name)) {
        r.st = S::name_mismatch; r.message = "signature is for '" + r.signed_file + "', not '" + std::string(name) + "'"; return r;
    }
    auto fd = hash::blake3::of(bytes);
    if (s.str_or("blake3", "") != detail::hex(fd) || s.num_or("size", -1) != double(bytes.size())) {
        r.st = S::content_mismatch; r.message = "content does not match the signed digest (file modified after signing)";
        // the signatures are still checked below so the report names the keys, but the status stays a mismatch
    }
    auto msg = detached_message(r.signed_file, bytes.size(), fd);
    std::vector<std::string> tk; for (auto& k : trusted) tk.push_back(detail::lower_ascii(k));
    auto* arr = s.find("signatures");
    if (!arr || !arr->is_arr()) { if (r.st == S::unsigned_) { r.st = S::malformed; r.message = "no signatures"; } return r; }
    std::vector<std::uint8_t> pub, sig;
    for (auto& e : arr->arr()) {
        if (!detail::unhex(e.str_or("public_key", ""), pub) || !detail::unhex(e.str_or("sig", ""), sig) || pub.size() != 32 || sig.size() != 64) { r.invalid_keys.push_back("?"); continue; }
        bool ok = crypto_ed25519_check(sig.data(), pub.data(), msg.data(), msg.size()) == 0;
        (ok ? r.valid_keys : r.invalid_keys).push_back(detail::key_id(pub));
        if (ok && (tk.empty() || std::find(tk.begin(), tk.end(), detail::hex(pub)) != tk.end())) r.trusted = true;
    }
    if (r.st == S::content_mismatch) { r.trusted = false; return r; }
    if (r.trusted) { r.st = S::ok; r.message = "ok (key " + r.valid_keys.front() + ")"; }
    else if (r.valid_keys.empty()) { r.st = S::no_valid_signature; r.message = "no valid signature"; }
    else { r.st = S::untrusted; r.message = "valid signature (key " + r.valid_keys.front() + ") but not by a trusted key"; }
    return r;
}

// Reads `<file>.sig`: nullopt when it does not exist; throws integrity_error when it exists but cannot be read or is
// larger than 64 KiB.
inline std::optional<std::string> read_detached_sig(const std::string& file) {
    auto sp = detached_sig_path(file);
    auto st = os::stat(sp);
    if (!st.exists) return std::nullopt;
    if (!st.is_file || st.size > detached_max_sig_bytes) throw integrity_error(sp + ": not a detached signature file", "INT-2");
    auto t = os::read_file(sp);
    if (!t) throw integrity_error(sp + ": " + t.err.what, "INT-2");
    return std::move(t.value);
}
// Verify a file on disk with its .sig (forge verify). The bytes are read once and hashed from that buffer.
inline detached_report detached_verify_file(const std::string& file, const std::vector<std::string>& trusted = {}, const std::string& sig_file = {}) {
    detached_report r;
    auto b = os::read_file(file);
    if (!b) { r.st = detached_report::status::malformed; r.message = b.err.what; return r; }
    std::optional<std::string> sig;
    if (!sig_file.empty()) { auto t = os::read_file(sig_file); if (!t) { r.message = sig_file + ": " + t.err.what; return r; } sig = std::move(t.value); }
    else { try { sig = read_detached_sig(file); } catch (const std::exception& e) { r.st = detached_report::status::malformed; r.message = e.what(); return r; } }
    if (!sig) { r.message = "unsigned (no " + detached_sig_path(file) + ")"; return r; }
    return detached_verify(detail::bytes_of(b.value), signed_name(file), *sig, trusted);
}

// ---------------------------------------------------------------- the policy decision (INT-1 / INT-4)
enum class artefact { plugin, script };
inline const char* name(artefact a) { return a == artefact::plugin ? "plugin" : "script"; }
inline requirement requirement_for(const policy& p, artefact a) { return a == artefact::plugin ? p.integrity.plugins : p.integrity.scripts; }

// Applies integrity.plugins / integrity.scripts of `p` to `bytes`, the exact bytes that will be loaded or parsed,
// known as `path` (its base name is what the signature must name, unless `name_override` is given). `sig_text`: the detached
// signature; nullopt reads `<path>.sig`, "" means none (source without a file). Policy inactive or requirement off: nothing. required: a trusted signature
// must verify, else integrity_error (audited "INT-4", fail). optional: a present signature must verify (same
// failure); an unsigned file passes, audited "INT-1" weaken. A verified file is audited "INT-2" allow.
inline void verify_artefact(const policy& p, artefact a, const std::string& path, std::span<const std::uint8_t> bytes,
                            std::optional<std::string> sig_text = std::nullopt, std::string_view name_override = {}) {
    if (!p.active()) return;
    const requirement req = requirement_for(p, a);
    if (req == requirement::off) return;
    const std::string what = std::string("integrity.") + (a == artefact::plugin ? "plugins" : "scripts");
    auto fail = [&](const std::string& why) {
        audit("integrity", decision::fail, "INT-4", path, std::string(name(a)) + ": " + why);
        throw integrity_error(path + ": " + why + " (" + what + " " + security::name(req) + ")", "INT-4");
    };
    if (!sig_text) {
        try { sig_text = read_detached_sig(path); } catch (const integrity_error& e) { fail(e.what()); }
    }
    if (!sig_text || sig_text->empty()) {
        if (req == requirement::required) fail("no detached signature (" + detached_sig_path(signed_name(path)) + ")");
        audit("integrity", decision::weaken, "INT-1", path, std::string("unsigned ") + name(a) + " loaded (" + what + " optional)");
        return;
    }
    const std::string nm = name_override.empty() ? signed_name(path) : std::string(name_override);
    auto r = detached_verify(bytes, nm, *sig_text, p.integrity.trusted_keys);
    if (!r.trusted) fail("signature check failed: " + r.message);
    audit("integrity", decision::allow, "INT-2", path, std::string(name(a)) + " signature verified (key " + r.valid_keys.front() + ")");
}

// ---------------------------------------------------------------- OS code signing (INT-3)
struct code_signature {
    enum class state { signed_, unsigned_, invalid, not_applicable, error };
    state st = state::not_applicable;
    std::string signer;    // certificate subject / signing identity, when known
    std::string backend;   // WinVerifyTrust, SecStaticCodeCheckValidity, ...
    std::string detail;
    bool ok() const { return st == state::signed_; }
};
inline const char* name(code_signature::state s) {
    using S = code_signature::state;
    switch (s) { case S::signed_: return "signed"; case S::unsigned_: return "unsigned"; case S::invalid: return "invalid"; case S::not_applicable: return "n/a"; case S::error: return "error"; }
    return "?";
}

#if defined(FORGE_OS_WINDOWS)
namespace detail {
// wintrust.dll / crypt32.dll from System32 at run time (no import library); GUIDs and the catalog structures are
// declared here so no softpub.h / mscat.h is needed.
struct wintrust_api {
    HMODULE wt = nullptr, c32 = nullptr;
    LONG (WINAPI* verify)(HWND, GUID*, LPVOID) = nullptr;
    CRYPT_PROVIDER_DATA* (WINAPI* prov_data)(HANDLE) = nullptr;
    CRYPT_PROVIDER_SGNR* (WINAPI* prov_signer)(CRYPT_PROVIDER_DATA*, DWORD, BOOL, DWORD) = nullptr;
    BOOL (WINAPI* cat_acquire2)(HANDLE*, const GUID*, PCWSTR, PCCERT_STRONG_SIGN_PARA, DWORD) = nullptr;
    BOOL (WINAPI* cat_hash2)(HANDLE, HANDLE, DWORD*, BYTE*, DWORD) = nullptr;
    HANDLE (WINAPI* cat_enum)(HANDLE, BYTE*, DWORD, DWORD, HANDLE*) = nullptr;
    BOOL (WINAPI* cat_info)(HANDLE, void*, DWORD) = nullptr;
    BOOL (WINAPI* cat_release_cat)(HANDLE, HANDLE, DWORD) = nullptr;
    BOOL (WINAPI* cat_release)(HANDLE, DWORD) = nullptr;
    DWORD (WINAPI* cert_name)(PCCERT_CONTEXT, DWORD, DWORD, void*, LPWSTR, DWORD) = nullptr;
    static wintrust_api& get() {
        static wintrust_api a = [] {
            wintrust_api x;
            x.wt = LoadLibraryExW(L"wintrust.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/);
            x.c32 = LoadLibraryExW(L"crypt32.dll", nullptr, 0x800);
            if (!x.wt) return x;
            auto f = [&](auto& fn, HMODULE m, const char* n) { if (m) fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(m, n))); };
            f(x.verify, x.wt, "WinVerifyTrust"); f(x.prov_data, x.wt, "WTHelperProvDataFromStateData"); f(x.prov_signer, x.wt, "WTHelperGetProvSignerFromChain");
            f(x.cat_acquire2, x.wt, "CryptCATAdminAcquireContext2"); f(x.cat_hash2, x.wt, "CryptCATAdminCalcHashFromFileHandle2");
            f(x.cat_enum, x.wt, "CryptCATAdminEnumCatalogFromHash"); f(x.cat_info, x.wt, "CryptCATCatalogInfoFromContext");
            f(x.cat_release_cat, x.wt, "CryptCATAdminReleaseCatalogContext"); f(x.cat_release, x.wt, "CryptCATAdminReleaseContext");
            f(x.cert_name, x.c32, "CertGetNameStringW");
            return x;
        }();
        return a;
    }
};
struct catalog_info { DWORD cbStruct; WCHAR wszCatalogFile[MAX_PATH]; };
// WINTRUST_ACTION_GENERIC_VERIFY_V2 and DRIVER_ACTION_VERIFY
inline GUID wvt_generic_v2() { return GUID{0x00AAC56B, 0xCD44, 0x11D0, {0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE}}; }
inline GUID wvt_driver_verify() { return GUID{0xF750E6C3, 0x38EE, 0x11D1, {0x85, 0xE5, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE}}; }

// One WinVerifyTrust call (embedded signature, or a catalog member), no UI, revocation from the cache only.
inline LONG wvt_run(wintrust_api& w, WINTRUST_DATA& wd, std::string& signer) {
    GUID g = wvt_generic_v2();
    wd.cbStruct = sizeof wd; wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwStateAction = WTD_STATEACTION_VERIFY; wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    LONG rc = w.verify(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &g, &wd);
    if (rc == 0 && w.prov_data && w.prov_signer && w.cert_name) {
        if (auto* pd = w.prov_data(wd.hWVTStateData))
            if (auto* sg = w.prov_signer(pd, 0, FALSE, 0); sg && sg->csCertChain > 0 && sg->pasCertChain && sg->pasCertChain[0].pCert) {
                wchar_t buf[512] = {};
                if (w.cert_name(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, buf, 512) > 1) signer = os::narrow(buf);
            }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    w.verify(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &g, &wd);
    return rc;
}
inline code_signature wvt_classify(LONG rc, std::string signer, const char* how) {
    code_signature r; r.backend = std::string("WinVerifyTrust (") + how + ")";
    char b[16]; std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(rc));
    if (rc == 0) { r.st = code_signature::state::signed_; r.signer = std::move(signer); r.detail = std::string("valid Authenticode signature (") + how + ")"; return r; }
    switch (static_cast<unsigned long>(rc)) {
    case 0x800B0100: /*TRUST_E_NOSIGNATURE*/ case 0x800B0003: /*TRUST_E_SUBJECT_FORM_UNKNOWN*/ case 0x800B0001: /*TRUST_E_PROVIDER_UNKNOWN*/
        r.st = code_signature::state::unsigned_; r.detail = std::string("no Authenticode signature (") + b + ")"; break;
    case 0x80096010: /*TRUST_E_BAD_DIGEST*/ r.st = code_signature::state::invalid; r.detail = "signature does not match the file (TRUST_E_BAD_DIGEST)"; break;
    case 0x800B010C: /*CERT_E_REVOKED*/ r.st = code_signature::state::invalid; r.detail = "certificate revoked"; break;
    case 0x800B0109: /*CERT_E_UNTRUSTEDROOT*/ r.st = code_signature::state::invalid; r.detail = "untrusted root (CERT_E_UNTRUSTEDROOT)"; break;
    case 0x800B0101: /*CERT_E_EXPIRED*/ r.st = code_signature::state::invalid; r.detail = "certificate expired"; break;
    case 0x800B0004: /*TRUST_E_SUBJECT_NOT_TRUSTED*/ r.st = code_signature::state::invalid; r.detail = "subject not trusted"; break;
    case 0x800B0111: /*TRUST_E_EXPLICIT_DISTRUST*/ r.st = code_signature::state::invalid; r.detail = "explicitly distrusted"; break;
    default: r.st = code_signature::state::invalid; r.detail = std::string("WinVerifyTrust failed (") + b + ")"; break;
    }
    return r;
}
// `file` is an open handle to `path` (the caller keeps writers out while it checks and loads).
inline code_signature win_code_signature(const std::string& path, HANDLE file) {
    auto& w = wintrust_api::get();
    if (!w.verify) { code_signature r; r.st = code_signature::state::error; r.backend = "WinVerifyTrust"; r.detail = "wintrust.dll not available"; return r; }
    const std::wstring wp = os::widen(path);
    std::string signer;
    WINTRUST_FILE_INFO fi{}; fi.cbStruct = sizeof fi; fi.pcwszFilePath = wp.c_str(); fi.hFile = file;
    WINTRUST_DATA wd{}; wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi;
    LONG rc = wvt_run(w, wd, signer);
    if (static_cast<unsigned long>(rc) != 0x800B0100 /*TRUST_E_NOSIGNATURE*/ || !w.cat_acquire2 || !w.cat_hash2 || !w.cat_enum || !w.cat_info)
        return wvt_classify(rc, std::move(signer), "embedded");
    // No embedded signature: Windows' own binaries are usually signed through a system catalog.
    GUID drv = wvt_driver_verify();
    for (const wchar_t* alg : {L"SHA256", L"SHA1"}) {
        HANDLE admin = nullptr;
        if (!w.cat_acquire2(&admin, &drv, alg, nullptr, 0)) continue;
        DWORD n = 0; std::vector<BYTE> h;
        if (w.cat_hash2(admin, file, &n, nullptr, 0) || GetLastError() == ERROR_INSUFFICIENT_BUFFER) { h.resize(n); if (!w.cat_hash2(admin, file, &n, h.data(), 0)) h.clear(); }
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        HANDLE cat = h.empty() ? nullptr : w.cat_enum(admin, h.data(), n, 0, nullptr);
        if (!cat) { if (w.cat_release) w.cat_release(admin, 0); continue; }
        catalog_info ci{}; ci.cbStruct = sizeof ci;
        LONG crc = static_cast<LONG>(0x800B0100);
        if (w.cat_info(cat, &ci, 0)) {
            std::wstring tag; static const wchar_t* hx = L"0123456789ABCDEF";
            for (BYTE b : h) { tag += hx[b >> 4]; tag += hx[b & 15]; }
            WINTRUST_CATALOG_INFO wc{}; wc.cbStruct = sizeof wc; wc.pcwszCatalogFilePath = ci.wszCatalogFile; wc.pcwszMemberTag = tag.c_str();
            wc.pcwszMemberFilePath = wp.c_str(); wc.hMemberFile = file; wc.pbCalculatedFileHash = h.data(); wc.cbCalculatedFileHash = n; wc.hCatAdmin = admin;
            WINTRUST_DATA cd{}; cd.dwUnionChoice = WTD_CHOICE_CATALOG; cd.pCatalog = &wc;
            crc = wvt_run(w, cd, signer);
        }
        if (w.cat_release_cat) w.cat_release_cat(admin, cat, 0);
        if (w.cat_release) w.cat_release(admin, 0);
        return wvt_classify(crc, std::move(signer), "catalog");
    }
    return wvt_classify(rc, std::move(signer), "embedded");
}
} // namespace detail
#endif

#if defined(__APPLE__)
namespace detail {
struct seccode_api {
    void* h = nullptr;
    int (*create)(const void*, std::uint32_t, const void**) = nullptr;            // SecStaticCodeCreateWithPath
    int (*check)(const void*, std::uint32_t, const void*) = nullptr;              // SecStaticCodeCheckValidity
    int (*info)(const void*, std::uint32_t, const void**) = nullptr;              // SecCodeCopySigningInformation
    const void* (*subject)(const void*) = nullptr;                                // SecCertificateCopySubjectSummary
    const void* (*url)(const void*, const std::uint8_t*, long, unsigned char) = nullptr;   // CFURLCreateFromFileSystemRepresentation
    const void* k_certs = nullptr; const void* k_flags = nullptr;
    static seccode_api& get() {
        static seccode_api a = [] {
            seccode_api x; x.h = ::dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
            auto& cf = os::detail::cf_api::get();
            if (!x.h || !cf.h) return x;
            x.create = reinterpret_cast<decltype(x.create)>(::dlsym(x.h, "SecStaticCodeCreateWithPath"));
            x.check = reinterpret_cast<decltype(x.check)>(::dlsym(x.h, "SecStaticCodeCheckValidity"));
            x.info = reinterpret_cast<decltype(x.info)>(::dlsym(x.h, "SecCodeCopySigningInformation"));
            x.subject = reinterpret_cast<decltype(x.subject)>(::dlsym(x.h, "SecCertificateCopySubjectSummary"));
            x.url = reinterpret_cast<decltype(x.url)>(::dlsym(cf.h, "CFURLCreateFromFileSystemRepresentation"));
            x.k_certs = os::detail::framework_const(x.h, "kSecCodeInfoCertificates");
            x.k_flags = os::detail::framework_const(x.h, "kSecCodeInfoFlags");
            return x;
        }();
        return a;
    }
};
inline code_signature mac_code_signature(const std::string& path) {
    code_signature r; r.backend = "SecStaticCodeCheckValidity";
    auto& s = seccode_api::get(); auto& cf = os::detail::cf_api::get();
    if (!s.create || !s.check || !s.url || !cf.CFRelease) { r.st = code_signature::state::error; r.detail = "Security.framework not available"; return r; }
    const void* u = s.url(nullptr, reinterpret_cast<const std::uint8_t*>(path.data()), long(path.size()), 0);
    if (!u) { r.st = code_signature::state::error; r.detail = "bad path"; return r; }
    const void* code = nullptr;
    int rc = s.create(u, 0, &code);
    cf.CFRelease(u);
    if (rc != 0 || !code) { r.st = code_signature::state::error; r.detail = "SecStaticCodeCreateWithPath: " + std::to_string(rc); return r; }
    // kSecCSCheckAllArchitectures | kSecCSStrictValidate | kSecCSCheckNestedCode
    rc = s.check(code, (1u << 0) | (1u << 4) | (1u << 3), nullptr);
    if (rc == -67062 /*errSecCSUnsigned*/) { r.st = code_signature::state::unsigned_; r.detail = "not code signed"; }
    else if (rc != 0) { r.st = code_signature::state::invalid; r.detail = "SecStaticCodeCheckValidity: OSStatus " + std::to_string(rc); }
    else {
        r.st = code_signature::state::signed_; r.detail = "valid code signature";
        const void* dict = nullptr;
        if (s.info && s.info(code, 1u << 1 /*kSecCSSigningInformation*/, &dict) == 0 && dict) {
            long long fl = s.k_flags ? cf.to_ll(cf.CFDictionaryGetValue(dict, s.k_flags), 0) : 0;
            const void* certs = s.k_certs ? cf.CFDictionaryGetValue(dict, s.k_certs) : nullptr;
            if (certs && cf.CFArrayGetCount && cf.CFArrayGetCount(certs) > 0 && s.subject) {
                if (const void* sum = s.subject(cf.CFArrayGetValueAtIndex(certs, 0))) { r.signer = cf.to_string(sum); cf.CFRelease(sum); }
            }
            if ((fl & 0x2 /*kSecCodeSignatureAdhoc*/) || !certs) { r.st = code_signature::state::unsigned_; r.detail = "ad-hoc signature only (no signing identity)"; }
            cf.CFRelease(dict);
        }
    }
    cf.CFRelease(code);
    return r;
}
} // namespace detail
#endif

// The OS code signature of `path` (INT-3). Windows: Authenticode through WinVerifyTrust (embedded signature, else a
// system catalog), no UI, revocation from the local cache only. macOS: SecStaticCodeCheckValidity; an ad-hoc signature
// counts as unsigned. Linux / FreeBSD / others: no standard mechanism for shared objects (n/a); on Linux the detail
// says whether an IMA signature (security.ima xattr) is present, which is reported, not appraised.
inline code_signature os_code_signature(const std::string& path) {
#if defined(FORGE_OS_WINDOWS)
    HANDLE h = CreateFileW(os::widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { code_signature r; r.st = code_signature::state::error; r.backend = "WinVerifyTrust"; r.detail = os::last_error("open " + path).what; return r; }
    auto r = detail::win_code_signature(path, h);
    CloseHandle(h);
    return r;
#elif defined(__APPLE__)
    return detail::mac_code_signature(path);
#else
    code_signature r; r.st = code_signature::state::not_applicable;
#if defined(__linux__)
    r.backend = "none (IMA reported only)";
    auto n = ::getxattr(path.c_str(), "security.ima", nullptr, 0);
    r.detail = n > 0 ? "no OS code signing for shared objects; an IMA signature (security.ima) is present but not appraised by FORGE"
                     : "no OS code signing for shared objects (no IMA signature)";
#else
    r.backend = "none";
    r.detail = "no OS code signing for shared objects on this OS";
#endif
    (void)path;
    return r;
#endif
}

} // namespace forge::security
