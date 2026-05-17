/**
 * @file tests/string_detect/string_detect_test.cpp
 * @brief Tests for reference-anchored string detection.
 *
 * Uses an in-memory IBinaryView backed by a flat byte buffer.
 */

#include "retdec/string_detect/string_detect.h"
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

using namespace retdec::string_detect;

// ─── In-memory binary view ────────────────────────────────────────────────────

class FlatView : public IBinaryView {
public:
    struct Section {
        uint64_t base;
        std::vector<uint8_t> data;
        bool isCode;
    };

    explicit FlatView(uint32_t ptrW = 8) : ptrW_(ptrW) {}

    void addSection(uint64_t base, const uint8_t* data, std::size_t len, bool code=false) {
        Section s;
        s.base   = base;
        s.data   = std::vector<uint8_t>(data, data+len);
        s.isCode = code;
        sections_.push_back(std::move(s));
    }

    void addSection(uint64_t base, const std::vector<uint8_t>& data, bool code=false) {
        addSection(base, data.data(), data.size(), code);
    }

    std::size_t readBytes(uint64_t vma, uint8_t* buf, std::size_t maxLen) const override {
        for (auto& s : sections_) {
            if (vma >= s.base && vma < s.base + s.data.size()) {
                std::size_t off = vma - s.base;
                std::size_t avail = s.data.size() - off;
                std::size_t n = std::min(avail, maxLen);
                std::memcpy(buf, s.data.data()+off, n);
                return n;
            }
        }
        return 0;
    }

    bool isDataSection(uint64_t vma) const override {
        for (auto& s : sections_) {
            if (vma >= s.base && vma < s.base+s.data.size()) return !s.isCode;
        }
        return false;
    }
    bool isCodeSection(uint64_t vma) const override {
        for (auto& s : sections_) {
            if (vma >= s.base && vma < s.base+s.data.size()) return s.isCode;
        }
        return false;
    }
    bool isMapped(uint64_t vma) const override {
        for (auto& s : sections_) {
            if (vma >= s.base && vma < s.base+s.data.size()) return true;
        }
        return false;
    }
    uint32_t pointerWidth() const noexcept override { return ptrW_; }

private:
    std::vector<Section> sections_;
    uint32_t ptrW_;
};

// Helper: build a flat byte buffer containing a C string
static std::vector<uint8_t> cstr(const char* s) {
    std::vector<uint8_t> v(s, s+std::strlen(s)+1);
    return v;
}
// Helper: write a 64-bit LE pointer into a buffer at offset
static void writePtr64(std::vector<uint8_t>& buf, std::size_t off, uint64_t v) {
    for (int i=0; i<8; ++i) buf[off+i] = (v>>(8*i))&0xFF;
}
static void writePtr32(std::vector<uint8_t>& buf, std::size_t off, uint32_t v) {
    for (int i=0; i<4; ++i) buf[off+i] = (v>>(8*i))&0xFF;
}

// ─── EncodingDetector tests ───────────────────────────────────────────────────

TEST(EncodingDetectorTest, PureASCII) {
    const char* s = "Hello, World!";
    auto r = detectEncoding(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
    EXPECT_EQ(r.kind, EncodingKind::ASCII);
    EXPECT_GE(r.confidence, 0.9f);
}

TEST(EncodingDetectorTest, EmptyBufferIsUnknown) {
    auto r = detectEncoding(nullptr, 0);
    EXPECT_EQ(r.kind, EncodingKind::Unknown);
}

TEST(EncodingDetectorTest, UTF16LEDetected) {
    // "Hi" in UTF-16LE: 48 00 69 00
    uint8_t buf[] = { 0x48,0x00, 0x69,0x00, 0x21,0x00, 0x00,0x00 };
    auto r = detectEncoding(buf, sizeof(buf));
    EXPECT_EQ(r.kind, EncodingKind::UTF16LE);
}

TEST(EncodingDetectorTest, UTF16BEDetected) {
    // "Hi" in UTF-16BE: 00 48 00 69
    uint8_t buf[] = { 0x00,0x48, 0x00,0x69, 0x00,0x21, 0x00,0x00 };
    auto r = detectEncoding(buf, sizeof(buf));
    EXPECT_EQ(r.kind, EncodingKind::UTF16BE);
}

TEST(EncodingDetectorTest, UTF8MultibyteDetected) {
    // "Héllo" in UTF-8: H 0xC3 0xA9 l l o
    uint8_t buf[] = { 'H', 0xC3,0xA9, 'l','l','o', 0 };
    auto r = detectEncoding(buf, sizeof(buf)-1);
    EXPECT_EQ(r.kind, EncodingKind::UTF8);
}

TEST(EncodingDetectorTest, Latin1WithHighBytes) {
    uint8_t buf[] = { 'H', 0xE9, 'l', 'l', 'o', 0 }; // é in Latin-1 = 0xE9
    auto r = detectEncoding(buf, sizeof(buf)-1);
    // 0xE9 alone is not valid UTF-8 lead + continuation, so → Latin-1
    EXPECT_EQ(r.kind, EncodingKind::Latin1);
}

// ─── typeString tests ─────────────────────────────────────────────────────────

TEST(TypeStringTest, SimpleASCII) {
    auto data = cstr("Hello, World!");
    FlatView view;
    view.addSection(0x1000, data);
    auto sl = typeString(view, 0x1000);
    ASSERT_TRUE(sl.has_value());
    EXPECT_EQ(sl->value, "Hello, World!");
    EXPECT_EQ(sl->kind, StringKind::CNulTerminated);
    EXPECT_EQ(sl->encoding, EncodingKind::ASCII);
    EXPECT_EQ(sl->charCount, 13u);
    EXPECT_EQ(sl->byteLength, 14u);
}

TEST(TypeStringTest, ShortStringsRejected) {
    uint8_t data[] = { 'A', 0 };
    FlatView view;
    view.addSection(0x1000, data, 2);
    // typeString returns value, but charCount=1 — caller filters by minStringLen
    // The type alone won't reject it; the engine config does.
    auto sl = typeString(view, 0x1000);
    // It's technically a valid 1-char C string
    if (sl) EXPECT_EQ(sl->charCount, 1u);
}

TEST(TypeStringTest, UTF16LEString) {
    // "Hi!" in UTF-16LE + NUL
    uint8_t data[] = { 'H',0, 'i',0, '!',0, 0,0 };
    FlatView view;
    view.addSection(0x2000, data, sizeof(data));
    auto sl = typeString(view, 0x2000);
    ASSERT_TRUE(sl.has_value());
    EXPECT_EQ(sl->kind, StringKind::Wide);
    EXPECT_EQ(sl->encoding, EncodingKind::UTF16LE);
    EXPECT_EQ(sl->value, "Hi!");
}

TEST(TypeStringTest, UTF16BEString) {
    uint8_t data[] = { 0,'H', 0,'i', 0,'!', 0,0 };
    FlatView view;
    view.addSection(0x2000, data, sizeof(data));
    auto sl = typeString(view, 0x2000);
    ASSERT_TRUE(sl.has_value());
    EXPECT_EQ(sl->encoding, EncodingKind::UTF16BE);
}

TEST(TypeStringTest, PascalString) {
    // u8 length=5, "Hello"
    uint8_t data[] = { 5, 'H','e','l','l','o', 0xFF };
    FlatView view;
    view.addSection(0x3000, data, sizeof(data));
    auto sl = typeString(view, 0x3000);
    ASSERT_TRUE(sl.has_value());
    EXPECT_EQ(sl->kind, StringKind::Pascal);
    EXPECT_EQ(sl->value, "Hello");
    EXPECT_EQ(sl->charCount, 5u);
}

TEST(TypeStringTest, LengthPrefixedMSVC) {
    // u32 length=5, "Hello", NUL
    uint8_t data[] = { 5,0,0,0, 'H','e','l','l','o', 0 };
    FlatView view;
    view.addSection(0x4000, data, sizeof(data));
    auto sl = typeString(view, 0x4000);
    ASSERT_TRUE(sl.has_value());
    EXPECT_EQ(sl->kind, StringKind::LengthPrefixed);
    EXPECT_EQ(sl->value, "Hello");
    EXPECT_EQ(sl->charCount, 5u);
}

TEST(TypeStringTest, UnmappedAddressReturnsNullopt) {
    FlatView view;
    auto sl = typeString(view, 0xDEADBEEF);
    EXPECT_FALSE(sl.has_value());
}

TEST(TypeStringTest, NullBytesAtStartReturnsNullopt) {
    uint8_t data[] = { 0,0,0,0 };
    FlatView view;
    view.addSection(0x1000, data, 4);
    auto sl = typeString(view, 0x1000);
    EXPECT_FALSE(sl.has_value());
}

TEST(TypeStringTest, ControlBytesReturnsNullopt) {
    uint8_t data[] = { 0x01, 0x02, 0x03, 0 };
    FlatView view;
    view.addSection(0x1000, data, 4);
    auto sl = typeString(view, 0x1000);
    EXPECT_FALSE(sl.has_value());
}

// ─── StringDetector::processRef tests ─────────────────────────────────────────

class DetectorTest : public ::testing::Test {
protected:
    FlatView view;
    StringDetectorConfig cfg;

    void SetUp() override {
        cfg.minStringLen = 2;
        cfg.detectTables = true;
    }

    StringDetector makeDetector() {
        return StringDetector(view, cfg);
    }
};

TEST_F(DetectorTest, FindsASCIIViaRef) {
    auto data = cstr("test_string_123");
    view.addSection(0x5000, data);

    auto det = makeDetector();
    std::vector<StringLiteral> found;
    det.onString([&](const StringLiteral& s){ found.push_back(s); });

    det.processRef({ 0x401000, 0x5000, false });
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].value, "test_string_123");
}

TEST_F(DetectorTest, DeduplicatesSameAddress) {
    auto data = cstr("once");
    view.addSection(0x5000, data);
    auto det = makeDetector();
    int count=0;
    det.onString([&](auto&){ ++count; });
    det.processRef({ 0x401000, 0x5000, false });
    det.processRef({ 0x401010, 0x5000, false }); // same target
    EXPECT_EQ(count, 1);
}

TEST_F(DetectorTest, IgnoresShortStringsWithConfig) {
    auto data = cstr("ab");
    view.addSection(0x5000, data);
    cfg.minStringLen = 4;
    auto det = makeDetector();
    int count=0;
    det.onString([&](auto&){ ++count; });
    det.processRef({ 0x401000, 0x5000, false });
    EXPECT_EQ(count, 0);
}

TEST_F(DetectorTest, BatchProcessRefs) {
    auto d1 = cstr("hello world");
    auto d2 = cstr("another string");
    view.addSection(0x6000, d1);
    view.addSection(0x7000, d2);

    std::vector<InstrRef> refs = {
        { 0x400100, 0x6000, false },
        { 0x400200, 0x7000, false },
    };
    auto det = makeDetector();
    std::size_t n = det.processRefs(refs);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(det.strings().size(), 2u);
}

TEST_F(DetectorTest, UnmappedRefIgnored) {
    auto det = makeDetector();
    det.processRef({ 0x401000, 0xDEADBEEF, false });
    EXPECT_TRUE(det.strings().empty());
}

TEST_F(DetectorTest, IsKnownStringAfterProcessing) {
    auto data = cstr("example");
    view.addSection(0x5000, data);
    auto det = makeDetector();
    det.processRef({ 0x400000, 0x5000, false });
    EXPECT_TRUE(det.isKnownString(0x5000));
    EXPECT_FALSE(det.isKnownString(0x9999));
}

// ─── String table tests ───────────────────────────────────────────────────────

TEST(StringTableTest, DetectsThreeEntryTable64) {
    // Layout: [ptr0][ptr1][ptr2] at 0x8000, strings at 0x9000, 0x9100, 0x9200
    std::vector<uint8_t> ptrBuf(24, 0);
    writePtr64(ptrBuf, 0,  0x9000);
    writePtr64(ptrBuf, 8,  0x9100);
    writePtr64(ptrBuf, 16, 0x9200);

    FlatView view;
    view.addSection(0x8000, ptrBuf);
    view.addSection(0x9000, cstr("first string"));
    view.addSection(0x9100, cstr("second string"));
    view.addSection(0x9200, cstr("third string"));

    auto tbl = detectStringTable(view, 0x8000);
    ASSERT_TRUE(tbl.has_value());
    EXPECT_EQ(tbl->count, 3u);
    EXPECT_EQ(tbl->targets[0], 0x9000u);
    EXPECT_EQ(tbl->targets[1], 0x9100u);
    EXPECT_EQ(tbl->targets[2], 0x9200u);
}

TEST(StringTableTest, StopsAtNonStringPointer) {
    // ptr0→string, ptr1→zero (null), ptr2→string: only 1 entry before null
    std::vector<uint8_t> ptrBuf(24, 0);
    writePtr64(ptrBuf, 0,  0x9000);
    writePtr64(ptrBuf, 8,  0);       // null pointer → stop
    writePtr64(ptrBuf, 16, 0x9200);

    FlatView view;
    view.addSection(0x8000, ptrBuf);
    view.addSection(0x9000, cstr("first string"));

    auto tbl = detectStringTable(view, 0x8000);
    EXPECT_FALSE(tbl.has_value()); // only 1 valid entry, need ≥2
}

TEST(StringTableTest, DetectsTableWith32BitPointers) {
    std::vector<uint8_t> ptrBuf(12, 0);
    writePtr32(ptrBuf, 0, 0x9000);
    writePtr32(ptrBuf, 4, 0x9100);
    writePtr32(ptrBuf, 8, 0x9200);

    FlatView view(4);
    view.addSection(0x8000, ptrBuf);
    view.addSection(0x9000, cstr("alpha"));
    view.addSection(0x9100, cstr("beta"));
    view.addSection(0x9200, cstr("gamma"));

    auto tbl = detectStringTable(view, 0x8000);
    ASSERT_TRUE(tbl.has_value());
    EXPECT_EQ(tbl->ptrWidth, 4u);
    EXPECT_EQ(tbl->count, 3u);
}

// ─── SSO branch detection tests ───────────────────────────────────────────────

TEST(SSOTest, LibStdCppThreshold15) {
    auto r = detectSSOBranch(15, 0x401000, 0x401010, 0x401050);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->threshold, 15u);
    EXPECT_EQ(r->impl, SSOImpl::LibStdCpp);
}

TEST(SSOTest, LibStdCppThreshold16JBForm) {
    auto r = detectSSOBranch(16, 0x401000, 0x401010, 0x401050);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->threshold, 15u);
}

TEST(SSOTest, LibCppThreshold22) {
    auto r = detectSSOBranch(22, 0x401000, 0x401010, 0x401050);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->threshold, 22u);
    EXPECT_EQ(r->impl, SSOImpl::LibCpp);
}

TEST(SSOTest, LibCppThreshold23JBForm) {
    auto r = detectSSOBranch(23, 0x401000, 0x401010, 0x401050);
    ASSERT_TRUE(r.has_value());
    // Either LibCpp or FollyFBString matches 23
    EXPECT_TRUE(r->impl==SSOImpl::LibCpp || r->impl==SSOImpl::FollyFBString);
}

TEST(SSOTest, UnknownThreshold99) {
    auto r = detectSSOBranch(99, 0x401000, 0x401010, 0x401050);
    EXPECT_FALSE(r.has_value());
}

TEST(SSOTest, SSOThresholdValues) {
    EXPECT_EQ(ssoThreshold(SSOImpl::LibStdCpp),     15u);
    EXPECT_EQ(ssoThreshold(SSOImpl::MsvcStl),       15u);
    EXPECT_EQ(ssoThreshold(SSOImpl::LibCpp),        22u);
    EXPECT_EQ(ssoThreshold(SSOImpl::FollyFBString), 23u);
}

TEST(SSOTest, ViaStringDetector) {
    FlatView view;
    StringDetectorConfig cfg;
    StringDetector det(view, cfg);
    std::vector<SSOBranchInfo> ssos;
    det.onSSO([&](auto& b){ ssos.push_back(b); });
    det.processCompareBranch(15, 0x401000, 0x401010, 0x401050);
    ASSERT_EQ(ssos.size(), 1u);
    EXPECT_EQ(ssos[0].threshold, 15u);
}

// ─── ARM literal pool tests ───────────────────────────────────────────────────

TEST(LiteralPoolTest, ExtractSingleEntry) {
    // LDR Rd,[PC,#8] at 0x1000 → literal at 0x100C (code section)
    uint8_t codeData[32] = {};
    // Write the literal value at offset 12 (0x100C): pointer to data at 0x5000
    uint64_t litVal = 0x5000;
    for (int i=0; i<8; ++i) codeData[12+i] = (litVal>>(8*i))&0xFF;

    FlatView view;
    view.addSection(0x1000, codeData, 32, /*code=*/true);
    view.addSection(0x5000, cstr("literal pool string"));

    std::vector<InstrRef> refs = {
        { 0x1000, 0x100C, /*isLiteralPool=*/true }
    };
    auto entries = extractLiteralPool(view, refs);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].vma,   0x100Cu);
    EXPECT_EQ(entries[0].value, 0x5000u);
    EXPECT_TRUE(entries[0].isPointer);
}

TEST(LiteralPoolTest, DuplicateRefsDeduped) {
    uint8_t codeData[32] = {};
    uint64_t litVal = 0x5000;
    for (int i=0; i<8; ++i) codeData[8+i]=(litVal>>(8*i))&0xFF;

    FlatView view;
    view.addSection(0x1000, codeData, 32, true);
    view.addSection(0x5000, cstr("test"));

    std::vector<InstrRef> refs = {
        { 0x1000, 0x1008, true },
        { 0x1004, 0x1008, true },  // same target
    };
    auto entries = extractLiteralPool(view, refs);
    EXPECT_EQ(entries.size(), 1u); // deduplicated
}

TEST(LiteralPoolTest, ViaStringDetectorLiteralPool) {
    uint8_t codeData[32] = {};
    // Literal at 0x100C = pointer to string at 0x5000
    uint64_t litVal = 0x5000;
    for (int i=0; i<8; ++i) codeData[12+i]=(litVal>>(8*i))&0xFF;

    FlatView view;
    view.addSection(0x1000, codeData, 32, true);
    view.addSection(0x5000, cstr("arm literal string"));

    StringDetectorConfig cfg; cfg.minStringLen=2;
    StringDetector det(view, cfg);
    std::vector<LiteralPoolEntry> pool;
    std::vector<StringLiteral>   strs;
    det.onLiteralPool([&](auto& e){ pool.push_back(e); });
    det.onString([&](auto& s){ strs.push_back(s); });

    det.processRef({ 0x1000, 0x100C, true });

    ASSERT_EQ(pool.size(), 1u);
    EXPECT_EQ(pool[0].value, 0x5000u);
    EXPECT_TRUE(pool[0].isPointer);
    // Also should have found the string at 0x5000
    ASSERT_EQ(strs.size(), 1u);
    EXPECT_EQ(strs[0].value, "arm literal string");
    // Pool address marked as data
    EXPECT_TRUE(det.isDataMarked(0x100C));
}

// ─── encodingName / stringKindName ───────────────────────────────────────────

TEST(NamesTest, EncodingNames) {
    EXPECT_STREQ(encodingName(EncodingKind::ASCII),   "ASCII");
    EXPECT_STREQ(encodingName(EncodingKind::UTF8),    "UTF-8");
    EXPECT_STREQ(encodingName(EncodingKind::Latin1),  "Latin-1");
    EXPECT_STREQ(encodingName(EncodingKind::UTF16LE), "UTF-16LE");
    EXPECT_STREQ(encodingName(EncodingKind::UTF16BE), "UTF-16BE");
    EXPECT_STREQ(encodingName(EncodingKind::Unknown), "Unknown");
}

TEST(NamesTest, StringKindNames) {
    EXPECT_STREQ(stringKindName(StringKind::CNulTerminated), "C-string");
    EXPECT_STREQ(stringKindName(StringKind::Pascal),         "Pascal");
    EXPECT_STREQ(stringKindName(StringKind::LengthPrefixed), "LengthPrefixed");
    EXPECT_STREQ(stringKindName(StringKind::Wide),           "Wide");
}

// ─── debugStr ─────────────────────────────────────────────────────────────────

TEST(DebugStrTest, StringLiteralDebugStr) {
    StringLiteral sl;
    sl.address   = 0x5000;
    sl.value     = "hello";
    sl.charCount = 5;
    sl.kind      = StringKind::CNulTerminated;
    sl.encoding  = EncodingKind::ASCII;
    std::string s = sl.debugStr();
    EXPECT_NE(s.find("hello"),  std::string::npos);
    EXPECT_NE(s.find("ASCII"),  std::string::npos);
    EXPECT_NE(s.find("5000"),   std::string::npos);
}
