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
 *   llvm::Instruction  → retdec::ssa::IrInstr      (op + calleeName + vma)
 *
 * Only instruction classes that the analysis passes actually inspect are
 * translated.  Phi nodes, SSA renaming, and liveness analysis are deliberately
 * skipped here because the LLVM IR is already in SSA form and the analysis
 * passes need only call-graph information and basic control-flow structure.
 */

#pragma once
#include <memory>

namespace llvm  { class Module; }
namespace retdec { namespace ssa { struct SSAModule; } }

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
 *   BinaryOperator  → Add/Sub/Mul/… (mapped by LLVM opcode)
 *   ICmpInst/FCmpInst → Compare
 *   Other           → Assign (conservative fallback)
 *
 * The returned SSAModule is heap-allocated and owned by the caller.
 */
std::unique_ptr<retdec::ssa::SSAModule> buildSsaModule(const llvm::Module& m);

} // namespace retdec
