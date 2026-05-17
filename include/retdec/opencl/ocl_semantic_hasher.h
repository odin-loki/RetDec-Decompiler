/**
 * @file include/retdec/opencl/ocl_semantic_hasher.h
 * @brief Parallel semantic hashing via mini x86-64 emulator (host side).
 *
 * Each function is executed on up to 16 adversarial test vectors.
 * The IO signature (input registers ++ output registers) is hashed with
 * FNV-1a to produce a compact 64-bit semantic fingerprint.
 *
 * Usage:
 *   OCLSemanticHasher hasher(&ctx);
 *
 *   FunctionBytecode f;
 *   f.bytes = { ... };
 *   f.testInputs = { ... };  // 16 × 8 uint64 rows
 *
 *   auto sigs = hasher.hash({f1, f2, ...});
 *   for (auto& s : sigs) {
 *       auto match = db.lookup(s.ioHash);
 *       ...
 *   }
 */

#ifndef RETDEC_OPENCL_OCL_SEMANTIC_HASHER_H
#define RETDEC_OPENCL_OCL_SEMANTIC_HASHER_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec {
namespace opencl {

class OCLContext;

// ─── Input descriptor ─────────────────────────────────────────────────────────

static constexpr std::size_t kTestVectorCount = 16;
static constexpr std::size_t kTestInputWidth  = 8;  ///< uint64 inputs per test vector

/// Adversarial test inputs for one function (row-major: [kTestVectorCount][kTestInputWidth]).
using TestInputMatrix = std::array<std::array<std::uint64_t, kTestInputWidth>, kTestVectorCount>;

struct FunctionBytecode {
    std::vector<std::uint8_t> bytes;
    TestInputMatrix            testInputs = {};  ///< zero-initialised = uses default vectors
};

// ─── Output descriptor ────────────────────────────────────────────────────────

enum class EmulationStatus : std::uint32_t {
    OK           = 0,
    Returned     = 1,
    Fault        = 2,
    StepLimit    = 3,
    Unsupported  = 4,
};

struct IOSignature {
    std::uint64_t    ioHash = 0;  ///< FNV-1a hash of (inputs ++ outputs)
    EmulationStatus  status = EmulationStatus::OK;
};

// ─── Predefined adversarial test-vector generator ────────────────────────────

/// Generate a set of adversarial test vectors designed to differentiate
/// common libc/libstdc++ functions (memcpy, strlen, strcmp, etc.).
TestInputMatrix defaultTestVectors(std::uint32_t funcIdx = 0);

// ─── OCLSemanticHasher ────────────────────────────────────────────────────────

class OCLSemanticHasher {
public:
    static constexpr std::uint32_t kScratchBytesPerWI = 4096u;

    explicit OCLSemanticHasher(OCLContext* ctx = nullptr);
    ~OCLSemanticHasher();

    OCLSemanticHasher(OCLSemanticHasher&&) noexcept;
    OCLSemanticHasher& operator=(OCLSemanticHasher&&) noexcept;

    OCLSemanticHasher(const OCLSemanticHasher&)            = delete;
    OCLSemanticHasher& operator=(const OCLSemanticHasher&) = delete;

    /// Hash all provided functions in parallel.
    ///
    /// @param funcs   One entry per function.  Each work-item gets kTestVectorCount
    ///                executions; results are compressed to one IOSignature per
    ///                (function × test-vector) pair.
    /// @returns       Flat vector of size funcs.size() × kTestVectorCount.
    ///                signatures[f * kTestVectorCount + tv] corresponds to
    ///                function f running on test vector tv.
    std::vector<IOSignature>
    hash(const std::vector<FunctionBytecode>& funcs);

    bool usesGPU() const noexcept;
    const std::string& lastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// ─── Semantic hash database (lightweight in-memory map) ──────────────────────

struct LibraryFunctionEntry {
    std::string   name;
    std::uint64_t ioHash;
};

class SemanticHashDB {
public:
    SemanticHashDB() = default;

    void insert(std::string name, std::uint64_t ioHash);

    /// Returns empty string if not found.
    std::string lookup(std::uint64_t ioHash) const;

    std::size_t size() const noexcept;

private:
    std::vector<LibraryFunctionEntry> _entries;
};

} // namespace opencl
} // namespace retdec

#endif // RETDEC_OPENCL_OCL_SEMANTIC_HASHER_H
