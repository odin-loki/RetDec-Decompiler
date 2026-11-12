#pragma once
// ForgeScript .fsc cache: the parsed and resolved program (the input to the bytecode compiler and the
// tree-walker), serialised so a cached run skips lexing, parsing and resolution. See docs/scripting.md.
//
// Layout (little-endian):
//   "FSC1" u32 vm_version  u8[32] key  u64 payload_len  payload  u8[32] BLAKE3(payload)  u8 signed  [u8[32] pub  u8[64] sig]
//   key = BLAKE3("forge-fsc\0" || u32 vm_version || file name || '\0' || source)
//   sig = Ed25519 over "forge-fsc-v1" || key || BLAKE3(payload)
// A cache file is used only when its key equals the key recomputed from the current source, the VM
// version matches, the payload checksum matches, the payload decodes with every bound checked (no
// back-reference cycles, nesting <= 4096), and — when it is signed or the directory is not private or the
// engine requires signatures — the signature verifies against a trusted public key. Anything else is
// ignored and the source is parsed normally. The checksum catches corruption, not forgery: an unsigned
// file is trusted because only its owner can write a private directory.
// The cache holds no capability decisions: static checks run on the decoded tree on every run, and
// capabilities, limits and the sandbox are applied at run time, so options are deliberately not in the key.
// Bytecode chunks are still compiled from the decoded tree and pass the VM verifier before running.
#include "ast.hpp"
#include <forge/security/secure_buffer.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::script::fsc {

// Bump when Node/FnDecl/ClassDecl, the encoding, or the meaning of the tree for the compiler changes.
inline constexpr std::uint32_t vm_version = 3;
static_assert(int(NK::Import) == 45, "NK changed: bump fsc::vm_version and update this assert");
inline constexpr std::uint64_t max_file_bytes = 64ull << 20;   // larger cache files are ignored (not read)

using key_t = std::array<std::uint8_t, 32>;
key_t key_of(std::string_view src, const std::string& file);
std::string key_hex(const key_t& k);

struct trust {
    std::vector<std::array<std::uint8_t, 32>> pubkeys;   // accepted signers
    bool require_signed = false;                         // reject unsigned caches
};

// Encode `top`. With a 32-byte seed the file is signed. The engine keeps its signing seed in a
// security::secure_bytes (locked, non-dumpable, wiped: FSS-CRY-2).
std::vector<std::uint8_t> encode(const FnDecl& top, const key_t& key, const std::array<std::uint8_t, 32>* seed = nullptr);
std::vector<std::uint8_t> encode(const FnDecl& top, const key_t& key, const security::secure_bytes<32>& seed);
// Decode; nullopt + why on any mismatch, bound violation or failed signature.
std::optional<std::shared_ptr<const FnDecl>> decode(std::span<const std::uint8_t> bytes, const key_t& expect, const trust& t, std::string* why = nullptr);
// The payload decoder alone (no header, checksum or signature): for tests and the fuzzer.
std::shared_ptr<const FnDecl> decode_payload(std::span<const std::uint8_t> payload, std::string* why = nullptr);

std::string path_for(const std::string& dir, const key_t& k);

// Whether `dir` exists, is owned by the effective user and is not writable by group or other (POSIX).
// Always false on Windows (no cheap owner/ACL check there): Windows caches must be signed.
bool dir_private(const std::string& dir);

// load / store: under an active forge::security policy the directory must pass enforce(fs_read) / enforce(fs_write)
// (CAP-1); a denial is audited and reported through `why` (the caller parses the source as on a miss).
// Read <dir>/<key>.fsc and decode it. Signatures are required in addition to `t` when the directory is not
// private or the file is not a regular file owned by us. POSIX: the directory is opened once and checked
// through that descriptor, the file is opened relative to it with O_NOFOLLOW (no symlinks, no TOCTOU swap).
std::optional<std::shared_ptr<const FnDecl>> load(const std::string& dir, const key_t& key, const trust& t, std::string* why = nullptr);

// Write <dir>/<key>.fsc atomically (unique temp file + rename, mode 0600; the directory is created 0700).
// Refuses (false + why) to store an unsigned file where it would never load: a directory that is not private.
bool store(const std::string& dir, const key_t& key, std::span<const std::uint8_t> bytes, bool is_signed, std::string* why = nullptr);

} // namespace forge::script::fsc
