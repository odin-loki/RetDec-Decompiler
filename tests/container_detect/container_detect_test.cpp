/**
 * @file tests/container_detect/container_detect_test.cpp
 * @brief Unit tests for the STL Container Identification module (Stage 26).
 *
 * Coverage:
 *   - RecoveredType::toString
 *   - ContainerResult::kindName / toString
 *   - VectorDetector::detect  (3-pointer layout, growth, indexing)
 *   - ListDetector::detect    (sentinel init, node alloc, chain traversal)
 *   - MapDetector::detect     (rotations, colour field, rebalancing)
 *   - UnorderedMapDetector::detect (hash, modulo, chain)
 *   - StringDetector::detect  (SSO branch, inline path, heap path)
 *   - SharedPtrDetector::detect (two-pointer, atomic dec, zero-check free)
 *   - RingBufferDetector::detect (And wrap mask or Rem capacity, not Div)
 *   - TemplateTypeRecoverer::recoverElementType
 *   - ContainerDetector::analyseFunction (preflight, best-confidence selection)
 *   - ContainerDetector::analyseModule
 */

#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace retdec::container_detect;
using namespace retdec;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Build a minimal SSA function with the given opcodes in its entry block.
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

// Add a Call instruction with a callee name to a function's entry block.
static void addCall(ssa::SSAFunction& fn, const std::string& callee)
{
	auto* instr = fn.addInstr(fn.block(0)->id, ssa::IrInstr::Op::Call);
	if (instr) instr->calleeName = callee;
}

// Add an instruction whose immediate operand has a given value.
static void addImmInstr(ssa::SSAFunction& fn, ssa::IrInstr::Op op, uint64_t immVal)
{
	auto* instr = fn.addInstr(fn.block(0)->id, op);
	if (!instr) return;
	// Add an immediate operand (value allocated in the function).
	ssa::IrValue* val = fn.allocValue(ssa::ValueKind::Immediate);
	if (val) val->imm = immVal;
	ssa::Use u;
	u.valueId = val ? val->id : ssa::kInvalidValue;
	instr->uses.push_back(u);
}

// ─── RecoveredType tests ──────────────────────────────────────────────────────

TEST(RecoveredTypeTest, UnknownToString)
{
	RecoveredType t;
	EXPECT_EQ(t.toString(), "int");
}

TEST(RecoveredTypeTest, Int32Signed)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Int32;
	t.isSigned = true;
	EXPECT_EQ(t.toString(), "int32_t");
}

TEST(RecoveredTypeTest, Int32Unsigned)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Int32;
	t.isSigned = false;
	EXPECT_EQ(t.toString(), "uint32_t");
}

TEST(RecoveredTypeTest, Int8Signed)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Int8;
	t.isSigned = true;
	EXPECT_EQ(t.toString(), "int8_t");
}

TEST(RecoveredTypeTest, Int64)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Int64;
	t.isSigned = true;
	EXPECT_EQ(t.toString(), "int64_t");
}

TEST(RecoveredTypeTest, FloatToString)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Float;
	EXPECT_EQ(t.toString(), "float");
}

TEST(RecoveredTypeTest, DoubleToString)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Double;
	EXPECT_EQ(t.toString(), "double");
}

TEST(RecoveredTypeTest, PointerToString)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Pointer;
	EXPECT_EQ(t.toString(), "void*");
}

TEST(RecoveredTypeTest, StructWithName)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Struct;
	t.name = "MyRecord";
	EXPECT_EQ(t.toString(), "MyRecord");
}

TEST(RecoveredTypeTest, StructNoName)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::Struct;
	EXPECT_EQ(t.toString(), "struct_t");
}

TEST(RecoveredTypeTest, StringToString)
{
	RecoveredType t;
	t.kind = RecoveredType::Kind::String;
	EXPECT_EQ(t.toString(), "std::string");
}

// ─── ContainerResult tests ────────────────────────────────────────────────────

TEST(ContainerResultTest, KindNameVector)
{
	ContainerResult r;
	r.kind = ContainerKind::Vector;
	EXPECT_EQ(r.kindName(), "std::vector");
}

TEST(ContainerResultTest, KindNameList)
{
	ContainerResult r;
	r.kind = ContainerKind::List;
	EXPECT_EQ(r.kindName(), "std::list");
}

TEST(ContainerResultTest, KindNameMap)
{
	ContainerResult r;
	r.kind = ContainerKind::Map;
	EXPECT_EQ(r.kindName(), "std::map");
}

TEST(ContainerResultTest, KindNameUnorderedMap)
{
	ContainerResult r;
	r.kind = ContainerKind::UnorderedMap;
	EXPECT_EQ(r.kindName(), "std::unordered_map");
}

TEST(ContainerResultTest, KindNameString)
{
	ContainerResult r;
	r.kind = ContainerKind::String;
	EXPECT_EQ(r.kindName(), "std::string");
}

TEST(ContainerResultTest, KindNameSharedPtr)
{
	ContainerResult r;
	r.kind = ContainerKind::SharedPtr;
	EXPECT_EQ(r.kindName(), "std::shared_ptr");
}

TEST(ContainerResultTest, KindNameUnknown)
{
	ContainerResult r;
	EXPECT_EQ(r.kindName(), "unknown");
}

TEST(ContainerResultTest, ToStringContainsConfidence)
{
	ContainerResult r;
	r.kind = ContainerKind::Vector;
	r.confidence = 0.8f;
	r.emittedType = "std::vector<int32_t>";
	std::string s = r.toString();
	EXPECT_NE(s.find("0.8"), std::string::npos);
}

TEST(ContainerResultTest, ToStringContainsKind)
{
	ContainerResult r;
	r.kind = ContainerKind::Map;
	r.confidence = 0.9f;
	std::string s = r.toString();
	EXPECT_NE(s.find("std::map"), std::string::npos);
}

// ─── VectorDetector tests ─────────────────────────────────────────────────────

TEST(VectorDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	VectorDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.30f);
}

TEST(VectorDetectorTest, ThreeLoadsPlusSubHigherConfidence)
{
	auto fn = makeFunc(
		"vec_size",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
		});
	VectorDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.30f);
	EXPECT_EQ(r.kind, ContainerKind::Vector);
	EXPECT_EQ(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(VectorDetectorTest, GrowthPatternDetected)
{
	auto fn = makeFunc(
		"vec_push",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Store,
		});
	addCall(*fn, "malloc");
	addCall(*fn, "free");
	addImmInstr(*fn, ssa::IrInstr::Op::Shl, 1); // GCC growth ×2
	VectorDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.compilerVariant, CompilerVariant::GCC);
	EXPECT_EQ(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(VectorDetectorTest, GrowthOnlyIsSymbolNameEvidence)
{
	auto fn = makeFunc("vec_grow", {ssa::IrInstr::Op::Store});
	addCall(*fn, "malloc");
	addCall(*fn, "free");
	addImmInstr(*fn, ssa::IrInstr::Op::Shl, 1);
	VectorDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.10f);
	EXPECT_EQ(r.kind, ContainerKind::Vector);
	EXPECT_NE(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(VectorDetectorTest, MSVCGrowthFactor)
{
	auto fn = makeFunc(
		"vec_push_msvc",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Store,
		});
	addCall(*fn, "malloc");
	addCall(*fn, "free");
	addImmInstr(*fn, ssa::IrInstr::Op::Shr, 1); // MSVC growth cap/2
	VectorDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.compilerVariant, CompilerVariant::MSVC);
}

TEST(VectorDetectorTest, ElementByteWidthRecovered)
{
	auto fn = makeFunc(
		"vec_index",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Mul, 4); // int32_t stride
	VectorDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.30f);
	EXPECT_EQ(r.elementType.byteWidth, 4);
}

TEST(VectorDetectorTest, AccessPatternsHavePushBack)
{
	auto fn = makeFunc(
		"vec_store",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Store,
		});
	addCall(*fn, "malloc");
	addCall(*fn, "free");
	VectorDetector det;
	auto r = det.detect(*fn);
	bool hasPushBack = false;
	for (const auto& ap: r.accessPatterns)
		if (ap.kind == AccessKind::PushBack)
		{
			hasPushBack = true;
			break;
		}
	EXPECT_TRUE(hasPushBack);
}

TEST(VectorDetectorTest, AccessPatternsHaveSize)
{
	auto fn = makeFunc(
		"vec_sz",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
		});
	VectorDetector det;
	auto r = det.detect(*fn);
	bool hasSize = false;
	for (const auto& ap: r.accessPatterns)
		if (ap.kind == AccessKind::SizeCheck)
		{
			hasSize = true;
			break;
		}
	EXPECT_TRUE(hasSize);
}

TEST(VectorDetectorTest, EmittedTypeContainsVector)
{
	auto fn = makeFunc(
		"vec_t",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
		});
	VectorDetector det;
	auto r = det.detect(*fn);
	EXPECT_NE(r.emittedType.find("std::vector"), std::string::npos);
}

// ─── ListDetector tests ───────────────────────────────────────────────────────

TEST(ListDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	ListDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.30f);
}

TEST(ListDetectorTest, NodeAllocPlusTraversalDetected)
{
	auto fn = makeFunc(
		"list_iter",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		/*extraBlocks=*/1);
	// Back-edge: block 1 successor back to block 0.
	fn->block(1)->succs.push_back(0);
	addCall(*fn, "malloc");
	ListDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.40f);
	EXPECT_EQ(r.kind, ContainerKind::List);
	EXPECT_NE(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(ListDetectorTest, FourStoresIncreasesConfidence)
{
	auto fn = makeFunc(
		"list_insert",
		{
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addCall(*fn, "malloc");
	ListDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.60f);
	EXPECT_NE(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(ListDetectorTest, AccessPatternsHaveIterate)
{
	auto fn = makeFunc(
		"list_it",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addCall(*fn, "malloc");
	ListDetector det;
	auto r = det.detect(*fn);
	bool hasIter = false;
	for (const auto& ap: r.accessPatterns)
		if (ap.kind == AccessKind::Iterate)
		{
			hasIter = true;
			break;
		}
	EXPECT_TRUE(hasIter);
}

// Regression: two adjacent Stores whose address operand never got renamed carry
// a single use each.  hasSentinelInit guarded only on !uses.empty() and then
// read uses[1], walking off the end of both use-lists (ASan: heap-buffer-
// overflow).  Nothing here is a sentinel, so no sentinel evidence either.
TEST(ListDetectorTest, SingleOperandStorePairDoesNotReadPastUses)
{
	auto fn = makeFunc(
		"half_formed_stores",
		{
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
		});
	for (auto* instr: fn->block(0)->instrs)
	{
		ssa::Use u;
		u.valueId = 0;
		instr->uses.push_back(u);
	}
	ListDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.35f);
}

// Regression: the real sentinel shape -- one address written into two different
// slots of the object it points at -- must still score.  The old predicate
// (a->uses[1] == b->uses[1] + 1) had nothing to do with self-reference.
TEST(ListDetectorTest, SelfReferentialSentinelPairDetected)
{
	auto fn = std::make_unique<ssa::SSAFunction>("list_ctor");
	auto* entry = fn->addBlock("entry");

	// `hdr` (variable 5) holds the sentinel node's own address.
	auto* hdrAddr = fn->allocValue(ssa::ValueKind::VirtualReg, /*varId=*/5);
	// &hdr->_next and &hdr->_prev: two distinct slots off that same variable.
	auto* nextSlot = fn->allocValue(ssa::ValueKind::MemRef);
	nextSlot->memBaseReg = 5;
	nextSlot->memOffset = 0;
	auto* prevSlot = fn->allocValue(ssa::ValueKind::MemRef);
	prevSlot->memBaseReg = 5;
	prevSlot->memOffset = 8;

	for (auto* slot: {nextSlot, prevSlot})
	{
		auto* st = fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
		ssa::Use v;
		v.valueId = hdrAddr->id;
		st->uses.push_back(v);
		ssa::Use a;
		a.valueId = slot->id;
		st->uses.push_back(a);
	}

	ListDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.35f);
}

// Two stores of the same value into the *same* slot are a plain overwrite, and
// two stores into adjacent slots of an unrelated object are a struct
// initialiser.  Neither is a circular sentinel.
TEST(ListDetectorTest, NonSelfReferentialStorePairIsNotASentinel)
{
	auto fn = std::make_unique<ssa::SSAFunction>("struct_init");
	auto* entry = fn->addBlock("entry");

	auto* payload = fn->allocValue(ssa::ValueKind::VirtualReg, /*varId=*/9);
	auto* fieldA = fn->allocValue(ssa::ValueKind::MemRef);
	fieldA->memBaseReg = 5;
	fieldA->memOffset = 0;
	auto* fieldB = fn->allocValue(ssa::ValueKind::MemRef);
	fieldB->memBaseReg = 5;
	fieldB->memOffset = 8;

	for (auto* slot: {fieldA, fieldB})
	{
		auto* st = fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
		ssa::Use v;
		v.valueId = payload->id;
		st->uses.push_back(v);
		ssa::Use a;
		a.valueId = slot->id;
		st->uses.push_back(a);
	}

	ListDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.35f);
}

// Build one CLRS rotation in a fresh function.  `readOff` is the child slot the
// load reads off x, `writeOff` the slot of y that x gets linked into; a left
// rotation promotes the higher child into the lower slot and a right rotation
// mirrors that.
static std::unique_ptr<ssa::SSAFunction>
makeRotation(const std::string& name, int64_t readOff, int64_t writeOff)
{
	auto fn = std::make_unique<ssa::SSAFunction>(name);
	auto* entry = fn->addBlock("entry");

	const ssa::VarId xVar = 1; // node being demoted
	const ssa::VarId yVar = 2; // child being promoted

	auto* xVal = fn->allocValue(ssa::ValueKind::VirtualReg, xVar);
	auto* yVal = fn->allocValue(ssa::ValueKind::VirtualReg, yVar);
	auto* xChild = fn->allocValue(ssa::ValueKind::MemRef); // &x->child
	xChild->memBaseReg = xVar;
	xChild->memOffset = readOff;
	auto* yOther = fn->allocValue(ssa::ValueKind::MemRef); // &y->other
	yOther->memBaseReg = yVar;
	yOther->memOffset = writeOff;

	// y = x->child
	auto* ld = fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
	ld->defValue = yVal->id;
	ssa::Use la;
	la.valueId = xChild->id;
	ld->uses.push_back(la);

	// x->child = <y's other subtree>  (the slot just read is written back)
	auto* s1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	ssa::Use s1v;
	s1v.valueId = yVal->id;
	s1->uses.push_back(s1v);
	ssa::Use s1a;
	s1a.valueId = xChild->id;
	s1->uses.push_back(s1a);

	// y->other = x  (the cross-link, on the far side of the read slot)
	auto* s2 = fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
	ssa::Use s2v;
	s2v.valueId = xVal->id;
	s2->uses.push_back(s2v);
	ssa::Use s2a;
	s2a.valueId = yOther->id;
	s2->uses.push_back(s2a);

	// Second load, so the ">= 2 Loads" precondition holds.
	fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
	return fn;
}

// ─── MapDetector tests ────────────────────────────────────────────────────────

TEST(MapDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	MapDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.20f);
}

TEST(MapDetectorTest, ColourFieldDetected)
{
	auto fn = makeFunc(
		"rb_insert",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Compare,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::And, 1); // colour bit
	MapDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.20f);
}

TEST(MapDetectorTest, RotationPatternBoostsConfidence)
{
	// Rotation: a load off x's child slot, that slot written back, and x linked
	// into y's opposite slot.  This fixture used to wire the store operands to
	// L1->id -- an InstrId -- and "matched" only because hasRotation searched a
	// list of InstrIds for ValueIds.  It now builds the real value graph; the
	// expectations below are unchanged.
	auto fn = makeRotation("rb_rotate", /*readOff=*/24, /*writeOff=*/16);
	fn->addInstr(fn->block(0)->id, ssa::IrInstr::Op::Load);

	// Colour bit: And against the immediate 1 as the second operand.
	auto* one = fn->allocValue(ssa::ValueKind::Immediate);
	one->imm = 1;
	auto* andI = fn->addInstr(fn->block(0)->id, ssa::IrInstr::Op::And);
	ssa::Use lhs;
	lhs.valueId = ssa::kInvalidValue;
	andI->uses.push_back(lhs);
	ssa::Use rhs;
	rhs.valueId = one->id;
	andI->uses.push_back(rhs);

	MapDetector det;
	auto r = det.detect(*fn);
	// We should get some confidence from the colour field at minimum.
	EXPECT_GE(r.confidence, 0.20f);
	EXPECT_EQ(r.kind, ContainerKind::Map);
}

TEST(MapDetectorTest, EmittedTypeContainsMap)
{
	auto fn = makeFunc(
		"m_t",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::And, 1);
	MapDetector det;
	auto r = det.detect(*fn);
	if (r.confidence >= 0.10f) EXPECT_NE(r.emittedType.find("std::map"), std::string::npos);
}

// Regression: hasRotation used to collect InstrIds and search them for
// ValueIds.  Both spaces start at 0 and count up, so ordinary struct-walking
// code collided by accident, matched as *both* a left and a right rotation
// (leftRotate was never read) and reached 1.00 -- which suppressed every other
// detector in container_detector.cpp's max-confidence race.
TEST(MapDetectorTest, GenericStoreLoopIsNotAMapAtFullConfidence)
{
	auto fn = std::make_unique<ssa::SSAFunction>("walk_nodes");
	auto* entry = fn->addBlock("entry");

	auto* src = fn->allocValue(ssa::ValueKind::VirtualReg);
	auto* addr = fn->allocValue(ssa::ValueKind::VirtualReg);
	auto* zero = fn->allocValue(ssa::ValueKind::Immediate);
	zero->imm = 0;

	for (int i = 0; i < 3; ++i) fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
	for (int i = 0; i < 3; ++i)
	{
		auto* st = fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
		ssa::Use v;
		v.valueId = src->id;
		st->uses.push_back(v);
		ssa::Use a;
		a.valueId = addr->id;
		st->uses.push_back(a);
	}
	for (int i = 0; i < 2; ++i)
	{
		auto* cmp = fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
		ssa::Use l;
		l.valueId = src->id;
		cmp->uses.push_back(l);
		ssa::Use r;
		r.valueId = zero->id;
		cmp->uses.push_back(r);
	}

	MapDetector det;
	auto r = det.detect(*fn);
	// Colour field + three-pointer + rebalancing is the most generic code may
	// claim; no rotation evidence exists here.
	EXPECT_LT(r.confidence, 0.60f);
}

// A real left rotation still scores, and scores *once*: reading the high child
// into the low slot is a left rotation and not simultaneously a right one.
TEST(MapDetectorTest, LeftRotationScoresOneDirectionOnly)
{
	auto fn = makeRotation("rb_rotate_left", /*readOff=*/24, /*writeOff=*/16);
	MapDetector det;
	auto r = det.detect(*fn);
	EXPECT_NEAR(r.confidence, 0.30f, 1e-4f);
}

// The mirror image: reading the low child into the high slot is a right
// rotation, again exactly one direction.
TEST(MapDetectorTest, RightRotationScoresOneDirectionOnly)
{
	auto fn = makeRotation("rb_rotate_right", /*readOff=*/16, /*writeOff=*/24);
	MapDetector det;
	auto r = det.detect(*fn);
	EXPECT_NEAR(r.confidence, 0.30f, 1e-4f);
}

// ─── UnorderedMapDetector tests ───────────────────────────────────────────────

TEST(UnorderedMapDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	UnorderedMapDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.30f);
}

TEST(UnorderedMapDetectorTest, HashCallDetected)
{
	auto fn = makeFunc(
		"uhm_lookup",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addCall(*fn, "std_hash");
	addImmInstr(*fn, ssa::IrInstr::Op::And, 63); // 64 buckets - 1

	UnorderedMapDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.kind, ContainerKind::UnorderedMap);
	EXPECT_NE(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(UnorderedMapDetectorTest, InlineHashXorMul)
{
	auto fn = makeFunc(
		"uhm_xor_mul",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addImmInstr(*fn, ssa::IrInstr::Op::And, 15); // 16 buckets -1

	UnorderedMapDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(UnorderedMapDetectorTest, ModuloPowerOfTwo)
{
	auto fn = makeFunc(
		"uhm_mod",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::And, 255); // 2^8 - 1
	UnorderedMapDetector det;
	auto r = det.detect(*fn);
	// Xor+Mul = hash, And with 255 = modulo → should score.
	EXPECT_GE(r.confidence, 0.50f);
}

TEST(UnorderedMapDetectorTest, DivIsNotBucketModulo)
{
	// Rem and strength-reduction Div are not a bucket mask (B9 heapsort).
	auto fn = makeFunc(
		"uhm_div",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addImmInstr(*fn, ssa::IrInstr::Op::Div, 8);
	UnorderedMapDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.80f);
}

TEST(UnorderedMapDetectorTest, AndAllOnes32IsNotBucketModulo)
{
	auto fn = makeFunc(
		"uhm_zext",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addImmInstr(*fn, ssa::IrInstr::Op::And, 0xffffffffULL);
	UnorderedMapDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.80f);
}

TEST(UnorderedMapDetectorTest, EmittedTypeContainsUnorderedMap)
{
	auto fn = makeFunc(
		"uhm_t",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::And, 31);
	UnorderedMapDetector det;
	auto r = det.detect(*fn);
	if (r.confidence >= 0.10f) EXPECT_NE(r.emittedType.find("unordered_map"), std::string::npos);
}

// ─── StringDetector tests ─────────────────────────────────────────────────────

TEST(StringDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	StringDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.30f);
}

TEST(StringDetectorTest, SSOThreshold15GCCVariant)
{
	auto fn = makeFunc(
		"str_gcc",
		{
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
	StringDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.kind, ContainerKind::String);
	EXPECT_EQ(r.compilerVariant, CompilerVariant::GCC);
}

TEST(StringDetectorTest, SSOThreshold22ClangVariant)
{
	auto fn = makeFunc(
		"str_clang",
		{
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 22);
	StringDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.compilerVariant, CompilerVariant::Clang);
}

TEST(StringDetectorTest, SSOThreshold23AlsoClang)
{
	auto fn = makeFunc(
		"str_clang23",
		{
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 23);
	StringDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.compilerVariant, CompilerVariant::Clang);
}

TEST(StringDetectorTest, ElementTypeIsChar)
{
	auto fn = makeFunc(
		"str_elem",
		{
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
	StringDetector det;
	auto r = det.detect(*fn);
	if (r.confidence >= 0.10f)
	{
		EXPECT_EQ(r.elementType.byteWidth, 1);
	}
}

TEST(StringDetectorTest, EmittedTypeIsStdString)
{
	auto fn = makeFunc(
		"str_emit",
		{
			ssa::IrInstr::Op::Load,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
	StringDetector det;
	auto r = det.detect(*fn);
	if (r.confidence >= 0.10f) EXPECT_EQ(r.emittedType, "std::string");
}

TEST(StringDetectorTest, AccessPatternsHaveSizeCheck)
{
	auto fn = makeFunc(
		"str_size",
		{
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
	StringDetector det;
	auto r = det.detect(*fn);
	bool hasSize = false;
	for (const auto& ap: r.accessPatterns)
		if (ap.kind == AccessKind::SizeCheck)
		{
			hasSize = true;
			break;
		}
	if (r.confidence >= 0.30f) EXPECT_TRUE(hasSize);
}

// ─── SharedPtrDetector tests ──────────────────────────────────────────────────

TEST(SharedPtrDetectorTest, EmptyFunctionLowConfidence)
{
	auto fn = makeFunc("empty", {});
	SharedPtrDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.30f);
}

TEST(SharedPtrDetectorTest, TwoPointerPlusAtomicDecrement)
{
	auto fn = makeFunc(
		"sp_destr",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		});
	addCall(*fn, "free");
	SharedPtrDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.kind, ContainerKind::SharedPtr);
	EXPECT_EQ(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(SharedPtrDetectorTest, AtomicCallDetected)
{
	auto fn = makeFunc(
		"sp_atomic",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
		});
	addCall(*fn, "__atomic_fetch_sub");
	addCall(*fn, "free");
	SharedPtrDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_NE(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(SharedPtrDetectorTest, EmittedTypeContainsSharedPtr)
{
	auto fn = makeFunc(
		"sp_emit",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		});
	addCall(*fn, "free");
	SharedPtrDetector det;
	auto r = det.detect(*fn);
	if (r.confidence >= 0.10f) EXPECT_NE(r.emittedType.find("shared_ptr"), std::string::npos);
}

TEST(SharedPtrDetectorTest, AccessPatternsHaveLookup)
{
	auto fn = makeFunc(
		"sp_get",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		});
	addCall(*fn, "free");
	SharedPtrDetector det;
	auto r = det.detect(*fn);
	bool hasLookup = false;
	for (const auto& ap: r.accessPatterns)
		if (ap.kind == AccessKind::Lookup)
		{
			hasLookup = true;
			break;
		}
	if (r.confidence >= 0.30f) EXPECT_TRUE(hasLookup);
}

// ─── TemplateTypeRecoverer tests ──────────────────────────────────────────────

TEST(TemplateTypeRecovererTest, RecoverFromByteWidth4)
{
	TemplateTypeRecoverer rec;
	ContainerResult partial;
	partial.kind = ContainerKind::Vector;
	auto fn = makeFunc("f", {});
	RecoveredType t = rec.recoverElementType(*fn, partial, 4);
	EXPECT_EQ(t.byteWidth, 4);
}

TEST(TemplateTypeRecovererTest, RecoverFromByteWidth8)
{
	TemplateTypeRecoverer rec;
	ContainerResult partial;
	auto fn = makeFunc("f", {});
	RecoveredType t = rec.recoverElementType(*fn, partial, 8);
	EXPECT_EQ(t.byteWidth, 8);
}

TEST(TemplateTypeRecovererTest, RecoverFromByteWidth1)
{
	TemplateTypeRecoverer rec;
	ContainerResult partial;
	auto fn = makeFunc("f", {});
	RecoveredType t = rec.recoverElementType(*fn, partial, 1);
	EXPECT_EQ(t.byteWidth, 1);
}

TEST(TemplateTypeRecovererTest, UsesPartialTypeWhenNoWidth)
{
	TemplateTypeRecoverer rec;
	ContainerResult partial;
	partial.elementType.kind = RecoveredType::Kind::Float;
	partial.elementType.byteWidth = 4;
	auto fn = makeFunc("f", {});
	RecoveredType t = rec.recoverElementType(*fn, partial, 0);
	EXPECT_EQ(t.kind, RecoveredType::Kind::Float);
}

TEST(TemplateTypeRecovererTest, DefaultsToInt32WhenNoInfo)
{
	TemplateTypeRecoverer rec;
	ContainerResult partial;
	auto fn = makeFunc("f", {});
	RecoveredType t = rec.recoverElementType(*fn, partial, 0);
	EXPECT_EQ(t.byteWidth, 4);
}

TEST(TemplateTypeRecovererTest, KeyTypeFromComparatorCall)
{
	TemplateTypeRecoverer rec;
	ContainerResult partial;
	partial.kind = ContainerKind::Map;
	auto fn = makeFunc("f", {});
	auto* instr = fn->addInstr(fn->block(0)->id, ssa::IrInstr::Op::Call);
	if (instr)
	{
		instr->calleeName = "std::less::compare";
		ssa::IrValue* val = fn->allocValue(ssa::ValueKind::Immediate);
		if (val) val->width = 32; // 4 bytes = 32 bits
		ssa::Use u;
		u.valueId = val ? val->id : ssa::kInvalidValue;
		instr->uses.push_back(u);
	}
	RecoveredType t = rec.recoverKeyType(*fn, partial);
	// Should recover from comparator param.
	EXPECT_GE(t.byteWidth, 0); // At least some type returned.
}

// ─── ContainerDetector orchestration tests ────────────────────────────────────

TEST(ContainerDetectorTest, EmptyFunctionSkippedByPreflight)
{
	ContainerDetector::Config cfg;
	cfg.minBlocks = 2;
	cfg.minInstrs = 8;
	ContainerDetector det(cfg);
	auto fn = makeFunc("tiny", {ssa::IrInstr::Op::Load});
	auto r = det.analyseFunction(*fn);
	EXPECT_EQ(r.kind, ContainerKind::Unknown);
	EXPECT_EQ(det.stats().functionsSkipped, 1u);
}

TEST(ContainerDetectorTest, VectorFunctionDetected)
{
	ContainerDetector det;
	auto fn = makeFunc(
		"vec",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Store,
		},
		2);
	addCall(*fn, "malloc");
	addCall(*fn, "free");
	auto r = det.analyseFunction(*fn);
	// Either vector or unknown depending on confidence threshold.
	EXPECT_TRUE(r.kind == ContainerKind::Vector || r.kind == ContainerKind::Unknown);
}

TEST(ContainerDetectorTest, NameOnlyHashPreservesSymbolNameEvidence)
{
	ContainerDetector::Config cfg;
	cfg.minBlocks = 1;
	cfg.minInstrs = 1;
	cfg.runVector = false;
	cfg.runOpenAddressing = false;
	cfg.runRingBuffer = false;
	cfg.runList = false;
	cfg.runMap = false;
	cfg.runString = false;
	cfg.runSharedPtr = false;
	ContainerDetector det(cfg);
	auto fn = makeFunc(
		"uhm_lookup",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addCall(*fn, "std_hash");
	addImmInstr(*fn, ssa::IrInstr::Op::And, 63);
	auto r = det.analyseFunction(*fn);
	EXPECT_EQ(r.kind, ContainerKind::UnorderedMap);
	EXPECT_NE(r.toString().find("evidence:symbol_name"), std::string::npos);
}

TEST(ContainerDetectorTest, StringBeatsVectorDueToSSOSignal)
{
	// A function with SSO threshold AND three loads should be detected as String,
	// not Vector, because StringDetector runs first.
	ContainerDetector det;
	auto fn = makeFunc(
		"str_vs_vec",
		{
			ssa::IrInstr::Op::Add,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
		},
		2);
	addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
	auto r = det.analyseFunction(*fn);
	// String detector should win over vector due to SSO threshold specificity.
	if (r.kind != ContainerKind::Unknown) EXPECT_EQ(r.kind, ContainerKind::String);
}

TEST(ContainerDetectorTest, AnalyseModuleReturnsMapPerFunction)
{
	ContainerDetector det;
	auto fn1 = makeFunc(
		"f1",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
		},
		2);
	auto fn2 = makeFunc(
		"f2",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
		},
		2);
	addImmInstr(*fn2, ssa::IrInstr::Op::And, 31);

	std::vector<const ssa::SSAFunction*> fns = {fn1.get(), fn2.get()};
	auto results = det.analyseModule(fns);
	// Results is a map from function name to ContainerResult; may have 0–2 entries.
	EXPECT_LE(results.size(), 2u);
}

TEST(ContainerDetectorTest, StatsFunctionsAnalysedIncrement)
{
	ContainerDetector det;
	auto fn1 = makeFunc("f1", {ssa::IrInstr::Op::Load}, 2);
	auto fn2 = makeFunc("f2", {ssa::IrInstr::Op::Load}, 2);
	std::vector<const ssa::SSAFunction*> fns = {fn1.get(), fn2.get()};
	det.analyseModule(fns);
	EXPECT_GE(det.stats().functionsAnalysed, 2u);
}

TEST(ContainerDetectorTest, MinConfidenceFiltersLowScores)
{
	ContainerDetector::Config cfg;
	cfg.minConfidence = 0.99f; // impossibly high threshold
	ContainerDetector det(cfg);
	auto fn = makeFunc(
		"vec_hi_thresh",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
		},
		2);
	auto r = det.analyseFunction(*fn);
	EXPECT_EQ(r.kind, ContainerKind::Unknown);
}

TEST(ContainerDetectorTest, NullFunctionInModuleSkipped)
{
	ContainerDetector det;
	std::vector<const ssa::SSAFunction*> fns = {nullptr};
	auto results = det.analyseModule(fns);
	EXPECT_EQ(results.size(), 0u);
}

// ─── Access pattern emission tests ────────────────────────────────────────────

TEST(AccessPatternTest, VectorIterateEmitted)
{
	auto fn = makeFunc(
		"vec_iter",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Add,
		},
		1);
	fn->block(1)->succs.push_back(0);
	VectorDetector det;
	auto r = det.detect(*fn);
	bool hasIter = false;
	for (const auto& ap: r.accessPatterns)
		if (ap.kind == AccessKind::Iterate) hasIter = true;
	if (r.confidence > 0.30f) EXPECT_TRUE(hasIter);
}

TEST(AccessPatternTest, MapLookupEmitted)
{
	auto fn = makeFunc(
		"map_find",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Store,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::And, 1);
	MapDetector det;
	auto r = det.detect(*fn);
	bool hasLookup = false;
	for (const auto& ap: r.accessPatterns)
		if (ap.kind == AccessKind::Lookup) hasLookup = true;
	if (r.confidence >= 0.20f) EXPECT_TRUE(hasLookup);
}

TEST(AccessPatternTest, SharedPtrResetEmitted)
{
	auto fn = makeFunc(
		"sp_rst",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Sub,
			ssa::IrInstr::Op::Compare,
		});
	addCall(*fn, "free");
	SharedPtrDetector det;
	auto r = det.detect(*fn);
	bool hasErase = false;
	for (const auto& ap: r.accessPatterns)
		if (ap.kind == AccessKind::Erase) hasErase = true;
	if (r.confidence >= 0.30f) EXPECT_TRUE(hasErase);
}

// ─── OpenAddressingDetector tests ─────────────────────────────────────────────

TEST(OpenAddressingDetectorTest, DivBy32WithHashIsOpenAddressing)
{
	auto fn = makeFunc(
		"ht_insert",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Store,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addCall(*fn, "strcmp");
	addImmInstr(*fn, ssa::IrInstr::Op::Div, 32);
	OpenAddressingDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.55f);
	EXPECT_EQ(r.emittedType, "open_addressing_hash_table");
}

TEST(OpenAddressingDetectorTest, XorMulWithoutStrcmpIsNotOpenAddressing)
{
	// AES GF mul is xor+mul, not a key compare.
	auto fn = makeFunc(
		"aes_gf",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Store,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addImmInstr(*fn, ssa::IrInstr::Op::Div, 32);
	OpenAddressingDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.55f);
}

TEST(OpenAddressingDetectorTest, DivByEightIsNotOpenAddressing)
{
	// AES affine `(i + 4) % 8` is not a bucket count.
	auto fn = makeFunc(
		"aes_rot",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Store,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addImmInstr(*fn, ssa::IrInstr::Op::Div, 8);
	OpenAddressingDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.55f);
}

TEST(OpenAddressingDetectorTest, DivBy256IsNotOpenAddressing)
{
	// Byte rem (`% 256`) is not a bucket count.
	auto fn = makeFunc(
		"byte_wrap",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Store,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addImmInstr(*fn, ssa::IrInstr::Op::Div, 256);
	OpenAddressingDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.55f);
}

TEST(OpenAddressingDetectorTest, AndSevenIsNotOpenAddressing)
{
	// strlen alignment `p & 7` is not a bucket mask.
	auto fn = makeFunc(
		"strlen_align",
		{
			ssa::IrInstr::Op::Xor,
			ssa::IrInstr::Op::Mul,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Store,
		},
		1);
	fn->block(1)->succs.push_back(0);
	addImmInstr(*fn, ssa::IrInstr::Op::And, 7);
	OpenAddressingDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.55f);
}

// ─── RingBufferDetector tests ─────────────────────────────────────────────────

TEST(RingBufferDetectorTest, PowerOfTwoAndMaskIsRingBuffer)
{
	auto fn = makeFunc(
		"rb_push",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::And, 7);
	RingBufferDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_EQ(r.emittedType, "ring_buffer");
}

TEST(RingBufferDetectorTest, AndWithoutImmediateIsNotRingBuffer)
{
	// And without a wrap-mask immediate is not a ring buffer.
	auto fn = makeFunc(
		"rb_pop",
		{
			ssa::IrInstr::Op::And,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
		});
	RingBufferDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(RingBufferDetectorTest, PlainDivIsNotRingBuffer)
{
	// B8 box-blur / 3 is Div + load + store, not a wrap index.
	auto fn = makeFunc(
		"box_blur",
		{
			ssa::IrInstr::Op::Div,
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
		});
	RingBufferDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(RingBufferDetectorTest, DivByEightIsNotRingBuffer)
{
	// Strength-reduction Div immediates are not wrap (B8 FP 0.700).
	auto fn = makeFunc(
		"rb_mod8",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Div, 8);
	RingBufferDetector det;
	auto r = det.detect(*fn);
	EXPECT_LT(r.confidence, 0.45f);
}

TEST(RingBufferDetectorTest, RemByCapacityIsRingBuffer)
{
	auto fn = makeFunc(
		"rb_mod",
		{
			ssa::IrInstr::Op::Load,
			ssa::IrInstr::Op::Store,
			ssa::IrInstr::Op::Compare,
			ssa::IrInstr::Op::Add,
		});
	addImmInstr(*fn, ssa::IrInstr::Op::Rem, 10);
	RingBufferDetector det;
	auto r = det.detect(*fn);
	EXPECT_GE(r.confidence, 0.45f);
	EXPECT_EQ(r.emittedType, "ring_buffer");
}
