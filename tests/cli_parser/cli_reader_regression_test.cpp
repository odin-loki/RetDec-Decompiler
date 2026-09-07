/**
 * @file tests/cli_parser/cli_reader_regression_test.cpp
 * @brief Regression tests for the SMT-confirmed defects in CLIReader's #Blob
 *        constant decoding.
 *
 * CLIReader::fieldConstantInt / fieldConstantFloat / fieldConstantString are
 * private, and the only observable route to them is a whole assembly: read()
 * walks TypeDef, then Field, then Constant, and drops the decoded values into
 * BcField::constantIntValue / constantFltValue / constantStrValue. So each test
 * here assembles a real .NET PE in memory -- DOS stub, PE signature, COFF
 * header, PE32 optional header with a COM descriptor directory, one section, a
 * CLI header, and a metadata root carrying #~, #Strings and #Blob -- with one
 * class holding one field that carries one Constant row. The only thing that
 * varies between tests is the ElementType byte and the constant blob.
 *
 * Not every test here is a replay of a defect, and each one says which it is
 * rather than leaving the reader to assume. Watched failing against the code
 * as it stood before the fix beside them: the three surrogate string tests;
 * the Char test; the Char rows above U+7FFF and the Boolean rows above 0x7F in
 * FixedWidthConstantsKeepTheirValues; and all ten rows of
 * TruncatedFixedWidthConstantYieldsNoValue. Not watched failing, because no
 * failing input exists on a little-endian host: the two float tests, whose
 * defect is a byte order this host cannot distinguish. Nor the remaining
 * fixed-width rows, which pin values a fix was not supposed to change. The
 * comment above each test says which of those it is.
 */

#include "retdec/cli_parser/cli_reader.h"
#include "retdec/cli_parser/cli_sig.h"
#include "retdec/cli_parser/cli_tables.h"

#include <gtest/gtest.h>

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

using namespace retdec::cli_parser;
using namespace retdec::bc_module;

namespace {

// ─── little-endian writers ───────────────────────────────────────────────────

void putU8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }
void putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
void putU32(std::vector<uint8_t>& v, uint32_t x) {
    putU16(v, static_cast<uint16_t>(x & 0xFFFF));
    putU16(v, static_cast<uint16_t>(x >> 16));
}
void putU64(std::vector<uint8_t>& v, uint64_t x) {
    putU32(v, static_cast<uint32_t>(x));
    putU32(v, static_cast<uint32_t>(x >> 32));
}

// ─── metadata table constants, ECMA-335 II.22 ────────────────────────────────

/// Table numbers, ECMA-335 II.22: the bit each table occupies in the #~ Valid
/// mask is its table number, and the row-count array is in ascending order of
/// that number.
constexpr unsigned kTableModule   = 0x00;
constexpr unsigned kTableTypeDef  = 0x02;
constexpr unsigned kTableField    = 0x04;
constexpr unsigned kTableConstant = 0x0B;

/// HasConstant is a 2-bit coded index over { Field, Param, Property }
/// (ECMA-335 II.24.2.6); tag 0 is Field, so a Field row index i encodes as
/// (i << 2) | 0.
constexpr uint16_t kHasConstantFieldTag = 0;
constexpr unsigned kHasConstantTagBits  = 2;

/// TypeAttributes.Public (ECMA-335 II.23.1.15). Anything but NotPublic keeps
/// the class through CliReadOptions::skipPrivate, which defaults to false
/// anyway; stated so the flags word is not a bare 1.
constexpr uint32_t kTypeAttrPublic = 0x00000001;

/// FieldAttributes: Public | Static | Literal | HasDefault -- what the C#
/// compiler emits for `public const`, which is the only shape that carries a
/// Constant row (ECMA-335 II.23.1.5).
constexpr uint16_t kFieldAttrPublic    = 0x0006;
constexpr uint16_t kFieldAttrStatic    = 0x0010;
constexpr uint16_t kFieldAttrLiteral   = 0x0040;
constexpr uint16_t kFieldAttrHasDefault = 0x8000;

// ─── #Strings heap ───────────────────────────────────────────────────────────
//
// ECMA-335 II.24.2.3: a run of NUL-terminated UTF-8 strings, indexed by byte
// offset, and offset 0 is the empty string.

constexpr uint16_t kStrEmpty    = 0;   ///< ""
constexpr uint16_t kStrModule   = 1;   ///< "<Module>"
constexpr uint16_t kStrClass    = 10;  ///< "C"
constexpr uint16_t kStrField    = 12;  ///< "K"
constexpr uint16_t kStrAsmName  = 14;  ///< "t.dll"

std::vector<uint8_t> buildStringsHeap() {
    std::vector<uint8_t> v;
    v.push_back(0);                                   // offset 0: ""
    const char* items[] = {"<Module>", "C", "K", "t.dll"};
    for (const char* s : items) {
        while (*s) v.push_back(static_cast<uint8_t>(*s++));
        v.push_back(0);
    }
    // The offsets above are load-bearing. They are checked here rather than
    // trusted, because a drift would leave the assembly parsing while every
    // name came out wrong -- a much harder failure to read than an abort.
    assert(v[kStrModule] == '<' && v[kStrClass] == 'C'
           && v[kStrField] == 'K' && v[kStrAsmName] == 't');
    return v;
}

// ─── #Blob heap ──────────────────────────────────────────────────────────────
//
// ECMA-335 II.24.2.4: each entry is a compressed-unsigned length followed by
// that many bytes. Offset 0 is the empty blob, which is what the Field
// signature index points at here -- CLIReader skips signature decoding for an
// empty blob, so no field signature has to be spelled out.

constexpr uint16_t kBlobEmpty    = 0;
constexpr uint16_t kBlobConstant = 1;

std::vector<uint8_t> buildBlobHeap(const std::vector<uint8_t>& constantBytes) {
    std::vector<uint8_t> v;
    v.push_back(0);  // offset 0: zero-length blob
    // ECMA-335 II.23.2: a compressed unsigned below 0x80 is one byte. Every
    // constant these tests use is a handful of bytes, so nothing wider is
    // needed -- and asserting it keeps a longer one from being encoded wrong.
    assert(constantBytes.size() < 0x80u);
    v.push_back(static_cast<uint8_t>(constantBytes.size()));
    v.insert(v.end(), constantBytes.begin(), constantBytes.end());
    return v;
}

// ─── #~ stream ───────────────────────────────────────────────────────────────

/// One Module row, two TypeDef rows (<Module> and C), one Field row (C.K), and
/// one Constant row attaching @p elementType / @p blobIdx to that field.
std::vector<uint8_t> buildTildeStream(uint8_t elementType, uint16_t blobIdx) {
    std::vector<uint8_t> v;
    putU32(v, 0);   // Reserved
    putU8(v, 2);    // MajorVersion
    putU8(v, 0);    // MinorVersion
    putU8(v, 0);    // HeapSizes = 0: #Strings, #GUID and #Blob indices are all
                    // two bytes wide, which is what every index below assumes.
    putU8(v, 1);    // Reserved2
    putU64(v, (1ULL << kTableModule) | (1ULL << kTableTypeDef)
             | (1ULL << kTableField) | (1ULL << kTableConstant));  // Valid
    putU64(v, 0);   // Sorted

    // Row counts, ascending table number.
    putU32(v, 1);   // Module
    putU32(v, 2);   // TypeDef
    putU32(v, 1);   // Field
    putU32(v, 1);   // Constant

    // Module: Generation(2) Name(str) Mvid(guid) EncId(guid) EncBaseId(guid)
    putU16(v, 0);
    putU16(v, kStrAsmName);
    putU16(v, 0); putU16(v, 0); putU16(v, 0);

    // TypeDef 1: <Module>, the pseudo-class every assembly carries.
    putU32(v, 0);            // Flags
    putU16(v, kStrModule);   // Name
    putU16(v, kStrEmpty);    // Namespace
    putU16(v, 0);            // Extends: coded TypeDefOrRef, index 0 = none
    putU16(v, 1);            // FieldList
    putU16(v, 1);            // MethodList

    // TypeDef 2: C, owning field 1.
    putU32(v, kTypeAttrPublic);
    putU16(v, kStrClass);
    putU16(v, kStrEmpty);
    putU16(v, 0);
    putU16(v, 1);            // FieldList -> Field row 1
    putU16(v, 1);            // MethodList (MethodDef table is absent)

    // Field 1: Flags(2) Name(str) Signature(blob)
    putU16(v, kFieldAttrPublic | kFieldAttrStatic | kFieldAttrLiteral
             | kFieldAttrHasDefault);
    putU16(v, kStrField);
    putU16(v, kBlobEmpty);

    // Constant 1: Type(1) padding(1) Parent(coded HasConstant) Value(blob)
    putU8(v, elementType);
    putU8(v, 0);
    putU16(v, static_cast<uint16_t>((1u << kHasConstantTagBits)
                                    | kHasConstantFieldTag));  // Field row 1
    putU16(v, blobIdx);

    return v;
}

// ─── metadata root ───────────────────────────────────────────────────────────

struct StreamSpec {
    std::string          name;
    std::vector<uint8_t> data;
};

/// ECMA-335 II.24.2.1 metadata root, followed by II.24.2.2 stream headers and
/// the stream bodies they point at. Stream offsets are relative to the root.
std::vector<uint8_t> buildMetadataRoot(const std::vector<StreamSpec>& streams) {
    // "v4.0.30319" padded to a 4-byte boundary, as the root's VersionLength is
    // required to be (II.24.2.1).
    const std::string version = "v4.0.30319";
    const size_t versionLen = (version.size() + 1 + 3) & ~static_cast<size_t>(3);

    size_t headerBytes = 16 + versionLen + 4;  // root + Flags + NumberOfStreams
    for (const auto& s : streams) {
        // Name is NUL-terminated and padded to 4 bytes; the header ahead of it
        // is Offset(4) + Size(4).
        headerBytes += 8 + ((s.name.size() + 4) & ~static_cast<size_t>(3));
    }

    std::vector<uint8_t> body;
    std::vector<uint32_t> offsets;
    for (const auto& s : streams) {
        while ((headerBytes + body.size()) % 4) body.push_back(0);
        offsets.push_back(static_cast<uint32_t>(headerBytes + body.size()));
        body.insert(body.end(), s.data.begin(), s.data.end());
    }

    std::vector<uint8_t> md;
    const char sig[4] = {'B', 'S', 'J', 'B'};
    md.insert(md.end(), sig, sig + 4);
    putU16(md, 1); putU16(md, 1);                       // Major/MinorVersion
    putU32(md, 0);                                      // Reserved
    putU32(md, static_cast<uint32_t>(versionLen));      // VersionLength
    for (char c : version) md.push_back(static_cast<uint8_t>(c));
    while (md.size() < 16 + versionLen) md.push_back(0);
    putU16(md, 0);                                      // Flags
    putU16(md, static_cast<uint16_t>(streams.size()));  // NumberOfStreams

    for (size_t i = 0; i < streams.size(); ++i) {
        putU32(md, offsets[i]);
        putU32(md, static_cast<uint32_t>(streams[i].data.size()));
        for (char c : streams[i].name) md.push_back(static_cast<uint8_t>(c));
        md.push_back(0);
        while (md.size() % 4) md.push_back(0);
    }

    // The stream offsets were computed from headerBytes before the header was
    // written, so the two must agree or every stream points at the wrong bytes.
    assert(headerBytes == md.size());
    md.insert(md.end(), body.begin(), body.end());
    return md;
}

// ─── the whole PE ────────────────────────────────────────────────────────────

/// A .NET PE carrying one `public const` field of type @p elementType whose
/// value is @p constantBytes, exactly as the #Blob would hold it.
std::vector<uint8_t> buildAssemblyWithConstant(
        ElementType elementType, const std::vector<uint8_t>& constantBytes) {
    constexpr size_t   kPeOff      = 0x80;
    constexpr size_t   kOptOff     = kPeOff + 4 + 20;
    constexpr size_t   kOptSize    = 224;
    constexpr size_t   kSectOff    = kOptOff + kOptSize;
    constexpr size_t   kSectionRaw = 0x200;
    constexpr uint32_t kSectionRva = 0x2000;
    constexpr uint32_t kCliRva     = kSectionRva;
    constexpr size_t   kCliHdrSize = 72;
    constexpr uint32_t kMdRva      = kSectionRva + kCliHdrSize;

    const std::vector<StreamSpec> streams = {
        {"#~",        buildTildeStream(static_cast<uint8_t>(elementType),
                                       kBlobConstant)},
        {"#Strings",  buildStringsHeap()},
        {"#Blob",     buildBlobHeap(constantBytes)},
    };
    const std::vector<uint8_t> md = buildMetadataRoot(streams);

    std::vector<uint8_t> buf(kSectionRaw, 0);
    auto put16 = [&buf](size_t off, uint16_t v) {
        buf[off] = static_cast<uint8_t>(v & 0xFF);
        buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto put32 = [&buf](size_t off, uint32_t v) {
        for (size_t i = 0; i < 4; ++i)
            buf[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    };

    buf[0] = 'M'; buf[1] = 'Z';
    put32(0x3C, static_cast<uint32_t>(kPeOff));
    put32(kPeOff, 0x00004550u);               // "PE\0\0"

    put16(kPeOff + 4 + 0,  0x014C);           // Machine = i386
    put16(kPeOff + 4 + 2,  1);                // NumberOfSections
    put16(kPeOff + 4 + 16, kOptSize);         // SizeOfOptionalHeader

    put16(kOptOff, 0x010B);                   // PE32 magic
    const size_t comOff = kOptOff + 96 + 14 * 8;  // data directory 14
    put32(comOff,     kCliRva);
    put32(comOff + 4, kCliHdrSize);

    std::memcpy(&buf[kSectOff], ".text\0\0", 7);
    put32(kSectOff + 12, kSectionRva);
    put32(kSectOff + 20, static_cast<uint32_t>(kSectionRaw));

    std::vector<uint8_t> body(kCliHdrSize, 0);
    auto bput32 = [&body](size_t off, uint32_t v) {
        for (size_t i = 0; i < 4; ++i)
            body[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    };
    bput32(0, kCliHdrSize);                   // cb
    body[4] = 2; body[6] = 5;                 // runtime version 2.5
    bput32(8,  kMdRva);                       // MetaData.rva
    bput32(12, static_cast<uint32_t>(md.size()));

    body.insert(body.end(), md.begin(), md.end());
    buf.insert(buf.end(), body.begin(), body.end());

    // The section must cover everything appended, or rvaToOffset refuses.
    put32(kSectOff + 16, static_cast<uint32_t>(buf.size() - kSectionRaw));
    return buf;
}

/// Read the assembly and hand back the single field of the single class.
///
/// Returns false with @p why set rather than asserting: the shim's ASSERT_*
/// macros expand to a `return`, so they cannot be used in a function that
/// returns a value.
bool readTheOnlyField(const std::vector<uint8_t>& buf, BcField& out,
                      std::string& why) {
    CLIReader reader;
    auto result = reader.read(buf.data(), buf.size(), "t.dll");
    if (!result.success) {
        why = "read failed: " + result.error;
        return false;
    }
    if (result.module.classes().size() != 1) {
        why = "expected 1 class, got "
              + std::to_string(result.module.classes().size());
        return false;
    }
    const auto& cls = result.module.classes()[0];
    if (cls.fields.size() != 1) {
        why = "expected 1 field, got " + std::to_string(cls.fields.size());
        return false;
    }
    out = cls.fields[0];
    return true;
}

/// An ElementType as its ECMA-335 II.23.1.16 spelling, so a table-driven
/// failure says which row it was.
std::string nameOf(ElementType t) {
    switch (t) {
    case ElementType::Boolean: return "Boolean";
    case ElementType::Char:    return "Char";
    case ElementType::I1:      return "I1";
    case ElementType::U1:      return "U1";
    case ElementType::I2:      return "I2";
    case ElementType::U2:      return "U2";
    case ElementType::I4:      return "I4";
    case ElementType::U4:      return "U4";
    case ElementType::I8:      return "I8";
    case ElementType::U8:      return "U8";
    case ElementType::R4:      return "R4";
    case ElementType::R8:      return "R8";
    case ElementType::String:  return "String";
    default:                   return "other";
    }
}

/// The float / double carrying @p bits as its IEEE-754 pattern.
///
/// The pattern is a host-order integer here and stays one: these two say
/// "the value whose bits are 0x3F800000", which is a statement about the
/// pattern, not about any byte order. The byte order under test is applied
/// separately, by writing the pattern into the #Blob with putU32 / putU64,
/// which spell little-endian explicitly.
float floatFromBits(uint32_t bits) {
    float f = 0;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}
double doubleFromBits(uint64_t bits) {
    double d = 0;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

/// The bytes of a string as a printable hex run, so a failure names them.
std::string hexOf(const std::string& s) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (!out.empty()) out.push_back(' ');
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 0x0F]);
    }
    return out;
}

} // namespace

// ─── the harness itself ──────────────────────────────────────────────────────

// If the synthetic assembly ever stops reaching the constant decoder, every
// test below would pass vacuously. This one fails instead.
TEST(CLIReaderConstantTest, TheSyntheticAssemblyReachesTheConstantDecoder) {
    BcField f;
    std::string why;
    // 0x2A as a four-byte I4: an ordinary constant with no edge case in it.
    ASSERT_TRUE(readTheOnlyField(
        buildAssemblyWithConstant(ElementType::I4, {0x2A, 0x00, 0x00, 0x00}),
        f, why)) << why;
    EXPECT_EQ("K", f.name);
    ASSERT_TRUE(f.constantIntValue.has_value());
    EXPECT_EQ(42, *f.constantIntValue);
}

// ─── cli_reader.cpp:776 — the UTF-16 surrogate pair ──────────────────────────

// The decoder took the three-byte `else` arm for every code unit at or above
// 0x800, surrogates included. ESBMC's witness for the arm as written was
// cu = 55296 = 0xD800, which it encoded as ED A0 80 -- not UTF-8, since a
// surrogate is not a scalar value. Here the two halves of one well-formed pair
// each took that arm, so U+1F600 came back as the six bytes
// ED A0 BD ED B8 80 rather than the four bytes F0 9F 98 80.
TEST(CLIReaderConstantTest, StringConstantSurrogatePairBecomesOneCharacter) {
    // U+1F600 GRINNING FACE = UTF-16 D83D DE00, little-endian in the #Blob.
    const std::vector<uint8_t> blob = {0x3D, 0xD8, 0x00, 0xDE};
    BcField f;
    std::string why;
    ASSERT_TRUE(readTheOnlyField(
        buildAssemblyWithConstant(ElementType::String, blob), f, why)) << why;
    ASSERT_TRUE(f.constantStrValue.has_value());

    const std::string expected = "\xF0\x9F\x98\x80";
    EXPECT_EQ(expected, *f.constantStrValue)
        << "got " << hexOf(*f.constantStrValue)
        << ", expected F0 9F 98 80 (U+1F600)";
    EXPECT_EQ(4u, f.constantStrValue->size());
}

// A lone high surrogate -- a truncated pair, which a corrupt or hostile #Blob
// supplies freely -- has no scalar value at all. It used to be re-emitted as
// ED A0 BD; U+FFFD is what a conforming decoder substitutes.
TEST(CLIReaderConstantTest, StringConstantLoneSurrogateBecomesReplacement) {
    const std::vector<uint8_t> blob = {0x3D, 0xD8};  // D83D with no low half
    BcField f;
    std::string why;
    ASSERT_TRUE(readTheOnlyField(
        buildAssemblyWithConstant(ElementType::String, blob), f, why)) << why;
    ASSERT_TRUE(f.constantStrValue.has_value());

    const std::string expected = "\xEF\xBF\xBD";  // U+FFFD
    EXPECT_EQ(expected, *f.constantStrValue)
        << "got " << hexOf(*f.constantStrValue) << ", expected EF BF BD (U+FFFD)";
}

// A lone LOW surrogate is the other half of the same defect: the old arm
// produced ED B8 80 for DE00.
TEST(CLIReaderConstantTest, StringConstantLoneLowSurrogateBecomesReplacement) {
    const std::vector<uint8_t> blob = {0x00, 0xDE};  // DE00 with no high half
    BcField f;
    std::string why;
    ASSERT_TRUE(readTheOnlyField(
        buildAssemblyWithConstant(ElementType::String, blob), f, why)) << why;
    ASSERT_TRUE(f.constantStrValue.has_value());
    EXPECT_EQ(std::string("\xEF\xBF\xBD"), *f.constantStrValue)
        << "got " << hexOf(*f.constantStrValue);
}

// Off the surrogates the kernel is proved byte-identical to the loop it
// replaced (proof_the_kernel_matches_dotnet_off_the_surrogates). These pin that
// the routing did not change anything it was not supposed to: one unit from
// each of the three BMP widths, plus the trailing odd byte, which the old
// `i + 1 < blob.size()` bound dropped and the kernel's `inBytes >> 1` unit
// count drops too.
TEST(CLIReaderConstantTest, StringConstantNonSurrogatesAreUnchanged) {
    struct Case { std::vector<uint8_t> blob; std::string expected; const char* what; };
    const Case cases[] = {
        {{'A', 0x00}, "A", "U+0041, one UTF-8 byte"},
        {{0xE9, 0x00}, "\xC3\xA9", "U+00E9, two UTF-8 bytes"},
        {{0xAC, 0x20}, "\xE2\x82\xAC", "U+20AC, three UTF-8 bytes"},
        {{'h', 0x00, 'i', 0x00}, "hi", "two units"},
        {{'h', 0x00, 'i', 0x00, 0x21}, "hi", "trailing half unit is dropped"},
        {{0xFF, 0xFF}, "\xEF\xBF\xBF", "U+FFFF, not a surrogate"},
    };
    for (const auto& c : cases) {
        BcField f;
        std::string why;
        ASSERT_TRUE(readTheOnlyField(
            buildAssemblyWithConstant(ElementType::String, c.blob), f, why))
            << c.what << ": " << why;
        ASSERT_TRUE(f.constantStrValue.has_value()) << c.what;
        EXPECT_EQ(c.expected, *f.constantStrValue)
            << c.what << ": got " << hexOf(*f.constantStrValue);
    }
}

// ─── fieldConstantInt — readSignedLE / readUnsignedLE ────────────────────────
//
// What each of the two tests below is worth, stated so neither is mistaken for
// the other:
//
// FixedWidthConstantsKeepTheirValues pins the value each ElementType decodes
// to, at both signs and at every width ECMA-335 II.23.1.16 defines. Two of its
// arms are a wrong-value replay and the rest are characterization: the Char
// rows at 0xFFFF and 0x8000 fail against the arm as it stood, which
// sign-extended a UTF-16 code unit, and so do the Boolean rows at 0x80 and
// 0xFF, which used to share I1's sign-extending arm. The rest pin values that
// the move onto byteorder::readLE was not supposed to change, and did not.
//
// TruncatedFixedWidthConstantYieldsNoValue is a replay of the ESBMC witness
// for the hand-rolled readSignedLE -- a blob shorter than n, whose sign test
// then read b[n - 1] past the end. That witness used to be unreachable from
// here because every arm pre-checked `blob.size() >= n` before calling, which
// is exactly what made the test vacuous: it exercised the call site's guard,
// not the kernel's. The arms no longer pre-check -- byteorder::readLE owns the
// bound now -- so the short blobs below reach the read, and restoring the
// hand-rolled accumulate makes every one of the ten rows report a value it
// assembled without the bytes to assemble it from.

TEST(CLIReaderConstantTest, FixedWidthConstantsKeepTheirValues) {
    struct Case { ElementType ty; std::vector<uint8_t> blob; int64_t expected; };
    const Case cases[] = {
        // I2 / U2: the same two bytes, read signed and unsigned.
        {ElementType::I2, {0xFF, 0xFF}, -1},
        {ElementType::I2, {0x00, 0x80}, -32768},
        {ElementType::U2, {0xFF, 0xFF}, 65535},
        // Char reads the same two bytes UNSIGNED -- see
        // CharConstantIsAnUnsignedCodeUnit below for why 0x41 alone would not
        // have shown which of the two readings this arm takes.
        {ElementType::Char, {0x41, 0x00}, 65},
        {ElementType::Char, {0xFF, 0xFF}, 65535},
        {ElementType::Char, {0x00, 0x80}, 32768},
        // I4 / U4.
        {ElementType::I4, {0xFF, 0xFF, 0xFF, 0xFF}, -1},
        {ElementType::I4, {0x00, 0x00, 0x00, 0x80}, INT32_MIN},
        {ElementType::U4, {0xFF, 0xFF, 0xFF, 0xFF}, 4294967295LL},
        // I8: the width at which the old `~0ull << (8 * n)` would have been an
        // undefined 64-bit shift had the `n < 8` guard not stood in front of it.
        {ElementType::I8,
         {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, -1},
        {ElementType::I8,
         {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, INT64_MIN},
        // One-byte types. I1 is the only signed one: U1 is unsigned by
        // ECMA-335 II.23.1.16, and bool is listed with the unsigned built-ins
        // in I.12.1. Boolean used to share I1's sign-extending arm, so the
        // 0xFF row below reported -1 for a constant whose type has no negative
        // value; 0x80 is the lowest byte at which the two readings differ.
        {ElementType::I1, {0x80}, -128},
        {ElementType::U1, {0x80}, 128},
        {ElementType::Boolean, {0x00}, 0},
        {ElementType::Boolean, {0x01}, 1},
        {ElementType::Boolean, {0x80}, 128},
        {ElementType::Boolean, {0xFF}, 255},
    };
    for (const auto& c : cases) {
        BcField f;
        std::string why;
        ASSERT_TRUE(readTheOnlyField(buildAssemblyWithConstant(c.ty, c.blob), f,
                                     why))
            << nameOf(c.ty) << ": " << why;
        ASSERT_TRUE(f.constantIntValue.has_value()) << nameOf(c.ty);
        EXPECT_EQ(c.expected, *f.constantIntValue) << nameOf(c.ty);
    }
}

// ─── fieldConstantInt, the Char arm — signed vs unsigned extension ───────────

// ELEMENT_TYPE_CHAR is a UTF-16 code unit (ECMA-335 II.23.1.16): two bytes,
// UNSIGNED. It used to share the `case ElementType::I2:` arm, which
// sign-extends, so every code unit at or above U+8000 decoded negative --
// `const char c = '\uFFFF';` came back as -1 rather than 65535.
//
// {Char, {0x41, 0x00}, 65} was Char's only row until this test was added, and
// it could not have seen the defect: U+0041 is ASCII, and below U+8000 the
// signed and unsigned readings of the same two bytes are equal. Three of the
// four rows here are at or above U+8000, which is the only region where the
// two readings differ at all; the fourth is below it, to pin that the change
// left that region alone.
TEST(CLIReaderConstantTest, CharConstantIsAnUnsignedCodeUnit) {
    struct Case { std::vector<uint8_t> blob; int64_t expected; const char* what; };
    const Case cases[] = {
        {{0x00, 0x80}, 32768, "U+8000, the lowest unit with the top bit set"},
        {{0xFF, 0xFF}, 65535, "U+FFFF, the highest UTF-16 code unit"},
        {{0x3D, 0xD8}, 55357, "U+D83D, a lone high surrogate is still a unit"},
        {{0xAC, 0x20}, 8364,  "U+20AC, below the sign bit: unchanged"},
    };
    for (const auto& c : cases) {
        BcField f;
        std::string why;
        ASSERT_TRUE(readTheOnlyField(
            buildAssemblyWithConstant(ElementType::Char, c.blob), f, why))
            << c.what << ": " << why;
        ASSERT_TRUE(f.constantIntValue.has_value()) << c.what;
        EXPECT_EQ(c.expected, *f.constantIntValue)
            << c.what << " -- a Char is unsigned, so no #Blob code unit "
            << "decodes negative";
        EXPECT_GE(*f.constantIntValue, 0) << c.what;
    }
}

// ─── fieldConstantFloat — the R4 and R8 arms ─────────────────────────────────
//
// HONEST LABEL, because it matters for what these are worth: the defect they
// guard is a host-endian read of a little-endian datum, and it cannot be
// observed on a little-endian host. Restoring the
// `std::memcpy(&f, blob.data(), 4)` these arms used to be leaves every row
// below passing on x86-64; it fails them on a big-endian host, which is the
// whole content of the bug. So this is a characterization test with a stated
// blind spot, not a counterexample replay.
//
// What it does pin, on every host: the #Blob spelling is little-endian
// (ECMA-335 II.22.9) -- the pattern goes in through putU32/putU64, which shift
// explicitly -- and the value that comes back is the float carrying exactly
// that pattern. The two are stated separately so neither can absorb an error
// in the other.
TEST(CLIReaderConstantTest, FloatConstantsAreReadLittleEndian) {
    struct Case { uint32_t bits; const char* what; };
    const Case r4[] = {
        {0x3F800000u, "1.0f"},
        {0xBF800000u, "-1.0f"},
        {0x40490FDBu, "float pi"},
        // Every byte distinct, so a swapped read cannot coincide with a
        // correct one; and the pattern read backwards (0x44332211) is a
        // perfectly ordinary float, so a wrong answer would not look wrong.
        {0x11223344u, "0x11223344, a pattern with four distinct bytes"},
        // 0x00000001 read backwards is 0x01000000: a denormal 1.4e-45 against
        // a normal 2.35e-38.
        {0x00000001u, "the smallest positive denormal"},
        {0x00000000u, "+0.0f"},
    };
    for (const auto& c : r4) {
        std::vector<uint8_t> blob;
        putU32(blob, c.bits);  // ECMA-335 II.22.9: little-endian in the #Blob.
        BcField f;
        std::string why;
        ASSERT_TRUE(readTheOnlyField(
            buildAssemblyWithConstant(ElementType::R4, blob), f, why))
            << c.what << ": " << why;
        ASSERT_TRUE(f.constantFltValue.has_value()) << c.what;
        EXPECT_EQ(static_cast<double>(floatFromBits(c.bits)), *f.constantFltValue)
            << c.what << ": R4 blob holds the four bytes of " << std::hex
            << c.bits << " little-endian";
        // An R4 is not an integer constant; the int decoder must decline it.
        EXPECT_FALSE(f.constantIntValue.has_value()) << c.what;
    }

    struct Case8 { uint64_t bits; const char* what; };
    const Case8 r8[] = {
        {0x3FF0000000000000ull, "1.0"},
        {0xBFF0000000000000ull, "-1.0"},
        {0x400921FB54442D18ull, "double pi"},
        {0x0102030405060708ull, "eight distinct bytes"},
        {0x0000000000000001ull, "the smallest positive denormal"},
    };
    for (const auto& c : r8) {
        std::vector<uint8_t> blob;
        putU64(blob, c.bits);
        BcField f;
        std::string why;
        ASSERT_TRUE(readTheOnlyField(
            buildAssemblyWithConstant(ElementType::R8, blob), f, why))
            << c.what << ": " << why;
        ASSERT_TRUE(f.constantFltValue.has_value()) << c.what;
        EXPECT_EQ(doubleFromBits(c.bits), *f.constantFltValue)
            << c.what << ": R8 blob holds the eight bytes of " << std::hex
            << c.bits << " little-endian";
        EXPECT_FALSE(f.constantIntValue.has_value()) << c.what;
    }
}

// A float constant shorter than the width its ElementType names carries no
// value. The refusal is byteorder::readLE's now -- rangeFits(0, size, n) --
// rather than a `blob.size() >= 4` spelled at the arm, so this pins that the
// move did not widen what the decoder accepts.
TEST(CLIReaderConstantTest, TruncatedFloatConstantYieldsNoValue) {
    struct Case { ElementType ty; std::vector<uint8_t> blob; };
    const Case cases[] = {
        {ElementType::R4, {}},
        {ElementType::R4, {0x00}},
        {ElementType::R4, {0x00, 0x00, 0x80}},
        {ElementType::R8, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0}},
    };
    for (const auto& c : cases) {
        BcField f;
        std::string why;
        ASSERT_TRUE(readTheOnlyField(buildAssemblyWithConstant(c.ty, c.blob), f,
                                     why))
            << nameOf(c.ty) << ": " << why;
        EXPECT_FALSE(f.constantFltValue.has_value())
            << nameOf(c.ty) << " decoded a value from " << c.blob.size()
            << " bytes";
    }
}

// A blob shorter than the width its ElementType names carries no constant. The
// answer has to be "no value", not a value assembled from whatever bytes were
// there -- and the refusal is byteorder::readLE's, since the arms no longer
// carry a `blob.size() >= n` of their own. Every row here decodes a value
// against the hand-rolled accumulate that preceded the kernel routing, so the
// test fails when that routing is undone instead of passing either way.
TEST(CLIReaderConstantTest, TruncatedFixedWidthConstantYieldsNoValue) {
    struct Case { ElementType ty; std::vector<uint8_t> blob; };
    const Case cases[] = {
        // The one-byte types against a zero-length blob: the arm has no byte
        // to read and must say so rather than read one.
        {ElementType::Boolean, {}},
        {ElementType::I1, {}},
        {ElementType::U1, {}},
        // Two, four and eight bytes short by one or more. The 1-byte blob at
        // I2 is the shape of the ESBMC witness for the hand-rolled
        // readSignedLE: the accumulate stopped at b.size() and the sign test
        // that followed it did not, so it indexed b[n - 1] past the end and
        // returned a value assembled from bytes it never had.
        {ElementType::I2, {0x01}},
        {ElementType::U2, {0x01}},
        {ElementType::Char, {0x01}},
        {ElementType::I4, {0x01, 0x02, 0x03}},
        {ElementType::U4, {0x01, 0x02, 0x03}},
        {ElementType::I8, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}},
        {ElementType::U8, {0x01}},
    };
    for (const auto& c : cases) {
        BcField f;
        std::string why;
        ASSERT_TRUE(readTheOnlyField(buildAssemblyWithConstant(c.ty, c.blob), f,
                                     why))
            << nameOf(c.ty) << ": " << why;
        EXPECT_FALSE(f.constantIntValue.has_value())
            << nameOf(c.ty) << " decoded a value from "
            << c.blob.size() << " bytes";
    }
}
