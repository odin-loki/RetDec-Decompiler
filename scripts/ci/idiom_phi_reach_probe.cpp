/**
* @file scripts/ci/idiom_phi_reach_probe.cpp
* @brief IDIOM-PHI-01 -- can any per-basic-block idiom exchanger fire on a PHI?
* @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
*
* IdiomsAnalysis::analyse(BasicBlock&, exchanger) carries a fix-up for the case
* "an exchanger replaced a PHI with something that is not a PHI", because the
* replacement cannot sit where the PHI was.  That fix-up used to reassign the
* iterator it then erased through, so had it ever fired it would have erased
* the first non-PHI instruction -- a live one, with uses -- instead of the PHI.
*
* Whether that mattered is not a question about the code, it is a question
* about the 39 exchangers the dispatcher registers: does any of them return
* non-null for a PHI input?  This measures it rather than reasoning about it.
* It builds a PHI of each of seven types at the top of a loop -- the shape a
* lifted loop really produces -- calls every registered exchanger on it, and
* counts how many fire.
*
* The count is compared against the number written in the comment at the fix-up
* in idioms_analysis.cpp.  A claim in a comment about what no exchanger does is
* exactly the kind of claim that goes quietly stale when someone adds the
* fortieth exchanger, and this is what stops that.
*
* A negative control runs last: the same exchanger that saw nothing in a PHI
* must fire on `lshr x, 3`.  Without it, "0 fired" could equally mean the probe
* never really called anything, which is a failure mode this audit has hit
* often enough to check for by default.
*/
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DerivedTypes.h>
#include <cstdio>
#include <vector>
#include <string>
#include "retdec/bin2llvmir/optimizations/idioms/idioms_analysis.h"

using namespace llvm;
using namespace retdec::bin2llvmir;

struct Probe: public IdiomsAnalysis {
	Probe(Module* m): IdiomsAnalysis(m, CC_ANY, ARCH_ANY) {}

	int run(LLVMContext& ctx, Module* mod) {
		// Scalars, the float types, an integer vector and a pointer: every
		// shape a lifted PHI can have.  Leaving the last two out would make
		// the answer partly a reading rather than a measurement.
		std::vector<Type*> types = {
			Type::getInt1Ty(ctx), Type::getInt8Ty(ctx), Type::getInt16Ty(ctx),
			Type::getInt32Ty(ctx), Type::getInt64Ty(ctx),
			Type::getFloatTy(ctx), Type::getDoubleTy(ctx),
			FixedVectorType::get(Type::getInt32Ty(ctx), 4),
			PointerType::get(ctx, 0),
		};
		unsigned probed = 0, fired = 0;

		for (Type* t : types) {
			FunctionType* ft = FunctionType::get(Type::getVoidTy(ctx), {t}, false);
			Function* f = Function::Create(ft, Function::ExternalLinkage, "f", mod);
			BasicBlock* entry = BasicBlock::Create(ctx, "entry", f);
			BasicBlock* loop = BasicBlock::Create(ctx, "loop", f);
			IRBuilder<> b(entry);
			b.CreateBr(loop);
			b.SetInsertPoint(loop);
			PHINode* phi = b.CreatePHI(t, 2, "p");
			phi->addIncoming(f->getArg(0), entry);
			Value* use;
			if (t->isPointerTy()) {
				use = b.CreateGEP(Type::getInt8Ty(ctx), phi,
					ConstantInt::get(Type::getInt64Ty(ctx), 1));
			} else if (t->isFPOrFPVectorTy()) {
				use = b.CreateFAdd(phi, ConstantFP::get(t, 1.0));
			} else {
				use = b.CreateAdd(phi, ConstantInt::get(t, 1));
			}
			phi->addIncoming(use, loop);
			b.CreateBr(loop);

			BasicBlock::iterator it = loop->begin();   // the PHI
			auto note = [&](const char* n, Instruction* r) {
				++probed;
				if (r) {
					++fired;
					printf("FIRES: %s returned %s on a PHI\n", n, r->getOpcodeName());
				}
			};
#define P(f) note(#f, f(it))
			P(exchangeBitShiftUDiv);     P(exchangeLessThanZero);
			P(exchangeGreaterEqualZero); P(exchangeBitShiftSDiv1);
			P(exchangeBitShiftMul);      P(exchangeUnsignedModulo2n);
			P(exchangeDivByMinusTwo);    P(exchangeSignedModulo2n);
			P(exchangeFloatNeg);         P(exchangeXorMinusOne);
			P(exchangeSignedModuloByTwo);P(exchangeCondBitShiftDiv1);
			P(exchangeCondBitShiftDiv2); P(exchangeCondBitShiftDiv3);
			P(exchangeFloatAbs);         P(exchangeCopysign);
			P(exchangeIsGreaterThanMinusOne);
			P(exchangeCompareEq);        P(exchangeCompareSlt);
			P(exchangeCompareSle);       P(exchangeCompareNeq);
			P(exchangeIntegerAbs);
			P(exchangeAndZeroAssign);    P(exchangeOrMinusOneAssign);
			P(magicUnsignedDiv1);        P(magicUnsignedDiv2);
			P(magicSignedDiv1);          P(magicSignedDiv2);
			P(magicSignedDiv3);          P(magicSignedDiv4);
			P(magicSignedDiv5);          P(magicSignedDiv6);
			P(magicSignedDiv7neg);       P(magicSignedDiv7pos);
			P(magicSignedDiv8neg);       P(magicSignedDiv8pos);
			P(signedMod1);               P(signedMod2);
			P(unsignedMod);
#undef P
		}
		printf("probed %u exchanger/type pairs; %u fired on a PHI\n", probed, fired);

		// Negative control: without this, "0 fired" could mean the probe never
		// really called anything.
		FunctionType* ft = FunctionType::get(Type::getVoidTy(ctx),
			{Type::getInt32Ty(ctx)}, false);
		Function* g = Function::Create(ft, Function::ExternalLinkage, "g", mod);
		BasicBlock* bb = BasicBlock::Create(ctx, "e", g);
		IRBuilder<> b(bb);
		Value* sh = b.CreateLShr(g->getArg(0),
			ConstantInt::get(Type::getInt32Ty(ctx), 3));
		b.CreateRetVoid();
		BasicBlock::iterator si = cast<Instruction>(sh)->getIterator();
		Instruction* r = exchangeBitShiftUDiv(si);
		printf("negative control (lshr x, 3): exchangeBitShiftUDiv %s\n",
			r ? "fired" : "DID NOT FIRE -- this probe proves nothing");
		if (!r) return 2;
		return fired ? 1 : 0;
	}
};

int main() {
	LLVMContext ctx;
	auto mod = std::make_unique<Module>("phi_probe", ctx);
	Probe probe(mod.get());
	return probe.run(ctx, mod.get());
}
