/**
 * @file src/retdec/llvm_to_ssa.cpp
 * @brief Adapter: build retdec::ssa::SSAModule from an llvm::Module.
 */

#include "llvm_to_ssa.h"
#include "retdec/ssa/ssa.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace retdec {

namespace {

/// Return the human-readable name of a called function, falling back to the
/// mangled LLVM name if demangling is not available.
static std::string calleeName(const llvm::CallInst& ci) {
    const llvm::Function* f = ci.getCalledFunction();
    if (!f) return "";                   // indirect call
    return f->getName().str();
}

/// Map an LLVM binary opcode to the nearest ssa::IrInstr::Op.
static ssa::IrInstr::Op binOp(unsigned llvmOpc) {
    using Op = ssa::IrInstr::Op;
    switch (llvmOpc) {
    case llvm::Instruction::Add:
    case llvm::Instruction::FAdd: return Op::Add;
    case llvm::Instruction::Sub:
    case llvm::Instruction::FSub: return Op::Sub;
    case llvm::Instruction::Mul:
    case llvm::Instruction::FMul: return Op::Mul;
    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv:
    case llvm::Instruction::FDiv: return Op::Div;
    case llvm::Instruction::And:  return Op::And;
    case llvm::Instruction::Or:   return Op::Or;
    case llvm::Instruction::Xor:  return Op::Xor;
    case llvm::Instruction::Shl:  return Op::Shl;
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr: return Op::Shr;
    default:                      return Op::Assign;
    }
}

/// Translate one LLVM instruction into an ssa::IrInstr and append it to
/// the given basic block.  Returns nullptr if the instruction should be
/// skipped (e.g. alloca, getelementptr, unreachable).
static ssa::IrInstr* translateInstr(const llvm::Instruction& li,
                                    ssa::SSAFunction& fn,
                                    ssa::BasicBlock& blk) {
    using Op = ssa::IrInstr::Op;

    // Determine VMA: use debug-info location if present; otherwise 0.
    uint64_t vma = 0;
    if (const llvm::DebugLoc& loc = li.getDebugLoc())
        vma = loc.getLine();   // line is a proxy; real VMA comes from metadata

    // Try to read the retdec address metadata attached by bin2llvmir.
    if (const auto* md = li.getMetadata("retdec.addr")) {
        if (md->getNumOperands() > 0) {
            if (const auto* ci = llvm::dyn_cast<llvm::ConstantInt>(
                    llvm::dyn_cast<llvm::ValueAsMetadata>(
                        md->getOperand(0).get())->getValue()))
                vma = ci->getZExtValue();
        }
    }

    Op op = Op::Assign;
    std::string calleeStr;

    if (llvm::isa<llvm::CallInst>(li)) {
        const auto& ci = llvm::cast<llvm::CallInst>(li);
        op        = Op::Call;
        calleeStr = calleeName(ci);
    } else if (llvm::isa<llvm::LoadInst>(li)) {
        op = Op::Load;
    } else if (llvm::isa<llvm::StoreInst>(li)) {
        op = Op::Store;
    } else if (llvm::isa<llvm::ReturnInst>(li)) {
        op = Op::Ret;
    } else if (const auto* bi = llvm::dyn_cast<llvm::BranchInst>(&li)) {
        op = bi->isConditional() ? Op::CondBranch : Op::Branch;
    } else if (llvm::isa<llvm::ICmpInst>(li) || llvm::isa<llvm::FCmpInst>(li)) {
        op = Op::Compare;
    } else if (const auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&li)) {
        op = binOp(bo->getOpcode());
    } else if (llvm::isa<llvm::AllocaInst>(li) ||
               llvm::isa<llvm::GetElementPtrInst>(li) ||
               llvm::isa<llvm::BitCastInst>(li) ||
               llvm::isa<llvm::TruncInst>(li) ||
               llvm::isa<llvm::ZExtInst>(li) ||
               llvm::isa<llvm::SExtInst>(li) ||
               llvm::isa<llvm::UnreachableInst>(li)) {
        return nullptr;   // not interesting for analysis passes
    }

    ssa::IrInstr* instr = fn.addInstr(blk.id, op, vma);
    if (instr && !calleeStr.empty())
        instr->calleeName = std::move(calleeStr);

    // For Ret: record the return value as a use so AbiSeeder can find it.
    if (op == Op::Ret && instr) {
        if (!li.getOperand(0) || llvm::isa<llvm::UndefValue>(li.getOperand(0))) {
            // void return — no use
        } else {
            // We can't recover full SSA value IDs here without running the
            // full SSA construction pass, so leave uses empty.  The analysis
            // passes that truly need return-value IDs should use the full
            // SSAPass on the output of a proper IR builder.
        }
    }

    return instr;
}

} // anonymous namespace

std::unique_ptr<ssa::SSAModule> buildSsaModule(const llvm::Module& m) {
    auto mod = std::make_unique<ssa::SSAModule>();

    for (const llvm::Function& lf : m) {
        if (lf.isDeclaration()) continue;   // external symbol — skip

        ssa::SSAFunction* fn = mod->addFunction(lf.getName().str());

        // Map LLVM basic-block pointer → ssa::BlockId for edge construction.
        std::unordered_map<const llvm::BasicBlock*, ssa::BlockId> bbMap;

        // First pass: create one ssa::BasicBlock per LLVM basic block.
        for (const llvm::BasicBlock& lb : lf) {
            ssa::BasicBlock* blk = fn->addBlock(lb.getName().str());
            bbMap[&lb] = blk->id;
        }

        // Second pass: translate instructions and wire CFG edges.
        for (const llvm::BasicBlock& lb : lf) {
            ssa::BlockId blkId = bbMap.at(&lb);
            ssa::BasicBlock* blk = fn->block(blkId);
            if (!blk) continue;

            for (const llvm::Instruction& li : lb)
                translateInstr(li, *fn, *blk);

            // Successor edges
            const llvm::Instruction* term = lb.getTerminator();
            if (term) {
                for (unsigned i = 0, n = term->getNumSuccessors(); i < n; ++i) {
                    const llvm::BasicBlock* succ = term->getSuccessor(i);
                    auto it = bbMap.find(succ);
                    if (it != bbMap.end())
                        blk->addSucc(it->second);
                }
            }
        }

        // Predecessor edges (reverse of successors)
        for (const llvm::BasicBlock& lb : lf) {
            ssa::BlockId blkId = bbMap.at(&lb);
            ssa::BasicBlock* blk = fn->block(blkId);
            if (!blk) continue;
            for (ssa::BlockId succId : blk->succs) {
                ssa::BasicBlock* succBlk = fn->block(succId);
                if (succBlk)
                    succBlk->addPred(blkId);
            }
        }
    }

    return mod;
}

} // namespace retdec
