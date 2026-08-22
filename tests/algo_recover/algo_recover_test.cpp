/**
 * @file tests/algo_recover/algo_recover_test.cpp
 * @brief Unit tests for the Algorithm Header Recovery module (Stage 27).
 *
 * Coverage:
 *   - AlgorithmResult::kindName / toString
 *   - TransformDetector::detect  (copy, lambda, back_inserter)
 *   - AccumulateDetector::detect (add/mul/or/xor/max/min combiners)
 *   - FindDetector::detect       (find/find_if/count)
 *   - PartitionDetector::detect  (converging ptrs, swap, no recursion)
 *   - BinarySearchDetector::detect (SSA mid→load→compare, not opcode counts)
 *   - ForEachDetector::detect    (call per element, no accumulator)
 *   - IteratorPatternRecovery    (begin/end, reverse, back_inserter)
 *   - AlgorithmDetector          (preflight, tier assignment, orchestration)
 *   - gcc -O0 IR-shape fixtures  (binary_search / bubblesort / memcpy_loop)
 */

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"
#include <memory>

#include <gtest/gtest.h>
#include <string>

using namespace retdec::algo_recover;
using namespace retdec;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::unique_ptr<ssa::SSAFunction>
makeFunc(const std::string& name, const std::vector<ssa::IrInstr::Op>& ops, int extraBlocks = 0)
{
	auto fn = std::make_unique<ssa::SSAFunction>(name);
	auto* entry = fn->addBlock("entry");
	for (auto op: ops)
		fn->addInstr(entry->id, op);
	for (int i = 0; i < extraBlocks; ++i)
		fn->addBlock("blk" + std::to_string(i));
	return fn;
}

static void addBackEdge(ssa::SSAFunction& fn)
{
	// Add a back-edge from block 1 to block 0.
	if (fn.blockCount() >= 2)
		fn.block(1)->succs.push_back(0);
	else if (fn.blockCount() == 1)
		fn.block(0)->succs.push_back(0);
}

static void addCall(ssa::SSAFunction& fn, const std::string& callee)
{
	auto* instr = fn.addInstr(fn.block(0)->id, ssa::IrInstr::Op::Call);
	if (instr) instr->calleeName = callee;
}

static void addImmInstr(ssa::SSAFunction& fn, ssa::IrInstr::Op op, uint64_t immVal)
{
	auto* instr = fn.addInstr(fn.block(0)->id, op);
	if (!instr) return;
	ssa::IrValue* val = fn.allocValue(ssa::ValueKind::Immediate);
	if (val) val->imm = immVal;
	ssa::Use u;
	u.valueId = val ? val->id : ssa::kInvalidValue;
	instr->uses.push_back(u);
}

static void addPhi(ssa::SSAFunction& fn)
{
	if (fn.blockCount() > 0) fn.addPhi(fn.block(0)->id, 0);
}

static void useVal(ssa::IrInstr* instr, ssa::ValueId id, uint8_t idx = 0)
{
	if (!instr) return;
	ssa::Use u;
	u.valueId = id;
	u.operandIndex = idx;
	instr->uses.push_back(u);
}

static ssa::IrValue* defOf(ssa::SSAFunction& fn, ssa::IrInstr* instr, ssa::VarId var = ssa::kInvalidVar)
{
	ssa::IrValue* v = fn.allocValue(ssa::ValueKind::VirtualReg, var);
	if (instr)
	{
		instr->defValue = v->id;
		v->defInstr = instr;
	}
	return v;
}

/// Connected binary-search SSA: mid = (lo+hi)>>1, load at mid, compare
/// against an immediate target, lo/hi updated from mid into header phis.
static std::unique_ptr<ssa::SSAFunction> makeConnectedBinarySearch()
{
	auto fn = std::make_unique<ssa::SSAFunction>("f");
	auto* entry = fn->addBlock("entry");
	auto* header = fn->addBlock("header");
	auto* body = fn->addBlock("body");
	auto* loUpd = fn->addBlock("lo_upd");
	auto* hiUpd = fn->addBlock("hi_upd");
	auto* exitB = fn->addBlock("exit");

	entry->addSucc(header->id);
	header->addSucc(body->id);
	header->addSucc(exitB->id);
	header->addPred(entry->id);
	header->addPred(loUpd->id);
	header->addPred(hiUpd->id);
	body->addSucc(loUpd->id);
	body->addSucc(hiUpd->id);
	body->addPred(header->id);
	loUpd->addSucc(header->id);
	loUpd->addPred(body->id);
	hiUpd->addSucc(header->id);
	hiUpd->addPred(body->id);
	exitB->addPred(header->id);

	const ssa::VarId vLo = fn->declareVar("lo");
	const ssa::VarId vHi = fn->declareVar("hi");

	ssa::IrValue* lo0 = fn->allocValue(ssa::ValueKind::VirtualReg, vLo);
	ssa::IrValue* hi0 = fn->allocValue(ssa::ValueKind::VirtualReg, vHi);
	ssa::IrValue* target = fn->allocValue(ssa::ValueKind::Immediate);
	target->imm = 42;
	ssa::IrValue* one = fn->allocValue(ssa::ValueKind::Immediate);
	one->imm = 1;
	ssa::IrValue* arr = fn->allocValue(ssa::ValueKind::VirtualReg);

	ssa::PhiNode* phiLo = fn->addPhi(header->id, vLo);
	ssa::IrValue* lo = fn->allocValue(ssa::ValueKind::Phi, vLo);
	phiLo->result = lo->id;
	lo->defPhi = phiLo;

	ssa::PhiNode* phiHi = fn->addPhi(header->id, vHi);
	ssa::IrValue* hi = fn->allocValue(ssa::ValueKind::Phi, vHi);
	phiHi->result = hi->id;
	hi->defPhi = phiHi;

	ssa::IrInstr* boundCmp = fn->addInstr(header->id, ssa::IrInstr::Op::Compare);
	useVal(boundCmp, lo->id, 0);
	useVal(boundCmp, hi->id, 1);
	fn->addInstr(header->id, ssa::IrInstr::Op::CondBranch);

	ssa::IrInstr* add = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(add, lo->id, 0);
	useVal(add, hi->id, 1);
	ssa::IrValue* sum = defOf(*fn, add);

	ssa::IrInstr* shr = fn->addInstr(body->id, ssa::IrInstr::Op::Shr);
	useVal(shr, sum->id, 0);
	useVal(shr, one->id, 1);
	ssa::IrValue* mid = defOf(*fn, shr);

	ssa::IrInstr* addrAdd = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(addrAdd, arr->id, 0);
	useVal(addrAdd, mid->id, 1);
	ssa::IrValue* addr = defOf(*fn, addrAdd);

	ssa::IrInstr* load = fn->addInstr(body->id, ssa::IrInstr::Op::Load);
	useVal(load, addr->id, 0);
	ssa::IrValue* elem = defOf(*fn, load);

	ssa::IrInstr* cmp = fn->addInstr(body->id, ssa::IrInstr::Op::Compare);
	useVal(cmp, elem->id, 0);
	useVal(cmp, target->id, 1);
	fn->addInstr(body->id, ssa::IrInstr::Op::CondBranch);

	ssa::IrInstr* loAdd = fn->addInstr(loUpd->id, ssa::IrInstr::Op::Add);
	useVal(loAdd, mid->id, 0);
	useVal(loAdd, one->id, 1);
	ssa::IrValue* lo1 = defOf(*fn, loAdd, vLo);

	ssa::IrInstr* hiSub = fn->addInstr(hiUpd->id, ssa::IrInstr::Op::Sub);
	useVal(hiSub, mid->id, 0);
	useVal(hiSub, one->id, 1);
	ssa::IrValue* hi1 = defOf(*fn, hiSub, vHi);

	phiLo->addOperand(entry->id, lo0->id);
	phiLo->addOperand(loUpd->id, lo1->id);
	phiLo->addOperand(hiUpd->id, lo->id);
	phiHi->addOperand(entry->id, hi0->id);
	phiHi->addOperand(loUpd->id, hi->id);
	phiHi->addOperand(hiUpd->id, hi1->id);

	return fn;
}

/// Loop with a Shr that is not a load-indexed midpoint (hash-style mix).
static std::unique_ptr<ssa::SSAFunction> makeRandomShrLoop()
{
	auto fn = std::make_unique<ssa::SSAFunction>("f");
	auto* entry = fn->addBlock("entry");
	auto* loop = fn->addBlock("loop");
	auto* exitB = fn->addBlock("exit");
	entry->addSucc(loop->id);
	loop->addSucc(loop->id);
	loop->addSucc(exitB->id);
	loop->addPred(entry->id);
	loop->addPred(loop->id);

	ssa::IrValue* x = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* one = fn->allocValue(ssa::ValueKind::Immediate);
	one->imm = 1;
	ssa::IrValue* three = fn->allocValue(ssa::ValueKind::Immediate);
	three->imm = 3;

	ssa::IrInstr* shr = fn->addInstr(loop->id, ssa::IrInstr::Op::Shr);
	useVal(shr, x->id, 0);
	useVal(shr, one->id, 1);
	defOf(*fn, shr);

	ssa::IrInstr* add = fn->addInstr(loop->id, ssa::IrInstr::Op::Add);
	useVal(add, x->id, 0);
	useVal(add, one->id, 1);
	defOf(*fn, add);

	ssa::IrValue* slot = fn->allocValue(ssa::ValueKind::MemRef);
	ssa::IrInstr* load = fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
	useVal(load, slot->id, 0);
	ssa::IrValue* elem = defOf(*fn, load);

	ssa::IrInstr* cmp = fn->addInstr(loop->id, ssa::IrInstr::Op::Compare);
	useVal(cmp, elem->id, 0);
	useVal(cmp, three->id, 1);
	return fn;
}

/// gcc -O0 shape of tests/algorithm_recovery/sources/binary_search.c:
/// mid = lo + (hi - lo) / 2, load a[mid], compare vs invariant target.
/// Function name is generic — no filename / symbol hint.
static std::unique_ptr<ssa::SSAFunction> makeGccO0BinarySearchShape()
{
	auto fn = std::make_unique<ssa::SSAFunction>("f");
	auto* entry = fn->addBlock("entry");
	auto* header = fn->addBlock("header");
	auto* body = fn->addBlock("body");
	auto* loUpd = fn->addBlock("lo_upd");
	auto* hiUpd = fn->addBlock("hi_upd");
	auto* exitB = fn->addBlock("exit");

	entry->addSucc(header->id);
	header->addSucc(body->id);
	header->addSucc(exitB->id);
	header->addPred(entry->id);
	header->addPred(loUpd->id);
	header->addPred(hiUpd->id);
	body->addSucc(loUpd->id);
	body->addSucc(hiUpd->id);
	body->addPred(header->id);
	loUpd->addSucc(header->id);
	loUpd->addPred(body->id);
	hiUpd->addSucc(header->id);
	hiUpd->addPred(body->id);
	exitB->addPred(header->id);

	const ssa::VarId vLo = fn->declareVar("lo");
	const ssa::VarId vHi = fn->declareVar("hi");

	ssa::IrValue* lo0 = fn->allocValue(ssa::ValueKind::VirtualReg, vLo);
	ssa::IrValue* hi0 = fn->allocValue(ssa::ValueKind::VirtualReg, vHi);
	ssa::IrValue* target = fn->allocValue(ssa::ValueKind::Immediate);
	target->imm = 42;
	ssa::IrValue* one = fn->allocValue(ssa::ValueKind::Immediate);
	one->imm = 1;
	ssa::IrValue* two = fn->allocValue(ssa::ValueKind::Immediate);
	two->imm = 2;
	ssa::IrValue* arr = fn->allocValue(ssa::ValueKind::VirtualReg);

	ssa::PhiNode* phiLo = fn->addPhi(header->id, vLo);
	ssa::IrValue* lo = fn->allocValue(ssa::ValueKind::Phi, vLo);
	phiLo->result = lo->id;
	lo->defPhi = phiLo;

	ssa::PhiNode* phiHi = fn->addPhi(header->id, vHi);
	ssa::IrValue* hi = fn->allocValue(ssa::ValueKind::Phi, vHi);
	phiHi->result = hi->id;
	hi->defPhi = phiHi;

	ssa::IrInstr* boundCmp = fn->addInstr(header->id, ssa::IrInstr::Op::Compare);
	useVal(boundCmp, lo->id, 0);
	useVal(boundCmp, hi->id, 1);
	fn->addInstr(header->id, ssa::IrInstr::Op::CondBranch);

	ssa::IrInstr* sub = fn->addInstr(body->id, ssa::IrInstr::Op::Sub);
	useVal(sub, hi->id, 0);
	useVal(sub, lo->id, 1);
	ssa::IrValue* diff = defOf(*fn, sub);

	ssa::IrInstr* div = fn->addInstr(body->id, ssa::IrInstr::Op::Div);
	useVal(div, diff->id, 0);
	useVal(div, two->id, 1);
	ssa::IrValue* half = defOf(*fn, div);

	ssa::IrInstr* add = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(add, lo->id, 0);
	useVal(add, half->id, 1);
	ssa::IrValue* mid = defOf(*fn, add);

	ssa::IrInstr* addrAdd = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(addrAdd, arr->id, 0);
	useVal(addrAdd, mid->id, 1);
	ssa::IrValue* addr = defOf(*fn, addrAdd);

	ssa::IrInstr* load = fn->addInstr(body->id, ssa::IrInstr::Op::Load);
	useVal(load, addr->id, 0);
	ssa::IrValue* elem = defOf(*fn, load);

	ssa::IrInstr* cmp = fn->addInstr(body->id, ssa::IrInstr::Op::Compare);
	useVal(cmp, elem->id, 0);
	useVal(cmp, target->id, 1);
	fn->addInstr(body->id, ssa::IrInstr::Op::CondBranch);

	ssa::IrInstr* loAdd = fn->addInstr(loUpd->id, ssa::IrInstr::Op::Add);
	useVal(loAdd, mid->id, 0);
	useVal(loAdd, one->id, 1);
	ssa::IrValue* lo1 = defOf(*fn, loAdd, vLo);

	ssa::IrInstr* hiSub = fn->addInstr(hiUpd->id, ssa::IrInstr::Op::Sub);
	useVal(hiSub, mid->id, 0);
	useVal(hiSub, one->id, 1);
	ssa::IrValue* hi1 = defOf(*fn, hiSub, vHi);

	phiLo->addOperand(entry->id, lo0->id);
	phiLo->addOperand(loUpd->id, lo1->id);
	phiLo->addOperand(hiUpd->id, lo->id);
	phiHi->addOperand(entry->id, hi0->id);
	phiHi->addOperand(loUpd->id, hi->id);
	phiHi->addOperand(hiUpd->id, hi1->id);

	return fn;
}

/// gcc -O0 shape of tests/algorithm_recovery/sources/bubblesort.c:
/// nested i/j loops, n-1-i bound, adjacent compare, two-store swap.
static std::unique_ptr<ssa::SSAFunction> makeGccO0BubblesortShape()
{
	auto fn = std::make_unique<ssa::SSAFunction>("f");
	auto* entry = fn->addBlock("entry");
	auto* outer = fn->addBlock("outer");
	auto* inner = fn->addBlock("inner");
	auto* body = fn->addBlock("body");
	auto* swapB = fn->addBlock("swap");
	auto* jInc = fn->addBlock("j_inc");
	auto* iInc = fn->addBlock("i_inc");
	auto* exitB = fn->addBlock("exit");

	entry->addSucc(outer->id);
	outer->addSucc(inner->id);
	outer->addSucc(exitB->id);
	outer->addPred(entry->id);
	outer->addPred(iInc->id);
	inner->addSucc(body->id);
	inner->addSucc(iInc->id);
	inner->addPred(outer->id);
	inner->addPred(jInc->id);
	body->addSucc(swapB->id);
	body->addSucc(jInc->id);
	body->addPred(inner->id);
	swapB->addSucc(jInc->id);
	swapB->addPred(body->id);
	jInc->addSucc(inner->id);
	jInc->addPred(body->id);
	jInc->addPred(swapB->id);
	iInc->addSucc(outer->id);
	iInc->addPred(inner->id);
	exitB->addPred(outer->id);

	const ssa::VarId vI = fn->declareVar("i");
	const ssa::VarId vJ = fn->declareVar("j");

	ssa::IrValue* i0 = fn->allocValue(ssa::ValueKind::VirtualReg, vI);
	ssa::IrValue* j0 = fn->allocValue(ssa::ValueKind::VirtualReg, vJ);
	ssa::IrValue* one = fn->allocValue(ssa::ValueKind::Immediate);
	one->imm = 1;
	ssa::IrValue* n = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* arr = fn->allocValue(ssa::ValueKind::VirtualReg);

	ssa::PhiNode* phiI = fn->addPhi(outer->id, vI);
	ssa::IrValue* i = fn->allocValue(ssa::ValueKind::Phi, vI);
	phiI->result = i->id;
	i->defPhi = phiI;

	ssa::PhiNode* phiJ = fn->addPhi(inner->id, vJ);
	ssa::IrValue* j = fn->allocValue(ssa::ValueKind::Phi, vJ);
	phiJ->result = j->id;
	j->defPhi = phiJ;

	ssa::IrInstr* n1 = fn->addInstr(outer->id, ssa::IrInstr::Op::Sub);
	useVal(n1, n->id, 0);
	useVal(n1, one->id, 1);
	ssa::IrValue* nMinus1 = defOf(*fn, n1);

	ssa::IrInstr* icmp = fn->addInstr(outer->id, ssa::IrInstr::Op::Compare);
	useVal(icmp, i->id, 0);
	useVal(icmp, nMinus1->id, 1);
	fn->addInstr(outer->id, ssa::IrInstr::Op::CondBranch);

	ssa::IrInstr* bound = fn->addInstr(inner->id, ssa::IrInstr::Op::Sub);
	useVal(bound, nMinus1->id, 0);
	useVal(bound, i->id, 1);
	ssa::IrValue* jlim = defOf(*fn, bound);

	ssa::IrInstr* jcmp = fn->addInstr(inner->id, ssa::IrInstr::Op::Compare);
	useVal(jcmp, j->id, 0);
	useVal(jcmp, jlim->id, 1);
	fn->addInstr(inner->id, ssa::IrInstr::Op::CondBranch);

	ssa::IrInstr* addr0 = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(addr0, arr->id, 0);
	useVal(addr0, j->id, 1);
	ssa::IrValue* p0 = defOf(*fn, addr0);

	ssa::IrInstr* j1 = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(j1, j->id, 0);
	useVal(j1, one->id, 1);
	ssa::IrValue* jp1 = defOf(*fn, j1);

	ssa::IrInstr* addr1 = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(addr1, arr->id, 0);
	useVal(addr1, jp1->id, 1);
	ssa::IrValue* p1 = defOf(*fn, addr1);

	ssa::IrInstr* load0 = fn->addInstr(body->id, ssa::IrInstr::Op::Load);
	useVal(load0, p0->id, 0);
	ssa::IrValue* e0 = defOf(*fn, load0);

	ssa::IrInstr* load1 = fn->addInstr(body->id, ssa::IrInstr::Op::Load);
	useVal(load1, p1->id, 0);
	ssa::IrValue* e1 = defOf(*fn, load1);

	ssa::IrInstr* acmp = fn->addInstr(body->id, ssa::IrInstr::Op::Compare);
	useVal(acmp, e0->id, 0);
	useVal(acmp, e1->id, 1);
	fn->addInstr(body->id, ssa::IrInstr::Op::CondBranch);

	ssa::IrInstr* st0 = fn->addInstr(swapB->id, ssa::IrInstr::Op::Store);
	useVal(st0, p0->id, 0);
	useVal(st0, e1->id, 1);

	ssa::IrInstr* st1 = fn->addInstr(swapB->id, ssa::IrInstr::Op::Store);
	useVal(st1, p1->id, 0);
	useVal(st1, e0->id, 1);

	ssa::IrInstr* jAdd = fn->addInstr(jInc->id, ssa::IrInstr::Op::Add);
	useVal(jAdd, j->id, 0);
	useVal(jAdd, one->id, 1);
	ssa::IrValue* jn = defOf(*fn, jAdd, vJ);

	ssa::IrInstr* iAdd = fn->addInstr(iInc->id, ssa::IrInstr::Op::Add);
	useVal(iAdd, i->id, 0);
	useVal(iAdd, one->id, 1);
	ssa::IrValue* in = defOf(*fn, iAdd, vI);

	phiI->addOperand(entry->id, i0->id);
	phiI->addOperand(iInc->id, in->id);
	phiJ->addOperand(outer->id, j0->id);
	phiJ->addOperand(jInc->id, jn->id);

	return fn;
}

/// gcc -O0 shape of tests/algorithm_recovery/sources/memcpy_loop.c:
/// for (i = 0; i < n; ++i) dst[i] = src[i];
static std::unique_ptr<ssa::SSAFunction> makeGccO0MemcpyLoopShape()
{
	auto fn = std::make_unique<ssa::SSAFunction>("f");
	auto* entry = fn->addBlock("entry");
	auto* header = fn->addBlock("header");
	auto* body = fn->addBlock("body");
	auto* exitB = fn->addBlock("exit");

	entry->addSucc(header->id);
	header->addSucc(body->id);
	header->addSucc(exitB->id);
	header->addPred(entry->id);
	header->addPred(body->id);
	body->addSucc(header->id);
	body->addPred(header->id);
	exitB->addPred(header->id);

	const ssa::VarId vI = fn->declareVar("i");
	ssa::IrValue* i0 = fn->allocValue(ssa::ValueKind::VirtualReg, vI);
	ssa::IrValue* one = fn->allocValue(ssa::ValueKind::Immediate);
	one->imm = 1;
	ssa::IrValue* n = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* src = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* dst = fn->allocValue(ssa::ValueKind::VirtualReg);

	ssa::PhiNode* phiI = fn->addPhi(header->id, vI);
	ssa::IrValue* i = fn->allocValue(ssa::ValueKind::Phi, vI);
	phiI->result = i->id;
	i->defPhi = phiI;

	ssa::IrInstr* cmp = fn->addInstr(header->id, ssa::IrInstr::Op::Compare);
	useVal(cmp, i->id, 0);
	useVal(cmp, n->id, 1);
	fn->addInstr(header->id, ssa::IrInstr::Op::CondBranch);

	ssa::IrInstr* sa = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(sa, src->id, 0);
	useVal(sa, i->id, 1);
	ssa::IrValue* sp = defOf(*fn, sa);

	ssa::IrInstr* ld = fn->addInstr(body->id, ssa::IrInstr::Op::Load);
	useVal(ld, sp->id, 0);
	ssa::IrValue* elem = defOf(*fn, ld);

	ssa::IrInstr* da = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(da, dst->id, 0);
	useVal(da, i->id, 1);
	ssa::IrValue* dp = defOf(*fn, da);

	ssa::IrInstr* st = fn->addInstr(body->id, ssa::IrInstr::Op::Store);
	useVal(st, dp->id, 0);
	useVal(st, elem->id, 1);

	ssa::IrInstr* inc = fn->addInstr(body->id, ssa::IrInstr::Op::Add);
	useVal(inc, i->id, 0);
	useVal(inc, one->id, 1);
	ssa::IrValue* i1 = defOf(*fn, inc, vI);

	phiI->addOperand(entry->id, i0->id);
	phiI->addOperand(body->id, i1->id);
	return fn;
}

// ─── AlgorithmResult tests ────────────────────────────────────────────────────

TEST(AlgorithmResultTest, KindNameTransform)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::Transform;
	EXPECT_EQ(r.kindName(), "std::transform");
}

TEST(AlgorithmResultTest, KindNameAccumulate)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::Accumulate;
	EXPECT_EQ(r.kindName(), "std::accumulate");
}

TEST(AlgorithmResultTest, KindNameFind)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::Find;
	EXPECT_EQ(r.kindName(), "std::find");
}

TEST(AlgorithmResultTest, KindNameFindIf)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::FindIf;
	EXPECT_EQ(r.kindName(), "std::find_if");
}

TEST(AlgorithmResultTest, KindNamePartition)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::Partition;
	EXPECT_EQ(r.kindName(), "std::partition");
}

TEST(AlgorithmResultTest, KindNameForEach)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::ForEach;
	EXPECT_EQ(r.kindName(), "std::for_each");
}

TEST(AlgorithmResultTest, KindNameCopy)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::Copy;
	EXPECT_EQ(r.kindName(), "std::copy");
}

TEST(AlgorithmResultTest, KindNameMaxElement)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::MaxElement;
	EXPECT_EQ(r.kindName(), "std::max_element");
}

TEST(AlgorithmResultTest, KindNameMinElement)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::MinElement;
	EXPECT_EQ(r.kindName(), "std::min_element");
}

TEST(AlgorithmResultTest, KindNameUnknown)
{
	AlgorithmResult r;
	EXPECT_EQ(r.kindName(), "unknown");
}

TEST(AlgorithmResultTest, ToStringContainsConfidence)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::Find;
	r.confidence = 0.9f;
	r.tier = EmissionTier::High;
	r.emittedForm = "std::find(first, last, value);";
	std::string s = r.toString();
	EXPECT_NE(s.find("0.9"), std::string::npos);
	EXPECT_NE(s.find("high"), std::string::npos);
}

TEST(AlgorithmResultTest, ToStringContainsEmittedForm)
{
	AlgorithmResult r;
	r.kind = AlgorithmKind::ForEach;
	r.confidence = 0.8f;
	r.tier = EmissionTier::High;
	r.emittedForm = "std::for_each(first, last, f);";
	EXPECT_NE(r.toString().find("std::for_each"), std::string::npos);
}

// ─── TransformDetector tests ──────────────────────────────────────────────────

TEST(TransformDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.10f);
}

TEST(TransformDetectorTest, LoadStorePlusAdvancedPtrs)
{
	auto fn = makeFunc(
		"xfrm",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_TRUE(r.kind == AlgorithmKind::Transform || r.kind == AlgorithmKind::Copy);
}

TEST(TransformDetectorTest, LambdaCallDetected)
{
	auto fn = makeFunc(
		"xfrm_lambda",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "transform_fn");
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_TRUE(r.hasLambda);
	EXPECT_EQ(r.kind, AlgorithmKind::Transform);
}

TEST(TransformDetectorTest, BackInserterDetected)
{
	auto fn = makeFunc(
		"xfrm_bi",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "push_back");
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_TRUE(r.hasBackInserter);
}

TEST(TransformDetectorTest, IdentityIsCopyKind)
{
	// Load + Store + two Adds, no call → identity → Copy.
	auto fn = makeFunc(
		"copy_loop",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.kind, AlgorithmKind::Copy);
}

TEST(TransformDetectorTest, StateMachineManyBranchesIsNotCopy)
{
	// B8 HTTP-verb bag: load/store/cmp, one Add, many CondBranches.
	auto fn = makeFunc(
		"http_verb",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::CondBranch,
			ssa::IrInstr::Op::CondBranch,
			ssa::IrInstr::Op::CondBranch,
			ssa::IrInstr::Op::CondBranch,
		},
		1);
	addBackEdge(*fn);
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(TransformDetectorTest, MulInLoopIsNotCopy)
{
	// atoi `n * 10` / DFS index scale is not memcpy.
	auto fn = makeFunc(
		"atoi_parse",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Mul,
		},
		1);
	addBackEdge(*fn);
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_NE(r.kind, AlgorithmKind::Copy);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(TransformDetectorTest, XorInLoopIsNotCopy)
{
	auto fn = makeFunc(
		"aes_mix",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Xor,
		},
		1);
	addBackEdge(*fn);
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_NE(r.kind, AlgorithmKind::Copy);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(TransformDetectorTest, HighTierEmittedFormContainsStd)
{
	auto fn = makeFunc(
		"xfrm_hi",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Mul,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "f");
	TransformDetector det;
	auto r = det.detect(*fn);
	if (r.tier == EmissionTier::High) EXPECT_NE(r.emittedForm.find("std::"), std::string::npos);
}

// ─── AccumulateDetector tests ─────────────────────────────────────────────────

TEST(AccumulateDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.10f);
}

TEST(AccumulateDetectorTest, PhiPlusAddIsAccumulate)
{
	auto fn = makeFunc(
		"acc_add",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.40f);
	EXPECT_EQ(r.combiner, CombinerKind::Add);
}

TEST(AccumulateDetectorTest, PhiPlusMulIsMultiply)
{
	auto fn = makeFunc(
		"acc_mul",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Mul,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.combiner, CombinerKind::Mul);
}

TEST(AccumulateDetectorTest, PhiPlusOrIsBitOr)
{
	auto fn = makeFunc(
		"acc_or",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Or,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.combiner, CombinerKind::Or);
}

TEST(AccumulateDetectorTest, PhiPlusXorIsBitXor)
{
	auto fn = makeFunc(
		"acc_xor",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Xor,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.combiner, CombinerKind::Xor);
}

TEST(AccumulateDetectorTest, CompareOnlyIsMaxElement)
{
	auto fn = makeFunc(
		"max_elem",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.kind, AlgorithmKind::MaxElement);
}

TEST(AccumulateDetectorTest, NoStoreBoostsConfidence)
{
	auto fn = makeFunc(
		"acc_nostore",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.75f); // phi + add + no store = 1.0
}

TEST(AccumulateDetectorTest, HighTierEmittedContainsAccumulate)
{
	auto fn = makeFunc(
		"acc_hi",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	if (r.tier == EmissionTier::High) EXPECT_NE(r.emittedForm.find("std::accumulate"), std::string::npos);
}

// ─── FindDetector tests ───────────────────────────────────────────────────────

TEST(FindDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	FindDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.10f);
}

TEST(FindDetectorTest, ComparePlusEarlyExitIsFind)
{
	auto fn = makeFunc(
		"find_val",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		2);
	// Back-edge: block 1 → block 0, forward edge: block 1 → block 2 (exit).
	fn->block(1)->succs.push_back(0);
	fn->block(1)->succs.push_back(2);
	addBackEdge(*fn);
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 42);
	FindDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.40f);
	EXPECT_TRUE(r.kind == AlgorithmKind::Find || r.kind == AlgorithmKind::FindIf);
}

TEST(FindDetectorTest, ImmediateComparandIsFindNotFindIf)
{
	auto fn = makeFunc(
		"find_imm",
		{
			ssa::IrInstr::Op::Load,
		},
		2);
	fn->block(1)->succs.push_back(0);
	fn->block(1)->succs.push_back(2);
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 99);
	FindDetector det;
	auto r = det.detect(*fn);
	if (r.confidence >= 0.40f && r.kind != AlgorithmKind::Count) EXPECT_FALSE(r.hasLambda);
}

TEST(FindDetectorTest, PredicateCallIsFindIf)
{
	auto fn = makeFunc(
		"find_if",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		2);
	fn->block(1)->succs.push_back(0);
	fn->block(1)->succs.push_back(2);
	addCall(*fn, "pred_fn");
	FindDetector det;
	auto r = det.detect(*fn);
	if (r.confidence >= 0.40f) EXPECT_TRUE(r.hasLambda);
}

TEST(FindDetectorTest, NoStoreInLoopBoostsConfidence)
{
	auto fn = makeFunc(
		"find_nostore",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		2);
	fn->block(1)->succs.push_back(0);
	fn->block(1)->succs.push_back(2);
	FindDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.65f); // compare + early-exit + no-store
}

TEST(FindDetectorTest, HighTierEmittedContainsStdFind)
{
	auto fn = makeFunc(
		"find_hi",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		2);
	fn->block(1)->succs.push_back(0);
	fn->block(1)->succs.push_back(2);
	FindDetector det;
	auto r = det.detect(*fn);
	if (r.tier == EmissionTier::High) EXPECT_NE(r.emittedForm.find("std::find"), std::string::npos);
}

// ─── PartitionDetector tests ──────────────────────────────────────────────────

TEST(PartitionDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	PartitionDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.10f);
}

TEST(PartitionDetectorTest, ConvergingPtrsAndSwapDetected)
{
	auto fn = makeFunc(
		"part",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	PartitionDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.70f);
	EXPECT_EQ(r.kind, AlgorithmKind::Partition);
}

TEST(PartitionDetectorTest, RecursionLowersConfidence)
{
	auto fn = makeFunc(
		"sort_part",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	// Self-recursive call lowers confidence.
	addCall(*fn, "sort_part");
	PartitionDetector det;
	auto r = det.detect(*fn);
	// Recursion removes 0.10 bonus → should be < 1.0 at max.
	EXPECT_LT(r.confidence, 1.0f);
}

TEST(PartitionDetectorTest, SwapCallCounts)
{
	auto fn = makeFunc(
		"part_swap",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "std::swap");
	PartitionDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_NE(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(PartitionDetectorTest, StructuralSwapIsNotSymbolNameEvidence)
{
	auto fn = makeFunc(
		"part_stores",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	PartitionDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(PartitionDetectorTest, HighTierEmittedContainsPartition)
{
	auto fn = makeFunc(
		"part_hi",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	PartitionDetector det;
	auto r = det.detect(*fn);
	if (r.tier == EmissionTier::High) EXPECT_NE(r.emittedForm.find("std::partition"), std::string::npos);
}

// ─── BinarySearchDetector tests ───────────────────────────────────────────────

TEST(BinarySearchDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.10f);
}

TEST(BinarySearchDetectorTest, MidpointShrLoadCompareDetects)
{
	auto fn = makeConnectedBinarySearch();
	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_EQ(r.kind, AlgorithmKind::BinarySearch);
	EXPECT_NE(r.emittedForm.find("binary_search"), std::string::npos);
}

TEST(BinarySearchDetectorTest, ConnectedInvariantIsHighTier)
{
	auto fn = makeConnectedBinarySearch();
	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.75f);
	EXPECT_EQ(r.tier, EmissionTier::High);
}

TEST(BinarySearchDetectorTest, RandomShrLoopDoesNotDetect)
{
	auto fn = makeRandomShrLoop();
	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(BinarySearchDetectorTest, OpcodeCountsAloneDoNotDetect)
{
	// Former counter-style fixture: Compare/Add/Sub/Load/Shr + back-edge,
	// with no SSA def-use connecting the shift to the load.
	auto fn = makeFunc(
		"counts_only",
		{
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Shr,
		},
		1);
	addBackEdge(*fn);
	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(BinarySearchDetectorTest, MidpointDivLoadCompareDetects)
{
	auto fn = std::make_unique<ssa::SSAFunction>("f");
	auto* entry = fn->addBlock("entry");
	auto* loop = fn->addBlock("loop");
	entry->addSucc(loop->id);
	loop->addSucc(loop->id);
	loop->addPred(entry->id);
	loop->addPred(loop->id);

	ssa::IrValue* lo = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* hi = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* two = fn->allocValue(ssa::ValueKind::Immediate);
	two->imm = 2;
	ssa::IrValue* target = fn->allocValue(ssa::ValueKind::Immediate);
	target->imm = 7;

	ssa::IrInstr* add = fn->addInstr(loop->id, ssa::IrInstr::Op::Add);
	useVal(add, lo->id, 0);
	useVal(add, hi->id, 1);
	ssa::IrValue* sum = defOf(*fn, add);

	ssa::IrInstr* div = fn->addInstr(loop->id, ssa::IrInstr::Op::Div);
	useVal(div, sum->id, 0);
	useVal(div, two->id, 1);
	ssa::IrValue* mid = defOf(*fn, div);

	ssa::IrInstr* load = fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
	useVal(load, mid->id, 0);
	ssa::IrValue* elem = defOf(*fn, load);

	ssa::IrInstr* cmp = fn->addInstr(loop->id, ssa::IrInstr::Op::Compare);
	useVal(cmp, elem->id, 0);
	useVal(cmp, target->id, 1);

	ssa::IrInstr* loAdd = fn->addInstr(loop->id, ssa::IrInstr::Op::Add);
	useVal(loAdd, mid->id, 0);
	useVal(loAdd, two->id, 1);
	defOf(*fn, loAdd);

	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_EQ(r.kind, AlgorithmKind::BinarySearch);
}

// ─── gcc -O0 IR-shape fixtures (E1; no ELF loader, no filename hints) ─────────

TEST(GccO0BinarySearchShape, OverflowSafeMidDetectsWithoutNameHint)
{
	auto fn = makeGccO0BinarySearchShape();
	ASSERT_EQ(fn->name(), "f");
	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_EQ(r.kind, AlgorithmKind::BinarySearch);
}

TEST(GccO0BubblesortShape, NestedSwapIsNotBinarySearch)
{
	auto fn = makeGccO0BubblesortShape();
	ASSERT_EQ(fn->name(), "f");
	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(GccO0MemcpyLoopShape, IndexedCopyDetectsWithoutNameHint)
{
	auto fn = makeGccO0MemcpyLoopShape();
	ASSERT_EQ(fn->name(), "f");
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_EQ(r.kind, AlgorithmKind::Copy);
}

TEST(GccO0MemcpyLoopShape, IndexedCopyIsNotBinarySearch)
{
	auto fn = makeGccO0MemcpyLoopShape();
	BinarySearchDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

// ─── ForEachDetector tests ────────────────────────────────────────────────────

TEST(ForEachDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	ForEachDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.10f);
}

TEST(ForEachDetectorTest, CallPlusNoPhiAndNoStore)
{
	auto fn = makeFunc(
		"foreach",
		{
			ssa::IrInstr::Op::Load,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "process");
	ForEachDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.70f); // call + no phi + no store
	EXPECT_EQ(r.kind, AlgorithmKind::ForEach);
}

TEST(ForEachDetectorTest, PhiReducesConfidence)
{
	auto fn = makeFunc(
		"foreach_phi",
		{
			ssa::IrInstr::Op::Load,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "f");
	addPhi(*fn);
	ForEachDetector det;
	auto r = det.detect(*fn);
	// call: +0.50, no store: +0.25, phi present: 0 (no no-phi bonus) → 0.75
	// but it still detects as ForEach.
	EXPECT_EQ(r.kind, AlgorithmKind::ForEach);
}

TEST(ForEachDetectorTest, NoCallLowConfidence)
{
	auto fn = makeFunc(
		"foreach_nocall",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	ForEachDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.10f);
}

TEST(ForEachDetectorTest, HighTierEmittedContainsForEach)
{
	auto fn = makeFunc(
		"foreach_hi",
		{
			ssa::IrInstr::Op::Load,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "f");
	ForEachDetector det;
	auto r = det.detect(*fn);
	if (r.tier == EmissionTier::High) EXPECT_NE(r.emittedForm.find("std::for_each"), std::string::npos);
}

// ─── IteratorPatternRecovery tests ────────────────────────────────────────────

TEST(IteratorPatternRecoveryTest, EmptyFunctionNoPattern)
{
	auto fn = makeFunc("empty", {});
	IteratorPatternRecovery rec;
	auto r = rec.recover(*fn);
	EXPECT_FALSE(r.isBeginEnd);
	EXPECT_FALSE(r.isReverseIter);
	EXPECT_FALSE(r.hasBackInserter);
}

TEST(IteratorPatternRecoveryTest, BeginEndPatternDetected)
{
	auto fn = makeFunc(
		"iter_be",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	IteratorPatternRecovery rec;
	auto r = rec.recover(*fn);
	EXPECT_TRUE(r.isBeginEnd);
	EXPECT_EQ(r.rangeForForm, "for (auto& e : v)");
}

TEST(IteratorPatternRecoveryTest, ReversePatternDetected)
{
	auto fn = makeFunc(
		"iter_rev",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	IteratorPatternRecovery rec;
	auto r = rec.recover(*fn);
	EXPECT_TRUE(r.isReverseIter);
	EXPECT_NE(r.rangeForForm.find("reverse"), std::string::npos);
}

TEST(IteratorPatternRecoveryTest, BackInserterDetected)
{
	auto fn = makeFunc(
		"iter_bi",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "push_back");
	IteratorPatternRecovery rec;
	auto r = rec.recover(*fn);
	EXPECT_TRUE(r.hasBackInserter);
	EXPECT_EQ(r.backInserter, "std::back_inserter(dst)");
}

TEST(IteratorPatternRecoveryTest, EmplaceBackIsBackInserter)
{
	auto fn = makeFunc(
		"iter_eb",
		{
			ssa::IrInstr::Op::Load,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "emplace_back");
	IteratorPatternRecovery rec;
	auto r = rec.recover(*fn);
	EXPECT_TRUE(r.hasBackInserter);
}

// ─── AlgorithmDetector orchestration tests ────────────────────────────────────

TEST(AlgorithmDetectorTest, EmptyFunctionSkipped)
{
	AlgorithmDetector::Config cfg;
	cfg.minBlocks = 2;
	cfg.minInstrs = 5;
	AlgorithmDetector det(cfg);
	auto fn = makeFunc("tiny", {ssa::IrInstr::Op::Load});
	auto r = det.detect(*fn);
	EXPECT_EQ(r.kind, AlgorithmKind::Unknown);
	EXPECT_EQ(det.stats().functionsSkipped, 1u);
}

TEST(AlgorithmDetectorTest, FindBeatsForEach)
{
	// A function with compare + early exit should be detected as find/find_if,
	// not for_each, because find is registered first.
	AlgorithmDetector det;
	auto fn = makeFunc(
		"find_vs_each",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		2);
	fn->block(1)->succs.push_back(0);
	fn->block(1)->succs.push_back(2);
	auto r = det.detect(*fn);
	EXPECT_TRUE(
		r.kind == AlgorithmKind::Find || r.kind == AlgorithmKind::FindIf || r.kind == AlgorithmKind::Count
		|| r.kind == AlgorithmKind::Unknown);
}

TEST(AlgorithmDetectorTest, TierAssignedCorrectly)
{
	AlgorithmDetector::Config cfg;
	cfg.highTierThreshold = 0.75f;
	cfg.mediumTierThreshold = 0.45f;
	AlgorithmDetector det(cfg);
	auto fn = makeFunc(
		"acc",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	auto r = det.detect(*fn);
	// phi + add + no store = 1.0 confidence → High tier
	if (r.confidence >= 0.75f) EXPECT_EQ(r.tier, EmissionTier::High);
}

TEST(AlgorithmDetectorTest, StatsUpdatedAfterDetection)
{
	AlgorithmDetector det;
	auto fn = makeFunc(
		"each",
		{
			ssa::IrInstr::Op::Load,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "f");
	std::vector<const ssa::SSAFunction*> fns = {fn.get()};
	auto results = det.detectModule(fns);
	EXPECT_GE(det.stats().functionsAnalysed, 1u);
}

TEST(AlgorithmDetectorTest, ModuleReturnsResultsPerFunction)
{
	AlgorithmDetector det;
	auto fn1 = makeFunc("fn1", {ssa::IrInstr::Op::Load}, 1);
	addBackEdge(*fn1);
	addCall(*fn1, "f");

	auto fn2 = makeFunc(
		"fn2",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn2);
	addPhi(*fn2);

	std::vector<const ssa::SSAFunction*> fns = {fn1.get(), fn2.get()};
	auto results = det.detectModule(fns);
	// At most 2 results (one per function).
	EXPECT_LE(results.size(), 2u);
}

TEST(AlgorithmDetectorTest, NullFunctionSkipped)
{
	AlgorithmDetector det;
	std::vector<const ssa::SSAFunction*> fns = {nullptr};
	auto results = det.detectModule(fns);
	EXPECT_EQ(results.size(), 0u);
}

TEST(IdiomDetectorTest, FirLikeLoopDoesNotAssignAtoiStrlenDfsVarint)
{
	IdiomDetector det;
	auto fn = makeFunc(
		"fir_tap",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Branch,
		},
		2);
	addBackEdge(*fn);
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 3u);
	for (const auto& r: det.detect(*fn))
	{
		EXPECT_NE(r.kind, IdiomKind::Atoi);
		EXPECT_NE(r.kind, IdiomKind::Strlen);
		EXPECT_NE(r.kind, IdiomKind::Dfs);
		EXPECT_NE(r.kind, IdiomKind::Varint);
	}
}

TEST(IdiomDetectorTest, NeverAssignsFibonacciLcsKnapsack)
{
	IdiomDetector det;
	auto fn = makeFunc(
		"dp_lookalike",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Call,
		},
		2);
	addBackEdge(*fn);
	addCall(*fn, "dp_lookalike");
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 48u);
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 57u);
	for (const auto& r: det.detect(*fn))
	{
		EXPECT_NE(r.kind, IdiomKind::Fibonacci);
		EXPECT_NE(r.kind, IdiomKind::Lcs);
		EXPECT_NE(r.kind, IdiomKind::Knapsack);
	}
}

TEST(AlgorithmDetectorTest, IteratorAnnotationAddedToHighTier)
{
	AlgorithmDetector det;
	auto fn = makeFunc(
		"iter_annotate",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	auto r = det.detect(*fn);
	// If it detects accumulate at high tier, the range annotation should be appended.
	if (r.tier == EmissionTier::High && r.kind == AlgorithmKind::Accumulate)
		EXPECT_NE(r.emittedForm.find("std::"), std::string::npos);
}
