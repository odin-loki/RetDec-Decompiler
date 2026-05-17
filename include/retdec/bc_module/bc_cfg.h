/**
 * @file include/retdec/bc_module/bc_cfg.h
 * @brief BcCFG — control-flow graph for a BcMethod.
 *
 * ## Structure
 *
 * Each `BcMethod` owns exactly one `BcCFG`.  The CFG contains:
 *   - A list of `BcBasicBlock`s (block 0 = method entry).
 *   - Explicit predecessor and successor lists (block IDs, no pointers).
 *   - An exception handler table mapping protected regions to handler blocks.
 *
 * ## Typed stack state
 *
 * Each basic block records the operand-stack type signature at its entry
 * (`entryStack`) and exit (`exitStack`).  These are filled in by the
 * bytecode verifier / lifter after the initial instruction pass.
 *
 * The entry stack of the successor must be compatible with (≤) the exit
 * stack of all predecessors — the verifier enforces this.
 *
 * ## Exception handlers
 *
 * A `BcExceptionHandler` covers a half-open range of instruction offsets
 * [start, end) and points to a handler block.  When the handler block is
 * entered, exactly one value (the caught exception object) is on the stack.
 * A null `catchType` means "catch all" (Java `finally`, CLR `fault`/`finally`).
 */

#ifndef RETDEC_BC_MODULE_BC_CFG_H
#define RETDEC_BC_MODULE_BC_CFG_H

#include "retdec/bc_module/bc_instr.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace bc_module {

// ─── Exception handler ────────────────────────────────────────────────────────

struct BcExceptionHandler {
    uint32_t               startOffset  = 0;    ///< Start of protected region (inclusive)
    uint32_t               endOffset    = 0;    ///< End of protected region (exclusive)
    uint32_t               handlerBlock = 0;    ///< Index of handler BcBasicBlock
    std::optional<BcType>  catchType;           ///< nullopt ↔ catch-all (finally/fault)
    bool                   isFinally    = false;///< CLR finally (always executes)
    bool                   isFault      = false;///< CLR fault (executes on exception)
};

// ─── Basic block ──────────────────────────────────────────────────────────────

struct BcBasicBlock {
    uint32_t id = 0;                        ///< Block index within the method
    std::string label;                      ///< Optional label (from debug info)

    std::vector<BcInstruction> instrs;      ///< Instructions in program order

    // Operand-stack type signatures at block boundaries.
    std::vector<BcType> entryStack;         ///< Types on stack at block entry
    std::vector<BcType> exitStack;          ///< Types on stack at block exit

    // CFG edges (block indices).
    std::vector<uint32_t> preds;
    std::vector<uint32_t> succs;

    // Dominator information (filled by DomTree builder if used).
    uint32_t idom         = UINT32_MAX;     ///< Immediate dominator block id
    bool     isLoopHeader = false;
    bool     isExceptionHandler = false;    ///< This block is an EH handler entry

    bool hasTerminator() const noexcept;
    BcInstruction* terminator();
    const BcInstruction* terminator() const;
    bool isReachable() const noexcept { return !preds.empty() || id == 0; }
};

// ─── CFG ──────────────────────────────────────────────────────────────────────

/**
 * @brief BcCFG — the control-flow graph of one BcMethod.
 */
class BcCFG {
public:
    BcCFG() = default;

    // Block management
    BcBasicBlock&       addBlock();
    BcBasicBlock&       block(uint32_t id);
    const BcBasicBlock& block(uint32_t id) const;
    uint32_t            blockCount() const { return static_cast<uint32_t>(blocks_.size()); }
    BcBasicBlock&       entry()  { return blocks_.front(); }
    const BcBasicBlock& entry()  const { return blocks_.front(); }

    // Edge management
    void addEdge(uint32_t from, uint32_t to);
    void removeEdge(uint32_t from, uint32_t to);
    bool hasEdge(uint32_t from, uint32_t to) const;

    // Exception handler table
    void addExceptionHandler(BcExceptionHandler eh);
    const std::vector<BcExceptionHandler>& handlers() const { return handlers_; }

    // Instruction-to-block mapping
    uint32_t blockOfOffset(uint32_t offset) const;   ///< Block containing bytecode offset
    void     buildOffsetMap();

    // Validation
    bool verify(std::string& error) const;

    const std::deque<BcBasicBlock>& blocks() const { return blocks_; }
    std::deque<BcBasicBlock>&       blocks()       { return blocks_; }

private:
    std::deque<BcBasicBlock>      blocks_;
    std::vector<BcExceptionHandler> handlers_;
    // offset → block index (built lazily by buildOffsetMap)
    std::vector<std::pair<uint32_t, uint32_t>> offsetMap_;
};

} // namespace bc_module
} // namespace retdec

#endif // RETDEC_BC_MODULE_BC_CFG_H
