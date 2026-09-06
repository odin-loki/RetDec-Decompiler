/**
 * @file tests/sort_detect/sort_detect_test.cpp
 * @brief Unit tests for the sort detection module (Stage 25).
 *
 * Coverage:
 *   - ElementType::toString
 *   - SortResult::algorithmName / toString
 *   - PartitionFingerprint::analyse
 *   - SiftDownFingerprint::analyse
 *   - RecursiveHalvingFingerprint::analyse
 *   - InsertionSortFingerprint::analyse
 *   - IntrosortDetector::detect
 *   - MergesortDetector::detect
 *   - HeapsortDetector::detect
 *   - RadixsortDetector::detect
 *   - InsertionSortDetector::detect
 *   - QuicksortDetector::detect
 *   - ElementTypeRecoverer::recover
 *   - SortDetector::analyseFunction / analyseModule
 */

#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"
#include <memory>

#include <gtest/gtest.h>

using namespace retdec::sort_detect;
using namespace retdec;

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Build a trivial SSA function with a given set of opcodes inserted once each
// into the entry block.
static std::unique_ptr<ssa::SSAFunction>
makeFunc(const std::string& name, const std::vector<ssa::IrInstr::Op>& ops, int extraBlocks = 0)
{
	auto fn = std::make_unique<ssa::SSAFunction>(name);
	auto* entry = fn->addBlock("entry");
	for (auto op: ops)
	{
		fn->addInstr(entry->id, op);
	}
	for (int i = 0; i < extraBlocks; ++i)
	{
		fn->addBlock("blk" + std::to_string(i));
	}
	return fn;
}

// Build a function that has a phi node.
static void addPhi(ssa::SSAFunction& fn, ssa::VarId var = 0)
{
	if (fn.blockCount() < 1) return;
	fn.addPhi(fn.block(0)->id, var);
}

// Build the opcode bag of a compiled bubble sort: nested loops (two phis, no
// loop-carried decrement), three element compares, a two-store adjacent swap
// and three conditional branches.  `n - 1 - i` is the inner-loop bound, so the
// Sub does not feed a phi.
static std::unique_ptr<ssa::SSAFunction> makeBubbleSort(const std::string& name)
{
	auto fn = std::make_unique<ssa::SSAFunction>(name);
	auto* entry = fn->addBlock("entry");
	auto* outer = fn->addBlock("outer");
	auto* inner = fn->addBlock("inner");
	fn->addBlock("exit");

	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);        // n - 1
	auto* iInc = fn->addInstr(outer->id, ssa::IrInstr::Op::Add);
	fn->addInstr(outer->id, ssa::IrInstr::Op::Sub);        // n - 1 - i (bound)
	fn->addInstr(outer->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(outer->id, ssa::IrInstr::Op::CondBranch);
	auto* jInc = fn->addInstr(inner->id, ssa::IrInstr::Op::Add);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Load);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Load);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(inner->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Store);      // the adjacent swap
	fn->addInstr(inner->id, ssa::IrInstr::Op::Store);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(inner->id, ssa::IrInstr::Op::CondBranch);
	for (int k = 0; k < 4; ++k)
		fn->addInstr(inner->id, ssa::IrInstr::Op::Add);

	// Both induction variables advance; neither retreats.
	auto* iPhi = fn->addPhi(outer->id, 0);
	auto* iVal = fn->allocValue(ssa::ValueKind::VirtualReg);
	iVal->defInstr = iInc;
	iPhi->addOperand(inner->id, iVal->id);
	auto* jPhi = fn->addPhi(inner->id, 1);
	auto* jVal = fn->allocValue(ssa::ValueKind::VirtualReg);
	jVal->defInstr = jInc;
	jPhi->addOperand(inner->id, jVal->id);
	return fn;
}

// Find a block by label; the fixtures below need a specific loop header and
// the ids are an implementation detail of the order they were added in.
static ssa::BasicBlock* blockNamed(ssa::SSAFunction& fn, const std::string& label)
{
	for (std::uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		auto* blk = fn.block(b);
		if (blk && blk->name == label) return blk;
	}
	return nullptr;
}

// The descending form:
//
//     for (i = n - 1; i > 0; --i)
//         for (j = 0; j < i; ++j)
//
// which is also what -O2 makes of the ascending one when it turns the
// `n - 1 - i` bound into a loop-carried decrement.  The outer index retreats
// and the inner advances, so the function has both an Add-fed and a Sub-fed
// phi -- but in *different* loop headers, because neither loop updates both.
static std::unique_ptr<ssa::SSAFunction> makeDescendingBubbleSort(const std::string& name)
{
	auto fn = std::make_unique<ssa::SSAFunction>(name);
	auto* entry = fn->addBlock("entry");
	auto* outer = fn->addBlock("outer");
	auto* inner = fn->addBlock("inner");
	fn->addBlock("exit");

	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);        // i = n - 1
	auto* iDec = fn->addInstr(outer->id, ssa::IrInstr::Op::Sub);   // --i
	fn->addInstr(outer->id, ssa::IrInstr::Op::Compare);    // i > 0
	fn->addInstr(outer->id, ssa::IrInstr::Op::CondBranch);
	auto* jInc = fn->addInstr(inner->id, ssa::IrInstr::Op::Add);   // ++j
	fn->addInstr(inner->id, ssa::IrInstr::Op::Load);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Load);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(inner->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Store);      // the adjacent swap
	fn->addInstr(inner->id, ssa::IrInstr::Op::Store);
	fn->addInstr(inner->id, ssa::IrInstr::Op::Compare);    // j < i
	fn->addInstr(inner->id, ssa::IrInstr::Op::CondBranch);
	for (int k = 0; k < 4; ++k)
		fn->addInstr(inner->id, ssa::IrInstr::Op::Add);

	// The outer index retreats, the inner one advances, and each phi is in the
	// header of the loop that updates it -- so no single loop updates both.
	auto* iPhi = fn->addPhi(outer->id, 0);
	auto* iVal = fn->allocValue(ssa::ValueKind::VirtualReg);
	iVal->defInstr = iDec;
	iPhi->addOperand(inner->id, iVal->id);
	auto* jPhi = fn->addPhi(inner->id, 1);
	auto* jVal = fn->allocValue(ssa::ValueKind::VirtualReg);
	jVal->defInstr = jInc;
	jPhi->addOperand(inner->id, jVal->id);
	return fn;
}

// ─── ElementType tests ────────────────────────────────────────────────────────

TEST(ElementTypeTest, UnknownToString)
{
	ElementType et;
	EXPECT_EQ(et.toString(), "unknown");
}

TEST(ElementTypeTest, Int32ToString)
{
	ElementType et;
	et.kind = ElementType::Kind::Int32;
	EXPECT_EQ(et.toString(), "int32_t");
}

TEST(ElementTypeTest, UInt8ToString)
{
	ElementType et;
	et.kind = ElementType::Kind::UInt8;
	EXPECT_EQ(et.toString(), "uint8_t");
}

TEST(ElementTypeTest, FloatToString)
{
	ElementType et;
	et.kind = ElementType::Kind::Float;
	EXPECT_EQ(et.toString(), "float");
}

TEST(ElementTypeTest, StructWithName)
{
	ElementType et;
	et.kind = ElementType::Kind::Struct;
	et.name = "MyRecord";
	EXPECT_EQ(et.toString(), "struct MyRecord");
}

// ─── SortResult tests ─────────────────────────────────────────────────────────

TEST(SortResultTest, AlgorithmNameIntrosort)
{
	SortResult r;
	r.algorithm = SortAlgorithm::Introsort;
	EXPECT_NE(r.algorithmName().find("introsort"), std::string::npos);
}

TEST(SortResultTest, AlgorithmNameMergesort)
{
	SortResult r;
	r.algorithm = SortAlgorithm::Mergesort;
	EXPECT_NE(r.algorithmName().find("mergesort"), std::string::npos);
}

TEST(SortResultTest, AlgorithmNameRadix)
{
	SortResult r;
	r.algorithm = SortAlgorithm::Radixsort;
	EXPECT_NE(r.algorithmName().find("radix"), std::string::npos);
}

TEST(SortResultTest, ToStringContainsConfidence)
{
	SortResult r;
	r.algorithm = SortAlgorithm::Heapsort;
	r.confidence = 0.75f;
	std::string s = r.toString();
	EXPECT_NE(s.find("0.75"), std::string::npos);
}

TEST(SortResultTest, ToStringContainsCompilerGCC)
{
	SortResult r;
	r.algorithm = SortAlgorithm::Introsort;
	r.confidence = 0.9f;
	r.compilerVariant = CompilerVariant::GCC;
	EXPECT_NE(r.toString().find("GCC"), std::string::npos);
}

TEST(SortResultTest, UnknownAlgorithmName)
{
	SortResult r;
	EXPECT_EQ(r.algorithmName(), "unknown");
}

// ─── PartitionFingerprint tests ───────────────────────────────────────────────

TEST(PartitionFingerprintTest, EmptyFunctionNoPartition)
{
	auto fn = makeFunc("empty", {});
	PartitionFingerprint pf;
	auto ev = pf.analyse(*fn);
	EXPECT_FALSE(ev.found);
	EXPECT_LT(ev.confidence, 0.30f);
}

TEST(PartitionFingerprintTest, CompareAloneGivesPartialScore)
{
	auto fn = makeFunc("cmp_only", {ssa::IrInstr::Op::Compare});
	PartitionFingerprint pf;
	auto ev = pf.analyse(*fn);
	// Compare contributes +0.30 to score, threshold is 0.30.
	EXPECT_GE(ev.confidence, 0.29f);
}

TEST(PartitionFingerprintTest, FullPartitionPattern)
{
	// Compare + 2 Stores + Add + Sub + CondBranch × 2 + extra blocks for phi.
	auto fn = makeFunc(
		"partition",
		{ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Add,
		 ssa::IrInstr::Op::Sub,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::CondBranch},
		/*extraBlocks=*/3);
	addPhi(*fn, 0);
	addPhi(*fn, 1);
	PartitionFingerprint pf;
	auto ev = pf.analyse(*fn);
	EXPECT_TRUE(ev.found);
	EXPECT_GE(ev.confidence, 0.50f);
}

// ─── SiftDownFingerprint tests ────────────────────────────────────────────────

TEST(SiftDownFingerprintTest, EmptyFunctionNoSiftDown)
{
	auto fn = makeFunc("empty", {});
	SiftDownFingerprint sdf;
	auto ev = sdf.analyse(*fn);
	EXPECT_FALSE(ev.found);
}

TEST(SiftDownFingerprintTest, ShlOnePatternDetected)
{
	// Shl(x, 1) is the 2*x child index arithmetic.
	auto fn = std::make_unique<ssa::SSAFunction>("sift");
	auto* blk = fn->addBlock("entry");
	auto* shl = fn->addInstr(blk->id, ssa::IrInstr::Op::Shl);
	// Attach a constant 1 as the shift amount.
	auto* immVal = fn->allocValue(ssa::ValueKind::Immediate);
	immVal->imm = 1;
	ssa::Use shiftUse;
	shiftUse.valueId = immVal->id;
	shiftUse.operandIndex = 1;
	shl->uses.push_back(shiftUse);

	SiftDownFingerprint sdf;
	auto ev = sdf.analyse(*fn);
	EXPECT_TRUE(ev.hasLeftArith);
}

TEST(SiftDownFingerprintTest, MulTwoImmediateIsChildIndex)
{
	// Recovered `i * 2` has only the ConstantInt use attached.
	auto fn = std::make_unique<ssa::SSAFunction>("sift");
	auto* blk = fn->addBlock("entry");
	auto* mul = fn->addInstr(blk->id, ssa::IrInstr::Op::Mul);
	auto* immVal = fn->allocValue(ssa::ValueKind::Immediate);
	immVal->imm = 2;
	ssa::Use mulUse;
	mulUse.valueId = immVal->id;
	mulUse.operandIndex = 1;
	mul->uses.push_back(mulUse);

	SiftDownFingerprint sdf;
	auto ev = sdf.analyse(*fn);
	EXPECT_TRUE(ev.hasLeftArith);
}

TEST(SiftDownFingerprintTest, FullSiftDownSignature)
{
	// Shl + Compare + CondBranch + 2 Stores.
	auto fn = makeFunc(
		"sift_down",
		{ssa::IrInstr::Op::Shl,
		 ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Store},
		2);
	// Attach const 1 to the Shl.
	auto* shlInstr = fn->block(fn->entryId())->instrs[0];
	auto* imm1 = fn->allocValue(ssa::ValueKind::Immediate);
	imm1->imm = 1;
	ssa::Use u;
	u.valueId = imm1->id;
	u.operandIndex = 1;
	shlInstr->uses.push_back(u);

	SiftDownFingerprint sdf;
	auto ev = sdf.analyse(*fn);
	EXPECT_TRUE(ev.found);
	EXPECT_GE(ev.confidence, 0.6f);
}

// ─── RecursiveHalvingFingerprint tests ────────────────────────────────────────

TEST(RecursiveHalvingFingerprintTest, NoSelfCallsNoHalving)
{
	auto fn = makeFunc("foo", {ssa::IrInstr::Op::Add, ssa::IrInstr::Op::Ret});
	RecursiveHalvingFingerprint rhf;
	auto ev = rhf.analyse(*fn);
	EXPECT_EQ(ev.selfCallCount, 0);
	EXPECT_FALSE(ev.found);
}

TEST(RecursiveHalvingFingerprintTest, TwoSelfCallsFound)
{
	auto fn = std::make_unique<ssa::SSAFunction>("merge_sort");
	auto* blk = fn->addBlock("entry");
	// Add two self-calls.
	auto* c1 = fn->addInstr(blk->id, ssa::IrInstr::Op::Call);
	c1->calleeName = "merge_sort";
	auto* c2 = fn->addInstr(blk->id, ssa::IrInstr::Op::Call);
	c2->calleeName = "merge_sort";
	fn->addInstr(blk->id, ssa::IrInstr::Op::Add);
	fn->addInstr(blk->id, ssa::IrInstr::Op::Shr);

	RecursiveHalvingFingerprint rhf;
	auto ev = rhf.analyse(*fn);
	EXPECT_EQ(ev.selfCallCount, 2);
	EXPECT_TRUE(ev.found);
	EXPECT_TRUE(ev.halvingConfirmed);
}

// ─── InsertionSortFingerprint tests ──────────────────────────────────────────

TEST(InsertionSortFingerprintTest, EmptyFunctionNoInsertion)
{
	auto fn = makeFunc("empty", {});
	InsertionSortFingerprint isf;
	auto ev = isf.analyse(*fn);
	EXPECT_FALSE(ev.found);
}

TEST(InsertionSortFingerprintTest, BasicInsertionPattern)
{
	// Sub (decrement) + Compare + Store + extra blocks.
	auto fn = makeFunc(
		"insertion",
		{ssa::IrInstr::Op::Sub,
		 ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::CondBranch},
		3);
	InsertionSortFingerprint isf;
	auto ev = isf.analyse(*fn);
	EXPECT_TRUE(ev.found);
	EXPECT_GE(ev.confidence, 0.4f);
}

TEST(InsertionSortFingerprintTest, OneCompareIsNotInsertion)
{
	// atoi/parse is Sub+one Compare+Store, not a shift loop.
	auto fn = makeFunc(
		"atoi_parse",
		{ssa::IrInstr::Op::Sub, ssa::IrInstr::Op::Compare, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::CondBranch},
		3);
	InsertionSortFingerprint isf;
	auto ev = isf.analyse(*fn);
	EXPECT_FALSE(ev.found);
}

TEST(InsertionSortFingerprintTest, ThresholdGuard16)
{
	auto fn = std::make_unique<ssa::SSAFunction>("insertion_sort");
	auto* entry = fn->addBlock("entry");
	fn->addBlock("loop");
	fn->addBlock("exit");

	// Add instructions to entry block.
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	auto* cmpI = fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);

	// Attach immediate 16 as a Compare operand.
	auto* imm16 = fn->allocValue(ssa::ValueKind::Immediate);
	imm16->imm = 16;
	ssa::Use u;
	u.valueId = imm16->id;
	u.operandIndex = 0;
	cmpI->uses.push_back(u);

	InsertionSortFingerprint isf;
	auto ev = isf.analyse(*fn);
	EXPECT_TRUE(ev.found);
	EXPECT_TRUE(ev.hasThresholdGuard);
	EXPECT_EQ(ev.threshold, 16);
	EXPECT_GE(ev.confidence, 0.7f);
}

// ─── IntrosortDetector tests ──────────────────────────────────────────────────

TEST(IntrosortDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("fn", {});
	IntrosortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(IntrosortDetectorTest, FullIntrosortPattern)
{
	// Compare + 2 Stores + Add + Sub × 2 + CondBranch × 2 + self-calls × 2 + phi × 2
	auto fn = std::make_unique<ssa::SSAFunction>("__sort");
	auto* entry = fn->addBlock("entry");
	fn->addBlock("loop");
	fn->addBlock("rec");
	fn->addBlock("ins");
	fn->addBlock("exit");

	fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Add);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	auto* c1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
	c1->calleeName = "__sort";
	auto* c2 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
	c2->calleeName = "__sort";

	// Add a Compare against 0 (depth counter check).
	auto* cmpZero = fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	auto* imm0 = fn->allocValue(ssa::ValueKind::Immediate);
	imm0->imm = 0;
	ssa::Use u;
	u.valueId = imm0->id;
	cmpZero->uses.push_back(u);

	fn->addPhi(entry->id, 0);
	fn->addPhi(entry->id, 1);

	IntrosortDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_EQ(r.algorithm, SortAlgorithm::Introsort);
	EXPECT_EQ(r.compilerVariant, CompilerVariant::GCC);
}

// ─── MergesortDetector tests ──────────────────────────────────────────────────

TEST(MergesortDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("fn", {});
	MergesortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.40f);
}

TEST(MergesortDetectorTest, TwoRecursiveCallsBoostScore)
{
	auto fn = std::make_unique<ssa::SSAFunction>("stable_sort");
	auto* entry = fn->addBlock("entry");
	fn->addBlock("merge_blk");
	fn->addBlock("exit");

	auto* c1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
	c1->calleeName = "stable_sort";
	auto* c2 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
	c2->calleeName = "stable_sort";

	// malloc call for aux buffer.
	auto* mallocI = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
	mallocI->calleeName = "malloc";

	// Merge loop: 2 loads + compare + 2 cond branches.
	fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Add);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Shr);

	MergesortDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.60f);
	EXPECT_EQ(r.algorithm, SortAlgorithm::Mergesort);
}

TEST(MergesortDetectorTest, MergeLoopWithoutRecursionStaysBelowAssign)
{
	// Same opcode bag as hasMergeLoop (2 loads + cmp + 2 branches).
	// The removed 0.55 floor must not assign this as mergesort.
	auto fn = makeFunc(
		"fir_tap",
		{ssa::IrInstr::Op::Load,
		 ssa::IrInstr::Op::Load,
		 ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::CondBranch});
	MergesortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.50f);
}

// ─── HeapsortDetector tests ───────────────────────────────────────────────────

TEST(HeapsortDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("fn", {});
	HeapsortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(HeapsortDetectorTest, FullHeapsortPattern)
{
	// Shl(x,1) + Compare + CondBranch + 2×Sub + 2×Store.
	auto fn = std::make_unique<ssa::SSAFunction>("sort_heap");
	auto* entry = fn->addBlock("entry");
	fn->addBlock("build");
	fn->addBlock("sort");

	auto* shl = fn->addInstr(entry->id, ssa::IrInstr::Op::Shl);
	auto* imm1 = fn->allocValue(ssa::ValueKind::Immediate);
	imm1->imm = 1;
	ssa::Use u;
	u.valueId = imm1->id;
	u.operandIndex = 1;
	shl->uses.push_back(u);

	fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);

	HeapsortDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.50f);
	EXPECT_EQ(r.algorithm, SortAlgorithm::Heapsort);
}

TEST(HeapsortDetectorTest, NoChildIndexStaysBelowAssign)
{
	// Build+sort opcode bag without Mul 2 / Shl 1 is not heapsort.
	auto fn = makeFunc(
		"not_heap",
		{ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::Sub,
		 ssa::IrInstr::Op::Sub,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Store});
	HeapsortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.55f);
}

TEST(HeapsortDetectorTest, XorHeavyIsNotHeapsort)
{
	// AES GF/T-table mixers have many Xors; that is not sift-down.
	auto fn = std::make_unique<ssa::SSAFunction>("aes_mix");
	auto* entry = fn->addBlock("entry");
	auto* shl = fn->addInstr(entry->id, ssa::IrInstr::Op::Shl);
	auto* imm1 = fn->allocValue(ssa::ValueKind::Immediate);
	imm1->imm = 1;
	ssa::Use u;
	u.valueId = imm1->id;
	u.operandIndex = 1;
	shl->uses.push_back(u);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	for (int i = 0; i < 8; ++i)
		fn->addInstr(entry->id, ssa::IrInstr::Op::Xor);
	HeapsortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.55f);
}

// ─── RadixsortDetector tests ──────────────────────────────────────────────────

TEST(RadixsortDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("fn", {});
	RadixsortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(RadixsortDetectorTest, ZeroComparisonsBoostScore)
{
	// No Compare instructions → radix signal.
	auto fn = makeFunc(
		"radix_sort",
		{ssa::IrInstr::Op::Shr,
		 ssa::IrInstr::Op::And,
		 ssa::IrInstr::Op::Load,
		 ssa::IrInstr::Op::Load,
		 ssa::IrInstr::Op::Load,
		 ssa::IrInstr::Op::Load,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Add,
		 ssa::IrInstr::Op::Add},
		2);
	// Attach constant shift and mask.
	auto* blk = fn->block(fn->entryId());
	// Shr with constant multiple of 8.
	auto* shrI = blk->instrs[0];
	auto* imm8 = fn->allocValue(ssa::ValueKind::Immediate);
	imm8->imm = 8;
	ssa::Use ush;
	ush.valueId = imm8->id;
	ush.operandIndex = 1;
	shrI->uses.push_back(ush);
	// And with mask 0xff.
	auto* andI = blk->instrs[1];
	auto* immMask = fn->allocValue(ssa::ValueKind::Immediate);
	immMask->imm = 0xff;
	ssa::Use uand;
	uand.valueId = immMask->id;
	uand.operandIndex = 1;
	andI->uses.push_back(uand);
	// Add phi for prefix-sum loop.
	fn->addPhi(blk->id, 0);

	RadixsortDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.50f);
	EXPECT_EQ(r.algorithm, SortAlgorithm::Radixsort);
}

TEST(RadixsortDetectorTest, WithComparisonsLowScore)
{
	// Has Compare → not a strong radix signal.
	auto fn = makeFunc(
		"cmp_sort",
		{ssa::IrInstr::Op::Compare, ssa::IrInstr::Op::CondBranch, ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store});
	RadixsortDetector det;
	auto r = det.detect(*fn);
	// Compare in a block that also has Load → countElementComparisons > 0.
	EXPECT_LT(r.confidence, 0.65f);
}

// ─── QuicksortDetector tests ──────────────────────────────────────────────────

TEST(QuicksortDetectorTest, PartitionWithoutSelfCallIsNotQuicksort)
{
	// Same bag as PartitionFingerprintTest.FullPartitionPattern.
	auto fn = makeFunc(
		"fir_tap",
		{ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Add,
		 ssa::IrInstr::Op::Sub,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::CondBranch},
		/*extraBlocks=*/3);
	addPhi(*fn, 0);
	addPhi(*fn, 1);
	QuicksortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(QuicksortDetectorTest, PartitionWithSelfCallIsQuicksort)
{
	auto fn = makeFunc(
		"qsort_rec",
		{ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Add,
		 ssa::IrInstr::Op::Sub,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::CondBranch},
		/*extraBlocks=*/3);
	addPhi(*fn, 0);
	addPhi(*fn, 1);
	auto* call = fn->addInstr(fn->block(fn->entryId())->id, ssa::IrInstr::Op::Call);
	call->calleeName = "qsort_rec";
	QuicksortDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.algorithm, SortAlgorithm::Quicksort);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_NEAR(r.confidence, 0.70f, 0.30f);
}

// ─── BubbleSortDetector tests ─────────────────────────────────────────────────

// BubbleSortDetector was unreachable: its own entry conditions (>= 3 compares,
// a swap, >= 3 branches) already put PartitionFingerprint at 0.75, so the
// "partition evidence exists" guard fired on every bubble sort and returned
// confidence 0.  Every bubble sort came back as introsort instead.
TEST(BubbleSortDetectorTest, BubbleSortIsDetected)
{
	auto fn = makeBubbleSort("bubble_sort");
	BubbleSortDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.algorithm, SortAlgorithm::BubbleSort);
	EXPECT_GE(r.confidence, 0.45f);
}

TEST(BubbleSortDetectorTest, BubbleSortWinsOverIntrosort)
{
	auto fn = makeBubbleSort("bubble_sort");
	SortDetector det;
	auto r = det.analyseFunction(*fn);
	EXPECT_EQ(r.algorithm, SortAlgorithm::BubbleSort);
}

// The suppression must still hold where it means something: a Hoare partition
// retreats its right index, and that decrement feeds the loop-header phi.
TEST(BubbleSortDetectorTest, ConvergingIndicesStillSuppressBubble)
{
	// A Hoare partition carries *both* indices in one loop, so the advancing
	// and the retreating phi sit in the same header.  This fixture used to put
	// the retreating phi in the entry block, where no advancing phi lives --
	// which is not a partition at all, it is the descending-bubble-sort shape
	// below, and asserting suppression for it asserted the bug.
	auto fn = makeBubbleSort("hoare_partition");
	auto* inner = blockNamed(*fn, "inner");   // the header the Add-fed phi is in
	auto* dec = fn->addInstr(inner->id, ssa::IrInstr::Op::Sub);   // hi = hi - 1
	auto* hiPhi = fn->addPhi(inner->id, 2);
	auto* hiVal = fn->allocValue(ssa::ValueKind::VirtualReg);
	hiVal->defInstr = dec;
	hiPhi->addOperand(inner->id, hiVal->id);
	BubbleSortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

// The reason the gate above has to ask which loop the phis belong to.
TEST(BubbleSortDetectorTest, DescendingBubbleSortIsStillABubbleSort)
{
	// Both an Add-fed and a Sub-fed phi exist here, in different loop headers.
	// Asking only that both exist somewhere in the function suppressed this as
	// a partition, so the textbook descending bubble sort came back as
	// `introsort (std::sort)` at 0.700 -- a false positive for introsort and a
	// false negative for bubble sort from one predicate.
	auto fn = makeDescendingBubbleSort("bubble_descending");
	BubbleSortDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_EQ(r.algorithm, SortAlgorithm::BubbleSort);
}

// Sift-down evidence may only veto a bubble sort when it carries the one
// heap-specific signal, the 2*i+1 child-index arithmetic.
TEST(BubbleSortDetectorTest, ChildIndexArithmeticStillSuppressesBubble)
{
	auto fn = makeBubbleSort("sift_down");
	auto* blk = fn->block(fn->entryId());
	auto* shl = fn->addInstr(blk->id, ssa::IrInstr::Op::Shl);
	auto* imm1 = fn->allocValue(ssa::ValueKind::Immediate);
	imm1->imm = 1;
	ssa::Use u;
	u.valueId = imm1->id;
	u.operandIndex = 1;
	shl->uses.push_back(u);
	BubbleSortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

// IntrosortDetector had no gate at all: half the partition confidence plus a
// flat 0.20 for an "insertion sort tail" whose predicate is only
// `>= 1 Sub, >= 2 Compares, >= 1 Store, >= 3 blocks`.  Introsort is quicksort
// with a depth bound and two fallbacks, so it recurses or it delegates; asking
// for one of the two is what the sibling QuicksortDetector already does, and
// for the same measured reason.
TEST(IntrosortDetectorTest, PlainCopyLoopIsNotIntrosort)
{
	// A backwards memmove-style copy: two loads, two stores, a Sub, two
	// compares, two conditional branches.  No calls of any kind.
	auto fn = std::make_unique<ssa::SSAFunction>("copy_backwards");
	auto* entry = fn->addBlock("entry");
	auto* loop  = fn->addBlock("loop");
	fn->addBlock("exit");

	fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Sub);        // --i
	fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Store);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Store);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(loop->id, ssa::IrInstr::Op::CondBranch);

	IntrosortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.50f);
}

// The gate may not cost a real introsort its score.
TEST(IntrosortDetectorTest, RecursivePartitionStillScores)
{
	auto fn = makeDescendingBubbleSort("__introsort_loop");
	// Two self-calls on the sub-ranges, which is what makes it introsort.
	for (int k = 0; k < 2; ++k) {
		auto* call = fn->addInstr(fn->block(1)->id, ssa::IrInstr::Op::Call);
		call->calleeName = "__introsort_loop";
	}
	IntrosortDetector det;
	auto r = det.detect(*fn);
	EXPECT_GT(r.confidence, 0.0f);
	EXPECT_EQ(r.algorithm, SortAlgorithm::Introsort);
}

// ─── InsertionSortDetector tests ──────────────────────────────────────────────

TEST(InsertionSortDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("fn", {});
	InsertionSortDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.40f);
}

TEST(InsertionSortDetectorTest, BasicPattern)
{
	auto fn = makeFunc(
		"insertion",
		{ssa::IrInstr::Op::Sub,
		 ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::CondBranch},
		3);
	InsertionSortDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.40f);
	EXPECT_EQ(r.algorithm, SortAlgorithm::InsertionSort);
}

// ─── ElementTypeRecoverer tests ───────────────────────────────────────────────

TEST(ElementTypeRecovererTest, NoCompareReturnsUnknown)
{
	auto fn = makeFunc("fn", {ssa::IrInstr::Op::Add});
	ElementTypeRecoverer rec;
	SortResult dummy;
	auto et = rec.recover(*fn, dummy);
	EXPECT_EQ(et.kind, ElementType::Kind::Unknown);
}

TEST(ElementTypeRecovererTest, CompareWithLoadOperandReturnsType)
{
	auto fn = std::make_unique<ssa::SSAFunction>("sort_fn");
	auto* blk = fn->addBlock("entry");

	// Create a Load instruction whose result is used by a Compare.
	auto* loadI = fn->addInstr(blk->id, ssa::IrInstr::Op::Load);
	auto* loadVal = fn->allocValue(ssa::ValueKind::VirtualReg);
	loadVal->width = 32;
	loadVal->defInstr = loadI;
	loadI->defValue = loadVal->id;

	auto* cmpI = fn->addInstr(blk->id, ssa::IrInstr::Op::Compare);
	ssa::Use u;
	u.valueId = loadVal->id;
	u.operandIndex = 0;
	cmpI->uses.push_back(u);

	ElementTypeRecoverer rec;
	SortResult dummy;
	auto et = rec.recover(*fn, dummy);
	EXPECT_EQ(et.kind, ElementType::Kind::Int32);
	EXPECT_EQ(et.byteWidth, 4);
}

// ─── SortDetector orchestration tests ────────────────────────────────────────

TEST(SortDetectorTest, EmptyModuleNoDetections)
{
	SortDetector det;
	auto res = det.analyseModule({});
	EXPECT_TRUE(res.empty());
	EXPECT_EQ(det.stats().functionsAnalysed, 0u);
}

TEST(SortDetectorTest, TrivialFunctionSkipped)
{
	SortDetector det; // default cfg: minBlocks=3, minInstrs=15
	auto fn = makeFunc("tiny", {ssa::IrInstr::Op::Ret});
	det.analyseFunction(*fn);
	EXPECT_GT(det.stats().functionsSkipped, 0u);
}

TEST(SortDetectorTest, StrongIntrosortSignalDetected)
{
	// Build a function with a strong introsort-like signature.
	auto fn = std::make_unique<ssa::SSAFunction>("__introsort_loop");
	auto* entry = fn->addBlock("entry");
	fn->addBlock("blk1");
	fn->addBlock("blk2");
	fn->addBlock("blk3");
	fn->addBlock("blk4");

	fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Add);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	auto* c1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
	c1->calleeName = "__introsort_loop";
	auto* c2 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
	c2->calleeName = "__introsort_loop";
	auto* cz = fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	auto* imm0 = fn->allocValue(ssa::ValueKind::Immediate);
	imm0->imm = 0;
	ssa::Use uz;
	uz.valueId = imm0->id;
	cz->uses.push_back(uz);
	fn->addPhi(entry->id, 0);
	fn->addPhi(entry->id, 1);

	// More instructions to pass minInstrs=15.
	for (int i = 0; i < 4; ++i)
		fn->addInstr(entry->id, ssa::IrInstr::Op::Add);

	SortDetector det;
	auto r = det.analyseFunction(*fn);
	EXPECT_EQ(r.algorithm, SortAlgorithm::Introsort);
	EXPECT_GE(r.confidence, SortDetector::Config{}.minConfidence);
}

TEST(SortDetectorTest, ModuleAnalysisMultipleFunctions)
{
	SortDetector det;
	// A very small function (will be skipped) + a larger one.
	auto tiny = makeFunc("tiny", {ssa::IrInstr::Op::Ret});

	auto merge = std::make_unique<ssa::SSAFunction>("stable_sort");
	auto* entry = merge->addBlock("entry");
	for (int i = 0; i < 3; ++i)
		merge->addBlock("b" + std::to_string(i));
	auto* c1 = merge->addInstr(entry->id, ssa::IrInstr::Op::Call);
	c1->calleeName = "stable_sort";
	auto* c2 = merge->addInstr(entry->id, ssa::IrInstr::Op::Call);
	c2->calleeName = "stable_sort";
	auto* mallocI = merge->addInstr(entry->id, ssa::IrInstr::Op::Call);
	mallocI->calleeName = "malloc";
	merge->addInstr(entry->id, ssa::IrInstr::Op::Load);
	merge->addInstr(entry->id, ssa::IrInstr::Op::Load);
	merge->addInstr(entry->id, ssa::IrInstr::Op::Compare);
	merge->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	merge->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
	merge->addInstr(entry->id, ssa::IrInstr::Op::Add);
	merge->addInstr(entry->id, ssa::IrInstr::Op::Shr);
	// Add 5 more instructions to reach minInstrs=15.
	for (int i = 0; i < 5; ++i)
		merge->addInstr(entry->id, ssa::IrInstr::Op::Add);

	std::vector<const ssa::SSAFunction*> fns = {tiny.get(), merge.get()};
	auto results = det.analyseModule(fns);

	// The "tiny" function should be skipped (not in results or low confidence).
	EXPECT_FALSE(results.count("tiny") && results.at("tiny").algorithm != SortAlgorithm::Unknown);
}

TEST(SortDetectorTest, StatsTracking)
{
	SortDetector det;
	auto fn1 = makeFunc("f1", {ssa::IrInstr::Op::Ret});
	auto fn2 = makeFunc("f2", {ssa::IrInstr::Op::Ret});
	det.analyseFunction(*fn1);
	det.analyseFunction(*fn2);
	EXPECT_EQ(det.stats().functionsAnalysed, 2u);
}

TEST(SortDetectorTest, CustomMinConfidence)
{
	SortDetector::Config cfg;
	cfg.minConfidence = 0.99f; // Very high threshold — nothing should pass.
	SortDetector det(cfg);

	auto fn = makeFunc(
		"sort_fn",
		{ssa::IrInstr::Op::Compare,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Add,
		 ssa::IrInstr::Op::Sub,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::CondBranch,
		 ssa::IrInstr::Op::Load,
		 ssa::IrInstr::Op::Load,
		 ssa::IrInstr::Op::Store,
		 ssa::IrInstr::Op::Store},
		5);
	fn->addPhi(fn->block(fn->entryId())->id, 0);
	auto r = det.analyseFunction(*fn);
	EXPECT_EQ(r.algorithm, SortAlgorithm::Unknown);
}
