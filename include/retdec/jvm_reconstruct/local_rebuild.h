/**
 * @file include/retdec/jvm_reconstruct/local_rebuild.h
 * @brief Local variable reconstruction from JVM bytecode.
 *
 * ## Sources of local variable information
 *
 *   1. **LocalVariableTable attribute** (debug builds):
 *      Provides name, descriptor, and live-range for each slot.
 *      This is the ground truth and takes priority.
 *
 *   2. **LocalVariableTypeTable attribute** (generic signatures):
 *      Provides generic type signatures for the same slots.
 *
 *   3. **Type inference** (all builds):
 *      When debug tables are absent, the type of a JVM local variable slot
 *      is inferred from its first use:
 *        - iload / istore → int  (or boolean/byte/short/char if narrowed)
 *        - lload / lstore → long
 *        - fload / fstore → float
 *        - dload / dstore → double
 *        - aload / astore → the BcRefType seen from the stack simulation
 *
 *   4. **Parameter slots**:
 *      Slot 0 = `this` (for instance methods), slots 1..N = parameters.
 *      Types come from the method descriptor.
 *
 * ## Output
 *
 * Populates `BcMethod::locals` with `BcLocalVar` entries, each with:
 *   - `index`: JVM local variable slot index
 *   - `name`:  from debug info, or synthetic "param0", "v1", etc.
 *   - `type`:  from debug info or inferred
 *   - `isParam`: true for parameter slots
 *   - `startOffset`, `endOffset`: live range (from LocalVariableTable or
 *     conservatively [0, method_end))
 */

#ifndef RETDEC_JVM_RECONSTRUCT_LOCAL_REBUILD_H
#define RETDEC_JVM_RECONSTRUCT_LOCAL_REBUILD_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/jvm_reconstruct/stack_sim.h"
#include "retdec/jvm_reconstruct/slot_coalesce.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace jvm_reconstruct {

// ─── Debug table entries (from JVM LocalVariableTable attribute) ──────────────

struct LVTEntry {
    uint16_t    startPc;     ///< Bytecode offset where the variable is live
    uint16_t    length;      ///< Number of code bytes the variable is live
    std::string name;        ///< Variable name (from class file)
    std::string descriptor;  ///< JVM descriptor ("I", "Ljava/lang/String;", etc.)
    std::string signature;   ///< Generic signature (may be empty)
    uint16_t    index;       ///< JVM local variable slot index
};

// ─── Options ─────────────────────────────────────────────────────────────────

struct LocalRebuildOptions {
    bool useDebugTables  = true; ///< Prefer LocalVariableTable if available
    bool inferFromUsage  = true; ///< Fall back to type inference
    bool nameParameters  = true; ///< Generate param0..paramN names
    bool generateSyntheticNames = true; ///< Generate v0..vN for unnamed slots
};

// ─── Result ───────────────────────────────────────────────────────────────────

struct LocalRebuildResult {
    enum Status { OK, Error };
    Status      status = OK;
    std::string error;

    /// Reconstructed local variables (includes parameters).
    std::vector<BcLocalVar> locals;

    /// Map from JVM slot index → BcLocalVar index in `locals`.
    std::unordered_map<uint32_t, uint32_t> slotToLocal;
};

// ─── Local variable rebuilder ─────────────────────────────────────────────────

/**
 * @brief Reconstructs named local variables from JVM bytecode.
 *
 * Call after JvmStackSim and SlotCoalescer have annotated the method.
 */
class LocalRebuilder {
public:
    explicit LocalRebuilder(LocalRebuildOptions opts = LocalRebuildOptions{});

    /**
     * @brief Run local variable reconstruction on one method.
     *
     * @param method     The BcMethod (descriptor provides param types).
     * @param cfg        The BcCFG (for instruction scanning).
     * @param simResult  Stack simulation result.
     * @param coalesceResult  Coalescer result.
     * @param lvtEntries  LocalVariableTable entries (may be empty).
     */
    LocalRebuildResult rebuild(const BcMethod& method,
                                const BcCFG& cfg,
                                const StackSimResult& simResult,
                                const CoalesceResult& coalesceResult,
                                const std::vector<LVTEntry>& lvtEntries = {});

private:
    LocalRebuildOptions opts_;

    // Build a slot→type map from instruction scanning.
    std::unordered_map<uint32_t, BcType>
        inferSlotTypes(const BcCFG& cfg,
                       const StackSimResult& simResult) const;

    // Convert a JVM type descriptor to BcType.
    static BcType descriptorToType(const std::string& desc);

    // Assign a human-readable name to a slot.
    std::string nameSlot(uint32_t slotIdx,
                          const BcType& type,
                          bool isParam,
                          uint32_t paramOrdinal) const;
};

// ─── Exception variable introducer ────────────────────────────────────────────

/**
 * @brief Introduces the exception variable at the start of each handler block.
 *
 * In JVM bytecode, when an exception handler is entered, the caught exception
 * reference is pushed onto an empty stack.  This function:
 *   1. Identifies all handler-entry blocks.
 *   2. Creates a BcLocalVar for the caught exception.
 *   3. Inserts a synthetic StoreLocal instruction at the block entry.
 *
 * Called after LocalRebuilder so the variable name can follow the
 * LocalVariableTable if available.
 */
class ExceptionVarIntroducer {
public:
    void introduce(BcCFG& cfg,
                   BcMethod& method,
                   const StackSimResult& simResult,
                   LocalRebuildResult& localResult);
};

} // namespace jvm_reconstruct
} // namespace retdec

#endif // RETDEC_JVM_RECONSTRUCT_LOCAL_REBUILD_H
