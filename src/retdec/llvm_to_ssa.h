/**
 * @file src/retdec/llvm_to_ssa.h
 * @brief Adapter: build retdec::ssa::SSAModule from an llvm::Module.
 *
 * After the bin2llvmir pipeline runs, the LLVM module holds decoded functions
 * in SSA form.  This adapter populates retdec::ssa::SSAModule with enough
 * information for the analysis passes (algo_recover, concurrency_detect,
 * container_detect, sort_detect, ipa, etc.) to operate.
 *
 * The adapter maps:
 *   llvm::Function     → retdec::ssa::SSAFunction
 *   llvm::BasicBlock   → retdec::ssa::BasicBlock  (with instrs list)
 *   llvm::Instruction  → retdec::ssa::IrInstr      (op + calleeName + vma;
 *                        BinaryOperator ConstantInt operands become Immediate uses)
 *
 * Only instruction classes that the analysis passes actually inspect are
 * translated.  LLVM `PHINode` becomes `IrInstr::Op::Phi` with ConstantInt
 * incoming values as Immediate uses.  The `BasicBlock::phis` list is left
 * empty — that list is SSAPass output, and AccumulateDetector treats a
 * non-empty list as std::accumulate evidence.  SSA renaming and liveness
 * are still skipped because LLVM IR is already in SSA form.
 */

#pragma once
#include <memory>

namespace llvm {
class Module;
}
namespace retdec {
namespace ssa {
struct SSAModule;
}
} // namespace retdec

namespace retdec {

/**
 * Build a retdec::ssa::SSAModule from the decoded LLVM IR.
 *
 * Each non-declaration llvm::Function becomes one SSAFunction.  Each
 * llvm::BasicBlock becomes one ssa::BasicBlock.  Instructions are
 * translated to IrInstr with the following opcode mapping:
 *
 *   CallInst        → Call  (calleeName = demangled callee name if available)
 *   LoadInst        → Load
 *   StoreInst       → Store
 *   BranchInst      → Branch / CondBranch
 *   ReturnInst      → Ret
 *   BinaryOperator  → Add/Sub/Mul/Div/And/… (SRem/URem map to Div)
 *   ICmpInst/FCmpInst → Compare
 *   PHINode         → Phi (instruction only; PhiNode list stays empty)
 *   Other           → Assign (conservative fallback)
 *
 * The returned SSAModule is heap-allocated and owned by the caller.
 */
std::unique_ptr<retdec::ssa::SSAModule> buildSsaModule(const llvm::Module& m);

} // namespace retdec
