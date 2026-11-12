#pragma once
// Secure key storage: put/get/remove a secret by (service, account).
//   macOS/iOS: Keychain generic passwords (SecItemUpdate, else SecItemAdd with AfterFirstUnlockThisDeviceOnly).
//   Windows: DPAPI (CryptProtectData, user scope) blobs under %LOCALAPPDATA%\forge\keys.
//   Linux/FreeBSD: libsecret (Secret Service over D-Bus) when a session bus has a Secret Service provider; its
//   failures (locked keyring, dismissed prompt) are returned, not papered over. With no bus or no provider the
//   file backend is used and keystore::backend() says "file".
//   Android: the app's JNI glue installs a backend (Android Keystore wrapping key + EncryptedFile) with
//   keystore::set_backend(), or sets FORGE_KEYSTORE_DIR to getFilesDir() for the file backend; otherwise every
//   call fails.
//   File backend (FORGE_KEYSTORE=file forces it): one file per secret, XChaCha20-Poly1305 (Monocypher) under a
//   random 32-byte store key, with a random nonce and the (service, account) pair as associated data, so a
//   tampered, truncated or swapped file fails get(). The store key lives in `.master` beside the secrets (0600,
//   created exclusively; DPAPI-wrapped on Windows), so this protects against other users and against copies of
//   one secret file, not against an attacker who can read the whole directory as this user, unless the store
//   key is wrapped (below). Files from the old XOR format are re-encrypted on first read and the old pad is
//   deleted once no old file remains.
//   Store key wrapping (file backend only):
//   - Passphrase: FORGE_KEYSTORE_PASSPHRASE, keystore::set_passphrase() or set_passphrase_provider(). `.master`
//     then holds the key sealed with XChaCha20-Poly1305 under Argon2id(passphrase, 16-byte random salt); the
//     Argon2 parameters (keystore_kdf, default m=64 MiB t=3 p=1, minimum m=8 MiB t=2) are in the header, which
//     is authenticated. A wrong passphrase or a modified header fails with EACCES, never with a wrong key. A plain
//     `.master` is wrapped the first time a passphrase is supplied; rewrap() changes or removes the passphrase.
//   - TPM (opt-in, FORGE_KEYSTORE_TPM=1): Linux/FreeBSD seal the key to a TPM 2.0 (/dev/tpmrm0) through tpm2-tss
//     loaded at run time (libtss2-esys/-mu/-tctildr): an SRK-template primary (ECC P-256, RSA-2048 fallback)
//     under the owner hierarchy parents a keyedhash sealed object whose public/private blobs are `.master.tpm`.
//     Windows encrypts the key with a non-exportable RSA-2048 key in the Microsoft Platform Crypto Provider
//     (NCrypt, TPM-backed); `.master.tpm` names the key and holds the RSA-OAEP ciphertext. A sealed key is lost
//     when the TPM is cleared: with a passphrase also set, `.master` keeps a passphrase-wrapped copy for recovery,
//     otherwise it holds only a marker. FORGE_KEYSTORE_TPM=1 without a usable TPM makes the file backend fail
//     with an unsupported error (tpm_available() says why); disable_tpm() unseals and removes the TPM copy.
//   backend() reports the wrapping in force: "file", "file+argon2id", "file+tpm", "file+tpm+argon2id".
//   The unwrapped store key is cached in memory per (directory, key files, passphrase); clear_passphrase() wipes it.
//   The cache and the Argon2id key-encryption keys are os::locked_buffer (locked, excluded from core dumps where
//   the OS can, wiped on release: FSS-CRY-2); short-lived copies made while a call runs are crypto_wipe'd.
// Link: the file backend needs Monocypher (forge_os links forge_trust in CMake).
#include <forge/os/detail.hpp>
#include <forge/os/locked_memory.hpp>
#include <monocypher.h>
#include <cstdlib>
#include <functional>
#include <map>
#include <thread>
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(FORGE_OS_FREEBSD)
#define FORGE_OS_KS_LIBSECRET_ 1
#define FORGE_OS_KS_TSS_ 1
#endif

namespace forge::os {
struct keystore_backend {
    const char* name = "custom";
    std::function<status(const std::string&, const std::string&, const std::string&)> put;
    std::function<result<std::string>(const std::string&, const std::string&)> get;
    std::function<status(const std::string&, const std::string&)> remove;
};
// Argon2id cost for passphrase wrapping. Stored in each wrapped `.master`, so changing it affects new wraps only.
struct keystore_kdf {
    std::uint32_t memory_kib = 65536, passes = 3, lanes = 1;
    static constexpr std::uint32_t min_memory_kib = 8192, min_passes = 2, max_memory_kib = 1u << 20, max_passes = 64, max_lanes = 16;
    bool valid() const { return memory_kib >= min_memory_kib && memory_kib <= max_memory_kib && passes >= min_passes && passes <= max_passes && lanes >= 1 && lanes <= max_lanes && memory_kib >= 8 * lanes; }
};
class keystore {
public:
    static void set_backend(keystore_backend b) { std::lock_guard<std::mutex> g(m_()); custom_() = std::make_unique<keystore_backend>(std::move(b)); }
    static void reset_backend() { std::lock_guard<std::mutex> g(m_()); custom_().reset(); }
    static std::string backend() {
        { std::lock_guard<std::mutex> g(m_()); if (custom_()) return custom_()->name; }
        if (force_file_()) return file_name_backend_();
#if defined(__APPLE__)
        return "keychain";
#elif defined(FORGE_OS_WINDOWS)
        return "dpapi";
#elif defined(__ANDROID__)
        return getenv("FORGE_KEYSTORE_DIR").empty() ? "none" : file_name_backend_();
#elif defined(FORGE_OS_KS_LIBSECRET_)
        return use_libsecret_() ? "libsecret" : file_name_backend_();
#else
        return file_name_backend_();
#endif
    }
    // Passphrase for wrapping the file backend's store key (takes precedence over the provider and
    // FORGE_KEYSTORE_PASSPHRASE). An empty string forgets it.
    static void set_passphrase(std::string_view p) { std::lock_guard<std::mutex> g(m_()); auto& s = ps_(); wipe_(s.pass); s.pass.assign(p); wipe_cache_(); }
    // Called when a passphrase is needed and none is set (e.g. to prompt); return "" for none.
    static void set_passphrase_provider(std::function<std::string()> f) { std::lock_guard<std::mutex> g(m_()); ps_().provider = std::move(f); }
    // Forgets the passphrase, the provider and the cached store key.
    static void clear_passphrase() { std::lock_guard<std::mutex> g(m_()); auto& s = ps_(); wipe_(s.pass); s.pass.clear(); s.provider = nullptr; wipe_cache_(); }
    static status set_kdf(keystore_kdf k) {
        if (!k.valid()) return {{}, {EINVAL, "keystore: Argon2 parameters out of range (memory >= 8192 KiB, passes >= 2, 1-16 lanes)"}};
        std::lock_guard<std::mutex> g(m_()); ps_().kdf = k; return {};
    }
    static keystore_kdf kdf() { std::lock_guard<std::mutex> g(m_()); return ps_().kdf; }
    // Where the file backend (and the Windows DPAPI backend) keeps its files: FORGE_KEYSTORE_DIR, else the
    // per-user default. It may not exist yet. forge::security checks it is private (FSS-FIO-2).
    static result<std::string> directory() { return dir_(); }
    // Re-wraps the file backend's store key: unlocks with old_pass ("" = not passphrase-wrapped, or unlock
    // through the TPM copy) and wraps with new_pass ("" = remove the passphrase layer). Secrets are not touched.
    // On success new_pass becomes the process passphrase (set_passphrase). The TPM copy, if any, is kept.
    static status rewrap(std::string_view old_pass, std::string_view new_pass) {
        auto d = dir_(); if (!d) return {{}, d.err};
        auto p = join(d.value, ".master"), tp = join(d.value, ".master.tpm");
        auto r = read_file(p); if (!r) return {{}, {2, "keystore: no store key in " + d.value}};
        auto mk = parse_master_(r.value); wipe_(r.value); if (!mk) return {{}, mk.err};
        auto t = read_file(tp); std::string tblob = t ? std::move(t.value) : std::string();
        bool tpm_copy = mk.value.kind != mk_kind::plain && !tblob.empty();
        std::string key; error terr;
        if (tpm_copy && (mk.value.kind == mk_kind::tpm || old_pass.empty())) { auto u = tpm_unseal_(tblob); if (u) key = std::move(u.value); else terr = u.err; }
        if (key.empty()) {
            if (mk.value.kind == mk_kind::plain) key = mk.value.payload;
            else if (mk.value.kind == mk_kind::tpm) return {{}, terr};
            else { auto u = fkkp_unlock_(mk.value.payload, old_pass); if (!u) return {{}, u.err}; key = std::move(u.value); }
        }
        wipe_(mk.value.payload);
        bool tpm_on = tpm_requested_();
        auto s = store_(d.value, key, std::string(new_pass), tpm_copy ? tpm_mode::keep : tpm_on ? tpm_mode::seal : tpm_mode::drop, tblob);
        wipe_(key); if (!s) return s;
        set_passphrase(new_pass); return {};
    }
    // Whether TPM wrapping can be used here (tpm2-tss + /dev/tpmrm0 on Linux/FreeBSD, the Platform Crypto Provider
    // on Windows); the error says what is missing.
    static status tpm_available() {
#if defined(FORGE_OS_KS_TSS_)
        auto& a = tss_api::get(); if (!a.ok) return {{}, unsupported("keystore TPM: tpm2-tss (libtss2-esys.so.0, libtss2-mu.so.0, libtss2-tctildr.so.0)")};
        if (!getenv("FORGE_KEYSTORE_TPM_TCTI").empty()) return {};
        if (!exists("/dev/tpmrm0")) return {{}, unsupported("keystore TPM: no /dev/tpmrm0")};
        if (::access("/dev/tpmrm0", R_OK | W_OK)) return {{}, {unsupported_code, "keystore TPM: /dev/tpmrm0 is not accessible (add the user to the tss group): " + error_text(errno)}};
        return {};
#elif defined(FORGE_OS_WINDOWS)
        auto& a = ncrypt_api::get(); if (!a.ok) return {{}, unsupported("keystore TPM: ncrypt.dll")};
        std::uintptr_t prov = 0; long rc = a.open_provider(&prov, L"Microsoft Platform Crypto Provider", 0);
        if (rc) return {{}, {unsupported_code, "keystore TPM: Microsoft Platform Crypto Provider unavailable (" + hex_(std::uint32_t(rc)) + ")"}};
        a.free_obj(prov); return {};
#else
        return {{}, unsupported("keystore TPM wrapping")};
#endif
    }
    // Unseals the store key and removes the TPM copy (.master.tpm and, on Windows, the NCrypt key); `.master`
    // keeps or gets the passphrase wrapping if a passphrase is available, else becomes a plain (DPAPI on Windows) key.
    static status disable_tpm() {
        auto d = dir_(); if (!d) return {{}, d.err};
        auto p = join(d.value, ".master"), tp = join(d.value, ".master.tpm");
        auto t = read_file(tp); if (!t) return {};
        auto r = read_file(p); if (!r) { tpm_destroy_(t.value); return os::remove(tp); }
        auto mk = parse_master_(r.value); wipe_(r.value); if (!mk) return {{}, mk.err};
        if (mk.value.kind == mk_kind::pass) { wipe_(mk.value.payload); tpm_destroy_(t.value); return os::remove(tp); }
        if (mk.value.kind == mk_kind::plain) { wipe_(mk.value.payload); tpm_destroy_(t.value); return os::remove(tp); }   // stale copy
        auto u = tpm_unseal_(t.value); if (!u) return {{}, u.err};
        auto pass = passphrase_(); if (pass.empty()) pass = provided_passphrase_();
        auto s = store_(d.value, u.value, pass, tpm_mode::drop, t.value); wipe_(u.value); wipe_(pass); return s;
    }
    static status put(const std::string& service, const std::string& account, const std::string& secret) {
        if (auto c = custom_copy_()) return c->put(service, account, secret);
        if (force_file_()) return file_put_(service, account, secret);
#if defined(__APPLE__)
        return kc_put_(service, account, secret);
#elif defined(FORGE_OS_WINDOWS)
        auto blob = dpapi_(secret, true); if (!blob) return {{}, blob.err};
        auto d = dir_(); if (!d) return {{}, d.err}; if (auto m = ensure_dir_(d.value); !m) return m;
        return replace_(d.value, join(d.value, file_name_(service, account)), blob.value);
#else
#if defined(FORGE_OS_KS_LIBSECRET_)
        if (use_libsecret_()) {
            auto& s = secret_api::get(); void* err = nullptr;
            if (s.store(s.schema(), nullptr, ("forge " + service).c_str(), secret.c_str(), nullptr, &err, "service", service.c_str(), "account", account.c_str(), nullptr) && !err) return {};
            return {{}, s.take_error(err, "libsecret store")};
        }
#endif
        return file_put_(service, account, secret);
#endif
    }
    static result<std::string> get(const std::string& service, const std::string& account) {
        if (auto c = custom_copy_()) return c->get(service, account);
        if (force_file_()) return file_get_(service, account);
#if defined(__APPLE__)
        return kc_get_(service, account);
#elif defined(FORGE_OS_WINDOWS)
        auto d = dir_(); if (!d) return {{}, d.err};
        auto r = read_file(join(d.value, file_name_(service, account))); if (!r) return {{}, {2, "keystore: no such secret"}};
        return dpapi_(r.value, false);
#else
#if defined(FORGE_OS_KS_LIBSECRET_)
        if (use_libsecret_()) {
            auto& s = secret_api::get(); void* err = nullptr; char* v = s.lookup(s.schema(), nullptr, &err, "service", service.c_str(), "account", account.c_str(), nullptr);
            if (err) { if (v) s.free_pw(v); return {{}, s.take_error(err, "libsecret lookup")}; }
            if (!v) return {{}, {2, "keystore: no such secret"}};
            std::string out = v; s.free_pw(v); return {out, {}};
        }
#endif
        return file_get_(service, account);
#endif
    }
    static status remove(const std::string& service, const std::string& account) {
        if (auto c = custom_copy_()) return c->remove(service, account);
        if (force_file_()) return file_remove_(service, account);
#if defined(__APPLE__)
        return kc_remove_(service, account);
#else
#if defined(FORGE_OS_KS_LIBSECRET_)
        if (use_libsecret_()) {
            auto& s = secret_api::get(); void* err = nullptr; s.clear(s.schema(), nullptr, &err, "service", service.c_str(), "account", account.c_str(), nullptr);
            if (err) return {{}, s.take_error(err, "libsecret clear")}; return {};
        }
#endif
        return file_remove_(service, account);
#endif
    }
private:
    static std::mutex& m_() { static std::mutex m; return m; }
    static std::unique_ptr<keystore_backend>& custom_() { static std::unique_ptr<keystore_backend> b; return b; }
    static std::unique_ptr<keystore_backend> custom_copy_() { std::lock_guard<std::mutex> g(m_()); return custom_() ? std::make_unique<keystore_backend>(*custom_()) : nullptr; }
    static bool force_file_() { return getenv("FORGE_KEYSTORE") == "file"; }
    static result<std::string> dir_() {
        if (auto d = getenv("FORGE_KEYSTORE_DIR"); !d.empty()) return {d, {}};
#if defined(FORGE_OS_WINDOWS)
        auto b = getenv("LOCALAPPDATA"); if (b.empty()) return {{}, {EINVAL, "keystore: LOCALAPPDATA is not set (set FORGE_KEYSTORE_DIR)"}};
        return {join(b, "forge/keys"), {}};
#elif defined(__ANDROID__)
        return {{}, {EINVAL, "keystore: set FORGE_KEYSTORE_DIR to the app's files dir or install keystore::set_backend() from JNI"}};
#else
        auto x = getenv("XDG_DATA_HOME"); return {join(x.empty() || x[0] != '/' ? join(home_dir(), ".local/share") : x, "forge/keys"), {}};
#endif
    }
    // Creates the store directory 0700 when missing; an existing directory's mode is left alone.
    static status ensure_dir_(const std::string& d) {
        if (stat(d).is_dir) return {};
        if (auto m = mkdirs(dirname(d)); !m) return m;
#if defined(FORGE_OS_POSIX)
        if (::mkdir(d.c_str(), 0700) && errno != EEXIST) return {{}, last_error("mkdir " + d)};
        return {};
#else
        return mkdir(d);
#endif
    }
    static std::string file_name_(const std::string& s, const std::string& a) {   // FNV-1a of both parts, hex
        std::uint64_t h = 1469598103934665603ull; for (char c : s + '\x1f' + a) { h ^= std::uint8_t(c); h *= 1099511628211ull; }
        char b[24]; std::snprintf(b, sizeof b, "%016llx.key", (unsigned long long)h); return b;
    }
    static std::string tmp_name_(const std::string& d, const std::string& base) {
        std::uint64_t r = 0; entropy(&r, sizeof r); char b[48]; std::snprintf(b, sizeof b, ".%llu.%016llx.tmp", (unsigned long long)pid(), (unsigned long long)r);
        return join(d, base + b);
    }
    // New file with owner-only access from the first byte (O_EXCL, 0600, no symlink follow; CREATE_NEW on
    // Windows, where the directory ACL applies), written and flushed.
    static status create_excl_(const std::string& p, std::string_view data) {
#if defined(FORGE_OS_WINDOWS)
        HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {{}, last_error("create " + p)};
        std::size_t off = 0; bool ok = true;
        while (ok && off < data.size()) { DWORD w = 0; ok = WriteFile(h, data.data() + off, DWORD(std::min<std::size_t>(data.size() - off, 1u << 30)), &w, nullptr) && w; off += w; }
        ok = ok && FlushFileBuffers(h); auto e = ok ? error{} : last_error("write " + p); CloseHandle(h);
        if (!ok) { DeleteFileW(widen(p).c_str()); return {{}, e}; }
        return {};
#else
        int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600); if (fd < 0) return {{}, last_error("create " + p)};
        std::size_t off = 0;
        while (off < data.size()) {
            auto w = ::write(fd, data.data() + off, data.size() - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { auto e = last_error("write " + p); ::close(fd); ::unlink(p.c_str()); return {{}, e}; }
            off += std::size_t(w);
        }
        if (::fsync(fd)) { auto e = last_error("fsync " + p); ::close(fd); ::unlink(p.c_str()); return {{}, e}; }
        ::close(fd); return {};
#endif
    }
    static status replace_(const std::string& d, const std::string& p, std::string_view data) {   // atomic replace
        auto tmp = tmp_name_(d, basename(p)); if (auto w = create_excl_(tmp, data); !w) return w;
        if (auto r = rename(tmp, p); !r) { os::remove(tmp); return r; }
        return {};
    }
    // Publish tmp at p only if p does not exist. true = published, false = someone else's p already exists.
    static result<bool> publish_excl_(const std::string& tmp, const std::string& p, std::string_view data) {
#if defined(FORGE_OS_WINDOWS)
        (void)data;
        if (MoveFileExW(widen(tmp).c_str(), widen(p).c_str(), MOVEFILE_WRITE_THROUGH)) return {true, {}};
        auto e = GetLastError(); DeleteFileW(widen(tmp).c_str());
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return {false, {}};
        return {false, {int(e), "keystore: publish " + p + ": " + error_text(int(e))}};
#else
        int rc = ::link(tmp.c_str(), p.c_str()); int e = rc ? errno : 0; ::unlink(tmp.c_str());
        if (!rc) return {true, {}};
        if (e == EEXIST) return {false, {}};
        // No hard links here (some FUSE/Android mounts): create in place; readers retry a short file.
        if (auto w = create_excl_(p, data); !w) { if (w.err.code == EEXIST) return {false, {}}; return {false, w.err}; }
        return {true, {}};
#endif
    }
    // ---- store key wrapping
    static void wipe_(std::string& s) { if (!s.empty()) crypto_wipe(s.data(), s.size()); }
    static std::string hex_(std::uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08x", unsigned(v)); return b; }
    struct pass_state { std::string pass; std::function<std::string()> provider; std::map<std::string, locked_buffer> cache; keystore_kdf kdf; };
    static pass_state& ps_() { static pass_state s; return s; }
    // The unwrapped store keys live in locked, non-dumpable pages (FSS-CRY-2), wiped when dropped.
    static void wipe_cache_() { ps_().cache.clear(); }   // caller holds m_()
    static bool tpm_requested_() { return getenv("FORGE_KEYSTORE_TPM") == "1"; }
    // API passphrase, else FORGE_KEYSTORE_PASSPHRASE (the provider is asked only when a key must be unwrapped/wrapped).
    static std::string passphrase_() { { std::lock_guard<std::mutex> g(m_()); if (!ps_().pass.empty()) return ps_().pass; } return getenv("FORGE_KEYSTORE_PASSPHRASE"); }
    static bool has_provider_() { std::lock_guard<std::mutex> g(m_()); return bool(ps_().provider); }
    static std::string provided_passphrase_() {
        std::function<std::string()> f; { std::lock_guard<std::mutex> g(m_()); f = ps_().provider; }
        return f ? f() : std::string();
    }
    static std::string file_name_backend_() {
        std::string n = "file";
        if (tpm_requested_() && tpm_available()) n += "+tpm";
        if (!passphrase_().empty() || has_provider_()) n += "+argon2id";
        return n;
    }
    static std::string cache_tag_(const std::string& d, const std::string& m, const std::string& t, const std::string& pass) {
        crypto_blake2b_ctx c; crypto_blake2b_init(&c, 32); std::uint8_t h[32];
        for (auto* s : {&d, &m, &t, &pass}) { std::uint64_t n = s->size(); crypto_blake2b_update(&c, reinterpret_cast<const std::uint8_t*>(&n), 8); crypto_blake2b_update(&c, reinterpret_cast<const std::uint8_t*>(s->data()), s->size()); }
        crypto_blake2b_final(&c, h); return std::string(reinterpret_cast<char*>(h), 32);
    }
    static std::string cache_get_(const std::string& tag) {
        std::lock_guard<std::mutex> g(m_()); auto it = ps_().cache.find(tag);
        return it == ps_().cache.end() ? std::string() : std::string(reinterpret_cast<const char*>(it->second.data()), it->second.size());
    }
    static void cache_put_(const std::string& tag, const std::string& key) {
        std::lock_guard<std::mutex> g(m_()); auto& c = ps_().cache; if (c.size() >= 16) wipe_cache_(); c[tag] = locked_buffer(key.data(), key.size());
    }
    static void put_u32_(char* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = char(v >> (8 * i)); }
    static std::uint32_t get_u32_(const char* p) { std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= std::uint32_t(std::uint8_t(p[i])) << (8 * i); return v; }
    // The passphrase-derived key encryption key, in locked memory.
    static result<locked_buffer> argon2id_(std::string_view pass, const std::uint8_t* salt, const keystore_kdf& k) {
        if (!k.valid()) return {{}, {EINVAL, "keystore: Argon2 parameters out of range"}};
        std::size_t bytes = std::size_t(k.memory_kib) * 1024; void* wa = std::malloc(bytes);
        if (!wa) return {{}, {ENOMEM, "keystore: no memory for Argon2 (" + std::to_string(k.memory_kib) + " KiB)"}};
        crypto_argon2_config cfg{CRYPTO_ARGON2_ID, k.memory_kib, k.passes, k.lanes};
        crypto_argon2_inputs in{reinterpret_cast<const std::uint8_t*>(pass.data()), salt, std::uint32_t(pass.size()), 16};
        locked_buffer out(32); crypto_argon2(out.data(), 32, wa, cfg, in, crypto_argon2_no_extras);
        crypto_wipe(wa, bytes); std::free(wa); return {std::move(out), {}};
    }
    // Passphrase-wrapped store key: "FKKP" | m_kib t lanes (u32 LE) | salt[16] | nonce[24] | mac[16] | key[32],
    // the first 32 bytes (magic, parameters, salt) authenticated as associated data.
    static constexpr std::size_t fkkp_size_ = 104;
    static result<std::string> fkkp_lock_(const std::string& key, std::string_view pass) {
        auto k = kdf(); std::string out(fkkp_size_, '\0'); std::memcpy(out.data(), "FKKP", 4);
        put_u32_(&out[4], k.memory_kib); put_u32_(&out[8], k.passes); put_u32_(&out[12], k.lanes);
        auto* u = reinterpret_cast<std::uint8_t*>(out.data());
        if (!entropy(u + 16, 16 + 24)) return {{}, {EIO, "keystore: no entropy"}};
        auto kek = argon2id_(pass, u + 16, k); if (!kek) return {{}, kek.err};
        crypto_aead_lock(u + 72, u + 56, kek.value.data(), u + 32, u, 32, reinterpret_cast<const std::uint8_t*>(key.data()), 32);
        kek.value.reset(); return {std::move(out), {}};
    }
    static result<std::string> fkkp_unlock_(const std::string& blob, std::string_view pass) {
        if (pass.empty()) return {{}, {EACCES, "keystore: the store key is passphrase-protected; set FORGE_KEYSTORE_PASSPHRASE or call keystore::set_passphrase()"}};
        keystore_kdf k; k.memory_kib = get_u32_(&blob[4]); k.passes = get_u32_(&blob[8]); k.lanes = get_u32_(&blob[12]);
        if (!k.valid()) return {{}, {EACCES, "keystore: store key header has out-of-range Argon2 parameters (tampered)"}};
        auto* u = reinterpret_cast<const std::uint8_t*>(blob.data());
        auto kek = argon2id_(pass, u + 16, k); if (!kek) return {{}, kek.err};
        std::string key(32, '\0');
        int bad = crypto_aead_unlock(reinterpret_cast<std::uint8_t*>(key.data()), u + 56, kek.value.data(), u + 32, u, 32, u + 72, 32);
        kek.value.reset();
        if (bad) return {{}, {EACCES, "keystore: wrong passphrase or tampered store key (authentication failed)"}};
        return {std::move(key), {}};
    }
    // `.master` forms. POSIX: "FKK1"+key | FKKP blob | "FKKT" (key only in .master.tpm).
    // Windows: "FKKD"+DPAPI(key or FKKP blob) | "FKKT".
    enum class mk_kind { plain, pass, tpm };
    struct mk_t { mk_kind kind = mk_kind::plain; std::string payload; };   // plain: the key; pass: the FKKP blob
    static result<mk_t> parse_master_(const std::string& f) {
        if (f == "FKKT") return {{mk_kind::tpm, {}}, {}};
#if defined(FORGE_OS_WINDOWS)
        if (f.size() > 4 && f.compare(0, 4, "FKKD") == 0) {
            auto in = dpapi_(f.substr(4), false);
            if (in && in.value.size() == 32) return {{mk_kind::plain, std::move(in.value)}, {}};
            if (in && in.value.size() == fkkp_size_ && in.value.compare(0, 4, "FKKP") == 0) return {{mk_kind::pass, std::move(in.value)}, {}};
            if (in) wipe_(in.value);
        }
#else
        if (f.size() == 36 && f.compare(0, 4, "FKK1") == 0) return {{mk_kind::plain, f.substr(4)}, {}};
        if (f.size() == fkkp_size_ && f.compare(0, 4, "FKKP") == 0) return {{mk_kind::pass, f}, {}};
#endif
        return {{}, {EIO, "keystore: store key is corrupt"}};
    }
    static result<std::string> encode_master_(const std::string& key, const std::string& pass, bool tpm) {
        if (pass.empty() && tpm) return {"FKKT", {}};
        std::string inner = key;
        if (!pass.empty()) { auto w = fkkp_lock_(key, pass); wipe_(inner); if (!w) return w; inner = std::move(w.value); }
#if defined(FORGE_OS_WINDOWS)
        auto b = dpapi_(inner, true); wipe_(inner); if (!b) return {{}, b.err}; return {"FKKD" + b.value, {}};
#else
        if (pass.empty()) { auto r = "FKK1" + inner; wipe_(inner); return {std::move(r), {}}; }
        return {std::move(inner), {}};
#endif
    }
    enum class tpm_mode { drop, keep, seal };
    // Rewrite `.master` (and `.master.tpm`) for an already-unwrapped key; each file is replaced atomically, the
    // TPM copy first, so a reader always finds a usable combination.
    static status store_(const std::string& d, const std::string& key, const std::string& pass, tpm_mode mode, const std::string& old_tblob) {
        auto p = join(d, ".master"), tp = join(d, ".master.tpm");
        if (mode == tpm_mode::seal) {
            auto s = tpm_seal_(key, old_tblob); if (!s) return {{}, s.err};
            if (auto w = replace_(d, tp, s.value); !w) return w;
        }
        auto enc = encode_master_(key, pass, mode != tpm_mode::drop); if (!enc) return {{}, enc.err};
        auto w = replace_(d, p, enc.value); wipe_(enc.value); if (!w) return w;
        if (mode == tpm_mode::drop && exists(tp)) { if (!old_tblob.empty()) tpm_destroy_(old_tblob); return os::remove(tp); }
        return {};
    }
    // First key in a store: published exclusively, so concurrent creators converge on one key. With the TPM the
    // sealed copy is written first and renamed into place only by the winner.
    static status create_key_(const std::string& d, bool tpm_on) {
        auto p = join(d, ".master"), tp = join(d, ".master.tpm");
        std::string k(32, '\0'); if (!entropy(k.data(), k.size())) return {{}, {EIO, "keystore: no entropy"}};
        auto pass = passphrase_(); if (pass.empty()) pass = provided_passphrase_();
        std::string sealed, ttmp;
        if (tpm_on) { auto s = tpm_seal_(k, {}); if (!s) { wipe_(k); wipe_(pass); return {{}, s.err}; } sealed = std::move(s.value); }
        auto w = encode_master_(k, pass, tpm_on); wipe_(k); wipe_(pass); if (!w) { if (tpm_on) tpm_destroy_(sealed); return {{}, w.err}; }
        if (tpm_on) { ttmp = tmp_name_(d, ".master.tpm"); if (auto c = create_excl_(ttmp, sealed); !c) { tpm_destroy_(sealed); wipe_(w.value); return c; } }
        auto tmp = tmp_name_(d, ".master");
        auto c = create_excl_(tmp, w.value); if (!c) { wipe_(w.value); if (tpm_on) { os::remove(ttmp); tpm_destroy_(sealed); } return c; }
        auto pub = publish_excl_(tmp, p, w.value); wipe_(w.value);
        if (tpm_on) {
            if (pub && pub.value) { if (auto r = rename(ttmp, tp); !r) return r; }
            else { os::remove(ttmp); tpm_destroy_(sealed); }
        }
        if (!pub && pub.err.code) return {{}, pub.err};
        return {};   // published or lost the race: the caller reads back whichever key won
    }
    // The store key: read and unwrap it (TPM copy first, then the passphrase), wrapping it further when a
    // passphrase or the TPM is newly configured; or create it.
    static result<std::string> key_(const std::string& d) {
        auto p = join(d, ".master"), tp = join(d, ".master.tpm"); error last{EIO, "keystore: store key is corrupt"};
        bool tpm_on = tpm_requested_();
        if (tpm_on) if (auto a = tpm_available(); !a) return {{}, {a.err.code, a.err.what + " (FORGE_KEYSTORE_TPM=1)"}};
        auto nap = [] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); };
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto r = read_file(p);
            if (!r) {
                // Exists but unreadable: on Windows a scanner or a concurrent rename can hold it briefly; retry.
                if (exists(p)) { last = last_error("keystore: read " + p); nap(); continue; }
                if (auto c = create_key_(d, tpm_on); !c) return {{}, c.err};
                continue;
            }
            auto t = read_file(tp); std::string tblob = t ? std::move(t.value) : std::string();
            std::string pass = passphrase_();
            auto tag = cache_tag_(d, r.value, tblob, pass);
            if (auto c = cache_get_(tag); !c.empty()) { wipe_(r.value); wipe_(pass); return {std::move(c), {}}; }
            auto mk = parse_master_(r.value); wipe_(r.value);
            if (!mk) { last = mk.err; nap(); continue; }   // maybe still being written
            auto kind = mk.value.kind;
            bool tpm_copy = kind != mk_kind::plain && !tblob.empty();
            std::string key; error terr;
            if (tpm_copy && (kind == mk_kind::tpm || tpm_on)) { auto u = tpm_unseal_(tblob); if (u) key = std::move(u.value); else terr = u.err; }
            if (key.empty()) {
                if (kind == mk_kind::plain) key = std::move(mk.value.payload);
                else if (kind == mk_kind::tpm) {
                    if (tblob.empty()) { last = {EIO, "keystore: the TPM-sealed store key (.master.tpm) is missing"}; nap(); continue; }
                    return {{}, {terr.code ? terr.code : EIO, terr.what + " (no passphrase copy to recover from)"}};
                } else {
                    if (pass.empty()) pass = provided_passphrase_();
                    auto u = fkkp_unlock_(mk.value.payload, pass);
                    if (!u) { wipe_(pass); wipe_(mk.value.payload); if (terr) u.err.what += " (TPM: " + terr.what + ")"; return {{}, u.err}; }
                    key = std::move(u.value);
                }
            }
            wipe_(mk.value.payload);
            if (pass.empty() && kind != mk_kind::pass && has_provider_()) pass = provided_passphrase_();
            bool want_seal = tpm_on && !tpm_copy;
            if ((!pass.empty() && kind != mk_kind::pass) || want_seal) {   // migrate: plain -> wrapped, add a TPM copy
                auto s = store_(d, key, pass, want_seal ? tpm_mode::seal : tpm_copy ? tpm_mode::keep : tpm_mode::drop, tblob);
                if (!s) { wipe_(key); wipe_(pass); return {{}, s.err}; }
            } else cache_put_(tag, key);
            wipe_(pass); return {std::move(key), {}};
        }
        return {{}, last};
    }
#if defined(FORGE_OS_KS_TSS_)
    // tpm2-tss, loaded at run time. Structures are built in TPM wire format and unmarshalled by libtss2-mu into
    // opaque buffers, so no TSS headers or struct layouts are needed here.
    struct tss_api {
        using rc_t = std::uint32_t; using tr = std::uint32_t;
        rc_t (*ldr_init)(const char*, void**) = nullptr; void (*ldr_fini)(void**) = nullptr;
        rc_t (*init)(void**, void*, void*) = nullptr; void (*fini)(void**) = nullptr; rc_t (*startup)(void*, std::uint16_t) = nullptr;
        rc_t (*create_primary)(void*, tr, tr, tr, tr, const void*, const void*, const void*, const void*, tr*, void**, void**, void**, void**) = nullptr;
        rc_t (*create)(void*, tr, tr, tr, tr, const void*, const void*, const void*, const void*, void**, void**, void**, void**, void**) = nullptr;
        rc_t (*load)(void*, tr, tr, tr, tr, const void*, const void*, tr*) = nullptr;
        rc_t (*unseal)(void*, tr, tr, tr, tr, void**) = nullptr; rc_t (*flush)(void*, tr) = nullptr; void (*free_)(void*) = nullptr;
        using um_t = rc_t (*)(const std::uint8_t*, std::size_t, std::size_t*, void*); using m_t = rc_t (*)(const void*, std::uint8_t*, std::size_t, std::size_t*);
        um_t pub_um = nullptr, priv_um = nullptr, sens_um = nullptr, data_um = nullptr, pcr_um = nullptr; m_t pub_m = nullptr, priv_m = nullptr, sdata_m = nullptr;
        const char* (*decode)(rc_t) = nullptr; bool ok = false;
        static constexpr tr none = 0xfff, password = 0x0ff, owner = 0x101;
        static tss_api& get() {
            static tss_api a = [] {
                tss_api x; void* e = ::dlopen("libtss2-esys.so.0", RTLD_LAZY); void* m = ::dlopen("libtss2-mu.so.0", RTLD_LAZY); void* l = ::dlopen("libtss2-tctildr.so.0", RTLD_LAZY);
                if (!e || !m || !l) return x;
                auto f = [](void* h, const char* n, auto& p) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(::dlsym(h, n)); return p != nullptr; };
                bool ok = f(l, "Tss2_TctiLdr_Initialize", x.ldr_init) & f(l, "Tss2_TctiLdr_Finalize", x.ldr_fini) & f(e, "Esys_Initialize", x.init) & f(e, "Esys_Finalize", x.fini)
                    & f(e, "Esys_Startup", x.startup) & f(e, "Esys_CreatePrimary", x.create_primary) & f(e, "Esys_Create", x.create) & f(e, "Esys_Load", x.load)
                    & f(e, "Esys_Unseal", x.unseal) & f(e, "Esys_FlushContext", x.flush) & f(e, "Esys_Free", x.free_)
                    & f(m, "Tss2_MU_TPM2B_PUBLIC_Unmarshal", x.pub_um) & f(m, "Tss2_MU_TPM2B_PRIVATE_Unmarshal", x.priv_um) & f(m, "Tss2_MU_TPM2B_SENSITIVE_CREATE_Unmarshal", x.sens_um)
                    & f(m, "Tss2_MU_TPM2B_DATA_Unmarshal", x.data_um) & f(m, "Tss2_MU_TPML_PCR_SELECTION_Unmarshal", x.pcr_um) & f(m, "Tss2_MU_TPM2B_PUBLIC_Marshal", x.pub_m)
                    & f(m, "Tss2_MU_TPM2B_PRIVATE_Marshal", x.priv_m) & f(m, "Tss2_MU_TPM2B_SENSITIVE_DATA_Marshal", x.sdata_m);
                if (void* r = ::dlopen("libtss2-rc.so.0", RTLD_LAZY)) f(r, "Tss2_RC_Decode", x.decode);
                x.ok = ok; return x;
            }();
            return a;
        }
        error err(const char* what, rc_t rc) const { return {EIO, std::string("keystore TPM: ") + what + " failed: " + (decode ? decode(rc) : hex_(rc))}; }
    };
    struct wire_ {
        std::string b;
        void u16(unsigned v) { b += char((v >> 8) & 0xff); b += char(v & 0xff); }
        void u32(std::uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
        void zeros(std::size_t n) { b.append(n, '\0'); }
        std::string tpm2b() const { wire_ w; w.u16(unsigned(b.size())); return w.b + b; }
    };
    struct opaque_ { alignas(16) unsigned char b[8192] = {}; ~opaque_() { crypto_wipe(b, sizeof b); } };
    // SRK-style primary under the owner hierarchy (TCG template: restricted decrypt, AES-128-CFB, noDA).
    static std::string srk_template_(int alg) {
        wire_ t; t.u16(alg == 1 ? 0x0023 : 0x0001); t.u16(0x000B); t.u32(0x00030472); t.u16(0);
        t.u16(0x0006); t.u16(128); t.u16(0x0043);
        if (alg == 1) { t.u16(0x0010); t.u16(0x0003); t.u16(0x0010); t.u16(32); t.zeros(32); t.u16(32); t.zeros(32); }
        else { t.u16(0x0010); t.u16(2048); t.u32(0); t.u16(256); t.zeros(256); }
        return t.tpm2b();
    }
    struct tpm_session_ {
        tss_api& a = tss_api::get(); void* tcti = nullptr; void* esys = nullptr; std::uint32_t primary = tss_api::none;
        ~tpm_session_() { if (primary != tss_api::none) a.flush(esys, primary); if (esys) a.fini(&esys); if (tcti) a.ldr_fini(&tcti); }
        error open() {
            auto conf = getenv("FORGE_KEYSTORE_TPM_TCTI");
            if (auto rc = a.ldr_init(conf.empty() ? "device:/dev/tpmrm0" : conf.c_str(), &tcti)) return a.err("TCTI init", rc);
            if (auto rc = a.init(&esys, tcti, nullptr)) return a.err("Esys_Initialize", rc);
            if (!conf.empty()) a.startup(esys, 0x0000);   // simulators start cold; TPM_RC_INITIALIZE otherwise
            return {};
        }
        error make_primary(int alg) {
            opaque_ sens, pub, out, pcr; std::size_t off = 0; wire_ s; s.u16(4); s.u16(0); s.u16(0);
            auto tpl = srk_template_(alg); const std::uint8_t z[4] = {};
            if (a.sens_um(reinterpret_cast<const std::uint8_t*>(s.b.data()), s.b.size(), &off, sens.b)) return {EIO, "keystore TPM: marshal"};
            off = 0; if (a.pub_um(reinterpret_cast<const std::uint8_t*>(tpl.data()), tpl.size(), &off, pub.b)) return {EIO, "keystore TPM: marshal"};
            off = 0; if (a.data_um(z, 2, &off, out.b)) return {EIO, "keystore TPM: marshal"};
            off = 0; if (a.pcr_um(z, 4, &off, pcr.b)) return {EIO, "keystore TPM: marshal"};
            if (auto rc = a.create_primary(esys, tss_api::owner, tss_api::password, tss_api::none, tss_api::none, sens.b, pub.b, out.b, pcr.b, &primary, nullptr, nullptr, nullptr, nullptr)) {
                primary = tss_api::none; return a.err("CreatePrimary", rc);
            }
            return {};
        }
    };
    static result<std::string> marshal_(tss_api::m_t m, const void* obj) {
        std::string b(8192, '\0'); std::size_t off = 0;
        if (m(obj, reinterpret_cast<std::uint8_t*>(b.data()), b.size(), &off)) return {{}, {EIO, "keystore TPM: marshal"}};
        b.resize(off); return {std::move(b), {}};
    }
    // `.master.tpm` (Linux/FreeBSD): "FKTL" | alg (1 ECC, 2 RSA) | u32 len + TPM2B_PUBLIC | u32 len + TPM2B_PRIVATE.
    static result<std::string> tpm_seal_(const std::string& key, const std::string&) {
        if (auto av = tpm_available(); !av) return {{}, av.err};
        tpm_session_ s; if (auto e = s.open()) return {{}, e};
        int alg = 1; error e = s.make_primary(1); if (e) { alg = 2; if (auto e2 = s.make_primary(2)) return {{}, e}; }
        auto& a = s.a; opaque_ sens, pub, out, pcr; std::size_t off = 0;
        wire_ in; in.u16(0); in.u16(32); in.b += key; auto sw = in.tpm2b(); wipe_(in.b);
        wire_ t; t.u16(0x0008); t.u16(0x000B); t.u32(0x00000452); t.u16(0); t.u16(0x0010); t.u16(0); auto tpl = t.tpm2b();
        const std::uint8_t z[4] = {};
        bool bad = a.sens_um(reinterpret_cast<const std::uint8_t*>(sw.data()), sw.size(), &off, sens.b); wipe_(sw);
        off = 0; bad |= a.pub_um(reinterpret_cast<const std::uint8_t*>(tpl.data()), tpl.size(), &off, pub.b) != 0;
        off = 0; bad |= a.data_um(z, 2, &off, out.b) != 0; off = 0; bad |= a.pcr_um(z, 4, &off, pcr.b) != 0;
        if (bad) return {{}, {EIO, "keystore TPM: marshal"}};
        void *opriv = nullptr, *opub = nullptr;
        if (auto rc = a.create(s.esys, s.primary, tss_api::password, tss_api::none, tss_api::none, sens.b, pub.b, out.b, pcr.b, &opriv, &opub, nullptr, nullptr, nullptr)) return {{}, a.err("Create (seal)", rc)};
        auto mp = marshal_(a.pub_m, opub), mv = marshal_(a.priv_m, opriv); a.free_(opub); a.free_(opriv);
        if (!mp || !mv) return {{}, {EIO, "keystore TPM: marshal"}};
        std::string r = "FKTL"; r += char(alg);
        char n[4]; put_u32_(n, std::uint32_t(mp.value.size())); r.append(n, 4); r += mp.value;
        put_u32_(n, std::uint32_t(mv.value.size())); r.append(n, 4); r += mv.value;
        return {std::move(r), {}};
    }
    static result<std::string> tpm_unseal_(const std::string& blob) {
        if (blob.size() < 13 || blob.compare(0, 4, "FKTL") != 0 || (blob[4] != 1 && blob[4] != 2)) return {{}, {EIO, "keystore TPM: .master.tpm is corrupt"}};
        std::size_t pl = get_u32_(&blob[5]); if (9 + pl + 4 > blob.size()) return {{}, {EIO, "keystore TPM: .master.tpm is corrupt"}};
        std::size_t vl = get_u32_(&blob[9 + pl]); if (13 + pl + vl != blob.size()) return {{}, {EIO, "keystore TPM: .master.tpm is corrupt"}};
        if (auto av = tpm_available(); !av) return {{}, av.err};
        tpm_session_ s; if (auto e = s.open()) return {{}, e};
        if (auto e = s.make_primary(blob[4])) return {{}, e};
        auto& a = s.a; opaque_ pub, priv; std::size_t off = 0;
        if (a.pub_um(reinterpret_cast<const std::uint8_t*>(blob.data() + 9), pl, &off, pub.b)) return {{}, {EIO, "keystore TPM: .master.tpm is corrupt"}};
        off = 0; if (a.priv_um(reinterpret_cast<const std::uint8_t*>(blob.data() + 13 + pl), vl, &off, priv.b)) return {{}, {EIO, "keystore TPM: .master.tpm is corrupt"}};
        std::uint32_t obj = tss_api::none;
        if (auto rc = a.load(s.esys, s.primary, tss_api::password, tss_api::none, tss_api::none, priv.b, pub.b, &obj)) return {{}, a.err("Load (sealed key; TPM cleared or replaced?)", rc)};
        void* outd = nullptr; auto rc = a.unseal(s.esys, obj, tss_api::password, tss_api::none, tss_api::none, &outd); a.flush(s.esys, obj);
        if (rc) return {{}, a.err("Unseal", rc)};
        auto m = marshal_(a.sdata_m, outd); crypto_wipe(outd, 2 + 128); a.free_(outd);
        if (!m || m.value.size() != 34 || std::uint8_t(m.value[0]) != 0 || std::uint8_t(m.value[1]) != 32) { if (m) wipe_(m.value); return {{}, {EIO, "keystore TPM: unsealed data is not a store key"}}; }
        std::string key = m.value.substr(2); wipe_(m.value); return {std::move(key), {}};
    }
    static void tpm_destroy_(const std::string&) {}   // sealed objects live only in .master.tpm
#elif defined(FORGE_OS_WINDOWS)
    // NCrypt, loaded at run time: a persisted non-exportable RSA-2048 key in the Platform Crypto Provider (TPM).
    struct ncrypt_api {
        using h = std::uintptr_t;
        long (WINAPI* open_provider)(h*, const wchar_t*, DWORD) = nullptr; long (WINAPI* open_key)(h, h*, const wchar_t*, DWORD, DWORD) = nullptr;
        long (WINAPI* create_key)(h, h*, const wchar_t*, const wchar_t*, DWORD, DWORD) = nullptr; long (WINAPI* set_prop)(h, const wchar_t*, BYTE*, DWORD, DWORD) = nullptr;
        long (WINAPI* finalize)(h, DWORD) = nullptr; long (WINAPI* encrypt)(h, BYTE*, DWORD, void*, BYTE*, DWORD, DWORD*, DWORD) = nullptr;
        long (WINAPI* decrypt)(h, BYTE*, DWORD, void*, BYTE*, DWORD, DWORD*, DWORD) = nullptr; long (WINAPI* del)(h, DWORD) = nullptr; long (WINAPI* free_obj)(h) = nullptr;
        bool ok = false;
        static ncrypt_api& get() {
            static ncrypt_api a = [] {
                ncrypt_api x; HMODULE m = LoadLibraryExW(L"ncrypt.dll", nullptr, 0x00000800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/); if (!m) return x;
                auto f = [&](const char* n, auto& p) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(reinterpret_cast<void*>(GetProcAddress(m, n))); return p != nullptr; };
                x.ok = f("NCryptOpenStorageProvider", x.open_provider) & f("NCryptOpenKey", x.open_key) & f("NCryptCreatePersistedKey", x.create_key) & f("NCryptSetProperty", x.set_prop)
                    & f("NCryptFinalizeKey", x.finalize) & f("NCryptEncrypt", x.encrypt) & f("NCryptDecrypt", x.decrypt) & f("NCryptDeleteKey", x.del) & f("NCryptFreeObject", x.free_obj);
                return x;
            }();
            return a;
        }
    };
    struct oaep_ { const wchar_t* alg; BYTE* label; ULONG cb; };
    static constexpr DWORD nc_silent_ = 0x40, nc_oaep_ = 0x4;
    // Opens the named key, creating it when missing. `created` tells whether this call made it.
    static result<ncrypt_api::h> nc_key_(ncrypt_api::h prov, const std::string& name, bool create, bool* created) {
        auto& a = ncrypt_api::get(); ncrypt_api::h k = 0; auto wn = widen(name); if (created) *created = false;
        long rc = a.open_key(prov, &k, wn.c_str(), 0, nc_silent_); if (!rc) return {k, {}};
        if (!create) return {0, {EIO, "keystore TPM: key " + name + " not found in the Platform Crypto Provider (" + hex_(std::uint32_t(rc)) + ")"}};
        rc = a.create_key(prov, &k, L"RSA", wn.c_str(), 0, 0);
        if (rc) return {0, {EIO, "keystore TPM: NCryptCreatePersistedKey " + hex_(std::uint32_t(rc))}};
        DWORD bits = 2048; a.set_prop(k, L"Length", reinterpret_cast<BYTE*>(&bits), sizeof bits, 0);
        if ((rc = a.finalize(k, nc_silent_))) { a.free_obj(k); return {0, {EIO, "keystore TPM: NCryptFinalizeKey " + hex_(std::uint32_t(rc))}}; }
        if (created) *created = true; return {k, {}};
    }
    // `.master.tpm` (Windows): "FKTW" | u16 name length | key name (UTF-8) | RSA-OAEP-SHA256 ciphertext.
    static bool nc_parse_(const std::string& blob, std::string& name, std::string& ct) {
        if (blob.size() < 6 || blob.compare(0, 4, "FKTW") != 0) return false;
        std::size_t n = std::uint8_t(blob[4]) | (std::size_t(std::uint8_t(blob[5])) << 8); if (6 + n >= blob.size()) return false;
        name = blob.substr(6, n); ct = blob.substr(6 + n); return true;
    }
    static result<std::string> tpm_seal_(const std::string& key, const std::string& old_blob) {
        if (auto av = tpm_available(); !av) return {{}, av.err};
        auto& a = ncrypt_api::get(); ncrypt_api::h prov = 0;
        if (long rc = a.open_provider(&prov, L"Microsoft Platform Crypto Provider", 0)) return {{}, {unsupported_code, "keystore TPM: Platform Crypto Provider " + hex_(std::uint32_t(rc))}};
        std::string name, oct; result<ncrypt_api::h> k{0, {EIO, ""}}; bool created = false;
        if (nc_parse_(old_blob, name, oct)) k = nc_key_(prov, name, false, nullptr);   // reuse this store's key
        if (!k) {
            std::uint64_t r = 0; entropy(&r, sizeof r); char b[24]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)r);
            auto pre = getenv("FORGE_KEYSTORE_TPM_KEY_PREFIX"); name = (pre.empty() ? std::string("forge-keystore-") : pre) + b;
            k = nc_key_(prov, name, true, &created);
        }
        if (!k) { a.free_obj(prov); return {{}, k.err}; }
        oaep_ pad{L"SHA256", nullptr, 0}; BYTE ct[512]; DWORD n = 0;
        long rc = a.encrypt(k.value, reinterpret_cast<BYTE*>(const_cast<char*>(key.data())), DWORD(key.size()), &pad, ct, sizeof ct, &n, nc_oaep_ | nc_silent_);
        if (rc && created) a.del(k.value, 0); else a.free_obj(k.value);
        a.free_obj(prov);
        if (rc) return {{}, {EIO, "keystore TPM: NCryptEncrypt " + hex_(std::uint32_t(rc))}};
        std::string out = "FKTW"; out += char(name.size() & 0xff); out += char(name.size() >> 8); out += name; out.append(reinterpret_cast<char*>(ct), n);
        return {std::move(out), {}};
    }
    static result<std::string> tpm_unseal_(const std::string& blob) {
        std::string name, ct; if (!nc_parse_(blob, name, ct)) return {{}, {EIO, "keystore TPM: .master.tpm is corrupt"}};
        if (auto av = tpm_available(); !av) return {{}, av.err};
        auto& a = ncrypt_api::get(); ncrypt_api::h prov = 0;
        if (long rc = a.open_provider(&prov, L"Microsoft Platform Crypto Provider", 0)) return {{}, {unsupported_code, "keystore TPM: Platform Crypto Provider " + hex_(std::uint32_t(rc))}};
        auto k = nc_key_(prov, name, false, nullptr); if (!k) { a.free_obj(prov); return {{}, k.err}; }
        oaep_ pad{L"SHA256", nullptr, 0}; BYTE pt[512]; DWORD n = 0;
        long rc = a.decrypt(k.value, reinterpret_cast<BYTE*>(ct.data()), DWORD(ct.size()), &pad, pt, sizeof pt, &n, nc_oaep_ | nc_silent_);
        a.free_obj(k.value); a.free_obj(prov);
        if (rc || n != 32) { SecureZeroMemory(pt, sizeof pt); return {{}, {EIO, "keystore TPM: NCryptDecrypt " + hex_(std::uint32_t(rc))}}; }
        std::string key(reinterpret_cast<char*>(pt), 32); SecureZeroMemory(pt, sizeof pt); return {std::move(key), {}};
    }
    static void tpm_destroy_(const std::string& blob) {   // deletes the store's NCrypt key
        std::string name, ct; if (!nc_parse_(blob, name, ct)) return;
        auto& a = ncrypt_api::get(); if (!a.ok) return; ncrypt_api::h prov = 0;
        if (a.open_provider(&prov, L"Microsoft Platform Crypto Provider", 0)) return;
        if (auto k = nc_key_(prov, name, false, nullptr)) { if (a.del(k.value, 0)) a.free_obj(k.value); }
        a.free_obj(prov);
    }
#else
    static result<std::string> tpm_seal_(const std::string&, const std::string&) { return {{}, unsupported("keystore TPM wrapping")}; }
    static result<std::string> tpm_unseal_(const std::string&) { return {{}, unsupported("keystore TPM wrapping")}; }
    static void tpm_destroy_(const std::string&) {}
#endif
    static std::string ad_(const std::string& s, const std::string& a) { return "forge-ks-v1" + s + '\x1f' + a; }
    static status file_put_(const std::string& s, const std::string& a, const std::string& v) {
        auto d = dir_(); if (!d) return {{}, d.err}; if (auto m = ensure_dir_(d.value); !m) return m;
        auto k = key_(d.value); if (!k) return {{}, k.err};
        std::string out(4 + 24 + 16 + v.size(), '\0'); std::memcpy(out.data(), "FKS1", 4);
        auto* nonce = reinterpret_cast<std::uint8_t*>(out.data() + 4); auto* mac = nonce + 24; auto* ct = mac + 16;
        if (!entropy(nonce, 24)) { crypto_wipe(k.value.data(), 32); return {{}, {EIO, "keystore: no entropy"}}; }
        auto ad = ad_(s, a);
        crypto_aead_lock(ct, mac, reinterpret_cast<const std::uint8_t*>(k.value.data()), nonce, reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size(),
                         reinterpret_cast<const std::uint8_t*>(v.data()), v.size());
        crypto_wipe(k.value.data(), k.value.size());
        return replace_(d.value, join(d.value, file_name_(s, a)), out);
    }
    static result<std::string> file_get_(const std::string& s, const std::string& a) {
        auto d = dir_(); if (!d) return {{}, d.err};
        auto r = read_file(join(d.value, file_name_(s, a))); if (!r) return {{}, {2, "keystore: no such secret"}};
        if (r.value.size() >= 44 && r.value.compare(0, 4, "FKS1") == 0) {
            auto k = key_(d.value); if (!k) return {{}, k.err};
            auto* nonce = reinterpret_cast<const std::uint8_t*>(r.value.data() + 4); auto* mac = nonce + 24; auto* ct = mac + 16;
            std::string pt(r.value.size() - 44, '\0'); auto ad = ad_(s, a);
            int bad = crypto_aead_unlock(reinterpret_cast<std::uint8_t*>(pt.data()), mac, reinterpret_cast<const std::uint8_t*>(k.value.data()), nonce,
                                         reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size(), ct, pt.size());
            crypto_wipe(k.value.data(), k.value.size());
            if (bad) return {{}, {EIO, "keystore: secret failed authentication (tampered, corrupt or store key replaced)"}};
            return {std::move(pt), {}};
        }
        return legacy_get_(d.value, s, a, std::move(r.value));
    }
    // Old format: plaintext XOR a 64-byte `.pad`. Decode, re-store encrypted, drop the pad when no old file is left.
    static result<std::string> legacy_get_(const std::string& d, const std::string& s, const std::string& a, std::string v) {
        auto pad = read_file(join(d, ".pad"));
        if (!pad || pad.value.size() != 64) return {{}, {EIO, "keystore: secret is in an unknown format"}};
        for (std::size_t i = 0; i < v.size(); ++i) v[i] = char(v[i] ^ pad.value[i % 64]);
        if (auto st = file_put_(s, a, v); !st) return {{}, st.err};
        bool old_left = false;
        if (auto l = list_dir(d))
            for (auto& e : l.value)
                if (e.name.size() == 20 && e.name.compare(16, 4, ".key") == 0) { auto t = read_file(join(d, e.name)); if (t && (t.value.size() < 44 || t.value.compare(0, 4, "FKS1") != 0)) { old_left = true; break; } }
        if (!old_left) os::remove(join(d, ".pad"));
        return {std::move(v), {}};
    }
    static status file_remove_(const std::string& s, const std::string& a) {
        auto d = dir_(); if (!d) return {{}, d.err};
        auto p = join(d.value, file_name_(s, a)); if (!exists(p)) return {};
        return os::remove(p);
    }
#if defined(FORGE_OS_WINDOWS)
    static result<std::string> dpapi_(const std::string& in, bool protect) {
        struct blob_t { DWORD cbData; BYTE* pbData; };
        using fn = BOOL(WINAPI*)(blob_t*, LPCWSTR, blob_t*, PVOID, PVOID, DWORD, blob_t*);
        using ufn = BOOL(WINAPI*)(blob_t*, LPWSTR*, blob_t*, PVOID, PVOID, DWORD, blob_t*);
        static HMODULE h = LoadLibraryExW(L"crypt32.dll", nullptr, 0x00000800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/); if (!h) return {{}, unsupported("crypt32")};
        blob_t i{DWORD(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))}, o{0, nullptr};
        BOOL ok;
        if (protect) ok = reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(h, "CryptProtectData")))(&i, L"forge", nullptr, nullptr, nullptr, 0x1 /*UI_FORBIDDEN*/, &o);
        else ok = reinterpret_cast<ufn>(reinterpret_cast<void*>(GetProcAddress(h, "CryptUnprotectData")))(&i, nullptr, nullptr, nullptr, nullptr, 0x1, &o);
        if (!ok) return {{}, last_error(protect ? "CryptProtectData" : "CryptUnprotectData")};
        std::string r(reinterpret_cast<char*>(o.pbData), o.cbData); SecureZeroMemory(o.pbData, o.cbData); LocalFree(o.pbData); return {r, {}};
    }
#endif
#if defined(FORGE_OS_KS_LIBSECRET_)
    struct secret_api {
        struct schema_attr { const char* name; int type; };
        struct schema_t { const char* name; int flags; schema_attr attributes[32]; int reserved; void *r1, *r2, *r3, *r4, *r5, *r6, *r7; };
        struct gerror_t { std::uint32_t domain; int code; char* message; };
        void* h = nullptr; schema_t sc{};
        int (*store)(const schema_t*, const char*, const char*, const char*, void*, void**, ...) = nullptr;
        char* (*lookup)(const schema_t*, void*, void**, ...) = nullptr;
        int (*clear)(const schema_t*, void*, void**, ...) = nullptr;
        void (*free_pw)(char*) = nullptr; void (*clear_error)(void**) = nullptr; std::uint32_t (*quark)(const char*) = nullptr; bool ok = false;
        const schema_t* schema() const { return &sc; }
        // GError -> error, freeing it.
        error take_error(void* e, const char* what) const {
            auto* g = static_cast<gerror_t*>(e); error r{g->code ? g->code : EIO, std::string("keystore: ") + what + ": " + (g->message ? g->message : "failed")};
            void* p = e; clear_error(&p); return r;
        }
        // No bus or no Secret Service provider on it (as opposed to a provider that refused).
        bool absent(void* e) const {
            auto* g = static_cast<gerror_t*>(e); if (!quark) return false;
            if (g->domain == quark("g-io-error-quark")) return true;
            if (g->domain != quark("g-dbus-error-quark")) return false;
            int c = g->code;   // SERVICE_UNKNOWN, NAME_HAS_NO_OWNER, NO_SERVER, NO_NETWORK, DISCONNECTED, SPAWN_*
            return c == 2 || c == 3 || c == 11 || c == 13 || c == 15 || (c >= 23 && c <= 31);
        }
        static secret_api& get() {
            static secret_api a = [] {
                secret_api s; s.sc.name = "org.forge.Secret"; s.sc.attributes[0] = {"service", 0}; s.sc.attributes[1] = {"account", 0};
                s.h = ::dlopen("libsecret-1.so.0", RTLD_LAZY); if (!s.h) return s;
                s.store = reinterpret_cast<decltype(s.store)>(::dlsym(s.h, "secret_password_store_sync"));
                s.lookup = reinterpret_cast<decltype(s.lookup)>(::dlsym(s.h, "secret_password_lookup_sync"));
                s.clear = reinterpret_cast<decltype(s.clear)>(::dlsym(s.h, "secret_password_clear_sync"));
                s.free_pw = reinterpret_cast<decltype(s.free_pw)>(::dlsym(s.h, "secret_password_free"));
                if (void* g = ::dlopen("libglib-2.0.so.0", RTLD_LAZY)) {
                    s.clear_error = reinterpret_cast<decltype(s.clear_error)>(::dlsym(g, "g_clear_error"));
                    s.quark = reinterpret_cast<decltype(s.quark)>(::dlsym(g, "g_quark_from_string"));
                }
                s.ok = s.store && s.lookup && s.clear && s.free_pw && s.clear_error; return s;
            }();
            return a;
        }
    };
    // libsecret when it loads, a session bus exists and a provider answers a probe lookup; decided once per process.
    static bool use_libsecret_() {
        static const bool v = [] {
            auto& s = secret_api::get(); if (!s.ok) return false;
            auto rt = getenv("XDG_RUNTIME_DIR");
            if (getenv("DBUS_SESSION_BUS_ADDRESS").empty() && (rt.empty() || !exists(rt + "/bus"))) return false;
            void* err = nullptr; char* r = s.lookup(s.schema(), nullptr, &err, "service", "forge-keystore-probe", "account", "", nullptr);
            if (r) s.free_pw(r);
            if (!err) return true;
            bool gone = s.absent(err); s.clear_error(&err); return !gone;
        }();
        return v;
    }
#endif
#if defined(__APPLE__)
    struct sec_api {
        void* h = nullptr; int (*add)(const void*, const void**) = nullptr; int (*copy)(const void*, const void**) = nullptr; int (*del)(const void*) = nullptr;
        int (*update)(const void*, const void*) = nullptr;
        const void *kClass, *kGeneric, *kService, *kAccount, *kValueData, *kReturnData, *kMatchLimit, *kMatchOne, *kAccessible, *kAfterFirstUnlockThisDevice;
        static sec_api& get() {
            static sec_api s = [] { sec_api x{}; x.h = ::dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY); if (!x.h) return x;
                x.add = reinterpret_cast<decltype(x.add)>(::dlsym(x.h, "SecItemAdd")); x.copy = reinterpret_cast<decltype(x.copy)>(::dlsym(x.h, "SecItemCopyMatching"));
                x.del = reinterpret_cast<decltype(x.del)>(::dlsym(x.h, "SecItemDelete")); x.update = reinterpret_cast<decltype(x.update)>(::dlsym(x.h, "SecItemUpdate"));
                x.kClass = detail::framework_const(x.h, "kSecClass"); x.kGeneric = detail::framework_const(x.h, "kSecClassGenericPassword");
                x.kService = detail::framework_const(x.h, "kSecAttrService"); x.kAccount = detail::framework_const(x.h, "kSecAttrAccount");
                x.kValueData = detail::framework_const(x.h, "kSecValueData"); x.kReturnData = detail::framework_const(x.h, "kSecReturnData");
                x.kMatchLimit = detail::framework_const(x.h, "kSecMatchLimit"); x.kMatchOne = detail::framework_const(x.h, "kSecMatchLimitOne");
                x.kAccessible = detail::framework_const(x.h, "kSecAttrAccessible");
                x.kAfterFirstUnlockThisDevice = detail::framework_const(x.h, "kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly");
                return x; }();
            return s;
        }
        bool ok() const { return add && copy && del && update && kClass && detail::cf_api::get().ok; }
    };
    static const void* kc_query_(const std::string& s, const std::string& a, const void* extra_k[], const void* extra_v[], int nextra, std::vector<const void*>& owned) {
        auto& cf = detail::cf_api::get(); auto& k = sec_api::get();
        auto ss = cf.str(s.c_str()), as = cf.str(a.c_str()); owned.push_back(ss); owned.push_back(as);
        const void* keys[8] = {k.kClass, k.kService, k.kAccount}; const void* vals[8] = {k.kGeneric, ss, as};
        for (int i = 0; i < nextra; ++i) { keys[3 + i] = extra_k[i]; vals[3 + i] = extra_v[i]; }
        return cf.CFDictionaryCreate(nullptr, keys, vals, 3 + nextra, cf.kCFTypeDictionaryKeyCallBacks, cf.kCFTypeDictionaryValueCallBacks);
    }
    // Update in place (the old secret survives a failure); add only when the item does not exist yet.
    static status kc_put_(const std::string& s, const std::string& a, const std::string& v) {
        auto& k = sec_api::get(); if (!k.ok()) return {{}, unsupported("Security.framework")};
        auto& cf = detail::cf_api::get(); std::vector<const void*> owned;
        auto data = cf.CFDataCreate(nullptr, reinterpret_cast<const std::uint8_t*>(v.data()), detail::CFIndex(v.size())); owned.push_back(data);
        const void* uk[] = {k.kValueData}; const void* uv[] = {data};
        auto attrs = cf.CFDictionaryCreate(nullptr, uk, uv, 1, cf.kCFTypeDictionaryKeyCallBacks, cf.kCFTypeDictionaryValueCallBacks); owned.push_back(attrs);
        auto q = kc_query_(s, a, nullptr, nullptr, 0, owned); int rc = k.update(q, attrs); cf.CFRelease(q);
        if (rc == -25300 /*errSecItemNotFound*/) {
            const void* ek[2] = {k.kValueData, k.kAccessible}; const void* ev[2] = {data, k.kAfterFirstUnlockThisDevice};
            auto aq = kc_query_(s, a, ek, ev, k.kAccessible && k.kAfterFirstUnlockThisDevice ? 2 : 1, owned); rc = k.add(aq, nullptr); cf.CFRelease(aq);
            for (auto o : owned) cf.CFRelease(o);
            if (rc) return {{}, {rc, "SecItemAdd failed (OSStatus " + std::to_string(rc) + ")"}}; return {};
        }
        for (auto o : owned) cf.CFRelease(o);
        if (rc) return {{}, {rc, "SecItemUpdate failed (OSStatus " + std::to_string(rc) + ")"}}; return {};
    }
    static result<std::string> kc_get_(const std::string& s, const std::string& a) {
        auto& k = sec_api::get(); if (!k.ok()) return {{}, unsupported("Security.framework")};
        auto& cf = detail::cf_api::get(); std::vector<const void*> owned;
        const void* ek[] = {k.kReturnData, k.kMatchLimit}; const void* ev[] = {cf.kCFBooleanTrue, k.kMatchOne};
        auto q = kc_query_(s, a, ek, ev, 2, owned); const void* out = nullptr; int rc = k.copy(q, &out); cf.CFRelease(q); for (auto o : owned) cf.CFRelease(o);
        if (rc == -25300 || (!rc && !out)) return {{}, {2, "keystore: no such secret"}};
        if (rc) return {{}, {rc, "SecItemCopyMatching failed (OSStatus " + std::to_string(rc) + ")"}};
        std::string r(reinterpret_cast<const char*>(cf.CFDataGetBytePtr(out)), std::size_t(cf.CFDataGetLength(out))); cf.CFRelease(out); return {r, {}};
    }
    static status kc_remove_(const std::string& s, const std::string& a) {
        auto& k = sec_api::get(); if (!k.ok()) return {{}, unsupported("Security.framework")};
        auto& cf = detail::cf_api::get(); std::vector<const void*> owned;
        auto q = kc_query_(s, a, nullptr, nullptr, 0, owned); int rc = k.del(q); cf.CFRelease(q); for (auto o : owned) cf.CFRelease(o);
        if (rc && rc != -25300 /*errSecItemNotFound*/) return {{}, {rc, "SecItemDelete failed"}}; return {};
    }
#endif
};
} // namespace forge::os
#undef FORGE_OS_KS_LIBSECRET_
#undef FORGE_OS_KS_TSS_
