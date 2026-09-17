/**
* @file scripts/ci/idiom_shared_use_check.cpp
* @brief IDIOM-USE-01 -- does an idiom rewrite corrupt a use it did not match?
* @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
*
* An idiom rewrite matches a small tree of instructions and replaces it. The
* tree's inner nodes then have to go, and IdiomsAbstract::eraseInstFromBasicBlock
* did that with `replaceAllUsesWith(UndefValue)` followed by an erase.
*
* That is correct exactly when the idiom's own root was the inner node's only
* reader. When it was not, every other reader silently starts reading `undef`:
*
*     %y = lshr i32 %x, 31
*     %z = xor  i32 %y, 1     <- ((X u>> 31) ^ 1), rewritten to X >= 0
*     %w = add  i32 %y, 5     <- nothing to do with the idiom
*
* becomes `%w = add i32 undef, 5`. The pass reports success, the IR verifies,
* and the decompiled C is wrong. 129 call sites do this.
*
* So this does not check the shape of the rewrite. It builds the function
* twice, runs the pass on one, and evaluates both over a domain of inputs with
* LLVM's own constant folder, requiring the same answer. `undef` reached by the
* rewrite but not by the original is reported as its own failure rather than
* folded away, because that is the defect this exists for.
*
* Each case also reports whether the pass changed anything at all: a case the
* pass declines compares two identical functions and would pass while measuring
* nothing. --self-test adds a function nothing rewrites and requires it to be
* reported as inert rather than counted as a pass.
*
* WHAT IT CANNOT SEE
*
* It covers the four shapes below, in one basic block, over thirteen inputs.
* Reverting the fix fails exactly one of them -- the other two shared-node
* cases are rewritten without leaving a live undef in this construction -- so
* this demonstrates the class rather than enumerating the 129 call sites.
*/
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "retdec/bin2llvmir/optimizations/idioms/idioms_analysis.h"

using namespace llvm;
using namespace retdec::bin2llvmir;
// A single-basic-block evaluator: run the function for a concrete argument by
// constant-folding every instruction in order.  Returns false when it meets
// anything it cannot model -- including undef, which is the point: a rewrite
// that leaves an unrelated use reading undef is exactly what this must not
// quietly accept.
static bool evalFn(Function* f, int64_t x, int64_t& out, bool& sawUndef)
{
	BasicBlock& bb = f->getEntryBlock();
	DenseMap<Value*, Constant*> env;
	Type* argTy = f->getArg(0)->getType();
	env[f->getArg(0)] = ConstantInt::get(argTy, x, true);
	const DataLayout& dl = f->getParent()->getDataLayout();

	auto resolve = [&](Value* v) -> Constant* {
		if (auto* c = dyn_cast<Constant>(v)) {
			if (isa<UndefValue>(c) || isa<PoisonValue>(c)) { sawUndef = true; return nullptr; }
			return c;
		}
		auto it = env.find(v);
		return it == env.end() ? nullptr : it->second;
	};

	for (Instruction& i : bb) {
		if (auto* ret = dyn_cast<ReturnInst>(&i)) {
			Constant* rv = ret->getReturnValue() ? resolve(ret->getReturnValue()) : nullptr;
			if (!rv) return false;
			auto* ci = dyn_cast<ConstantInt>(rv);
			if (!ci) return false;
			out = ci->getValue().getSExtValue();
			return true;
		}
		SmallVector<Constant*, 4> ops;
		bool ok = true;
		for (Use& u : i.operands()) {
			Constant* c = resolve(u.get());
			if (!c) { ok = false; break; }
			ops.push_back(c);
		}
		if (!ok) return false;
		Constant* folded = ConstantFoldInstOperands(&i, ops, dl);
		if (!folded) return false;
		env[&i] = folded;
	}
	return false;
}

static std::string dump(Function* f) {
	std::string s; raw_string_ostream os(s); f->print(os); return os.str();
}

static const std::vector<int64_t> DOMAIN = {
	-2147483648LL, -70000, -1024, -31, -2, -1, 0, 1, 2, 31, 1024, 70000, 2147483647LL
};

// Build the same function twice, run the pass on one, and require the two to
// agree for every input.
static bool check(const char* what, LLVMContext& ctx, Module* mod,
		const std::function<void(IRBuilder<>&, Function*)>& build)
{
	Type* i32 = Type::getInt32Ty(ctx);
	FunctionType* ft = FunctionType::get(i32, {i32}, false);

	Function* before = Function::Create(ft, Function::ExternalLinkage,
		std::string(what) + ".before", mod);
	{ IRBuilder<> b(BasicBlock::Create(ctx, "e", before)); build(b, before); }

	Function* after = Function::Create(ft, Function::ExternalLinkage,
		std::string(what) + ".after", mod);
	{ IRBuilder<> b(BasicBlock::Create(ctx, "e", after)); build(b, after); }

	IdiomsAnalysis ia(mod, CC_ANY, ARCH_ANY);
	ia.doAnalysis(*after, nullptr);

	// Compare the BODY, not the dump: the dump opens with `define ... @name`,
	// and the two functions necessarily have different names, so comparing
	// dumps reports every function as rewritten.  The self-test caught this by
	// reporting a function nothing touches as rewritten.
	auto body = [](const std::string& d) {
		auto nl = d.find('\n');
		return nl == std::string::npos ? d : d.substr(nl);
	};
	bool changed = body(dump(before)) != body(dump(after));
	bool ok = true;
	for (int64_t x : DOMAIN) {
		int64_t a = 0, c = 0;
		bool ua = false, uc = false;
		bool ra = evalFn(before, x, a, ua);
		bool rc = evalFn(after, x, c, uc);
		if (uc && !ua) {
			printf("   %s: x=%lld -- the rewrite left something reading undef\n",
				what, (long long)x);
			ok = false; break;
		}
		if (!ra || !rc) {
			printf("   %s: x=%lld -- the evaluator could not model %s\n",
				what, (long long)x, ra ? "the rewrite" : "the input");
			ok = false; break;
		}
		if (a != c) {
			printf("   %s: x=%lld -- before gives %lld, after gives %lld\n",
				what, (long long)x, (long long)a, (long long)c);
			ok = false; break;
		}
	}
	printf("%-28s %s  (%s)\n", what, ok ? "ok " : "FAIL",
		changed ? "rewritten" : "unchanged -- measured nothing");
	if (!ok) printf("before:\n%s\nafter:\n%s\n", dump(before).c_str(), dump(after).c_str());
	return ok && changed;
}

int main(int argc, char** argv) {
	LLVMContext ctx;
	auto mod = std::make_unique<Module>("usesem", ctx);
	Type* i32 = Type::getInt32Ty(ctx);
	auto C = [&](int64_t v) { return ConstantInt::get(i32, v); };
	int bad = 0;

	// ((X u>> 31) ^ 1), with the lshr read by nothing else.
	if (!check("ge_zero/private", ctx, mod.get(), [&](IRBuilder<>& b, Function* f) {
		Value* y = b.CreateLShr(f->getArg(0), C(31), "y");
		b.CreateRet(b.CreateXor(y, C(1), "z"));
	})) bad = 1;

	// The same, with the lshr also read by an unrelated add.  This is the case
	// that produced `add i32 undef, 5`.
	if (!check("ge_zero/shared", ctx, mod.get(), [&](IRBuilder<>& b, Function* f) {
		Value* y = b.CreateLShr(f->getArg(0), C(31), "y");
		b.CreateXor(y, C(1), "z");
		b.CreateRet(b.CreateAdd(y, C(5), "w"));
	})) bad = 1;

	// ((X ^ -1) u>> 31), shared.
	if (!check("ge_zero_xor/shared", ctx, mod.get(), [&](IRBuilder<>& b, Function* f) {
		Value* n = b.CreateXor(f->getArg(0), C(-1), "n");
		b.CreateLShr(n, C(31), "s");
		b.CreateRet(b.CreateAdd(n, C(7), "w"));
	})) bad = 1;

	// (X s>> 31) & C  -- the signed-modulo shape, shared.
	if (!check("smod/shared", ctx, mod.get(), [&](IRBuilder<>& b, Function* f) {
		Value* a = b.CreateAShr(f->getArg(0), C(31), "a");
		Value* m = b.CreateAnd(a, C(7), "m");
		Value* s = b.CreateAdd(f->getArg(0), m, "s");
		Value* d = b.CreateAnd(s, C(-8), "d");
		b.CreateSub(f->getArg(0), d, "r");
		b.CreateRet(b.CreateAdd(a, C(3), "w"));
	})) bad = 1;

	if (argc > 1 && std::string(argv[1]) == "--self-test") {
		// The differential has to be able to fail: a function nothing rewrites
		// must report "measured nothing" rather than a quiet pass.
		bool inert = check("self-test/inert", ctx, mod.get(),
			[&](IRBuilder<>& b, Function* f) {
				b.CreateRet(b.CreateAdd(f->getArg(0), C(3), "w"));
			});
		if (inert) {
			printf("self-test FAILED: an unrewritten function reported a result\n");
			bad = 1;
		} else {
			printf("self-test ok: an unrewritten function is reported, not passed\n");
		}
	}
	return bad;
}
