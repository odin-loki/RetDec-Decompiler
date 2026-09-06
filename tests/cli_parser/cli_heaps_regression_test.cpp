/**
 * @file tests/cli_parser/cli_heaps_regression_test.cpp
 * @brief Regression tests for the SMT-confirmed defects in the CLI heap,
 *        signature and table readers.
 *
 * Every test here was watched failing against the code as it stood before the
 * fix beside it. Each names the ESBMC witness it encodes, so a revert is a
 * named failure rather than a mysterious one.
 */

#include "retdec/cli_parser/cli_heaps.h"
#include "retdec/cli_parser/cli_sig.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

using namespace retdec::cli_parser;
using namespace retdec::bc_module;

// ─── Allocation counter ───────────────────────────────────────────────────────
//
// ArrayShape's `sizes` and `loBounds` vectors are consumed inside decodeType and
// never surface in the CliType it returns, so the only observable consequence of
// a declared count the blob cannot supply is the memory it commits. Counting it
// is therefore the only way to assert the bound; a wall-clock assertion would be
// both flaky and, at a count small enough to be safe to run, too fast to
// measure.
//
// Replacing the global operator new is program-wide and deliberate: it forwards
// to malloc/free and only adds a counter, so every other test in this binary
// behaves exactly as before.

namespace {
std::atomic<std::size_t> g_bytesAllocated{0};
std::atomic<bool>        g_countingAllocations{false};

/// Count allocations for the duration of a scope.
struct AllocationScope {
    AllocationScope() {
        g_bytesAllocated.store(0, std::memory_order_relaxed);
        g_countingAllocations.store(true, std::memory_order_relaxed);
    }
    ~AllocationScope() { g_countingAllocations.store(false, std::memory_order_relaxed); }
    std::size_t bytes() const { return g_bytesAllocated.load(std::memory_order_relaxed); }
};
} // namespace

void* operator new(std::size_t n) {
    if (g_countingAllocations.load(std::memory_order_relaxed))
        g_bytesAllocated.fetch_add(n, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete[](void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// ─── GuidHeap: the 1-based index multiplication ───────────────────────────────

namespace {
/// ECMA-335 II.24.2.5: the #GUID heap is a sequence of 16-byte entries.
constexpr std::size_t kGuidBytes = 16;

bool isAllZero(const Guid& g) {
    for (std::size_t i = 0; i < kGuidBytes; ++i)
        if (g.bytes[i] != 0) return false;
    return true;
}
} // namespace

// ESBMC's round witness for src/cli_parser/cli_heaps.cpp:128. A heap holding
// exactly one GUID is the ordinary case -- an assembly has one MVID. With
// `size_t off = (index - 1) * 16` formed in uint32, index 0x10000001 gives a
// true offset of 4294967296 which wraps to 0, `0 + 16 > 16` is false, and the
// module's own MVID comes back for an index that addresses nothing.
TEST(CliHeapsGuidRegression, IndexAt16MDoesNotWrapOntoTheFirstGuid) {
    std::vector<uint8_t> heap(kGuidBytes);
    for (std::size_t i = 0; i < kGuidBytes; ++i)
        heap[i] = static_cast<uint8_t>(0xA0 + i);
    GuidHeap h({heap.data(), heap.size()});

    // The one GUID that is really there is still readable.
    Guid mvid = h.get(1);
    EXPECT_EQ(0xA0, mvid.bytes[0]);
    EXPECT_EQ(0xAF, mvid.bytes[15]);

    // (0x10000001 - 1) * 16 == 2^32, which is 0 in 32 bits.
    Guid wrapped = h.get(0x10000001u);
    EXPECT_TRUE(isAllZero(wrapped))
        << "index 0x10000001 aliased onto heap offset 0";
}

// The second ESBMC witness, at a less round point: index = 805306623 on a
// 4080-byte heap. True offset 12884905952 (12 GB into a 4 KB heap); the uint32
// product wraps to 4064, 4064 + 16 <= 4080, and the LAST GUID of the heap is
// returned with a success indication no caller can distinguish from a real read.
TEST(CliHeapsGuidRegression, WrappedIndexDoesNotAliasTheLastGuid) {
    // 4080 == 255 * 16, so the last valid 1-based index is 255.
    std::vector<uint8_t> heap(4080, 0);
    for (std::size_t i = 0; i < kGuidBytes; ++i)
        heap[4080 - kGuidBytes + i] = static_cast<uint8_t>(0x50 + i);
    GuidHeap h({heap.data(), heap.size()});

    Guid last = h.get(255);
    EXPECT_EQ(0x50, last.bytes[0]);

    Guid wrapped = h.get(805306623u);
    EXPECT_TRUE(isAllZero(wrapped))
        << "index 805306623 aliased onto heap offset 4064";
}

// The plain out-of-range case, which the old code did get right, kept so the fix
// cannot be "refuse everything".
TEST(CliHeapsGuidRegression, IndexPastTheEndIsStillRefusedAndIndexZeroIsNull) {
    std::vector<uint8_t> heap(kGuidBytes, 0xFF);
    GuidHeap h({heap.data(), heap.size()});
    EXPECT_TRUE(isAllZero(h.get(0)));
    EXPECT_TRUE(isAllZero(h.get(2)));
    EXPECT_FALSE(isAllZero(h.get(1)));
}

// ─── decodeCompressedInt: sign extension by width, not by magnitude ──────────

namespace {
int32_t decodeSigned(const std::vector<uint8_t>& blob, std::size_t* consumed = nullptr) {
    std::size_t pos = 0;
    auto v = decodeCompressedInt({blob.data(), blob.size()}, pos);
    EXPECT_TRUE(v.has_value());
    if (consumed) *consumed = pos;
    return v.value_or(0x7FFFFFFF);
}
} // namespace

// ECMA-335 II.23.2: the one-byte signed form spans [-64, 63] and 0x7F is -1.
// The old code picked its sign-extension mask from the decoded MAGNITUDE, so
// every odd byte above 0x3F -- all 32 of them -- took the two-byte branch and
// came back extended from bit 13: 0x7F returned 63 | 0xFFFFE000 == -8129.
TEST(CliHeapsCompressedIntRegression, OneByteSignedMatchesEcma) {
    std::size_t used = 0;
    EXPECT_EQ(-1,  decodeSigned({0x7F}, &used));   // ESBMC witness b = 127
    EXPECT_EQ(1u,  used);
    EXPECT_EQ(-2,  decodeSigned({0x7D}));
    EXPECT_EQ(-3,  decodeSigned({0x7B}));          // II.23.2's worked example
    EXPECT_EQ(-32, decodeSigned({0x41}));
    EXPECT_EQ(-64, decodeSigned({0x01}));          // most negative one-byte value
    EXPECT_EQ(0,   decodeSigned({0x00}));
    EXPECT_EQ(3,   decodeSigned({0x06}));
    EXPECT_EQ(63,  decodeSigned({0x7E}));          // most positive one-byte value
}

// The defect is not confined to the one-byte form: the same magnitude is a legal
// payload at all three widths. 0x80 0x3F has magnitude 63, which the old code
// put in the one-byte bucket, so -8161 came back as -33.
TEST(CliHeapsCompressedIntRegression, TwoByteSignedMatchesEcma) {
    std::size_t used = 0;
    EXPECT_EQ(-8192, decodeSigned({0x80, 0x01}, &used)); // II.23.2: most negative
    EXPECT_EQ(2u,    used);
    EXPECT_EQ(-8161, decodeSigned({0x80, 0x3F}));        // ESBMC witness u = 63
    EXPECT_EQ(8191,  decodeSigned({0xBF, 0xFE}));        // most positive
}

// 0xC0 0x00 0x24 0x7F has magnitude 9343, which the old code put in the two-byte
// bucket: -268430785 came back as -3521.
TEST(CliHeapsCompressedIntRegression, FourByteSignedMatchesEcma) {
    std::size_t used = 0;
    EXPECT_EQ(-268435456, decodeSigned({0xC0, 0x00, 0x00, 0x01}, &used));
    EXPECT_EQ(4u,         used);
    EXPECT_EQ(-268430785, decodeSigned({0xC0, 0x00, 0x24, 0x7F})); // ESBMC witness
    EXPECT_EQ(268435455,  decodeSigned({0xDF, 0xFF, 0xFF, 0xFE})); // most positive
}

// A refusal must consume nothing, so a caller can retry or resynchronise.
TEST(CliHeapsCompressedIntRegression, RefusalLeavesTheCursorAlone) {
    std::vector<uint8_t> truncated = {0xC0, 0x00}; // declares 4 bytes, supplies 2
    std::size_t pos = 0;
    EXPECT_FALSE(decodeCompressedUInt({truncated.data(), truncated.size()}, pos)
                     .has_value());
    EXPECT_EQ(0u, pos);

    std::vector<uint8_t> badTag = {0xE0}; // 111xxxxx is not a compressed integer
    pos = 0;
    EXPECT_FALSE(decodeCompressedUInt({badTag.data(), badTag.size()}, pos)
                     .has_value());
    EXPECT_EQ(0u, pos);
}

// ─── UserStringsHeap: one cursor, not two ────────────────────────────────────

// The cursor used to start at `offset` and then be handed to a span already
// advanced by `offset`, so the length prefix was read from data_[2 * offset] and
// the characters from 2 * offset + bytesRead. This heap is built so that both
// readings succeed and return DIFFERENT strings, which is what makes it a silent
// wrong answer rather than a refusal.
//
// Offset 6 holds "Hi"; data_[12] (== 2 * 6) is another valid length prefix and
// the four bytes after it spell "XY".
TEST(CliHeapsUserStringsRegression, StringAtNonZeroOffsetUsesItsOwnLengthPrefix) {
    std::vector<uint8_t> heap = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       // 0..5   filler
        0x05, 0x48, 0x00, 0x69, 0x00, 0x00,       // 6..11  len 5, "Hi", flag
        0x05, 0x58, 0x00, 0x59, 0x00, 0x00,       // 12..17 len 5, "XY", flag
    };
    UserStringsHeap h({heap.data(), heap.size()});

    // Old code: prefix from heap[12], characters from heap[13..16] -> "XY".
    EXPECT_EQ("Hi", h.get(6));
    // The string that really is at 12 is unaffected either way.
    EXPECT_EQ("XY", h.get(12));
    // Offset 0 was the one case the old spelling happened to get right, and is
    // why this went unnoticed.
    EXPECT_EQ("", h.get(0));
}

TEST(CliHeapsUserStringsRegression, OffsetPastTheHeapIsRefused) {
    std::vector<uint8_t> heap = {0x05, 0x48, 0x00, 0x69, 0x00, 0x00};
    UserStringsHeap h({heap.data(), heap.size()});
    EXPECT_EQ("Hi", h.get(0));
    EXPECT_EQ("", h.get(6));
    EXPECT_EQ("", h.get(0xFFFFFFFFu));
}

// U+1F600, which UTF-16 spells as the surrogate pair D83D DE00. The old decoder
// took the three-byte `else` branch for each unit separately and emitted
// ED A0 BD ED B8 80 -- two ill-formed sequences where one four-byte character
// belongs. ESBMC's witness for the branch as written was cp = 0xD800 -> ED A0 80.
TEST(CliHeapsUserStringsRegression, SurrogatePairBecomesOneScalarValue) {
    // Blob length 5 == 4 character bytes + the trailing flag byte (II.24.2.4).
    std::vector<uint8_t> heap = {0x05, 0x3D, 0xD8, 0x00, 0xDE, 0x00};
    UserStringsHeap h({heap.data(), heap.size()});

    const std::string s = h.get(0);
    ASSERT_EQ(4u, s.size()) << "a surrogate pair is one character, not two";
    EXPECT_EQ("\xF0\x9F\x98\x80", s);
    EXPECT_NE(static_cast<char>(0xED), s[0]);
}

// A high surrogate with nothing after it is not a character at all. It must
// become U+FFFD rather than the ill-formed ED A0 BD the old branch produced.
TEST(CliHeapsUserStringsRegression, LoneSurrogateBecomesTheReplacementCharacter) {
    std::vector<uint8_t> heap = {0x03, 0x3D, 0xD8, 0x00};
    UserStringsHeap h({heap.data(), heap.size()});

    const std::string s = h.get(0);
    EXPECT_EQ("\xEF\xBF\xBD", s);
}

// The well-formed cases the kernel is proved to leave byte-identical, kept so
// the routing cannot silently change ordinary strings.
TEST(CliHeapsUserStringsRegression, NonSurrogateUnitsAreUnchanged) {
    // "A" U+00E9 U+20AC : one, two and three byte outputs.
    std::vector<uint8_t> heap = {
        0x07, 0x41, 0x00, 0xE9, 0x00, 0xAC, 0x20, 0x00
    };
    UserStringsHeap h({heap.data(), heap.size()});
    EXPECT_EQ("A\xC3\xA9\xE2\x82\xAC", h.get(0));
}

// ─── cli_sig: a declared count is not a promise ──────────────────────────────

// ELEMENT_TYPE_GENERICINST (II.23.2.12): 0x15, CLASS, TypeDefOrRef, GenArgCount,
// then that many Types. Here the blob ends at the count, so no argument can
// exist -- but `.value_or(0)` on the count fed the loop bound directly and the
// loop pushed a default-constructed BcType per iteration regardless.
TEST(CliSigCountRegression, GenericArgCountIsBoundedByTheBytesLeft) {
    // 0x83 0xE8 is the two-byte encoding of 1000.
    std::vector<uint8_t> blob = {0x15, 0x12, 0x04, 0x83, 0xE8};
    CliSigDecoder dec;
    auto ct = dec.decodeTypeSpec({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
    ASSERT_TRUE(ct->base.isRef());
    EXPECT_EQ(0u, ct->base.ref().typeArgs.size())
        << "1000 generic arguments declared by a 5-byte blob";
}

// The same blob shape with the arguments actually present must still decode, so
// the bound cannot be "refuse every generic instantiation".
TEST(CliSigCountRegression, GenericArgsThatArePresentStillDecode) {
    // GENERICINST CLASS <token> 2, then I4 and STRING.
    std::vector<uint8_t> blob = {0x15, 0x12, 0x04, 0x02, 0x08, 0x0E};
    CliSigDecoder dec;
    auto ct = dec.decodeTypeSpec({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
    ASSERT_TRUE(ct->base.isRef());
    EXPECT_EQ(2u, ct->base.ref().typeArgs.size());
}

// ELEMENT_TYPE_ARRAY (II.23.2.13): 0x14, Type, rank, NumSizes, Size*,
// NumLoBounds, LoBound*. The spec's own ESBMC witness is the five-byte blob
// 01 DF FF FF FF, whose NumSizes decodes to 536870911 with zero bytes left --
// 2048 MB of int32 pushed out of five bytes. A count of 1000000 is the same
// defect at a size that is safe to run un-fixed: the old loop still commits
// megabytes for a seven-byte signature.
//
// decodeArrayShape's sizes/loBounds do not reach the returned CliType, so the
// memory is the only observable consequence and the assertion is on the memory.
TEST(CliSigCountRegression, ArrayShapeSizeCountIsBoundedByTheBytesLeft) {
    // 0xC0 0x0F 0x42 0x40 is the four-byte encoding of 1000000.
    std::vector<uint8_t> blob = {0x14, 0x08, 0x01, 0xC0, 0x0F, 0x42, 0x40};
    CliSigDecoder dec;

    std::size_t used = 0;
    std::optional<CliType> ct;
    {
        AllocationScope scope;
        ct = dec.decodeTypeSpec({blob.data(), blob.size()});
        used = scope.bytes();
    }

    ASSERT_TRUE(ct.has_value());
    ASSERT_TRUE(ct->base.isRef());
    EXPECT_EQ(1, ct->base.ref().arrayDims);
    // 1000000 int32 is 4 MB before the reallocation churn. A correct decode of a
    // seven-byte signature allocates a few hundred bytes.
    EXPECT_LT(used, 256u * 1024u)
        << "a 7-byte array signature committed " << used << " bytes";
}

// A well-formed ArrayShape must still be walked to its end, so that the type
// after it in a signature is decoded from the right position.
TEST(CliSigCountRegression, WellFormedArrayShapeStillDecodes) {
    // ARRAY I4 rank=2 NumSizes=2 [4,4] NumLoBounds=2 [-1(0x01), 0(0x00)]
    std::vector<uint8_t> blob = {
        0x14, 0x08, 0x02, 0x02, 0x04, 0x04, 0x02, 0x01, 0x00
    };
    CliSigDecoder dec;
    auto ct = dec.decodeTypeSpec({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
    ASSERT_TRUE(ct->base.isRef());
    EXPECT_EQ(2, ct->base.ref().arrayDims);
}
