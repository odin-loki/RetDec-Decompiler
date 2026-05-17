/**
 * @file include/retdec/cli_parser/cil_lifter.h
 * @brief CIL (Common Intermediate Language) instruction decoder and BcCFG builder.
 *
 * ## CIL Method body layout (ECMA-335 §II.25.4)
 *
 * Tiny format (if CodeSize < 64 and MaxStack ≤ 8 and no locals and no exceptions):
 *   byte  Format_CodeSize  — low 2 bits = 0x2 (tiny), high 6 bits = code size
 *   byte* Code
 *
 * Fat format:
 *   word  Flags_Size  — Flags[15:12], Size[11:0] (size in 4-byte units, ≥3)
 *   word  MaxStack
 *   dword CodeSize
 *   dword LocalVarSigTok  — StandAloneSig token (0 = none)
 *   byte* Code
 *   (optional: ExceptionHandlingClause*)
 *
 * ## CIL instruction set
 *
 * Single-byte prefix instructions (0x00–0xFE excl.):
 *   0x00  nop         0x01 break       0x02 ldarg.0 … 0x05 ldarg.3
 *   0x06  ldloc.0 … 0x09 ldloc.3      0x0A stloc.0 … 0x0D stloc.3
 *   0x0E  ldarg.s     0x0F ldarg.s     (the short form)
 *   0x10  starg.s     0x11 ldloc.s     0x12 ldloca.s  0x13 stloc.s
 *   0x14  ldnull      0x15 ldc.i4.m1   0x16 ldc.i4.0  …
 *   0x20  ldc.i4      0x21 ldc.i8      0x22 ldc.r4    0x23 ldc.r8
 *   … (complete set in implementation)
 *
 * Two-byte prefix 0xFE:
 *   0xFE 0x01 ceq      0xFE 0x02 cgt     0xFE 0x03 cgt.un
 *   0xFE 0x04 clt      0xFE 0x05 clt.un
 *   0xFE 0x06 ldftn    0xFE 0x07 ldvirtftn
 *   0xFE 0x09 ldarg    0xFE 0x0A ldarga  0xFE 0x0B starg
 *   0xFE 0x0C ldloc    0xFE 0x0D ldloca  0xFE 0x0E stloc
 *   0xFE 0x0F localloc 0xFE 0x11 endfilter 0xFE 0x12 unaligned
 *   0xFE 0x13 volatile 0xFE 0x14 tail    0xFE 0x15 initobj
 *   0xFE 0x16 constrained 0xFE 0x17 cpblk 0xFE 0x18 initblk
 *   0xFE 0x19 no.      0xFE 0x1A rethrow 0xFE 0x1C sizeof
 *   0xFE 0x1D refanytype 0xFE 0x1E readonly
 *
 * ## BcOpcode mapping
 *
 * CIL opcodes are mapped to BcOpcode values in the DOTNET_* range.
 * We add DOTNET_* variants to bc_instr.h alongside the existing DALVIK_* range.
 *
 * ## Exception clauses (§II.25.4.6)
 *
 * Fat SEH tables: each clause is 24 bytes.
 * Small SEH tables: each clause is 12 bytes.
 *
 * Clause kind:
 *   0 = catch (type filter)
 *   1 = filter (IL filter block)
 *   2 = finally
 *   4 = fault
 */

#ifndef RETDEC_CLI_PARSER_CIL_LIFTER_H
#define RETDEC_CLI_PARSER_CIL_LIFTER_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/cli_parser/cli_sig.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace retdec {
namespace cli_parser {

using namespace bc_module;

// ─── CIL exception clause ────────────────────────────────────────────────────

enum class EHClauseKind : uint32_t {
    Catch   = 0,
    Filter  = 1,
    Finally = 2,
    Fault   = 4,
};

struct CILExceptionClause {
    EHClauseKind  kind;
    uint32_t      tryOffset;
    uint32_t      tryLength;
    uint32_t      handlerOffset;
    uint32_t      handlerLength;
    uint32_t      classToken;    ///< For Catch: TypeDefOrRef token
    uint32_t      filterOffset;  ///< For Filter: IL offset of filter block
};

// ─── CIL method header ────────────────────────────────────────────────────────

struct CILMethodHeader {
    bool     isTiny           = false;
    uint16_t maxStack         = 0;
    uint32_t codeSize         = 0;
    uint32_t localVarSigTok   = 0;
    bool     initLocals       = false;
    std::vector<CILExceptionClause> exceptionClauses;
};

// ─── CIL lifter ───────────────────────────────────────────────────────────────

/**
 * @brief Decodes a CIL method body into a BcCFG.
 *
 * Unlike the JVM lifter (which models a stack-based machine), the CIL lifter
 * also models a stack machine but with explicit operand pop/push annotations
 * per instruction.  The CIL instruction set is larger (218 opcodes) and
 * supports additional constructs (constrained, tail., volatile., unaligned.).
 *
 * ## Algorithm
 *
 * 1. Parse the method header (tiny / fat).
 * 2. Decode all instructions into a flat instruction stream with byte offsets.
 * 3. Identify basic block leaders (branch targets, exception handler offsets,
 *    method entry).
 * 4. Partition the instruction stream into basic blocks.
 * 5. Wire up successor edges (fall-through, explicit branches).
 * 6. Wire up exception handler edges from exception clauses.
 * 7. Annotate each instruction with BcOpcode and BcOperand.
 */
class CILLifter {
public:
    struct Options {
        bool decodeMetadataTokens = true;  ///< Resolve tokens to names
        bool decodeSwitchTargets  = true;
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit CILLifter(const ITypeNameResolver* resolver = nullptr,
                        const Options& opts = defaultOptions());

    /**
     * @brief Decode a method body from a byte span.
     *
     * @param body        Raw bytes starting at the method header.
     * @param outHeader   Receives the parsed header (for maxStack, localVarSig).
     * @return            The constructed BcCFG, or empty on failure.
     */
    BcCFG lift(std::span<const uint8_t> body,
                CILMethodHeader& outHeader) const;

    const std::string& error() const { return error_; }

private:
    const ITypeNameResolver* resolver_;
    Options opts_;
    mutable std::string error_;

    // ── Header parsing ────────────────────────────────────────────────────

    static bool parseHeader(std::span<const uint8_t> body,
                             CILMethodHeader& hdr, size_t& codeStart);

    // ── Instruction decoding ─────────────────────────────────────────────

    struct RawInsn {
        uint32_t offset;          ///< Byte offset from code start
        BcOpcode opcode;
        std::vector<BcOperand> operands;
        uint32_t size;            ///< Total byte size of this instruction
    };

    std::vector<RawInsn> decodeInstructions(
        std::span<const uint8_t> code) const;

    // Decode a single instruction at code[pos]. Advances pos.
    bool decodeOne(std::span<const uint8_t> code, size_t& pos,
                   RawInsn& out) const;

    // ── CFG construction ──────────────────────────────────────────────────

    BcCFG buildCFG(
        const std::vector<RawInsn>& insns,
        const CILMethodHeader& hdr) const;

    // ── Token resolution ─────────────────────────────────────────────────

    BcOperand tokenToOperand(uint32_t token) const;
    std::string tokenToString(uint32_t token) const;

    // ── CIL opcode → BcOpcode mapping ────────────────────────────────────

    static BcOpcode mapOpcode(uint16_t cilOpcode);

    // ── Little-endian read helpers ────────────────────────────────────────

    static uint8_t  r8 (std::span<const uint8_t> code, size_t pos);
    static uint16_t r16(std::span<const uint8_t> code, size_t pos);
    static uint32_t r32(std::span<const uint8_t> code, size_t pos);
    static uint64_t r64(std::span<const uint8_t> code, size_t pos);
    static float    rf32(std::span<const uint8_t> code, size_t pos);
    static double   rf64(std::span<const uint8_t> code, size_t pos);
};

} // namespace cli_parser
} // namespace retdec

#endif // RETDEC_CLI_PARSER_CIL_LIFTER_H
