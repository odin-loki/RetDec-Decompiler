/**
 * @file include/retdec/idiom_reconstruct/idiom_reconstruct.h
 * @brief Semantic compiler idiom reconstruction engine.
 *
 * ## What this module does
 *
 * Modern compilers routinely replace high-level arithmetic and library calls
 * with low-level instruction sequences that are faster on the target
 * microarchitecture but unreadable in disassembly.  This module recognises
 * those sequences semantically (by algebraic invariant, not opcode pattern)
 * and replaces them in the IR with a `ReplacementNode` that carries the
 * original high-level meaning.
 *
 * ## Idioms handled
 *
 * ### Integer division / modulo by compile-time constant
 *
 *   Compilers replace `x / K` (K a power-of-2 or arbitrary constant) with a
 *   multiply-by-magic-M + shift sequence.  We recover K algebraically:
 *
 *   Unsigned (GCC/Clang/MSVC):
 *     t = MULHI(x, M)         ; high half of x*M
 *     q = (t + (x-t)>>1) >> k ; or t >> k depending on M
 *     Invariant: M = ceil(2^(32+k) / K), verified via: K = 2^(32+k) / M
 *
 *   Signed (GCC/Clang):
 *     t = IMULHI(x, M)        ; high half of x*M (signed)
 *     q = t >> k              ; arithmetic shift
 *     sign_fix = q + (q >> 31); add 1 if negative quotient
 *     Invariant: M = ceil(2^(31+k) / K) for K > 0
 *
 *   MSVC 32-bit uses a slightly different rounding for some K; the
 *   CompilerProfile selects the correct verifier.
 *
 * ### Branchless absolute value
 *
 *   x86:  CDQ; XOR eax,edx; SUB eax,edx  → abs(x)
 *   Also: (x ^ (x>>31)) - (x>>31)         (used by Clang)
 *
 * ### Bit manipulation idioms
 *
 *   x & -x         → lowest_set_bit(x)      (__builtin_ctz analogue)
 *   x & (x-1)      → clear_lowest_set_bit(x)
 *   !x             → x == 0
 *   !!x            → x != 0   (to_bool)
 *   Parallel popcount (byte-split + multiply trick) → __builtin_popcount(x)
 *   Byte-swap sequence (ROR16, BSWAP) → __builtin_bswap32/64(x)
 *
 * ### SIMD memset / memcpy
 *
 *   Vectorised loops with SSE/AVX stores of constant value → memset()
 *   Vectorised loops with load-store pairs, stride = vector width → memcpy()
 *   Scalar epilogues are absorbed into the call.
 *
 * ## Architecture
 *
 * The engine operates on a light, ABI-independent instruction description
 * (`IdiomInstr`) fed from the platform-specific IR.  Matchers implement
 * `IIdiomMatcher` and are registered in `IdiomEngine`.  Each successful
 * match produces a `ReplacementNode` the caller splices into its IR.
 *
 * The separation means: the IR never leaks into the matchers, and matchers
 * are fully unit-testable with synthetic `IdiomInstr` sequences.
 */

#ifndef RETDEC_IDIOM_RECONSTRUCT_H
#define RETDEC_IDIOM_RECONSTRUCT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace idiom_reconstruct {

// ─── Compiler profile ─────────────────────────────────────────────────────────

/**
 * Selects the magic-constant verification algorithm.
 * Different compiler versions use slightly different rounding for edge cases
 * near power-of-2 boundaries.
 */
enum class CompilerProfile : uint8_t {
    Generic,    ///< Accept any standard formulation
    GCC,        ///< GCC-specific rounding (covers GCC 4.x–12.x)
    Clang,      ///< Clang-specific (same as GCC for most cases)
    MSVC,       ///< MSVC-specific formulation (/O2 codegen)
    ICC,        ///< Intel C++ Compiler
};

// ─── Lightweight instruction description ─────────────────────────────────────

/**
 * Instruction opcode category used by idiom matchers.
 * We use a semantic category, not an actual x86/ARM opcode, so matchers are
 * architecture-neutral (they are specialised later by the platform adapter).
 */
enum class IdiomOp : uint8_t {
    // Arithmetic
    Mul,        ///< Unsigned multiply (full or high-half result)
    IMul,       ///< Signed multiply
    MulHi,      ///< Unsigned multiply, high half only (MULHU / UMULH)
    IMulHi,     ///< Signed multiply, high half only (IMULHI / SMULH)
    Add,
    Sub,
    Neg,        ///< Two's complement negation
    // Shifts
    Shl,        ///< Logical left shift
    Shr,        ///< Logical right shift
    Sar,        ///< Arithmetic right shift
    Ror,        ///< Rotate right
    // Logic
    And,
    Or,
    Xor,
    Not,
    // Conversion / flag
    Cdq,        ///< CDQ / CQO: sign-extend EAX→EDX:EAX
    Movsx,      ///< Sign-extend move
    Movzx,      ///< Zero-extend move
    // Comparison / branch
    Cmp,
    Test,
    // Memory
    Load,
    Store,
    // SIMD
    VecLoad,    ///< Any SIMD vector load (SSE/AVX/NEON)
    VecStore,   ///< Any SIMD vector store
    VecSet,     ///< Broadcast scalar to all lanes (VPBROADCAST, DUP, etc.)
    // Misc
    Mov,        ///< Register-to-register copy or immediate load
    Lea,        ///< Load effective address (often acts as ADD)
    Other,
};

/**
 * Operand kind for `IdiomOperand`.
 */
enum class OperandKind : uint8_t {
    Reg,       ///< Register (identified by index)
    Imm,       ///< Immediate constant (signed 64-bit)
    Mem,       ///< Memory reference (base + index*scale + disp)
    VecReg,    ///< SIMD register
    None,
};

struct IdiomOperand {
    OperandKind kind   = OperandKind::None;
    uint32_t    reg    = 0;     ///< Register index (when kind==Reg or VecReg)
    int64_t     imm    = 0;     ///< Immediate value (when kind==Imm)
    uint32_t    width  = 0;     ///< Bit width of operand (8/16/32/64/128/256/512)
    bool        isSigned= false;///< For immediate: treat as signed?

    static IdiomOperand reg32(uint32_t r) { IdiomOperand o; o.kind=OperandKind::Reg; o.reg=r; o.width=32; return o; }
    static IdiomOperand reg64(uint32_t r) { IdiomOperand o; o.kind=OperandKind::Reg; o.reg=r; o.width=64; return o; }
    static IdiomOperand makeImm(int64_t v, uint32_t w=64) { IdiomOperand o; o.kind=OperandKind::Imm; o.imm=v; o.width=w; return o; }
    static IdiomOperand vreg(uint32_t r, uint32_t w) { IdiomOperand o; o.kind=OperandKind::VecReg; o.reg=r; o.width=w; return o; }
};

/**
 * One instruction in the idiom window.
 *
 * The platform adapter translates its native IR instruction into `IdiomInstr`
 * before handing it to the matchers.
 */
struct IdiomInstr {
    IdiomOp  op    = IdiomOp::Other;
    IdiomOperand dst;
    IdiomOperand src0;
    IdiomOperand src1;
    IdiomOperand src2;
    uint64_t     vma   = 0;    ///< Address in binary (for diagnostics)
    uint32_t     vecWidth = 0; ///< For SIMD: vector register width in bytes

    bool isImm(int i) const {
        if (i==0) return src0.kind==OperandKind::Imm;
        if (i==1) return src1.kind==OperandKind::Imm;
        return false;
    }
    int64_t getImm(int i) const {
        if (i==0) return src0.imm;
        if (i==1) return src1.imm;
        return 0;
    }
    bool sameReg(int srcIdx, const IdiomInstr& other, int otherSrcIdx) const;
    bool dstEqSrc(int srcIdx) const;
};

// ─── Replacement node ─────────────────────────────────────────────────────────

/**
 * The semantic meaning of a recognised idiom.
 * Spliced into the IR in place of the raw instruction sequence.
 */
enum class ReplacementKind : uint8_t {
    DivSigned,         ///< x / K  (signed integer K)
    DivUnsigned,       ///< x / K  (unsigned integer K)
    ModSigned,         ///< x % K
    ModUnsigned,       ///< x % K  (unsigned)
    AbsValue,          ///< abs(x)
    LowestSetBit,      ///< x & -x
    ClearLowestSetBit, ///< x & (x-1)
    ToBool,            ///< (bool)x  (!!x)
    IsZero,            ///< x == 0
    Popcount,          ///< __builtin_popcount(x)
    ByteSwap,          ///< __builtin_bswap32/64(x)
    Memset,            ///< memset(dst, value, count)
    Memcpy,            ///< memcpy(dst, src, count)
    Memmove,           ///< memmove(dst, src, count)
};

struct ReplacementNode {
    ReplacementKind kind;

    // ── Integer arithmetic fields ──────────────────────────────────────────────
    uint32_t    inputReg   = 0;   ///< Primary input register
    uint32_t    outputReg  = 0;   ///< Result register
    int64_t     divisor    = 0;   ///< For Div/Mod: the constant K
    uint32_t    operandWidth = 32;///< Bit width of operand (32 or 64)

    // ── Memory operation fields ────────────────────────────────────────────────
    uint32_t    dstReg     = 0;   ///< Destination pointer register
    uint32_t    srcReg     = 0;   ///< Source pointer register (Memcpy)
    int64_t     fillValue  = 0;   ///< For Memset: constant fill byte
    uint32_t    countReg   = 0;   ///< Register holding the byte count
    int64_t     countImm   = -1;  ///< Constant byte count (-1 = register)

    // ── Span in original instruction stream ───────────────────────────────────
    uint64_t    firstVma   = 0;   ///< VMA of first instruction replaced
    uint64_t    lastVma    = 0;   ///< VMA of last instruction replaced
    std::size_t instrCount = 0;   ///< Number of original instructions replaced

    std::string debugStr() const;
};

// ─── Idiom matcher interface ──────────────────────────────────────────────────

/**
 * A window of instructions the matcher inspects.
 * `begin` and `end` are iterators/indices into the caller's instruction list.
 * The matcher reads but never modifies the window.
 */
using InstrWindow = std::vector<IdiomInstr>;

/**
 * Abstract base for all idiom matchers.
 */
class IIdiomMatcher {
public:
    virtual ~IIdiomMatcher() = default;

    /**
     * Name of this matcher (for diagnostics / logging).
     */
    virtual const char* name() const noexcept = 0;

    /**
     * Minimum number of instructions this matcher needs to examine.
     * The engine skips calling `match` if the window is shorter.
     */
    virtual std::size_t minWindowSize() const noexcept = 0;

    /**
     * Try to match an idiom starting at `window[offset]`.
     *
     * @param window  All instructions in the basic block.
     * @param offset  Start index within window.
     * @param profile Compiler profile for algorithm variant selection.
     * @returns A populated ReplacementNode if matched, or nullopt.
     *
     * On success, the returned node's `instrCount` field indicates how many
     * instructions were consumed (so the engine can advance the scan cursor).
     */
    virtual std::optional<ReplacementNode> match(
        const InstrWindow& window,
        std::size_t        offset,
        CompilerProfile    profile) const = 0;
};

// ─── Idiom engine ─────────────────────────────────────────────────────────────

/**
 * Orchestrates all registered matchers over a basic-block instruction window.
 *
 * Usage:
 *   IdiomEngine engine;
 *   engine.setProfile(CompilerProfile::GCC);
 *   engine.registerMatcher(makeDivisionMatcher());
 *   engine.registerMatcher(makeModuloMatcher());
 *   // ... register others ...
 *   auto replacements = engine.process(instrWindow);
 */
class IdiomEngine {
public:
    IdiomEngine();
    ~IdiomEngine();
    IdiomEngine(const IdiomEngine&) = delete;
    IdiomEngine& operator=(const IdiomEngine&) = delete;
    IdiomEngine(IdiomEngine&&) = default;
    IdiomEngine& operator=(IdiomEngine&&) = default;

    void setProfile(CompilerProfile p) { profile_ = p; }
    CompilerProfile profile() const    { return profile_; }

    /// Register a matcher (takes ownership).
    void registerMatcher(std::unique_ptr<IIdiomMatcher> m);

    /**
     * Scan `window` left-to-right, trying each matcher at each position.
     * Returns all found replacements in order of first instruction.
     * Replacements do not overlap (greedy first-match wins).
     */
    std::vector<ReplacementNode> process(const InstrWindow& window) const;

    const std::vector<std::unique_ptr<IIdiomMatcher>>& matchers() const {
        return matchers_;
    }

private:
    std::vector<std::unique_ptr<IIdiomMatcher>> matchers_;
    CompilerProfile profile_ = CompilerProfile::Generic;
};

/// Build an IdiomEngine pre-populated with all standard matchers.
IdiomEngine makeDefaultEngine(CompilerProfile profile = CompilerProfile::Generic);

// ─── Factory functions for individual matchers ────────────────────────────────

std::unique_ptr<IIdiomMatcher> makeDivisionMatcher();
std::unique_ptr<IIdiomMatcher> makeModuloMatcher();
std::unique_ptr<IIdiomMatcher> makeAbsMatcher();
std::unique_ptr<IIdiomMatcher> makeBitIdiomMatcher();
std::unique_ptr<IIdiomMatcher> makeSimdMemMatcher();

} // namespace idiom_reconstruct
} // namespace retdec

#endif // RETDEC_IDIOM_RECONSTRUCT_H
