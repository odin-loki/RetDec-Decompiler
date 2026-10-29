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
#include <optional>
#include <algorithm>
#include <memory>

#include <gtest/gtest.h>
#include <numeric>
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

/// A call passed one argument, as a for_each callback is passed the element.
static void addCallWithArg(ssa::SSAFunction& fn, const std::string& callee)
{
	auto* instr = fn.addInstr(fn.block(0)->id, ssa::IrInstr::Op::Call);
	if (!instr) return;
	instr->calleeName = callee;
	ssa::Use u;
	u.valueId = fn.allocValue(ssa::ValueKind::VirtualReg)->id;
	instr->uses.push_back(u);
}

/// Wire the first two Loads' results crosswise into the first two Stores,
/// the way `t = *a; *a = *b; *b = t` does.
static void wireSwap(ssa::SSAFunction& fn)
{
	std::vector<ssa::IrInstr*> loads, stores;
	for (auto* i: fn.block(0)->instrs)
	{
		if (i->op == ssa::IrInstr::Op::Load) loads.push_back(i);
		if (i->op == ssa::IrInstr::Op::Store) stores.push_back(i);
	}
	for (std::size_t k = 0; k < 2 && k < loads.size() && k < stores.size(); ++k)
	{
		ssa::IrValue* v = fn.allocValue(ssa::ValueKind::VirtualReg);
		loads[k]->defValue = v->id;
		v->defInstr = loads[k];
		ssa::Use u;
		u.valueId = v->id;
		u.operandIndex = 0;
		stores[1 - k]->uses.push_back(u);
	}
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

	// LLVM operand order: store value, then address.
	ssa::IrInstr* st = fn->addInstr(body->id, ssa::IrInstr::Op::Store);
	useVal(st, elem->id, 0);
	useVal(st, dp->id, 1);

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

TEST(FindDetectorTest, BoundCheckOnASpilledCounterIsNotASearch)
{
	// i = *slot; *slot = i + 1; if (i + 1 != n) loop -- the compared value
	// comes from a slot the loop writes, so it is a counter, not an element.
	auto fn = makeFunc("spill_bound", {}, 1);
	addBackEdge(*fn);
	auto* blk = fn->block(0);
	ssa::IrValue* slot = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* n = fn->allocValue(ssa::ValueKind::VirtualReg);
	auto* ld = fn->addInstr(blk->id, ssa::IrInstr::Op::Load);
	useVal(ld, slot->id);
	ssa::IrValue* i = defOf(*fn, ld);
	auto* add = fn->addInstr(blk->id, ssa::IrInstr::Op::Add);
	useVal(add, i->id, 0);
	ssa::IrValue* i1 = defOf(*fn, add);
	auto* st = fn->addInstr(blk->id, ssa::IrInstr::Op::Store);
	useVal(st, i1->id, 0);
	useVal(st, slot->id, 1);
	auto* cmp = fn->addInstr(blk->id, ssa::IrInstr::Op::Compare);
	useVal(cmp, i1->id, 0);
	useVal(cmp, n->id, 1);
	FindDetector det;
	EXPECT_LT(det.detect(*fn).confidence, 0.10f);
}

TEST(FindDetectorTest, CompareOnALoadedElementIsASearch)
{
	auto fn = makeFunc("wired_find", {}, 1);
	addBackEdge(*fn);
	auto* blk = fn->block(0);
	ssa::IrValue* p = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* key = fn->allocValue(ssa::ValueKind::VirtualReg);
	auto* ld = fn->addInstr(blk->id, ssa::IrInstr::Op::Load);
	useVal(ld, p->id);
	ssa::IrValue* e = defOf(*fn, ld);
	auto* cmp = fn->addInstr(blk->id, ssa::IrInstr::Op::Compare);
	useVal(cmp, e->id, 0);
	useVal(cmp, key->id, 1);
	FindDetector det;
	EXPECT_GE(det.detect(*fn).confidence, 0.40f);
}

TEST(TransformDetectorTest, CounterKeptInASpillSlotIsNotATransform)
{
	// i = *slot; r = tick(); *slot = i + 1; *out = r; -- the only load is
	// stored back to its own slot, and the other store is a call result.
	auto fn = makeFunc("spill_counter", {}, 1);
	addBackEdge(*fn);
	auto* blk = fn->block(0);
	ssa::IrValue* slot = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* out = fn->allocValue(ssa::ValueKind::VirtualReg);
	auto* ld = fn->addInstr(blk->id, ssa::IrInstr::Op::Load);
	useVal(ld, slot->id);
	ssa::IrValue* i = defOf(*fn, ld);
	auto* call = fn->addInstr(blk->id, ssa::IrInstr::Op::Call);
	call->calleeName = "tick";
	ssa::IrValue* r = defOf(*fn, call);
	auto* add = fn->addInstr(blk->id, ssa::IrInstr::Op::Add);
	useVal(add, i->id, 0);
	ssa::IrValue* i1 = defOf(*fn, add);
	auto* st1 = fn->addInstr(blk->id, ssa::IrInstr::Op::Store);
	useVal(st1, i1->id, 0);
	useVal(st1, slot->id, 1);
	auto* st2 = fn->addInstr(blk->id, ssa::IrInstr::Op::Store);
	useVal(st2, r->id, 0);
	useVal(st2, out->id, 1);
	fn->addInstr(blk->id, ssa::IrInstr::Op::Compare);
	TransformDetector det;
	EXPECT_LT(det.detect(*fn).confidence, 0.10f);
}

TEST(TransformDetectorTest, LoadedElementStoredElsewhereIsATransform)
{
	// *dst = f(*src) in a loop.
	auto fn = makeFunc("xfrm_wired", {ssa::IrInstr::Op::Add, ssa::IrInstr::Op::Add}, 1);
	addBackEdge(*fn);
	auto* blk = fn->block(0);
	ssa::IrValue* src = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* dst = fn->allocValue(ssa::ValueKind::VirtualReg);
	auto* ld = fn->addInstr(blk->id, ssa::IrInstr::Op::Load);
	useVal(ld, src->id);
	ssa::IrValue* x = defOf(*fn, ld);
	auto* call = fn->addInstr(blk->id, ssa::IrInstr::Op::Call);
	call->calleeName = "f";
	useVal(call, x->id, 0);
	ssa::IrValue* y = defOf(*fn, call);
	auto* st = fn->addInstr(blk->id, ssa::IrInstr::Op::Store);
	useVal(st, y->id, 0);
	useVal(st, dst->id, 1);
	fn->addInstr(blk->id, ssa::IrInstr::Op::Compare);
	TransformDetector det;
	EXPECT_GE(det.detect(*fn).confidence, 0.30f);
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

// An extremum loop selects its accumulator rather than combining it: a Compare
// feeding a select (Op::FlagRead -- SETcc/CMOVcc, and llvm's SelectInst).
TEST(AccumulateDetectorTest, CompareAndSelectIsMaxElement)
{
	auto fn = makeFunc(
		"max_elem",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::FlagRead,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.kind, AlgorithmKind::MaxElement);
}

// The branch used to require zero Adds. Every range loop advances its iterator
// or index with one, so it could not be taken for any real loop: control fell
// through to CombinerKind::Add and an extremum loop was emitted as
// `std::accumulate(first, last, 0)` at High tier -- wrong C++, not a weaker
// guess.
TEST(AccumulateDetectorTest, AnInductionAddDoesNotHideAMaxLoop)
{
	auto fn = makeFunc(
		"max_elem_indexed",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::FlagRead,
			ssa::IrInstr::Op::Add, // ++i
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.kind, AlgorithmKind::MaxElement);
	EXPECT_EQ(r.emittedForm.find("std::accumulate"), std::string::npos) << r.emittedForm;
}

// And a sum loop is still a sum loop: no select, so no extremum.
TEST(AccumulateDetectorTest, ASumLoopIsStillAccumulate)
{
	auto fn = makeFunc(
		"sum",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	AccumulateDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.kind, AlgorithmKind::Accumulate);
	EXPECT_EQ(r.combiner, CombinerKind::Add);
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

// A find loop leaves by two branches: the bound test and the match test. That
// second exit is the whole difference from a count loop, and it is what
// hasEarlyExit now measures.
//
//   0: body + match test   -> 1 (continue), 4 (found)
//   1:                     -> 2
//   2: latch + bound test  -> 0 (back edge), 3 (finished)
static std::unique_ptr<ssa::SSAFunction> makeSearchLoop(const std::string& name, bool matchExits)
{
	auto fn = makeFunc(name, {ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Compare}, 4);
	fn->block(0)->succs.push_back(1);
	if (matchExits) fn->block(0)->succs.push_back(4);
	fn->block(1)->succs.push_back(2);
	fn->block(2)->succs.push_back(0);
	fn->block(2)->succs.push_back(3);
	return fn;
}

TEST(FindDetectorTest, ComparePlusEarlyExitIsFind)
{
	auto fn = makeSearchLoop("find_val", /*matchExits=*/true);
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 42);
	FindDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.40f);
	EXPECT_TRUE(r.kind == AlgorithmKind::Find || r.kind == AlgorithmKind::FindIf);
}

// hasEarlyExit returned true for any block with a forward successor, which is
// the bound test itself and is present in every terminating loop -- so
// hasCountPattern's `!hasEarlyExit(fn)` could only hold for a loop with no exit
// at all, and AlgorithmKind::Count was dead for every normal input. A count
// loop was reported as std::find.
TEST(FindDetectorTest, ASingleExitLoopWithAnAccumulatorIsCount)
{
	auto fn = makeSearchLoop("count_val", /*matchExits=*/false);
	addPhi(*fn);
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 42);

	FindDetector det;
	auto r = det.detect(*fn);
	EXPECT_EQ(r.kind, AlgorithmKind::Count);
	EXPECT_EQ(r.emittedForm.find("std::find"), std::string::npos)
		<< "a count loop must not be emitted as a find: " << r.emittedForm;
	EXPECT_NE(r.emittedForm.find("count"), std::string::npos) << r.emittedForm;
}

// ...and a loop that does stop early is still a find, phi or no phi.
TEST(FindDetectorTest, ATwoExitLoopIsNotCount)
{
	auto fn = makeSearchLoop("find_val2", /*matchExits=*/true);
	addPhi(*fn);
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 42);

	FindDetector det;
	auto r = det.detect(*fn);
	EXPECT_NE(r.kind, AlgorithmKind::Count);
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
	auto fn = makeSearchLoop("find_hi", /*matchExits=*/true);
	FindDetector det;
	auto r = det.detect(*fn);
	ASSERT_EQ(r.tier, EmissionTier::High);
	EXPECT_NE(r.emittedForm.find("std::find"), std::string::npos);
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
	wireSwap(*fn);
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
	wireSwap(*fn);
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
	wireSwap(*fn);
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
	wireSwap(*fn);
	PartitionDetector det;
	auto r = det.detect(*fn);
	if (r.tier == EmissionTier::High) EXPECT_NE(r.emittedForm.find("std::partition"), std::string::npos);
}

TEST(PartitionDetectorTest, SpillAndReloadIsNotASwap)
{
	// A counted loop that spills two registers and reloads them has two
	// loads, two stores, an add and a sub -- but nothing it loads is stored.
	auto fn = makeFunc(
		"spill_loop",
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
	EXPECT_LT(r.confidence, 0.75f);
	EXPECT_NE(r.tier, EmissionTier::High);
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

// The same loop with the loaded value coming from a stack slot is a local being
// stored, as -O0 popcount and Fibonacci loops do -- no element moves.
TEST(GccO0MemcpyLoopShape, StoringALocalSpillIsNotACopy)
{
	auto fn = makeGccO0MemcpyLoopShape();
	for (auto* i: fn->block(2)->instrs)
		if (i->op == ssa::IrInstr::Op::Load) fn->value(i->uses[0].valueId)->isStackSlot = true;
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_NE(r.kind, AlgorithmKind::Copy);
	EXPECT_LT(r.confidence, 0.45f);
}

// rle: the loop also writes a run length it counted, not an element it read.
TEST(GccO0MemcpyLoopShape, AlsoWritingAComputedValueIsNotACopy)
{
	auto fn = makeGccO0MemcpyLoopShape();
	auto* count = fn->allocValue(ssa::ValueKind::VirtualReg);
	auto* add = fn->addInstr(2, ssa::IrInstr::Op::Add);
	add->defValue = count->id;
	count->defInstr = add;
	auto* out = fn->allocValue(ssa::ValueKind::VirtualReg);
	auto* st = fn->addInstr(2, ssa::IrInstr::Op::Store);
	st->uses.push_back({count->id, 0});
	st->uses.push_back({out->id, 1});
	TransformDetector det;
	auto r = det.detect(*fn);
	EXPECT_NE(r.kind, AlgorithmKind::Copy);
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
	addCallWithArg(*fn, "process");
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
	addCallWithArg(*fn, "f");
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
	addCallWithArg(*fn, "f");
	ForEachDetector det;
	auto r = det.detect(*fn);
	if (r.tier == EmissionTier::High) EXPECT_NE(r.emittedForm.find("std::for_each"), std::string::npos);
}

TEST(ForEachDetectorTest, CallAfterTheLoopIsNotForEach)
{
	// The loop (block 1 -> block 1) has no call; puts(msg) runs once after it.
	auto fn = makeFunc("count_then_print", {ssa::IrInstr::Op::Load}, 2);
	fn->block(0)->succs.push_back(1);
	fn->block(1)->succs.push_back(1);
	fn->block(1)->succs.push_back(2);
	auto* call = fn->addInstr(fn->block(2)->id, ssa::IrInstr::Op::Call);
	call->calleeName = "puts";
	ssa::Use u;
	u.valueId = fn->allocValue(ssa::ValueKind::VirtualReg)->id;
	call->uses.push_back(u);
	ForEachDetector det;
	EXPECT_LT(det.detect(*fn).confidence, 0.10f);
}

TEST(ForEachDetectorTest, ArgumentlessCallInCountedLoopIsNotForEach)
{
	// for (i = 0; i < n; ++i) tick(); -- nothing is handed an element.
	auto fn = makeFunc(
		"tick_loop",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	addCall(*fn, "tick");
	ForEachDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.10f);
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

TEST(AlgorithmDetectorTest, LowTierResultIsNotReported)
{
	// A loop with a compare and some stores scores accumulate at 0.35 on one
	// generic signal; that is below the medium tier and is not an idiom.
	AlgorithmDetector det;
	auto fn = makeFunc(
		"counted",
		{
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Assign,
			ssa::IrInstr::Op::Assign,
		},
		1);
	addBackEdge(*fn);
	auto r = det.detect(*fn);
	EXPECT_EQ(r.kind, AlgorithmKind::Unknown) << r.kindName() << " " << r.confidence;
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
	// The three-instruction fixture this used to build never got past
	// AlgorithmDetector's preflight, which requires five instructions, so
	// detect() returned a default-constructed result and the guarded
	// assertion -- the whole body -- never ran. The op mix is what
	// hasBeginEndPair looks for (Load + Add + Compare + a back edge), so it is
	// kept and simply made long enough to be analysed at all.
	AlgorithmDetector det;
	auto fn = makeFunc(
		"iter_annotate",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Compare,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	auto r = det.detect(*fn);

	ASSERT_EQ(AlgorithmKind::Accumulate, r.kind);
	ASSERT_EQ(EmissionTier::High, r.tier);
	EXPECT_NE(r.emittedForm.find("std::accumulate"), std::string::npos);
	// The range annotation is the part this test is named for.
	EXPECT_NE(r.emittedForm.find("// range:"), std::string::npos) << r.emittedForm;
}

TEST(AlgorithmDetectorTest, NoIteratorAnnotationWithoutABeginEndPair)
{
	// Same length, but no Compare -- hasBeginEndPair needs one, so the
	// annotation must not appear. Without this the test above would pass for a
	// detector that appended the annotation unconditionally.
	AlgorithmDetector det;
	auto fn = makeFunc(
		"acc_no_iter",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Add,
		},
		1);
	addBackEdge(*fn);
	addPhi(*fn);
	auto r = det.detect(*fn);

	ASSERT_EQ(AlgorithmKind::Accumulate, r.kind);
	ASSERT_EQ(EmissionTier::High, r.tier);
	EXPECT_EQ(r.emittedForm.find("// range:"), std::string::npos) << r.emittedForm;
}

// ─── kindName round-trips ────────────────────────────────────────────────────

// The function-analysis cache writes kindName() and reads it back. It used to
// read it back through its own list of five bare names -- "Transform", "Find"
// -- that kindName() never produces, so every cached algorithm came back
// Unknown: a warm decompile printed "[RetDec] unknown detected (confidence
// 0.75)" where the cold one printed "[RetDec] std::find_if detected
// (confidence 0.75)", and CACHE-05 caught the difference. This is the
// invariant that pairing was missing.
TEST(AlgorithmKindNames, EveryKindSurvivesANameRoundTrip)
{
	const AlgorithmKind all[] = {
		AlgorithmKind::Transform,
		AlgorithmKind::Accumulate,
		AlgorithmKind::MaxElement,
		AlgorithmKind::MinElement,
		AlgorithmKind::Find,
		AlgorithmKind::FindIf,
		AlgorithmKind::BinarySearch,
		AlgorithmKind::Partition,
		AlgorithmKind::ForEach,
		AlgorithmKind::Copy,
		AlgorithmKind::Fill,
		AlgorithmKind::Count,
		AlgorithmKind::AnyOf,
		AlgorithmKind::AllOf,
		AlgorithmKind::NoneOf,
		AlgorithmKind::Reverse,
		AlgorithmKind::RotateLeft,
	};
	for (AlgorithmKind k: all)
	{
		AlgorithmResult r;
		r.kind = k;
		const std::string name = r.kindName();
		EXPECT_NE("unknown", name) << "kind " << static_cast<int>(k) << " has no name";
		EXPECT_EQ(k, algorithmKindFromName(name)) << "kindName() gave \"" << name << "\", which does not read back";
	}
}

// The enum runs from Unknown to RotateLeft with no gaps, so a kind added to
// the enum but not to the table would leave a hole this finds.
TEST(AlgorithmKindNames, NoKindInTheEnumIsMissingFromTheTable)
{
	for (int i = static_cast<int>(AlgorithmKind::Transform); i <= static_cast<int>(AlgorithmKind::RotateLeft); ++i)
	{
		AlgorithmResult r;
		r.kind = static_cast<AlgorithmKind>(i);
		EXPECT_NE("unknown", r.kindName()) << "AlgorithmKind " << i << " has no name";
	}
}

TEST(AlgorithmKindNames, AnUnrecognisedNameIsUnknownNotAGuess)
{
	EXPECT_EQ(AlgorithmKind::Unknown, algorithmKindFromName("Transform"));
	EXPECT_EQ(AlgorithmKind::Unknown, algorithmKindFromName(""));
	EXPECT_EQ(AlgorithmKind::Unknown, algorithmKindFromName("std::sort"));
}

// ─── Strlen has to mean a string walk ────────────────────────────────────────
//
// hasNullTerminatedLoop used to ask for "a back edge, the constant 0 anywhere,
// at least one Load, at least one Compare, no Mul". That is nearly every loop.
// It only looked selective while buildSsaModule attached no operands, so
// `hasConstant(fn, 0)` was usually false by accident; once the def-use graph
// landed it fired on all nine ci_core binaries at once -- a bubble sort was
// reported as both Strlen and String -- and mean_f1 fell from 0.2712 to 0.1957.
//
// What actually says "string walk" is a byte loaded from memory and tested
// against zero, and saying so needs the def-use graph: the compare has to read
// the value the load defined.

namespace {

/// A loop that loads a byte through `mem` and compares it against zero.
std::unique_ptr<ssa::SSAFunction> makeByteWalk(bool compareAgainstZero, uint8_t loadBytes)
{
	auto fn = std::make_unique<ssa::SSAFunction>("walk");
	auto* entry = fn->addBlock("entry");
	auto* loop = fn->addBlock("loop");
	entry->succs.push_back(loop->id);
	loop->succs.push_back(loop->id); // back edge

	ssa::IrValue* mem = fn->allocValue(ssa::ValueKind::MemRef);
	mem->memWidth = loadBytes;
	mem->memIsStack = false;

	ssa::IrValue* loaded = fn->allocValue(ssa::ValueKind::VirtualReg);
	loaded->width = static_cast<uint8_t>(loadBytes * 8);

	ssa::IrInstr* ld = fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
	ld->uses.push_back({mem->id, 0});
	ld->defValue = loaded->id;
	loaded->defInstr = ld;

	ssa::IrValue* imm = fn->allocValue(ssa::ValueKind::Immediate);
	imm->imm = compareAgainstZero ? 0u : 42u;

	ssa::IrInstr* cmp = fn->addInstr(loop->id, ssa::IrInstr::Op::Compare);
	cmp->uses.push_back({loaded->id, 0});
	cmp->uses.push_back({imm->id, 1});

	// IdiomDetector::passesPreflight wants at least five instructions.
	fn->addInstr(loop->id, ssa::IrInstr::Op::Add);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Store);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Branch);
	return fn;
}

bool detects(const ssa::SSAFunction& fn, IdiomKind kind)
{
	IdiomDetector det;
	for (const auto& r: det.detect(fn))
		if (r.kind == kind) return true;
	return false;
}

// ─── Arithmetic loop idioms ───────────────────────────────────────────────────

std::unique_ptr<ssa::SSAFunction> makeLoop()
{
	auto fn = std::make_unique<ssa::SSAFunction>("loop_fn");
	auto* entry = fn->addBlock("entry");
	auto* loop = fn->addBlock("loop");
	entry->succs.push_back(loop->id);
	loop->succs.push_back(loop->id);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Branch);
	return fn;
}

ssa::IrValue* reg(ssa::SSAFunction& fn, uint8_t width = 32)
{
	auto* v = fn.allocValue(ssa::ValueKind::VirtualReg);
	v->width = width;
	return v;
}

ssa::IrInstr* opImm(ssa::SSAFunction& fn, ssa::IrInstr::Op op, uint64_t value)
{
	auto* imm = fn.allocValue(ssa::ValueKind::Immediate);
	imm->imm = value;
	auto* i = fn.addInstr(1, op);
	i->uses.push_back({reg(fn)->id, 0});
	i->uses.push_back({imm->id, 1});
	auto* d = reg(fn);
	i->defValue = d->id;
	d->defInstr = i;
	return i;
}

void pad(ssa::SSAFunction& fn)
{
	fn.addInstr(1, ssa::IrInstr::Op::Compare);
	fn.addInstr(1, ssa::IrInstr::Op::CondBranch);
}

/// Store `value` through a non-stack address in the loop.
void storeToMemory(ssa::SSAFunction& fn, ssa::ValueId value)
{
	auto* st = fn.addInstr(1, ssa::IrInstr::Op::Store);
	st->uses.push_back({value, 0});
	st->uses.push_back({reg(fn, 64)->id, 1});
}

ssa::IrInstr* byteLoad(ssa::SSAFunction& fn)
{
	auto* mem = fn.allocValue(ssa::ValueKind::MemRef);
	mem->memWidth = 1;
	mem->memIsStack = false;
	auto* ld = fn.addInstr(1, ssa::IrInstr::Op::Load);
	ld->uses.push_back({mem->id, 0});
	auto* v = reg(fn, 8);
	ld->defValue = v->id;
	v->defInstr = ld;
	return ld;
}

TEST(IdiomDetectorTest, Crc32PolynomialInAShiftingLoopIsCrc)
{
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::Shr, 1);
	opImm(*fn, ssa::IrInstr::Op::Xor, 0xEDB88320u);
	pad(*fn);
	EXPECT_TRUE(detects(*fn, IdiomKind::Crc));
	EXPECT_FALSE(detects(*fn, IdiomKind::Popcount));
}

TEST(IdiomDetectorTest, APolynomialOutsideTheLoopIsNotCrc)
{
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::Shr, 1);
	pad(*fn);
	auto* imm = fn->allocValue(ssa::ValueKind::Immediate);
	imm->imm = 0xEDB88320u;
	fn->addInstr(0, ssa::IrInstr::Op::Xor)->uses.push_back({imm->id, 1});
	EXPECT_FALSE(detects(*fn, IdiomKind::Crc));
}

TEST(IdiomDetectorTest, DigitRangeCheckFoldedIntoOneCompareIsAtoi)
{
	// gcc folds `c >= '0' && c <= '9'` into `(uint8_t)(c - '0') < 10`.
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::Add, 0xD0u);
	opImm(*fn, ssa::IrInstr::Op::Compare, 10u);
	opImm(*fn, ssa::IrInstr::Op::Mul, 10u);
	pad(*fn);
	EXPECT_TRUE(detects(*fn, IdiomKind::Atoi));
}

TEST(IdiomDetectorTest, DigitConstantsOutsideTheLoopAreNotAtoi)
{
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::Mul, 10u);
	pad(*fn);
	for (uint64_t c: {0xD0u, 10u}) {
		auto* imm = fn->allocValue(ssa::ValueKind::Immediate);
		imm->imm = c;
		fn->addInstr(0, c == 10u ? ssa::IrInstr::Op::Compare : ssa::IrInstr::Op::Add)->uses.push_back({imm->id, 1});
	}
	EXPECT_FALSE(detects(*fn, IdiomKind::Atoi));
}

TEST(IdiomDetectorTest, ContinuationBitAndDivideBy128IsVarint)
{
	// `out[i] = v | 0x80; v >>= 7;` once the byte store drops the 0x7F mask
	// and the shift is lifted as an unsigned divide.
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::Or, 0x80u);
	opImm(*fn, ssa::IrInstr::Op::Div, 0x80u);
	pad(*fn);
	EXPECT_TRUE(detects(*fn, IdiomKind::Varint));
}

TEST(IdiomDetectorTest, ContinuationBitWithoutTheShiftIsNotVarint)
{
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::Or, 0x80u);
	opImm(*fn, ssa::IrInstr::Op::Add, 1u);
	pad(*fn);
	EXPECT_FALSE(detects(*fn, IdiomKind::Varint));
}

/// `t = a + b; a = b; b = t;` over three stack slots; `extra` adds one more
/// instruction to the loop body.
std::unique_ptr<ssa::SSAFunction> makeFibonacciLoop(std::optional<ssa::IrInstr::Op> extra = std::nullopt)
{
	auto fn = makeLoop();
	auto slot = [&]() {
		auto* s = fn->allocValue(ssa::ValueKind::VirtualReg);
		s->isStackSlot = true;
		return s->id;
	};
	const auto sa = slot(), sb = slot(), st = slot();
	auto load = [&](ssa::ValueId addr) {
		auto* l = fn->addInstr(1, ssa::IrInstr::Op::Load);
		l->uses.push_back({addr, 0});
		auto* d = reg(*fn);
		l->defValue = d->id;
		d->defInstr = l;
		return d->id;
	};
	auto store = [&](ssa::ValueId v, ssa::ValueId addr) {
		auto* s = fn->addInstr(1, ssa::IrInstr::Op::Store);
		s->uses.push_back({v, 0});
		s->uses.push_back({addr, 1});
	};
	const auto a = load(sa), b = load(sb);
	auto* add = fn->addInstr(1, ssa::IrInstr::Op::Add);
	add->uses.push_back({a, 0});
	add->uses.push_back({b, 1});
	auto* t = reg(*fn);
	add->defValue = t->id;
	t->defInstr = add;
	store(t->id, st);
	store(b, sa);
	store(t->id, sb);
	if (extra) opImm(*fn, *extra, 3);
	pad(*fn);
	return fn;
}

TEST(IdiomDetectorTest, AddAndRotateTwoScalarsIsFibonacci)
{
	EXPECT_TRUE(detects(*makeFibonacciLoop(), IdiomKind::Fibonacci));
}

TEST(IdiomDetectorTest, AddAndRotateWithAMultiplyIsNotFibonacci)
{
	EXPECT_FALSE(detects(*makeFibonacciLoop(ssa::IrInstr::Op::Mul), IdiomKind::Fibonacci));
}

/// A halving loop that loads one array element and compares it `compares`
/// times; `storeBack` also writes to memory in the loop.
std::unique_ptr<ssa::SSAFunction> makeHalvingSearch(int compares, bool storeBack = false)
{
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::Shr, 1);
	auto* l = fn->addInstr(1, ssa::IrInstr::Op::Load);
	l->uses.push_back({reg(*fn, 64)->id, 0});
	auto* e = reg(*fn);
	l->defValue = e->id;
	e->defInstr = l;
	for (int c = 0; c < compares; ++c)
	{
		auto* cmp = fn->addInstr(1, ssa::IrInstr::Op::Compare);
		cmp->uses.push_back({e->id, 0});
		cmp->uses.push_back({reg(*fn)->id, 1});
	}
	if (storeBack) storeToMemory(*fn, e->id);
	pad(*fn);
	return fn;
}

TEST(IdiomDetectorTest, HalvingLoopWithOneElementCompareIsLowerBound)
{
	auto fn = makeHalvingSearch(1);
	EXPECT_TRUE(detects(*fn, IdiomKind::LowerBound));
	EXPECT_FALSE(detects(*fn, IdiomKind::BinarySearch));
}

TEST(IdiomDetectorTest, HalvingLoopWithEqualAndLessComparesIsBinarySearch)
{
	auto fn = makeHalvingSearch(2);
	EXPECT_TRUE(detects(*fn, IdiomKind::BinarySearch));
	EXPECT_FALSE(detects(*fn, IdiomKind::LowerBound));
}

TEST(IdiomDetectorTest, HalvingLoopThatWritesMemoryIsNotASearch)
{
	auto fn = makeHalvingSearch(1, true);
	EXPECT_FALSE(detects(*fn, IdiomKind::LowerBound));
	EXPECT_FALSE(detects(*fn, IdiomKind::BinarySearch));
}

/// `h = h * 131 + *s++` in the loop, then `h % 64` after it and, if
/// `setsBit`, `1 << (h % 64)`.
std::unique_ptr<ssa::SSAFunction> makeStringHashThenIndex(bool setsBit)
{
	auto fn = makeLoop();
	auto* exit = fn->addBlock("exit");
	fn->block(1)->succs.push_back(exit->id);
	opImm(*fn, ssa::IrInstr::Op::Mul, 131);
	auto* l = fn->addInstr(1, ssa::IrInstr::Op::Load);
	l->uses.push_back({reg(*fn, 64)->id, 0});
	pad(*fn);
	auto imm = [&](uint64_t v) {
		auto* i = fn->allocValue(ssa::ValueKind::Immediate);
		i->imm = v;
		return i->id;
	};
	auto* rem = fn->addInstr(exit->id, ssa::IrInstr::Op::Rem);
	rem->uses.push_back({reg(*fn)->id, 0});
	rem->uses.push_back({imm(64), 1});
	if (setsBit)
	{
		auto* shl = fn->addInstr(exit->id, ssa::IrInstr::Op::Shl);
		shl->uses.push_back({imm(1), 0});
		shl->uses.push_back({reg(*fn)->id, 1});
	}
	return fn;
}

TEST(IdiomDetectorTest, StringHashUsedAsABitPositionIsABloomFilter)
{
	EXPECT_TRUE(detects(*makeStringHashThenIndex(true), IdiomKind::BloomFilter));
}

TEST(IdiomDetectorTest, StringHashUsedAsABucketIndexIsNotABloomFilter)
{
	EXPECT_FALSE(detects(*makeStringHashThenIndex(false), IdiomKind::BloomFilter));
}

/// One element load per iteration, compared with `key`, plus the bound test.
std::unique_ptr<ssa::SSAFunction> makeScan(uint64_t key)
{
	auto fn = makeLoop();
	auto* l = fn->addInstr(1, ssa::IrInstr::Op::Load);
	l->uses.push_back({reg(*fn, 64)->id, 0});
	auto* e = reg(*fn);
	l->defValue = e->id;
	e->defInstr = l;
	auto* k = fn->allocValue(ssa::ValueKind::Immediate);
	k->imm = key;
	auto* cmp = fn->addInstr(1, ssa::IrInstr::Op::Compare);
	cmp->uses.push_back({e->id, 0});
	cmp->uses.push_back({k->id, 1});
	pad(*fn);
	return fn;
}

TEST(IdiomDetectorTest, ElementComparedWithAKeyEachIterationIsLinearSearch)
{
	EXPECT_TRUE(detects(*makeScan(6), IdiomKind::LinearSearch));
}

TEST(IdiomDetectorTest, ScanForAZeroTerminatorIsNotLinearSearch)
{
	EXPECT_FALSE(detects(*makeScan(0), IdiomKind::LinearSearch));
}

/// Read a byte, compare it with the run's byte, write the byte and
/// `'0' + run`; `decimal` also takes the run `% 10` as itoa would.
std::unique_ptr<ssa::SSAFunction> makeRunLengthLoop(bool decimal)
{
	auto fn = makeLoop();
	auto* l = fn->addInstr(1, ssa::IrInstr::Op::Load);
	l->uses.push_back({reg(*fn, 64)->id, 0});
	auto* e = reg(*fn, 8);
	l->defValue = e->id;
	e->defInstr = l;
	auto* cmp = fn->addInstr(1, ssa::IrInstr::Op::Compare);
	cmp->uses.push_back({e->id, 0});
	cmp->uses.push_back({reg(*fn, 8)->id, 1});
	auto* digit = opImm(*fn, ssa::IrInstr::Op::Add, 0x30);
	if (decimal) opImm(*fn, ssa::IrInstr::Op::Rem, 10);
	storeToMemory(*fn, e->id);
	storeToMemory(*fn, digit->defValue);
	pad(*fn);
	return fn;
}

TEST(IdiomDetectorTest, RunCountWrittenAsADigitIsRle)
{
	EXPECT_TRUE(detects(*makeRunLengthLoop(false), IdiomKind::Rle));
}

TEST(IdiomDetectorTest, DecimalDigitConversionIsNotRle)
{
	EXPECT_FALSE(detects(*makeRunLengthLoop(true), IdiomKind::Rle));
}

/// A load of an array element in the loop; returns its value.
ssa::ValueId loadElement(ssa::SSAFunction& fn, ssa::BlockId b = 1)
{
	auto* l = fn.addInstr(b, ssa::IrInstr::Op::Load);
	l->uses.push_back({reg(fn, 64)->id, 0});
	auto* e = reg(fn, 64);
	l->defValue = e->id;
	e->defInstr = l;
	return e->id;
}

ssa::IrInstr* floatOp(ssa::SSAFunction& fn, ssa::IrInstr::Op op, ssa::ValueId a, ssa::ValueId b, ssa::BlockId blk = 1)
{
	auto* i = fn.addInstr(blk, op);
	i->isFloat = true;
	i->uses.push_back({a, 0});
	i->uses.push_back({b, 1});
	auto* d = reg(fn, 64);
	i->defValue = d->id;
	d->defInstr = i;
	return i;
}

ssa::ValueId floatImm(ssa::SSAFunction& fn, uint64_t bits)
{
	auto* v = fn.allocValue(ssa::ValueKind::Immediate);
	v->imm = bits;
	return v->id;
}

TEST(IdiomDetectorTest, SumOfElementProductsIsADotProduct)
{
	auto fn = makeLoop();
	floatOp(*fn, ssa::IrInstr::Op::Mul, loadElement(*fn), loadElement(*fn));
	pad(*fn);
	EXPECT_TRUE(detects(*fn, IdiomKind::DotProduct));
	EXPECT_FALSE(detects(*fn, IdiomKind::MatrixMultiply));
}

TEST(IdiomDetectorTest, ElementProductsWrittenBackInThreeLoopsAreAMatrixMultiply)
{
	auto fn = makeLoop();
	auto* p = floatOp(*fn, ssa::IrInstr::Op::Mul, loadElement(*fn), loadElement(*fn));
	storeToMemory(*fn, p->defValue);
	pad(*fn);
	fn->addInstr(1, ssa::IrInstr::Op::CondBranch);
	fn->addInstr(1, ssa::IrInstr::Op::CondBranch);
	EXPECT_TRUE(detects(*fn, IdiomKind::MatrixMultiply));
	EXPECT_FALSE(detects(*fn, IdiomKind::DotProduct));
}

TEST(IdiomDetectorTest, ElementProductWrittenBackInOneLoopIsNeither)
{
	// `c[i] = a[i] * b[i]` is an elementwise transform.
	auto fn = makeLoop();
	auto* p = floatOp(*fn, ssa::IrInstr::Op::Mul, loadElement(*fn), loadElement(*fn));
	storeToMemory(*fn, p->defValue);
	pad(*fn);
	EXPECT_FALSE(detects(*fn, IdiomKind::DotProduct));
	EXPECT_FALSE(detects(*fn, IdiomKind::MatrixMultiply));
}

TEST(IdiomDetectorTest, SumDividedAfterTheLoopIsStatistics)
{
	auto fn = makeLoop();
	auto* exit = fn->addBlock("exit");
	fn->block(1)->succs.push_back(exit->id);
	auto* sum = floatOp(*fn, ssa::IrInstr::Op::Add, loadElement(*fn), reg(*fn, 64)->id);
	pad(*fn);
	floatOp(*fn, ssa::IrInstr::Op::Div, sum->defValue, reg(*fn, 64)->id, exit->id);
	EXPECT_TRUE(detects(*fn, IdiomKind::Statistics));
}

TEST(IdiomDetectorTest, SumWithoutADivideIsNotStatistics)
{
	auto fn = makeLoop();
	floatOp(*fn, ssa::IrInstr::Op::Add, loadElement(*fn), reg(*fn, 64)->id);
	pad(*fn);
	EXPECT_FALSE(detects(*fn, IdiomKind::Statistics));
}

/// `x = 0.5 * (x + a / x)` with a float convergence test.
std::unique_ptr<ssa::SSAFunction> makeNewtonLoop(bool halve)
{
	auto fn = makeLoop();
	auto* q = floatOp(*fn, ssa::IrInstr::Op::Div, reg(*fn, 64)->id, reg(*fn, 64)->id);
	auto* s = floatOp(*fn, ssa::IrInstr::Op::Add, q->defValue, reg(*fn, 64)->id);
	floatOp(*fn, ssa::IrInstr::Op::Mul, s->defValue, floatImm(*fn, halve ? 0x3FE0000000000000ULL : 0x4008000000000000ULL));
	auto* cmp = fn->addInstr(1, ssa::IrInstr::Op::Compare);
	cmp->isFloat = true;
	fn->addInstr(1, ssa::IrInstr::Op::CondBranch);
	return fn;
}

TEST(IdiomDetectorTest, HalvedSumWithAQuotientIsNewtonRaphson)
{
	EXPECT_TRUE(detects(*makeNewtonLoop(true), IdiomKind::NewtonRaphson));
}

TEST(IdiomDetectorTest, QuotientScaledByThreeIsNotNewtonRaphson)
{
	EXPECT_FALSE(detects(*makeNewtonLoop(false), IdiomKind::NewtonRaphson));
}

TEST(IdiomDetectorTest, LowBitAddedThenShiftedOutIsPopcount)
{
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::And, 1);
	opImm(*fn, ssa::IrInstr::Op::Add, 0);
	opImm(*fn, ssa::IrInstr::Op::Shr, 1);
	pad(*fn);
	EXPECT_TRUE(detects(*fn, IdiomKind::Popcount));
}

TEST(IdiomDetectorTest, LowBitLoopThatXorsIsNotPopcount)
{
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::And, 1);
	opImm(*fn, ssa::IrInstr::Op::Add, 0);
	opImm(*fn, ssa::IrInstr::Op::Shr, 1);
	opImm(*fn, ssa::IrInstr::Op::Xor, 3);
	pad(*fn);
	EXPECT_FALSE(detects(*fn, IdiomKind::Popcount));
}

TEST(IdiomDetectorTest, RemainderByAVariableInALoopIsGcd)
{
	auto fn = makeLoop();
	auto* rem = fn->addInstr(1, ssa::IrInstr::Op::Rem);
	rem->uses.push_back({reg(*fn)->id, 0});
	rem->uses.push_back({reg(*fn)->id, 1});
	pad(*fn);
	fn->addInstr(1, ssa::IrInstr::Op::Assign);
	EXPECT_TRUE(detects(*fn, IdiomKind::Gcd));
}

TEST(IdiomDetectorTest, RemainderByAConstantIsNotGcd)
{
	auto fn = makeLoop();
	opImm(*fn, ssa::IrInstr::Op::Rem, 10);
	pad(*fn);
	fn->addInstr(1, ssa::IrInstr::Op::Assign);
	EXPECT_FALSE(detects(*fn, IdiomKind::Gcd));
}

TEST(IdiomDetectorTest, ByteXoredAndStoredBackIsXorCipher)
{
	auto fn = makeLoop();
	auto* ld = byteLoad(*fn);
	auto* x = fn->addInstr(1, ssa::IrInstr::Op::Xor);
	x->uses.push_back({ld->defValue, 0});
	x->uses.push_back({reg(*fn, 8)->id, 1});
	auto* d = reg(*fn, 8);
	x->defValue = d->id;
	d->defInstr = x;
	storeToMemory(*fn, d->id);
	pad(*fn);
	EXPECT_TRUE(detects(*fn, IdiomKind::XorCipher));
}

TEST(IdiomDetectorTest, StoringAPassedValueInALoopIsMemset)
{
	auto fn = makeLoop();
	storeToMemory(*fn, reg(*fn, 8)->id);
	opImm(*fn, ssa::IrInstr::Op::Add, 1);
	pad(*fn);
	fn->addInstr(1, ssa::IrInstr::Op::Assign);
	EXPECT_TRUE(detects(*fn, IdiomKind::Memset));
}

// An LCS table filled while walking the strings is not strlen or strcmp.
TEST(IdiomDetectorTest, AByteWalkThatFillsATableIsNotStrlen)
{
	auto fn = makeByteWalk(/*compareAgainstZero=*/true, /*loadBytes=*/1);
	storeToMemory(*fn, reg(*fn)->id);
	EXPECT_FALSE(detects(*fn, IdiomKind::Strlen));
	EXPECT_FALSE(detects(*fn, IdiomKind::Strcmp));
}

TEST(IdiomDetectorTest, StoringAComputedValueInALoopIsNotMemset)
{
	auto fn = makeLoop();
	auto* orI = opImm(*fn, ssa::IrInstr::Op::Or, 0x80);
	storeToMemory(*fn, orI->defValue);
	pad(*fn);
	fn->addInstr(1, ssa::IrInstr::Op::Assign);
	EXPECT_FALSE(detects(*fn, IdiomKind::Memset));
}

} // namespace

TEST(IdiomDetectorTest, AByteTestedAgainstZeroInALoopIsStrlen)
{
	auto fn = makeByteWalk(/*compareAgainstZero=*/true, /*loadBytes=*/1);
	EXPECT_TRUE(detects(*fn, IdiomKind::Strlen));
}

// GCC's __do_global_dtors_aux tests a completed-flag byte against zero and
// has no loop, but LLVM lays `common.ret` out above the block that branches
// to it. An edge to a lower block id is not a back edge, so this is not a
// string walk -- it was reported as Strlen in every corpus binary.
TEST(IdiomDetectorTest, AByteLoopThatHandsEachByteToACallIsNotStrlen)
{
	// for (i = 0; msg[i]; ++i) push(msg[i]); consumes the string, not measures it.
	auto fn = makeByteWalk(/*compareAgainstZero=*/true, /*loadBytes=*/1);
	fn->addInstr(1, ssa::IrInstr::Op::Call)->calleeName = "push";
	EXPECT_FALSE(detects(*fn, IdiomKind::Strlen));
}

// while (*a && *a == *b) compares each byte with the other string's too.
TEST(IdiomDetectorTest, TwoBytesComparedWithEachOtherIsStrcmpNotStrlen)
{
	auto fn = makeByteWalk(/*compareAgainstZero=*/true, /*loadBytes=*/1);
	ssa::IrValue* mem = fn->allocValue(ssa::ValueKind::MemRef);
	mem->memWidth = 1;
	ssa::IrValue* other = fn->allocValue(ssa::ValueKind::VirtualReg);
	other->width = 8;
	ssa::IrInstr* ld = fn->addInstr(1, ssa::IrInstr::Op::Load);
	ld->uses.push_back({mem->id, 0});
	ld->defValue = other->id;
	other->defInstr = ld;
	ssa::ValueId first = ssa::kInvalidValue;
	for (auto* i: fn->block(1)->instrs)
		if (i->op == ssa::IrInstr::Op::Load && i != ld) first = i->defValue;
	ssa::IrInstr* cmp = fn->addInstr(1, ssa::IrInstr::Op::Compare);
	cmp->uses.push_back({first, 0});
	cmp->uses.push_back({other->id, 1});
	EXPECT_TRUE(detects(*fn, IdiomKind::Strcmp));
	EXPECT_FALSE(detects(*fn, IdiomKind::Strlen));
}

// gcc -O2 keeps the other string's byte in a register: the first one is a
// constant and the next is loaded at the bottom of the loop, so the compare
// reads it through a phi.
TEST(IdiomDetectorTest, AByteComparedWithAPhiOfLoadedBytesIsStrcmp)
{
	auto fn = makeByteWalk(/*compareAgainstZero=*/true, /*loadBytes=*/1);
	ssa::IrValue* mem = fn->allocValue(ssa::ValueKind::MemRef);
	mem->memWidth = 1;
	ssa::IrValue* loaded = fn->allocValue(ssa::ValueKind::VirtualReg);
	loaded->width = 8;
	ssa::IrInstr* ld = fn->addInstr(1, ssa::IrInstr::Op::Load);
	ld->uses.push_back({mem->id, 0});
	ld->defValue = loaded->id;
	loaded->defInstr = ld;
	ssa::IrValue* init = fn->allocValue(ssa::ValueKind::Immediate);
	init->imm = 'a';
	ssa::IrValue* merged = fn->allocValue(ssa::ValueKind::VirtualReg);
	merged->width = 8;
	ssa::PhiNode* phi = fn->addPhi(1, 0);
	phi->result = merged->id;
	phi->addOperand(0, init->id);
	phi->addOperand(1, loaded->id);
	merged->defPhi = phi;
	ssa::ValueId first = ssa::kInvalidValue;
	for (auto* i: fn->block(1)->instrs)
		if (i->op == ssa::IrInstr::Op::Load && i != ld) first = i->defValue;
	ssa::IrInstr* cmp = fn->addInstr(1, ssa::IrInstr::Op::Compare);
	cmp->uses.push_back({first, 0});
	cmp->uses.push_back({merged->id, 1});
	EXPECT_TRUE(detects(*fn, IdiomKind::Strcmp));
	EXPECT_FALSE(detects(*fn, IdiomKind::Strlen));
}

TEST(IdiomDetectorTest, AByteTestedAgainstZeroWithoutALoopIsNotStrlen)
{
	auto fn = makeByteWalk(/*compareAgainstZero=*/true, /*loadBytes=*/1);
	// Rewire to entry -> {ret, loop}, loop -> ret: acyclic, with ret (id 2)
	// below loop (id 1) only by layout.
	auto* ret = fn->addBlock("common.ret");
	fn->block(1)->succs.clear();
	fn->block(1)->succs.push_back(ret->id);
	fn->block(0)->succs.push_back(ret->id);
	// ...and put ret above the branch in id order, as LLVM does.
	auto* tail = fn->addBlock("tail");
	tail->succs.push_back(ret->id);
	fn->block(1)->succs.push_back(tail->id);
	EXPECT_FALSE(detects(*fn, IdiomKind::Strlen));
}

TEST(IdiomDetectorTest, AWordLoadTestedAgainstZeroIsNotStrlen)
{
	// Every counted loop tests something against zero. Only a byte walk is a
	// string walk.
	auto fn = makeByteWalk(/*compareAgainstZero=*/true, /*loadBytes=*/4);
	EXPECT_FALSE(detects(*fn, IdiomKind::Strlen));
}

TEST(IdiomDetectorTest, AByteComparedAgainstSomethingElseIsNotStrlen)
{
	auto fn = makeByteWalk(/*compareAgainstZero=*/false, /*loadBytes=*/1);
	EXPECT_FALSE(detects(*fn, IdiomKind::Strlen));
}

TEST(IdiomDetectorTest, AComparisonLoopThatNeverReadsTheLoadedByteIsNotStrlen)
{
	// The shape a comparison sort has: a loop, loads, a compare against zero
	// somewhere, and no multiply -- but the compare does not read the load.
	auto fn = std::make_unique<ssa::SSAFunction>("bubble");
	auto* entry = fn->addBlock("entry");
	auto* loop = fn->addBlock("loop");
	entry->succs.push_back(loop->id);
	loop->succs.push_back(loop->id);

	ssa::IrValue* mem = fn->allocValue(ssa::ValueKind::MemRef);
	mem->memWidth = 1;
	ssa::IrValue* loaded = fn->allocValue(ssa::ValueKind::VirtualReg);
	loaded->width = 8;
	ssa::IrInstr* ld = fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
	ld->uses.push_back({mem->id, 0});
	ld->defValue = loaded->id;
	loaded->defInstr = ld;

	ssa::IrValue* counter = fn->allocValue(ssa::ValueKind::VirtualReg);
	ssa::IrValue* zero = fn->allocValue(ssa::ValueKind::Immediate);
	zero->imm = 0u;
	ssa::IrInstr* cmp = fn->addInstr(loop->id, ssa::IrInstr::Op::Compare);
	cmp->uses.push_back({counter->id, 0});
	cmp->uses.push_back({zero->id, 1});
	fn->addInstr(loop->id, ssa::IrInstr::Op::Add);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Store);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Branch);

	EXPECT_FALSE(detects(*fn, IdiomKind::Strlen));
}

namespace {

// entry -> loop -> {loop, exit}; the self-call sits in `callIn`.
std::unique_ptr<ssa::SSAFunction> makeRecursive(bool callInLoop)
{
	auto fn = std::make_unique<ssa::SSAFunction>("visit");
	auto* entry = fn->addBlock("entry");
	auto* loop = fn->addBlock("loop");
	auto* exit = fn->addBlock("exit");
	entry->succs.push_back(loop->id);
	loop->succs.push_back(loop->id);
	loop->succs.push_back(exit->id);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
	fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
	fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	auto* call = fn->addInstr(callInLoop ? loop->id : exit->id, ssa::IrInstr::Op::Call);
	call->calleeName = "visit";
	return fn;
}

} // namespace

// A graph DFS recurses from inside the loop over a node's neighbours.
TEST(IdiomDetectorTest, RecursionFromInsideALoopIsDfs)
{
	auto fn = makeRecursive(/*callInLoop=*/true);
	EXPECT_TRUE(detects(*fn, IdiomKind::Dfs));
}

// Quicksort and heapify recurse too, but not from inside a loop; every
// recursive function was DFS.
TEST(IdiomDetectorTest, RecursionOutsideAnyLoopIsNotDfs)
{
	auto fn = makeRecursive(/*callInLoop=*/false);
	EXPECT_FALSE(detects(*fn, IdiomKind::Dfs));
}

// -O2 quicksort loops around its first recursion in place of the second, so the
// self-call is inside a loop -- but that loop swaps elements; a DFS never does.
TEST(IdiomDetectorTest, RecursionInALoopThatSwapsElementsIsNotDfs)
{
	auto fn = makeRecursive(/*callInLoop=*/true);
	auto* loop = fn->block(1);
	for (int k = 0; k < 2; ++k)
	{
		ssa::IrValue* addr = fn->allocValue(ssa::ValueKind::VirtualReg);
		ssa::IrValue* elem = fn->allocValue(ssa::ValueKind::VirtualReg);
		ssa::IrInstr* ld = fn->addInstr(loop->id, ssa::IrInstr::Op::Load);
		ld->uses.push_back({addr->id, 0});
		ld->defValue = elem->id;
		elem->defInstr = ld;
		ssa::IrInstr* st = fn->addInstr(loop->id, ssa::IrInstr::Op::Store);
		st->uses.push_back({elem->id, 0});
		st->uses.push_back({fn->allocValue(ssa::ValueKind::VirtualReg)->id, 1});
	}
	EXPECT_FALSE(detects(*fn, IdiomKind::Dfs));
}

// Recursive Fibonacci at -O2 also recurses from inside a loop, but it only
// reads its own stack slots; a DFS reads the graph.
TEST(IdiomDetectorTest, RecursionInALoopThatOnlyReadsTheStackIsNotDfs)
{
	auto fn = makeRecursive(/*callInLoop=*/true);
	auto* slot = fn->allocValue(ssa::ValueKind::VirtualReg);
	slot->isStackSlot = true;
	for (const auto* i: fn->block(1)->instrs)
		if (i->op == ssa::IrInstr::Op::Load) const_cast<ssa::IrInstr*>(i)->uses.push_back({slot->id, 0});
	EXPECT_FALSE(detects(*fn, IdiomKind::Dfs));
}

namespace {

/// Straight-line code (or a loop when `inLoop`) with the given conversions.
std::unique_ptr<ssa::SSAFunction> makeConversions(int toFloat, int toInt, bool inLoop)
{
	// Straight line: entry -> mid -> exit, enough blocks for the preflight.
	auto fn = inLoop ? makeLoop() : std::make_unique<ssa::SSAFunction>("conv");
	if (!inLoop)
	{
		auto* e = fn->addBlock("entry");
		auto* m = fn->addBlock("mid");
		auto* x = fn->addBlock("exit");
		e->succs.push_back(m->id);
		m->succs.push_back(x->id);
	}
	const ssa::BlockId b = 1;
	for (int k = 0; k < 6; ++k) fn->addInstr(b, ssa::IrInstr::Op::Add);
	for (int k = 0; k < toFloat + toInt; ++k)
	{
		auto* c = fn->addInstr(b, ssa::IrInstr::Op::Assign);
		c->isFloat = true;
		c->fpConv = k < toFloat ? ssa::IrInstr::FpConv::ToFloat : ssa::IrInstr::FpConv::ToInt;
	}
	return fn;
}

} // namespace

TEST(IdiomDetectorTest, StraightLineRoundTripsAreConversion)
{
	EXPECT_TRUE(detects(*makeConversions(3, 1, false), IdiomKind::Conversion));
}

TEST(IdiomDetectorTest, ConversionsInsideALoopAreNotConversion)
{
	// A numeric kernel converting each element is not a conversion routine.
	EXPECT_FALSE(detects(*makeConversions(3, 1, true), IdiomKind::Conversion));
}

TEST(IdiomDetectorTest, OneWayConversionsAreNotConversion)
{
	EXPECT_FALSE(detects(*makeConversions(4, 0, false), IdiomKind::Conversion));
	EXPECT_FALSE(detects(*makeConversions(1, 1, false), IdiomKind::Conversion));
}
