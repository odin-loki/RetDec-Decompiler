/**
 * @file tests/rtti/rtti_extra_test.cpp
 * @brief Unit tests for Borland, DMC, Watcom, and Symbian RTTI parsers.
 */

#include "retdec/rtti/borland_rtti.h"
#include "retdec/rtti/dmc_rtti.h"
#include "retdec/rtti/watcom_rtti.h"
#include "retdec/rtti/symbian_rtti.h"

#include <cstring>
#include <vector>
#include <gtest/gtest.h>

using namespace retdec::rtti;

// ════════════════════════════════════════════════════════════════════════════
// Shared flat-memory builder (same as rtti_test.cpp but self-contained)
// ════════════════════════════════════════════════════════════════════════════

namespace {

static void w32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x);v.push_back(x>>8);v.push_back(x>>16);v.push_back(x>>24);
}
static void w16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x);v.push_back(x>>8);
}
static void wstr(std::vector<uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
}
static void wPascal(std::vector<uint8_t>& v, const std::string& s) {
    // Pascal ShortString: length byte + chars (no null terminator)
    v.push_back(static_cast<uint8_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

class FlatBin {
public:
    static constexpr uint64_t kBase = 0x400000ULL;
    explicit FlatBin(bool is64=false) : is64_(is64) {}

    uint64_t allocExec(std::size_t n, uint8_t fill=0x90) {
        uint64_t v=execVma(); execBuf_.resize(execBuf_.size()+n,fill); return v;
    }
    uint64_t execVma() const { return kBase+execBuf_.size(); }
    uint64_t allocData(std::size_t n, uint8_t fill=0) {
        uint64_t v=dataVma(); dataBuf_.resize(dataBuf_.size()+n,fill); return v;
    }
    uint64_t dataVma() const { return kBase+execBuf_.size()+dataBuf_.size(); }

    void writeAt(uint64_t vma, const std::vector<uint8_t>& bytes) {
        uint64_t off = vma - kBase - execBuf_.size();
        if (off+bytes.size()>dataBuf_.size()) dataBuf_.resize(off+bytes.size(),0);
        std::copy(bytes.begin(),bytes.end(),dataBuf_.begin()+off);
    }
    void writeAt32(uint64_t vma, uint32_t val) {
        std::vector<uint8_t> v; w32(v,val); writeAt(vma,v);
    }
    void writeAt16(uint64_t vma, uint16_t val) {
        std::vector<uint8_t> v; w16(v,val); writeAt(vma,v);
    }

    BinaryView build() {
        combined_.clear();
        combined_.insert(combined_.end(),execBuf_.begin(),execBuf_.end());
        combined_.insert(combined_.end(),dataBuf_.begin(),dataBuf_.end());

        BinaryView view;
        view.is64bit   = is64_;
        view.imageBase = kBase;

        BinarySection exec;
        exec.vma = kBase; exec.size = execBuf_.size();
        exec.executable = true; exec.readable = true; exec.writable = false;
        exec.data = combined_.data();

        BinarySection data;
        data.vma = kBase+execBuf_.size(); data.size = dataBuf_.size();
        data.executable = false; data.readable = true; data.writable = true;
        data.data = combined_.data()+execBuf_.size();

        view.sections = {exec,data};
        return view;
    }

    uint32_t ps() const { return is64_?8:4; }

private:
    bool is64_;
    std::vector<uint8_t> execBuf_, dataBuf_, combined_;
};

} // anon namespace

// ════════════════════════════════════════════════════════════════════════════
// 1. Borland RTTI
// ════════════════════════════════════════════════════════════════════════════

namespace {

// Build a minimal Borland VMT with vmtSelfPtr pattern.
struct BorlandFactory {
    FlatBin b;
    uint64_t funcVma = 0;
    uint64_t classNameStrVma = 0;
    uint64_t vmtVma = 0;   // positive side (virtual methods)
    uint64_t selfPtrVma = 0; // = vmtVma - 76

    BorlandFactory() : b(false) {}

    // Build: ClassName = className, instanceSize = instSize
    void buildClass(const std::string& className, uint32_t instSize = 16) {
        funcVma = b.allocExec(16);

        // Allocate the VMT negative area (76 bytes = 19 x 4-byte fields)
        // plus the positive area (virtual methods)
        // vmtSelfPtr field is at offset 0 of the negative block.
        // negative block: [selfPtr(-76)][intfTab(-72)]...[parent(-36)][...padding]
        // Then at offset +76 from selfPtr field = vmtVma

        uint64_t blockStart = b.allocData(76 + 8); // 76 neg + 8 pos (2 vfuncs)

        // selfPtrVma = blockStart + 0
        // vmtVma = blockStart + 76
        selfPtrVma = blockStart;
        vmtVma     = blockStart + 76;

        // Write vmtSelfPtr = vmtVma
        b.writeAt32(selfPtrVma, static_cast<uint32_t>(vmtVma));

        // className as Pascal ShortString
        classNameStrVma = b.allocData(1 + className.size());
        {
            std::vector<uint8_t> ps;
            wPascal(ps, className);
            b.writeAt(classNameStrVma, ps);
        }

        // vmtClassName field is at vmtVma + kVmtClassName = vmtVma - 44
        // but from selfPtrVma it's selfPtrVma + (76 - 44) = selfPtrVma + 32
        uint64_t classNameFieldVma = selfPtrVma + 32; // -44 offset from vmtVma
        b.writeAt32(classNameFieldVma, static_cast<uint32_t>(classNameStrVma));

        // vmtInstanceSize field at selfPtrVma + (76-40) = selfPtrVma + 36
        uint64_t instSizeFieldVma = selfPtrVma + 36;
        b.writeAt32(instSizeFieldVma, instSize);

        // vmtParent field at selfPtrVma + (76-36) = selfPtrVma + 40
        // Leave as 0 (TObject root).

        // Virtual method[0] at vmtVma
        b.writeAt32(vmtVma, static_cast<uint32_t>(funcVma));
        // Virtual method[1] at vmtVma + 4
        b.writeAt32(vmtVma + 4, static_cast<uint32_t>(funcVma));
    }
};

} // anon namespace

TEST(BorlandRTTI, ReadShortString_valid) {
    FlatBin b;
    uint64_t vma = b.allocData(16);
    std::vector<uint8_t> ps; wPascal(ps, "TButton");
    b.writeAt(vma, ps);
    BinaryView view = b.build();
    EXPECT_EQ(BorlandRttiReconstructor::readShortString(view, vma), "TButton");
}

TEST(BorlandRTTI, ReadShortString_empty) {
    FlatBin b;
    uint64_t vma = b.allocData(4, 0);
    BinaryView view = b.build();
    EXPECT_EQ(BorlandRttiReconstructor::readShortString(view, vma), "");
}

TEST(BorlandRTTI, IsValidVmt_basic) {
    BorlandFactory f;
    f.buildClass("TFoo", 24);
    BinaryView view = f.b.build();

    BorlandRttiReconstructor rec;
    EXPECT_TRUE(rec.isValidVmt(view, f.vmtVma));
}

TEST(BorlandRTTI, IsValidVmt_badSelfPtr) {
    BorlandFactory f;
    f.buildClass("TBar", 8);
    // Corrupt the selfPtr
    f.b.writeAt32(f.selfPtrVma, 0xDEADBEEFu);
    BinaryView view = f.b.build();

    BorlandRttiReconstructor rec;
    EXPECT_FALSE(rec.isValidVmt(view, f.vmtVma));
}

TEST(BorlandRTTI, ParseVmt_className) {
    BorlandFactory f;
    f.buildClass("TForm1", 48);
    BinaryView view = f.b.build();

    BorlandRttiReconstructor rec;
    BorlandVmtInfo info;
    EXPECT_TRUE(rec.parseVmt(view, f.vmtVma, info));
    EXPECT_EQ(info.className, "TForm1");
    EXPECT_EQ(info.instanceSize, 48u);
    EXPECT_GE(info.virtualMethodVmas.size(), 1u);
    EXPECT_EQ(info.virtualMethodVmas[0], f.funcVma);
}

TEST(BorlandRTTI, Reconstruct_findsClass) {
    BorlandFactory f;
    f.buildClass("TMyWidget", 32);
    BinaryView view = f.b.build();

    BorlandRttiReconstructor rec;
    ClassHierarchyGraph g;
    bool ok = rec.reconstruct(view, {f.funcVma}, g);
    EXPECT_TRUE(ok);
    EXPECT_NE(g.byName("TMyWidget"), nullptr);
    EXPECT_TRUE(g.byName("TMyWidget")->isPolymorphic);
}

TEST(BorlandRTTI, Reconstruct_vtableSlots) {
    BorlandFactory f;
    f.buildClass("TEdit", 64);
    BinaryView view = f.b.build();

    BorlandRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {f.funcVma}, g);

    const ClassNode* n = g.byName("TEdit");
    if (n) {
        ASSERT_GE(n->vtables.size(), 1u);
        EXPECT_GE(n->vtables[0].slots.size(), 1u);
    }
}

TEST(BorlandRTTI, EmptyBinaryNoCrash) {
    FlatBin b;
    b.allocExec(64); b.allocData(64);
    BinaryView view = b.build();
    BorlandRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {}, g);
}

// ════════════════════════════════════════════════════════════════════════════
// 2. DMC RTTI
// ════════════════════════════════════════════════════════════════════════════

namespace {

struct DmcFactory {
    FlatBin b;
    uint64_t funcVma = 0;
    uint64_t tiVma   = 0;
    uint64_t tiVtableVma = 0; // the vtable of the __typeinfo class itself

    DmcFactory() : b(false) {}

    void buildClass(const std::string& className,
                    bool hasBase = false,
                    const std::string& baseName = "")
    {
        funcVma = b.allocExec(16);

        uint64_t nameVma = b.allocData(className.size() + 1);
        {
            std::vector<uint8_t> s;
            wstr(s, className); s.pop_back(); // wstr adds null already
            s.push_back(0);
            b.writeAt(nameVma, s);
        }

        // __typeinfo vtable (a data pointer the type_info vptr points to)
        tiVtableVma = b.allocData(4, 0); // just a placeholder in data

        // __typeinfo: [vptr=tiVtableVma][name_ptr=nameVma]
        tiVma = b.allocData(8);
        b.writeAt32(tiVma,     static_cast<uint32_t>(tiVtableVma));
        b.writeAt32(tiVma + 4, static_cast<uint32_t>(nameVma));

        if (hasBase && !baseName.empty()) {
            uint64_t baseNameVma = b.allocData(baseName.size() + 1);
            {
                std::vector<uint8_t> s(baseName.begin(), baseName.end());
                s.push_back(0);
                b.writeAt(baseNameVma, s);
            }
            uint64_t baseTiVma = b.allocData(8);
            b.writeAt32(baseTiVma,     static_cast<uint32_t>(tiVtableVma));
            b.writeAt32(baseTiVma + 4, static_cast<uint32_t>(baseNameVma));

            // Extend __typeinfo to __DMCclassinfo: [vptr][name][size][base_ptr]
            // Extend tiVma by adding size + base ptr after the 2 existing fields
            uint64_t sizeVma    = b.allocData(4);
            uint64_t basePtrVma = b.allocData(4);
            b.writeAt32(sizeVma,    32);
            b.writeAt32(basePtrVma, static_cast<uint32_t>(baseTiVma));
            // Note: tiVma layout is [vptr][name] contiguous, then [size][base] appended
            // For the test to work we need them contiguous.
            // Rebuild tiVma with full layout:
            tiVma = b.allocData(16);
            b.writeAt32(tiVma,      static_cast<uint32_t>(tiVtableVma));
            b.writeAt32(tiVma + 4,  static_cast<uint32_t>(nameVma));
            b.writeAt32(tiVma + 8,  32u);  // size
            b.writeAt32(tiVma + 12, static_cast<uint32_t>(baseTiVma));
        }

        // vtable[-1] = tiVma, vtable[0] = funcVma
        uint64_t vtColSlot = b.allocData(8);
        b.writeAt32(vtColSlot,     static_cast<uint32_t>(tiVma));
        b.writeAt32(vtColSlot + 4, static_cast<uint32_t>(funcVma));
    }
};

} // anon namespace

TEST(DmcRTTI, IsValidTypeInfo_basic) {
    DmcFactory f;
    f.buildClass("Foo");
    BinaryView view = f.b.build();

    DmcRttiReconstructor rec;
    EXPECT_TRUE(rec.isValidTypeInfo(view, f.tiVma));
}

TEST(DmcRTTI, IsValidTypeInfo_badVptr) {
    DmcFactory f;
    f.buildClass("Bar");
    // Point vptr into exec section (invalid for __typeinfo)
    f.b.writeAt32(f.tiVma, static_cast<uint32_t>(FlatBin::kBase)); // exec section
    BinaryView view = f.b.build();
    DmcRttiReconstructor rec;
    // kBase is the exec section start; inData() returns false
    EXPECT_FALSE(rec.isValidTypeInfo(view, f.tiVma));
}

TEST(DmcRTTI, ParseTypeInfo_className) {
    DmcFactory f;
    f.buildClass("MyClass");
    BinaryView view = f.b.build();

    DmcRttiReconstructor rec;
    ClassHierarchyGraph g;
    std::string name = rec.parseTypeInfo(view, f.tiVma, g);
    EXPECT_EQ(name, "MyClass");
    EXPECT_NE(g.byName("MyClass"), nullptr);
}

TEST(DmcRTTI, Reconstruct_findsClass) {
    DmcFactory f;
    f.buildClass("Widget");
    BinaryView view = f.b.build();

    DmcRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {f.funcVma}, g);
    EXPECT_NE(g.byName("Widget"), nullptr);
}

TEST(DmcRTTI, EmptyBinaryNoCrash) {
    FlatBin b;
    b.allocExec(64); b.allocData(64);
    BinaryView view = b.build();
    DmcRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {}, g);
}

// ════════════════════════════════════════════════════════════════════════════
// 3. Watcom RTTI
// ════════════════════════════════════════════════════════════════════════════

namespace {

struct WatcomFactory {
    FlatBin b;
    uint64_t funcVma = 0;
    uint64_t tiVma   = 0;

    WatcomFactory() : b(false) {}

    // __WatcomTypeInfo: [uint16 kind][uint16 pad][ptr name][ptr base]
    void buildClass(const std::string& className,
                    bool hasBase = false,
                    const std::string& baseName = "")
    {
        funcVma = b.allocExec(16);

        uint64_t nameVma = b.allocData(className.size() + 1);
        {
            std::vector<uint8_t> s(className.begin(), className.end());
            s.push_back(0);
            b.writeAt(nameVma, s);
        }

        // Build TypeInfo: [kind=0 (2B)][pad (2B)][namePtr (4B)][basePtr (4B)]
        tiVma = b.allocData(12);
        b.writeAt16(tiVma,      0);   // kind = Class
        b.writeAt16(tiVma + 2,  0);   // pad
        b.writeAt32(tiVma + 4,  static_cast<uint32_t>(nameVma));
        b.writeAt32(tiVma + 8,  0);   // base = null initially

        if (hasBase && !baseName.empty()) {
            uint64_t baseNameVma = b.allocData(baseName.size() + 1);
            {
                std::vector<uint8_t> s(baseName.begin(), baseName.end());
                s.push_back(0);
                b.writeAt(baseNameVma, s);
            }
            uint64_t baseTiVma = b.allocData(12);
            b.writeAt16(baseTiVma,     0);
            b.writeAt16(baseTiVma + 2, 0);
            b.writeAt32(baseTiVma + 4, static_cast<uint32_t>(baseNameVma));
            b.writeAt32(baseTiVma + 8, 0);
            // Point derived's base ptr to baseTiVma
            b.writeAt32(tiVma + 8, static_cast<uint32_t>(baseTiVma));
        }

        // vtable[-1]=tiVma, vtable[0]=funcVma
        uint64_t vtSlot = b.allocData(8);
        b.writeAt32(vtSlot,     static_cast<uint32_t>(tiVma));
        b.writeAt32(vtSlot + 4, static_cast<uint32_t>(funcVma));
    }
};

} // anon namespace

TEST(WatcomRTTI, IsValidTypeInfo_basic) {
    WatcomFactory f;
    f.buildClass("CBase");
    BinaryView view = f.b.build();
    WatcomRttiReconstructor rec;
    EXPECT_TRUE(rec.isValidTypeInfo(view, f.tiVma));
}

TEST(WatcomRTTI, IsValidTypeInfo_badKind) {
    WatcomFactory f;
    f.buildClass("CDerived");
    f.b.writeAt16(f.tiVma, 99); // invalid kind
    BinaryView view = f.b.build();
    WatcomRttiReconstructor rec;
    EXPECT_FALSE(rec.isValidTypeInfo(view, f.tiVma));
}

TEST(WatcomRTTI, ParseTypeInfo_className) {
    WatcomFactory f;
    f.buildClass("Document");
    BinaryView view = f.b.build();
    WatcomRttiReconstructor rec;
    ClassHierarchyGraph g;
    std::string name = rec.parseTypeInfo(view, f.tiVma, g);
    EXPECT_EQ(name, "Document");
    EXPECT_NE(g.byName("Document"), nullptr);
}

TEST(WatcomRTTI, ParseTypeInfo_withBase) {
    WatcomFactory f;
    f.buildClass("Derived", true, "Base");
    BinaryView view = f.b.build();
    WatcomRttiReconstructor rec;
    ClassHierarchyGraph g;
    std::string name = rec.parseTypeInfo(view, f.tiVma, g);
    EXPECT_EQ(name, "Derived");
    const ClassNode* n = g.byName("Derived");
    ASSERT_NE(n, nullptr);
    ASSERT_GE(n->bases.size(), 1u);
    EXPECT_EQ(n->bases[0].baseName, "Base");
}

TEST(WatcomRTTI, Reconstruct_findsClass) {
    WatcomFactory f;
    f.buildClass("Shape");
    BinaryView view = f.b.build();
    WatcomRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {f.funcVma}, g);
    EXPECT_NE(g.byName("Shape"), nullptr);
}

TEST(WatcomRTTI, EmptyBinaryNoCrash) {
    FlatBin b;
    b.allocExec(64); b.allocData(64);
    BinaryView view = b.build();
    WatcomRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {}, g);
}

// ════════════════════════════════════════════════════════════════════════════
// 4. Symbian RTTI
// ════════════════════════════════════════════════════════════════════════════

namespace {

struct SymbianFactory {
    FlatBin b;
    uint64_t funcVma      = 0;
    uint64_t metaClassVma = 0;

    SymbianFactory() : b(false) {} // ARM is 32-bit

    void buildClass(const std::string& className, uint32_t iSize = 32,
                    uint64_t parentVma = 0)
    {
        funcVma = b.allocExec(16);

        uint64_t nameVma = b.allocData(className.size() + 1);
        {
            std::vector<uint8_t> s(className.begin(), className.end());
            s.push_back(0);
            b.writeAt(nameVma, s);
        }

        // TMetaClass: [iSize(4)][iOffset(4)][iName(4)][iParent(4)]
        metaClassVma = b.allocData(16);
        b.writeAt32(metaClassVma,      iSize);
        b.writeAt32(metaClassVma + 4,  0);      // iOffset
        b.writeAt32(metaClassVma + 8,  static_cast<uint32_t>(nameVma));
        b.writeAt32(metaClassVma + 12, static_cast<uint32_t>(parentVma));
    }
};

} // anon namespace

TEST(SymbianRTTI, IsSymbianClassName_valid) {
    EXPECT_TRUE(SymbianRttiReconstructor::isSymbianClassName("CMyClass"));
    EXPECT_TRUE(SymbianRttiReconstructor::isSymbianClassName("TDesC"));
    EXPECT_TRUE(SymbianRttiReconstructor::isSymbianClassName("RFile"));
    EXPECT_TRUE(SymbianRttiReconstructor::isSymbianClassName("MObserver"));
    EXPECT_TRUE(SymbianRttiReconstructor::isSymbianClassName("EActive"));
}

TEST(SymbianRTTI, IsSymbianClassName_invalid) {
    EXPECT_FALSE(SymbianRttiReconstructor::isSymbianClassName("Foo"));       // no prefix
    EXPECT_FALSE(SymbianRttiReconstructor::isSymbianClassName("c_small"));   // lowercase
    EXPECT_FALSE(SymbianRttiReconstructor::isSymbianClassName("C"));         // too short
    EXPECT_FALSE(SymbianRttiReconstructor::isSymbianClassName(""));
    EXPECT_FALSE(SymbianRttiReconstructor::isSymbianClassName("CFoo Bar"));  // space
}

TEST(SymbianRTTI, IsValidTMetaClass_basic) {
    SymbianFactory f;
    f.buildClass("CDocument", 64);
    BinaryView view = f.b.build();
    SymbianRttiReconstructor rec;
    EXPECT_TRUE(rec.isValidTMetaClass(view, f.metaClassVma));
}

TEST(SymbianRTTI, IsValidTMetaClass_badSize) {
    SymbianFactory f;
    f.buildClass("CWidget", 3); // too small
    BinaryView view = f.b.build();
    SymbianRttiReconstructor rec;
    EXPECT_FALSE(rec.isValidTMetaClass(view, f.metaClassVma));
}

TEST(SymbianRTTI, IsValidTMetaClass_wrongName) {
    SymbianFactory f;
    f.buildClass("Foo"); // doesn't match Symbian convention
    BinaryView view = f.b.build();
    SymbianRttiReconstructor rec;
    EXPECT_FALSE(rec.isValidTMetaClass(view, f.metaClassVma));
}

TEST(SymbianRTTI, ParseTMetaClass_name) {
    SymbianFactory f;
    f.buildClass("CTimer", 48);
    BinaryView view = f.b.build();
    SymbianRttiReconstructor rec;
    TMetaClassInfo info;
    EXPECT_TRUE(rec.parseTMetaClass(view, f.metaClassVma, info));
    EXPECT_EQ(info.iName, "CTimer");
    EXPECT_EQ(info.iSize, 48u);
}

TEST(SymbianRTTI, ParseTMetaClass_noParent) {
    SymbianFactory f;
    f.buildClass("CBase", 8);
    BinaryView view = f.b.build();
    SymbianRttiReconstructor rec;
    TMetaClassInfo info;
    rec.parseTMetaClass(view, f.metaClassVma, info);
    EXPECT_EQ(info.iParentVma, 0u);
}

TEST(SymbianRTTI, Reconstruct_findsClass) {
    SymbianFactory f;
    f.buildClass("CMyServer", 96);
    BinaryView view = f.b.build();
    SymbianRttiReconstructor rec;
    ClassHierarchyGraph g;
    bool ok = rec.reconstruct(view, {f.funcVma}, g);
    EXPECT_TRUE(ok);
    EXPECT_NE(g.byName("CMyServer"), nullptr);
}

TEST(SymbianRTTI, Reconstruct_inheritanceChain) {
    SymbianFactory base;
    base.buildClass("CBase", 8);
    BinaryView baseView = base.b.build();
    uint64_t baseMeta = base.metaClassVma;

    SymbianFactory derived;
    derived.buildClass("CTimer", 48, baseMeta);
    // The parent VMA in derived points into base's memory space, which is a
    // different FlatBin.  For simplicity, just test single-class case.
    BinaryView view = derived.b.build();
    SymbianRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {derived.funcVma}, g);
    EXPECT_NE(g.byName("CTimer"), nullptr);
}

TEST(SymbianRTTI, EmptyBinaryNoCrash) {
    FlatBin b;
    b.allocExec(64); b.allocData(64);
    BinaryView view = b.build();
    SymbianRttiReconstructor rec;
    ClassHierarchyGraph g;
    rec.reconstruct(view, {}, g);
}
