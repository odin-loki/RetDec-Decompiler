/**
 * @file scripts/ci/inst_opt_semantics_check.cpp
 * @brief Fails when a bin2llvmir peephole changes what a function computes.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of OPT-01. Run from scripts/ci/check_bin2llvmir_opts.sh.
 *
 * WHY THIS EXISTS
 *
 * The inst_opt tests check the shape of the IR: they parse a module, run one
 * peephole, and compare the result against expected IR text. That answers
 * "did the rewrite fire and produce the instruction I wrote down". It does not
 * answer "do the two programs compute the same number", and those are
 * different questions. `and i1 x, y` -> `icmp eq i1 x, y` passes a text
 * comparison and is wrong for x = y = 0. The test that covers it picks y = 1,
 * where the two happen to agree, so it has never been able to fail.
 *
 * This checker does not look at the IR text at all. It takes the module
 * before the rewrite and the module after it, evaluates BOTH over a domain of
 * inputs, and requires the same answer every time. The evaluator is LLVM's own
 * constant folder, which knows nothing about RetDec, so a rewrite cannot talk
 * it into agreeing.
 *
 * Shift amounts, divisors and casts are where this bites, so the domain
 * deliberately includes the values a hand-written test would not think to try:
 * zero, one, INT_MIN, all-ones, and for floating point the doubles that do not
 * survive a round trip through float.
 *
 * WHAT IT CANNOT SEE
 *
 * Say this plainly, because an instrument whose blind spots are undocumented
 * gets trusted for things it never measured.
 *
 * - Poison. ConstantFoldInstOperands ignores nsw, nuw and exact and hands back
 *   the wrapped value, so a rewrite that turns a defined result into poison --
 *   reassociating two `add nsw` into one, say -- compares equal here. Flag
 *   handling is checked by shape, in the gtest suite, not by this.
 * - Anything past one basic block. evaluateFunction refuses a function with
 *   more than one, so a rewrite that depends on control flow is out of scope.
 * - Partial overwrites. Memory is one Constant per location, not bytes, so a
 *   four-byte store over an eight-byte one cannot be expressed. A case that
 *   needs it is caught by shape in the gtest suite instead.
 * - Aliasing beyond stripPointerCasts. Locations are compared after stripping
 *   bitcasts and zero-index GEPs, which covers the way lifted code names one
 *   address twice. Two addresses that are equal only after arithmetic are
 *   still two locations here.
 */

#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt.h"
#include "retdec/bin2llvmir/optimizations/strength_reduction/strength_reduction.h"
#include "retdec/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.h"

using namespace llvm;

namespace {

// ── the evaluator ───────────────────────────────────────────────────────────
//
// A function here is a single basic block of arithmetic over its arguments,
// plus loads and stores to module globals. Everything except memory is handed
// to ConstantFoldInstOperands; memory is a map from pointer to current value,
// seeded from each global's initialiser. A `nullptr` result means the fold
// gave up, which is reported rather than silently skipped -- an evaluator that
// quietly answers "don't know" is an instrument that cannot fail.

struct EvalResult
{
	Constant* value = nullptr;
	const char* giveUp = nullptr;   // non-null: could not evaluate, and why
};

EvalResult evaluateFunction(Function& f, ArrayRef<Constant*> args)
{
	const DataLayout& dl = f.getParent()->getDataLayout();

	std::map<const Value*, Constant*> env;
	std::map<const Value*, Constant*> mem;

	unsigned ai = 0;
	for (Argument& a : f.args())
	{
		if (ai >= args.size())
		{
			return {nullptr, "not enough inputs for the function's arguments"};
		}
		env[&a] = args[ai++];
	}
	for (GlobalVariable& g : f.getParent()->globals())
	{
		if (g.hasInitializer())
		{
			mem[&g] = g.getInitializer();
		}
	}

	if (f.size() != 1)
	{
		return {nullptr, "function is not a single basic block"};
	}

	// Two Value* can name one address: a global and a zero-index GEP of it, a
	// pointer and a bitcast of it. Keying memory on the raw Value* would make
	// this evaluator share exactly the assumption that makes a load/store pass
	// wrong, and an instrument that repeats the defect cannot detect it.
	auto location = [](Value* p) -> const Value*
	{
		return p->stripPointerCasts();
	};

	auto resolve = [&](Value* v) -> Constant*
	{
		if (auto* c = dyn_cast<Constant>(v))
		{
			// A global is its own address, not its contents.
			return c;
		}
		auto it = env.find(v);
		return it == env.end() ? nullptr : it->second;
	};

	for (Instruction& i : f.front())
	{
		if (auto* ret = dyn_cast<ReturnInst>(&i))
		{
			if (ret->getNumOperands() == 0)
			{
				return {nullptr, "function returns void, so there is nothing to compare"};
			}
			Constant* v = resolve(ret->getOperand(0));
			if (v == nullptr)
			{
				return {nullptr, "the returned value did not evaluate"};
			}
			return {v, nullptr};
		}
		if (auto* st = dyn_cast<StoreInst>(&i))
		{
			Constant* v = resolve(st->getValueOperand());
			if (v == nullptr)
			{
				return {nullptr, "a stored value did not evaluate"};
			}
			mem[location(st->getPointerOperand())] = v;
			continue;
		}
		if (auto* ld = dyn_cast<LoadInst>(&i))
		{
			auto it = mem.find(location(ld->getPointerOperand()));
			if (it == mem.end())
			{
				return {nullptr, "a load read a location that was never written"};
			}
			if (it->second->getType() != ld->getType())
			{
				return {nullptr, "a load's type does not match what was stored"};
			}
			env[&i] = it->second;
			continue;
		}
		if (isa<AllocaInst>(&i))
		{
			continue;   // the pointer itself is only ever used as a map key
		}

		SmallVector<Constant*, 4> ops;
		bool ok = true;
		for (Use& u : i.operands())
		{
			Constant* c = resolve(u.get());
			if (c == nullptr)
			{
				ok = false;
				break;
			}
			ops.push_back(c);
		}
		if (!ok)
		{
			return {nullptr, "an operand did not evaluate"};
		}

		Constant* folded = ConstantFoldInstOperands(&i, ops, dl);
		if (folded == nullptr)
		{
			return {nullptr, "LLVM's constant folder declined an instruction"};
		}
		env[&i] = folded;
	}

	return {nullptr, "the block ran off the end without returning"};
}

// Two results agree only if they are bit-for-bit identical. For floating
// point that is deliberate: 0.0 and -0.0 compare equal as numbers and are
// different answers, and a NaN payload that changes is a changed answer too.
bool sameAnswer(Constant* a, Constant* b)
{
	if (a == nullptr || b == nullptr)
	{
		return a == b;
	}
	if (a->getType() != b->getType())
	{
		return false;
	}
	if (auto* ia = dyn_cast<ConstantInt>(a))
	{
		auto* ib = dyn_cast<ConstantInt>(b);
		return ib && ia->getValue() == ib->getValue();
	}
	if (auto* fa = dyn_cast<ConstantFP>(a))
	{
		auto* fb = dyn_cast<ConstantFP>(b);
		return fb && fa->getValueAPF().bitcastToAPInt()
				== fb->getValueAPF().bitcastToAPInt();
	}
	if (isa<UndefValue>(a) && isa<UndefValue>(b))
	{
		return true;
	}
	return a == b;
}

std::string render(Constant* c)
{
	if (c == nullptr)
	{
		return "<none>";
	}
	std::string s;
	raw_string_ostream os(s);
	c->printAsOperand(os, false);
	return os.str();
}

// ── the input domains ───────────────────────────────────────────────────────
//
// Chosen to contain what a test written by hand leaves out. Every entry below
// has caught something somewhere in this tree: 0 and 1 for divisors, INT_MIN
// for signed division and for shifts, all-ones for masks, and the three
// doubles at the end because they are exactly the values that do not survive
// a round trip through float.

std::vector<Constant*> domainFor(Type* t)
{
	std::vector<Constant*> out;
	if (auto* it = dyn_cast<IntegerType>(t))
	{
		unsigned w = it->getBitWidth();
		if (w == 1)
		{
			out.push_back(ConstantInt::get(it, 0));
			out.push_back(ConstantInt::get(it, 1));
			return out;
		}
		static const uint64_t seeds[] = {
			0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 33, 63, 64, 255, 256,
			0x5555555555555555ull, 0xaaaaaaaaaaaaaaaaull,
			0x0f0f0f0f0f0f0f0full, 0xf0f0f0f0f0f0f0f0ull,
			0x123456789abcdef0ull, 0xdeadbeefdeadbeefull,
		};
		for (uint64_t s : seeds)
		{
			// The seeds are written as 64-bit patterns; narrow (or widen) each
			// one to the type actually under test. APInt's constructor asserts
			// rather than truncating.
			out.push_back(ConstantInt::get(it, APInt(64, s, false).zextOrTrunc(w)));
		}
		out.push_back(ConstantInt::get(it, APInt::getSignedMinValue(w)));
		out.push_back(ConstantInt::get(it, APInt::getSignedMaxValue(w)));
		out.push_back(ConstantInt::get(it, APInt::getAllOnes(w)));
		return out;
	}
	if (t->isDoubleTy() || t->isFloatTy())
	{
		static const double seeds[] = {
			0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0,
			3.14159265358979311599796346854,
			1.0e300, 1.0e-300, 1.0e30, -1.0e30,
			16777217.0,              // first integer a float cannot hold
			1.0000000000000002,      // one ulp above 1.0 in double
			1.7976931348623157e308,  // DBL_MAX: overflows float
			4.9406564584124654e-324, // DBL_MIN denormal: underflows float
		};
		for (double d : seeds)
		{
			out.push_back(ConstantFP::get(t, d));
		}
		return out;
	}
	return out;
}

// ── the cases ───────────────────────────────────────────────────────────────

enum class eRun
{
	InstOptOnNamed,     // run inst_opt::optimize on the named instruction
	InstOptOnAll,       // run inst_opt::optimize over the whole function
	StrengthReduction,  // run the StrengthReduction module pass
	RedundantLoadStore, // run the RedundantLoadStoreElim module pass
};

struct Case
{
	const char* name;
	eRun how;
	const char* target;   // instruction name, for InstOptOnNamed
	const char* ir;
};

const Case CASES[] = {

// ── inst_opt ────────────────────────────────────────────────────────────────

{"and_i1", eRun::InstOptOnNamed, "r", R"(
	define i1 @fnc(i1 %x, i1 %y) {
		%r = and i1 %x, %y
		ret i1 %r
	})"},

{"xor_i1", eRun::InstOptOnNamed, "r", R"(
	define i1 @fnc(i1 %x, i1 %y) {
		%r = xor i1 %x, %y
		ret i1 %r
	})"},

{"or_i1", eRun::InstOptOnNamed, "r", R"(
	define i1 @fnc(i1 %x, i1 %y) {
		%r = or i1 %x, %y
		ret i1 %r
	})"},

{"addZero", eRun::InstOptOnNamed, "r", R"(
	define i32 @fnc(i32 %x) {
		%r = add i32 %x, 0
		ret i32 %r
	})"},

{"subZero", eRun::InstOptOnNamed, "r", R"(
	define i32 @fnc(i32 %x) {
		%r = sub i32 %x, 0
		ret i32 %r
	})"},

{"truncZext8", eRun::InstOptOnNamed, "r", R"(
	define i32 @fnc(i32 %x) {
		%a = trunc i32 %x to i8
		%r = zext i8 %a to i32
		ret i32 %r
	})"},

{"truncZext16", eRun::InstOptOnNamed, "r", R"(
	define i32 @fnc(i32 %x) {
		%a = trunc i32 %x to i16
		%r = zext i16 %a to i32
		ret i32 %r
	})"},

{"xorXX", eRun::InstOptOnNamed, "r", R"(
	define i32 @fnc(i32 %x) {
		%r = xor i32 %x, %x
		ret i32 %r
	})"},

{"orXX", eRun::InstOptOnNamed, "r", R"(
	define i32 @fnc(i32 %x) {
		%r = or i32 %x, %x
		ret i32 %r
	})"},

{"andXX", eRun::InstOptOnNamed, "r", R"(
	define i32 @fnc(i32 %x) {
		%r = and i32 %x, %x
		ret i32 %r
	})"},

{"addSequence", eRun::InstOptOnNamed, "r", R"(
	define i32 @fnc(i32 %x) {
		%a = add i32 %x, 100
		%r = add i32 %a, 200
		ret i32 %r
	})"},

{"addSequenceWraps", eRun::InstOptOnNamed, "r", R"(
	define i8 @fnc(i8 %x) {
		%a = add i8 %x, 100
		%r = add i8 %a, 100
		ret i8 %r
	})"},

// A double does not fit in a float. Narrowing and widening back is not the
// identity, so collapsing the pair is not a cast that can be removed.
{"fpRoundTripDouble", eRun::InstOptOnNamed, "r", R"(
	define double @fnc(double %x) {
		%a = fptrunc double %x to float
		%r = fpext float %a to double
		ret double %r
	})"},

// The same shape written as a chain, which castSequenceFinder walks.
{"fpRoundTripChain", eRun::InstOptOnNamed, "r", R"(
	define fp128 @fnc(double %x) {
		%a = fptrunc double %x to float
		%b = fpext float %a to double
		%r = fpext double %b to fp128
		ret fp128 %r
	})"},

// Widening then narrowing back IS the identity, so this one may collapse.
{"fpWidenNarrow", eRun::InstOptOnNamed, "r", R"(
	define float @fnc(float %x) {
		%a = fpext float %x to double
		%r = fptrunc double %a to float
		ret float %r
	})"},

// Two loads of one global with a store in between are two different values.
{"loadStoreLoadXor", eRun::InstOptOnNamed, "r", R"(
	@g = global i32 0
	define i32 @fnc(i32 %x) {
		store i32 %x, ptr @g
		%a = load i32, ptr @g
		store i32 7, ptr @g
		%b = load i32, ptr @g
		%r = xor i32 %a, %b
		ret i32 %r
	})"},

{"loadStoreLoadOr", eRun::InstOptOnNamed, "r", R"(
	@g = global i32 0
	define i32 @fnc(i32 %x) {
		store i32 %x, ptr @g
		%a = load i32, ptr @g
		store i32 7, ptr @g
		%b = load i32, ptr @g
		%r = or i32 %a, %b
		ret i32 %r
	})"},

{"loadStoreLoadAnd", eRun::InstOptOnNamed, "r", R"(
	@g = global i32 0
	define i32 @fnc(i32 %x) {
		store i32 %x, ptr @g
		%a = load i32, ptr @g
		store i32 7, ptr @g
		%b = load i32, ptr @g
		%r = and i32 %a, %b
		ret i32 %r
	})"},

// No store in between: folding these two IS sound, and must keep working.
{"loadLoadXorNoStore", eRun::InstOptOnNamed, "r", R"(
	@g = global i32 9
	define i32 @fnc(i32 %x) {
		%a = load i32, ptr @g
		%b = load i32, ptr @g
		%r = xor i32 %a, %b
		ret i32 %r
	})"},

// ── strength reduction ──────────────────────────────────────────────────────

{"srShlLshr4", eRun::StrengthReduction, nullptr, R"(
	define i32 @fnc(i32 %x) {
		%a = shl i32 %x, 4
		%r = lshr i32 %a, 4
		ret i32 %r
	})"},

{"srShlLshr1", eRun::StrengthReduction, nullptr, R"(
	define i32 @fnc(i32 %x) {
		%a = shl i32 %x, 1
		%r = lshr i32 %a, 1
		ret i32 %r
	})"},

{"srShlLshr3", eRun::StrengthReduction, nullptr, R"(
	define i32 @fnc(i32 %x) {
		%a = shl i32 %x, 3
		%r = lshr i32 %a, 3
		ret i32 %r
	})"},

{"srShlLshr16i64", eRun::StrengthReduction, nullptr, R"(
	define i64 @fnc(i64 %x) {
		%a = shl i64 %x, 16
		%r = lshr i64 %a, 16
		ret i64 %r
	})"},

{"srShlLshr7i8", eRun::StrengthReduction, nullptr, R"(
	define i8 @fnc(i8 %x) {
		%a = shl i8 %x, 7
		%r = lshr i8 %a, 7
		ret i8 %r
	})"},

{"srMulPow2", eRun::StrengthReduction, nullptr, R"(
	define i32 @fnc(i32 %x) {
		%r = mul i32 %x, 8
		ret i32 %r
	})"},

{"srMulNegPow2", eRun::StrengthReduction, nullptr, R"(
	define i32 @fnc(i32 %x) {
		%r = mul i32 %x, -8
		ret i32 %r
	})"},

{"srUDivPow2", eRun::StrengthReduction, nullptr, R"(
	define i32 @fnc(i32 %x) {
		%r = udiv i32 %x, 16
		ret i32 %r
	})"},

{"srURemPow2", eRun::StrengthReduction, nullptr, R"(
	define i32 @fnc(i32 %x) {
		%r = urem i32 %x, 16
		ret i32 %r
	})"},

{"srSelfSub", eRun::StrengthReduction, nullptr, R"(
	define i32 @fnc(i32 %x) {
		%r = sub i32 %x, %x
		ret i32 %r
	})"},

// isPow2Const used to call getZExtValue() with no width guard, which asserts
// above 64 active bits. MIPS builds i128 registers, so this is reachable.
{"srMulWideConst", eRun::StrengthReduction, nullptr, R"(
	define i128 @fnc(i128 %x) {
		%r = mul i128 %x, 18446744073709551616
		ret i128 %r
	})"},

// ── pointer cast chains ─────────────────────────────────────────────────────
//
// Under opaque pointers every ptr in address space 0 is one Type*, so the two
// ends of a chain compare equal however much the middle threw away. The
// datalayout here makes pointers 64 bits, so the trunc to i32 is a real loss.

{"ptrRoundTripNarrow", eRun::InstOptOnNamed, "c", R"(
	target datalayout = "e-p:64:64:64"
	define i64 @fnc(i64 %x) {
		%p = inttoptr i64 %x to ptr
		%a = ptrtoint ptr %p to i64
		%b = trunc i64 %a to i32
		%c = inttoptr i32 %b to ptr
		%r = ptrtoint ptr %c to i64
		ret i64 %r
	})"},

// The same chain with no narrowing step: this one IS sound and must keep
// collapsing, or the fix above has simply disabled the transform.
{"ptrRoundTripExact", eRun::InstOptOnNamed, "c", R"(
	target datalayout = "e-p:64:64:64"
	define i64 @fnc(i64 %x) {
		%p = inttoptr i64 %x to ptr
		%a = ptrtoint ptr %p to i64
		%c = inttoptr i64 %a to ptr
		%r = ptrtoint ptr %c to i64
		ret i64 %r
	})"},

// A 32-bit target, where ptrtoint to i32 loses nothing.
{"ptrRoundTrip32", eRun::InstOptOnNamed, "c", R"(
	target datalayout = "e-p:32:32:32"
	define i32 @fnc(i32 %x) {
		%p = inttoptr i32 %x to ptr
		%a = ptrtoint ptr %p to i32
		%c = inttoptr i32 %a to ptr
		%r = ptrtoint ptr %c to i32
		ret i32 %r
	})"},
// ── redundant load/store elimination ────────────────────────────────────────
//
// This pass forwards a stored value to a later load and deletes a store that a
// later store overwrites. Both are only sound while nothing in between can see
// or change the memory, and the map it keeps is keyed on the pointer Value*,
// which is not the same thing as the address.

// A narrower store does not overwrite a wider one. That case is NOT here:
// memory in this evaluator is one Constant per pointer, so it cannot express
// four bytes of an eight-byte location being replaced. It is covered by shape
// in the gtest suite instead.

// Two Value* for one address. The second store writes the same location the
// first one did, so the remembered value is stale.
{"rlsTwoNamesForOneAddress", eRun::RedundantLoadStore, nullptr, R"(
	@g = global i32 0
	define i32 @fnc(i32 %x) {
		%p = getelementptr i8, ptr @g, i64 0
		store i32 1, ptr @g
		store i32 2, ptr %p
		%r = load i32, ptr @g
		ret i32 %r
	})"},

// Plain forwarding, which must keep working.
{"rlsForwardsAStore", eRun::RedundantLoadStore, nullptr, R"(
	@g = global i32 0
	define i32 @fnc(i32 %x) {
		store i32 %x, ptr @g
		%r = load i32, ptr @g
		ret i32 %r
	})"},

// A store that a later one really does cover may be removed.
{"rlsDeadStoreIsRemovable", eRun::RedundantLoadStore, nullptr, R"(
	@g = global i32 0
	define i32 @fnc(i32 %x) {
		store i32 1, ptr @g
		store i32 %x, ptr @g
		%r = load i32, ptr @g
		ret i32 %r
	})"},
};

// ── the driver ──────────────────────────────────────────────────────────────

Instruction* findNamed(Function& f, const char* name)
{
	for (Instruction& i : f.front())
	{
		if (i.hasName() && i.getName() == name)
		{
			return &i;
		}
	}
	return nullptr;
}

void runTransform(Module& m, const Case& c)
{
	Function* f = m.getFunction("fnc");
	switch (c.how)
	{
		case eRun::InstOptOnNamed:
		{
			if (Instruction* i = findNamed(*f, c.target))
			{
				retdec::bin2llvmir::inst_opt::optimize(i);
			}
			break;
		}
		case eRun::InstOptOnAll:
		{
			bool again = true;
			while (again)
			{
				again = false;
				for (auto it = f->front().begin(); it != f->front().end(); )
				{
					Instruction* i = &*it++;
					if (retdec::bin2llvmir::inst_opt::optimize(i))
					{
						again = true;
						break;
					}
				}
			}
			break;
		}
		case eRun::StrengthReduction:
		{
			retdec::bin2llvmir::StrengthReduction sr;
			sr.runOnModule(m);
			break;
		}
		case eRun::RedundantLoadStore:
		{
			retdec::bin2llvmir::RedundantLoadStoreElim rls;
			rls.runOnModule(m);
			break;
		}
	}
}

struct Counts
{
	unsigned cases = 0;
	unsigned points = 0;
	unsigned bad = 0;
};

// When `sabotage` is set the "after" module is deliberately broken by making
// the function return a constant. Every case must then be reported. That is
// the self-test: a differ that cannot see a wholesale replacement cannot see
// a subtle one either.
Counts runCases(bool sabotage, bool quiet)
{
	Counts n;

	for (const Case& c : CASES)
	{
		LLVMContext ctx;
		SMDiagnostic err;
		auto before = parseAssemblyString(c.ir, err, ctx);
		if (!before)
		{
			std::string s;
			raw_string_ostream os(s);
			err.print("inst_opt_semantics_check", os);
			fprintf(stderr, "OPT-01: FAIL %s: the case does not parse:\n%s\n",
					c.name, os.str().c_str());
			n.bad++;
			continue;
		}

		auto after = CloneModule(*before);
		if (sabotage)
		{
			// Flip the answer rather than replace it with a fixed constant: a
			// constant can collide with what the function really returns (the
			// `xor x, x` case genuinely answers zero for every input), and a
			// sabotage that coincides with the truth proves nothing. Negating
			// a float and complementing an integer both change the value for
			// every input in the domain, bit for bit.
			Function* f = after->getFunction("fnc");
			auto* ret = cast<ReturnInst>(f->front().getTerminator());
			Value* v = ret->getOperand(0);
			Value* wrong = nullptr;
			if (v->getType()->isFloatingPointTy())
			{
				wrong = UnaryOperator::CreateFNeg(v, "", ret);
			}
			else
			{
				wrong = BinaryOperator::CreateNot(v, "", ret);
			}
			ret->setOperand(0, wrong);
		}
		else
		{
			runTransform(*after, c);
		}

		Function* fb = before->getFunction("fnc");
		Function* fa = after->getFunction("fnc");

		std::vector<std::vector<Constant*>> doms;
		for (Argument& a : fb->args())
		{
			doms.push_back(domainFor(a.getType()));
			if (doms.back().empty())
			{
				fprintf(stderr, "OPT-01: FAIL %s: no input domain for an argument type\n",
						c.name);
				n.bad++;
			}
		}

		// Cartesian product over the arguments' domains.
		std::vector<size_t> idx(doms.size(), 0);
		bool more = true;
		unsigned mismatches = 0;
		while (more && !doms.empty())
		{
			std::vector<Constant*> argsB, argsA;
			for (size_t k = 0; k < doms.size(); ++k)
			{
				argsB.push_back(doms[k][idx[k]]);
			}
			// The two modules have their own context-free constants; the
			// types are identical, so the same Constant* works for both.
			argsA = argsB;

			EvalResult rb = evaluateFunction(*fb, argsB);
			EvalResult ra = evaluateFunction(*fa, argsA);
			n.points++;

			if (rb.giveUp || ra.giveUp)
			{
				fprintf(stderr, "OPT-01: FAIL %s: could not evaluate (%s)\n",
						c.name, rb.giveUp ? rb.giveUp : ra.giveUp);
				n.bad++;
				mismatches++;
				break;
			}
			if (!sameAnswer(rb.value, ra.value))
			{
				if (mismatches < 3 && !quiet)
				{
					std::string in;
					for (size_t k = 0; k < argsB.size(); ++k)
					{
						in += (k ? ", " : "") + render(argsB[k]);
					}
					fprintf(stderr,
							"OPT-01: MISMATCH %s: on (%s) the original gives %s "
							"and the rewrite gives %s\n",
							c.name, in.c_str(),
							render(rb.value).c_str(), render(ra.value).c_str());
				}
				mismatches++;
			}

			size_t k = doms.size();
			while (k-- > 0)
			{
				if (++idx[k] < doms[k].size())
				{
					break;
				}
				idx[k] = 0;
				if (k == 0)
				{
					more = false;
				}
			}
		}

		n.cases++;
		if (mismatches)
		{
			if (!quiet)
			{
				fprintf(stderr, "OPT-01: FAIL %s: %u input(s) changed answer\n",
						c.name, mismatches);
			}
			n.bad++;
		}
	}

	return n;
}

} // anonymous namespace

int main(int argc, char** argv)
{
	bool selfTest = false;
	for (int i = 1; i < argc; ++i)
	{
		if (!strcmp(argv[i], "--self-test"))
		{
			selfTest = true;
		}
	}

	if (selfTest)
	{
		// Every case, with the rewrite replaced by "return a constant". If a
		// single one of them still compares equal, the differ is blind and
		// nothing it says about the real rewrites can be believed.
		Counts s = runCases(/*sabotage=*/true, /*quiet=*/true);
		if (s.bad < s.cases || s.cases == 0)
		{
			fprintf(stderr,
					"OPT-01: self-test FAILED -- only %u of %u sabotaged cases "
					"were caught\n",
					s.bad, s.cases);
			return 2;
		}
		printf("OPT-01: self-test ok (%u sabotaged case(s), all caught)\n", s.cases);
	}

	Counts n = runCases(/*sabotage=*/false, /*quiet=*/false);
	printf("OPT-01: examined %u case(s) over %u input point(s)\n",
			n.cases, n.points);
	if (n.bad)
	{
		fprintf(stderr, "OPT-01: %u case(s) changed what the function computes\n",
				n.bad);
		return 1;
	}
	printf("OPT-01: every rewrite preserved the value\n");
	return 0;
}
