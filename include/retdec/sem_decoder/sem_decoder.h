/**
 * @file include/retdec/sem_decoder/sem_decoder.h
 * @brief Semantics-first instruction decoder with undefined flag propagation.
 *
 * ## Overview
 *
 * SemDecoder wraps Capstone to produce a normalised Semantic IR for every
 * instruction it decodes.  The pipeline is:
 *
 *   1. **Raw decode** — Capstone disassembly of the input byte stream.
 *   2. **Normalisation** — algebraically equivalent rewrites to a canonical form:
 *        • LEA reg, [reg+0]       → MOV reg, reg  (→ SemanticOp::Nop)
 *        • XCHG a, b; XCHG a, b  → NOP
 *        • SUB reg, reg           → MOV reg, 0; ZF=1 CF=0
 *        • ADD reg, 0             → NOP (flags preserved)
 *   3. **Flag annotation** — each instruction is tagged with
 *        {defined_flags, undefined_flags, preserved_flags}.
 *   4. **Undef propagation** — consumers of undefined flags receive a special
 *        FlagVal::Undef SSA value so the IR does not silently assume 0/1.
 *   5. **Overlapping decode** — for high-entropy (obfuscated) regions an
 *        instruction decode graph is built: every byte offset is a candidate
 *        instruction start; a max-likelihood path through the graph is found
 *        using a unigram frequency model trained on typical compiler output.
 *
 * ## Semantic IR
 *
 * Each instruction is lowered to a list of SemanticOp:
 *
 *   SemanticOp { type, dst, src1, src2, flagsEffect }
 *
 * where `type` is one of:
 *   Nop, Assign, Add, Sub, And, Or, Xor, Not, Neg, Shl, Shr, Sar,
 *   Mul, Div, Load, Store, Branch, Call, Return, Undef, Compare
 *
 * ## Architecture
 *
 * Currently supports X86 (32-bit) and X86_64 only.  The Capstone handle is
 * opened once per SemDecoder instance and reused for all decodes.
 */

#ifndef RETDEC_SEM_DECODER_SEM_DECODER_H
#define RETDEC_SEM_DECODER_SEM_DECODER_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace sem_decoder {

// ─── Flag masks (EFLAGS subset) ───────────────────────────────────────────────

using FlagSet = uint32_t;

namespace Flags {
    static constexpr FlagSet CF = 1u << 0;  ///< Carry
    static constexpr FlagSet PF = 1u << 2;  ///< Parity
    static constexpr FlagSet AF = 1u << 4;  ///< Auxiliary carry
    static constexpr FlagSet ZF = 1u << 6;  ///< Zero
    static constexpr FlagSet SF = 1u << 7;  ///< Sign
    static constexpr FlagSet OF = 1u << 11; ///< Overflow
    static constexpr FlagSet DF = 1u << 10; ///< Direction
    static constexpr FlagSet ALL = CF|PF|AF|ZF|SF|OF|DF;
    static constexpr FlagSet NONE = 0u;
    static constexpr FlagSet ARITH = CF|PF|AF|ZF|SF|OF; ///< Arithmetic flags
}

// ─── Flag effect on an instruction ───────────────────────────────────────────

struct FlagsEffect {
    FlagSet defined;    ///< Flags with deterministic output (written)
    FlagSet undefined;  ///< Flags that become architecturally undefined
    FlagSet preserved;  ///< Flags unchanged by this instruction

    FlagsEffect() : defined(0), undefined(0), preserved(Flags::ALL) {}
    FlagsEffect(FlagSet def, FlagSet undef, FlagSet pres)
        : defined(def), undefined(undef), preserved(pres) {}

    bool writesAny() const noexcept { return (defined | undefined) != 0; }
};

// ─── Semantic IR types ────────────────────────────────────────────────────────

enum class SemOpType : uint8_t {
    Nop,      ///< No-operation
    Assign,   ///< dst ← src1
    Add,      ///< dst ← src1 + src2
    Sub,      ///< dst ← src1 - src2
    And,      ///< dst ← src1 & src2
    Or,       ///< dst ← src1 | src2
    Xor,      ///< dst ← src1 ^ src2
    Not,      ///< dst ← ~src1
    Neg,      ///< dst ← -src1
    Shl,      ///< dst ← src1 << src2
    Shr,      ///< dst ← src1 >> src2 (logical)
    Sar,      ///< dst ← src1 >> src2 (arithmetic)
    Mul,      ///< dst ← src1 * src2 (unsigned)
    IMul,     ///< dst ← src1 * src2 (signed)
    Div,      ///< dst ← src1 / src2
    Load,     ///< dst ← mem[src1]
    Store,    ///< mem[dst] ← src1
    Branch,   ///< pc ← dst  (conditional: src1 = condition)
    Call,     ///< call dst
    Return,   ///< return
    Compare,  ///< sets flags only; dst = flags
    Undef,    ///< dst ← ⊥  (undefined value)
    SetFlag,  ///< explicit flag write (e.g. CLD, STD, STC)
};

// ─── Semantic operand ─────────────────────────────────────────────────────────

enum class SemValKind : uint8_t {
    None,
    Reg,       ///< Named register
    Imm,       ///< Immediate constant
    MemDeref,  ///< Memory dereference [base + index*scale + disp]
    Undef,     ///< Architecturally undefined value (for undef flag propagation)
};

struct SemVal {
    SemValKind kind   = SemValKind::None;
    uint32_t   reg    = 0;    ///< Capstone register ID (if kind==Reg)
    int64_t    imm    = 0;    ///< Immediate (if kind==Imm)

    // Memory components (if kind==MemDeref)
    uint32_t   base   = 0;
    uint32_t   index  = 0;
    int32_t    scale  = 1;
    int64_t    disp   = 0;

    static SemVal makeReg(uint32_t r)  { SemVal v; v.kind=SemValKind::Reg; v.reg=r; return v; }
    static SemVal makeImm(int64_t i)   { SemVal v; v.kind=SemValKind::Imm; v.imm=i; return v; }
    static SemVal undef()          { SemVal v; v.kind=SemValKind::Undef; return v; }
    static SemVal mem(uint32_t b, uint32_t idx, int32_t sc, int64_t d)
    { SemVal v; v.kind=SemValKind::MemDeref; v.base=b; v.index=idx; v.scale=sc; v.disp=d; return v; }

    bool isNone()  const noexcept { return kind == SemValKind::None; }
    bool isUndef() const noexcept { return kind == SemValKind::Undef; }
};

// A single semantic micro-operation.
struct SemanticOp {
    SemOpType   type  = SemOpType::Nop;
    SemVal      dst;
    SemVal      src1;
    SemVal      src2;
    FlagsEffect flagsEffect;
};

// ─── Decoded instruction (after normalisation) ────────────────────────────────

struct DecodedInstr {
    uint64_t    addr     = 0;   ///< Virtual address
    uint8_t     bytes[16]{};    ///< Raw encoding
    uint8_t     len      = 0;   ///< Byte count
    std::string mnemonic;       ///< Textual mnemonic (for debug)
    std::string opStr;          ///< Operand string (for debug)
    bool        isNormalised = false; ///< True if normalisation changed the op
    bool        isValid      = true;  ///< False if disassembly failed

    std::vector<SemanticOp> ops; ///< Normalised semantic micro-ops
    FlagsEffect             flagsEffect; ///< Aggregate flag effect
};

// ─── Decode graph (for overlapping / obfuscated regions) ─────────────────────

struct DecodeNode {
    uint64_t          addr;
    uint32_t          len;
    double            logFreq;  ///< log(unigram frequency) of this instruction
    std::vector<uint64_t> successors; ///< addr of compatible successor instructions
};

struct DecodeGraph {
    std::unordered_map<uint64_t, DecodeNode> nodes;
    std::vector<uint64_t>                    path; ///< Max-likelihood instruction sequence
};

// ─── SemDecoder ───────────────────────────────────────────────────────────────

class SemDecoder {
public:
    enum class Mode { X86, X86_64 };

    /**
     * Construct a decoder for the given mode.
     * @throws std::runtime_error if Capstone fails to initialise.
     */
    explicit SemDecoder(Mode mode = Mode::X86_64);
    ~SemDecoder();

    SemDecoder(const SemDecoder&)            = delete;
    SemDecoder& operator=(const SemDecoder&) = delete;

    // ── Linear decode ─────────────────────────────────────────────────────────

    /**
     * Decode a single instruction at the given byte offset.
     * Returns a DecodedInstr with isValid=false if decoding fails.
     */
    DecodedInstr decodeOne(const uint8_t* code, std::size_t codeSize,
                           uint64_t addr) const;

    /**
     * Decode a contiguous sequence of instructions until `end` is reached or
     * decoding fails.  Applies normalisation and flag annotation to each.
     */
    std::vector<DecodedInstr> decodeLinear(const uint8_t* code,
                                           std::size_t    codeSize,
                                           uint64_t       startAddr,
                                           uint64_t       endAddr) const;

    // ── Overlapping decode graph ───────────────────────────────────────────────

    /**
     * Shannon entropy of `data` (bits per byte).
     */
    static double entropy(const uint8_t* data, std::size_t size) noexcept;

    /**
     * True if the region's entropy exceeds the obfuscation threshold (6.5 bpb).
     */
    static bool isHighEntropy(const uint8_t* data, std::size_t size) noexcept;

    /**
     * Build an overlapping decode graph for [code, code+codeSize).
     * Every byte offset is tried as an instruction start.  Edges connect any
     * instruction at offset O of length L to all instructions at offset O+L.
     */
    DecodeGraph buildDecodeGraph(const uint8_t* code, std::size_t codeSize,
                                 uint64_t startAddr) const;

    /**
     * Find the max-likelihood linear path through a decode graph using the
     * Viterbi-style DP on log-frequency scores.
     * Returns the sequence of addresses in the best path.
     */
    static std::vector<uint64_t> bestPath(const DecodeGraph& g,
                                           uint64_t startAddr,
                                           uint64_t endAddr);

    // ── Flag propagation helpers ───────────────────────────────────────────────

    /**
     * Given a sequence of decoded instructions (in program order), propagate
     * undefined flags: any instruction that reads a flag that was most recently
     * written as "undefined" has that source replaced with SemVal::undef().
     *
     * The function modifies the SemanticOps in-place.
     */
    static void propagateUndefFlags(std::vector<DecodedInstr>& instrs);

    // ── Normalisation (exposed for testing) ───────────────────────────────────

    /**
     * Apply normalisation rewrites to a single decoded instruction.
     * Returns true if any rewrite was applied.
     */
    static bool normalise(DecodedInstr& instr,
                          const DecodedInstr* prev = nullptr);

    // ── Unigram model (exposed for testing) ───────────────────────────────────

    /**
     * Return the log-frequency for a (mnemonic, len) pair from the built-in
     * unigram model.  Unknown instructions get a small negative default.
     */
    static double unigramLogFreq(const std::string& mnemonic,
                                 uint32_t           instrLen) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    // Internalise decode + lower to semantic ops (no normalisation).
    DecodedInstr decodeRaw(const uint8_t* code, std::size_t sz,
                           uint64_t addr) const;

    // Lower a Capstone instruction to SemanticOps and FlagsEffect.
    static void lowerToSemOps(DecodedInstr& out);

    // Compute the aggregate FlagsEffect from Capstone EFLAGS metadata.
    static FlagsEffect extractFlagsEffect(const void* csInsn);
};

} // namespace sem_decoder
} // namespace retdec

#endif // RETDEC_SEM_DECODER_SEM_DECODER_H
