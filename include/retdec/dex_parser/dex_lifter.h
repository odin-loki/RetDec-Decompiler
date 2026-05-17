/**
 * @file include/retdec/dex_parser/dex_lifter.h
 * @brief Dalvik bytecode → BcCFG lifter.
 *
 * Unlike the JVM (stack-based), Dalvik is register-based. Each instruction
 * specifies explicit source and destination registers (vN). The lifter:
 *
 *   1. Scans all instructions in the code_item to find basic-block leaders
 *      (branch targets, try-block entries, exception handler entries, first insn).
 *   2. Splits into BcBasicBlocks.
 *   3. Wires exception handlers from the try/catch table.
 *   4. Maps each Dalvik register vN to a BcLocalVar (typed once a move or
 *      const is seen; otherwise typed as Object initially).
 *   5. Translates every Dalvik opcode to BcInstruction(s) using the shared
 *      BcOpcode set (DALVIK_ prefixed opcodes in BcOpcode).
 *
 * Register operands are encoded as BcOperand::Local with id = register number.
 * Wide register pairs (vN, vN+1) are encoded as the lower-numbered register.
 */

#ifndef RETDEC_DEX_PARSER_DEX_LIFTER_H
#define RETDEC_DEX_PARSER_DEX_LIFTER_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/dex_parser/dex_header.h"

#include <memory>
#include <string>

namespace retdec {
namespace dex_parser {

struct LiftOptions {
    bool emitLineNumbers = true;    ///< Populate BcInstruction::line when debug info present
    bool emitAnnotations = true;    ///< Wire exception handlers from try/catch table
    bool resolveStrings  = true;    ///< Resolve string-const indices to BcOperand::Str
};
static LiftOptions defaultLiftOptions() noexcept { return {}; }

struct DexLiftResult {
    enum Status { OK, Error };
    Status             status = OK;
    std::string        error;
    bc_module::BcCFG   cfg;
};

/**
 * @brief Converts a single DEX code_item to a BcCFG.
 */
class DexLifter {
public:
    DexLifter(const DexFile& dexFile, LiftOptions opts = defaultLiftOptions());

    DexLiftResult lift(const CodeItem& code, uint32_t methodIdx);

private:
    const DexFile& dex_;
    LiftOptions    opts_;

    using BlockId = uint32_t;

    // Pass 1: find leader addresses (code unit offsets).
    std::vector<uint32_t> findLeaders(const CodeItem& code) const;

    // Pass 2: build BcBasicBlock skeletons.
    void buildBlocks(bc_module::BcCFG& cfg,
                     const CodeItem& code,
                     const std::vector<uint32_t>& leaders);

    // Pass 3: wire exception handlers.
    void wireExceptions(bc_module::BcCFG& cfg, const CodeItem& code,
                        const std::vector<uint32_t>& leaders);

    // Decode one Dalvik instruction starting at codeUnit offset `off`.
    // Returns number of code units consumed.
    uint32_t decodeInsn(bc_module::BcBasicBlock& blk,
                        const std::vector<uint16_t>& insns,
                        uint32_t off,
                        const DexFile& dex);
};

} // namespace dex_parser
} // namespace retdec

#endif // RETDEC_DEX_PARSER_DEX_LIFTER_H
