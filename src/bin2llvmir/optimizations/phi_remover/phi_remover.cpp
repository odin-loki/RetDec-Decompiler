/**
* @file src/bin2llvmir/optimizations/phi_remover/phi_remover.cpp
* @brief Remove all Phi nodes — with coalescing pre-pass.
* @copyright (c) 2017 Odin Loch Trading as Imortek (original)
* @copyright (c) 2024, MIT license (coalescing improvement)
*
* Improvement over the original:
*
*   The original demotes every PHI to a stack alloca independently, producing
*   one alloca+store-per-pred+load per PHI.  When multiple PHIs in the same
*   block have identical sets of incoming (block, value) pairs — which is
*   common after copy propagation — they can share a single alloca.
*
*   The coalescing pre-pass merges such PHIs before demotion, reducing the
*   number of allocas and improving the quality of output C variable names.
*
*   Additionally: PHIs where ALL incoming values are the same constant or
*   variable (trivial PHIs) are replaced with that value directly — no alloca
*   needed.
*/

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/Local.h>

#include "retdec/bin2llvmir/optimizations/phi_remover/phi_remover.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"

namespace retdec {
namespace bin2llvmir {

char PhiRemover::ID = 0;

static llvm::RegisterPass<PhiRemover> X(
    "retdec-remove-phi",
    "Phi removal",
    false,
    false
);

PhiRemover::PhiRemover() : ModulePass(ID) {}

bool PhiRemover::runOnModule(llvm::Module& M) {
    _module = &M;
    _config = ConfigProvider::getConfig(_module);
    return run();
}

bool PhiRemover::runOnModuleCustom(llvm::Module& M, Config* c) {
    _module = &M;
    _config = c;
    return run();
}

common::Address getInstAddress(const llvm::Instruction* i) {
    if (llvm::MDNode* mdn = i->getMetadata("insn.addr")) {
        llvm::ConstantInt* CI = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
            mdn->getOperand(0));
        return CI->getZExtValue();
    }
    return common::Address::Undefined;
}

llvm::MDNode* getInstAddressMeta(common::Address a, llvm::Module* m) {
    return llvm::MDNode::get(
        m->getContext(),
        llvm::ValueAsMetadata::get(llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(m->getContext()), a, false)));
}

//===========================================================================
// Coalescing pre-pass
//===========================================================================

/// Replace trivial PHIs (all incoming values identical) with the value.
/// Returns true if any PHI was replaced.
static bool eliminateTrivialPhis(llvm::Function& fn) {
    bool changed = false;
    for (auto& bb : fn) {
        for (auto it = bb.begin(); it != bb.end(); ) {
            auto* phi = llvm::dyn_cast<llvm::PHINode>(&*it);
            if (!phi) break; // PHIs are always at the top of a block.
            ++it;

            if (phi->getNumIncomingValues() == 0) {
                phi->replaceAllUsesWith(llvm::UndefValue::get(phi->getType()));
                phi->eraseFromParent();
                changed = true;
                continue;
            }

            // Check if all incoming values are the same.
            llvm::Value* first = phi->getIncomingValue(0);
            bool trivial = true;
            for (unsigned i = 1; i < phi->getNumIncomingValues(); ++i) {
                if (phi->getIncomingValue(i) != first) { trivial = false; break; }
            }
            if (trivial) {
                phi->replaceAllUsesWith(first);
                phi->eraseFromParent();
                changed = true;
            }
        }
    }
    return changed;
}

/// Coalesce PHIs with identical (block, value) incoming sets.
/// They will map to the same alloca after demotion.
static bool coalescePhis(llvm::Function& fn) {
    bool changed = false;
    for (auto& bb : fn) {
        // Collect all PHIs in this block.
        std::vector<llvm::PHINode*> phis;
        for (auto& inst : bb) {
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(&inst))
                phis.push_back(phi);
            else break;
        }
        if (phis.size() < 2) continue;

        // Group PHIs by their incoming signature: a sorted vector of
        // (block*, value*) pairs.
        using Sig = std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>>;
        std::map<Sig, llvm::PHINode*> seen;

        for (auto* phi : phis) {
            if (phi->getType()->isVoidTy()) continue;

            Sig sig;
            sig.reserve(phi->getNumIncomingValues());
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
                sig.emplace_back(phi->getIncomingBlock(i),
                                  phi->getIncomingValue(i));
            std::sort(sig.begin(), sig.end());

            auto it = seen.find(sig);
            if (it == seen.end()) {
                seen[sig] = phi;
            } else {
                // Types must match for safe replacement.
                if (it->second->getType() == phi->getType()) {
                    phi->replaceAllUsesWith(it->second);
                    phi->eraseFromParent();
                    changed = true;
                }
            }
        }
    }
    return changed;
}

//===========================================================================
// Main run
//===========================================================================

bool PhiRemover::run() {
    bool changed = false;

    for (llvm::Function& f : *_module) {
        if (f.isDeclaration()) continue;

        // Pre-pass 1: eliminate trivial PHIs.
        changed |= eliminateTrivialPhis(f);
        // Pre-pass 2: coalesce identical PHIs.
        changed |= coalescePhis(f);
    }

    // Demotion pass (original logic).
    for (llvm::Function& f : *_module) {
        if (f.isDeclaration()) continue;

        auto faddr = _config->getFunctionAddress(&f);
        llvm::MDNode* fmeta = nullptr;

        auto* entryBb = &f.getEntryBlock();
        llvm::BasicBlock::iterator insertIt = entryBb->begin();
        while (llvm::isa<llvm::AllocaInst>(insertIt)) ++insertIt;

        for (auto it = llvm::inst_begin(&f), eIt = llvm::inst_end(&f); it != eIt;) {
            llvm::Instruction* insn = &*it++;
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(insn)) {
                if (!fmeta)
                    fmeta = getInstAddressMeta(faddr, _module);
                changed |= demotePhiToStack(phi, fmeta);
            }
        }
    }

    return changed;
}

//===========================================================================
// demotePhiToStack (original implementation preserved)
//===========================================================================

bool PhiRemover::demotePhiToStack(llvm::PHINode* phi, llvm::MDNode* faddr) {
    if (phi->use_empty()) {
        phi->eraseFromParent();
        return true;
    }

    const llvm::DataLayout& DL = phi->getModule()->getDataLayout();
    llvm::Function* F = phi->getParent()->getParent();

    auto* alloca = new llvm::AllocaInst(
        phi->getType(),
        DL.getAllocaAddrSpace(),
        nullptr,
        phi->getName() + ".reg2mem",
        &F->getEntryBlock().front());
    alloca->setMetadata("insn.addr", faddr);

    llvm::Type* phiTy = phi->getType();
    for (unsigned i = 0, e = phi->getNumIncomingValues(); i < e; ++i) {
        llvm::Value* inc = phi->getIncomingValue(i);
        if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(inc))
            assert(II->getParent() != phi->getIncomingBlock(i));
        auto* insertInsn = phi->getIncomingBlock(i)->getTerminator();
        // Coerce incoming value to match alloca type if needed (e.g. after
        // load/store type fixes that truncated i64 to i32).
        if (inc->getType() != phiTy) {
            if (phiTy->isIntegerTy() && inc->getType()->isIntegerTy())
                inc = llvm::CastInst::CreateIntegerCast(inc, phiTy, false, "", insertInsn);
            else if (phiTy->isPointerTy() && inc->getType()->isPointerTy())
                inc = llvm::CastInst::CreatePointerBitCastOrAddrSpaceCast(inc, phiTy, "", insertInsn);
        }
        auto a = getInstAddress(insertInsn);
        auto* s = new llvm::StoreInst(inc, alloca, insertInsn);
        if (a.isDefined())
            s->setMetadata("insn.addr", getInstAddressMeta(a, _module));
    }

    llvm::BasicBlock::iterator InsertPt = phi->getIterator();
    for (; llvm::isa<llvm::PHINode>(InsertPt) || InsertPt->isEHPad(); ++InsertPt);

    auto a = getInstAddress(phi);
    auto* l = new llvm::LoadInst(alloca, phi->getName() + ".reload", &*InsertPt);
    if (a.isDefined())
        l->setMetadata("insn.addr", getInstAddressMeta(a, _module));

    phi->replaceAllUsesWith(l);
    phi->eraseFromParent();
    return alloca;
}

} // namespace bin2llvmir
} // namespace retdec
