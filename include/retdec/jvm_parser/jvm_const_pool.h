/**
 * @file include/retdec/jvm_parser/jvm_const_pool.h
 * @brief JVM Constant Pool — all 18 entry kinds per JVMS §4.4.
 *
 * Covers Java 1.0 through Java 21 (class file versions 45–65).
 *
 * ## Entry kinds
 *   1  Utf8                §4.4.7
 *   3  Integer             §4.4.4
 *   4  Float               §4.4.4
 *   5  Long                §4.4.5
 *   6  Double              §4.4.5
 *   7  Class               §4.4.1
 *   8  String              §4.4.3
 *   9  Fieldref            §4.4.2
 *  10  Methodref           §4.4.2
 *  11  InterfaceMethodref  §4.4.2
 *  12  NameAndType         §4.4.6
 *  15  MethodHandle        §4.4.8
 *  16  MethodType          §4.4.9
 *  17  Dynamic             §4.4.10  (Java 11+)
 *  18  InvokeDynamic       §4.4.10
 *  19  Module              §4.4.11  (Java 9+)
 *  20  Package             §4.4.12  (Java 9+)
 *
 * ## Usage
 *
 * `ConstPool::read(reader)` parses the full pool from a binary stream and
 * caches resolved strings/references so that callers do not re-parse.
 */

#ifndef RETDEC_JVM_PARSER_JVM_CONST_POOL_H
#define RETDEC_JVM_PARSER_JVM_CONST_POOL_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace jvm_parser {

// ─── Binary reader ────────────────────────────────────────────────────────────

/**
 * @brief Minimal big-endian binary reader over a byte span.
 *
 * Throws `std::out_of_range` on buffer overrun.
 */
class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    size_t pos()  const { return pos_; }
    size_t size() const { return size_; }
    size_t remaining() const { return size_ - pos_; }

    uint8_t  u1();
    uint16_t u2();
    uint32_t u4();
    uint64_t u8();
    int32_t  s4();
    int64_t  s8();
    float    f4();
    double   f8();
    std::string utf8(uint16_t len);
    std::string mutf8(uint16_t len);  ///< Modified UTF-8 (JVM encoding)

    void skip(size_t n);
    std::vector<uint8_t> bytes(size_t n);

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;

    void check(size_t n) const;
};

// ─── Constant pool tag constants ─────────────────────────────────────────────

enum class CpTag : uint8_t {
    Utf8               =  1,
    Integer            =  3,
    Float              =  4,
    Long               =  5,
    Double             =  6,
    Class              =  7,
    String             =  8,
    Fieldref           =  9,
    Methodref          = 10,
    InterfaceMethodref = 11,
    NameAndType        = 12,
    MethodHandle       = 15,
    MethodType         = 16,
    Dynamic            = 17,
    InvokeDynamic      = 18,
    Module             = 19,
    Package            = 20,
};

// ─── Constant pool entry structs ─────────────────────────────────────────────

struct CpUtf8    { std::string value; };
struct CpInt     { int32_t  value = 0; };
struct CpFloat   { float    value = 0.f; };
struct CpLong    { int64_t  value = 0; };
struct CpDouble  { double   value = 0.0; };
struct CpClass   { uint16_t nameIndex = 0; };
struct CpString  { uint16_t utf8Index = 0; };

/// Common layout for Fieldref / Methodref / InterfaceMethodref.
struct CpRef { uint16_t classIndex = 0; uint16_t nameAndTypeIndex = 0; };
struct CpFieldref           { uint16_t classIndex = 0; uint16_t nameAndTypeIndex = 0; };
struct CpMethodref          { uint16_t classIndex = 0; uint16_t nameAndTypeIndex = 0; };
struct CpInterfaceMethodref { uint16_t classIndex = 0; uint16_t nameAndTypeIndex = 0; };

struct CpNameAndType {
    uint16_t nameIndex       = 0;
    uint16_t descriptorIndex = 0;
};

struct CpMethodHandle {
    uint8_t  referenceKind  = 0;  ///< 1–9, see JVMS Table 4.4-C
    uint16_t referenceIndex = 0;
};

struct CpMethodType {
    uint16_t descriptorIndex = 0;
};

struct CpDynamic {
    uint16_t bootstrapMethodAttrIndex = 0;
    uint16_t nameAndTypeIndex         = 0;
};
struct CpInvokeDynamic { uint16_t bootstrapMethodAttrIndex = 0; uint16_t nameAndTypeIndex = 0; };

struct CpModule  { uint16_t nameIndex = 0; };
struct CpPackage { uint16_t nameIndex = 0; };

/** Placeholder for the second slot of Long/Double (JVMS §4.4.5). */
struct CpLongDoubleHigh {};

using CpEntry = std::variant<
    CpUtf8, CpInt, CpFloat, CpLong, CpDouble,
    CpClass, CpString,
    CpFieldref, CpMethodref, CpInterfaceMethodref,
    CpNameAndType, CpMethodHandle, CpMethodType,
    CpDynamic, CpInvokeDynamic,
    CpModule, CpPackage,
    CpLongDoubleHigh   // slot filler for indices after Long/Double
>;

// ─── ConstPool ────────────────────────────────────────────────────────────────

/**
 * @brief Parsed JVM constant pool.
 *
 * 1-indexed (index 0 is unused, per JVMS).
 * Long and Double occupy two slots; slot n+1 has a `CpLongDoubleHigh` filler.
 */
class ConstPool {
public:
    /// Read and parse a constant pool from a binary stream.
    /// `count` is the cp_count value from the ClassFile header (= actual size + 1).
    static ConstPool read(BinaryReader& r);

    /// Number of valid entries (1-based indices 1 … size()).
    size_t size() const { return entries_.size(); }

    /// Raw entry access (1-based).
    const CpEntry& entry(uint16_t idx) const;

    /// Resolved string helpers.
    const std::string& utf8(uint16_t idx) const;          ///< Must be Utf8 entry
    std::string        className(uint16_t classIdx) const; ///< Class → name (slash-separated)
    std::string        string(uint16_t stringIdx) const;   ///< String → utf8 value
    std::string        descriptor(uint16_t natIdx) const;  ///< NameAndType → descriptor
    std::string        name(uint16_t natIdx) const;        ///< NameAndType → name
    std::string        refClass(uint16_t refIdx) const;    ///< Fieldref/etc → class name
    std::string        refName(uint16_t refIdx) const;     ///< Fieldref/etc → member name
    std::string        refDescriptor(uint16_t refIdx) const; ///< Fieldref/etc → descriptor

    /// Tag of an entry.
    CpTag tag(uint16_t idx) const;

    /// Check tag.
    bool is(uint16_t idx, CpTag t) const;

private:
    std::vector<CpEntry> entries_; // entries_[0] unused; entries_[1..n] valid.
};

// ─── Parse error ─────────────────────────────────────────────────────────────

class JvmParseError : public std::runtime_error {
public:
    explicit JvmParseError(const std::string& msg)
        : std::runtime_error("JVM parse error: " + msg) {}
};

} // namespace jvm_parser
} // namespace retdec

#endif // RETDEC_JVM_PARSER_JVM_CONST_POOL_H
