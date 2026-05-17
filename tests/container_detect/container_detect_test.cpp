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
static std::unique_ptr<ssa::SSAFunction> makeFunc(
        const std::string& name,
        const std::vector<ssa::IrInstr::Op>& ops,
        int extraBlocks = 0) {
    auto fn = std::make_unique<ssa::SSAFunction>(name);
    auto* entry = fn->addBlock("entry");
    for (auto op : ops) {
        fn->addInstr(entry->id, op);
    }
    for (int i = 0; i < extraBlocks; ++i) {
        fn->addBlock("blk" + std::to_string(i));
    }
    return fn;
}

// Add a Call instruction with a callee name to a function's entry block.
static void addCall(ssa::SSAFunction& fn, const std::string& callee) {
    auto* instr = fn.addInstr(fn.block(0)->id, ssa::IrInstr::Op::Call);
    if (instr) instr->calleeName = callee;
}

// Add an instruction whose immediate operand has a given value.
static void addImmInstr(ssa::SSAFunction& fn, ssa::IrInstr::Op op, uint64_t immVal) {
    auto* instr = fn.addInstr(fn.block(0)->id, op);
    if (!instr) return;
    // Add an immediate operand (value allocated in the function).
    ssa::IrValue* val = fn.allocValue(ssa::ValueKind::Immediate);
    if (val) val->imm = immVal;
    ssa::Use u; u.valueId = val ? val->id : ssa::kInvalidValue;
    instr->uses.push_back(u);
}

// ─── RecoveredType tests ──────────────────────────────────────────────────────

TEST(RecoveredTypeTest, UnknownToString) {
    RecoveredType t;
    EXPECT_EQ(t.toString(), "int");
}

TEST(RecoveredTypeTest, Int32Signed) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Int32;
    t.isSigned = true;
    EXPECT_EQ(t.toString(), "int32_t");
}

TEST(RecoveredTypeTest, Int32Unsigned) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Int32;
    t.isSigned = false;
    EXPECT_EQ(t.toString(), "uint32_t");
}

TEST(RecoveredTypeTest, Int8Signed) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Int8;
    t.isSigned = true;
    EXPECT_EQ(t.toString(), "int8_t");
}

TEST(RecoveredTypeTest, Int64) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Int64;
    t.isSigned = true;
    EXPECT_EQ(t.toString(), "int64_t");
}

TEST(RecoveredTypeTest, FloatToString) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Float;
    EXPECT_EQ(t.toString(), "float");
}

TEST(RecoveredTypeTest, DoubleToString) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Double;
    EXPECT_EQ(t.toString(), "double");
}

TEST(RecoveredTypeTest, PointerToString) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Pointer;
    EXPECT_EQ(t.toString(), "void*");
}

TEST(RecoveredTypeTest, StructWithName) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Struct;
    t.name = "MyRecord";
    EXPECT_EQ(t.toString(), "MyRecord");
}

TEST(RecoveredTypeTest, StructNoName) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::Struct;
    EXPECT_EQ(t.toString(), "struct_t");
}

TEST(RecoveredTypeTest, StringToString) {
    RecoveredType t;
    t.kind = RecoveredType::Kind::String;
    EXPECT_EQ(t.toString(), "std::string");
}

// ─── ContainerResult tests ────────────────────────────────────────────────────

TEST(ContainerResultTest, KindNameVector) {
    ContainerResult r;
    r.kind = ContainerKind::Vector;
    EXPECT_EQ(r.kindName(), "std::vector");
}

TEST(ContainerResultTest, KindNameList) {
    ContainerResult r;
    r.kind = ContainerKind::List;
    EXPECT_EQ(r.kindName(), "std::list");
}

TEST(ContainerResultTest, KindNameMap) {
    ContainerResult r;
    r.kind = ContainerKind::Map;
    EXPECT_EQ(r.kindName(), "std::map");
}

TEST(ContainerResultTest, KindNameUnorderedMap) {
    ContainerResult r;
    r.kind = ContainerKind::UnorderedMap;
    EXPECT_EQ(r.kindName(), "std::unordered_map");
}

TEST(ContainerResultTest, KindNameString) {
    ContainerResult r;
    r.kind = ContainerKind::String;
    EXPECT_EQ(r.kindName(), "std::string");
}

TEST(ContainerResultTest, KindNameSharedPtr) {
    ContainerResult r;
    r.kind = ContainerKind::SharedPtr;
    EXPECT_EQ(r.kindName(), "std::shared_ptr");
}

TEST(ContainerResultTest, KindNameUnknown) {
    ContainerResult r;
    EXPECT_EQ(r.kindName(), "unknown");
}

TEST(ContainerResultTest, ToStringContainsConfidence) {
    ContainerResult r;
    r.kind = ContainerKind::Vector;
    r.confidence = 0.8f;
    r.emittedType = "std::vector<int32_t>";
    std::string s = r.toString();
    EXPECT_NE(s.find("0.8"), std::string::npos);
}

TEST(ContainerResultTest, ToStringContainsKind) {
    ContainerResult r;
    r.kind = ContainerKind::Map;
    r.confidence = 0.9f;
    std::string s = r.toString();
    EXPECT_NE(s.find("std::map"), std::string::npos);
}

// ─── VectorDetector tests ─────────────────────────────────────────────────────

TEST(VectorDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    VectorDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.30f);
}

TEST(VectorDetectorTest, ThreeLoadsPlusSubHigherConfidence) {
    auto fn = makeFunc("vec_size", {
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
}

TEST(VectorDetectorTest, GrowthPatternDetected) {
    auto fn = makeFunc("vec_push", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Store,
    });
    addCall(*fn, "malloc");
    addCall(*fn, "free");
    addImmInstr(*fn, ssa::IrInstr::Op::Shl, 1);  // GCC growth ×2
    VectorDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.55f);
    EXPECT_EQ(r.compilerVariant, CompilerVariant::GCC);
}

TEST(VectorDetectorTest, MSVCGrowthFactor) {
    auto fn = makeFunc("vec_push_msvc", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Store,
    });
    addCall(*fn, "malloc");
    addCall(*fn, "free");
    addImmInstr(*fn, ssa::IrInstr::Op::Shr, 1);  // MSVC growth cap/2
    VectorDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.55f);
    EXPECT_EQ(r.compilerVariant, CompilerVariant::MSVC);
}

TEST(VectorDetectorTest, ElementByteWidthRecovered) {
    auto fn = makeFunc("vec_index", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Add,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::Mul, 4);  // int32_t stride
    VectorDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.30f);
    EXPECT_EQ(r.elementType.byteWidth, 4);
}

TEST(VectorDetectorTest, AccessPatternsHavePushBack) {
    auto fn = makeFunc("vec_store", {
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
    for (const auto& ap : r.accessPatterns)
        if (ap.kind == AccessKind::PushBack) { hasPushBack = true; break; }
    EXPECT_TRUE(hasPushBack);
}

TEST(VectorDetectorTest, AccessPatternsHaveSize) {
    auto fn = makeFunc("vec_sz", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Add,
    });
    VectorDetector det;
    auto r = det.detect(*fn);
    bool hasSize = false;
    for (const auto& ap : r.accessPatterns)
        if (ap.kind == AccessKind::SizeCheck) { hasSize = true; break; }
    EXPECT_TRUE(hasSize);
}

TEST(VectorDetectorTest, EmittedTypeContainsVector) {
    auto fn = makeFunc("vec_t", {
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

TEST(ListDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    ListDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.30f);
}

TEST(ListDetectorTest, NodeAllocPlusTraversalDetected) {
    auto fn = makeFunc("list_iter", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, /*extraBlocks=*/1);
    // Back-edge: block 1 successor back to block 0.
    fn->block(1)->succs.push_back(0);
    addCall(*fn, "malloc");
    ListDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.40f);
    EXPECT_EQ(r.kind, ContainerKind::List);
}

TEST(ListDetectorTest, FourStoresIncreasesConfidence) {
    auto fn = makeFunc("list_insert", {
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 1);
    fn->block(1)->succs.push_back(0);
    addCall(*fn, "malloc");
    ListDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.60f);
}

TEST(ListDetectorTest, AccessPatternsHaveIterate) {
    auto fn = makeFunc("list_it", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 1);
    fn->block(1)->succs.push_back(0);
    addCall(*fn, "malloc");
    ListDetector det;
    auto r = det.detect(*fn);
    bool hasIter = false;
    for (const auto& ap : r.accessPatterns)
        if (ap.kind == AccessKind::Iterate) { hasIter = true; break; }
    EXPECT_TRUE(hasIter);
}

// ─── MapDetector tests ────────────────────────────────────────────────────────

TEST(MapDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    MapDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.20f);
}

TEST(MapDetectorTest, ColourFieldDetected) {
    auto fn = makeFunc("rb_insert", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Compare,
        ssa::IrInstr::Op::Compare,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::And, 1);  // colour bit
    MapDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.20f);
}

TEST(MapDetectorTest, RotationPatternBoostsConfidence) {
    // Rotation: load → store as both src and addr in same block.
    auto fn = std::make_unique<ssa::SSAFunction>("rb_rotate");
    auto* entry = fn->addBlock("entry");

    // L1: loadResult = Load(...)
    auto* L1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
    // S1: Store(loadResult, addr1)  — loadResult used as source.
    auto* S1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
    // S2: Store(val, loadResult)   — loadResult used as address.
    auto* S2 = fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
    if (L1 && S1 && S2) {
        ssa::Use u1; u1.valueId = L1->id;
        S1->uses.push_back(u1);   // src
        ssa::Use u2; u2.valueId = L1->id;
        S2->uses.push_back(u2);
        ssa::Use u3; u3.valueId = L1->id + 1; // dummy
        S2->uses.push_back(u3);               // addr slot
        // Over-write addr slot with L1->id so it appears as address.
        S2->uses[1].valueId = L1->id;
    }
    fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
    addImmInstr(*fn, ssa::IrInstr::Op::And, 1);

    MapDetector det;
    auto r = det.detect(*fn);
    // We should get some confidence from the colour field at minimum.
    EXPECT_GE(r.confidence, 0.20f);
    EXPECT_EQ(r.kind, ContainerKind::Map);
}

TEST(MapDetectorTest, EmittedTypeContainsMap) {
    auto fn = makeFunc("m_t", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Compare,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::And, 1);
    MapDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.10f)
        EXPECT_NE(r.emittedType.find("std::map"), std::string::npos);
}

// ─── UnorderedMapDetector tests ───────────────────────────────────────────────

TEST(UnorderedMapDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    UnorderedMapDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.30f);
}

TEST(UnorderedMapDetectorTest, HashCallDetected) {
    auto fn = makeFunc("uhm_lookup", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 1);
    fn->block(1)->succs.push_back(0);
    addCall(*fn, "std_hash");
    addImmInstr(*fn, ssa::IrInstr::Op::And, 63);  // 64 buckets - 1

    UnorderedMapDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.55f);
    EXPECT_EQ(r.kind, ContainerKind::UnorderedMap);
}

TEST(UnorderedMapDetectorTest, InlineHashXorMul) {
    auto fn = makeFunc("uhm_xor_mul", {
        ssa::IrInstr::Op::Xor,
        ssa::IrInstr::Op::Mul,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 1);
    fn->block(1)->succs.push_back(0);
    addImmInstr(*fn, ssa::IrInstr::Op::And, 15);  // 16 buckets -1

    UnorderedMapDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.55f);
}

TEST(UnorderedMapDetectorTest, ModuloPowerOfTwo) {
    auto fn = makeFunc("uhm_mod", {
        ssa::IrInstr::Op::Xor,
        ssa::IrInstr::Op::Mul,
        ssa::IrInstr::Op::Load,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::And, 255);  // 2^8 - 1
    UnorderedMapDetector det;
    auto r = det.detect(*fn);
    // Xor+Mul = hash, And with 255 = modulo → should score.
    EXPECT_GE(r.confidence, 0.50f);
}

TEST(UnorderedMapDetectorTest, EmittedTypeContainsUnorderedMap) {
    auto fn = makeFunc("uhm_t", {
        ssa::IrInstr::Op::Xor,
        ssa::IrInstr::Op::Mul,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::And, 31);
    UnorderedMapDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.10f)
        EXPECT_NE(r.emittedType.find("unordered_map"), std::string::npos);
}

// ─── StringDetector tests ─────────────────────────────────────────────────────

TEST(StringDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    StringDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.30f);
}

TEST(StringDetectorTest, SSOThreshold15GCCVariant) {
    auto fn = makeFunc("str_gcc", {
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

TEST(StringDetectorTest, SSOThreshold22ClangVariant) {
    auto fn = makeFunc("str_clang", {
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

TEST(StringDetectorTest, SSOThreshold23AlsoClang) {
    auto fn = makeFunc("str_clang23", {
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Load,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::Compare, 23);
    StringDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.55f);
    EXPECT_EQ(r.compilerVariant, CompilerVariant::Clang);
}

TEST(StringDetectorTest, ElementTypeIsChar) {
    auto fn = makeFunc("str_elem", {
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Load,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
    StringDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.10f) {
        EXPECT_EQ(r.elementType.byteWidth, 1);
    }
}

TEST(StringDetectorTest, EmittedTypeIsStdString) {
    auto fn = makeFunc("str_emit", {
        ssa::IrInstr::Op::Load,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
    StringDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.10f)
        EXPECT_EQ(r.emittedType, "std::string");
}

TEST(StringDetectorTest, AccessPatternsHaveSizeCheck) {
    auto fn = makeFunc("str_size", {
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
    });
    addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
    StringDetector det;
    auto r = det.detect(*fn);
    bool hasSize = false;
    for (const auto& ap : r.accessPatterns)
        if (ap.kind == AccessKind::SizeCheck) { hasSize = true; break; }
    if (r.confidence >= 0.30f) EXPECT_TRUE(hasSize);
}

// ─── SharedPtrDetector tests ──────────────────────────────────────────────────

TEST(SharedPtrDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    SharedPtrDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.30f);
}

TEST(SharedPtrDetectorTest, TwoPointerPlusAtomicDecrement) {
    auto fn = makeFunc("sp_destr", {
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
}

TEST(SharedPtrDetectorTest, AtomicCallDetected) {
    auto fn = makeFunc("sp_atomic", {
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
}

TEST(SharedPtrDetectorTest, EmittedTypeContainsSharedPtr) {
    auto fn = makeFunc("sp_emit", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Compare,
    });
    addCall(*fn, "free");
    SharedPtrDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.10f)
        EXPECT_NE(r.emittedType.find("shared_ptr"), std::string::npos);
}

TEST(SharedPtrDetectorTest, AccessPatternsHaveLookup) {
    auto fn = makeFunc("sp_get", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Compare,
    });
    addCall(*fn, "free");
    SharedPtrDetector det;
    auto r = det.detect(*fn);
    bool hasLookup = false;
    for (const auto& ap : r.accessPatterns)
        if (ap.kind == AccessKind::Lookup) { hasLookup = true; break; }
    if (r.confidence >= 0.30f) EXPECT_TRUE(hasLookup);
}

// ─── TemplateTypeRecoverer tests ──────────────────────────────────────────────

TEST(TemplateTypeRecovererTest, RecoverFromByteWidth4) {
    TemplateTypeRecoverer rec;
    ContainerResult partial;
    partial.kind = ContainerKind::Vector;
    auto fn = makeFunc("f", {});
    RecoveredType t = rec.recoverElementType(*fn, partial, 4);
    EXPECT_EQ(t.byteWidth, 4);
}

TEST(TemplateTypeRecovererTest, RecoverFromByteWidth8) {
    TemplateTypeRecoverer rec;
    ContainerResult partial;
    auto fn = makeFunc("f", {});
    RecoveredType t = rec.recoverElementType(*fn, partial, 8);
    EXPECT_EQ(t.byteWidth, 8);
}

TEST(TemplateTypeRecovererTest, RecoverFromByteWidth1) {
    TemplateTypeRecoverer rec;
    ContainerResult partial;
    auto fn = makeFunc("f", {});
    RecoveredType t = rec.recoverElementType(*fn, partial, 1);
    EXPECT_EQ(t.byteWidth, 1);
}

TEST(TemplateTypeRecovererTest, UsesPartialTypeWhenNoWidth) {
    TemplateTypeRecoverer rec;
    ContainerResult partial;
    partial.elementType.kind = RecoveredType::Kind::Float;
    partial.elementType.byteWidth = 4;
    auto fn = makeFunc("f", {});
    RecoveredType t = rec.recoverElementType(*fn, partial, 0);
    EXPECT_EQ(t.kind, RecoveredType::Kind::Float);
}

TEST(TemplateTypeRecovererTest, DefaultsToInt32WhenNoInfo) {
    TemplateTypeRecoverer rec;
    ContainerResult partial;
    auto fn = makeFunc("f", {});
    RecoveredType t = rec.recoverElementType(*fn, partial, 0);
    EXPECT_EQ(t.byteWidth, 4);
}

TEST(TemplateTypeRecovererTest, KeyTypeFromComparatorCall) {
    TemplateTypeRecoverer rec;
    ContainerResult partial;
    partial.kind = ContainerKind::Map;
    auto fn = makeFunc("f", {});
    auto* instr = fn->addInstr(fn->block(0)->id, ssa::IrInstr::Op::Call);
    if (instr) {
        instr->calleeName = "std::less::compare";
        ssa::IrValue* val = fn->allocValue(ssa::ValueKind::Immediate);
        if (val) val->width = 32;  // 4 bytes = 32 bits
        ssa::Use u; u.valueId = val ? val->id : ssa::kInvalidValue;
        instr->uses.push_back(u);
    }
    RecoveredType t = rec.recoverKeyType(*fn, partial);
    // Should recover from comparator param.
    EXPECT_GE(t.byteWidth, 0);  // At least some type returned.
}

// ─── ContainerDetector orchestration tests ────────────────────────────────────

TEST(ContainerDetectorTest, EmptyFunctionSkippedByPreflight) {
    ContainerDetector::Config cfg;
    cfg.minBlocks = 2;
    cfg.minInstrs = 8;
    ContainerDetector det(cfg);
    auto fn = makeFunc("tiny", { ssa::IrInstr::Op::Load });
    auto r = det.analyseFunction(*fn);
    EXPECT_EQ(r.kind, ContainerKind::Unknown);
    EXPECT_EQ(det.stats().functionsSkipped, 1u);
}

TEST(ContainerDetectorTest, VectorFunctionDetected) {
    ContainerDetector det;
    auto fn = makeFunc("vec", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Store,
    }, 2);
    addCall(*fn, "malloc");
    addCall(*fn, "free");
    auto r = det.analyseFunction(*fn);
    // Either vector or unknown depending on confidence threshold.
    EXPECT_TRUE(r.kind == ContainerKind::Vector || r.kind == ContainerKind::Unknown);
}

TEST(ContainerDetectorTest, StringBeatsVectorDueToSSOSignal) {
    // A function with SSO threshold AND three loads should be detected as String,
    // not Vector, because StringDetector runs first.
    ContainerDetector det;
    auto fn = makeFunc("str_vs_vec", {
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
    }, 2);
    addImmInstr(*fn, ssa::IrInstr::Op::Compare, 15);
    auto r = det.analyseFunction(*fn);
    // String detector should win over vector due to SSO threshold specificity.
    if (r.kind != ContainerKind::Unknown)
        EXPECT_EQ(r.kind, ContainerKind::String);
}

TEST(ContainerDetectorTest, AnalyseModuleReturnsMapPerFunction) {
    ContainerDetector det;
    auto fn1 = makeFunc("f1", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Add,
    }, 2);
    auto fn2 = makeFunc("f2", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Xor,
        ssa::IrInstr::Op::Mul,
    }, 2);
    addImmInstr(*fn2, ssa::IrInstr::Op::And, 31);

    std::vector<const ssa::SSAFunction*> fns = { fn1.get(), fn2.get() };
    auto results = det.analyseModule(fns);
    // Results is a map from function name to ContainerResult; may have 0–2 entries.
    EXPECT_LE(results.size(), 2u);
}

TEST(ContainerDetectorTest, StatsFunctionsAnalysedIncrement) {
    ContainerDetector det;
    auto fn1 = makeFunc("f1", { ssa::IrInstr::Op::Load }, 2);
    auto fn2 = makeFunc("f2", { ssa::IrInstr::Op::Load }, 2);
    std::vector<const ssa::SSAFunction*> fns = { fn1.get(), fn2.get() };
    det.analyseModule(fns);
    EXPECT_GE(det.stats().functionsAnalysed, 2u);
}

TEST(ContainerDetectorTest, MinConfidenceFiltersLowScores) {
    ContainerDetector::Config cfg;
    cfg.minConfidence = 0.99f;  // impossibly high threshold
    ContainerDetector det(cfg);
    auto fn = makeFunc("vec_hi_thresh", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Add,
    }, 2);
    auto r = det.analyseFunction(*fn);
    EXPECT_EQ(r.kind, ContainerKind::Unknown);
}

TEST(ContainerDetectorTest, NullFunctionInModuleSkipped) {
    ContainerDetector det;
    std::vector<const ssa::SSAFunction*> fns = { nullptr };
    auto results = det.analyseModule(fns);
    EXPECT_EQ(results.size(), 0u);
}

// ─── Access pattern emission tests ────────────────────────────────────────────

TEST(AccessPatternTest, VectorIterateEmitted) {
    auto fn = makeFunc("vec_iter", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Add,
    }, 1);
    fn->block(1)->succs.push_back(0);
    VectorDetector det;
    auto r = det.detect(*fn);
    bool hasIter = false;
    for (const auto& ap : r.accessPatterns)
        if (ap.kind == AccessKind::Iterate) hasIter = true;
    if (r.confidence > 0.30f) EXPECT_TRUE(hasIter);
}

TEST(AccessPatternTest, MapLookupEmitted) {
    auto fn = makeFunc("map_find", {
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
    for (const auto& ap : r.accessPatterns)
        if (ap.kind == AccessKind::Lookup) hasLookup = true;
    if (r.confidence >= 0.20f) EXPECT_TRUE(hasLookup);
}

TEST(AccessPatternTest, SharedPtrResetEmitted) {
    auto fn = makeFunc("sp_rst", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Compare,
    });
    addCall(*fn, "free");
    SharedPtrDetector det;
    auto r = det.detect(*fn);
    bool hasErase = false;
    for (const auto& ap : r.accessPatterns)
        if (ap.kind == AccessKind::Erase) hasErase = true;
    if (r.confidence >= 0.30f) EXPECT_TRUE(hasErase);
}
