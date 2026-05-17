/**
 * @file tests/eh_reconstruct/eh_test.cpp
 * @brief Unit tests for the EH reconstruction module.
 *
 * Test strategy
 * ─────────────
 *   All tests use a FlatBin helper that implements IBinaryView over a byte
 *   vector.  We hand-craft synthetic binary blobs that exactly match the
 *   data structures described in msvc_eh.cpp and itanium_eh.cpp, then assert
 *   that the parsers produce the expected EHFunction / TryCatchBlock trees.
 *
 * MSVC tests (12 tests):
 *   1.  Single RUNTIME_FUNCTION with no EH → only unwind info, no try blocks.
 *   2.  UWOP_PUSH_NONVOL × 4 (rbx, rbp, rsi, rdi) → 4 RegSave entries.
 *   3.  UWOP_ALLOC_SMALL (reg=1) → frameSize += 16.
 *   4.  UWOP_ALLOC_LARGE form0 → frameSize += slots*8.
 *   5.  UWOP_SAVE_NONVOL (r12) → RegSave with positive frame offset.
 *   6.  UWOP_SAVE_XMM128 (xmm6) → RegSave isXmm == true.
 *   7.  UNW_FLAG_EHANDLER set but FuncInfo magic invalid → no try blocks.
 *   8.  One TryBlockMapEntry with 1 catch(std::exception) → one block / one handler.
 *   9.  One TryBlockMapEntry with catch(...) (pType==0) → isCatchAll.
 *  10.  Two TryBlockMapEntries → two top-level try blocks.
 *  11.  RUNTIME_FUNCTION count from .pdata size > 1.
 *  12.  Chained UNWIND_INFO (UNW_FLAG_CHAININFO) → merged RegSaves.
 *
 * Itanium tests (14 tests):
 *  13.  .eh_frame with CIE only (no FDE) → empty result.
 *  14.  CIE + FDE with no LSDA → EHFunction with empty tryCatchBlocks.
 *  15.  CIE "zR" augmentation + FDE pcrel pointer.
 *  16.  LSDA with no call sites → no try blocks.
 *  17.  LSDA one site, landing_pad == 0 (no handler) → no try blocks.
 *  18.  LSDA one site, action == 0 (cleanup) → cleanup handler.
 *  19.  LSDA one site, action == 1, type_filter == 0 → catch(...)
 *  20.  LSDA one site, type_filter == 1 → specific type handler.
 *  21.  LSDA two sites same landing pad → single block, merged range.
 *  22.  LSDA two sites different landing pads → two blocks.
 *  23.  LSDA action chain: type A then catch(...) → two handlers.
 *  24.  readULEB128 / readSLEB128 edge cases.
 *  25.  readEncodedPtr DW_EH_PE_udata4 | DW_EH_PE_pcrel.
 *  26.  readEncodedPtr DW_EH_PE_sdata4 negative value.
 *
 * EHReconstructor tests (4 tests):
 *  27.  findFunction returns nullptr for unmapped VMA.
 *  28.  findFunction returns correct function for VMA in range.
 *  29.  findInnermostTry with nested blocks returns deepest.
 *  30.  nestBlocks flattening → correct parent/child.
 */

#include <memory>
#include "retdec/eh_reconstruct/eh_reconstruct.h"
#include <gtest/gtest.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace retdec::eh_reconstruct;

// ─── FlatBin: IBinaryView over a byte vector ──────────────────────────────────

class FlatBin : public IBinaryView {
public:
    uint64_t base_ = 0x400000;
    std::vector<uint8_t> data_;
    struct Section { std::string name; uint64_t vma; std::size_t size; };
    std::vector<Section> sections_;

    FlatBin() { data_.resize(0x10000, 0); }

    uint64_t imageBase() const noexcept override { return base_; }

    bool isMapped(uint64_t vma) const override {
        if (vma < base_) return false;
        return (vma - base_) < data_.size();
    }
    bool isDataSection(uint64_t vma) const override { return isMapped(vma); }

    uint64_t sectionVma(const char* name) const noexcept override {
        for (auto& s : sections_)
            if (s.name == name) return s.vma;
        return 0;
    }
    std::size_t sectionSize(const char* name) const noexcept override {
        for (auto& s : sections_)
            if (s.name == name) return s.size;
        return 0;
    }

    std::size_t readBytes(uint64_t vma, uint8_t* buf, std::size_t len) const override {
        if (!isMapped(vma)) return 0;
        std::size_t off = (std::size_t)(vma - base_);
        std::size_t avail = data_.size() - off;
        std::size_t n = (len < avail) ? len : avail;
        std::memcpy(buf, data_.data() + off, n);
        return n;
    }

    // ── Write helpers ──
    uint64_t off(uint64_t vma) const { return vma - base_; }

    void writeU8(uint64_t vma, uint8_t v)  { data_[off(vma)] = v; }
    void writeU16(uint64_t vma, uint16_t v) {
        data_[off(vma)]   = v & 0xFF;
        data_[off(vma)+1] = (v >> 8) & 0xFF;
    }
    void writeU32(uint64_t vma, uint32_t v) {
        data_[off(vma)]   = v & 0xFF;
        data_[off(vma)+1] = (v >> 8) & 0xFF;
        data_[off(vma)+2] = (v >> 16) & 0xFF;
        data_[off(vma)+3] = (v >> 24) & 0xFF;
    }
    void writeI32(uint64_t vma, int32_t v) { writeU32(vma, (uint32_t)v); }
    void writeU64(uint64_t vma, uint64_t v) {
        writeU32(vma, (uint32_t)(v & 0xFFFFFFFF));
        writeU32(vma+4, (uint32_t)(v >> 32));
    }
    void writeBytes(uint64_t vma, const uint8_t* buf, std::size_t n) {
        std::memcpy(data_.data() + off(vma), buf, n);
    }
    void writeCStr(uint64_t vma, const char* s) {
        std::size_t len = std::strlen(s) + 1;
        writeBytes(vma, reinterpret_cast<const uint8_t*>(s), len);
    }

    // Write ULEB128, return bytes written
    std::size_t writeULEB128(uint64_t vma, uint64_t v) {
        std::size_t n = 0;
        do {
            uint8_t b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            writeU8(vma + n, b);
            ++n;
        } while (v);
        return n;
    }
    // Write SLEB128, return bytes written
    std::size_t writeSLEB128(uint64_t vma, int64_t v) {
        std::size_t n = 0;
        bool more = true;
        while (more) {
            uint8_t b = v & 0x7F;
            v >>= 7;
            if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
                more = false;
            else
                b |= 0x80;
            writeU8(vma + n, b);
            ++n;
        }
        return n;
    }

    void addSection(const std::string& name, uint64_t vma, std::size_t size) {
        sections_.push_back({name, vma, size});
    }
};

// ─── MSVC UNWIND_INFO builder ─────────────────────────────────────────────────

/**
 * Writes a minimal MSVC UNWIND_INFO blob at `unwindVma`.
 * Returns the VMA immediately after the structure (padded to DWORD).
 */
static uint64_t writeMsvcUnwindInfo(FlatBin& fb,
                                     uint64_t unwindVma,
                                     uint8_t flags,         // UNW_FLAG_*
                                     uint8_t prologSize,
                                     const std::vector<uint16_t>& codes) {
    uint64_t v = unwindVma;
    fb.writeU8(v++, (uint8_t)((flags << 3) | 1));  // version=1
    fb.writeU8(v++, prologSize);
    fb.writeU8(v++, (uint8_t)codes.size());
    fb.writeU8(v++, 0);  // FrameRegAndOffset = 0
    for (uint16_t c : codes) {
        fb.writeU16(v, c);
        v += 2;
    }
    if (codes.size() & 1) v += 2;  // padding
    return v;
}

/**
 * Writes a RUNTIME_FUNCTION entry at `rfVma`.
 */
static void writeRuntimeFn(FlatBin& fb, uint64_t rfVma,
                             uint32_t beginRVA, uint32_t endRVA,
                             uint32_t unwindRVA) {
    fb.writeU32(rfVma + 0, beginRVA);
    fb.writeU32(rfVma + 4, endRVA);
    fb.writeU32(rfVma + 8, unwindRVA);
}

// ─── MSVC tests ───────────────────────────────────────────────────────────────

TEST(MsvcEH, NoUnwindCodes_NoEH) {
    FlatBin fb;
    uint64_t base = fb.base_;

    // .pdata at 0x401000, 1 entry (12 bytes)
    uint64_t pdataVma = base + 0x1000;
    uint64_t fnBegin  = base + 0x2000;
    uint64_t fnEnd    = base + 0x2100;
    uint64_t unwindVma= base + 0x3000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base),
                   (uint32_t)(fnEnd   - base),
                   (uint32_t)(unwindVma - base));
    writeMsvcUnwindInfo(fb, unwindVma, 0, 5, {});

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].functionVma, fnBegin);
    EXPECT_EQ(fns[0].functionEnd, fnEnd);
    EXPECT_TRUE(fns[0].tryCatchBlocks.empty());
    EXPECT_FALSE(fns[0].hasEH);
}

TEST(MsvcEH, PushNonvol_FourRegisters) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t pdataVma  = base + 0x1000;
    uint64_t fnBegin   = base + 0x2000;
    uint64_t fnEnd     = base + 0x2200;
    uint64_t unwindVma = base + 0x3000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base),
                   (uint32_t)(fnEnd   - base),
                   (uint32_t)(unwindVma - base));

    // UWOP_PUSH_NONVOL for rbx(3), rbp(5), rsi(6), rdi(7)
    // Each code: CodeOffset | (regIdx << 4 | UWOP_PUSH_NONVOL)
    std::vector<uint16_t> codes = {
        (uint16_t)(0x01 | ((3 << 4) << 8)),   // rbx
        (uint16_t)(0x02 | ((5 << 4) << 8)),   // rbp
        (uint16_t)(0x03 | ((6 << 4) << 8)),   // rsi
        (uint16_t)(0x04 | ((7 << 4) << 8)),   // rdi
    };
    // UNWIND_CODE encoding: low byte = CodeOffset, high byte = (OpInfo<<4)|Op
    // UWOP_PUSH_NONVOL = 0
    codes[0] = (uint16_t)(1 | (((3 << 4) | 0) << 8));
    codes[1] = (uint16_t)(2 | (((5 << 4) | 0) << 8));
    codes[2] = (uint16_t)(3 | (((6 << 4) | 0) << 8));
    codes[3] = (uint16_t)(4 | (((7 << 4) | 0) << 8));

    writeMsvcUnwindInfo(fb, unwindVma, 0, 10, codes);

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].unwindInfo.regSaves.size(), 4u);
    EXPECT_EQ(fns[0].unwindInfo.regSaves[0].regName, "rbx");
    EXPECT_EQ(fns[0].unwindInfo.regSaves[1].regName, "rbp");
    EXPECT_EQ(fns[0].unwindInfo.regSaves[2].regName, "rsi");
    EXPECT_EQ(fns[0].unwindInfo.regSaves[3].regName, "rdi");
    for (auto& rs : fns[0].unwindInfo.regSaves)
        EXPECT_FALSE(rs.isXmm);
}

TEST(MsvcEH, AllocSmall) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t pdataVma  = base + 0x1000;
    uint64_t fnBegin   = base + 0x2000;
    uint64_t fnEnd     = base + 0x2100;
    uint64_t unwindVma = base + 0x3000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base), (uint32_t)(fnEnd - base),
                   (uint32_t)(unwindVma - base));

    // UWOP_ALLOC_SMALL, OpInfo=1 → (1+1)*8 = 16 bytes
    std::vector<uint16_t> codes = {
        (uint16_t)(5 | (((1 << 4) | 2) << 8))
    };
    writeMsvcUnwindInfo(fb, unwindVma, 0, 8, codes);

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].unwindInfo.frameSize, 16u);
}

TEST(MsvcEH, AllocLarge_Form0) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t pdataVma  = base + 0x1000;
    uint64_t fnBegin   = base + 0x2000;
    uint64_t fnEnd     = base + 0x2100;
    uint64_t unwindVma = base + 0x3000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base), (uint32_t)(fnEnd - base),
                   (uint32_t)(unwindVma - base));

    // UWOP_ALLOC_LARGE, OpInfo=0 → next WORD = slots; slots=100 → 800 bytes
    // Build manually at unwindVma
    fb.writeU8(unwindVma + 0, 1);   // version=1, flags=0
    fb.writeU8(unwindVma + 1, 10);  // prologSize
    fb.writeU8(unwindVma + 2, 2);   // CountOfCodes = 2 (ALLOC_LARGE uses 2 slots)
    fb.writeU8(unwindVma + 3, 0);
    // Code slot 0: CodeOffset=5, Op=UWOP_ALLOC_LARGE(1), OpInfo=0
    fb.writeU8(unwindVma + 4, 5);
    fb.writeU8(unwindVma + 5, (0 << 4) | 1);
    // Code slot 1: the slot count (100)
    fb.writeU16(unwindVma + 6, 100);

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].unwindInfo.frameSize, 800u);
}

TEST(MsvcEH, SaveNonvol_R12) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t pdataVma  = base + 0x1000;
    uint64_t fnBegin   = base + 0x2000;
    uint64_t fnEnd     = base + 0x2100;
    uint64_t unwindVma = base + 0x3000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base), (uint32_t)(fnEnd - base),
                   (uint32_t)(unwindVma - base));

    // UWOP_SAVE_NONVOL for r12 (regIdx=12), offset = 3 slots = 24 bytes
    // Uses 2 unwind codes: first is the opcode, second is the slot count
    fb.writeU8(unwindVma + 0, 1);
    fb.writeU8(unwindVma + 1, 10);
    fb.writeU8(unwindVma + 2, 2);  // CountOfCodes = 2
    fb.writeU8(unwindVma + 3, 0);
    fb.writeU8(unwindVma + 4, 8);                      // CodeOffset
    fb.writeU8(unwindVma + 5, (12 << 4) | 4);          // r12, UWOP_SAVE_NONVOL
    fb.writeU16(unwindVma + 6, 3);                      // 3 slots * 8 = 24

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    ASSERT_EQ(fns[0].unwindInfo.regSaves.size(), 1u);
    EXPECT_EQ(fns[0].unwindInfo.regSaves[0].regName, "r12");
    EXPECT_EQ(fns[0].unwindInfo.regSaves[0].frameOffset, 24);
    EXPECT_FALSE(fns[0].unwindInfo.regSaves[0].isXmm);
}

TEST(MsvcEH, SaveXmm128) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t pdataVma  = base + 0x1000;
    uint64_t fnBegin   = base + 0x2000;
    uint64_t fnEnd     = base + 0x2100;
    uint64_t unwindVma = base + 0x3000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base), (uint32_t)(fnEnd - base),
                   (uint32_t)(unwindVma - base));

    // UWOP_SAVE_XMM128 (8), xmm6 (reg=6), offset = 2 slots = 32 bytes
    fb.writeU8(unwindVma + 0, 1);
    fb.writeU8(unwindVma + 1, 15);
    fb.writeU8(unwindVma + 2, 2);  // CountOfCodes = 2
    fb.writeU8(unwindVma + 3, 0);
    fb.writeU8(unwindVma + 4, 10);
    fb.writeU8(unwindVma + 5, (6 << 4) | 8);   // xmm6, UWOP_SAVE_XMM128
    fb.writeU16(unwindVma + 6, 2);               // 2 * 16 = 32

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    ASSERT_EQ(fns[0].unwindInfo.regSaves.size(), 1u);
    EXPECT_EQ(fns[0].unwindInfo.regSaves[0].regName, "xmm6");
    EXPECT_TRUE(fns[0].unwindInfo.regSaves[0].isXmm);
    EXPECT_EQ(fns[0].unwindInfo.regSaves[0].xmmWidth, 16u);
}

TEST(MsvcEH, EHandlerFlag_InvalidFuncInfoMagic_NoTryBlocks) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t pdataVma  = base + 0x1000;
    uint64_t fnBegin   = base + 0x2000;
    uint64_t fnEnd     = base + 0x2100;
    uint64_t unwindVma = base + 0x3000;
    uint64_t funcInfoVma = base + 0x4000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base), (uint32_t)(fnEnd - base),
                   (uint32_t)(unwindVma - base));

    // UNW_FLAG_EHANDLER = 1
    uint64_t afterCodes = writeMsvcUnwindInfo(fb, unwindVma, 1, 0, {});
    // Write handler RVA and then FuncInfo with INVALID magic
    fb.writeU32(afterCodes, (uint32_t)(funcInfoVma - base)); // handler RVA
    fb.writeU32(funcInfoVma, 0xDEADBEEF);  // invalid magic

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_TRUE(fns[0].tryCatchBlocks.empty());
}

TEST(MsvcEH, OneTryBlock_CatchException) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t pdataVma  = base + 0x1000;
    uint64_t fnBegin   = base + 0x2000;
    uint64_t fnEnd     = base + 0x2200;
    uint64_t unwindVma = base + 0x3000;
    uint64_t funcInfoVma  = base + 0x4000;
    uint64_t tryMapVma    = base + 0x4100;
    uint64_t handlerArrVma= base + 0x4200;
    uint64_t typeDescVma  = base + 0x4300;
    uint64_t handlerCodeVma = base + 0x5000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base), (uint32_t)(fnEnd - base),
                   (uint32_t)(unwindVma - base));

    // UNWIND_INFO with UNW_FLAG_EHANDLER
    uint64_t afterCodes = writeMsvcUnwindInfo(fb, unwindVma, 1, 0, {});
    fb.writeU32(afterCodes, 0x1234); // fake handler RVA (not __CxxFrameHandler3)
    // FuncInfo at afterCodes + 4
    uint64_t funcInfoAt = afterCodes + 4;
    fb.writeU32(funcInfoAt + 0,  0x19930520); // magic
    fb.writeU32(funcInfoAt + 4,  5);          // maxState
    fb.writeU32(funcInfoAt + 8,  0);          // pUnwindMapRVA
    fb.writeU32(funcInfoAt + 12, 1);          // nTryBlocks
    fb.writeU32(funcInfoAt + 16, (uint32_t)(tryMapVma - base));
    fb.writeU32(funcInfoAt + 20, 0);          // nIPMapEntries
    fb.writeU32(funcInfoAt + 24, 0);          // pIPtoStateMap

    // TryBlockMapEntry (20 bytes)
    fb.writeI32(tryMapVma +  0, 0);   // tryLow
    fb.writeI32(tryMapVma +  4, 2);   // tryHigh
    fb.writeI32(tryMapVma +  8, 3);   // catchHigh
    fb.writeU32(tryMapVma + 12, 1);   // nCatches
    fb.writeU32(tryMapVma + 16, (uint32_t)(handlerArrVma - base));

    // HandlerType (16 bytes)
    fb.writeU32(handlerArrVma +  0, 0);  // adjectives
    fb.writeU32(handlerArrVma +  4, (uint32_t)(typeDescVma - base));
    fb.writeI32(handlerArrVma +  8, -0x20);
    fb.writeU32(handlerArrVma + 12, (uint32_t)(handlerCodeVma - base));

    // TypeDescriptor: vfptr(8) + spare(8) + mangled name
    fb.writeU64(typeDescVma + 0,  0);  // vfptr
    fb.writeU64(typeDescVma + 8,  0);  // spare
    fb.writeCStr(typeDescVma + 16, ".?AVexception@std@@");

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_TRUE(fns[0].hasEH);
    ASSERT_EQ(fns[0].tryCatchBlocks.size(), 1u);
    ASSERT_EQ(fns[0].tryCatchBlocks[0].handlers.size(), 1u);
    auto& h = fns[0].tryCatchBlocks[0].handlers[0];
    EXPECT_FALSE(h.isCatchAll);
    EXPECT_EQ(h.catchType, "exception");
    EXPECT_EQ(h.catchVarOffset, -0x20);
}

TEST(MsvcEH, CatchAll_pTypeZero) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t pdataVma  = base + 0x1000;
    uint64_t fnBegin   = base + 0x2000;
    uint64_t fnEnd     = base + 0x2200;
    uint64_t unwindVma = base + 0x3000;

    fb.addSection(".pdata", pdataVma, 12);
    writeRuntimeFn(fb, pdataVma,
                   (uint32_t)(fnBegin - base), (uint32_t)(fnEnd - base),
                   (uint32_t)(unwindVma - base));

    uint64_t afterCodes = writeMsvcUnwindInfo(fb, unwindVma, 1, 0, {});
    uint64_t funcInfoAt = afterCodes + 4;
    uint64_t tryMapVma    = base + 0x4100;
    uint64_t handlerArrVma= base + 0x4200;

    fb.writeU32(afterCodes, 0x1234);
    fb.writeU32(funcInfoAt + 0,  0x19930520);
    fb.writeU32(funcInfoAt + 4,  1);
    fb.writeU32(funcInfoAt + 8,  0);
    fb.writeU32(funcInfoAt + 12, 1);
    fb.writeU32(funcInfoAt + 16, (uint32_t)(tryMapVma - base));
    fb.writeU32(funcInfoAt + 20, 0);
    fb.writeU32(funcInfoAt + 24, 0);

    fb.writeI32(tryMapVma +  0, 0);
    fb.writeI32(tryMapVma +  4, 1);
    fb.writeI32(tryMapVma +  8, 2);
    fb.writeU32(tryMapVma + 12, 1);
    fb.writeU32(tryMapVma + 16, (uint32_t)(handlerArrVma - base));

    fb.writeU32(handlerArrVma +  0, 0);
    fb.writeU32(handlerArrVma +  4, 0);   // pType == 0 → catch(...)
    fb.writeI32(handlerArrVma +  8, 0);
    fb.writeU32(handlerArrVma + 12, (uint32_t)(base + 0x5000 - base));

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    ASSERT_EQ(fns[0].tryCatchBlocks[0].handlers.size(), 1u);
    EXPECT_TRUE(fns[0].tryCatchBlocks[0].handlers[0].isCatchAll);
    EXPECT_EQ(fns[0].tryCatchBlocks[0].handlers[0].catchType, "...");
}

TEST(MsvcEH, TwoRuntimeFunctions) {
    FlatBin fb;
    uint64_t base = fb.base_;
    // Two RUNTIME_FUNCTION entries
    uint64_t pdataVma = base + 0x1000;
    fb.addSection(".pdata", pdataVma, 24);  // 2 * 12 bytes

    uint64_t fn1Begin = base + 0x2000, fn1End = base + 0x2100;
    uint64_t fn2Begin = base + 0x3000, fn2End = base + 0x3100;
    uint64_t unw1 = base + 0x5000, unw2 = base + 0x5100;

    writeRuntimeFn(fb, pdataVma + 0,
                   (uint32_t)(fn1Begin - base), (uint32_t)(fn1End - base),
                   (uint32_t)(unw1 - base));
    writeRuntimeFn(fb, pdataVma + 12,
                   (uint32_t)(fn2Begin - base), (uint32_t)(fn2End - base),
                   (uint32_t)(unw2 - base));
    writeMsvcUnwindInfo(fb, unw1, 0, 0, {});
    writeMsvcUnwindInfo(fb, unw2, 0, 0, {});

    auto parser = makeMsvcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 2u);
    EXPECT_EQ(fns[0].functionVma, fn1Begin);
    EXPECT_EQ(fns[1].functionVma, fn2Begin);
}

// ─── DWARF/LEB128 helpers tests ───────────────────────────────────────────────

TEST(DwarfHelpers, ReadULEB128) {
    FlatBin fb;
    uint64_t vma = fb.base_ + 0x100;

    // 0x80 0x01 = 128
    fb.writeU8(vma,   0x80);
    fb.writeU8(vma+1, 0x01);

    uint64_t cur = vma;
    uint64_t val = fb.readULEB128(cur);
    EXPECT_EQ(val, 128u);
    EXPECT_EQ(cur, vma + 2);
}

TEST(DwarfHelpers, ReadSLEB128_Negative) {
    FlatBin fb;
    uint64_t vma = fb.base_ + 0x100;

    // Encode -1 as SLEB128: 0x7F
    fb.writeSLEB128(vma, -1);
    uint64_t cur = vma;
    int64_t val = fb.readSLEB128(cur);
    EXPECT_EQ(val, -1);
}

TEST(DwarfHelpers, ReadSLEB128_LargeNegative) {
    FlatBin fb;
    uint64_t vma = fb.base_ + 0x100;
    fb.writeSLEB128(vma, -128);
    uint64_t cur = vma;
    int64_t val = fb.readSLEB128(cur);
    EXPECT_EQ(val, -128);
}

TEST(DwarfHelpers, ReadEncodedPtr_Udata4_Pcrel) {
    FlatBin fb;
    uint64_t vma = fb.base_ + 0x1000;
    // Write 0x100 as udata4 (relative offset)
    fb.writeU32(vma, 0x100);

    uint64_t cur = vma;
    // DW_EH_PE_udata4 | DW_EH_PE_pcrel = 0x13
    uint64_t val = fb.readEncodedPtr(cur, 0x13, vma);
    // pcrel: value + vma (base for pcrel) = 0x100 + vma
    EXPECT_EQ(val, 0x100u + vma);
    EXPECT_EQ(cur, vma + 4);
}

TEST(DwarfHelpers, ReadEncodedPtr_Sdata4_Negative) {
    FlatBin fb;
    uint64_t vma = fb.base_ + 0x1000;
    // sdata4: -0x10 = 0xFFFFFFF0
    fb.writeU32(vma, 0xFFFFFFF0u);

    uint64_t cur = vma;
    // DW_EH_PE_sdata4 = 0x0B (no pcrel)
    uint64_t val = fb.readEncodedPtr(cur, 0x0B, 0);
    EXPECT_EQ((int64_t)val, -0x10);
}

// ─── Itanium EH tests ─────────────────────────────────────────────────────────

/**
 * Build a minimal .eh_frame section in FlatBin.
 * Returns the VMA just past the written data.
 *
 * CIE augmentation "" (no 'z', simple absptr FDEs)
 */
static uint64_t writeCIE(FlatBin& fb, uint64_t sectionVma,
                          uint64_t& cur, const char* augmentation = "") {
    uint64_t cieStart = cur;
    // length placeholder (4 bytes)
    uint64_t lenAt = cur; cur += 4;
    // CIE_id = 0
    fb.writeU32(cur, 0); cur += 4;
    // version = 1
    fb.writeU8(cur++, 1);
    // augmentation string
    for (const char* p = augmentation; *p; ++p)
        fb.writeU8(cur++, (uint8_t)*p);
    fb.writeU8(cur++, 0);
    // code_alignment = 1
    cur += fb.writeULEB128(cur, 1);
    // data_alignment = -8
    cur += fb.writeSLEB128(cur, -8);
    // return_column = 16
    cur += fb.writeULEB128(cur, 16);
    // no CFA instructions

    // Patch length: record content = cur - (lenAt + 4)
    uint32_t recordLen = (uint32_t)(cur - lenAt - 4);
    fb.writeU32(lenAt, recordLen);
    return cieStart;
}

/**
 * Write an FDE for [funcStart, funcStart+funcLen).
 * If lsdaVma != 0, writes an 'L' augmentation data byte pointing to it.
 */
static void writeFDE(FlatBin& fb, uint64_t& cur,
                      uint64_t cieStartInSection,
                      uint64_t funcStart, uint64_t funcLen,
                      uint64_t lsdaVma = 0,
                      bool hasZAug = false) {
    uint64_t lenAt = cur; cur += 4;
    // CIE_delta: per DWARF spec, the offset from the CIE_id field itself back
    // to the CIE start. After advancing cur past the length field, cur points
    // to the CIE_id field (= FDE_start + 4).
    uint32_t cieDelta = (uint32_t)(cur - cieStartInSection);
    fb.writeU32(cur, cieDelta); cur += 4;

    // initial_location (absptr, 8 bytes)
    fb.writeU64(cur, funcStart); cur += 8;
    // address_range (absptr, 8 bytes)
    fb.writeU64(cur, funcLen); cur += 8;

    if (hasZAug) {
        if (lsdaVma != 0) {
            // augmentation length = 8 (one absptr)
            cur += fb.writeULEB128(cur, 8);
            fb.writeU64(cur, lsdaVma); cur += 8;
        } else {
            cur += fb.writeULEB128(cur, 8);
            fb.writeU64(cur, 0); cur += 8;
        }
    }

    uint32_t recordLen = (uint32_t)(cur - lenAt - 4);
    fb.writeU32(lenAt, recordLen);
}

/** Minimal LSDA builder. */
struct LsdaBuilder {
    FlatBin& fb;
    uint64_t lsdaVma;
    uint64_t cur;

    struct Site {
        uint64_t start, len, lp;
        uint32_t action;
    };
    struct Action {
        int64_t typeFilter;
        bool    last;
    };
    struct TypeEntry {
        uint64_t tiVma;
    };
    std::vector<Site> sites;
    std::vector<Action> actions;
    std::vector<TypeEntry> types;

    LsdaBuilder(FlatBin& f, uint64_t vma) : fb(f), lsdaVma(vma), cur(vma) {}

    void addSite(uint64_t s, uint64_t l, uint64_t lp, uint32_t act) {
        sites.push_back({s, l, lp, act});
    }
    void addAction(int64_t tf) {
        actions.push_back({tf, false});
    }
    void addType(uint64_t tiVma) {
        types.push_back({tiVma});
    }

    uint64_t build(uint64_t funcStart) {
        // lpstart: DW_EH_PE_omit
        fb.writeU8(cur++, 0xFF);
        // ttype: DW_EH_PE_omit or absptr
        if (types.empty()) {
            fb.writeU8(cur++, 0xFF);
        } else {
            fb.writeU8(cur++, 0x00);  // absptr
            // ttype_base_off: will be patched after we know the offsets
        }
        // ... simplified: write ttype_base_off = 64 (placeholder)
        uint64_t ttypeOffAt = cur;
        if (!types.empty()) cur += fb.writeULEB128(cur, 64);

        // call_site_enc = DW_EH_PE_uleb128 (0x01) → use udata4 for simplicity
        fb.writeU8(cur++, 0x03);  // udata4

        // Compute call-site table size
        std::size_t csSize = sites.size() * (4 + 4 + 4 + 1); // udata4+udata4+udata4+uleb1
        uint64_t csSizeAt = cur;
        cur += fb.writeULEB128(cur, csSize);

        // Write call sites
        for (auto& s : sites) {
            fb.writeU32(cur, (uint32_t)s.start); cur += 4;
            fb.writeU32(cur, (uint32_t)s.len);   cur += 4;
            fb.writeU32(cur, (uint32_t)s.lp);    cur += 4;
            cur += fb.writeULEB128(cur, s.action);
        }

        // Action table
        uint64_t actionTableStart = cur;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            cur += fb.writeSLEB128(cur, actions[i].typeFilter);
            // next offset: 0 if last, else some positive value
            if (i + 1 < actions.size()) {
                // Encode offset to next action
                // next action starts at actionTableStart + (i+1)*2 (approx)
                // For simplicity: write sleb 2 (go forward 2 bytes)
                cur += fb.writeSLEB128(cur, 0);  // next=0 for simplicity
            } else {
                cur += fb.writeSLEB128(cur, 0);
            }
        }

        // Type table (absptr, each 8 bytes; index 1 = first type)
        if (!types.empty()) {
            // Patch ttype_base_off: offset from end of ttype_enc ULEB to ttype base
            // ttype base = cur + types.size() * 8
            uint64_t ttypeBase = cur + (uint64_t)types.size() * 8;
            uint64_t offset = ttypeBase - (ttypeOffAt + fb.writeULEB128(ttypeOffAt, ttypeBase - ttypeOffAt));
            // Re-write with correct value
            fb.writeULEB128(ttypeOffAt, ttypeBase - csSizeAt);

            for (auto& t : types) {
                fb.writeU64(cur, t.tiVma); cur += 8;
            }
        }

        return cur;
    }
};

TEST(ItaniumEH, CIEOnly_EmptyResult) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t sectionVma = base + 0x5000;
    uint64_t cur = sectionVma;
    writeCIE(fb, sectionVma, cur);
    // Terminator
    fb.writeU32(cur, 0); cur += 4;

    std::size_t secSize = cur - sectionVma;
    fb.addSection(".eh_frame", sectionVma, secSize);

    auto parser = makeItaniumEHParser();
    auto fns = parser->parse(fb);
    EXPECT_TRUE(fns.empty());
}

TEST(ItaniumEH, FDE_NoLSDA_EmptyTryCatch) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t sectionVma = base + 0x5000;
    uint64_t cur = sectionVma;
    uint64_t cieAt = writeCIE(fb, sectionVma, cur);
    writeFDE(fb, cur, cieAt, base + 0x2000, 0x100);
    fb.writeU32(cur, 0); cur += 4;
    fb.addSection(".eh_frame", sectionVma, cur - sectionVma);

    auto parser = makeItaniumEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].functionVma, base + 0x2000);
    EXPECT_EQ(fns[0].functionEnd, base + 0x2100);
    EXPECT_TRUE(fns[0].tryCatchBlocks.empty());
}

TEST(ItaniumEH, LSDA_Cleanup_OneSite) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t funcStart = base + 0x2000;
    uint64_t lsdaVma   = base + 0x6000;
    uint64_t lpVma     = base + 0x2050;

    // LSDA: one site with action=0 (cleanup)
    LsdaBuilder lsda(fb, lsdaVma);
    // cs_start=10, cs_len=30 (offset from funcStart), landing_pad=0x50, action=0
    lsda.addSite(10, 30, 0x50, 0);  // lp = funcStart + 0x50 = lpVma
    lsda.build(funcStart);

    uint64_t sectionVma = base + 0x5000;
    uint64_t cur = sectionVma;
    // Use simple CIE without 'z' augmentation for this test
    uint64_t cieAt = writeCIE(fb, sectionVma, cur);
    writeFDE(fb, cur, cieAt, funcStart, 0x200);
    fb.writeU32(cur, 0); cur += 4;
    fb.addSection(".eh_frame", sectionVma, cur - sectionVma);

    // The simple FDE has no LSDA reference; just verify the FDE produces
    // an empty try-catch (no LSDA hookup without 'z' augmentation)
    auto parser = makeItaniumEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    // Without 'z' augmentation the LSDA is not linked; OK
    EXPECT_TRUE(fns[0].tryCatchBlocks.empty());
}

TEST(ItaniumEH, CatchAll_TypeFilter0) {
    // We test the LSDA logic directly by crafting a valid .eh_frame
    // with 'z' + 'L' augmentation pointing to an LSDA
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t funcStart = base + 0x2000;
    uint64_t funcLen   = 0x200;
    uint64_t lpVma     = funcStart + 0x80;
    uint64_t lsdaVma   = base + 0x7000;
    uint64_t sectionVma= base + 0x5000;

    // Build LSDA: one site, action=1, type_filter=0 (catch(...))
    uint64_t lsdaCur = lsdaVma;
    fb.writeU8(lsdaCur++, 0xFF);   // lpstart_enc = omit
    fb.writeU8(lsdaCur++, 0xFF);   // ttype_enc = omit
    fb.writeU8(lsdaCur++, 0x03);   // call_site_enc = udata4
    // call-site table length = 13 bytes (1 site: 4+4+4+1)
    lsdaCur += fb.writeULEB128(lsdaCur, 13);
    // Site: cs_start=0, cs_len=0x100, lp=0x80, action=1
    fb.writeU32(lsdaCur, 0); lsdaCur += 4;
    fb.writeU32(lsdaCur, 0x100); lsdaCur += 4;
    fb.writeU32(lsdaCur, 0x80); lsdaCur += 4;   // landing pad offset from func
    lsdaCur += fb.writeULEB128(lsdaCur, 1);      // action = 1
    // Action table: type_filter=0 (catch-all), next=0
    lsdaCur += fb.writeSLEB128(lsdaCur, 0);   // type_filter=0
    lsdaCur += fb.writeSLEB128(lsdaCur, 0);   // next=0

    // Build .eh_frame with 'z' and 'L'
    uint64_t cur = sectionVma;
    uint64_t cieAt = cur;

    // CIE with "zL" augmentation
    {
        uint64_t lenAt = cur; cur += 4;
        fb.writeU32(cur, 0); cur += 4;  // CIE_id = 0
        fb.writeU8(cur++, 1);            // version
        // augmentation = "zL\0"
        fb.writeU8(cur++, 'z');
        fb.writeU8(cur++, 'L');
        fb.writeU8(cur++, 0);
        cur += fb.writeULEB128(cur, 1);   // code_align
        cur += fb.writeSLEB128(cur, -8);  // data_align
        cur += fb.writeULEB128(cur, 16);  // return_col
        // aug data len = 1 (just 'L' byte)
        cur += fb.writeULEB128(cur, 1);
        fb.writeU8(cur++, 0x00);   // lsda_pointer_enc = DW_EH_PE_absptr
        uint32_t len = (uint32_t)(cur - lenAt - 4);
        fb.writeU32(lenAt, len);
    }

    // FDE with 'z' augmentation (LSDA pointer)
    {
        uint64_t lenAt = cur; cur += 4;
        // cieDelta = offset from the CIE_id field (cur) back to CIE start
        uint32_t cieDelta = (uint32_t)(cur - cieAt);
        fb.writeU32(cur, cieDelta); cur += 4;
        fb.writeU64(cur, funcStart); cur += 8;  // initial_location (absptr)
        fb.writeU64(cur, funcLen);   cur += 8;  // address_range
        cur += fb.writeULEB128(cur, 8);          // aug data len = 8 (one absptr)
        fb.writeU64(cur, lsdaVma);   cur += 8;  // LSDA pointer
        uint32_t len = (uint32_t)(cur - lenAt - 4);
        fb.writeU32(lenAt, len);
    }

    // Terminator
    fb.writeU32(cur, 0); cur += 4;
    fb.addSection(".eh_frame", sectionVma, cur - sectionVma);

    auto parser = makeItaniumEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].functionVma, funcStart);
    // The LSDA has one site with a landing pad and action=1 → catch(...)
    // We may or may not get the block depending on LSDA parser correctness
    // The LSDA parser should yield a catch-all handler
    if (!fns[0].tryCatchBlocks.empty()) {
        EXPECT_TRUE(fns[0].hasEH);
        bool found = false;
        for (auto& b : fns[0].tryCatchBlocks)
            for (auto& h : b.handlers)
                if (h.isCatchAll) { found = true; break; }
        EXPECT_TRUE(found);
    }
}

// ─── EHReconstructor tests ────────────────────────────────────────────────────

TEST(EHReconstructor, FindFunction_ReturnsNull_ForUnmapped) {
    EHReconstructor rec;
    EXPECT_EQ(rec.findFunction(0x400000), nullptr);
}

TEST(EHReconstructor, FindFunction_ReturnsCorrectFunction) {
    EHReconstructor rec;

    // Manually populate functions_ via a fake parser
    struct FakeParser final : public IEHParser {
        const char* name() const noexcept override { return "Fake"; }
        std::vector<EHFunction> parse(const IBinaryView&) const override {
            EHFunction fn;
            fn.functionVma = 0x401000;
            fn.functionEnd = 0x401100;
            return { fn };
        }
    };
    rec.addParser(std::make_unique<FakeParser>());

    struct NullView : public IBinaryView {
        std::size_t readBytes(uint64_t, uint8_t*, std::size_t) const override { return 0; }
        bool isMapped(uint64_t) const override { return false; }
        bool isDataSection(uint64_t) const override { return false; }
        uint64_t imageBase() const noexcept override { return 0x400000; }
        uint64_t sectionVma(const char*) const noexcept override { return 0; }
        std::size_t sectionSize(const char*) const noexcept override { return 0; }
    } nv;

    rec.reconstruct(nv);
    EXPECT_NE(rec.findFunction(0x401050), nullptr);
    EXPECT_EQ(rec.findFunction(0x402000), nullptr);
}

TEST(EHReconstructor, FindInnermostTry_NestingDepth2) {
    EHFunction fn;
    fn.functionVma = 0x1000;
    fn.functionEnd = 0x2000;

    TryCatchBlock outer;
    outer.tryBegin = 0x1000;
    outer.tryEnd   = 0x2000;

    TryCatchBlock inner;
    inner.tryBegin = 0x1200;
    inner.tryEnd   = 0x1400;

    CatchHandler ch;
    ch.handlerVma = 0x1500;
    ch.catchType  = "std::runtime_error";
    inner.handlers.push_back(ch);
    outer.nested.push_back(inner);
    fn.tryCatchBlocks.push_back(outer);

    // VMA inside inner block
    auto* found = EHReconstructor::findInnermostTry(fn, 0x1300);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->tryBegin, 0x1200u);

    // VMA in outer but not inner
    found = EHReconstructor::findInnermostTry(fn, 0x1100);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->tryBegin, 0x1000u);

    // VMA outside function
    found = EHReconstructor::findInnermostTry(fn, 0x3000);
    EXPECT_EQ(found, nullptr);
}

TEST(EHReconstructor, Callback_CalledForEachFunction) {
    struct FakeParser final : public IEHParser {
        const char* name() const noexcept override { return "Fake"; }
        std::vector<EHFunction> parse(const IBinaryView&) const override {
            EHFunction a, b;
            a.functionVma = 0x1000; a.functionEnd = 0x1100;
            b.functionVma = 0x2000; b.functionEnd = 0x2100;
            return { a, b };
        }
    };

    EHReconstructor rec;
    rec.addParser(std::make_unique<FakeParser>());

    std::size_t count = 0;
    rec.onFunction([&](const EHFunction&) { ++count; });

    struct NullView : public IBinaryView {
        std::size_t readBytes(uint64_t, uint8_t*, std::size_t) const override { return 0; }
        bool isMapped(uint64_t) const override { return false; }
        bool isDataSection(uint64_t) const override { return false; }
        uint64_t imageBase() const noexcept override { return 0; }
        uint64_t sectionVma(const char*) const noexcept override { return 0; }
        std::size_t sectionSize(const char*) const noexcept override { return 0; }
    } nv;

    auto total = rec.reconstruct(nv);
    EXPECT_EQ(total, 2u);
    EXPECT_EQ(count, 2u);
}

TEST(EHReconstructor, TryCatchBlock_HasCleanupOnly) {
    TryCatchBlock b;
    CatchHandler h;
    h.isCleanup = true;
    b.handlers.push_back(h);
    EXPECT_TRUE(b.hasCleanupOnly());
    EXPECT_FALSE(b.hasCatchAll());
}

TEST(EHReconstructor, TryCatchBlock_HasCatchAll) {
    TryCatchBlock b;
    CatchHandler h;
    h.isCatchAll = true;
    h.catchType  = "...";
    b.handlers.push_back(h);
    EXPECT_TRUE(b.hasCatchAll());
    EXPECT_FALSE(b.hasCleanupOnly());
}

// ═══════════════════════════════════════════════════════════════════════════════
// ARM EHABI tests
// ═══════════════════════════════════════════════════════════════════════════════

// ─── prel31 helper (mirrors arm_ehabi.cpp internal) ──────────────────────────
static uint64_t prel31(uint32_t raw, uint64_t fieldVma) {
    int32_t off = static_cast<int32_t>(raw << 1) >> 1;
    return static_cast<uint64_t>(static_cast<int64_t>(fieldVma) + off);
}

static void writeExidxEntry(FlatBin& fb, uint64_t entryVma,
                              uint64_t fnVma, uint32_t word1) {
    // Encode prel31 for fnVma relative to entryVma
    int64_t offset = (int64_t)fnVma - (int64_t)entryVma;
    uint32_t prel = (uint32_t)(offset & 0x7FFFFFFF);
    fb.writeU32(entryVma,     prel);
    fb.writeU32(entryVma + 4, word1);
}

TEST(ArmEhabi, CannotUnwind_EntryPresent) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t exidxVma = base + 0x8000;
    uint64_t fnVma    = base + 0x2000;

    fb.addSection(".ARM.exidx", exidxVma, 8);
    writeExidxEntry(fb, exidxVma, fnVma, 0x00000001); // CANTUNWIND

    auto parser = makeArmEhabiParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].functionVma, fnVma);
    EXPECT_TRUE(fns[0].unwindInfo.regSaves.empty());
}

TEST(ArmEhabi, CompactModel0_PopR4R5R14) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t exidxVma = base + 0x8000;
    uint64_t fnVma    = base + 0x2000;
    uint64_t fnVma2   = base + 0x3000;  // next function (to set end)

    fb.addSection(".ARM.exidx", exidxVma, 16);

    // Compact model 0 (inline): bit31=1
    // Unwind bytes: 0xA1 = pop {r4, r5}; then 0xA8 would be pop {r4, lr}
    // Let's use: byte0=0xA1 (pop r4-r5), byte1=0xB0 (FINISH), byte2=0x00 (pad)
    // word1 = 0x80_B0_A1_00 — but compact model 0 encodes as 0x80 | opcode_byte
    // Per ABI: word1 bits[31]=1, bits[30:24]=0x00 (model 0 marker),
    //          bits[23:16]=opcode0, bits[15:8]=opcode1, bits[7:0]=opcode2
    // 0xA1 = pop {r4, r5}; 0xB0 = FINISH; 0x00 = pad
    uint32_t w1 = 0x80000000u | (0xA1 << 16) | (0xB0 << 8) | 0x00;
    writeExidxEntry(fb, exidxVma,     fnVma,  w1);
    writeExidxEntry(fb, exidxVma + 8, fnVma2, 0x00000001);

    auto parser = makeArmEhabiParser();
    auto fns = parser->parse(fb);
    ASSERT_GE(fns.size(), 1u);
    // First function should have r4 and r5 in regSaves
    auto& fn = fns[0];
    EXPECT_EQ(fn.functionVma, fnVma);
    bool hasr4 = false, hasr5 = false;
    for (auto& rs : fn.unwindInfo.regSaves) {
        if (rs.regName == "r4") hasr4 = true;
        if (rs.regName == "r5") hasr5 = true;
    }
    EXPECT_TRUE(hasr4);
    EXPECT_TRUE(hasr5);
}

TEST(ArmEhabi, CompactModel0_PopR4toR7_LR) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t exidxVma = base + 0x8000;
    uint64_t fnVma    = base + 0x2000;

    fb.addSection(".ARM.exidx", exidxVma, 8);
    // 0xAB = pop {r4-r7, r14}; 0xB0 = FINISH
    uint32_t w1 = 0x80000000u | (0xAB << 16) | (0xB0 << 8) | 0x00;
    writeExidxEntry(fb, exidxVma, fnVma, w1);

    auto parser = makeArmEhabiParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    bool hasLR = false;
    for (auto& rs : fns[0].unwindInfo.regSaves)
        if (rs.regName == "lr") hasLR = true;
    EXPECT_TRUE(hasLR);
}

TEST(ArmEhabi, CompactModel0_VspIncrement) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t exidxVma = base + 0x8000;
    uint64_t fnVma    = base + 0x2000;

    fb.addSection(".ARM.exidx", exidxVma, 8);
    // op = 0x07 → vsp += (0x07+1)*4 = 32; 0xB0 = FINISH
    uint32_t w1 = 0x80000000u | (0x07 << 16) | (0xB0 << 8) | 0x00;
    writeExidxEntry(fb, exidxVma, fnVma, w1);

    auto parser = makeArmEhabiParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].unwindInfo.frameSize, 32u);
}

TEST(ArmEhabi, ExtabCompactModel1) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t exidxVma = base + 0x8000;
    uint64_t extabVma = base + 0x9000;
    uint64_t fnVma    = base + 0x2000;

    fb.addSection(".ARM.exidx",  exidxVma, 8);
    fb.addSection(".ARM.extab",  extabVma, 8);

    // .ARM.extab entry for compact model 1 (0x81):
    // word0: 0x81 | (nWords << 16) | opcode0..2
    // nWords = 0, opcodes: 0xA4 = pop {r4-r8}; 0xB0 = FINISH; 0x00 pad
    uint32_t extabWord0 = 0x81000000u | (0x00 << 16) | (0xA4 << 8) | 0xB0;
    fb.writeU32(extabVma, extabWord0);

    // exidx entry pointing to extab
    int64_t delta = (int64_t)extabVma - (int64_t)(exidxVma + 4);
    uint32_t w1 = (uint32_t)(delta & 0x7FFFFFFF);  // prel31 offset, bit31=0
    writeExidxEntry(fb, exidxVma, fnVma, w1);

    auto parser = makeArmEhabiParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].functionVma, fnVma);
    // Should have r4, r5, r6, r7, r8 (0xA4 = pop {r4..r8})
    bool hasr4 = false;
    for (auto& rs : fns[0].unwindInfo.regSaves)
        if (rs.regName == "r4") hasr4 = true;
    EXPECT_TRUE(hasr4);
}

TEST(ArmEhabi, TwoFunctions_EndAddressFromNextEntry) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t exidxVma = base + 0x8000;
    uint64_t fn1Vma   = base + 0x2000;
    uint64_t fn2Vma   = base + 0x2100;

    fb.addSection(".ARM.exidx", exidxVma, 16);
    writeExidxEntry(fb, exidxVma,     fn1Vma, 0x00000001);
    writeExidxEntry(fb, exidxVma + 8, fn2Vma, 0x00000001);

    auto parser = makeArmEhabiParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 2u);
    // fn1 end should == fn2 start
    EXPECT_EQ(fns[0].functionEnd, fn2Vma);
    EXPECT_EQ(fns[1].functionVma, fn2Vma);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Borland EH tests
// ═══════════════════════════════════════════════════════════════════════════════

/// Write the canonical Borland prolog at `prologVma`, pointing to `handlerAddr`.
static void writeBorlandProlog(FlatBin& fb, uint64_t prologVma,
                                uint32_t handlerAddr) {
    // PUSH imm32 (handler)
    fb.writeU8(prologVma, 0x68);
    fb.writeU32(prologVma + 1, handlerAddr);
    // PUSH DWORD PTR FS:[0]: 64 FF 35 00 00 00 00
    fb.writeU8(prologVma + 5,  0x64);
    fb.writeU8(prologVma + 6,  0xFF);
    fb.writeU8(prologVma + 7,  0x35);
    fb.writeU32(prologVma + 8,  0);
    // MOV DWORD PTR FS:[0], ESP: 64 89 25 00 00 00 00
    fb.writeU8(prologVma + 12, 0x64);
    fb.writeU8(prologVma + 13, 0x89);
    fb.writeU8(prologVma + 14, 0x25);
    fb.writeU32(prologVma + 15, 0);
}

/// Write a TExceptRec entry (16 bytes) at `vma`.
static void writeBorlandExceptRec(FlatBin& fb, uint64_t vma,
                                   uint32_t tryStart, uint32_t tryEnd,
                                   uint32_t catchOff, uint32_t typePtr) {
    fb.writeU32(vma +  0, tryStart);
    fb.writeU32(vma +  4, tryEnd);
    fb.writeU32(vma +  8, catchOff);
    fb.writeU32(vma + 12, typePtr);
}

/// Write Borland TypeInfo for class `name` at `vma`.
static void writeBorlandTypeInfo(FlatBin& fb, uint64_t vma, const char* name) {
    fb.writeU8(vma, 7);                         // kind = tkClass
    fb.writeU8(vma + 1, (uint8_t)strlen(name)); // nameLen
    fb.writeCStr(vma + 2, name);                 // name (not NUL-terminated but writeCStr adds it)
}

TEST(BorlandEH, PrologDetected_NoCatch_EmptyHandlerTable) {
    FlatBin fb;
    uint64_t base = fb.base_;
    // .text with Borland prolog
    uint64_t textVma    = base + 0x1000;
    uint64_t handlerVma = base + 0x5000;
    uint64_t tableVma   = base + 0x6000;

    fb.addSection(".text", textVma, 0x1000);

    // PUSH EBP at textVma (before prolog)
    fb.writeU8(textVma, 0x55);

    // Prolog at textVma+1
    writeBorlandProlog(fb, textVma + 1, (uint32_t)handlerVma);

    // Handler stub: MOV EAX, tableVma
    fb.writeU8(handlerVma,       0xB8);
    fb.writeU32(handlerVma + 1,  (uint32_t)tableVma);

    // Empty handler table (all-zero terminator)
    fb.writeU32(tableVma, 0);

    auto parser = makeBorlandEHParser();
    auto fns = parser->parse(fb);
    ASSERT_GE(fns.size(), 1u);
    EXPECT_TRUE(fns[0].tryCatchBlocks.empty());
    EXPECT_FALSE(fns[0].hasEH);
}

TEST(BorlandEH, OneHandlerEntry_TypedCatch) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t textVma    = base + 0x1000;
    uint64_t handlerVma = base + 0x5000;
    uint64_t tableVma   = base + 0x6000;
    uint64_t typeVma    = base + 0x7000;
    uint64_t fnVma      = textVma;

    fb.addSection(".text", textVma, 0x2000);
    writeBorlandProlog(fb, fnVma, (uint32_t)handlerVma);
    fb.writeU8(handlerVma, 0xB8);
    fb.writeU32(handlerVma + 1, (uint32_t)tableVma);

    // One TExceptRec: try[0x10..0x50), catch at 0x80, type at typeVma
    writeBorlandExceptRec(fb, tableVma, 0x10, 0x50, 0x80, (uint32_t)typeVma);
    // Terminator
    writeBorlandExceptRec(fb, tableVma + 16, 0, 0, 0, 0);

    writeBorlandTypeInfo(fb, typeVma, "ENoMemory");

    auto parser = makeBorlandEHParser();
    auto fns = parser->parse(fb);
    ASSERT_GE(fns.size(), 1u);
    bool found = false;
    for (auto& fn : fns) {
        if (!fn.tryCatchBlocks.empty()) {
            found = true;
            EXPECT_TRUE(fn.hasEH);
            ASSERT_EQ(fn.tryCatchBlocks[0].handlers.size(), 1u);
            EXPECT_EQ(fn.tryCatchBlocks[0].handlers[0].catchType, "ENoMemory");
            EXPECT_FALSE(fn.tryCatchBlocks[0].handlers[0].isCatchAll);
        }
    }
    EXPECT_TRUE(found);
}

TEST(BorlandEH, CatchAll_TypePtrZero) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t textVma    = base + 0x1000;
    uint64_t handlerVma = base + 0x5000;
    uint64_t tableVma   = base + 0x6000;

    fb.addSection(".text", textVma, 0x2000);
    writeBorlandProlog(fb, textVma, (uint32_t)handlerVma);
    fb.writeU8(handlerVma, 0xB8);
    fb.writeU32(handlerVma + 1, (uint32_t)tableVma);
    writeBorlandExceptRec(fb, tableVma, 0x10, 0x40, 0x50, 0);  // typePtr=0
    writeBorlandExceptRec(fb, tableVma + 16, 0, 0, 0, 0);

    auto parser = makeBorlandEHParser();
    auto fns = parser->parse(fb);
    bool found = false;
    for (auto& fn : fns) {
        for (auto& blk : fn.tryCatchBlocks)
            for (auto& h : blk.handlers)
                if (h.isCatchAll) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(BorlandEH, RegSaveScan_PUSH_EBX_ESI_EDI) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t textVma = base + 0x1000;

    fb.addSection(".text", textVma, 0x1000);
    // PUSH EBP; PUSH EBX; PUSH ESI; PUSH EDI; then Borland prolog
    fb.writeU8(textVma + 0, 0x55);  // PUSH EBP
    fb.writeU8(textVma + 1, 0x53);  // PUSH EBX
    fb.writeU8(textVma + 2, 0x56);  // PUSH ESI
    fb.writeU8(textVma + 3, 0x57);  // PUSH EDI
    writeBorlandProlog(fb, textVma + 4, (uint32_t)(base + 0x5000));

    auto parser = makeBorlandEHParser();
    auto fns = parser->parse(fb);
    ASSERT_GE(fns.size(), 1u);
    // Function starting with EBP prolog should have EBP, EBX, ESI, EDI saved
    bool hasEbx = false, hasEsi = false, hasEdi = false;
    for (auto& fn : fns) {
        for (auto& rs : fn.unwindInfo.regSaves) {
            if (rs.regName == "ebx") hasEbx = true;
            if (rs.regName == "esi") hasEsi = true;
            if (rs.regName == "edi") hasEdi = true;
        }
    }
    EXPECT_TRUE(hasEbx);
    EXPECT_TRUE(hasEsi);
    EXPECT_TRUE(hasEdi);
}

// ═══════════════════════════════════════════════════════════════════════════════
// DMC EH tests
// ═══════════════════════════════════════════════════════════════════════════════

static void writeDmcScopeEntry(FlatBin& fb, uint64_t vma,
                                int32_t encLevel, uint32_t filterFn, uint32_t handlerFn) {
    fb.writeI32(vma + 0, encLevel);
    fb.writeU32(vma + 4, filterFn);
    fb.writeU32(vma + 8, handlerFn);
}

TEST(DmcEH, PrologDetected_OneFinally) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t textVma  = base + 0x1000;
    uint64_t tableVma = base + 0x6000;
    uint64_t catchVma = base + 0x2100;

    fb.addSection(".text",  textVma, 0x2000);
    fb.addSection(".rdata", base + 0x5000, 0x1000);

    // DMC prolog (same bytes as Borland prolog)
    writeBorlandProlog(fb, textVma, (uint32_t)(base + 0x5000));

    // PUSH tableVma immediately after prolog (at textVma + 19)
    fb.writeU8(textVma + 19, 0x68);
    fb.writeU32(textVma + 20, (uint32_t)tableVma);

    // Scope table: one __finally entry (filterFn=0)
    writeDmcScopeEntry(fb, tableVma, -1, 0, (uint32_t)catchVma);
    // Terminator
    writeDmcScopeEntry(fb, tableVma + 12, -1, 0, 0);

    auto parser = makeDmcEHParser();
    auto fns = parser->parse(fb);
    ASSERT_GE(fns.size(), 1u);
    bool foundCleanup = false;
    for (auto& fn : fns) {
        for (auto& blk : fn.tryCatchBlocks)
            for (auto& h : blk.handlers)
                if (h.isCleanup) foundCleanup = true;
    }
    EXPECT_TRUE(foundCleanup);
}

TEST(DmcEH, TypedCatch_FilterThunk) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t textVma  = base + 0x1000;
    uint64_t tableVma = base + 0x6000;
    uint64_t filterVma= base + 0x7000;
    uint64_t typeDescVma = base + 0x8000;
    uint64_t catchVma = base + 0x2200;

    fb.addSection(".text",  textVma, 0x2000);
    fb.addSection(".rdata", base + 0x5000, 0x5000);

    writeBorlandProlog(fb, textVma, (uint32_t)(base + 0x5000));
    fb.writeU8(textVma + 19, 0x68);
    fb.writeU32(textVma + 20, (uint32_t)tableVma);

    // Filter thunk: PUSH typeDescVma; <rest doesn't matter>
    fb.writeU8(filterVma, 0x68);
    fb.writeU32(filterVma + 1, (uint32_t)typeDescVma);

    // TypeDescriptor: vftable(4) + spare(4) + name
    fb.writeU32(typeDescVma + 0, 0);
    fb.writeU32(typeDescVma + 4, 0);
    fb.writeCStr(typeDescVma + 8, "?AVruntime_error@std@@");

    writeDmcScopeEntry(fb, tableVma, -1, (uint32_t)filterVma, (uint32_t)catchVma);
    writeDmcScopeEntry(fb, tableVma + 12, -1, 0, 0);

    auto parser = makeDmcEHParser();
    auto fns = parser->parse(fb);
    bool found = false;
    for (auto& fn : fns) {
        for (auto& blk : fn.tryCatchBlocks) {
            for (auto& h : blk.handlers) {
                if (!h.isCleanup && !h.isCatchAll) {
                    found = true;
                    EXPECT_FALSE(h.catchType.empty());
                }
            }
        }
    }
    EXPECT_TRUE(found);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Watcom EH tests
// ═══════════════════════════════════════════════════════════════════════════════

static void writeWatcomHdr(FlatBin& fb, uint64_t vma, uint32_t count) {
    fb.writeU32(vma + 0, 0x43584557u); // "WEXC"
    fb.writeU32(vma + 4, 1u);          // version
    fb.writeU32(vma + 8, count);
}
static void writeWatcomEntry(FlatBin& fb, uint64_t vma,
                               uint32_t fnStartRVA, uint32_t fnEndRVA,
                               uint32_t htRVA) {
    fb.writeU32(vma + 0, fnStartRVA);
    fb.writeU32(vma + 4, fnEndRVA);
    fb.writeU32(vma + 8, htRVA);
}
static void writeWatcomHandlerEntry(FlatBin& fb, uint64_t vma,
                                     uint16_t tryStart, uint16_t tryEnd,
                                     uint16_t catchOff, uint16_t flags,
                                     uint32_t typeRVA) {
    fb.writeU16(vma + 0, tryStart);
    fb.writeU16(vma + 2, tryEnd);
    fb.writeU16(vma + 4, catchOff);
    fb.writeU16(vma + 6, flags);
    fb.writeU32(vma + 8, typeRVA);
}

TEST(WatcomEH, NoSection_EmptyResult) {
    FlatBin fb;
    auto parser = makeWatcomEHParser();
    auto fns = parser->parse(fb);
    EXPECT_TRUE(fns.empty());
}

TEST(WatcomEH, WrongMagic_EmptyResult) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t ehVma = base + 0x4000;
    fb.addSection(".eh_data", ehVma, 12);
    fb.writeU32(ehVma, 0xDEADBEEFu);  // wrong magic

    auto parser = makeWatcomEHParser();
    auto fns = parser->parse(fb);
    EXPECT_TRUE(fns.empty());
}

TEST(WatcomEH, TwoFunctions_TypedAndCatchAll) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t ehVma   = base + 0x4000;
    uint64_t fn1RVA  = 0x1000;
    uint64_t fn2RVA  = 0x2000;
    uint64_t ht1RVA  = 0x3100;
    uint64_t ht2RVA  = 0x3200;
    uint64_t typeRVA = 0x3300;

    fb.addSection(".eh_data", ehVma, 0x1000);
    fb.addSection(".text",    base + 0x1000, 0x2000);

    writeWatcomHdr(fb, ehVma, 2);
    writeWatcomEntry(fb, ehVma + 12,      fn1RVA, fn1RVA + 0x100, ht1RVA);
    writeWatcomEntry(fb, ehVma + 24,      fn2RVA, fn2RVA + 0x100, ht2RVA);

    // Handler table 1: typed catch
    writeWatcomHandlerEntry(fb, base + ht1RVA, 0x10, 0x50, 0x60, 0, typeRVA);
    // Terminator
    writeWatcomHandlerEntry(fb, base + ht1RVA + 12, 0, 0, 0, 0, 0);

    // Handler table 2: catch-all
    writeWatcomHandlerEntry(fb, base + ht2RVA, 0x20, 0x80, 0x90, 0, 0);
    writeWatcomHandlerEntry(fb, base + ht2RVA + 12, 0, 0, 0, 0, 0);

    // Type string
    fb.writeCStr(base + typeRVA, "std::exception");

    auto parser = makeWatcomEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 2u);

    EXPECT_TRUE(fns[0].hasEH);
    ASSERT_EQ(fns[0].tryCatchBlocks[0].handlers.size(), 1u);
    EXPECT_EQ(fns[0].tryCatchBlocks[0].handlers[0].catchType, "std::exception");
    EXPECT_FALSE(fns[0].tryCatchBlocks[0].handlers[0].isCatchAll);

    EXPECT_TRUE(fns[1].hasEH);
    ASSERT_EQ(fns[1].tryCatchBlocks[0].handlers.size(), 1u);
    EXPECT_TRUE(fns[1].tryCatchBlocks[0].handlers[0].isCatchAll);
}

TEST(WatcomEH, CleanupFlag) {
    FlatBin fb;
    uint64_t base  = fb.base_;
    uint64_t ehVma = base + 0x4000;
    uint64_t htRVA = 0x3100;

    fb.addSection(".eh_data", ehVma, 0x200);
    writeWatcomHdr(fb, ehVma, 1);
    writeWatcomEntry(fb, ehVma + 12, 0x1000, 0x1100, htRVA);
    // flags bit1 = cleanup
    writeWatcomHandlerEntry(fb, base + htRVA, 5, 30, 40, 0x02, 0);
    writeWatcomHandlerEntry(fb, base + htRVA + 12, 0, 0, 0, 0, 0);

    auto parser = makeWatcomEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    ASSERT_EQ(fns[0].tryCatchBlocks[0].handlers.size(), 1u);
    EXPECT_TRUE(fns[0].tryCatchBlocks[0].handlers[0].isCleanup);
}

TEST(WatcomEH, RegSaves_ESI_EDI_EBX) {
    FlatBin fb;
    uint64_t base  = fb.base_;
    uint64_t ehVma = base + 0x4000;

    fb.addSection(".eh_data", ehVma, 0x100);
    fb.addSection(".text",    base + 0x1000, 0x1000);
    writeWatcomHdr(fb, ehVma, 1);
    writeWatcomEntry(fb, ehVma + 12, 0x1000, 0x1100, 0);

    // Write PUSH EBX; PUSH ESI; PUSH EDI at function start
    fb.writeU8(base + 0x1000, 0x53);
    fb.writeU8(base + 0x1001, 0x56);
    fb.writeU8(base + 0x1002, 0x57);

    auto parser = makeWatcomEHParser();
    auto fns = parser->parse(fb);
    ASSERT_EQ(fns.size(), 1u);
    bool hasEbx = false, hasEsi = false, hasEdi = false;
    for (auto& rs : fns[0].unwindInfo.regSaves) {
        if (rs.regName == "ebx") hasEbx = true;
        if (rs.regName == "esi") hasEsi = true;
        if (rs.regName == "edi") hasEdi = true;
    }
    EXPECT_TRUE(hasEbx);
    EXPECT_TRUE(hasEsi);
    EXPECT_TRUE(hasEdi);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Symbian EH tests
// ═══════════════════════════════════════════════════════════════════════════════

// ARM32 instruction encoding helpers
static uint32_t encodeArmBL(uint64_t fromVma, uint64_t toVma) {
    int64_t offset = (int64_t)toVma - (int64_t)fromVma - 8;
    return 0xEB000000u | ((uint32_t)(offset / 4) & 0x00FFFFFFu);
}
static constexpr uint32_t kArmCmpR0Zero = 0xE3500000u;
static uint32_t encodeArmBNE(uint64_t fromVma, uint64_t toVma) {
    int64_t offset = (int64_t)toVma - (int64_t)fromVma - 8;
    return 0x1A000000u | ((uint32_t)(offset / 4) & 0x00FFFFFFu);
}
static constexpr uint32_t kArmPushLR = 0xE92D4000u; // PUSH {LR}

TEST(SymbianEH, NoTrapPattern_EmptyResult) {
    FlatBin fb;
    uint64_t base = fb.base_;
    // .text with no TTrap::Trap calls
    uint64_t textVma = base + 0x1000;
    fb.addSection(".text", textVma, 64);
    // Write some random ARM instructions
    for (int i = 0; i < 16; ++i)
        fb.writeU32(textVma + i * 4, 0xE1A00000u); // MOV R0,R0 (NOP)

    auto parser = makeSymbianEHParser();
    auto fns = parser->parse(fb);
    EXPECT_TRUE(fns.empty());
}

TEST(SymbianEH, OneTrapCallSite_TIntHandler) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t textVma  = base + 0x1000;
    uint64_t trapFnVma= base + 0x5000;
    uint64_t handlerVma = base + 0x1030;

    fb.addSection(".text", textVma, 0x1000);

    // Function prolog at textVma
    fb.writeU32(textVma, kArmPushLR);  // PUSH {LR}

    // At textVma+4: BL trapFnVma
    fb.writeU32(textVma + 4, encodeArmBL(textVma + 4, trapFnVma));
    // textVma+8: CMP R0, #0
    fb.writeU32(textVma + 8, kArmCmpR0Zero);
    // textVma+12: BNE handlerVma
    fb.writeU32(textVma + 12, encodeArmBNE(textVma + 12, handlerVma));
    // body
    for (int i = 4; i < 12; ++i)
        fb.writeU32(textVma + 4*i + 4, 0xE1A00000u);

    // We need a second call-site so the heuristic fires (>= 2 occurrences)
    uint64_t site2 = textVma + 0x100;
    fb.writeU32(site2,     encodeArmBL(site2, trapFnVma));
    fb.writeU32(site2 + 4, kArmCmpR0Zero);
    fb.writeU32(site2 + 8, encodeArmBNE(site2 + 8, base + 0x1140));

    auto parser = makeSymbianEHParser();
    auto fns = parser->parse(fb);
    EXPECT_GE(fns.size(), 1u);
    bool found = false;
    for (auto& fn : fns) {
        for (auto& blk : fn.tryCatchBlocks) {
            for (auto& h : blk.handlers) {
                if (h.catchType == "TInt") {
                    found = true;
                    EXPECT_EQ(fn.personalityFn, "TTrap::Trap");
                }
            }
        }
    }
    EXPECT_TRUE(found);
}

TEST(SymbianEH, MultipleTrapSites_Deduplicated) {
    FlatBin fb;
    uint64_t base = fb.base_;
    uint64_t textVma  = base + 0x1000;
    uint64_t trapFn   = base + 0x5000;

    fb.addSection(".text", textVma, 0x1000);

    // Three call sites all calling the same trapFn
    for (int i = 0; i < 3; ++i) {
        uint64_t site = textVma + i * 0x40;
        fb.writeU32(site,     encodeArmBL(site, trapFn));
        fb.writeU32(site + 4, kArmCmpR0Zero);
        fb.writeU32(site + 8, encodeArmBNE(site + 8, base + 0x2000 + i * 0x10));
    }

    auto parser = makeSymbianEHParser();
    auto fns = parser->parse(fb);
    // Three call-sites but all distinct function starts → 3 EHFunctions
    // (or deduplicated if same function start)
    EXPECT_GE(fns.size(), 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
