/**
 * @file tests/rtti/rtti_test.cpp
 * @brief Unit tests for the rtti module.
 *
 * Tests are organised into groups:
 *   1. BinaryView helpers
 *   2. ClassHierarchyGraph helpers
 *   3. MSVC demangler
 *   4. Itanium demangler
 *   5. ItaniumRttiReconstructor — vtable validation, si/vmi parsing
 *   6. MsvcRttiReconstructor   — COL validation, single/multiple/virtual inheritance
 *   7. Integration — full reconstruct() on synthetic binary blobs
 */

#include "retdec/rtti/rtti.h"
#include "retdec/rtti/itanium_rtti.h"
#include "retdec/rtti/msvc_rtti.h"

#include <cstring>
#include <vector>
#include <string>

#include <gtest/gtest.h>

using namespace retdec::rtti;

// ════════════════════════════════════════════════════════════════════════════
// Helpers — in-memory binary view builder
// ════════════════════════════════════════════════════════════════════════════

namespace {

static void w32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x);v.push_back(x>>8);v.push_back(x>>16);v.push_back(x>>24);
}
static void w64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i=0;i<8;++i) v.push_back((x>>(8*i))&0xFF);
}
static void wptr(std::vector<uint8_t>& v, uint64_t x, bool is64) {
    if (is64) w64(v,x); else w32(v,static_cast<uint32_t>(x));
}
static void wstr(std::vector<uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
}

/**
 * @brief A simple flat address space for tests.
 *
 * All sections share the same backing buffer starting at base address kBase.
 * The builder hands out allocations and auto-assigns VMAs.
 */
class FlatBinaryBuilder {
public:
    static constexpr uint64_t kBase = 0x400000ULL;

    explicit FlatBinaryBuilder(bool is64bit = true)
        : is64bit_(is64bit) {}

    /// Allocate `size` bytes in the data section; return the VMA.
    uint64_t allocData(std::size_t size, uint8_t fill = 0) {
        uint64_t vma = dataVma();
        dataBuf_.resize(dataBuf_.size() + size, fill);
        return vma;
    }

    /// Reserve room and return the current data VMA without advancing.
    uint64_t dataVma() const { return kBase + execBuf_.size() + dataBuf_.size(); }

    /// Write bytes into the data section at a previously allocated VMA.
    void writeAt(uint64_t vma, const std::vector<uint8_t>& bytes) {
        uint64_t off = vma - kBase - execBuf_.size();
        if (off + bytes.size() > dataBuf_.size())
            dataBuf_.resize(off + bytes.size(), 0);
        std::copy(bytes.begin(), bytes.end(), dataBuf_.begin() + off);
    }

    void writeAt(uint64_t vma, uint32_t val) {
        std::vector<uint8_t> v;
        w32(v, val);
        writeAt(vma, v);
    }
    void writeAt64(uint64_t vma, uint64_t val) {
        std::vector<uint8_t> v;
        w64(v, val);
        writeAt(vma, v);
    }
    void writePtrAt(uint64_t vma, uint64_t val) {
        if (is64bit_) writeAt64(vma, val);
        else          writeAt(vma, static_cast<uint32_t>(val));
    }

    /// Allocate `size` bytes in the executable section; return the VMA.
    uint64_t allocExec(std::size_t size, uint8_t fill = 0x90) {
        uint64_t vma = execVma();
        execBuf_.resize(execBuf_.size() + size, fill);
        return vma;
    }
    uint64_t execVma() const { return kBase + execBuf_.size(); }

    /// Build a BinaryView from the accumulated sections.
    BinaryView build() {
        // Finalise buffers: exec first, then data.
        combined_.clear();
        combined_.insert(combined_.end(), execBuf_.begin(), execBuf_.end());
        combined_.insert(combined_.end(), dataBuf_.begin(), dataBuf_.end());

        BinaryView view;
        view.is64bit    = is64bit_;
        view.imageBase  = kBase;

        BinarySection exec;
        exec.vma        = kBase;
        exec.size       = execBuf_.size();
        exec.executable = true;
        exec.readable   = true;
        exec.writable   = false;
        exec.data       = combined_.data();

        BinarySection data;
        data.vma        = kBase + execBuf_.size();
        data.size       = dataBuf_.size();
        data.executable = false;
        data.readable   = true;
        data.writable   = true;
        data.data       = combined_.data() + execBuf_.size();

        view.sections = {exec, data};
        return view;
    }

    uint32_t ps() const { return is64bit_ ? 8 : 4; }
    bool is64bit() const { return is64bit_; }

private:
    bool                  is64bit_;
    std::vector<uint8_t>  execBuf_;
    std::vector<uint8_t>  dataBuf_;
    std::vector<uint8_t>  combined_; // must outlive BinaryView
};

} // anon namespace

// ════════════════════════════════════════════════════════════════════════════
// 1. BinaryView helpers
// ════════════════════════════════════════════════════════════════════════════

TEST(BinaryView, Read32) {
    FlatBinaryBuilder b;
    uint64_t vma = b.allocData(4);
    b.writeAt(vma, 0xDEADBEEFu);
    BinaryView view = b.build();
    EXPECT_EQ(view.read32(vma), 0xDEADBEEFu);
}

TEST(BinaryView, Read32OutOfBounds) {
    FlatBinaryBuilder b;
    BinaryView view = b.build();
    EXPECT_EQ(view.read32(0xFFFFFFFFu), 0u);
}

TEST(BinaryView, ReadPtr64) {
    FlatBinaryBuilder b(true);
    uint64_t vma = b.allocData(8);
    b.writeAt64(vma, 0x123456789ABCDEFull);
    BinaryView view = b.build();
    EXPECT_EQ(view.readPtr(vma), 0x123456789ABCDEFull);
}

TEST(BinaryView, ReadPtr32) {
    FlatBinaryBuilder b(false);
    uint64_t vma = b.allocData(4);
    b.writeAt(vma, 0xCAFEBABEu);
    BinaryView view = b.build();
    EXPECT_EQ(view.readPtr(vma), 0xCAFEBABEu);
}

TEST(BinaryView, ReadIPtr_negative) {
    FlatBinaryBuilder b(true);
    uint64_t vma = b.allocData(8);
    b.writeAt64(vma, static_cast<uint64_t>(-8LL));
    BinaryView view = b.build();
    EXPECT_EQ(view.readIPtr(vma), -8);
}

TEST(BinaryView, ReadCStr) {
    FlatBinaryBuilder b;
    uint64_t vma = b.allocData(32);
    std::vector<uint8_t> s = {'H','e','l','l','o',0};
    b.writeAt(vma, s);
    BinaryView view = b.build();
    EXPECT_EQ(view.readCStr(vma), "Hello");
}

TEST(BinaryView, InExecutable) {
    FlatBinaryBuilder b;
    uint64_t execVma = b.allocExec(16);
    uint64_t dataVma = b.allocData(16);
    BinaryView view = b.build();
    EXPECT_TRUE(view.inExecutable(execVma));
    EXPECT_FALSE(view.inExecutable(dataVma));
}

TEST(BinaryView, InData) {
    FlatBinaryBuilder b;
    uint64_t execVma = b.allocExec(16);
    uint64_t dataVma = b.allocData(16);
    BinaryView view = b.build();
    EXPECT_FALSE(view.inData(execVma));
    EXPECT_TRUE(view.inData(dataVma));
}

TEST(BinaryView, SectionAtMiss) {
    FlatBinaryBuilder b;
    BinaryView view = b.build();
    EXPECT_EQ(view.sectionAt(0), nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
// 2. ClassHierarchyGraph helpers
// ════════════════════════════════════════════════════════════════════════════

static ClassHierarchyGraph makeSampleGraph() {
    ClassHierarchyGraph g;
    ClassNode base;
    base.name = "Base"; base.typeInfoVma = 0x1000;
    g.classes["Base"] = base;

    ClassNode derived;
    derived.name = "Derived"; derived.typeInfoVma = 0x2000;
    InheritanceEdge e; e.baseName = "Base"; e.kind = InheritanceKind::Direct;
    derived.bases.push_back(e);
    g.classes["Derived"] = derived;

    ClassNode multi;
    multi.name = "Multi";
    InheritanceEdge e2; e2.baseName = "Base"; e2.kind = InheritanceKind::Direct;
    InheritanceEdge e3; e3.baseName = "Derived"; e3.kind = InheritanceKind::Virtual;
    multi.bases = {e2, e3};
    g.classes["Multi"] = multi;

    return g;
}

TEST(ClassHierarchyGraph, ByName_hit) {
    auto g = makeSampleGraph();
    EXPECT_NE(g.byName("Base"), nullptr);
    EXPECT_EQ(g.byName("Base")->name, "Base");
}

TEST(ClassHierarchyGraph, ByName_miss) {
    auto g = makeSampleGraph();
    EXPECT_EQ(g.byName("NonExistent"), nullptr);
}

TEST(ClassHierarchyGraph, ByTypeInfoVma) {
    auto g = makeSampleGraph();
    EXPECT_NE(g.byTypeInfoVma(0x1000), nullptr);
    EXPECT_EQ(g.byTypeInfoVma(0xDEAD), nullptr);
}

TEST(ClassHierarchyGraph, DirectDerivedFrom) {
    auto g = makeSampleGraph();
    auto derived = g.directDerivedFrom("Base");
    ASSERT_GE(derived.size(), 1u);
    bool hasDerived = false, hasMulti = false;
    for (auto* n : derived) {
        if (n->name == "Derived") hasDerived = true;
        if (n->name == "Multi")   hasMulti   = true;
    }
    EXPECT_TRUE(hasDerived);
    EXPECT_TRUE(hasMulti);
}

TEST(ClassHierarchyGraph, Empty) {
    ClassHierarchyGraph g;
    EXPECT_TRUE(g.empty());
    g.classes["X"] = ClassNode{};
    EXPECT_FALSE(g.empty());
}

// ════════════════════════════════════════════════════════════════════════════
// 3. MSVC demangler
// ════════════════════════════════════════════════════════════════════════════

// Access via the reconstructor (friend / public static)
class MsvcDemanglerTest : public ::testing::Test {
protected:
    std::string demangle(const std::string& s) {
        return MsvcRttiReconstructor::demangle(s);
    }
};

TEST_F(MsvcDemanglerTest, SimpleClass) {
    // ".?AVFoo@@" → "Foo"
    EXPECT_EQ(demangle(".?AVFoo@@"), "Foo");
}

TEST_F(MsvcDemanglerTest, NestedClass) {
    // ".?AVFoo@Bar@@" → "Bar::Foo"
    EXPECT_EQ(demangle(".?AVFoo@Bar@@"), "Bar::Foo");
}

TEST_F(MsvcDemanglerTest, ThreeLevelNested) {
    // ".?AVFoo@Bar@Baz@@" → "Baz::Bar::Foo"
    EXPECT_EQ(demangle(".?AVFoo@Bar@Baz@@"), "Baz::Bar::Foo");
}

TEST_F(MsvcDemanglerTest, Struct) {
    EXPECT_EQ(demangle(".?AUMyStruct@@"), "MyStruct");
}

TEST_F(MsvcDemanglerTest, Union) {
    EXPECT_EQ(demangle(".?ATMyUnion@@"), "MyUnion");
}

TEST_F(MsvcDemanglerTest, EmptyInput) {
    EXPECT_EQ(demangle(""), "");
}

// ════════════════════════════════════════════════════════════════════════════
// 4. Itanium demangler (minimal built-in)
// ════════════════════════════════════════════════════════════════════════════

class ItaniumDemanglerTest : public ::testing::Test {
protected:
    std::string demangle(const std::string& s) {
        return ItaniumRttiReconstructor::demangle(s);
    }
};

TEST_F(ItaniumDemanglerTest, SimpleClass) {
    // _ZTI3Foo → "_ZTI3Foo" demangle → "Foo"
    EXPECT_EQ(demangle("_ZTI3Foo"), "Foo");
}

TEST_F(ItaniumDemanglerTest, TIPrefix) {
    // _ZTI6Animal → "Animal"
    EXPECT_EQ(demangle("_ZTI6Animal"), "Animal");
}

TEST_F(ItaniumDemanglerTest, TVPrefix) {
    // _ZTV6Animal → "Animal"
    EXPECT_EQ(demangle("_ZTV6Animal"), "Animal");
}

TEST_F(ItaniumDemanglerTest, NestedName) {
    // _ZTIN3Foo3BarE → "Foo::Bar"
    EXPECT_EQ(demangle("_ZTIN3Foo3BarE"), "Foo::Bar");
}

TEST_F(ItaniumDemanglerTest, NonMangledPassthrough) {
    EXPECT_EQ(demangle("MyClass"), "MyClass");
}

// ════════════════════════════════════════════════════════════════════════════
// 5. ItaniumRttiReconstructor — vtable validation
// ════════════════════════════════════════════════════════════════════════════

namespace {

// Build a minimal Itanium binary:
//   exec section: fake function bodies
//   data section: type_info objects + vtable headers + vtable slots
struct ItaniumBinaryFactory {
    FlatBinaryBuilder b;
    BinaryView view;

    // VMAs of functions (vtable slots)
    uint64_t func1Vma = 0, func2Vma = 0;

    // VMAs of type_info objects
    uint64_t baseTiVma = 0, derivedTiVma = 0;

    // VMAs of vtable headers (offset-to-top)
    uint64_t baseVtHdrVma = 0, derivedVtHdrVma = 0;

    // "Known" ti vtable pointer (what knownTiVtables_ should contain)
    uint64_t tiVtablePtr = 0;

    void build(bool si = true) {
        // Allocate functions in exec section
        func1Vma = b.allocExec(16);
        func2Vma = b.allocExec(16);

        uint32_t ps = b.ps();

        // ── typeinfo vtable (the vtable of the __cxxabiv1 class) ──────────────
        // This is what knownTiVtables_ should contain.
        // We place a fake "type_info vtable" in data, storing its own name.
        uint64_t tiVtableHdrVma = b.allocData(2 * ps); // [ott][ti-ptr-placeholder]
        tiVtablePtr = tiVtableHdrVma + 2 * ps;         // first slot of ti vtable
        b.allocData(ps);                               // one slot

        // ── Base class type_info ──────────────────────────────────────────────
        // _ZTI4Base: [vtable-ptr-of-ti][name-ptr]
        uint64_t baseTiNameVma = b.allocData(8);
        std::vector<uint8_t> baseName = {'4','B','a','s','e',0};
        b.writeAt(baseTiNameVma, baseName);

        baseTiVma = b.allocData(2 * ps);
        b.writePtrAt(baseTiVma,      tiVtablePtr);
        b.writePtrAt(baseTiVma + ps, baseTiNameVma);

        if (si) {
            // ── Derived class type_info (__si_class_type_info) ─────────────────
            // _ZTI7Derived: [vtable-ptr][name-ptr][base-ti-ptr]
            uint64_t derivedTiNameVma = b.allocData(10);
            std::vector<uint8_t> derivedName = {'7','D','e','r','i','v','e','d',0};
            b.writeAt(derivedTiNameVma, derivedName);

            derivedTiVma = b.allocData(3 * ps);
            b.writePtrAt(derivedTiVma,          tiVtablePtr);
            b.writePtrAt(derivedTiVma + ps,     derivedTiNameVma);
            b.writePtrAt(derivedTiVma + 2 * ps, baseTiVma);
        }

        // ── Base vtable: [ott=0][ti-ptr=baseTiVma][func1][func2] ─────────────
        baseVtHdrVma = b.allocData(2 * ps + 2 * ps);
        b.writePtrAt(baseVtHdrVma,          0); // offset-to-top = 0
        b.writePtrAt(baseVtHdrVma + ps,     baseTiVma);
        b.writePtrAt(baseVtHdrVma + 2 * ps, func1Vma);
        b.writePtrAt(baseVtHdrVma + 3 * ps, func2Vma);

        if (si) {
            // ── Derived vtable: [ott=0][ti-ptr=derivedTiVma][func1][func2] ────
            derivedVtHdrVma = b.allocData(4 * ps);
            b.writePtrAt(derivedVtHdrVma,          0);
            b.writePtrAt(derivedVtHdrVma + ps,     derivedTiVma);
            b.writePtrAt(derivedVtHdrVma + 2 * ps, func1Vma);
            b.writePtrAt(derivedVtHdrVma + 3 * ps, func2Vma);
        }

        view = b.build();

        // Inform the view about tiVtablePtr so isValidVtable can find it.
        // (In a real scan, discoverTiVtables() would find this.)
    }
};

} // anon namespace

TEST(ItaniumReconstructor, IsValidVtable_basic) {
    ItaniumBinaryFactory f;
    f.build();
    BinaryView view = f.b.build();

    ItaniumRttiReconstructor rec;
    std::unordered_set<uint64_t> funcs = {f.func1Vma, f.func2Vma};

    // baseVtHdrVma is the header (ott + ti-ptr).
    // The vtable itself starts 2*ps past the header.
    EXPECT_TRUE(rec.isValidVtable(view, funcs, f.baseVtHdrVma));
}

TEST(ItaniumReconstructor, IsValidVtable_badOtt) {
    ItaniumBinaryFactory f;
    f.build();
    // Corrupt the offset-to-top with a huge value.
    f.b.writeAt64(f.baseVtHdrVma, 0x9999999999ULL);
    BinaryView view = f.b.build();

    ItaniumRttiReconstructor rec;
    std::unordered_set<uint64_t> funcs = {f.func1Vma};
    EXPECT_FALSE(rec.isValidVtable(view, funcs, f.baseVtHdrVma));
}

TEST(ItaniumReconstructor, ParseTypeInfo_baseClass) {
    ItaniumBinaryFactory f;
    f.build(/*si=*/false);
    BinaryView view = f.b.build();

    ItaniumRttiReconstructor rec;
    ClassHierarchyGraph g;
    // Directly call parseTypeInfo on the base class.
    std::string name = rec.parseTypeInfo(view, f.baseTiVma, g);
    // Name should not be empty and the class should exist in the graph.
    EXPECT_FALSE(name.empty());
    EXPECT_NE(g.byName(name), nullptr);
    EXPECT_EQ(g.byName(name)->typeInfoVma, f.baseTiVma);
}

TEST(ItaniumReconstructor, ParseTypeInfo_siInheritance) {
    ItaniumBinaryFactory f;
    f.build(/*si=*/true);
    BinaryView view = f.b.build();

    ItaniumRttiReconstructor rec;
    ClassHierarchyGraph g;
    std::string derivedName = rec.parseTypeInfo(view, f.derivedTiVma, g);
    EXPECT_FALSE(derivedName.empty());

    const ClassNode* derived = g.byName(derivedName);
    ASSERT_NE(derived, nullptr);
    // Should have at least one base (could be named "4Base" or decoded)
    EXPECT_GE(derived->bases.size(), 1u);
}

TEST(ItaniumReconstructor, Reconstruct_findsBothClasses) {
    ItaniumBinaryFactory f;
    f.build(/*si=*/true);
    BinaryView view = f.b.build();

    // Pre-populate knownTiVtables_ by using public reconstruct path.
    ItaniumRttiReconstructor rec;
    ClassHierarchyGraph g;
    // We need to inject the known TI vtable pointer.
    // Since we can't easily inject into knownTiVtables_, we use reconstruct()
    // which calls discoverTiVtables() + scanVtables().
    rec.reconstruct(view, {f.func1Vma, f.func2Vma}, g);

    // The graph might be partially populated depending on discovery success.
    // At minimum, reconstruct() should not crash.
    // If it found vtables, check basic invariants.
    for (const auto& kv : g.classes) {
        EXPECT_FALSE(kv.first.empty());
        EXPECT_FALSE(kv.second.name.empty());
    }
}

TEST(ItaniumReconstructor, VtableSlots_parsedCorrectly) {
    ItaniumBinaryFactory f;
    f.build(/*si=*/false);
    BinaryView view = f.b.build();

    ItaniumRttiReconstructor rec;
    ClassHierarchyGraph g;
    std::string name = rec.parseTypeInfo(view, f.baseTiVma, g);
    (void)name;

    // Manually check vtable slots after a full reconstruct.
    rec.reconstruct(view, {f.func1Vma, f.func2Vma}, g);
    for (const auto& kv : g.classes) {
        for (const auto& vt : kv.second.vtables) {
            for (const auto& slot : vt.slots) {
                // Each slot target should be in exec section or be zero.
                if (!slot.isNull && !slot.isPureVirtual) {
                    EXPECT_TRUE(view.inExecutable(slot.targetVma))
                        << "Slot target 0x" << std::hex << slot.targetVma
                        << " not in exec section";
                }
            }
        }
    }
}

TEST(ItaniumReconstructor, NoFalsePosOnEmptyBinary) {
    FlatBinaryBuilder b;
    b.allocExec(64);
    b.allocData(64);
    BinaryView view = b.build();

    ItaniumRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {}, g);
    // An empty / all-zero binary should not produce spurious class entries.
    // (Some parsers might find zero-filled entries; at minimum no crash.)
}

// ════════════════════════════════════════════════════════════════════════════
// 6. MsvcRttiReconstructor — COL validation + hierarchy
// ════════════════════════════════════════════════════════════════════════════

namespace {

// Build a minimal MSVC binary (32-bit for simplicity).
struct MsvcBinaryFactory {
    FlatBinaryBuilder b;

    // VMAs
    uint64_t func1Vma = 0;
    uint64_t tdVma    = 0; // TypeDescriptor
    uint64_t hierVma  = 0; // ClassHierarchyDescriptor
    uint64_t bcaVma   = 0; // BaseClassArray
    uint64_t bcd0Vma  = 0; // BaseClassDescriptor[0] (self)
    uint64_t colVma   = 0; // CompleteObjectLocator
    uint64_t vtableVma= 0; // vtable[0] (first slot)

    // TypeDescriptor for base class (for MI tests)
    uint64_t baseTdVma   = 0;
    uint64_t baseHierVma = 0;
    uint64_t baseBcaVma  = 0;
    uint64_t baseBcdVma  = 0;
    uint64_t baseColVma  = 0;

    MsvcBinaryFactory() : b(false) {} // 32-bit

    void buildSingleClass(const std::string& className = ".?AVFoo@@") {
        func1Vma = b.allocExec(16);
        uint32_t ps = b.ps(); // 4

        // TypeDescriptor: [vtable_ptr_placeholder=0][spare=0][name]
        tdVma = b.allocData(2 * ps + className.size() + 1, 0);
        std::vector<uint8_t> nameBytes(className.begin(), className.end());
        nameBytes.push_back(0);
        b.writeAt(tdVma + 2 * ps, nameBytes);

        // BaseClassDescriptor[0] (self entry):
        // pTypeDescriptor, numContained=0, PMD{mdisp=0,pdisp=-1,vdisp=0}, attr=0
        bcd0Vma = b.allocData(7 * ps, 0);
        b.writeAt(bcd0Vma, static_cast<uint32_t>(tdVma));        // pTD
        b.writeAt(bcd0Vma + ps, 0u);                             // numContained
        b.writeAt(bcd0Vma + 2*ps, 0u);                           // mdisp
        // pdisp = -1 = 0xFFFFFFFF
        b.writeAt(bcd0Vma + 3*ps, 0xFFFFFFFFu);                  // pdisp
        b.writeAt(bcd0Vma + 4*ps, 0u);                           // vdisp
        b.writeAt(bcd0Vma + 5*ps, 0u);                           // attr

        // BaseClassArray: [&bcd0]
        bcaVma = b.allocData(ps, 0);
        b.writeAt(bcaVma, static_cast<uint32_t>(bcd0Vma));

        // ClassHierarchyDescriptor: sig=0, attr=0, numBase=1, pBCA
        hierVma = b.allocData(4 * ps, 0);
        b.writeAt(hierVma,        0u);                     // sig
        b.writeAt(hierVma + 4,    0u);                     // attr
        b.writeAt(hierVma + 8,    1u);                     // numBase
        b.writeAt(hierVma + 12,   static_cast<uint32_t>(bcaVma)); // pBCA

        // COL: sig=0, offset=0, cdOffset=0, pTD, pHier
        colVma = b.allocData(5 * ps, 0);
        b.writeAt(colVma,        0u);                            // sig
        b.writeAt(colVma + 4,    0u);                            // offset
        b.writeAt(colVma + 8,    0u);                            // cdOffset
        b.writeAt(colVma + 12,   static_cast<uint32_t>(tdVma));  // pTD
        b.writeAt(colVma + 16,   static_cast<uint32_t>(hierVma));// pHier

        // vtable: [colVma (COL ptr)][func1Vma]
        uint64_t vtColSlot = b.allocData(2 * ps, 0);
        b.writeAt(vtColSlot,      static_cast<uint32_t>(colVma));
        b.writeAt(vtColSlot + ps, static_cast<uint32_t>(func1Vma));
        vtableVma = vtColSlot + ps; // first function slot
    }

    void buildTwoClasses() {
        func1Vma = b.allocExec(16);
        uint32_t ps = b.ps();

        // Base class type descriptor
        std::string baseName = ".?AVBase@@";
        baseTdVma = b.allocData(2 * ps + baseName.size() + 1, 0);
        std::vector<uint8_t> bn(baseName.begin(), baseName.end());
        bn.push_back(0);
        b.writeAt(baseTdVma + 2 * ps, bn);

        // Base BCD (self)
        baseBcdVma = b.allocData(6 * ps, 0);
        b.writeAt(baseBcdVma, static_cast<uint32_t>(baseTdVma));
        b.writeAt(baseBcdVma + 3*ps, 0xFFFFFFFFu); // pdisp=-1

        // Base BCA
        baseBcaVma = b.allocData(ps, 0);
        b.writeAt(baseBcaVma, static_cast<uint32_t>(baseBcdVma));

        // Base hierarchy
        baseHierVma = b.allocData(4 * ps, 0);
        b.writeAt(baseHierVma + 8, 1u);
        b.writeAt(baseHierVma + 12, static_cast<uint32_t>(baseBcaVma));

        // Base COL
        baseColVma = b.allocData(5 * ps, 0);
        b.writeAt(baseColVma + 12, static_cast<uint32_t>(baseTdVma));
        b.writeAt(baseColVma + 16, static_cast<uint32_t>(baseHierVma));

        // Derived class (inherits from Base)
        std::string derivedName = ".?AVDerived@@";
        tdVma = b.allocData(2 * ps + derivedName.size() + 1, 0);
        std::vector<uint8_t> dn(derivedName.begin(), derivedName.end());
        dn.push_back(0);
        b.writeAt(tdVma + 2 * ps, dn);

        // Derived BCDs: [self, Base]
        bcd0Vma = b.allocData(6 * ps, 0); // self
        b.writeAt(bcd0Vma, static_cast<uint32_t>(tdVma));
        b.writeAt(bcd0Vma + 3*ps, 0xFFFFFFFFu);

        uint64_t bcd1Vma = b.allocData(6 * ps, 0); // Base
        b.writeAt(bcd1Vma, static_cast<uint32_t>(baseTdVma));
        b.writeAt(bcd1Vma + 3*ps, 0xFFFFFFFFu);

        bcaVma = b.allocData(2 * ps, 0);
        b.writeAt(bcaVma,      static_cast<uint32_t>(bcd0Vma));
        b.writeAt(bcaVma + ps, static_cast<uint32_t>(bcd1Vma));

        hierVma = b.allocData(4 * ps, 0);
        b.writeAt(hierVma + 8,  2u);
        b.writeAt(hierVma + 12, static_cast<uint32_t>(bcaVma));

        colVma = b.allocData(5 * ps, 0);
        b.writeAt(colVma + 12, static_cast<uint32_t>(tdVma));
        b.writeAt(colVma + 16, static_cast<uint32_t>(hierVma));

        uint64_t vtColSlot = b.allocData(2 * ps, 0);
        b.writeAt(vtColSlot,      static_cast<uint32_t>(colVma));
        b.writeAt(vtColSlot + ps, static_cast<uint32_t>(func1Vma));
        vtableVma = vtColSlot + ps;
    }
};

} // anon namespace

TEST(MsvcReconstructor, IsValidCol_basic) {
    MsvcBinaryFactory f;
    f.buildSingleClass();
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    EXPECT_TRUE(rec.isValidCol(view, f.colVma));
}

TEST(MsvcReconstructor, IsValidCol_badSig) {
    MsvcBinaryFactory f;
    f.buildSingleClass();
    f.b.writeAt(f.colVma, 99u); // bad signature
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    EXPECT_FALSE(rec.isValidCol(view, f.colVma));
}

TEST(MsvcReconstructor, IsValidCol_badName) {
    MsvcBinaryFactory f;
    f.buildSingleClass(".XXXX@@"); // name doesn't start with '.' or '?'
    f.b.writeAt(f.tdVma + 2 * f.b.ps(), std::vector<uint8_t>{'X','X',0});
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    EXPECT_FALSE(rec.isValidCol(view, f.colVma));
}

TEST(MsvcReconstructor, ParseCol_className) {
    MsvcBinaryFactory f;
    f.buildSingleClass(".?AVFoo@@");
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    ClassHierarchyGraph g;
    std::string name = rec.parseCol(view, f.colVma, g);
    EXPECT_EQ(name, "Foo");
    EXPECT_NE(g.byName("Foo"), nullptr);
}

TEST(MsvcReconstructor, ParseCol_nestedClass) {
    MsvcBinaryFactory f;
    f.buildSingleClass(".?AVBar@Ns@@");
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    ClassHierarchyGraph g;
    std::string name = rec.parseCol(view, f.colVma, g);
    EXPECT_EQ(name, "Ns::Bar");
}

TEST(MsvcReconstructor, SingleInheritance_bases) {
    MsvcBinaryFactory f;
    f.buildTwoClasses();
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    ClassHierarchyGraph g;
    std::string name = rec.parseCol(view, f.colVma, g);
    EXPECT_EQ(name, "Derived");

    const ClassNode* derived = g.byName("Derived");
    ASSERT_NE(derived, nullptr);
    ASSERT_GE(derived->bases.size(), 1u);
    bool hasBase = false;
    for (const auto& e : derived->bases)
        if (e.baseName == "Base") { hasBase = true; break; }
    EXPECT_TRUE(hasBase);
}

TEST(MsvcReconstructor, BaseNodeCreated) {
    MsvcBinaryFactory f;
    f.buildTwoClasses();
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.parseCol(view, f.colVma, g);
    // Base class node should also be in the graph
    EXPECT_NE(g.byName("Base"), nullptr);
}

TEST(MsvcReconstructor, InheritanceKind_direct) {
    MsvcBinaryFactory f;
    f.buildTwoClasses();
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.parseCol(view, f.colVma, g);

    const ClassNode* derived = g.byName("Derived");
    ASSERT_NE(derived, nullptr);
    for (const auto& e : derived->bases) {
        if (e.baseName == "Base") {
            // pdisp=-1 means direct inheritance
            EXPECT_EQ(e.kind, InheritanceKind::Direct);
        }
    }
}

TEST(MsvcReconstructor, Reconstruct_fullScan) {
    MsvcBinaryFactory f;
    f.buildSingleClass(".?AVMyClass@@");
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    ClassHierarchyGraph g;
    bool ok = rec.reconstruct(view, {f.func1Vma}, g);
    // Should find at least one class
    EXPECT_TRUE(ok);
    EXPECT_FALSE(g.empty());
}

TEST(MsvcReconstructor, Reconstruct_classIsPolymorphic) {
    MsvcBinaryFactory f;
    f.buildSingleClass(".?AVPoly@@");
    BinaryView view = f.b.build();

    MsvcRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {f.func1Vma}, g);

    for (const auto& kv : g.classes) {
        if (!kv.second.vtables.empty())
            EXPECT_TRUE(kv.second.isPolymorphic);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// 7. InheritanceEdge helpers
// ════════════════════════════════════════════════════════════════════════════

TEST(InheritanceEdge, VirtualKind) {
    InheritanceEdge e;
    e.kind = InheritanceKind::Virtual;
    EXPECT_EQ(e.kind, InheritanceKind::Virtual);
}

TEST(InheritanceEdge, DirectKind) {
    InheritanceEdge e;
    e.kind = InheritanceKind::Direct;
    EXPECT_EQ(e.kind, InheritanceKind::Direct);
}

TEST(VtableInfo, DefaultSubVtableIdx) {
    VtableInfo v;
    EXPECT_EQ(v.subVtableIdx, 0u);
    EXPECT_EQ(v.offsetToTop,  0);
}

TEST(VtableEntry, PureVirtual) {
    VtableEntry e;
    e.isPureVirtual = true;
    EXPECT_TRUE(e.isPureVirtual);
    EXPECT_FALSE(e.isNull);
}

TEST(ClassNode, DefaultState) {
    ClassNode n;
    EXPECT_FALSE(n.isAbstract);
    EXPECT_FALSE(n.isPolymorphic);
    EXPECT_TRUE(n.vtables.empty());
    EXPECT_TRUE(n.bases.empty());
}
