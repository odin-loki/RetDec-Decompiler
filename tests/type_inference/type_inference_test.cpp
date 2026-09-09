/**
 * @file tests/type_inference/type_inference_test.cpp
 * @brief Unit tests for Width-Seeded Type Inference.
 *
 * Test categories:
 *
 * IRTYPE (6):
 *   1.  IrType::integer(32,Signed) → toString == "int32_t".
 *   2.  IrType::integer(64,Unsigned) → "uint64_t".
 *   3.  IrType::fp(32) → "float".
 *   4.  IrType::fp(64) → "double".
 *   5.  IrType::ptr() → "void*".
 *   6.  IrType::boolean() → "bool".
 *
 * WIDTH SEEDER (8):
 *   7.  Value with width=32 → seeded as 32.
 *   8.  MemRef width 4 bytes → seeded as 32 bits.
 *   9.  MemRef width 8 bytes → 64 bits.
 *  10.  FlagBundle → width 1.
 *  11.  Phi: inherits max incoming width.
 *  12.  Load from 4-byte MemRef → 32 bits.
 *  13.  Arithmetic: widest operand propagates.
 *  14.  No definition → width 0 (unknown).
 *
 * TYPE PROPAGATION (12):
 *  15.  HasWidth sets width on class.
 *  16.  IsPointer sets kind=Pointer.
 *  17.  IsSigned sets signedness.
 *  18.  IsUnsigned sets signedness.
 *  19.  IsFloat sets kind=Float.
 *  20.  IsBool sets kind=Bool, width=1.
 *  21.  SameWidth propagates width from seeded to unseeded.
 *  22.  SameSign propagates sign.
 *  23.  IsStruct sets kind=Pointer, pointeeKind=Struct.
 *  24.  IsArray sets kind=Pointer, pointeeKind=Array.
 *  25.  Higher-priority constraint wins conflict.
 *  26.  classCount decreases after union.
 *
 * ABI SEEDER (6):
 *  27.  SysV x64: integer param → HasWidth(64) + ParamType.
 *  28.  SysV x64: float param → IsFloat.
 *  29.  Return value seeded.
 *  30.  Pointer param → IsPointer.
 *  31.  Multiple params seeded in order.
 *  32.  Empty param list → no crash.
 *
 * STRUCT RECOVERY (8):
 *  33.  Two distinct offsets → struct created.
 *  34.  Single offset → no struct (plain pointer).
 *  35.  Three offsets → struct with 3 fields.
 *  36.  Field widths match access sizes.
 *  37.  Struct size = last field end.
 *  38.  StructLayout::hasField() correct.
 *  39.  Array pattern (Phi base) → IsArray constraint.
 *  40.  Stack accesses excluded from struct recovery.
 *
 * TYPE INFERENCE PASS INTEGRATION (8):
 *  41.  SAR operand → Signed.
 *  42.  SHR result → Unsigned.
 *  43.  AND/OR/XOR result → Unsigned.
 *  44.  CondBranch condition → Bool.
 *  45.  Load from non-stack → base is Pointer.
 *  46.  Stats: valuesWithKnownWidth > 0 after seeding.
 *  47.  Stats: structsRecovered correct.
 *  48.  Full pipeline: integer function, types correct.
 */

#include "retdec/type_inference/type_inference.h"
#include <array>
#include "retdec/ssa/ssa.h"
#include <gtest/gtest.h>

using namespace retdec::type_inference;
using namespace retdec::ssa;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static IrValue* addReg(SSAFunction& fn, BlockId blk, uint16_t width) {
    IrValue* v = fn.allocValue(ValueKind::VirtualReg, fn.declareVar("x"));
    v->width = width;
    IrInstr* ins = fn.addInstr(blk, IrInstr::Op::Assign, 0x1000);
    ins->defValue = v->id;
    v->defInstr = ins;
    return v;
}

static IrValue* addStackMem(SSAFunction& fn, BlockId blk,
                              int64_t off, uint8_t sz) {
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX - 2);
    v->memOffset = off; v->memWidth = sz; v->memIsStack = true;
    IrInstr* ins = fn.addInstr(blk, IrInstr::Op::Load, 0x1000);
    ins->uses.push_back({v->id, 0}); v->defInstr = ins;
    return v;
}

static IrValue* addHeapMem(SSAFunction& fn, BlockId blk,
                              uint32_t baseReg, int64_t off, uint8_t sz) {
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX - 3);
    v->memBaseReg = baseReg; v->memOffset = off; v->memWidth = sz;
    v->memIsStack = false;
    IrInstr* ins = fn.addInstr(blk, IrInstr::Op::Load, 0x1000);
    ins->uses.push_back({v->id, 0}); v->defInstr = ins;
    return v;
}

// ═══════════════════════════════════════════════════════════════════════════════
// IrType tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(IrType, Int32Signed_ToString) {
    EXPECT_EQ(IrType::integer(32, Signedness::Signed).toString(), "int32_t");
}
TEST(IrType, Uint64_ToString) {
    EXPECT_EQ(IrType::integer(64, Signedness::Unsigned).toString(), "uint64_t");
}
TEST(IrType, Float32_ToString) {
    EXPECT_EQ(IrType::fp(32).toString(), "float");
}
TEST(IrType, Float64_ToString) {
    EXPECT_EQ(IrType::fp(64).toString(), "double");
}
TEST(IrType, Ptr_ToString) {
    EXPECT_EQ(IrType::ptr().toString(), "void*");
}
TEST(IrType, Bool_ToString) {
    EXPECT_EQ(IrType::boolean().toString(), "bool");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Width seeder tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WidthSeeder, ValueWidth32_Seeded) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    auto* v = addReg(fn, b->id, 32);
    WidthSeeder ws; auto r = ws.run(fn);
    EXPECT_EQ(r.widths[v->id], 32u);
}

TEST(WidthSeeder, MemRef4Bytes_Is32bits) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    auto* v = addStackMem(fn, b->id, -8, 4);
    WidthSeeder ws; auto r = ws.run(fn);
    EXPECT_EQ(r.widths[v->id], 32u);
}

TEST(WidthSeeder, MemRef8Bytes_Is64bits) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    auto* v = addStackMem(fn, b->id, -8, 8);
    WidthSeeder ws; auto r = ws.run(fn);
    EXPECT_EQ(r.widths[v->id], 64u);
}

TEST(WidthSeeder, FlagBundle_Width1) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    IrValue* v = fn.allocValue(ValueKind::FlagBundle, fn.declareVar("flags"));
    IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Compare, 0x1000);
    ins->defValue = v->id; v->defInstr = ins;
    WidthSeeder ws; auto r = ws.run(fn);
    EXPECT_EQ(r.widths[v->id], 1u);
}

TEST(WidthSeeder, Phi_InheritsMaxWidth) {
    SSAFunction fn("f");
    auto* entry = fn.addBlock("entry");
    auto* merge = fn.addBlock("merge");
    entry->addSucc(merge->id); merge->addPred(entry->id);

    VarId x = fn.declareVar("x");
    IrValue* v1 = fn.allocValue(ValueKind::VirtualReg, x); v1->width = 32;
    IrInstr* i1 = fn.addInstr(entry->id, IrInstr::Op::Assign, 0);
    i1->defValue = v1->id; v1->defInstr = i1;

    PhiNode* phi = fn.addPhi(merge->id, x);
    phi->operands.push_back({entry->id, v1->id});

    IrValue* pv = fn.allocValue(ValueKind::Phi, x);
    pv->defPhi = phi; phi->result = pv->id;

    WidthSeeder ws; auto r = ws.run(fn);
    EXPECT_EQ(r.widths[pv->id], 32u);
}

TEST(WidthSeeder, LoadFrom4ByteMemRef_32bits) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    IrValue* mem = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    mem->memWidth = 4; mem->memIsStack = true;
    IrValue* def = fn.allocValue(ValueKind::VirtualReg, fn.declareVar("v"));
    IrInstr* load = fn.addInstr(b->id, IrInstr::Op::Load, 0x1000);
    load->uses.push_back({mem->id, 0});
    load->defValue = def->id; def->defInstr = load;
    WidthSeeder ws; auto r = ws.run(fn);
    EXPECT_EQ(r.widths[def->id], 32u);
}

TEST(WidthSeeder, Arithmetic_WidestOperand) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    auto* a = addReg(fn, b->id, 32);
    auto* c = addReg(fn, b->id, 64);
    IrValue* res = fn.allocValue(ValueKind::VirtualReg, fn.declareVar("r"));
    IrInstr* add = fn.addInstr(b->id, IrInstr::Op::Add, 0x1010);
    add->uses.push_back({a->id, 0}); add->uses.push_back({c->id, 1});
    add->defValue = res->id; res->defInstr = add;
    WidthSeeder ws; auto r = ws.run(fn);
    EXPECT_EQ(r.widths[res->id], 64u);
}

TEST(WidthSeeder, NoDefinition_Width0) {
    SSAFunction fn("f"); fn.addBlock("entry");
    IrValue* v = fn.allocValue(ValueKind::Undef, UINT32_MAX);
    WidthSeeder ws; auto r = ws.run(fn);
    EXPECT_EQ(r.widths.count(v->id), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Type propagation tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TypePropagation, HasWidth_SetsWidth) {
    TypePropagation tp;
    tp.addValue(0);
    tp.addConstraint(TypeConstraint::hasWidth(0, 32));
    tp.run();
    EXPECT_EQ(tp.typeOf(0).width, 32u);
}

TEST(TypePropagation, IsPointer_SetsKind) {
    TypePropagation tp;
    tp.addValue(0);
    tp.addConstraint(TypeConstraint::isPointer(0));
    tp.run();
    EXPECT_EQ(tp.typeOf(0).kind, TypeKind::Pointer);
}

TEST(TypePropagation, IsSigned_SetsSigned) {
    TypePropagation tp;
    tp.addValue(0);
    tp.addConstraint(TypeConstraint::isSigned(0));
    tp.run();
    EXPECT_EQ(tp.typeOf(0).sign, Signedness::Signed);
}

TEST(TypePropagation, IsUnsigned_SetsUnsigned) {
    TypePropagation tp;
    tp.addValue(0);
    tp.addConstraint(TypeConstraint::isUnsigned(0));
    tp.run();
    EXPECT_EQ(tp.typeOf(0).sign, Signedness::Unsigned);
}

TEST(TypePropagation, IsFloat_SetsKind) {
    TypePropagation tp;
    tp.addValue(0);
    tp.addConstraint(TypeConstraint::isFloat(0, 32));
    tp.run();
    EXPECT_EQ(tp.typeOf(0).kind, TypeKind::Float);
    EXPECT_EQ(tp.typeOf(0).width, 32u);
}

TEST(TypePropagation, IsBool_SetsKindAndWidth) {
    TypePropagation tp;
    tp.addValue(0);
    tp.addConstraint({ConstraintKind::IsBool, 0});
    tp.run();
    EXPECT_EQ(tp.typeOf(0).kind, TypeKind::Bool);
    EXPECT_EQ(tp.typeOf(0).width, 1u);
}

TEST(TypePropagation, SameWidth_Propagates) {
    TypePropagation tp;
    tp.addValue(0); tp.addValue(1);
    tp.addConstraint(TypeConstraint::hasWidth(0, 64));
    tp.addConstraint(TypeConstraint::sameWidth(0, 1));
    tp.run();
    EXPECT_EQ(tp.typeOf(1).width, 64u);
}

TEST(TypePropagation, SameSign_Propagates) {
    TypePropagation tp;
    tp.addValue(0); tp.addValue(1);
    tp.addConstraint(TypeConstraint::isSigned(0));
    tp.addConstraint(TypeConstraint::sameSign(0, 1));
    tp.run();
    EXPECT_EQ(tp.typeOf(1).sign, Signedness::Signed);
}

TEST(TypePropagation, IsStruct_SetsPointerToStruct) {
    TypePropagation tp;
    tp.addValue(0);
    tp.addConstraint(TypeConstraint::isStruct(0, 42));
    tp.run();
    EXPECT_EQ(tp.typeOf(0).kind, TypeKind::Pointer);
    EXPECT_EQ(tp.typeOf(0).pointeeKind, TypeKind::Struct);
    EXPECT_EQ(tp.typeOf(0).structId, 42u);
}

TEST(TypePropagation, IsArray_SetsPointerToArray) {
    TypePropagation tp;
    tp.addValue(0);
    tp.addConstraint(TypeConstraint::isArray(0, 10, 4, TypeKind::Integer));
    tp.run();
    EXPECT_EQ(tp.typeOf(0).kind, TypeKind::Pointer);
    EXPECT_EQ(tp.typeOf(0).pointeeKind, TypeKind::Array);
    EXPECT_EQ(tp.typeOf(0).arrayCount, 10u);
    EXPECT_EQ(tp.typeOf(0).elemWidth, 4u);
}

TEST(TypePropagation, HigherPriority_Wins) {
    TypePropagation tp;
    tp.addValue(0);
    // Low priority: width=32 integer
    tp.addConstraint(TypeConstraint::hasWidth(0, 32));
    // Higher priority: ABI seed says Float
    TypeConstraint c = TypeConstraint::isFloat(0, 64);
    tp.addConstraint(c);
    tp.run();
    // Float should win (from IsFloat constraint applied after HasWidth)
    EXPECT_EQ(tp.typeOf(0).kind, TypeKind::Float);
}

TEST(TypePropagation, ClassCount_DecreasesWithSameWidth) {
    TypePropagation tp;
    tp.addValue(0); tp.addValue(1); tp.addValue(2);
    std::size_t before = tp.classCount();
    tp.addConstraint(TypeConstraint::sameWidth(0, 1));
    tp.run();
	// This used to assert the opposite of its own name, with the comment
	// "classCount should still be 3 (SameWidth doesn't unite classes)" -- an
	// observation of the behaviour rather than a statement of the contract.
	// type_propagation.cpp's header has always said "SameWidth(a, b) -> unite
	// classes, propagate width", and not uniting is what made the result
	// depend on the order the constraints arrived in.
	EXPECT_LT(tp.classCount(), before);
	EXPECT_EQ(tp.findRoot(0), tp.findRoot(1));
	EXPECT_NE(tp.findRoot(0), tp.findRoot(2));
}

// ═══════════════════════════════════════════════════════════════════════════════
// ABI seeder tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AbiSeeder, IntParam_HasWidth64) {
    TypePropagation tp;
    tp.addValue(10);
    AbiSeedInfo info;
    info.abi = AbiSeedInfo::Abi::SysV_x64;
    info.params.push_back({10, IrType::integer(64), 0});

    AbiSeeder seeder;
    seeder.seed(tp, info);
    tp.run();
    EXPECT_EQ(tp.typeOf(10).width, 64u);
}

TEST(AbiSeeder, FloatParam_IsFloat) {
    TypePropagation tp;
    tp.addValue(20);
    AbiSeedInfo info;
    info.abi = AbiSeedInfo::Abi::SysV_x64;
    info.params.push_back({20, IrType::fp(32), 0});

    AbiSeeder seeder;
    seeder.seed(tp, info);
    tp.run();
    EXPECT_EQ(tp.typeOf(20).kind, TypeKind::Float);
}

TEST(AbiSeeder, ReturnValue_Seeded) {
    TypePropagation tp;
    tp.addValue(5);
    AbiSeedInfo info;
    info.abi = AbiSeedInfo::Abi::SysV_x64;
    info.retVal = AbiSeedInfo::ReturnSeed{5, IrType::integer(64)};

    AbiSeeder seeder;
    seeder.seed(tp, info);
    tp.run();
    EXPECT_EQ(tp.typeOf(5).width, 64u);
}

TEST(AbiSeeder, PointerParam_IsPointer) {
    TypePropagation tp;
    tp.addValue(7);
    AbiSeedInfo info;
    info.abi = AbiSeedInfo::Abi::SysV_x64;
    info.params.push_back({7, IrType::ptr(), 0});

    AbiSeeder seeder;
    seeder.seed(tp, info);
    tp.run();
    EXPECT_EQ(tp.typeOf(7).kind, TypeKind::Pointer);
}

TEST(AbiSeeder, MultipleParams_AllSeeded) {
    TypePropagation tp;
    for (int i = 0; i < 3; ++i) tp.addValue(i);
    AbiSeedInfo info;
    info.abi = AbiSeedInfo::Abi::SysV_x64;
    info.params.push_back({0, IrType::integer(64), 0});
    info.params.push_back({1, IrType::integer(32), 1});
    info.params.push_back({2, IrType::fp(64), 2});

    AbiSeeder seeder;
    seeder.seed(tp, info);
    tp.run();
    EXPECT_EQ(tp.typeOf(0).width, 64u);
    EXPECT_EQ(tp.typeOf(1).width, 32u);
    EXPECT_EQ(tp.typeOf(2).kind,  TypeKind::Float);
}

TEST(AbiSeeder, EmptyParams_NocrashNoConstraints) {
    TypePropagation tp;
    AbiSeedInfo info;
    info.abi = AbiSeedInfo::Abi::SysV_x64;
    AbiSeeder seeder;
    ASSERT_NO_THROW(seeder.seed(tp, info));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Struct recovery tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(StructRecovery, TwoDistinctOffsets_StructCreated) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    uint32_t baseReg = fn.declareVar("p");
    addHeapMem(fn, b->id, baseReg, 0,  4);   // field at 0
    addHeapMem(fn, b->id, baseReg, 4,  4);   // field at 4

    StructRecovery sr; auto res = sr.run(fn);
    EXPECT_EQ(res.structsFound, 1u);
    ASSERT_EQ(res.layouts.size(), 1u);
}

TEST(StructRecovery, SingleOffset_NoStruct) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    uint32_t baseReg = fn.declareVar("p");
    addHeapMem(fn, b->id, baseReg, 0, 8);

    StructRecovery sr; auto res = sr.run(fn);
    EXPECT_EQ(res.structsFound, 0u);
}

TEST(StructRecovery, ThreeOffsets_ThreeFields) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    uint32_t baseReg = fn.declareVar("p");
    addHeapMem(fn, b->id, baseReg, 0,  4);
    addHeapMem(fn, b->id, baseReg, 4,  8);
    addHeapMem(fn, b->id, baseReg, 12, 4);

    StructRecovery sr; auto res = sr.run(fn);
    ASSERT_GE(res.layouts.size(), 1u);
    EXPECT_EQ(res.layouts[0].fields.size(), 3u);
}

TEST(StructRecovery, FieldWidths_MatchAccessSize) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    uint32_t baseReg = fn.declareVar("p");
    addHeapMem(fn, b->id, baseReg, 0, 4);
    addHeapMem(fn, b->id, baseReg, 4, 8);

    StructRecovery sr; auto res = sr.run(fn);
    ASSERT_GE(res.layouts.size(), 1u);
    EXPECT_TRUE(res.layouts[0].hasField(0));
    EXPECT_TRUE(res.layouts[0].hasField(4));
}

TEST(StructRecovery, StructSize_IsLastFieldEnd) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    uint32_t baseReg = fn.declareVar("p");
    addHeapMem(fn, b->id, baseReg, 0,  4);
    addHeapMem(fn, b->id, baseReg, 4,  4);  // end at 8

    StructRecovery sr; auto res = sr.run(fn);
    ASSERT_GE(res.layouts.size(), 1u);
    EXPECT_EQ(res.layouts[0].size, 8u);
}

TEST(StructRecovery, HasField_Correct) {
    StructLayout sl;
    StructField f; f.offset = 8; f.width = 4;
    sl.addField(f);
    EXPECT_TRUE(sl.hasField(8));
    EXPECT_FALSE(sl.hasField(12));
}

TEST(StructRecovery, StackAccesses_Excluded) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    // Stack accesses should NOT trigger struct recovery
    addStackMem(fn, b->id, -8,  4);
    addStackMem(fn, b->id, -12, 4);

    StructRecovery sr; auto res = sr.run(fn);
    EXPECT_EQ(res.structsFound, 0u);
}

TEST(StructRecovery, ArrayPattern_PhiBase) {
    SSAFunction fn("f");
    auto* entry  = fn.addBlock("entry");
    auto* header = fn.addBlock("header");
    entry->addSucc(header->id); header->addPred(entry->id);
    header->addSucc(header->id); header->addPred(header->id);

    // Loop variable (phi)
    VarId ivar = fn.declareVar("i");
    IrValue* phi = fn.allocValue(ValueKind::Phi, ivar);
    PhiNode* pn = fn.addPhi(header->id, ivar);
    phi->defPhi = pn; pn->result = phi->id;

    // Array access via phi as base
    IrValue* mem = fn.allocValue(ValueKind::MemRef, UINT32_MAX-3);
    mem->memBaseReg = phi->id;
    mem->memOffset  = 0;
    mem->memWidth   = 4;
    mem->memIsStack = false;
    IrInstr* load = fn.addInstr(header->id, IrInstr::Op::Load, 0x1000);
    load->uses.push_back({mem->id, 0}); mem->defInstr = load;

    StructRecovery sr; auto res = sr.run(fn);
    EXPECT_EQ(res.arraysFound, 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// TypeInferencePass integration tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TypeInferencePass, SAR_IsSigned) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    auto* v = addReg(fn, b->id, 32);
    auto* res = addReg(fn, b->id, 32);
    IrInstr* sar = fn.addInstr(b->id, IrInstr::Op::Sar, 0x2000);
    sar->uses.push_back({v->id, 0});
    sar->defValue = res->id; res->defInstr = sar;

    TypeInferencePass pass; pass.run(fn);
    EXPECT_EQ(pass.typeOf(v->id).sign, Signedness::Signed);
}

TEST(TypeInferencePass, SHR_IsUnsigned) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    auto* v = addReg(fn, b->id, 32);
    auto* res = addReg(fn, b->id, 32);
    IrInstr* shr = fn.addInstr(b->id, IrInstr::Op::Shr, 0x2000);
    shr->uses.push_back({v->id, 0});
    shr->defValue = res->id; res->defInstr = shr;

    TypeInferencePass pass; pass.run(fn);
    EXPECT_EQ(pass.typeOf(v->id).sign, Signedness::Unsigned);
}

TEST(TypeInferencePass, AND_IsUnsigned) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    auto* v = addReg(fn, b->id, 64);
    auto* res = addReg(fn, b->id, 64);
    IrInstr* andI = fn.addInstr(b->id, IrInstr::Op::And, 0x2000);
    andI->uses.push_back({v->id, 0});
    andI->defValue = res->id; res->defInstr = andI;

    TypeInferencePass pass; pass.run(fn);
    EXPECT_EQ(pass.typeOf(res->id).sign, Signedness::Unsigned);
}

TEST(TypeInferencePass, CondBranch_IsBool) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    auto* cond = addReg(fn, b->id, 1);
    IrInstr* br = fn.addInstr(b->id, IrInstr::Op::CondBranch, 0x2000);
    br->uses.push_back({cond->id, 0});

    TypeInferencePass pass; pass.run(fn);
    EXPECT_EQ(pass.typeOf(cond->id).kind, TypeKind::Bool);
}

TEST(TypeInferencePass, LoadFromNonStack_BaseIsPointer) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    uint32_t baseReg = fn.declareVar("ptr");
    IrValue* mem = fn.allocValue(ValueKind::MemRef, UINT32_MAX-3);
    mem->memBaseReg = baseReg; mem->memOffset = 0; mem->memWidth = 8;
    mem->memIsStack = false;
    IrInstr* load = fn.addInstr(b->id, IrInstr::Op::Load, 0x1000);
    load->uses.push_back({mem->id, 0}); mem->defInstr = load;

    TypeInferencePass pass; pass.run(fn);
    EXPECT_EQ(pass.typeOf(baseReg).kind, TypeKind::Pointer);
}

TEST(TypeInferencePass, Stats_ValuesWithKnownWidth) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    addReg(fn, b->id, 32);
    addReg(fn, b->id, 64);
    TypeInferencePass pass; pass.run(fn);
    EXPECT_GE(pass.stats().valuesWithKnownWidth, 2u);
}

TEST(TypeInferencePass, Stats_StructsRecovered) {
    SSAFunction fn("f"); auto* b = fn.addBlock("entry");
    uint32_t baseReg = fn.declareVar("p");
    addHeapMem(fn, b->id, baseReg, 0, 4);
    addHeapMem(fn, b->id, baseReg, 4, 4);
    TypeInferencePass pass; pass.run(fn);
    EXPECT_EQ(pass.stats().structsRecovered, 1u);
}

TEST(TypeInferencePass, FullPipeline_IntegerFunction) {
    SSAFunction fn("add");
    auto* b = fn.addBlock("entry");
    auto* a = addReg(fn, b->id, 32);
    auto* bv = addReg(fn, b->id, 32);
    auto* res = addReg(fn, b->id, 32);
    IrInstr* add = fn.addInstr(b->id, IrInstr::Op::Add, 0x1000);
    add->uses.push_back({a->id, 0}); add->uses.push_back({bv->id, 1});
    add->defValue = res->id; res->defInstr = add;

    TypeInferencePass pass; pass.run(fn);
    // All three values should have width 32
    EXPECT_EQ(pass.typeOf(a->id).width,   32u);
    EXPECT_EQ(pass.typeOf(bv->id).width,  32u);
    EXPECT_EQ(pass.typeOf(res->id).width, 32u);
    SUCCEED();  // TypeInferencePass has no public errors() interface
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// ─── An id that is its own index ─────────────────────────────────────────────

// `id + 1` was computed in uint32 arithmetic, so addValue(UINT32_MAX) resized
// all three vectors to 0 and the indexed read that follows went off the end --
// ASan reported a SEGV inside addValue. Sizing it correctly instead asks for
// four billion entries, which ASan reported as a 16 GB allocation. The id is
// the index, so it needs a bound, and find() already treats an id it has never
// seen as its own singleton class.
TEST(TypePropagationBounds, AnImplausiblyLargeValueIdIsNotMaterialised)
{
	TypePropagation tp;
	tp.addValue(3);
	tp.addValue(UINT32_MAX);
	tp.addValue(TypePropagation::kMaxValueId);
	tp.addValue(TypePropagation::kMaxValueId + 1000);
	// Each unseen id is still its own class, which is what an unconstrained
	// value is.
	EXPECT_EQ(UINT32_MAX, tp.findRoot(UINT32_MAX));
	EXPECT_EQ(3u, tp.findRoot(3));
}

// The ids a real function uses are unaffected.
TEST(TypePropagationBounds, OrdinaryValueIdsStillJoin)
{
	TypePropagation tp;
	tp.addValue(1);
	tp.addValue(2);
	tp.addConstraint(TypeConstraint::sameWidth(1, 2));
	tp.run();
	EXPECT_EQ(tp.findRoot(1), tp.findRoot(2));
}

// ─── The union-find that was never called ────────────────────────────────────

// SameWidth and SameSign copied a value between the two ids the constraint
// named and went no further: unite() was defined and never called, so every
// value stayed its own class and the answer depended on the order the
// constraints arrived in. The same three constraints, with the two SameWidth
// swapped, left v1 at 0 bits instead of 64.
TEST(TypePropagationOrder, TheAnswerDoesNotDependOnConstraintOrder)
{
	auto widths = [](bool forward) {
		TypePropagation tp;
		for (uint32_t i = 1; i <= 3; ++i)
			tp.addValue(i);
		TypeConstraint w;
		w.kind = ConstraintKind::HasWidth;
		w.lhsId = 3;
		w.width = 64;
		tp.addConstraint(w);
		if (forward)
		{
			tp.addConstraint(TypeConstraint::sameWidth(2, 3));
			tp.addConstraint(TypeConstraint::sameWidth(1, 2));
		}
		else
		{
			tp.addConstraint(TypeConstraint::sameWidth(1, 2));
			tp.addConstraint(TypeConstraint::sameWidth(2, 3));
		}
		tp.run();
		return std::array<uint16_t, 3>{tp.typeOf(1).width, tp.typeOf(2).width, tp.typeOf(3).width};
	};

	const auto a = widths(true);
	const auto b = widths(false);
	EXPECT_EQ(64u, a[0]);
	EXPECT_EQ(64u, b[0]) << "the width did not reach v1 when the constraints arrived the other way";
	EXPECT_EQ(a, b);
}

// And the values genuinely share a class, which is what makes that true.
TEST(TypePropagationOrder, SameWidthUnitesTheTwoClasses)
{
	TypePropagation tp;
	for (uint32_t i = 1; i <= 3; ++i)
		tp.addValue(i);
	tp.addConstraint(TypeConstraint::sameWidth(1, 2));
	tp.addConstraint(TypeConstraint::sameWidth(2, 3));
	tp.run();
	EXPECT_EQ(tp.findRoot(1), tp.findRoot(3));
	EXPECT_LT(tp.classCount(), 3u);
}

// The bound above stops addValue() from materialising an implausible id, but
// every caller then indexed classes_/rank_/parent_ with that same id anyway --
// so declining to materialise it turned a 16 GB allocation into a read roughly
// 1.2e11 bytes past the base. Only the SameWidth right-hand side was checked,
// which is what makes it a missed case rather than a design.
TEST(TypePropagationBounds, AConstraintOnAnUnmaterialisedIdIsIgnored)
{
	TypePropagation tp;
	tp.addValue(1);
	tp.addConstraint(TypeConstraint::hasWidth(TypePropagation::kMaxValueId, 32));
	tp.addConstraint(TypeConstraint::isPointer(UINT32_MAX));
	tp.addConstraint(TypeConstraint::isUnsigned(TypePropagation::kMaxValueId + 7));
	tp.addConstraint(TypeConstraint::sameWidth(1, TypePropagation::kMaxValueId));
	tp.run();

	EXPECT_EQ(UINT32_MAX, tp.findRoot(UINT32_MAX));
	EXPECT_EQ(0u, tp.typeOf(TypePropagation::kMaxValueId).width);
}

// A MemRef with an absolute address and no base register -- `mov eax,
// [0x404000]` -- leaves memBaseReg at its kInvalidVar default. The MemRef loop
// in StructRecovery::collectPatterns skips those; the one in
// emitInstructionConstraints' Load case did not, and handed kInvalidVar
// straight to isPointer().
TEST(TypeInferenceBounds, AbsoluteAddressLoadWithNoBaseRegisterDoesNotCrash)
{
	SSAFunction fn("f");
	auto* b = fn.addBlock("entry");
	IrValue* mem = fn.allocValue(ValueKind::MemRef);
	mem->memOffset = 0x404000;
	mem->memWidth = 8;
	mem->memIsStack = false;
	IrInstr* ld = fn.addInstr(b->id, IrInstr::Op::Load, 0x1000);
	ld->uses.push_back({mem->id, 0});
	mem->defInstr = ld;

	TypeInferencePass pass;
	pass.run(fn);
	EXPECT_EQ(64u, pass.typeOf(mem->id).width);
}

// ─── A phi wider than any of its operands ────────────────────────────────────
//
// resolvePhiWidth's `else` was attached to `it != known.end() && it->second >
// maxW` rather than to the map lookup alone. So an operand that IS seeded, but
// whose seeded width does not strictly beat the running maximum, fell through
// to the raw IrValue::width -- which defaults to 64 and is exactly what the
// seeded map exists to override for MemRefs, FlagBundles and Load-defined
// values. Equality is enough to trigger it, so even a phi whose operands all
// carry the same correct narrow width came out 64 bits wide.
//
// WidthSeeder::Phi_InheritsMaxWidth above cannot see this: with one operand,
// maxW is still 0 when it is examined, so the first branch always wins.

TEST(WidthSeeder, PhiOfTwoFlagBundlesIsOneBitWide)
{
	SSAFunction fn("f");
	auto* entry = fn.addBlock("entry");
	auto* other = fn.addBlock("other");
	auto* merge = fn.addBlock("merge");
	entry->addSucc(merge->id);
	other->addSucc(merge->id);
	merge->addPred(entry->id);
	merge->addPred(other->id);

	VarId x = fn.declareVar("flags");
	IrValue* f1 = fn.allocValue(ValueKind::FlagBundle, x);
	IrInstr* c1 = fn.addInstr(entry->id, IrInstr::Op::Compare, 0x1000);
	c1->defValue = f1->id;
	f1->defInstr = c1;
	IrValue* f2 = fn.allocValue(ValueKind::FlagBundle, x);
	IrInstr* c2 = fn.addInstr(other->id, IrInstr::Op::Compare, 0x2000);
	c2->defValue = f2->id;
	f2->defInstr = c2;

	PhiNode* phi = fn.addPhi(merge->id, x);
	phi->operands.push_back({entry->id, f1->id});
	phi->operands.push_back({other->id, f2->id});
	IrValue* pv = fn.allocValue(ValueKind::Phi, x);
	pv->defPhi = phi;
	phi->result = pv->id;

	WidthSeeder ws;
	auto r = ws.run(fn);
	ASSERT_EQ(1u, r.widths[f1->id]);
	ASSERT_EQ(1u, r.widths[f2->id]);
	EXPECT_EQ(1u, r.widths[pv->id]) << "both operands are one bit wide, so the merge cannot be wider";
}

TEST(WidthSeeder, PhiWidthDoesNotDependOnOperandOrder)
{
	// A 32-bit register merged with an 8-bit load. Whichever comes first, the
	// phi is 32 bits: the widest operand, and nothing wider.
	auto build = [](bool wideFirst) {
		SSAFunction fn("f");
		auto* entry = fn.addBlock("entry");
		auto* loop = fn.addBlock("loop");
		auto* merge = fn.addBlock("merge");
		entry->addSucc(merge->id);
		loop->addSucc(merge->id);
		merge->addPred(entry->id);
		merge->addPred(loop->id);

		VarId x = fn.declareVar("x");
		IrValue* wide = fn.allocValue(ValueKind::VirtualReg, x);
		wide->width = 32;
		IrInstr* asg = fn.addInstr(entry->id, IrInstr::Op::Assign, 0x1000);
		asg->defValue = wide->id;
		wide->defInstr = asg;

		IrValue* mem = fn.allocValue(ValueKind::MemRef);
		mem->memWidth = 1;
		// A byte load leaves the loaded value's own width at the 64 default;
		// the seeded map is what says it is 8.
		IrValue* narrow = fn.allocValue(ValueKind::VirtualReg, x);
		IrInstr* ld = fn.addInstr(loop->id, IrInstr::Op::Load, 0x2000);
		ld->defValue = narrow->id;
		ld->uses.push_back({mem->id, 0});
		narrow->defInstr = ld;

		PhiNode* phi = fn.addPhi(merge->id, x);
		if (wideFirst)
		{
			phi->operands.push_back({entry->id, wide->id});
			phi->operands.push_back({loop->id, narrow->id});
		}
		else
		{
			phi->operands.push_back({loop->id, narrow->id});
			phi->operands.push_back({entry->id, wide->id});
		}
		IrValue* pv = fn.allocValue(ValueKind::Phi, x);
		pv->defPhi = phi;
		phi->result = pv->id;

		WidthSeeder ws;
		auto r = ws.run(fn);
		return r.widths[pv->id];
	};

	EXPECT_EQ(32u, build(/*wideFirst=*/true)) << "the 8-bit operand fell back to its raw 64";
	EXPECT_EQ(32u, build(/*wideFirst=*/false));
}
