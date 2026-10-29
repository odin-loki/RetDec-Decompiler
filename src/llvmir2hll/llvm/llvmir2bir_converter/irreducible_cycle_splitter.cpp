/**
* @file src/llvmir2hll/llvm/llvmir2bir_converter/irreducible_cycle_splitter.cpp
* @brief Makes small irreducible cycles reducible by duplicating entry blocks.
* @copyright (c) 2026 Avast Software, licensed under the MIT license
*/

#include <map>
#include <set>
#include <vector>

#include <llvm/ADT/SCCIterator.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/SSAUpdater.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "retdec/llvmir2hll/llvm/llvmir2bir_converter/irreducible_cycle_splitter.h"

namespace retdec {
namespace llvmir2hll {

namespace {

using BBSet = std::set<llvm::BasicBlock *>;
using BBVector = std::vector<llvm::BasicBlock *>;

unsigned weightOf(llvm::BasicBlock *bb) {
	unsigned w = 0;
	for (auto &inst : *bb) {
		if (!llvm::isa<llvm::PHINode>(inst) && !inst.isDebugOrPseudoInst()) {
			++w;
		}
	}
	return w;
}

/// Blocks of @a scc reachable from @a entries without passing @a header, in
/// an order where every block comes after its predecessors in the region.
/// Returns false when the region has a cycle of its own.
bool regionToCopy(const BBSet &scc, llvm::BasicBlock *header,
		const BBVector &entries, BBVector &order) {
	BBSet region;
	BBVector work(entries.begin(), entries.end());
	while (!work.empty()) {
		auto bb = work.back();
		work.pop_back();
		if (bb == header || !region.insert(bb).second) {
			continue;
		}
		for (auto succ : llvm::successors(bb)) {
			if (scc.count(succ)) {
				work.push_back(succ);
			}
		}
	}
	// Topological order (Kahn); leftovers mean a cycle.
	std::map<llvm::BasicBlock *, unsigned> indeg;
	for (auto bb : region) {
		indeg[bb] = 0;
	}
	for (auto bb : region) {
		for (auto succ : llvm::successors(bb)) {
			if (region.count(succ)) {
				++indeg[succ];
			}
		}
	}
	BBVector ready;
	for (auto bb : region) {
		if (indeg[bb] == 0) {
			ready.push_back(bb);
		}
	}
	order.clear();
	while (!ready.empty()) {
		auto bb = ready.back();
		ready.pop_back();
		order.push_back(bb);
		for (auto succ : llvm::successors(bb)) {
			if (region.count(succ) && --indeg[succ] == 0) {
				ready.push_back(succ);
			}
		}
	}
	return order.size() == region.size();
}

/// Copies @a region, sends the edges from outside @a scc into it to the
/// copies and repairs PHIs and SSA form.
void copyRegion(llvm::Function &func, const BBSet &scc, const BBVector &region) {
	llvm::ValueToValueMapTy vmap;
	std::map<llvm::BasicBlock *, llvm::BasicBlock *> copyOf;
	BBSet copies;
	for (auto bb : region) {
		auto copy = llvm::CloneBasicBlock(bb, vmap, ".split", &func);
		vmap[bb] = copy;
		copyOf[bb] = copy;
		copies.insert(copy);
	}

	// Edges into each region block that move to its copy: those from outside
	// the cycle, and those from (the copies of) other region blocks.
	for (auto bb : region) {
		auto copy = copyOf[bb];
		for (auto &inst : *copy) {
			llvm::RemapInstruction(&inst, vmap,
				llvm::RF_IgnoreMissingLocals | llvm::RF_NoModuleLevelChanges);
		}
	}
	// Redirect outside edges into the region entries.
	for (auto bb : region) {
		BBVector outsidePreds;
		for (auto pred : llvm::predecessors(bb)) {
			if (!scc.count(pred)) {
				outsidePreds.push_back(pred);
			}
		}
		for (auto pred : outsidePreds) {
			pred->getTerminator()->replaceSuccessorWith(bb, copyOf[bb]);
		}
	}
	// PHIs. In a copy keep only incomings from its new predecessors (outside
	// blocks, unchanged, and region copies, which RemapInstruction already
	// substituted for region originals); in an original drop the incomings
	// that moved to the copy.
	for (auto bb : region) {
		auto copy = copyOf[bb];
		std::set<llvm::BasicBlock *> copyPreds(llvm::pred_begin(copy), llvm::pred_end(copy));
		std::set<llvm::BasicBlock *> origPreds(llvm::pred_begin(bb), llvm::pred_end(bb));
		for (auto &phi : copy->phis()) {
			for (unsigned i = phi.getNumIncomingValues(); i-- > 0;) {
				if (!copyPreds.count(phi.getIncomingBlock(i))) {
					phi.removeIncomingValue(i, false);
				}
			}
		}
		for (auto &phi : bb->phis()) {
			for (unsigned i = phi.getNumIncomingValues(); i-- > 0;) {
				if (!origPreds.count(phi.getIncomingBlock(i))) {
					phi.removeIncomingValue(i, false);
				}
			}
		}
	}
	// Successors outside the region (the header, cycle exits) gain the copies
	// as predecessors.
	for (auto bb : region) {
		auto copy = copyOf[bb];
		// One incoming per edge: a block that branches twice to the same
		// successor needs two.
		for (auto succ : llvm::successors(copy)) {
			if (copies.count(succ)) {
				continue;
			}
			for (auto &phi : succ->phis()) {
				auto idx = phi.getBasicBlockIndex(bb);
				if (idx < 0) {
					continue;
				}
				auto v = phi.getIncomingValue(idx);
				auto it = vmap.find(v);
				llvm::Value *nv = it != vmap.end() ? static_cast<llvm::Value *>(it->second) : v;
				phi.addIncoming(nv, copy);
			}
		}
	}
	// SSA repair: a value defined in the region now has two definitions.
	// Snapshot the instructions first: the updater inserts PHIs, also into
	// region blocks, and those have no copy.
	std::vector<std::pair<llvm::Instruction *, llvm::BasicBlock *>> defs;
	for (auto bb : region) {
		for (auto &inst : *bb) {
			defs.emplace_back(&inst, bb);
		}
	}
	for (auto &def : defs) {
		auto &inst = *def.first;
		auto bb = def.second;
		auto copy = copyOf[bb];
		{
			auto clone = llvm::cast<llvm::Instruction>(vmap[&inst]);
			std::vector<llvm::Use *> uses;
			for (auto &use : inst.uses()) {
				auto user = llvm::cast<llvm::Instruction>(use.getUser());
				auto userBB = user->getParent();
				if (auto phi = llvm::dyn_cast<llvm::PHINode>(user)) {
					userBB = phi->getIncomingBlock(use);
				}
				if (userBB != bb) {
					uses.push_back(&use);
				}
			}
			if (uses.empty()) {
				continue;
			}
			llvm::SSAUpdater updater;
			updater.Initialize(inst.getType(), inst.getName());
			updater.AddAvailableValue(bb, &inst);
			updater.AddAvailableValue(copy, clone);
			for (auto use : uses) {
				updater.RewriteUse(*use);
			}
		}
	}
}

} // anonymous namespace

unsigned splitIrreducibleCycles(llvm::Function &func,
		unsigned maxInstsPerCycle, unsigned maxInstsPerFunc) {
	if (func.isDeclaration()) {
		return 0;
	}
	unsigned added = 0;
	// Each split can expose another irreducible cycle, so repeat; the budget
	// bounds the work.
	for (bool changed = true; changed;) {
		changed = false;
		for (auto it = llvm::scc_begin(&func); !it.isAtEnd(); ++it) {
			const auto &sccVec = *it;
			if (sccVec.size() < 2) {
				continue;
			}
			BBSet scc(sccVec.begin(), sccVec.end());
			BBVector entries;
			for (auto bb : sccVec) {
				bool entry = bb == &func.getEntryBlock();
				for (auto pred : llvm::predecessors(bb)) {
					entry |= !scc.count(pred);
				}
				if (entry) {
					entries.push_back(bb);
				}
			}
			if (entries.size() < 2) {
				continue;
			}
			// Keep as the header the block that needs the least copying. It
			// need not be an entry: when two entries each start an inner
			// cycle, the block where their paths meet is the only header
			// that leaves the copied blocks acyclic (heapsort's sift-down).
			BBVector best;
			unsigned bestWeight = ~0u;
			for (auto header : sccVec) {
				BBVector others;
				bool entryBlockCopied = false;
				for (auto e : entries) {
					if (e != header) {
						others.push_back(e);
						entryBlockCopied |= e == &func.getEntryBlock();
					}
				}
				BBVector order;
				if (entryBlockCopied || !regionToCopy(scc, header, others, order)) {
					continue;
				}
				unsigned w = 0;
				for (auto bb : order) {
					w += weightOf(bb);
				}
				if (w < bestWeight) {
					bestWeight = w;
					best = order;
				}
			}
			if (best.empty() || bestWeight > maxInstsPerCycle ||
					added + bestWeight > maxInstsPerFunc) {
				continue;
			}
			copyRegion(func, scc, best);
			added += bestWeight;
			changed = true;
			break; // the SCC iterator is stale now
		}
	}
	return added;
}

} // namespace llvmir2hll
} // namespace retdec
