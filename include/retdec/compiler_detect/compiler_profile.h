/**
 * @file include/retdec/compiler_detect/compiler_profile.h
 * @brief Compiler/version/optimisation-level result types.
 *
 * CompilerProfile is produced by CompilerFingerprinter and consumed by every
 * downstream ABI-parameterised stage (variable recovery, type inference,
 * calling-convention detection, etc.).
 */

#ifndef RETDEC_COMPILER_DETECT_COMPILER_PROFILE_H
#define RETDEC_COMPILER_DETECT_COMPILER_PROFILE_H

#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace compiler_detect {

// ─── Compiler family ─────────────────────────────────────────────────────────

enum class CompilerFamily : uint8_t {
    Unknown = 0,
    GCC,
    Clang,
    MSVC,
    ICC,    ///< Intel C/C++ Compiler
    TCC,    ///< Tiny C Compiler
};

std::string toString(CompilerFamily f);

// ─── Optimisation level ───────────────────────────────────────────────────────

enum class OptLevel : uint8_t {
    Unknown = 0,
    O0,     ///< -O0 / /Od — no optimisation
    O1,     ///< -O1
    O2,     ///< -O2 / /O2
    O3,     ///< -O3
    Os,     ///< -Os / /O1 — size optimised
    Oz,     ///< -Oz  (Clang only)
};

std::string toString(OptLevel o);

// ─── C++ ABI ─────────────────────────────────────────────────────────────────

enum class CppABI : uint8_t {
    Unknown  = 0,
    Itanium,    ///< GCC / Clang (all Unix)
    MSVC,       ///< Microsoft
    Rust,       ///< Rust name mangling
};

std::string toString(CppABI a);

// ─── Calling convention set ───────────────────────────────────────────────────

enum class CallingConvention : uint8_t {
    Unknown       = 0,
    SystemV_AMD64, ///< Linux/macOS x86-64  (RDI,RSI,RDX,RCX,R8,R9 → RAX)
    Win64,         ///< Windows x86-64      (RCX,RDX,R8,R9 + shadow space → RAX)
    Cdecl_x86,     ///< 32-bit __cdecl
    Stdcall_x86,   ///< 32-bit __stdcall
    Fastcall_x86,  ///< 32-bit __fastcall
    Thiscall_x86,  ///< 32-bit __thiscall  (MSVC)
};

std::string toString(CallingConvention cc);

// ─── Version range ────────────────────────────────────────────────────────────

struct VersionRange {
    uint32_t major_lo = 0;  ///< minimum major version (inclusive)
    uint32_t major_hi = 0;  ///< maximum major version (inclusive)

    bool isKnown() const noexcept { return major_lo != 0 || major_hi != 0; }
    std::string toString() const;
};

// ─── Raw feature vector (all intermediate measurements) ───────────────────────

struct FeatureVector {
    /// Fraction of functions (out of first kMaxFunctions) with RBP frame ptr.
    float framePointerRatio    = 0.0f;

    /// Fraction of function exits via JMP (tail call) vs RET.
    float tailCallRatio        = 0.0f;

    /// Maximum number of bytes inlined as memset (0 = none detected).
    uint32_t memsetInlineThreshold = 0;

    /// Whether RSP is aligned to 16 bytes in function prologues.
    bool stackAlign16          = false;

    /// Whether 32-byte shadow space is pre-allocated at call sites (MSVC/Win64).
    bool shadowSpaceAlloc      = false;

    /// MSVC Rich header present and parsed VS major version (0 = absent).
    uint32_t richHeaderVSMajor = 0;

    /// EH personality symbol detected ('g'=gxx, 'm'=MSVC, 0=none).
    char ehPersonality         = 0;

    /// Whether the binary's symbol table contains Itanium-mangled names (_Z...).
    bool itaniumMangling       = false;

    /// Whether the binary's symbol table contains MSVC-mangled names (?...).
    bool msvcMangling          = false;

    /// Whether __stack_chk_fail / __stack_chk_guard appear in imports.
    bool stackCanary           = false;

    /// Number of functions analysed.
    uint32_t functionsAnalysed = 0;
};

// ─── CompilerProfile — the final output ──────────────────────────────────────

struct CompilerProfile {
    CompilerFamily    family    = CompilerFamily::Unknown;
    VersionRange      version;
    OptLevel          optLevel  = OptLevel::Unknown;
    CppABI            cppABI    = CppABI::Unknown;
    CallingConvention callConv  = CallingConvention::Unknown;

    /// Normalised confidence in [0.0, 1.0].
    float confidence = 0.0f;

    /// Raw features used for classification (diagnostic / test use).
    FeatureVector features;

    /// Human-readable summary, e.g. "GCC 11-12 -O2 (SystemV_AMD64)".
    std::string summary() const;

    bool isKnown() const noexcept { return family != CompilerFamily::Unknown; }
};

} // namespace compiler_detect
} // namespace retdec

#endif // RETDEC_COMPILER_DETECT_COMPILER_PROFILE_H
