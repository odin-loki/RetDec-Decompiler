#pragma once
// FSS-CRY-2: key material in locked, non-dumpable memory, wiped on free.
//
//   security::secure_buffer b(64);          // os::locked_buffer: page-backed, mlock/VirtualLock (best effort),
//                                           // MADV_DONTDUMP / MADV_NOCORE / WER exclusion, crypto_wipe on free
//   security::secure_bytes<32> seed;        // fixed-size key (std::array-like: data(), size(), begin(), [])
//   security::secure_string tok = "s3cret"; // bearer token / passphrase: converts to std::string_view
//   auto st = security::memory_state(b);    // "locked, no-dump" / "unlocked (mlock refused), dump not excluded"
//
// secure_buffer and secure_bytes are move-only. secure_string is copyable because the structs that hold tokens
// (graph::rpc::server_security) are copied; a copy is a fresh locked mapping, never a shared one.
// What the protections are on each OS, and what they do not cover (copies the caller makes, OpenSSL's own key
// storage, the kernel's socket buffers), is in os/locked_memory.hpp and docs/security-api.md "Key memory".
#include <forge/os/locked_memory.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace forge::security {

using secure_buffer = os::locked_buffer;

// Human-readable lock / dump state of a buffer (for check reports and logs; never the contents).
inline std::string memory_state(const secure_buffer& b) {
    if (b.data() == nullptr) return "empty";
    std::string s = b.locked() ? "locked" : "unlocked (lock refused)";
    s += b.dump_excluded() ? ", excluded from dumps" : ", dump not excluded";
    if (b.guarded()) s += ", guard pages";
    return s;
}

// A fixed-size secret (an Ed25519 seed, a symmetric key). Zero on construction; move-only.
template <std::size_t N>
class secure_bytes {
public:
    secure_bytes() : b_(N) {}
    secure_bytes(secure_bytes&&) noexcept = default;
    secure_bytes& operator=(secure_bytes&&) noexcept = default;
    secure_bytes(const secure_bytes&) = delete;
    secure_bytes& operator=(const secure_bytes&) = delete;
    // An explicit deep copy into a new locked mapping.
    secure_bytes clone() const { secure_bytes c; if (b_.data()) std::memcpy(c.data(), b_.data(), N); return c; }

    std::uint8_t* data() noexcept { return b_.data(); }
    const std::uint8_t* data() const noexcept { return b_.data(); }
    static constexpr std::size_t size() noexcept { return N; }
    std::uint8_t* begin() noexcept { return b_.data(); }
    std::uint8_t* end() noexcept { return b_.data() + (b_.data() ? N : 0); }
    const std::uint8_t* begin() const noexcept { return b_.data(); }
    const std::uint8_t* end() const noexcept { return b_.data() + (b_.data() ? N : 0); }
    std::uint8_t& operator[](std::size_t i) noexcept { return b_.data()[i]; }
    const std::uint8_t& operator[](std::size_t i) const noexcept { return b_.data()[i]; }
    const secure_buffer& buffer() const noexcept { return b_; }

private:
    secure_buffer b_;
};

// A secret string (bearer token, passphrase). Converts to std::string_view; copies make a new locked mapping.
// Callers should wipe the std::string they built it from (crypto_wipe) when that string held the secret.
class secure_string {
public:
    secure_string() = default;
    secure_string(std::string_view s) { if (!s.empty()) b_ = secure_buffer(s.data(), s.size()); }   // NOLINT: implicit (token fields keep their API)
    secure_string(const std::string& s) : secure_string(std::string_view(s)) {}
    secure_string(const char* s) : secure_string(std::string_view(s ? s : "")) {}
    secure_string(const secure_string& o) : secure_string(o.view()) {}
    secure_string& operator=(const secure_string& o) { if (this != &o) *this = o.view(); return *this; }
    secure_string(secure_string&&) noexcept = default;
    secure_string& operator=(secure_string&&) noexcept = default;
    secure_string& operator=(std::string_view s) { if (s.empty()) b_.reset(); else b_ = secure_buffer(s.data(), s.size()); return *this; }
    secure_string& operator=(const std::string& s) { return *this = std::string_view(s); }
    secure_string& operator=(const char* s) { return *this = std::string_view(s ? s : ""); }

    std::string_view view() const noexcept { return b_.data() ? std::string_view(reinterpret_cast<const char*>(b_.data()), b_.size()) : std::string_view(); }
    operator std::string_view() const noexcept { return view(); }   // NOLINT
    const char* data() const noexcept { return b_.data() ? reinterpret_cast<const char*>(b_.data()) : ""; }
    std::size_t size() const noexcept { return b_.size(); }
    bool empty() const noexcept { return b_.size() == 0; }
    void clear() noexcept { b_.reset(); }
    std::string str() const { return std::string(view()); }   // an ordinary (unprotected) copy: avoid for long-lived use
    const secure_buffer& buffer() const noexcept { return b_; }
    friend bool operator==(const secure_string& a, std::string_view b) noexcept { return a.view() == b; }

private:
    secure_buffer b_;
};

} // namespace forge::security
