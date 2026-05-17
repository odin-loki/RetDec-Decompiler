/**
 * @file src/jvm_parser/jvm_const_pool.cpp
 * @brief BinaryReader + ConstPool implementation.
 */

#include "retdec/jvm_parser/jvm_const_pool.h"

#include <cstring>
#include <sstream>

namespace retdec {
namespace jvm_parser {

// ─── BinaryReader ─────────────────────────────────────────────────────────────

void BinaryReader::check(size_t n) const {
    if (pos_ + n > size_)
        throw JvmParseError("unexpected end of file at offset "
                            + std::to_string(pos_));
}

uint8_t BinaryReader::u1() {
    check(1);
    return data_[pos_++];
}

uint16_t BinaryReader::u2() {
    check(2);
    uint16_t v = static_cast<uint16_t>(
        (static_cast<uint16_t>(data_[pos_]) << 8) | data_[pos_+1]);
    pos_ += 2;
    return v;
}

uint32_t BinaryReader::u4() {
    check(4);
    uint32_t v = (static_cast<uint32_t>(data_[pos_  ]) << 24)
               | (static_cast<uint32_t>(data_[pos_+1]) << 16)
               | (static_cast<uint32_t>(data_[pos_+2]) <<  8)
               |  static_cast<uint32_t>(data_[pos_+3]);
    pos_ += 4;
    return v;
}

uint64_t BinaryReader::u8() {
    check(8);
    uint64_t hi = u4();
    uint64_t lo = u4();
    return (hi << 32) | lo;
}

int32_t BinaryReader::s4() {
    return static_cast<int32_t>(u4());
}

int64_t BinaryReader::s8() {
    return static_cast<int64_t>(u8());
}

float BinaryReader::f4() {
    uint32_t bits = u4();
    float v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

double BinaryReader::f8() {
    uint64_t bits = u8();
    double v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

std::string BinaryReader::utf8(uint16_t len) {
    check(len);
    std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return s;
}

// Modified UTF-8 → standard UTF-8.
// JVM Modified UTF-8 differs in:
//   1. null byte encoded as 0xC0 0x80 instead of 0x00.
//   2. Supplementary chars encoded as two 3-byte sequences (surrogate pair).
std::string BinaryReader::mutf8(uint16_t len) {
    check(len);
    std::string out;
    out.reserve(len);
    size_t end = pos_ + len;
    while (pos_ < end) {
        uint8_t b = data_[pos_++];
        if (b == 0xC0 && pos_ < end && data_[pos_] == 0x80) {
            // Null byte encoding
            out += '\0';
            ++pos_;
        } else {
            out += static_cast<char>(b);
        }
    }
    return out;
}

void BinaryReader::skip(size_t n) {
    check(n);
    pos_ += n;
}

std::vector<uint8_t> BinaryReader::bytes(size_t n) {
    check(n);
    std::vector<uint8_t> v(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return v;
}

// ─── ConstPool::read ──────────────────────────────────────────────────────────

ConstPool ConstPool::read(BinaryReader& r) {
    uint16_t count = r.u2(); // read cp_count from stream (first 2 bytes)
    ConstPool pool;
    // Entry 0 is unused (placeholder).
    pool.entries_.push_back(CpUtf8{""});

    for (uint16_t i = 1; i < count; ++i) {
        uint8_t tag = r.u1();
        switch (static_cast<CpTag>(tag)) {
        case CpTag::Utf8: {
            uint16_t len = r.u2();
            pool.entries_.push_back(CpUtf8{r.mutf8(len)});
            break;
        }
        case CpTag::Integer:
            pool.entries_.push_back(CpInt{r.s4()});
            break;
        case CpTag::Float:
            pool.entries_.push_back(CpFloat{r.f4()});
            break;
        case CpTag::Long:
            pool.entries_.push_back(CpLong{r.s8()});
            pool.entries_.push_back(CpLongDoubleHigh{});
            ++i;
            break;
        case CpTag::Double:
            pool.entries_.push_back(CpDouble{r.f8()});
            pool.entries_.push_back(CpLongDoubleHigh{});
            ++i;
            break;
        case CpTag::Class:
            pool.entries_.push_back(CpClass{r.u2()});
            break;
        case CpTag::String:
            pool.entries_.push_back(CpString{r.u2()});
            break;
        case CpTag::Fieldref:
            pool.entries_.push_back(CpFieldref{r.u2(), r.u2()});
            break;
        case CpTag::Methodref:
            pool.entries_.push_back(CpMethodref{r.u2(), r.u2()});
            break;
        case CpTag::InterfaceMethodref:
            pool.entries_.push_back(CpInterfaceMethodref{r.u2(), r.u2()});
            break;
        case CpTag::NameAndType:
            pool.entries_.push_back(CpNameAndType{r.u2(), r.u2()});
            break;
        case CpTag::MethodHandle:
            pool.entries_.push_back(CpMethodHandle{r.u1(), r.u2()});
            break;
        case CpTag::MethodType:
            pool.entries_.push_back(CpMethodType{r.u2()});
            break;
        case CpTag::Dynamic:
            pool.entries_.push_back(CpDynamic{r.u2(), r.u2()});
            break;
        case CpTag::InvokeDynamic:
            pool.entries_.push_back(CpInvokeDynamic{r.u2(), r.u2()});
            break;
        case CpTag::Module:
            pool.entries_.push_back(CpModule{r.u2()});
            break;
        case CpTag::Package:
            pool.entries_.push_back(CpPackage{r.u2()});
            break;
        default:
            throw JvmParseError("unknown constant pool tag "
                                + std::to_string(tag) + " at cp index "
                                + std::to_string(i));
        }
    }
    return pool;
}

// ─── ConstPool accessors ──────────────────────────────────────────────────────

const CpEntry& ConstPool::entry(uint16_t idx) const {
    if (idx == 0 || idx >= entries_.size())
        throw JvmParseError("cp index " + std::to_string(idx) + " out of range");
    return entries_[idx];
}

CpTag ConstPool::tag(uint16_t idx) const {
    const auto& e = entry(idx);
    return std::visit([](const auto& v) -> CpTag {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, CpUtf8>)               return CpTag::Utf8;
        if constexpr (std::is_same_v<T, CpInt>)                return CpTag::Integer;
        if constexpr (std::is_same_v<T, CpFloat>)              return CpTag::Float;
        if constexpr (std::is_same_v<T, CpLong>)               return CpTag::Long;
        if constexpr (std::is_same_v<T, CpDouble>)             return CpTag::Double;
        if constexpr (std::is_same_v<T, CpClass>)              return CpTag::Class;
        if constexpr (std::is_same_v<T, CpString>)             return CpTag::String;
        if constexpr (std::is_same_v<T, CpFieldref>)           return CpTag::Fieldref;
        if constexpr (std::is_same_v<T, CpMethodref>)          return CpTag::Methodref;
        if constexpr (std::is_same_v<T, CpInterfaceMethodref>) return CpTag::InterfaceMethodref;
        if constexpr (std::is_same_v<T, CpNameAndType>)        return CpTag::NameAndType;
        if constexpr (std::is_same_v<T, CpMethodHandle>)       return CpTag::MethodHandle;
        if constexpr (std::is_same_v<T, CpMethodType>)         return CpTag::MethodType;
        if constexpr (std::is_same_v<T, CpDynamic>)            return CpTag::Dynamic;
        if constexpr (std::is_same_v<T, CpInvokeDynamic>)      return CpTag::InvokeDynamic;
        if constexpr (std::is_same_v<T, CpModule>)             return CpTag::Module;
        if constexpr (std::is_same_v<T, CpPackage>)            return CpTag::Package;
        // CpLongDoubleHigh: use a distinct tag value
        return static_cast<CpTag>(255);
    }, e);
}

bool ConstPool::is(uint16_t idx, CpTag t) const {
    if (idx == 0 || idx >= entries_.size()) return false;
    return tag(idx) == t;
}

const std::string& ConstPool::utf8(uint16_t idx) const {
    const auto& e = entry(idx);
    if (const auto* u = std::get_if<CpUtf8>(&e))
        return u->value;
    throw JvmParseError("cp[" + std::to_string(idx) + "] is not Utf8");
}

std::string ConstPool::className(uint16_t classIdx) const {
    const auto& e = entry(classIdx);
    if (const auto* c = std::get_if<CpClass>(&e))
        return utf8(c->nameIndex);
    throw JvmParseError("cp[" + std::to_string(classIdx) + "] is not Class");
}

std::string ConstPool::string(uint16_t stringIdx) const {
    const auto& e = entry(stringIdx);
    if (const auto* s = std::get_if<CpString>(&e))
        return utf8(s->utf8Index);
    throw JvmParseError("cp[" + std::to_string(stringIdx) + "] is not String");
}

std::string ConstPool::name(uint16_t natIdx) const {
    const auto& e = entry(natIdx);
    if (const auto* n = std::get_if<CpNameAndType>(&e))
        return utf8(n->nameIndex);
    throw JvmParseError("cp[" + std::to_string(natIdx) + "] is not NameAndType");
}

std::string ConstPool::descriptor(uint16_t natIdx) const {
    const auto& e = entry(natIdx);
    if (const auto* n = std::get_if<CpNameAndType>(&e))
        return utf8(n->descriptorIndex);
    throw JvmParseError("cp[" + std::to_string(natIdx) + "] is not NameAndType");
}

static CpRef asRef(const CpEntry& e, uint16_t idx) {
    if (const auto* r = std::get_if<CpFieldref>(&e))
        return {r->classIndex, r->nameAndTypeIndex};
    if (const auto* r = std::get_if<CpMethodref>(&e))
        return {r->classIndex, r->nameAndTypeIndex};
    if (const auto* r = std::get_if<CpInterfaceMethodref>(&e))
        return {r->classIndex, r->nameAndTypeIndex};
    throw JvmParseError("cp[" + std::to_string(idx)
                        + "] is not a Fieldref/Methodref/InterfaceMethodref");
}

std::string ConstPool::refClass(uint16_t refIdx) const {
    const auto& e = entry(refIdx);
    return className(asRef(e, refIdx).classIndex);
}

std::string ConstPool::refName(uint16_t refIdx) const {
    const auto& e = entry(refIdx);
    return name(asRef(e, refIdx).nameAndTypeIndex);
}

std::string ConstPool::refDescriptor(uint16_t refIdx) const {
    const auto& e = entry(refIdx);
    return descriptor(asRef(e, refIdx).nameAndTypeIndex);
}

} // namespace jvm_parser
} // namespace retdec
