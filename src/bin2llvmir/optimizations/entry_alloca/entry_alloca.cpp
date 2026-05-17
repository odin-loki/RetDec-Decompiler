/**
 * @file src/bin2llvmir/optimizations/entry_alloca/entry_alloca.cpp
 * @brief Ensure all constant-size allocas live in function entry blocks.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 *
 * This pass fixes two classes of IR invalidity that can arise after
 * simplifycfg and similar transformations:
 *
 * 1. Constant-size alloca instructions that got sunk out of the function
 *    entry block.  LLVM's mem2reg and the verifier both require constant-size
 *    allocas to dominate all their uses, which is only guaranteed when they
 *    live in the entry block.
 *
 * 2. Instruction operands that reference an AllocaInst defined in a
 *    *different* function.  This can happen when the stack analysis pass
 *    creates an alloca for function A at a given stack offset, but a
 *    subsequent pass resolves the same stack offset in function B to the
 *    already-named alloca from function A (e.g. via a name-based lookup
 *    that crosses function boundaries).  The LLVM verifier reports such
 *    cross-function alloca uses as dominance failures because the defining
 *    instruction is in a different function's entry block.
 */

#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/IRBuilder.h>

#include "retdec/bin2llvmir/optimizations/entry_alloca/entry_alloca.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char EntryAlloca::ID = 0;

static RegisterPass<EntryAlloca> X(
	"retdec-entry-alloca",
	"Hoist non-entry constant-size allocas to function entry block",
	false,
	false);

EntryAlloca::EntryAlloca() : ModulePass(ID) {}

// Track how many times this pass has run.  The first invocation (before
// heavy optimization passes) only does alloca/type fixes.  The second
// invocation (just before retdec-llvmir2hll) additionally lowers vector
// ops to scalar arithmetic so the HLL writer never sees vector types.
static int sEntryAllocaInvocation = 0;

static bool traceEnabled()
{
	const char* e = std::getenv("RETDEC_ENTRY_ALLOCA_TRACE");
	return e && *e && std::string(e) != "0";
}

static void eaTrace(const std::string& msg)
{
	if (!traceEnabled()) return;
	llvm::errs() << "[entry-alloca] " << msg << "\n";
	llvm::errs().flush();
}

bool EntryAlloca::runOnModule(Module& M)
{
	bool changed = false;
	int myInvocation = ++sEntryAllocaInvocation;
	// Only lower vector ops in the second (or later) invocation.
	// The first invocation runs before heavy optimization passes; expanding
	// vectors to i128 arithmetic here would make instcombine very slow on
	// large functions like __gdtoa.
	bool doVectorLower = (myInvocation >= 2);

	// When tracing, dump IR for specific functions for debugging.
	if (traceEnabled())
	{
		for (Function& f : M)
		{
			const char* dumps[] = {
				"__pformat_cvt", "__freedtoa", "__gdtoa", nullptr
			};
			for (int i = 0; dumps[i]; ++i)
			{
				if (f.getName() == dumps[i])
				{
					llvm::errs() << "\n=== [entry-alloca] IR dump: "
						<< f.getName() << " ===\n";
					f.print(llvm::errs(), nullptr);
					llvm::errs() << "=== end IR dump: "
						<< f.getName() << " ===\n";
					llvm::errs().flush();
					break;
				}
			}
		}
	}

	// -----------------------------------------------------------------------
	// Pass 1: Hoist constant-size allocas from non-entry blocks to the
	//         function's entry block.
	// -----------------------------------------------------------------------
	for (Function& f : M)
	{
		if (f.empty())
		{
			continue;
		}

		BasicBlock& entry = f.getEntryBlock();
		Instruction* insertPt = &entry.front();

		// Collect non-entry constant-size allocas.
		std::vector<AllocaInst*> toHoist;
		for (auto& bb : f)
		{
			if (&bb == &entry)
			{
				continue;
			}
			for (auto& instr : bb)
			{
				if (auto* a = dyn_cast<AllocaInst>(&instr))
				{
					if (isa<ConstantInt>(a->getArraySize()))
					{
						toHoist.push_back(a);
					}
				}
			}
		}

		if (!toHoist.empty())
		{
			eaTrace("in " + f.getName().str() + ": hoisting "
				+ std::to_string(toHoist.size()) + " non-entry alloca(s)");
		}
		for (auto* a : toHoist)
		{
			eaTrace("  hoist: " + (a->hasName() ? a->getName().str() : "<unnamed>")
				+ " from block: " + a->getParent()->getName().str());
			a->moveBefore(insertPt);
			changed = true;
		}

		// Also ensure allocas already in the entry block come before any use
		// that is also in the entry block (to handle within-entry ordering).
		std::vector<AllocaInst*> entryAllocas;
		for (auto& instr : entry)
		{
			if (auto* a = dyn_cast<AllocaInst>(&instr))
			{
				if (isa<ConstantInt>(a->getArraySize()))
				{
					entryAllocas.push_back(a);
				}
			}
		}
		for (auto* a : entryAllocas)
		{
			Instruction* earliest = nullptr;
			for (User* u : a->users())
			{
				auto* ui = dyn_cast<Instruction>(u);
				if (ui == nullptr || ui->getParent() != &entry)
				{
					continue;
				}
				bool uiBefore = false;
				for (Instruction& bi : entry)
				{
					if (&bi == ui)  { uiBefore = true; break; }
					if (&bi == a)   { break; }
				}
				if (!uiBefore) continue;
				if (earliest == nullptr)
				{
					earliest = ui;
				}
				else
				{
					for (Instruction& bi : entry)
					{
						if (&bi == ui)      { earliest = ui; break; }
						if (&bi == earliest) { break; }
					}
				}
			}
			if (earliest != nullptr)
			{
				eaTrace("reorder entry-alloca before use: "
					+ (a->hasName() ? a->getName().str() : "<unnamed>")
					+ " in " + f.getName().str());
				a->moveBefore(earliest);
				changed = true;
			}
		}
	}

	// -----------------------------------------------------------------------
	// Pass 2: Fix cross-function alloca references.
	//
	// Some instructions in function F may use an AllocaInst defined in a
	// different function G.  Create a new alloca of the right type in F's
	// entry block and replace the illegal reference.
	//
	// We keep a per-function cache so we only create one replacement alloca
	// per (function, original-alloca) pair.
	// -----------------------------------------------------------------------
	for (Function& f : M)
	{
		if (f.empty())
		{
			continue;
		}

		BasicBlock& entry = f.getEntryBlock();
		Instruction* insertPt = &entry.front();

		// Map from foreign AllocaInst* -> local replacement AllocaInst*
		std::map<AllocaInst*, AllocaInst*> replacements;

		// Collect all instructions that have a foreign alloca as an operand.
		// We need to do this in two steps (collect then fix) to avoid
		// invalidating iterators while we insert new allocas.
		struct FixEntry { Instruction* inst; unsigned opIdx; AllocaInst* foreign; };
		std::vector<FixEntry> toFix;

		for (inst_iterator it = inst_begin(f), e = inst_end(f); it != e; ++it)
		{
			Instruction* inst = &*it;
			for (unsigned i = 0, n = inst->getNumOperands(); i < n; ++i)
			{
				auto* a = dyn_cast<AllocaInst>(inst->getOperand(i));
				if (a == nullptr)
				{
					continue;
				}
				// Is the alloca in a DIFFERENT function?
				if (a->getFunction() == &f)
				{
					continue;
				}
				toFix.push_back({inst, i, a});
			}
		}

		if (toFix.empty())
		{
			continue;
		}

		eaTrace("in " + f.getName().str() + ": fixing "
			+ std::to_string(toFix.size()) + " cross-function alloca ref(s)");

		for (auto& fe : toFix)
		{
			AllocaInst* foreign = fe.foreign;
			AllocaInst*& local  = replacements[foreign];

			if (local == nullptr)
			{
				// Create a replacement alloca of the same allocated type.
				local = new AllocaInst(
					foreign->getAllocatedType(),
					foreign->getType()->getAddressSpace(),
					foreign->hasName()
						? (foreign->getName().str() + ".local")
						: "foreign_alloca_local",
					insertPt);

				eaTrace("  created replacement for "
					+ (foreign->hasName() ? foreign->getName().str() : "<unnamed>")
					+ " (from " + foreign->getFunction()->getName().str() + ")"
					+ " -> " + local->getName().str());
				changed = true;
			}

			fe.inst->setOperand(fe.opIdx, local);
		}
	}

	// -----------------------------------------------------------------------
	// Pass 3: Fix all load/store type mismatches.
	//
	// When the value type and pointer's pointee type disagree we have two
	// strategies depending on whether the types are integer-compatible:
	//
	//  a) Integer size mismatch  (e.g. store i128 %v, i64* %p)
	//     Truncate or zero-extend the value so it matches the pointer's
	//     pointee type.  This avoids widening types that llvmir2hll can't
	//     represent (e.g. i128).  For loads we extend the loaded value
	//     after it has been loaded.
	//
	//  b) Pointer-compatible mismatch  (e.g. store i32 %v, i64* %p  on a
	//     64-bit target where the element sizes differ but both types are
	//     pointer-sized)  -> same integer handling as above.
	//
	//  c) Pointer type to pointer type mismatch  (e.g. load i32*, i64** %p)
	//     Insert a pointer bitcast on the pointer operand.
	// -----------------------------------------------------------------------
	for (Function& f : M)
	{
		auto& ctx = M.getContext();

		// Collect mismatches before iterating to avoid iterator invalidation.
		struct StoreFix   { StoreInst* s; };
		struct LoadFix    { LoadInst*  l; };
		struct PtrBitcast { Instruction* inst; unsigned opIdx; Type* newPtrTy; };

		std::vector<StoreFix>   storeFixes;
		std::vector<LoadFix>    loadFixes;
		std::vector<PtrBitcast> ptrFixes;

		for (inst_iterator it = inst_begin(f), e = inst_end(f); it != e; ++it)
		{
			if (auto* s = dyn_cast<StoreInst>(&*it))
			{
				Type* valTy = s->getValueOperand()->getType();
				Type* pteeTy = s->getPointerOperand()
					->getType()->getPointerElementType();
				if (valTy != pteeTy)
				{
					if (valTy->isIntegerTy() && pteeTy->isIntegerTy())
					{
						storeFixes.push_back({s});
					}
					else
					{
						Type* newPtrTy = PointerType::get(
							valTy, s->getPointerAddressSpace());
						ptrFixes.push_back({s, 1, newPtrTy});
					}
				}
			}
			else if (auto* l = dyn_cast<LoadInst>(&*it))
			{
				Type* loadTy = l->getType();
				Type* pteeTy = l->getPointerOperand()
					->getType()->getPointerElementType();
				if (loadTy != pteeTy)
				{
					if (loadTy->isIntegerTy() && pteeTy->isIntegerTy())
					{
						loadFixes.push_back({l});
					}
					else
					{
						Type* newPtrTy = PointerType::get(
							loadTy, l->getPointerAddressSpace());
						ptrFixes.push_back({l, 0, newPtrTy});
					}
				}
			}
		}

		// Fix stores with integer type mismatch: truncate or zero-extend the
		// stored value to match the alloca's element type.
		for (auto& sf : storeFixes)
		{
			StoreInst* s     = sf.s;
			Type* valTy  = s->getValueOperand()->getType();
			Type* pteeTy = s->getPointerOperand()
				->getType()->getPointerElementType();
			unsigned valBits  = valTy->getIntegerBitWidth();
			unsigned pteeBits = pteeTy->getIntegerBitWidth();
			Value* newVal = s->getValueOperand();
			if (valBits > pteeBits)
			{
				newVal = new TruncInst(newVal,
					IntegerType::get(ctx, pteeBits), "", s);
			}
			else
			{
				newVal = new ZExtInst(newVal,
					IntegerType::get(ctx, pteeBits), "", s);
			}
			s->setOperand(0, newVal);
			changed = true;
		}

		// Fix loads with integer type mismatch: load the alloca's element
		// type and then trunc/zext to the original load type.
		for (auto& lf : loadFixes)
		{
			LoadInst* l     = lf.l;
			Type* loadTy = l->getType();
			Type* pteeTy = l->getPointerOperand()
				->getType()->getPointerElementType();
			unsigned loadBits = loadTy->getIntegerBitWidth();
			unsigned pteeBits = pteeTy->getIntegerBitWidth();

			// Create a new load with the pointer's native type.
			auto* newLoad = new LoadInst(l->getPointerOperand(), "", l);
			Value* conv = nullptr;
			if (pteeBits > loadBits)
			{
				conv = new TruncInst(newLoad,
					IntegerType::get(ctx, loadBits), "", l);
			}
			else
			{
				conv = new ZExtInst(newLoad,
					IntegerType::get(ctx, loadBits), "", l);
			}
			l->replaceAllUsesWith(conv);
			l->eraseFromParent();
			changed = true;
		}

		// Fix pointer type mismatches with a bitcast on the pointer operand.
		for (auto& pf : ptrFixes)
		{
			Value* ptr = pf.inst->getOperand(pf.opIdx);
			auto*  cast = CastInst::CreatePointerBitCastOrAddrSpaceCast(
				ptr, pf.newPtrTy, "", pf.inst);
			pf.inst->setOperand(pf.opIdx, cast);
			changed = true;
		}
	}

	// -----------------------------------------------------------------------
	// Pass 4: Fix PHI nodes where incoming values have different type than
	//         the PHI.  After Pass 3's load/store fixes, some values may have
	//         been truncated (e.g. i64 -> i32).  If those feed into a PHI that
	//         expects the original type, later passes (simplifycfg, phi-to-
	//         select, retdec-remove-phi) will create invalid IR.  Coerce
	//         incoming values to match the PHI type.
	// -----------------------------------------------------------------------
	for (Function& f : M)
	{
		if (f.empty()) continue;

		for (BasicBlock& bb : f)
		{
			for (Instruction& inst : bb)
			{
				auto* phi = dyn_cast<PHINode>(&inst);
				if (!phi) break;  // PHIs are at block start only.

				Type* phiTy = phi->getType();
				for (unsigned i = 0, e = phi->getNumIncomingValues(); i < e; ++i)
				{
					Value* inc = phi->getIncomingValue(i);
					if (inc->getType() == phiTy) continue;

					Instruction* insertBefore = phi->getIncomingBlock(i)
						->getTerminator();
					Value* casted = nullptr;
					if (phiTy->isIntegerTy() && inc->getType()->isIntegerTy())
					{
						casted = CastInst::CreateIntegerCast(
							inc, phiTy, false, "", insertBefore);
					}
					else if (phiTy->isPointerTy() && inc->getType()->isPointerTy())
					{
						casted = CastInst::CreatePointerBitCastOrAddrSpaceCast(
							inc, phiTy, "", insertBefore);
					}
					if (casted)
					{
						phi->setIncomingValue(i, casted);
						changed = true;
					}
				}
			}
		}
	}

	// -----------------------------------------------------------------------
	// Pass 5: Fix SelectInst where operand types don't match result type.
	//         LLVM's simplifycfg or other passes can create selects from PHIs
	//         without coercing truncated values, producing invalid IR.
	// -----------------------------------------------------------------------
	for (Function& f : M)
	{
		if (f.empty()) continue;

		for (inst_iterator it = inst_begin(f), e = inst_end(f); it != e; ++it)
		{
			auto* sel = dyn_cast<SelectInst>(&*it);
			if (!sel) continue;

			Type* resTy = sel->getType();
			Value* tVal = sel->getTrueValue();
			Value* fVal = sel->getFalseValue();
			if (tVal->getType() == resTy && fVal->getType() == resTy)
				continue;

			IRBuilder<> irb(sel);
			Value* newT = tVal;
			Value* newF = fVal;
			if (tVal->getType() != resTy && resTy->isIntegerTy() &&
			    tVal->getType()->isIntegerTy())
				newT = irb.CreateIntCast(tVal, resTy, false);
			if (fVal->getType() != resTy && resTy->isIntegerTy() &&
			    fVal->getType()->isIntegerTy())
				newF = irb.CreateIntCast(fVal, resTy, false);
			if (tVal->getType() != resTy && resTy->isPointerTy() &&
			    tVal->getType()->isPointerTy())
				newT = irb.CreatePointerCast(tVal, resTy);
			if (fVal->getType() != resTy && resTy->isPointerTy() &&
			    fVal->getType()->isPointerTy())
				newF = irb.CreatePointerCast(fVal, resTy);

			if (newT != tVal || newF != fVal)
			{
				sel->setOperand(1, newT);
				sel->setOperand(2, newF);
				changed = true;
			}
		}
	}

	// Note: A previous Pass 5 (now Pass 6) attempted to lower LLVM vector operations
	// (bitcast <vec> to iN, shufflevector, insertelement, extractelement)
	// to scalar arithmetic before llvmir2hll saw them.  This was removed
	// because the IR manipulation caused use-after-free and corruption when
	// combined with the surrounding LLVM pass pipeline.
	//
	// Instead, llvmir2hll/llvm/llvmir2bir_converter/llvm_instruction_converter.cpp
	// and llvm_type_converter.cpp have been updated with safe fallback handlers
	// for all vector instruction types, so llvmir2hll can survive the presence
	// of vector IR without crashing.  The decompiled output for vector-heavy
	// functions will be semantically approximate (e.g., shuffle results may
	// appear as integer casts of the first element) but the decompiler will
	// not crash.
	(void)doVectorLower; // suppress unused-variable warning

	// Post-pass IR dump for tracing.
	if (traceEnabled())
	{
		for (Function& f : M)
		{
			const char* dumps[] = { "__freedtoa", "__gdtoa", nullptr };
			for (int i = 0; dumps[i]; ++i)
			{
				if (f.getName() == dumps[i])
				{
					llvm::errs() << "\n=== [entry-alloca POST] IR dump: "
						<< f.getName() << " ===\n";
					f.print(llvm::errs(), nullptr);
					llvm::errs() << "=== end POST IR dump: "
						<< f.getName() << " ===\n";
					llvm::errs().flush();
					break;
				}
			}
		}
	}

	return changed;
}

} // namespace bin2llvmir
} // namespace retdec
