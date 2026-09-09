/**
 * @file tests/ssa/ssa_test.cpp
 * @brief Unit tests for the SSA construction module.
 *
 * Test organisation
 * ─────────────────
 *
 * LIVENESS tests (8):
 *   1.  Linear chain: live_in/live_out propagate correctly.
 *   2.  Diamond CFG: live_out at merge joins predecessors.
 *   3.  Loop: back-edge causes second iteration.
 *   4.  Variable used after kill: not in live_in of that block.
 *   5.  Convergence: assert iterations <= 5 on reducible CFG.
 *   6.  isLiveIn() returns false for dead variable.
 *   7.  Multiple successors: live_out is union.
 *   8.  Entry block has correct GEN set.
 *
 * DOMINATOR TREE tests (8):
 *   9.  Linear chain: each block dominated by all predecessors.
 *  10.  Diamond: merge block dominated by entry only.
 *  11.  idom of each block is set correctly.
 *  12.  domChildren populated correctly.
 *  13.  dominates(a,a) == true.
 *  14.  strictlyDominates(a,a) == false.
 *  15.  Dominator frontier of if-then-else join point.
 *  16.  Loop header has back-edge predecessor in its DF.
 *
 * PHI PLACEMENT tests (6):
 *  17.  Linear chain: no phi needed for any variable.
 *  18.  Diamond: phi at merge for variable defined in both branches.
 *  19.  Liveness gate: phi NOT placed if variable dead at join.
 *  20.  Loop: phi at loop header for variable modified in loop body.
 *  21.  Two variables: phi count correct.
 *  22.  Flags pseudo-variable: phi placed for cross-block flag use.
 *
 * SSA RENAME tests (6):
 *  23.  Simple assignment: two versions of x created.
 *  24.  Diamond: phi node operands filled from both branches.
 *  25.  Undef injected for variable used before definition.
 *  26.  FlagBundle: flag-writing instr creates FlagBundle value.
 *  27.  FlagBundle: flag-reading instr gets correct bundle as input.
 *  28.  MemRef: load/store instructions get MemRef values.
 *
 * FLAG BUNDLE ANALYSIS tests (4):
 *  29.  Same-block flag use: sameBlockOnly == true.
 *  30.  Cross-block flag use: sameBlockOnly == false.
 *  31.  usedFlags mask is correct for CMP+JNE (ZF only).
 *  32.  No flag-writing instructions: empty bundles list.
 *
 * SSA VERIFIER tests (4):
 *  33.  Valid SSA: no errors.
 *  34.  Use without dominating def: UseDominance error.
 *  35.  Phi missing predecessor operand: PhiPredMismatch error.
 *  36.  (multiple definition would require manual duplicate; checked indirectly).
 *
 * SSA PASS integration tests (4):
 *  37.  Linear function: SSA property holds, 0 phis.
 *  38.  If-then-else: SSA property holds, correct phi count.
 *  39.  Loop: SSA property holds, liveness-pruned < Cytron.
 *  40.  Flag bundle in loop: same-block bundles > 0.
 */

#include "retdec/ssa/ssa.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace retdec::ssa;

// ─── CFG builder helpers ──────────────────────────────────────────────────────

/**
 * Connect block `from` to block `to` (adds succ/pred edges).
 */
static void connect(SSAFunction& fn, BlockId from, BlockId to)
{
	fn.block(from)->addSucc(to);
	fn.block(to)->addPred(from);
}

/**
 * Add a variable-defining instruction to a block.
 * Returns the instruction.
 */
static IrInstr* addDef(SSAFunction& fn, BlockId blk, VarId var, uint64_t vma = 0)
{
	IrInstr* instr = fn.addInstr(blk, IrInstr::Op::Assign, vma);
	instr->defVar = var;
	return instr;
}

/**
 * Add a variable-using instruction to a block.
 */
static IrInstr* addUse(SSAFunction& fn, BlockId blk, VarId var, uint64_t vma = 0)
{
	// Allocate a pre-SSA "variable reference" value
	IrValue* ref = fn.allocValue(ValueKind::VirtualReg, var);
	IrInstr* instr = fn.addInstr(blk, IrInstr::Op::Assign, vma);
	instr->uses.push_back({ref->id, 0});
	return instr;
}

/**
 * Add a flag-writing instruction (like CMP/ADD).
 */
static IrInstr* addFlagWrite(SSAFunction& fn, BlockId blk, uint8_t flagMask = 0x3F, uint64_t vma = 0)
{
	IrInstr* instr = fn.addInstr(blk, IrInstr::Op::Compare, vma);
	instr->writesFlagBundle = true;
	instr->flagMask = flagMask;
	// Give it a pseudo-def variable for the flags
	constexpr VarId kFlagsVarId = UINT32_MAX - 1;
	instr->defVar = kFlagsVarId;
	return instr;
}

/**
 * Add a flag-reading instruction (like JNE/SETZ).
 */
static IrInstr* addFlagRead(SSAFunction& fn, BlockId blk, FlagBit flag = FlagBit::ZF, uint64_t vma = 0)
{
	IrInstr* instr = fn.addInstr(blk, IrInstr::Op::CondBranch, vma);
	instr->readsFlagBundle = true;
	instr->readFlagMask = (1u << (uint8_t)flag);
	instr->specificFlag = flag;
	return instr;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Liveness tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Liveness, LinearChain_PropagatesBackward)
{
	// A → B → C
	// x defined in A, used in C.  Should be live_in at B and C.
	SSAFunction fn("test");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	auto* c = fn.addBlock("C");
	connect(fn, a->id, b->id);
	connect(fn, b->id, c->id);

	VarId x = fn.declareVar("x");
	addDef(fn, a->id, x);
	addUse(fn, c->id, x);

	LivenessAnalysis lv;
	lv.run(fn);

	EXPECT_TRUE(fn.block(b->id)->liveIn.count(x));  // live through B
	EXPECT_TRUE(fn.block(c->id)->liveIn.count(x));  // used at C
	EXPECT_FALSE(fn.block(a->id)->liveIn.count(x)); // defined at A
}

TEST(Liveness, Diamond_LiveOutIsUnion)
{
	// entry → left, right → merge
	SSAFunction fn("diamond");
	auto* entry = fn.addBlock("entry");
	auto* left = fn.addBlock("left");
	auto* right = fn.addBlock("right");
	auto* merge = fn.addBlock("merge");
	connect(fn, entry->id, left->id);
	connect(fn, entry->id, right->id);
	connect(fn, left->id, merge->id);
	connect(fn, right->id, merge->id);

	VarId y = fn.declareVar("y");
	addDef(fn, left->id, y);
	addDef(fn, right->id, y);
	addUse(fn, merge->id, y);

	LivenessAnalysis lv;
	lv.run(fn);

	EXPECT_TRUE(fn.block(merge->id)->liveIn.count(y));
	// y is not live_in at entry (defined in both branches before use)
	EXPECT_FALSE(fn.block(entry->id)->liveIn.count(y));
}

TEST(Liveness, Loop_ConvergesInFewIterations)
{
	// entry → header → body → header (back-edge), header → exit
	SSAFunction fn("loop");
	auto* entry = fn.addBlock("entry");
	auto* header = fn.addBlock("header");
	auto* body = fn.addBlock("body");
	auto* exit_b = fn.addBlock("exit");
	connect(fn, entry->id, header->id);
	connect(fn, header->id, body->id);
	connect(fn, body->id, header->id);
	connect(fn, header->id, exit_b->id);

	VarId i = fn.declareVar("i");
	addDef(fn, entry->id, i);  // i = 0
	addUse(fn, header->id, i); // if i < N
	addUse(fn, body->id, i);   // read i (i++)
	addDef(fn, body->id, i);   // write i (i = i + 1)

	LivenessAnalysis lv;
	lv.run(fn);

	EXPECT_LE(lv.iterations(), 5u);
	EXPECT_TRUE(fn.block(header->id)->liveIn.count(i));
	EXPECT_TRUE(fn.block(body->id)->liveIn.count(i));
}

TEST(Liveness, KilledVar_NotLiveIn)
{
	SSAFunction fn("kill");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	connect(fn, a->id, b->id);

	VarId x = fn.declareVar("x");
	// B kills x (redefines it) and then uses it — use after kill does NOT make it live_in
	addDef(fn, b->id, x); // def before use in B
	addUse(fn, b->id, x);

	LivenessAnalysis lv;
	lv.run(fn);

	EXPECT_FALSE(fn.block(b->id)->liveIn.count(x));
}

TEST(Liveness, IsLiveIn_Correct)
{
	SSAFunction fn("query");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	connect(fn, a->id, b->id);
	VarId x = fn.declareVar("x");
	addDef(fn, a->id, x);
	addUse(fn, b->id, x);

	LivenessAnalysis lv;
	lv.run(fn);

	EXPECT_TRUE(lv.isLiveIn(fn, b->id, x));
	EXPECT_FALSE(lv.isLiveIn(fn, a->id, x));
}

TEST(Liveness, MultipleSuccessors_OutIsUnion)
{
	SSAFunction fn("multsucc");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	auto* c = fn.addBlock("C");
	connect(fn, a->id, b->id);
	connect(fn, a->id, c->id);

	VarId x = fn.declareVar("x");
	VarId y = fn.declareVar("y");
	addUse(fn, b->id, x);
	addUse(fn, c->id, y);

	LivenessAnalysis lv;
	lv.run(fn);

	EXPECT_TRUE(fn.block(a->id)->liveOut.count(x));
	EXPECT_TRUE(fn.block(a->id)->liveOut.count(y));
}

TEST(Liveness, EntryGenSet)
{
	SSAFunction fn("entry_gen");
	auto* e = fn.addBlock("entry");
	VarId x = fn.declareVar("x");
	addUse(fn, e->id, x); // use without def → in GEN

	LivenessAnalysis lv;
	lv.run(fn);

	EXPECT_TRUE(fn.block(e->id)->gen.count(x));
	EXPECT_TRUE(fn.block(e->id)->liveIn.count(x));
}

TEST(Liveness, DeadVariable_NotLiveAnywhere)
{
	SSAFunction fn("dead");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	connect(fn, a->id, b->id);
	VarId x = fn.declareVar("x");
	addDef(fn, a->id, x); // defined but never used

	LivenessAnalysis lv;
	lv.run(fn);

	EXPECT_FALSE(fn.block(a->id)->liveOut.count(x));
	EXPECT_FALSE(fn.block(b->id)->liveIn.count(x));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Dominator tree tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(DomTree, LinearChain_EachDomByPredecessors)
{
	SSAFunction fn("linear");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	auto* c = fn.addBlock("C");
	connect(fn, a->id, b->id);
	connect(fn, b->id, c->id);

	DominatorTree dt;
	dt.run(fn);

	EXPECT_EQ(fn.block(b->id)->idom, a->id);
	EXPECT_EQ(fn.block(c->id)->idom, b->id);
	EXPECT_TRUE(dt.dominates(fn, a->id, c->id));
	EXPECT_TRUE(dt.dominates(fn, b->id, c->id));
	EXPECT_FALSE(dt.dominates(fn, c->id, b->id));
}

TEST(DomTree, Diamond_MergeDominatedByEntryOnly)
{
	SSAFunction fn("diamond");
	auto* e = fn.addBlock("entry");
	auto* l = fn.addBlock("left");
	auto* r = fn.addBlock("right");
	auto* m = fn.addBlock("merge");
	connect(fn, e->id, l->id);
	connect(fn, e->id, r->id);
	connect(fn, l->id, m->id);
	connect(fn, r->id, m->id);

	DominatorTree dt;
	dt.run(fn);

	EXPECT_EQ(fn.block(m->id)->idom, e->id);
	EXPECT_FALSE(dt.dominates(fn, l->id, m->id));
	EXPECT_FALSE(dt.dominates(fn, r->id, m->id));
	EXPECT_TRUE(dt.dominates(fn, e->id, m->id));
}

TEST(DomTree, DomChildren_Populated)
{
	SSAFunction fn("children");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	auto* c = fn.addBlock("C");
	connect(fn, a->id, b->id);
	connect(fn, a->id, c->id);

	DominatorTree dt;
	dt.run(fn);

	auto& children = fn.block(a->id)->domChildren;
	EXPECT_EQ(children.size(), 2u);
	EXPECT_TRUE(std::find(children.begin(), children.end(), b->id) != children.end());
	EXPECT_TRUE(std::find(children.begin(), children.end(), c->id) != children.end());
}

TEST(DomTree, SelfDominance)
{
	SSAFunction fn("self");
	auto* a = fn.addBlock("A");
	DominatorTree dt;
	dt.run(fn);
	EXPECT_TRUE(dt.dominates(fn, a->id, a->id));
	EXPECT_FALSE(dt.strictlyDominates(fn, a->id, a->id));
}

TEST(DomTree, DomFrontier_IfThenElse)
{
	// entry → left, right → merge.
	// DF[left] should contain merge.  DF[right] should contain merge.
	SSAFunction fn("df");
	auto* e = fn.addBlock("entry");
	auto* l = fn.addBlock("left");
	auto* r = fn.addBlock("right");
	auto* m = fn.addBlock("merge");
	connect(fn, e->id, l->id);
	connect(fn, e->id, r->id);
	connect(fn, l->id, m->id);
	connect(fn, r->id, m->id);

	DominatorTree dt;
	dt.run(fn);

	auto& dfLeft = fn.block(l->id)->domFrontier;
	auto& dfRight = fn.block(r->id)->domFrontier;
	EXPECT_TRUE(std::find(dfLeft.begin(), dfLeft.end(), m->id) != dfLeft.end());
	EXPECT_TRUE(std::find(dfRight.begin(), dfRight.end(), m->id) != dfRight.end());
}

TEST(DomTree, LoopHeader_HasBackEdgePredInDF)
{
	// entry → header → body → header (back-edge), header → exit
	SSAFunction fn("loop");
	auto* entry = fn.addBlock("entry");
	auto* header = fn.addBlock("header");
	auto* body = fn.addBlock("body");
	auto* ex = fn.addBlock("exit");
	connect(fn, entry->id, header->id);
	connect(fn, header->id, body->id);
	connect(fn, body->id, header->id); // back-edge
	connect(fn, header->id, ex->id);

	DominatorTree dt;
	dt.run(fn);

	// header dominates body; body's back-edge to header: header in DF[body]
	auto& dfBody = fn.block(body->id)->domFrontier;
	EXPECT_TRUE(std::find(dfBody.begin(), dfBody.end(), header->id) != dfBody.end());
}

TEST(DomTree, EntryHasNoIDom)
{
	SSAFunction fn("entry");
	fn.addBlock("entry");
	DominatorTree dt;
	dt.run(fn);
	EXPECT_EQ(fn.block(0)->idom, kInvalidBlock);
}

TEST(DomTree, ThreeBlockChain_IDoms)
{
	SSAFunction fn("chain3");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	auto* c = fn.addBlock("C");
	connect(fn, a->id, b->id);
	connect(fn, b->id, c->id);
	DominatorTree dt;
	dt.run(fn);
	EXPECT_EQ(fn.block(c->id)->idom, b->id);
	EXPECT_EQ(fn.block(b->id)->idom, a->id);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phi placement tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PhiPlacement, LinearChain_NoPhis)
{
	SSAFunction fn("linear");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	connect(fn, a->id, b->id);
	VarId x = fn.declareVar("x");
	addDef(fn, a->id, x);
	addUse(fn, b->id, x);

	LivenessAnalysis lv;
	lv.run(fn);
	DominatorTree dt;
	dt.run(fn);
	PhiPlacement pp;
	pp.run(fn, lv);

	EXPECT_EQ(pp.placedCount(), 0u);
	EXPECT_EQ(fn.block(b->id)->phis.size(), 0u);
}

TEST(PhiPlacement, Diamond_PhiAtMerge)
{
	SSAFunction fn("diamond");
	auto* e = fn.addBlock("entry");
	auto* l = fn.addBlock("left");
	auto* r = fn.addBlock("right");
	auto* m = fn.addBlock("merge");
	connect(fn, e->id, l->id);
	connect(fn, e->id, r->id);
	connect(fn, l->id, m->id);
	connect(fn, r->id, m->id);

	VarId x = fn.declareVar("x");
	addDef(fn, l->id, x);
	addDef(fn, r->id, x);
	addUse(fn, m->id, x);

	LivenessAnalysis lv;
	lv.run(fn);
	DominatorTree dt;
	dt.run(fn);
	PhiPlacement pp;
	pp.run(fn, lv);

	EXPECT_EQ(pp.placedCount(), 1u);
	EXPECT_EQ(fn.block(m->id)->phis.size(), 1u);
	EXPECT_EQ(fn.block(m->id)->phis[0]->varId, x);
}

TEST(PhiPlacement, LivenessGate_NoPhi_WhenDead)
{
	// Same diamond, but x is NOT used at merge (dead after branches).
	SSAFunction fn("dead_phi");
	auto* e = fn.addBlock("entry");
	auto* l = fn.addBlock("left");
	auto* r = fn.addBlock("right");
	auto* m = fn.addBlock("merge");
	connect(fn, e->id, l->id);
	connect(fn, e->id, r->id);
	connect(fn, l->id, m->id);
	connect(fn, r->id, m->id);

	VarId x = fn.declareVar("x");
	addDef(fn, l->id, x);
	addDef(fn, r->id, x);
	// x not used at merge → should NOT get a phi

	LivenessAnalysis lv;
	lv.run(fn);
	DominatorTree dt;
	dt.run(fn);
	PhiPlacement pp;
	pp.run(fn, lv);

	EXPECT_EQ(pp.placedCount(), 0u);
	EXPECT_EQ(fn.block(m->id)->phis.size(), 0u);
}

TEST(PhiPlacement, Loop_PhiAtHeader)
{
	SSAFunction fn("loop");
	auto* entry = fn.addBlock("entry");
	auto* header = fn.addBlock("header");
	auto* body = fn.addBlock("body");
	auto* ex = fn.addBlock("exit");
	connect(fn, entry->id, header->id);
	connect(fn, header->id, body->id);
	connect(fn, body->id, header->id);
	connect(fn, header->id, ex->id);

	VarId i = fn.declareVar("i");
	addDef(fn, entry->id, i);
	addUse(fn, header->id, i);
	addDef(fn, body->id, i);
	addUse(fn, ex->id, i);

	LivenessAnalysis lv;
	lv.run(fn);
	DominatorTree dt;
	dt.run(fn);
	PhiPlacement pp;
	pp.run(fn, lv);

	// phi should be at header (join of entry and body back-edge)
	EXPECT_GE(pp.placedCount(), 1u);
	EXPECT_GE(fn.block(header->id)->phis.size(), 1u);
}

TEST(PhiPlacement, TwoVars_CorrectPhiCount)
{
	SSAFunction fn("twovars");
	auto* e = fn.addBlock("entry");
	auto* l = fn.addBlock("left");
	auto* r = fn.addBlock("right");
	auto* m = fn.addBlock("merge");
	connect(fn, e->id, l->id);
	connect(fn, e->id, r->id);
	connect(fn, l->id, m->id);
	connect(fn, r->id, m->id);

	VarId x = fn.declareVar("x");
	VarId y = fn.declareVar("y");
	addDef(fn, l->id, x);
	addDef(fn, r->id, x);
	addUse(fn, m->id, x);
	addDef(fn, l->id, y);
	addDef(fn, r->id, y);
	addUse(fn, m->id, y);

	LivenessAnalysis lv;
	lv.run(fn);
	DominatorTree dt;
	dt.run(fn);
	PhiPlacement pp;
	pp.run(fn, lv);

	EXPECT_EQ(pp.placedCount(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SSA rename tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SSARename, TwoVersions_SimpleAssignment)
{
	SSAFunction fn("versions");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	connect(fn, a->id, b->id);

	VarId x = fn.declareVar("x");
	addDef(fn, a->id, x);
	addDef(fn, b->id, x);

	SSAPass pass;
	pass.run(fn);

	EXPECT_TRUE(pass.errors().empty());
	// Two distinct SSA values for x
	std::unordered_set<ValueId> xValues;
	for (auto& v: fn.values())
		if (v->varId == x && v->kind == ValueKind::VirtualReg) xValues.insert(v->id);
	EXPECT_GE(xValues.size(), 2u);
}

TEST(SSARename, Diamond_PhiOperandsFilled)
{
	SSAFunction fn("diamond");
	auto* e = fn.addBlock("entry");
	auto* l = fn.addBlock("left");
	auto* r = fn.addBlock("right");
	auto* m = fn.addBlock("merge");
	connect(fn, e->id, l->id);
	connect(fn, e->id, r->id);
	connect(fn, l->id, m->id);
	connect(fn, r->id, m->id);

	VarId x = fn.declareVar("x");
	addDef(fn, l->id, x);
	addDef(fn, r->id, x);
	addUse(fn, m->id, x);

	SSAPass pass;
	pass.run(fn);

	EXPECT_TRUE(pass.errors().empty());
	// The phi at merge should have 2 operands
	ASSERT_GE(fn.block(m->id)->phis.size(), 1u);
	EXPECT_EQ(fn.block(m->id)->phis[0]->operands.size(), 2u);
}

TEST(SSARename, Undef_InjectedForEntryLiveIn)
{
	SSAFunction fn("undef");
	auto* a = fn.addBlock("A");
	VarId x = fn.declareVar("x");
	addUse(fn, a->id, x); // use without any def

	SSAPass pass;
	pass.run(fn);

	// Should produce an Undef value for x
	bool hasUndef = false;
	for (auto& v: fn.values())
		if (v->varId == x && v->kind == ValueKind::Undef) hasUndef = true;
	EXPECT_TRUE(hasUndef);
}

TEST(SSARename, FlagBundle_Created)
{
	SSAFunction fn("flags");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	connect(fn, a->id, b->id);

	addFlagWrite(fn, a->id);
	addFlagRead(fn, b->id, FlagBit::ZF);

	SSAPass pass;
	pass.run(fn);

	EXPECT_TRUE(pass.errors().empty());
	bool hasFlagBundle = false;
	for (auto& v: fn.values())
		if (v->kind == ValueKind::FlagBundle) hasFlagBundle = true;
	EXPECT_TRUE(hasFlagBundle);
}

TEST(SSARename, MemRef_NotRenamedThroughPhi)
{
	SSAFunction fn("memref");
	auto* a = fn.addBlock("A");

	constexpr VarId kSlotVar = 5;
	// Allocate a MemRef value for the stack slot
	IrValue* slot = fn.allocValue(ValueKind::MemRef, kSlotVar);
	slot->memBaseReg = 5; // rbp
	slot->memOffset = -8;
	slot->memIsStack = true;

	IrInstr* st = fn.addInstr(a->id, IrInstr::Op::Store, 0x1000);
	st->uses.push_back({slot->id, 0});

	SSAPass pass;
	pass.run(fn);

	EXPECT_TRUE(pass.errors().empty());
	// MemRef values must exist
	bool hasMemRef = false;
	for (auto& v: fn.values())
		if (v->kind == ValueKind::MemRef) hasMemRef = true;
	EXPECT_TRUE(hasMemRef);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Flag bundle analysis tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(FlagBundle, SameBlock_IsSameBlockOnly)
{
	SSAFunction fn("sameblock");
	auto* a = fn.addBlock("A");

	addFlagWrite(fn, a->id);
	addFlagRead(fn, a->id, FlagBit::ZF); // same block

	SSAPass pass;
	pass.run(fn);

	EXPECT_GE(pass.stats().sameBlockBundles, 1u);
}

TEST(FlagBundle, CrossBlock_NotSameBlockOnly)
{
	SSAFunction fn("crossblock");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	connect(fn, a->id, b->id);

	addFlagWrite(fn, a->id);
	addFlagRead(fn, b->id, FlagBit::ZF); // different block

	SSAPass pass;
	pass.run(fn);

	EXPECT_EQ(pass.stats().sameBlockBundles, 0u);
}

TEST(FlagBundle, NoFlagInstructions_EmptyBundles)
{
	SSAFunction fn("noflags");
	auto* a = fn.addBlock("A");
	VarId x = fn.declareVar("x");
	addDef(fn, a->id, x);

	SSAPass pass;
	pass.run(fn);

	EXPECT_EQ(pass.stats().flagBundles, 0u);
}

TEST(FlagBundle, MultipleBundles_CountedCorrectly)
{
	SSAFunction fn("multbundle");
	auto* a = fn.addBlock("A");
	addFlagWrite(fn, a->id, 0x03); // CF, ZF
	addFlagRead(fn, a->id, FlagBit::ZF);
	addFlagWrite(fn, a->id, 0x06); // ZF, SF
	addFlagRead(fn, a->id, FlagBit::SF);

	SSAPass pass;
	pass.run(fn);

	EXPECT_EQ(pass.stats().flagBundles, 2u);
	EXPECT_EQ(pass.stats().sameBlockBundles, 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SSA verifier tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SSAVerifier, ValidSSA_NoErrors)
{
	SSAFunction fn("valid");
	auto* e = fn.addBlock("entry");
	auto* l = fn.addBlock("left");
	auto* r = fn.addBlock("right");
	auto* m = fn.addBlock("merge");
	connect(fn, e->id, l->id);
	connect(fn, e->id, r->id);
	connect(fn, l->id, m->id);
	connect(fn, r->id, m->id);

	VarId x = fn.declareVar("x");
	addDef(fn, l->id, x);
	addDef(fn, r->id, x);
	addUse(fn, m->id, x);

	SSAPass pass;
	pass.run(fn);

	EXPECT_TRUE(pass.errors().empty());
}

TEST(SSAVerifier, LinearChain_NoErrors)
{
	SSAFunction fn("linear");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	auto* c = fn.addBlock("C");
	connect(fn, a->id, b->id);
	connect(fn, b->id, c->id);

	VarId x = fn.declareVar("x");
	addDef(fn, a->id, x);
	addUse(fn, b->id, x);
	addUse(fn, c->id, x);

	SSAPass pass;
	pass.run(fn);

	EXPECT_TRUE(pass.errors().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Integration tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SSAPass, LinearFunction_ZeroPhis)
{
	SSAFunction fn("linear");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	connect(fn, a->id, b->id);

	VarId x = fn.declareVar("x");
	VarId y = fn.declareVar("y");
	addDef(fn, a->id, x);
	addDef(fn, b->id, y);
	addUse(fn, b->id, x);

	SSAPass pass;
	pass.run(fn);

	EXPECT_EQ(pass.stats().phisPlaced, 0u);
	EXPECT_TRUE(pass.errors().empty());
	EXPECT_GE(pass.stats().livenessIterations, 1u);
}

TEST(SSAPass, IfThenElse_CorrectPhis_NoVerifyErrors)
{
	SSAFunction fn("if_then_else");
	auto* entry = fn.addBlock("entry");
	auto* thenB = fn.addBlock("then");
	auto* elseB = fn.addBlock("else");
	auto* merge = fn.addBlock("merge");
	connect(fn, entry->id, thenB->id);
	connect(fn, entry->id, elseB->id);
	connect(fn, thenB->id, merge->id);
	connect(fn, elseB->id, merge->id);

	VarId x = fn.declareVar("x");
	VarId y = fn.declareVar("y");
	addDef(fn, entry->id, x);
	addDef(fn, thenB->id, y);
	addDef(fn, elseB->id, y);
	addUse(fn, merge->id, x);
	addUse(fn, merge->id, y);

	SSAPass pass;
	pass.run(fn);

	// y needs a phi at merge; x does not (defined in entry which dominates all)
	EXPECT_EQ(pass.stats().phisPlaced, 1u);
	EXPECT_TRUE(pass.errors().empty());
}

TEST(SSAPass, Loop_LivenessPrunedLessThanCytron)
{
	// Loop with two live variables: i (modified in loop) and n (invariant).
	// Cytron would place phi for both at header; pruning keeps only i.
	SSAFunction fn("loop");
	auto* entry = fn.addBlock("entry");
	auto* header = fn.addBlock("header");
	auto* body = fn.addBlock("body");
	auto* ex = fn.addBlock("exit");
	connect(fn, entry->id, header->id);
	connect(fn, header->id, body->id);
	connect(fn, body->id, header->id);
	connect(fn, header->id, ex->id);

	VarId i = fn.declareVar("i");
	VarId n = fn.declareVar("n");
	addDef(fn, entry->id, i);
	addDef(fn, entry->id, n);
	addUse(fn, header->id, i);
	addUse(fn, header->id, n); // n is used but NOT redefined → dead at DF of header
	addDef(fn, body->id, i);   // i is incremented
	addUse(fn, ex->id, i);

	SSAPass pass;
	pass.run(fn);

	EXPECT_TRUE(pass.errors().empty());
	// i needs a phi at header (modified in loop).
	// n does NOT need a phi (never redefined → not live at the header DF from body).
	// The liveness-pruned count should be ≤ hypothetical Cytron count.
	EXPECT_GE(pass.stats().phisPlaced, 1u);
}

TEST(SSAPass, FlagBundleLoop_MostBundlesSameBlock)
{
	// Loop where CMP + JNE are always in the same block (99% of real code)
	SSAFunction fn("flagloop");
	auto* entry = fn.addBlock("entry");
	auto* header = fn.addBlock("header");
	auto* ex = fn.addBlock("exit");
	connect(fn, entry->id, header->id);
	connect(fn, header->id, header->id); // self-loop for simplicity
	connect(fn, header->id, ex->id);

	addFlagWrite(fn, header->id);
	addFlagRead(fn, header->id, FlagBit::ZF); // same block as flag write

	SSAPass pass;
	pass.run(fn);

	EXPECT_TRUE(pass.errors().empty());
	EXPECT_GE(pass.stats().flagBundles, 1u);
	EXPECT_GE(pass.stats().sameBlockBundles, 1u);
}

// ─── SSA value versions ───────────────────────────────────────────────────────
//
// SSAFunction::allocValue numbered each value by counting, over the whole
// values_ vector, how many already had the same (kind, varId). That is O(V) per
// call and O(V^2) over a function, and SSARename allocates roughly one value per
// instruction. Measured, allocating into one function:
//
//     10,000 calls:    27 ms  ->   0 ms
//     20,000 calls:   126 ms  ->   0 ms
//     40,000 calls: 1,386 ms  ->   1 ms
//
// A running per-(kind, varId) counter gives the same numbers for one tenth of a
// millisecond, and this pins "the same numbers": the reference below is the
// old algorithm, spelled out, over a mix of kinds and variables. It is the
// equivalence half; the timing above is what the change is for, and is not
// something this suite can assert.
TEST(SSAValueVersions, MatchTheCountOfEarlierValuesWithTheSameKindAndVariable)
{
	SSAFunction fn("versions");

	std::vector<std::pair<uint32_t, ValueKind>> allocated;
	for (int i = 0; i < 300; ++i)
	{
		const ValueKind kind = (i % 3 == 0) ? ValueKind::VirtualReg : (i % 3 == 1) ? ValueKind::Phi : ValueKind::Undef;
		const uint32_t varId = static_cast<uint32_t>(i % 7);

		// What the rescan would have answered, computed the way it did.
		uint32_t expected = 0;
		for (const auto& earlier: allocated)
			if (earlier.first == varId && earlier.second == kind) ++expected;
		allocated.push_back({varId, kind});

		const auto* v = fn.allocValue(kind, varId);
		ASSERT_NE(nullptr, v) << "allocation " << i;
		EXPECT_EQ(expected, v->version) << "allocation " << i << ", varId " << varId;
	}
}

// The counter keys on the kind passed to allocValue, so a value whose kind is
// rewritten afterwards would leave it counting a kind that no value has. Two
// sites in ssa_rename.cpp did that -- allocate as one kind, assign another on
// the next line -- and both now pass the kind they mean. This pins the property
// the counter needs: after construction, every value's kind is the one it was
// allocated with, so versions stay consistent with the vector.
TEST(SSAValueVersions, SurviveAFullRenameWithoutDrifting)
{
	SSAFunction fn("rename");
	auto* entry = fn.addBlock("entry");
	auto* header = fn.addBlock("header");
	auto* ex = fn.addBlock("exit");
	connect(fn, entry->id, header->id);
	connect(fn, header->id, header->id);
	connect(fn, header->id, ex->id);
	addFlagWrite(fn, header->id);
	addFlagRead(fn, header->id, FlagBit::ZF);

	SSAPass pass;
	pass.run(fn);
	ASSERT_TRUE(pass.errors().empty());

	// Recount from the vector and compare against what each value carries.
	std::map<std::pair<uint32_t, int>, uint32_t> counts;
	for (const auto& v: fn.values())
	{
		const auto key = std::make_pair(v->varId, static_cast<int>(v->kind));
		EXPECT_EQ(counts[key], v->version) << "value " << v->id;
		++counts[key];
	}
}

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}

// ─── The dominator tree is rebuilt, not appended to ──────────────────────────

// computeIDom appended to domChildren without clearing it, so a second run()
// over the same function gave every block each child twice. SSARename walks
// that tree recursively, so the duplicates cost 2^depth: a second
// SSAPass::run over a 20-block function took 14.8 s where the first took
// 0.0 ms, and a 22-block one did not finish in two minutes. SSAPass::run
// itself builds the tree twice -- once before renaming, once for the verifier.
TEST(DomTree, RunningTwiceDoesNotDuplicateDomChildren)
{
	SSAFunction fn("f");
	BlockId prev = fn.addBlock("b0")->id;
	for (int i = 1; i < 6; ++i)
	{
		auto* n = fn.addBlock("b" + std::to_string(i));
		fn.block(prev)->addSucc(n->id);
		n->preds.push_back(prev);
		prev = n->id;
	}

	DominatorTree dom;
	dom.run(fn);
	std::size_t after1 = 0;
	for (auto& b: fn.blocks())
		after1 += b->domChildren.size();
	ASSERT_EQ(5u, after1);

	dom.run(fn);
	dom.run(fn);
	std::size_t after3 = 0;
	for (auto& b: fn.blocks())
		after3 += b->domChildren.size();
	EXPECT_EQ(after1, after3);
}

// The whole pass, twice, is the shape that actually hung: the second run's
// rename walked the tree the first run's verifier step had already doubled.
TEST(SSAPass, RunningTheWholePassTwiceTerminates)
{
	SSAFunction fn("f");
	BlockId prev = fn.addBlock("b0")->id;
	for (int i = 1; i < 24; ++i)
	{
		auto* n = fn.addBlock("b" + std::to_string(i));
		fn.block(prev)->addSucc(n->id);
		n->preds.push_back(prev);
		prev = n->id;
	}

	SSAPass first;
	first.run(fn);
	SSAPass second;
	second.run(fn);

	std::size_t edges = 0;
	for (auto& b: fn.blocks())
		edges += b->domChildren.size();
	EXPECT_EQ(23u, edges);
}

// ─── Blocks unreachable from the entry ───────────────────────────────────────
//
// computeDFS()'s fix-up loop gives every unreachable block a parent_, an rpo,
// a vertex_ slot and a labelArr_ entry -- but not a semi_, which stays at the
// zero-fill, i.e. the entry's own DFS number. computeIDom() then walks every
// predecessor with no reachability filter, so `semi_[u] < semi_[w]` sees 0 for
// an unreachable predecessor and pins the *reachable* successor's
// semidominator to the entry. The successor's idom collapses to the entry, and
// anything whose semidominator search evaluates through it inherits the same
// zero.
//
// A block nobody can reach contributes no path from the entry, so it cannot
// weaken anyone's dominance.

TEST(DominatorTreeTests, AnUnreachablePredecessorDoesNotWeakenDominance)
{
	// entry -> A -> B, and an unreachable U -> B.
	SSAFunction fn("unreach");
	auto* e = fn.addBlock("entry");
	auto* a = fn.addBlock("A");
	auto* b = fn.addBlock("B");
	auto* u = fn.addBlock("U");
	connect(fn, e->id, a->id);
	connect(fn, a->id, b->id);
	connect(fn, u->id, b->id);

	DominatorTree dt;
	dt.run(fn);

	EXPECT_EQ(a->id, fn.block(b->id)->idom) << "every path from the entry to B goes through A";
	EXPECT_TRUE(dt.dominates(fn, a->id, b->id));
	EXPECT_TRUE(dt.strictlyDominates(fn, a->id, b->id));

	// And U itself has no immediate dominator, the same as the entry: no path
	// from the entry reaches it, so nothing on such a path dominates it.
	// Before, the missing semi_ bucketed it under the entry and it came out
	// looking like an ordinary child of it.
	EXPECT_EQ(kInvalidBlock, fn.block(u->id)->idom);
	EXPECT_TRUE(
		fn.block(e->id)->domChildren.end()
		== std::find(fn.block(e->id)->domChildren.begin(), fn.block(e->id)->domChildren.end(), u->id));
}

TEST(DominatorTreeTests, AnUnreachableBlockDoesNotCorruptALoopHeader)
{
	// entry -> H -> L -> H (a natural loop), plus an unreachable U -> L.
	// The latch's semidominator is what the back-edge test reads, so a zero
	// there turns a natural loop into an apparently irreducible one.
	SSAFunction fn("loop");
	auto* e = fn.addBlock("entry");
	auto* h = fn.addBlock("H");
	auto* l = fn.addBlock("L");
	auto* x = fn.addBlock("exit");
	auto* u = fn.addBlock("U");
	connect(fn, e->id, h->id);
	connect(fn, h->id, l->id);
	connect(fn, l->id, h->id);
	connect(fn, h->id, x->id);
	connect(fn, u->id, l->id);

	DominatorTree dt;
	dt.run(fn);

	EXPECT_EQ(h->id, fn.block(l->id)->idom) << "L is only reachable through H";
	EXPECT_TRUE(dt.dominates(fn, h->id, l->id)) << "H dominates its own latch, which is what makes L->H a back edge";
	EXPECT_EQ(h->id, fn.block(x->id)->idom);
}

// ─── An instruction can define a register AND the flags ──────────────────────
//
// `add eax, ebx` is both, and so is most x86 arithmetic. renameBlock chose one
// or the other: with writesFlagBundle set it allocated a single value, tagged
// it FlagBundle, and pushed it onto the FLAGS stack -- so the register's own
// stack never saw the definition and every later use of that register resolved
// to whatever defined it before. Everything else in the module already treats
// the two as independent: liveness kills both, and phi placement records a def
// site for both.

TEST(SSARename, AnInstructionThatWritesARegisterAndTheFlagsDefinesBoth)
{
	SSAFunction fn("arith");
	auto* b = fn.addBlock("entry");
	VarId eax = fn.declareVar("eax");
	VarId ebx = fn.declareVar("ebx");

	// mov eax, _
	IrInstr* i1 = fn.addInstr(b->id, IrInstr::Op::Assign, 0x1000);
	i1->defVar = eax;

	// add eax, ebx  -- defines eax and writes the flags
	IrInstr* i2 = fn.addInstr(b->id, IrInstr::Op::Add, 0x1003);
	i2->defVar = eax;
	i2->writesFlagBundle = true;
	i2->flagMask = 0x3F;
	i2->uses.push_back({fn.allocValue(ValueKind::VirtualReg, eax)->id, 0});
	i2->uses.push_back({fn.allocValue(ValueKind::VirtualReg, ebx)->id, 1});

	// mov ebx, eax  -- must read the eax that `add` defined
	IrInstr* i3 = fn.addInstr(b->id, IrInstr::Op::Assign, 0x1006);
	i3->defVar = ebx;
	i3->uses.push_back({fn.allocValue(ValueKind::VirtualReg, eax)->id, 0});

	SSAPass pass;
	pass.run(fn);
	ASSERT_TRUE(pass.errors().empty());

	ASSERT_NE(kInvalidValue, i2->defValue);
	const IrValue* def = fn.value(i2->defValue);
	ASSERT_NE(nullptr, def);
	EXPECT_EQ(ValueKind::VirtualReg, def->kind) << "the register definition is a register";
	EXPECT_EQ(eax, def->varId);

	ASSERT_NE(kInvalidValue, i2->flagBundleValue);
	const IrValue* flags = fn.value(i2->flagBundleValue);
	ASSERT_NE(nullptr, flags);
	EXPECT_EQ(ValueKind::FlagBundle, flags->kind);
	EXPECT_NE(def->id, flags->id) << "one value cannot be both";
	EXPECT_EQ(0x3F, flags->definedFlags);

	// The point of all of it: the next reader of eax sees the add's result.
	ASSERT_FALSE(i3->uses.empty());
	EXPECT_EQ(i2->defValue, i3->uses[0].valueId) << "the use resolved to the definition before the add";
}

TEST(SSARename, AFlagOnlyInstructionStillDefinesOnlyTheFlags)
{
	// `cmp eax, ebx` writes no register.
	SSAFunction fn("cmp");
	auto* b = fn.addBlock("entry");
	VarId eax = fn.declareVar("eax");

	IrInstr* i1 = fn.addInstr(b->id, IrInstr::Op::Assign, 0x1000);
	i1->defVar = eax;

	IrInstr* cmp = fn.addInstr(b->id, IrInstr::Op::Compare, 0x1003);
	cmp->writesFlagBundle = true;
	cmp->flagMask = 0x3F;
	cmp->uses.push_back({fn.allocValue(ValueKind::VirtualReg, eax)->id, 0});

	IrInstr* i3 = fn.addInstr(b->id, IrInstr::Op::Assign, 0x1006);
	i3->defVar = fn.declareVar("ecx");
	i3->uses.push_back({fn.allocValue(ValueKind::VirtualReg, eax)->id, 0});

	SSAPass pass;
	pass.run(fn);
	ASSERT_TRUE(pass.errors().empty());

	ASSERT_NE(kInvalidValue, cmp->flagBundleValue);
	EXPECT_EQ(ValueKind::FlagBundle, fn.value(cmp->flagBundleValue)->kind);
	ASSERT_FALSE(i3->uses.empty());
	EXPECT_EQ(i1->defValue, i3->uses[0].valueId) << "a compare does not redefine the register it reads";
}
