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
 *  - Any store at all. There is no alias analysis here, and two different
 *    Value* naming one address is the normal case in lifted code, so a store
 *    is taken to have changed everything.
 *  - A call that may write memory.
 *  - Any other instruction that may write memory.
 *
 * A read -- an ordinary load, or a call that only reads -- does not change
 * what is known, but it does make the preceding store observable, so it ends
 * that store's eligibility for removal.
 *
 * Volatile and atomic accesses are barriers and are never folded away: the
 * access itself is the point.
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
#include <llvm/IR/DataLayout.h>

#include "retdec/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char RedundantLoadStoreElim::ID = 0;

static RegisterPass<RedundantLoadStoreElim>
	X("retdec-redundant-load-store", "Single-BB redundant load/store elimination", false, false);

RedundantLoadStoreElim::RedundantLoadStoreElim(): ModulePass(ID) {}

bool RedundantLoadStoreElim::runOnModule(Module& M)
{
	bool changed = false;
	for (auto& F: M)
		for (auto& BB: F)
			changed |= runOnBlock(BB);
	return changed;
}

// Returns true if a CallInst may write to memory (conservative).
static bool callMayWrite(CallInst* ci)
{
	if (auto* F = ci->getCalledFunction())
	{
		// Pure / readonly functions don't clobber memory.
		if (F->doesNotAccessMemory() || F->onlyReadsMemory()) return false;
	}
	return true;
}

// Returns true if a CallInst may read memory. A read matters as much as a
// write here: a readonly callee cannot invalidate a value we already know, but
// it CAN observe the store that produced it, so that store is not dead.
static bool callMayRead(CallInst* ci)
{
	if (auto* F = ci->getCalledFunction())
	{
		if (F->doesNotAccessMemory()) return false;
	}
	return true;
}

bool RedundantLoadStoreElim::runOnBlock(BasicBlock& BB)
{
	// ptr → {last stored Value*, the StoreInst* itself}
	DenseMap<Value*, std::pair<Value*, StoreInst*>> knownValues;
	// ptr → last StoreInst* (for dead store elimination)
	DenseMap<Value*, StoreInst*> lastStore;

	const DataLayout& DL = BB.getModule()->getDataLayout();

	SmallVector<Instruction*, 16> toErase;
	bool changed = false;

	for (auto it = BB.begin(); it != BB.end();)
	{
		Instruction* I = &*it++;

		if (auto* LI = dyn_cast<LoadInst>(I))
		{
			// A volatile or atomic load is an event, not just a read: it may
			// not be answered from a remembered value, and it orders
			// everything around it. Treat it as a full barrier.
			if (!LI->isSimple())
			{
				knownValues.clear();
				lastStore.clear();
				continue;
			}

			Value* ptr = LI->getPointerOperand();
			auto kv = knownValues.find(ptr);
			if (kv != knownValues.end())
			{
				Value* knownVal = kv->second.first;
				// Types must match exactly.
				if (knownVal->getType() == LI->getType())
				{
					LI->replaceAllUsesWith(knownVal);
					toErase.push_back(LI);
					changed = true;
					continue;
				}
			}
			// Unknown load — record it as a "load barrier" for stores.
			// (A load of P means the previous store to P is observable.)
			lastStore.erase(ptr);
		}
		else if (auto* SI = dyn_cast<StoreInst>(I))
		{
			if (!SI->isSimple())
			{
				knownValues.clear();
				lastStore.clear();
				continue;
			}

			Value* ptr = SI->getPointerOperand();
			Value* val = SI->getValueOperand();

			// Dead store elimination: previous store to same ptr, no
			// intervening load. The new store only makes the old one dead if
			// it covers at least as many bytes -- `store i64` then `store i32`
			// to one pointer leaves the top four bytes of the i64 live.
			auto ds = lastStore.find(ptr);
			if (ds != lastStore.end())
			{
				auto oldSize = DL.getTypeStoreSize(ds->second->getValueOperand()->getType());
				auto newSize = DL.getTypeStoreSize(val->getType());
				if (newSize.isScalable() == oldSize.isScalable()
					&& newSize.getKnownMinValue() >= oldSize.getKnownMinValue())
				{
					toErase.push_back(ds->second);
					changed = true;
				}
			}

			// This map is keyed on the pointer Value*, and two different
			// Value* routinely name one address in lifted code -- a global
			// reached directly and through a zero GEP, or the same constant
			// address materialised twice. Without an alias analysis the only
			// sound reading of a store is that it may have changed anything,
			// which is what this file's own header says it does.
			knownValues.clear();
			lastStore.clear();

			knownValues[ptr] = {val, SI};
			lastStore[ptr] = SI;
		}
		else if (auto* CI = dyn_cast<CallInst>(I))
		{
			if (callMayWrite(CI))
			{
				knownValues.clear();
				lastStore.clear();
			}
			else if (callMayRead(CI))
			{
				// It cannot change what we know, but it can see it, so no
				// preceding store is dead across it.
				lastStore.clear();
			}
		}
		else if (I->mayWriteToMemory())
		{
			// Any other memory-writing instruction: full invalidation.
			knownValues.clear();
			lastStore.clear();
		}
		else if (I->mayReadFromMemory())
		{
			lastStore.clear();
		}
	}

	for (auto* I: toErase)
	{
		I->eraseFromParent();
	}

	return changed;
}

} // namespace bin2llvmir
} // namespace retdec
