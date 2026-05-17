/**
 * @file include/retdec/jvm_reconstruct/stack_sim.h
 * @brief JVM operand-stack simulation and type-state propagation.
 *
 * ## Problem
 *
 * The JVM is a pure stack machine: instructions consume and produce typed
 * values on an implicit operand stack.  Decompilation requires lifting this
 * back to a variable-based representation.
 *
 * ## Algorithm
 *
 * This module implements Phase 1 + Phase 2 of the JVM reconstruction pipeline:
 *
 *   Phase 1 — Stack-slot temporary introduction:
 *     Each pop/push pair within a basic block is named as a fresh typed
 *     temporary (e.g. `$s0`, `$s1`).  The slot index is the depth from the
 *     bottom of the stack at the point of creation.
 *
 *   Phase 2 — Join-point type meeting:
 *     At control-flow merge points the types of stack-top entries must agree.
 *     This uses a fixed-point iteration with a lattice:
 *
 *       ⊤ (Top, uninitialized)
 *         / | \
 *       int long float double Object[] ...
 *         \ | /
 *       ⊥ (Bottom, incompatible — error)
 *
 *     The meet of two reference types is their least common supertype in the
 *     class hierarchy.  For simplicity, if the hierarchy is not available,
 *     the meet defaults to java/lang/Object.
 *
 * ## Output
 *
 * `StackSimResult` for each method:
 *   - `slots`: the set of named temporaries (one per unique stack slot).
 *   - For each `BcInstruction` in each `BcBasicBlock`, its operand list is
 *     rewritten: every implicit stack reference becomes an explicit reference
 *     to a named `StackSlot`.
 *
 * The result is consumed by the slot coalescer and local rebuilder.
 */

#ifndef RETDEC_JVM_RECONSTRUCT_STACK_SIM_H
#define RETDEC_JVM_RECONSTRUCT_STACK_SIM_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/bc_module/bc_module.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace jvm_reconstruct {

using namespace bc_module;

// ─── Stack slot ───────────────────────────────────────────────────────────────

/**
 * @brief A named stack temporary produced by the stack simulator.
 *
 * Each distinct value that flows through a JVM stack slot becomes one
 * StackSlot.  The simulator assigns a unique `name` ($s0, $s1, …) and
 * infers the `type` from the producing instruction.
 */
struct StackSlot {
    uint32_t    id   = 0;         ///< Sequential ID
    std::string name;             ///< "$s0", "$s1", …
    BcType      type;             ///< Inferred type at the definition point
    uint32_t    defBlock  = 0;    ///< Block where the slot is first defined
    uint32_t    defOffset = 0;    ///< Bytecode offset of the defining instruction

    /// True for wide (long/double) slots — occupy two JVM stack positions.
    bool isWide() const;
};

// ─── Per-block stack state ────────────────────────────────────────────────────

/**
 * @brief The operand-stack type state at a block boundary.
 *
 * Each element corresponds to one JVM stack depth (bottom = index 0).
 * For wide types (long, double), a single StackSlot occupies two depths but
 * is represented as one entry in this vector.
 */
struct StackState {
    std::vector<uint32_t> slotIds;  ///< Slot IDs from bottom of stack upward

    bool empty() const { return slotIds.empty(); }
    size_t size() const { return slotIds.size(); }
    uint32_t top() const { return slotIds.back(); }
    void push(uint32_t id) { slotIds.push_back(id); }
    uint32_t pop() {
        uint32_t id = slotIds.back();
        slotIds.pop_back();
        return id;
    }
};

// ─── Instruction stack annotation ────────────────────────────────────────────

/**
 * @brief Records which stack slots an instruction consumes and produces.
 *
 * After simulation, every instruction has an associated `InstrStackInfo`.
 */
struct InstrStackInfo {
    std::vector<uint32_t> pops;   ///< Slot IDs consumed (in pop order)
    std::vector<uint32_t> pushes; ///< Slot IDs produced (in push order)
};

// ─── Simulator options ────────────────────────────────────────────────────────

struct StackSimOptions {
    bool useLocalVarTable = true; ///< Consult LocalVariableTable for slot types
    bool meetAtJoins      = true; ///< Run fixed-point iteration at join points
    int  maxIter          = 32;   ///< Max fixed-point iterations per method
};

// ─── Result ───────────────────────────────────────────────────────────────────

struct StackSimResult {
    enum Status { OK, Error };

    Status      status = OK;
    std::string error;

    /// All named stack temporaries for this method.
    std::vector<StackSlot> slots;

    /// Stack state at entry of each block (block id → StackState).
    std::unordered_map<uint32_t, StackState> blockEntryStates;
    std::unordered_map<uint32_t, StackState> blockExitStates;

    /// Per-instruction stack annotation (instruction global id → InstrStackInfo).
    std::unordered_map<uint32_t, InstrStackInfo> instrInfo;

    const StackSlot* slot(uint32_t id) const;
};

// ─── JVM Stack Simulator ─────────────────────────────────────────────────────

/**
 * @brief Simulates the JVM operand stack for one method's BcCFG.
 *
 * Input: a BcCFG produced by JvmLifter (instructions still use stack-based
 *        BcOpcode values like PushInt, Add, etc.)
 * Output: StackSimResult with named slot assignments for every instruction.
 *
 * The simulator does NOT modify the BcCFG in place; the SlotCoalescer and
 * LocalRebuilder perform the actual rewriting in subsequent passes.
 */
class JvmStackSim {
public:
    explicit JvmStackSim(StackSimOptions opts = StackSimOptions{});

    StackSimResult simulate(const BcCFG& cfg,
                             const BcMethod& method);

private:
    StackSimOptions opts_;
    uint32_t        nextSlotId_ = 0;

    // Allocate a new slot with the given type.
    uint32_t newSlot(StackSimResult& result,
                     const BcType& type,
                     uint32_t defBlock,
                     uint32_t defOffset);

    // Process one basic block forward, updating `state`.
    void processBlock(const BcBasicBlock& blk,
                      StackState& state,
                      StackSimResult& result);

    // Infer the produced type from an instruction's opcode and operands.
    BcType inferPushType(const BcInstruction& insn,
                         const StackState& stateBeforeInsn,
                         const StackSimResult& result) const;

    // Compute the type-lattice meet of two types.
    // Returns the join type (least common supertype for refs).
    static BcType meetTypes(const BcType& a, const BcType& b);
};

} // namespace jvm_reconstruct
} // namespace retdec

#endif // RETDEC_JVM_RECONSTRUCT_STACK_SIM_H
