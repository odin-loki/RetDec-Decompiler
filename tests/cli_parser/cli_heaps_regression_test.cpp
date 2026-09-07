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
#include "retdec/cli_parser/cli_tables.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

namespace retdec {
namespace cli_parser {
// Defined in src/cli_parser/cli_tables.cpp, and declared here rather than in
// cli_tables.h because it is not API. It is a free function precisely so that
// this file can reach the ECMA-335 II.24.2.6 coded-index split: the split's
// only other caller is MetadataTables::RowReader::codedToken, and RowReader is
// a private nested type that no test can name. Every one of decodeRow's
// nineteen codedToken calls passes a literal tagBits of 1, 2, 3 or 5, so
// nothing reachable through the public parse() can exercise the widths that go
// wrong.
MetadataToken splitCodedToken(std::uint32_t raw, const std::uint8_t* tableIds,
                              std::size_t count, unsigned tagBits);
} // namespace cli_parser
} // namespace retdec

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
    // And the refusal says so. Setting the count to 0 and carrying on produced
    // types::Generic(Class(name), {}), an instantiation with no arguments --
    // which II.23.2.12's `Type Type*` argument list makes impossible in a
    // well-formed signature, so no caller could tell it from a real decode.
    // decodeArrayShape refuses rather than truncating for the same reason; this
    // is the assertion that the two sites agree.
    // Compared as integers because BcRefKind's underlying type is uint8_t, which
    // an ostream prints as a control character rather than as a number.
    EXPECT_NE(static_cast<int>(BcRefKind::Generic),
              static_cast<int>(ct->base.ref().kind))
        << "a count no continuation of this blob could supply was reported as "
           "a generic instantiation of arity zero";
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
    // ARRAY I4 rank=2 NumSizes=2 [4,4] NumLoBounds=2 [-64(0x01), 0(0x00)].
    //
    // The lo-bound bytes are named by the value ECMA-335 II.23.2 gives them and
    // not by the value they look like: in the one-byte signed form the payload
    // is rotated, so 0x01 is -64 and -1 is 0x7F. OneByteSignedMatchesEcma above
    // asserts exactly that pairing, and it is the fact the sign-extension fix
    // was written to establish.
    std::vector<uint8_t> blob = {
        0x14, 0x08, 0x02, 0x02, 0x04, 0x04, 0x02, 0x01, 0x00
    };
    CliSigDecoder dec;
    auto ct = dec.decodeTypeSpec({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
    ASSERT_TRUE(ct->base.isRef());
    EXPECT_EQ(2, ct->base.ref().arrayDims);
}

// The control above cannot see where the cursor ended up: decodeTypeSpec decodes
// one Type and stops, and the shape's sizes and lo-bounds never reach the
// returned CliType. A LocalVarSig can see it, because the second local is
// decoded from wherever the first one left `pos`.
//
// So this is the assertion that a countFits refusal cannot be tightened into
// "refuse the whole shape whenever a count is present". Either count arm
// returning early leaves `pos` inside the shape and decodes the second local
// from a byte that is not a type: from the NumLoBounds arm it lands on the
// first lo-bound, 0x01, which is ELEMENT_TYPE_VOID; from the NumSizes arm on
// the first size, 0x04, which is ELEMENT_TYPE_I1. Neither is the STRING that
// is really there.
TEST(CliSigCountRegression, ArrayShapeIsWalkedToItsEnd) {
    // LOCAL_SIG(0x07) count=2
    //   local 0: ARRAY I4 rank=2 NumSizes=2 [4,4] NumLoBounds=2 [-64, 0]
    //   local 1: STRING
    std::vector<uint8_t> blob = {
        0x07, 0x02,
        0x14, 0x08, 0x02, 0x02, 0x04, 0x04, 0x02, 0x01, 0x00,
        0x0E
    };
    CliSigDecoder dec;
    auto sig = dec.decodeLocalVar({blob.data(), blob.size()});
    ASSERT_TRUE(sig.has_value());
    ASSERT_EQ(2u, sig->locals.size());
    ASSERT_TRUE(sig->locals[0].base.isRef());
    EXPECT_EQ(2, sig->locals[0].base.ref().arrayDims);
    EXPECT_EQ(types::ClrString(), sig->locals[1].base)
        << "the second local decoded as " << sig->locals[1].base.toString()
        << ", so the ArrayShape walk left the cursor in the wrong place";
}

// ─── cli_sig: a Type is defined recursively, and nothing bounded the descent ──

namespace {
/// Depth of the SZARRAY / ARRAY chain @p t is the outside of.
std::size_t arrayNesting(const BcType& t) {
    std::size_t n = 0;
    const BcType* cur = &t;
    while (cur->isRef() && cur->ref().kind == BcRefKind::Array
           && cur->ref().elementType) {
        ++n;
        cur = cur->ref().elementType.get();
    }
    return n;
}
} // namespace

// ELEMENT_TYPE_SZARRAY (0x1D) is one byte and its operand is another Type, so a
// blob of nothing but 0x1D bytes recurses once per byte, and there is no count
// anywhere in it for bounds::countFits to bound. Measured on this tree with
// `g++ -std=c++20 -O1 -g` and the 8 MB stack `ulimit -s` reports here: 10000
// such bytes came back as a 10000-deep type, 12343 still returned, and 12500
// segfaulted.
//
// The sizes here are deliberately well under that crash point, because a test
// that dies on the stack when the fix is reverted takes the whole binary with
// it -- including every result buffered before it -- and reports nothing about
// what went wrong. What is asserted instead is the property that makes the
// crash unreachable at any size: the descent is bounded by the decoder, so the
// answer stops getting deeper as the blob gets longer.
TEST(CliSigDepthRegression, NestedTypeDepthDoesNotFollowTheBlobLength) {
    constexpr std::size_t kShort = 128;
    constexpr std::size_t kLong  = 4096;

    CliSigDecoder dec;
    std::vector<uint8_t> shortBlob(kShort, 0x1D);
    std::vector<uint8_t> longBlob(kLong, 0x1D);

    auto shortCt = dec.decodeTypeSpec({shortBlob.data(), shortBlob.size()});
    auto longCt  = dec.decodeTypeSpec({longBlob.data(), longBlob.size()});
    ASSERT_TRUE(shortCt.has_value());
    ASSERT_TRUE(longCt.has_value());

    const std::size_t shortNesting = arrayNesting(shortCt->base);
    const std::size_t longNesting  = arrayNesting(longCt->base);

    // Not "refuse everything": the outermost arrays are still decoded.
    EXPECT_GT(shortNesting, 0u);
    EXPECT_LT(longNesting, kLong)
        << "a " << kLong << "-byte SZARRAY chain nested " << longNesting
        << " deep, one frame per byte";
    // The bound is on the descent, not on the input, so thirty-two times the
    // input buys no extra depth -- which is what makes 12500 bytes safe without
    // this test having to run 12500 bytes through an unfixed decoder.
    EXPECT_EQ(shortNesting, longNesting)
        << kShort << " bytes nested " << shortNesting << " deep and " << kLong
        << " bytes nested " << longNesting << " deep, so nothing bounds the "
           "descent but the length of the blob";
}

namespace {
/// The smallest model of CLIReader::typeSpecType that reproduces the cycle: a
/// TypeSpec row whose signature blob is fetched from the #Blob heap and handed
/// back to the same signature decoder.
///
/// `kCallCeiling` is what keeps a revert of the fix reportable. Without it the
/// recursion below is unbounded and the process dies on the stack; with it the
/// resolver stops feeding the cycle after a fixed number of turns and the test
/// asserts on the count instead. The ceiling is set well under the 6001 turns
/// this cycle was measured to survive, so it is always the ceiling that stops an
/// unfixed decoder, never the stack.
class SelfReferentialTypeSpec : public ITypeNameResolver {
public:
    static constexpr long kCallCeiling = 2048;

    void setDecoder(const CliSigDecoder* d) { decoder_ = d; }
    long calls() const { return calls_; }

    std::string typeDefName(uint32_t) const override { return "TypeDef"; }
    std::string typeRefName(uint32_t) const override { return "TypeRef"; }

    BcType typeSpecType(uint32_t) const override {
        if (++calls_ > kCallCeiling) return types::ClrObject();
        // ELEMENT_TYPE_CLASS, then the compressed TypeDefOrRef 0x06: tag 2 is
        // TypeSpec and the index above the tag is 1, so this row's signature
        // names this row. Every turn starts a fresh decode of the same two
        // bytes from position zero, so the cycle makes no progress through any
        // buffer and no cursor bound can stop it.
        static const uint8_t kSelfBlob[] = {0x12, 0x06};
        auto ct = decoder_->decodeTypeSpec({kSelfBlob, sizeof(kSelfBlob)});
        return ct ? ct->base : types::ClrObject();
    }

private:
    const CliSigDecoder* decoder_ = nullptr;
    mutable long         calls_   = 0;
};
} // namespace

// The cheaper half of the same defect: the descent does not have to consume
// input at all. decodeType's CLASS arm calls tokenName, tokenName asks the
// resolver for a TypeSpec row's type, and the resolver decodes that row's blob
// with this decoder -- so a row that names itself re-enters decodeType forever.
// Against a resolver modelled on CLIReader::typeSpecType this segfaulted, having
// survived 6001 turns and died by 6500.
TEST(CliSigDepthRegression, SelfReferentialTypeSpecTerminates) {
    SelfReferentialTypeSpec resolver;
    CliSigDecoder dec(&resolver);
    resolver.setDecoder(&dec);

    const std::vector<uint8_t> blob = {0x12, 0x06};
    auto ct = dec.decodeTypeSpec({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());

    // The cycle was entered -- otherwise this asserts nothing -- and the decoder
    // and not the ceiling is what stopped it.
    EXPECT_GT(resolver.calls(), 0);
    EXPECT_LT(resolver.calls(), SelfReferentialTypeSpec::kCallCeiling)
        << "the decoder re-entered itself until the fake resolver's ceiling "
           "stopped it; nothing in the decoder bounds the descent";
}

// ─── cli_tables: the coded-index split ───────────────────────────────────────

namespace {
/// ECMA-335 II.24.2.6 TypeDefOrRef: TypeDef, TypeRef, TypeSpec, 2 tag bits.
const uint8_t kTypeDefOrRefTables[] = {0x02, 0x01, 0x1B};
} // namespace

// The split used to be `raw & ((1u << tagBits) - 1)` and `raw >> tagBits`, on a
// tagBits the caller supplies. The left operand of each shift is a 32-bit
// unsigned int, so a count of 32 or more is undefined behaviour rather than a
// wrong answer: `g++ -fsanitize=undefined` on those two expressions at
// tagBits = 65 reports "shift exponent 65 is too large for 32-bit type
// 'unsigned int'" for each of them.
//
// What comes out without a sanitizer depends on how the shift was compiled.
// x86-64 takes the count modulo 32, so tagBits = 65 masks and shifts by one;
// GCC folding the shift at compile time yields zero for `1u << 65` instead, and
// zero minus one is a mask of every bit. The raw words used here are 0, 1 and 2,
// which stay in range for this three-entry table under either -- so what
// separates a fixed split from an unfixed one is the refusal itself, whichever
// way the undefined shift went.
TEST(CliTablesCodedTokenRegression, TagWiderThanTheWordIsRefused) {
    for (unsigned tagBits : {32u, 65u, 255u}) {
        for (uint32_t raw : {0u, 1u, 2u}) {
            MetadataToken tok = splitCodedToken(
                raw, kTypeDefOrRefTables, 3, tagBits);
            EXPECT_EQ(0xFF, unsigned(tok.table))
                << "tagBits=" << tagBits << " raw=" << raw
                << " named table " << unsigned(tok.table);
            EXPECT_EQ(0u, tok.index) << "tagBits=" << tagBits << " raw=" << raw;
            EXPECT_FALSE(tok.valid());
        }
    }
}

// The widths decodeRow actually passes must still split the way II.24.2.6 says,
// so the refusal above cannot be "refuse everything".
TEST(CliTablesCodedTokenRegression, TheWidthsDecodeRowUsesStillSplit) {
    // 0x0D is 1101b: 2 tag bits give tag 1 (TypeRef) and row 3.
    MetadataToken tok = splitCodedToken(0x0Du, kTypeDefOrRefTables, 3, 2);
    EXPECT_EQ(0x01u, unsigned(tok.table));
    EXPECT_EQ(3u,   tok.index);

    // Lossless: the two halves put back together are the word that went in.
    EXPECT_EQ(0x0Du, (tok.index << 2) | 1u);

    // Tag 0 with no tag bits at all: the whole word is the row index.
    MetadataToken all = splitCodedToken(0xDEADBEEFu, kTypeDefOrRefTables, 3, 0);
    EXPECT_EQ(0x02u,       unsigned(all.table));
    EXPECT_EQ(0xDEADBEEFu, all.index);

    // A 3-bit tag admits eight values and CustomAttributeType has five entries.
    // Tag 6 is one of the three that index nothing, and must not become a table.
    static const uint8_t kCustomAttrType[] = {0xFF, 0xFF, 0x06, 0x0A, 0xFF};
    MetadataToken oob = splitCodedToken(0x2Eu, kCustomAttrType, 5, 3);
    EXPECT_EQ(0xFFu, unsigned(oob.table));
    EXPECT_EQ(5u,   oob.index);  // 0x2E >> 3, kept as it always has been

    // The widest defined tag. 31 bits is still a defined shift; 32 is not.
    MetadataToken wide = splitCodedToken(0x80000003u, kTypeDefOrRefTables, 3, 31);
    EXPECT_EQ(1u, wide.index);
    EXPECT_EQ(0xFFu, unsigned(wide.table));  // tag 3 indexes nothing here
}
