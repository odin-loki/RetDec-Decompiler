/**
 * @file src/dex_parser/dex_header.cpp
 * @brief DEX header parsing, binary reader, index table loading.
 */

#include "retdec/dex_parser/dex_header.h"

#include <cstring>
#include <sstream>

namespace retdec {
namespace dex_parser {

// ─── DexReader ────────────────────────────────────────────────────────────────

DexReader::DexReader(const uint8_t* data, size_t size)
    : data_(data), size_(size), pos_(0) {}

void DexReader::seek(size_t offset) {
    if (offset > size_)
        throw DexParseError("seek past end of file (" +
                            std::to_string(offset) + " > " +
                            std::to_string(size_) + ")");
    pos_ = offset;
}

void DexReader::skip(size_t n) {
    check(n);
    pos_ += n;
}

void DexReader::check(size_t n) const {
    if (pos_ + n > size_)
        throw DexParseError("unexpected end of data (need " +
                            std::to_string(n) + " bytes at offset " +
                            std::to_string(pos_) + ", file size " +
                            std::to_string(size_) + ")");
}

uint8_t DexReader::u1() {
    check(1);
    return data_[pos_++];
}

uint16_t DexReader::u2() {
    check(2);
    uint16_t v = static_cast<uint16_t>(data_[pos_]) |
                 (static_cast<uint16_t>(data_[pos_+1]) << 8);
    pos_ += 2;
    return v;
}

uint32_t DexReader::u4() {
    check(4);
    uint32_t v = static_cast<uint32_t>(data_[pos_])       |
                (static_cast<uint32_t>(data_[pos_+1]) << 8) |
                (static_cast<uint32_t>(data_[pos_+2]) << 16) |
                (static_cast<uint32_t>(data_[pos_+3]) << 24);
    pos_ += 4;
    return v;
}

uint64_t DexReader::u8() {
    uint64_t lo = u4();
    uint64_t hi = u4();
    return lo | (hi << 32);
}

int8_t  DexReader::s1() { return static_cast<int8_t>(u1()); }
int16_t DexReader::s2() { return static_cast<int16_t>(u2()); }
int32_t DexReader::s4() { return static_cast<int32_t>(u4()); }
int64_t DexReader::s8() { return static_cast<int64_t>(u8()); }

float DexReader::f4() {
    uint32_t bits = u4();
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

double DexReader::f8() {
    uint64_t bits = u8();
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

uint32_t DexReader::uleb128() {
    uint32_t result = 0;
    uint32_t shift  = 0;
    for (;;) {
        uint8_t b = u1();
        result |= (static_cast<uint32_t>(b & 0x7F) << shift);
        if ((b & 0x80) == 0)
            break;
        shift += 7;
        if (shift >= 35)
            throw DexParseError("ULEB128 overflow");
    }
    return result;
}

int32_t DexReader::sleb128() {
    int32_t  result = 0;
    uint32_t shift  = 0;
    uint8_t  b      = 0;
    for (;;) {
        b = u1();
        result |= (static_cast<int32_t>(b & 0x7F) << shift);
        shift += 7;
        if ((b & 0x80) == 0)
            break;
        if (shift >= 35)
            throw DexParseError("SLEB128 overflow");
    }
    // Sign extend
    if ((b & 0x40) && shift < 32)
        result |= -(1 << shift);
    return result;
}

int32_t DexReader::uleb128p1() {
    return static_cast<int32_t>(uleb128()) - 1;
}

std::string DexReader::mutf8(uint32_t len) {
    // DEX strings are MUTF-8 encoded (like JVM).
    // We do a basic decode — surrogate pairs for U+10000+ are not common in DEX.
    std::string result;
    result.reserve(len);
    for (uint32_t i = 0; i < len; ) {
        uint8_t c = u1();
        if (c == 0) {
            // Modified UTF-8 encodes NUL as 0xC0 0x80, but raw NUL is also used
            // in some DEX files. We tolerate both.
            result += '\0';
            ++i;
        } else if (c < 0x80) {
            result += static_cast<char>(c);
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            uint8_t c2 = u1();
            uint32_t cp = ((c & 0x1F) << 6) | (c2 & 0x3F);
            // Re-encode as UTF-8
            if (cp < 0x80) { result += static_cast<char>(cp); }
            else {
                result += static_cast<char>(0xC0 | (cp >> 6));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            }
            ++i;
        } else if ((c & 0xF0) == 0xE0) {
            uint8_t c2 = u1();
            uint8_t c3 = u1();
            uint32_t cp = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            result += static_cast<char>(0xE0 | (cp >> 12));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
            ++i;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte sequence
            uint8_t c2 = u1(); uint8_t c3 = u1(); uint8_t c4 = u1();
            uint32_t cp = ((c & 0x07) << 18) | ((c2 & 0x3F) << 12)
                        | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
            result += static_cast<char>(0xF0 | (cp >> 18));
            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
            ++i;
        } else {
            // Unknown byte — skip.
            ++i;
        }
    }
    return result;
}

std::vector<uint8_t> DexReader::bytes(size_t n) {
    check(n);
    std::vector<uint8_t> buf(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return buf;
}

// ─── DexHeader ────────────────────────────────────────────────────────────────

DexVersion DexHeader::version() const {
    // magic bytes [4..7] are the version string, e.g. "035\0"
    if (magic[0] == 'c' && magic[1] == 'd' && magic[2] == 'e' && magic[3] == 'x')
        return DexVersion::V040; // CDex
    // Standard DEX: magic = "dex\n" + 3-digit version + '\0'
    int v = (magic[4]-'0')*100 + (magic[5]-'0')*10 + (magic[6]-'0');
    switch (v) {
        case 35: return DexVersion::V035;
        case 36: return DexVersion::V036;
        case 37: return DexVersion::V037;
        case 38: return DexVersion::V038;
        case 39: return DexVersion::V039;
        case 40: return DexVersion::V040;
        default: return DexVersion::Unknown;
    }
}

bool DexHeader::isCompact() const {
    return magic[0] == 'c' && magic[1] == 'd' && magic[2] == 'e' && magic[3] == 'x';
}

// ─── DexFile ─────────────────────────────────────────────────────────────────

DexFile DexFile::parse(const uint8_t* data, size_t size) {
    DexFile df;
    df.data_.assign(data, data + size);

    DexReader r(df.data_.data(), df.data_.size());
    df.parseHeader(r);
    df.parseStringIds(r);
    df.parseTypeIds(r);
    df.parseProtoIds(r);
    df.parseFieldIds(r);
    df.parseMethodIds(r);
    df.parseClassDefs(r);
    return df;
}

DexFile DexFile::parse(const std::vector<uint8_t>& data) {
    return parse(data.data(), data.size());
}

void DexFile::parseHeader(DexReader& r) {
    r.seek(0);
    r.bytes(8); // magic — read and copy
    std::memcpy(header_.magic, data_.data(), 8);

    // Validate magic
    bool isStdDex  = (std::memcmp(header_.magic, "dex\n", 4) == 0);
    bool isCdex    = (std::memcmp(header_.magic, "cdex",  4) == 0);
    if (!isStdDex && !isCdex)
        throw DexParseError("invalid DEX magic");

    header_.checksum         = r.u4();
    r.bytes(20); // SHA-1
    std::memcpy(header_.sha1, data_.data() + 12, 20);

    header_.fileSize         = r.u4();
    header_.headerSize       = r.u4();
    header_.endianTag        = r.u4();

    if (header_.endianTag != kEndianConst && header_.endianTag != kEndianConstSwap)
        throw DexParseError("invalid endian tag: " + std::to_string(header_.endianTag));
    if (header_.endianTag == kEndianConstSwap)
        throw DexParseError("big-endian DEX not supported (very rare in practice)");

    header_.linkSize         = r.u4();
    header_.linkOff          = r.u4();
    header_.mapOff           = r.u4();
    header_.stringIdsSize    = r.u4();
    header_.stringIdsOff     = r.u4();
    header_.typeIdsSize      = r.u4();
    header_.typeIdsOff       = r.u4();
    header_.protoIdsSize     = r.u4();
    header_.protoIdsOff      = r.u4();
    header_.fieldIdsSize     = r.u4();
    header_.fieldIdsOff      = r.u4();
    header_.methodIdsSize    = r.u4();
    header_.methodIdsOff     = r.u4();
    header_.classDefsSize    = r.u4();
    header_.classDefsOff     = r.u4();
    header_.dataSize         = r.u4();
    header_.dataOff          = r.u4();
}

void DexFile::parseStringIds(DexReader& r) {
    strings_.resize(header_.stringIdsSize);
    for (uint32_t i = 0; i < header_.stringIdsSize; ++i) {
        r.seek(header_.stringIdsOff + i * 4u);
        uint32_t off = r.u4();
        // string_data_item: ULEB128 utf16_size, then MUTF-8 bytes.
        r.seek(off);
        uint32_t utf16len = r.uleb128();
        strings_[i] = r.mutf8(utf16len);
    }
}

void DexFile::parseTypeIds(DexReader& r) {
    r.seek(header_.typeIdsOff);
    typeIds_.resize(header_.typeIdsSize);
    for (auto& t : typeIds_)
        t.descriptorIdx = r.u4();
}

void DexFile::parseProtoIds(DexReader& r) {
    r.seek(header_.protoIdsOff);
    protoIds_.resize(header_.protoIdsSize);
    for (auto& p : protoIds_) {
        p.shortyIdx      = r.u4();
        p.returnTypeIdx  = r.u4();
        p.parametersOff  = r.u4();
    }
}

void DexFile::parseFieldIds(DexReader& r) {
    r.seek(header_.fieldIdsOff);
    fieldIds_.resize(header_.fieldIdsSize);
    for (auto& f : fieldIds_) {
        f.classIdx = r.u2();
        f.typeIdx  = r.u2();
        f.nameIdx  = r.u4();
    }
}

void DexFile::parseMethodIds(DexReader& r) {
    r.seek(header_.methodIdsOff);
    methodIds_.resize(header_.methodIdsSize);
    for (auto& m : methodIds_) {
        m.classIdx = r.u2();
        m.protoIdx = r.u2();
        m.nameIdx  = r.u4();
    }
}

void DexFile::parseClassDefs(DexReader& r) {
    r.seek(header_.classDefsOff);
    classDefs_.resize(header_.classDefsSize);
    for (auto& cd : classDefs_) {
        cd.classIdx        = r.u4();
        cd.accessFlags     = r.u4();
        cd.superclassIdx   = r.u4();
        cd.interfacesOff   = r.u4();
        cd.sourceFileIdx   = r.u4();
        cd.annotationsOff  = r.u4();
        cd.classDataOff    = r.u4();
        cd.staticValuesOff = r.u4();
    }
}

// ─── DexFile resolution helpers ──────────────────────────────────────────────

const std::string& DexFile::string(uint32_t idx) const {
    if (idx >= strings_.size())
        throw DexParseError("string index out of range: " + std::to_string(idx));
    return strings_[idx];
}

const std::string& DexFile::typeName(uint32_t typeIdx) const {
    return string(typeIds_.at(typeIdx).descriptorIdx);
}

std::string DexFile::typeDescriptor(uint32_t typeIdx) const {
    return typeName(typeIdx);
}

std::string DexFile::fieldClass(uint32_t fieldIdx) const {
    return typeName(fieldIds_.at(fieldIdx).classIdx);
}

std::string DexFile::fieldType(uint32_t fieldIdx) const {
    return typeName(fieldIds_.at(fieldIdx).typeIdx);
}

std::string DexFile::fieldName(uint32_t fieldIdx) const {
    return string(fieldIds_.at(fieldIdx).nameIdx);
}

std::string DexFile::methodClass(uint32_t methodIdx) const {
    return typeName(methodIds_.at(methodIdx).classIdx);
}

std::string DexFile::methodName(uint32_t methodIdx) const {
    return string(methodIds_.at(methodIdx).nameIdx);
}

std::string DexFile::methodProto(uint32_t methodIdx) const {
    const MethodId& mid   = methodIds_.at(methodIdx);
    const ProtoId&  proto = protoIds_.at(mid.protoIdx);

    // Build descriptor "(params)retType"
    std::string result = "(";
    if (proto.parametersOff != 0) {
        auto params = readTypeList(proto.parametersOff);
        for (uint32_t ti : params)
            result += typeName(ti);
    }
    result += ')';
    result += typeName(proto.returnTypeIdx);
    return result;
}

ClassData DexFile::readClassData(uint32_t offset) const {
    DexReader r(data_.data(), data_.size());
    r.seek(offset);

    ClassData cd;
    uint32_t numStaticFields   = r.uleb128();
    uint32_t numInstanceFields = r.uleb128();
    uint32_t numDirectMethods  = r.uleb128();
    uint32_t numVirtualMethods = r.uleb128();

    auto readFields = [&](uint32_t count) {
        std::vector<EncodedField> fields;
        fields.resize(count);
        uint32_t prevIdx = 0;
        for (auto& f : fields) {
            f.fieldIdxDiff = r.uleb128();
            f.accessFlags  = r.uleb128();
            f.fieldIdx     = prevIdx + f.fieldIdxDiff;
            prevIdx        = f.fieldIdx;
        }
        return fields;
    };

    auto readMethods = [&](uint32_t count) {
        std::vector<EncodedMethod> methods;
        methods.resize(count);
        uint32_t prevIdx = 0;
        for (auto& m : methods) {
            m.methodIdxDiff = r.uleb128();
            m.accessFlags   = r.uleb128();
            m.codeOff       = r.uleb128();
            m.methodIdx     = prevIdx + m.methodIdxDiff;
            prevIdx         = m.methodIdx;
        }
        return methods;
    };

    cd.staticFields   = readFields(numStaticFields);
    cd.instanceFields = readFields(numInstanceFields);
    cd.directMethods  = readMethods(numDirectMethods);
    cd.virtualMethods = readMethods(numVirtualMethods);
    return cd;
}

CodeItem DexFile::readCodeItem(uint32_t offset) const {
    DexReader r(data_.data(), data_.size());
    r.seek(offset);

    CodeItem code;
    code.registersSize = r.u2();
    code.insSize       = r.u2();
    code.outsSize      = r.u2();
    code.triesSize     = r.u2();
    code.debugInfoOff  = r.u4();
    code.insnsSize     = r.u4();

    code.insns.resize(code.insnsSize);
    for (auto& w : code.insns)
        w = r.u2();

    // Align to 4 bytes if tries present and instructions count is odd.
    if (code.triesSize > 0 && (code.insnsSize & 1))
        r.u2(); // padding

    if (code.triesSize > 0) {
        code.tries.resize(code.triesSize);
        for (auto& t : code.tries) {
            t.startAddr  = r.u4();
            t.insnCount  = r.u2();
            t.handlerOff = r.u2();
        }

        // encoded_catch_handler_list
        size_t handlerListStart = r.pos();
        uint32_t handlerListSize = r.uleb128(); // number of handler lists
        code.handlers.handlers.resize(handlerListSize);
        code.handlers.catchAllAddrs.resize(handlerListSize, ~0u);

        for (uint32_t i = 0; i < handlerListSize; ++i) {
            int32_t size = r.sleb128(); // positive: type+addr pairs; negative: pairs + catch-all
            uint32_t pairCount = static_cast<uint32_t>(size < 0 ? -size : size);
            code.handlers.handlers[i].resize(pairCount);
            for (auto& handler : code.handlers.handlers[i]) {
                handler.typeIdx = static_cast<int32_t>(r.uleb128());
                handler.addr    = r.uleb128();
            }
            if (size <= 0) {
                // catch-all handler follows
                code.handlers.catchAllAddrs[i] = r.uleb128();
            }
        }
        (void)handlerListStart; // suppress unused warning
    }

    return code;
}

std::vector<uint32_t> DexFile::readTypeList(uint32_t offset) const {
    if (offset == 0)
        return {};
    DexReader r(data_.data(), data_.size());
    r.seek(offset);
    uint32_t size = r.u4();
    std::vector<uint32_t> result(size);
    for (auto& t : result)
        t = r.u2(); // type_item.typeIdx
    return result;
}

} // namespace dex_parser
} // namespace retdec
