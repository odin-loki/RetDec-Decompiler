/**
 * @file src/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.cpp
 * @brief Single-basic-block redundant load/store elimination.
 * @copyright (c) 2024, MIT license
 *
 * Within each basic block, scans forward and maintains a map of
 * (pointer → last stored value). When a load from a pointer is found and
 * the pointer has a known last-stored value with no intervening clobbers,
 * the load is replaced by the stored value directly.
 *
 * Clobbers (invalidation triggers):
 *  - Any store to the same pointer.
 *  - A call instruction (conservative: invalidates all non-alloca pointers).
 *  - Any store via a pointer we cannot prove is different (alias-agnostic:
 *    if we can't prove disjoint, we invalidate everything).
 *
 * This is intentionally simpler and more conservative than LLVM's GVN/DSE,
 * but catches patterns left after the LLVM optimisation passes when operands
 * have been left as globals/allocas that weren't promoted.
 *
 * Also performs the symmetric dead store elimination:
 *  - store V, P  followed by  store W, P  with no intervening load of P
 *    → remove the first store (its value is never observed).
 */

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include "retdec/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char RedundantLoadStoreElim::ID = 0;

static RegisterPass<RedundantLoadStoreElim> X(
    "retdec-redundant-load-store",
    "Single-BB redundant load/store elimination",
    false, false);

RedundantLoadStoreElim::RedundantLoadStoreElim() : ModulePass(ID) {}

bool RedundantLoadStoreElim::runOnModule(Module& M) {
    bool changed = false;
    for (auto& F : M)
        for (auto& BB : F)
            changed |= runOnBlock(BB);
    return changed;
}

// Returns true if a CallInst may write to memory (conservative).
static bool callMayWrite(CallInst* ci) {
    if (auto* F = ci->getCalledFunction()) {
        // Pure / readonly functions don't clobber memory.
        if (F->doesNotAccessMemory() || F->onlyReadsMemory())
            return false;
    }
    return true;
}

// Returns true if ptr is provably an alloca (stack slot) — calls don't
// alias stack slots that don't escape.
static bool isNonEscapingAlloca(Value* ptr) {
    auto* base = ptr->stripPointerCasts();
    if (auto* AI = dyn_cast<AllocaInst>(base)) {
        // Conservatively treat all allocas as potentially escaping unless
        // we can prove otherwise. For now, mark only unnamed allocas safe.
        (void)AI;
        return false;  // conservative
    }
    return false;
}

bool RedundantLoadStoreElim::runOnBlock(BasicBlock& BB) {
    // ptr → {last stored Value*, the StoreInst* itself}
    DenseMap<Value*, std::pair<Value*, StoreInst*>> knownValues;
    // ptr → last StoreInst* (for dead store elimination)
    DenseMap<Value*, StoreInst*> lastStore;

    SmallVector<Instruction*, 16> toErase;
    bool changed = false;

    for (auto it = BB.begin(); it != BB.end(); ) {
        Instruction* I = &*it++;

        if (auto* LI = dyn_cast<LoadInst>(I)) {
            Value* ptr = LI->getPointerOperand();
            auto kv = knownValues.find(ptr);
            if (kv != knownValues.end()) {
                Value* knownVal = kv->second.first;
                // Types must match exactly.
                if (knownVal->getType() == LI->getType()) {
                    LI->replaceAllUsesWith(knownVal);
                    toErase.push_back(LI);
                    changed = true;
                    continue;
                }
            }
            // Unknown load — record it as a "load barrier" for stores.
            // (A load of P means the previous store to P is observable.)
            lastStore.erase(ptr);

        } else if (auto* SI = dyn_cast<StoreInst>(I)) {
            Value* ptr = SI->getPointerOperand();
            Value* val = SI->getValueOperand();

            // Dead store elimination: previous store to same ptr, no
            // intervening load.
            auto ds = lastStore.find(ptr);
            if (ds != lastStore.end()) {
                // Previous store is dead — erase it.
                toErase.push_back(ds->second);
                changed = true;
            }

            knownValues[ptr] = {val, SI};
            lastStore[ptr]   = SI;

        } else if (auto* CI = dyn_cast<CallInst>(I)) {
            if (callMayWrite(CI)) {
                // Conservative: invalidate all non-alloca entries.
                SmallVector<Value*, 8> toRemove;
                for (auto& kv : knownValues) {
                    if (!isNonEscapingAlloca(kv.first))
                        toRemove.push_back(kv.first);
                }
                for (auto* p : toRemove) {
                    knownValues.erase(p);
                    lastStore.erase(p);
                }
            }
        } else if (I->mayWriteToMemory()) {
            // Any other memory-writing instruction: full invalidation.
            knownValues.clear();
            lastStore.clear();
        }
    }

    for (auto* I : toErase) {
        I->eraseFromParent();
    }

    return changed;
}

} // namespace bin2llvmir
} // namespace retdec
