/**
 * @file src/pyc_parser/py_marshal.cpp
 * @brief Python marshal format reader implementation.
 */

#include <memory>
#include "retdec/pyc_parser/py_marshal.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <sstream>

namespace retdec {
namespace pyc_parser {

// ─── MarshalObject::toConst ───────────────────────────────────────────────────

PyCodeObject::Const MarshalObject::toConst() const {
    PyCodeObject::Const c;
    switch (type) {
    case Type::None:     c.kind = PyCodeObject::Const::Kind::None;    break;
    case Type::True:     c.kind = PyCodeObject::Const::Kind::True;    break;
    case Type::False:    c.kind = PyCodeObject::Const::Kind::False;   break;
    case Type::Ellipsis: c.kind = PyCodeObject::Const::Kind::Ellipsis;break;
    case Type::Int:
    case Type::Long:
        c.kind = PyCodeObject::Const::Kind::Int;
        c.ival = asInt();
        break;
    case Type::Float:
        c.kind = PyCodeObject::Const::Kind::Float;
        c.fval = asFloat();
        break;
    case Type::Bytes:
        c.kind = PyCodeObject::Const::Kind::Bytes;
        c.sval = asStr();
        break;
    case Type::Str:
        c.kind = PyCodeObject::Const::Kind::Unicode;
        c.sval = asStr();
        break;
    case Type::Tuple: {
        c.kind = PyCodeObject::Const::Kind::Tuple;
        for (const auto& elem : asTuple())
            if (elem) c.elements.push_back(elem->toConst());
        break;
    }
    case Type::FrozenSet: {
        c.kind = PyCodeObject::Const::Kind::FrozenSet;
        for (const auto& elem : asTuple())
            if (elem) c.elements.push_back(elem->toConst());
        break;
    }
    case Type::Code:
        c.kind = PyCodeObject::Const::Kind::Code;
        c.code = asCode();
        break;
    default:
        c.kind = PyCodeObject::Const::Kind::None;
        break;
    }
    return c;
}

// ─── MarshalReader ────────────────────────────────────────────────────────────

MarshalReader::MarshalReader(const uint8_t* data, size_t size,
                              const PythonVersion& version)
    : data_(data), size_(size), version_(version) {}

// ─── Primitive readers ────────────────────────────────────────────────────────

bool MarshalReader::readByte(uint8_t& out) {
    if (pos_ >= size_) {
        setError("Unexpected EOF reading byte");
        return false;
    }
    out = data_[pos_++];
    return true;
}

bool MarshalReader::readU16LE(uint16_t& out) {
    if (pos_ + 2 > size_) {
        setError("Unexpected EOF reading uint16");
        return false;
    }
    out = static_cast<uint16_t>(data_[pos_]) |
          (static_cast<uint16_t>(data_[pos_+1]) << 8);
    pos_ += 2;
    return true;
}

bool MarshalReader::readS32LE(int32_t& out) {
    if (pos_ + 4 > size_) {
        setError("Unexpected EOF reading int32");
        return false;
    }
    uint32_t v = static_cast<uint32_t>(data_[pos_])
               | (static_cast<uint32_t>(data_[pos_+1]) << 8)
               | (static_cast<uint32_t>(data_[pos_+2]) << 16)
               | (static_cast<uint32_t>(data_[pos_+3]) << 24);
    out = static_cast<int32_t>(v);
    pos_ += 4;
    return true;
}

bool MarshalReader::readU32LE(uint32_t& out) {
    if (pos_ + 4 > size_) {
        setError("Unexpected EOF reading uint32");
        return false;
    }
    out = static_cast<uint32_t>(data_[pos_])
        | (static_cast<uint32_t>(data_[pos_+1]) << 8)
        | (static_cast<uint32_t>(data_[pos_+2]) << 16)
        | (static_cast<uint32_t>(data_[pos_+3]) << 24);
    pos_ += 4;
    return true;
}

bool MarshalReader::readF64LE(double& out) {
    if (pos_ + 8 > size_) {
        setError("Unexpected EOF reading float64");
        return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(data_[pos_+i]) << (8*i);
    memcpy(&out, &v, 8);
    pos_ += 8;
    return true;
}

bool MarshalReader::readBytes(std::vector<uint8_t>& out, size_t n) {
    if (pos_ + n > size_) {
        setError("Unexpected EOF reading bytes");
        return false;
    }
    out.assign(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return true;
}

bool MarshalReader::readString(std::string& out, size_t n) {
    if (pos_ + n > size_) {
        setError("Unexpected EOF reading string");
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return true;
}

bool MarshalReader::readAscii(std::string& out, size_t n) {
    return readString(out, n);
}

bool MarshalReader::readNullTerminatedFloat(double& out) {
    // TYPE_FLOAT: uint8 length + ASCII digits
    uint8_t len;
    if (!readByte(len)) return false;
    std::string s;
    if (!readString(s, len)) return false;
    try {
        out = std::stod(s);
    } catch (...) {
        out = 0.0;
    }
    return true;
}

// ─── Reference table ─────────────────────────────────────────────────────────

size_t MarshalReader::reserveRef() {
    refs_.push_back(nullptr);
    return refs_.size() - 1;
}

void MarshalReader::setRef(size_t idx, std::shared_ptr<MarshalObject> obj) {
    if (idx < refs_.size()) refs_[idx] = std::move(obj);
}

std::shared_ptr<MarshalObject> MarshalReader::getRef(size_t idx) const {
    if (idx < refs_.size()) return refs_[idx];
    return nullptr;
}

// ─── Type-specific readers ────────────────────────────────────────────────────

std::shared_ptr<MarshalObject> MarshalReader::readInt() {
    int32_t v;
    if (!readS32LE(v)) return nullptr;
    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Int;
    obj->value = static_cast<int64_t>(v);
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readLong() {
    // Python TYPE_LONG: int32 n, then n uint16 "digits" in base 2^15
    int32_t n;
    if (!readS32LE(n)) return nullptr;
    int sign = (n < 0) ? -1 : 1;
    if (n < 0) n = -n;
    int64_t result = 0;
    for (int32_t i = 0; i < n; ++i) {
        uint16_t d;
        if (!readU16LE(d)) return nullptr;
        result |= static_cast<int64_t>(d) << (15 * i);
    }
    result *= sign;
    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Long;
    obj->value = result;
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readFloat(bool binary) {
    double v = 0.0;
    if (binary) {
        if (!readF64LE(v)) return nullptr;
    } else {
        if (!readNullTerminatedFloat(v)) return nullptr;
    }
    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Float;
    obj->value = v;
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readComplex(bool binary) {
    double re = 0.0, im = 0.0;
    if (binary) {
        if (!readF64LE(re) || !readF64LE(im)) return nullptr;
    } else {
        if (!readNullTerminatedFloat(re) || !readNullTerminatedFloat(im)) return nullptr;
    }
    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Complex;
    obj->value = std::make_pair(re, im);
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readBytes() {
    int32_t len;
    if (!readS32LE(len)) return nullptr;
    if (len < 0) { setError("Negative bytes length"); return nullptr; }
    std::string s;
    if (!readString(s, static_cast<size_t>(len))) return nullptr;
    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Bytes;
    obj->value = std::move(s);
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readStr(size_t len, bool ascii) {
    (void)ascii;
    std::string s;
    if (!readString(s, len)) return nullptr;
    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Str;
    obj->value = std::move(s);
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readShortStr(bool ascii) {
    uint8_t len;
    if (!readByte(len)) return nullptr;
    return readStr(len, ascii);
}

std::shared_ptr<MarshalObject> MarshalReader::readTuple(bool small) {
    size_t n = 0;
    if (small) {
        uint8_t nb;
        if (!readByte(nb)) return nullptr;
        n = nb;
    } else {
        int32_t ni;
        if (!readS32LE(ni)) return nullptr;
        if (ni < 0) { setError("Negative tuple size"); return nullptr; }
        n = static_cast<size_t>(ni);
    }

    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Tuple;
    obj->value = std::vector<std::shared_ptr<MarshalObject>>{};
    auto& elems = std::get<std::vector<std::shared_ptr<MarshalObject>>>(obj->value);
    elems.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        auto elem = readObject();
        if (!elem && hasError_) return nullptr;
        elems.push_back(std::move(elem));
    }
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readList() {
    int32_t n;
    if (!readS32LE(n)) return nullptr;
    if (n < 0) { setError("Negative list size"); return nullptr; }

    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::List;
    obj->value = std::vector<std::shared_ptr<MarshalObject>>{};
    auto& elems = std::get<std::vector<std::shared_ptr<MarshalObject>>>(obj->value);

    for (int32_t i = 0; i < n; ++i) {
        auto elem = readObject();
        if (!elem && hasError_) return nullptr;
        elems.push_back(std::move(elem));
    }
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readSet(bool frozen) {
    int32_t n;
    if (!readS32LE(n)) return nullptr;
    if (n < 0) { setError("Negative set size"); return nullptr; }

    auto obj = std::make_shared<MarshalObject>();
    obj->type  = frozen ? MarshalObject::Type::FrozenSet : MarshalObject::Type::Set;
    obj->value = std::vector<std::shared_ptr<MarshalObject>>{};
    auto& elems = std::get<std::vector<std::shared_ptr<MarshalObject>>>(obj->value);

    for (int32_t i = 0; i < n; ++i) {
        auto elem = readObject();
        if (!elem && hasError_) return nullptr;
        elems.push_back(std::move(elem));
    }
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readDict() {
    using DictT = std::vector<std::pair<std::shared_ptr<MarshalObject>,
                                        std::shared_ptr<MarshalObject>>>;
    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Dict;
    obj->value = DictT{};
    auto& dict = std::get<DictT>(obj->value);

    while (!eof()) {
        auto key = readObject();
        if (!key) break; // TYPE_NULL signals end of dict
        if (key->type == MarshalObject::Type::Null) break;
        auto val = readObject();
        if (!val && hasError_) return nullptr;
        dict.emplace_back(std::move(key), std::move(val));
    }
    return obj;
}

// ─── Code object reader ───────────────────────────────────────────────────────

std::shared_ptr<PyCodeObject> MarshalReader::readCodeFields() {
    auto code = std::make_shared<PyCodeObject>();
    code->version = version_;

    auto readInt32 = [&](int32_t& out) -> bool {
        return readS32LE(out);
    };

    auto readStrObj = [&](std::string& out) -> bool {
        auto obj = readObject();
        if (!obj) return !hasError_;
        if (obj->isStr() || obj->isBytes())
            out = obj->asStr();
        return true;
    };

    auto readStrTuple = [&](std::vector<std::string>& out) -> bool {
        auto obj = readObject();
        if (!obj) return !hasError_;
        if (!obj->isTuple()) return true;
        for (const auto& elem : obj->asTuple()) {
            if (elem && (elem->isStr() || elem->isBytes()))
                out.push_back(elem->asStr());
            else
                out.push_back("");
        }
        return true;
    };

    // Field order (matches CPython marshal_write_code/marshal_read_code):
    // 3.8–3.10:
    //   argcount, posonlyargcount(3.8+), kwonlyargcount, nlocals,
    //   stacksize, flags, code_bytes, consts, names, varnames,
    //   freevars, cellvars, filename, name, firstlineno, lnotab
    //
    // 3.11+:
    //   argcount, posonlyargcount, kwonlyargcount, stacksize, flags,
    //   code_bytes, consts, names, localsplusnames, localspluskinds,
    //   filename, name, qualname, firstlineno, linetable, exceptiontable
    //
    // 3.12: same as 3.11 but qualname moved, exceptiontable restructured

    if (!readInt32(code->co_argcount)) return nullptr;

    // co_posonlyargcount added in 3.8
    if (!readInt32(code->co_posonlyargcount)) return nullptr;

    if (!readInt32(code->co_kwonlyargcount)) return nullptr;

    if (!version_.atLeast(3, 11)) {
        if (!readInt32(code->co_nlocals)) return nullptr;
    }

    if (!readInt32(code->co_stacksize)) return nullptr;

    int32_t flags;
    if (!readInt32(flags)) return nullptr;
    code->co_flags = static_cast<uint32_t>(flags);

    // co_code (bytecode bytes)
    {
        auto obj = readObject();
        if (!obj) {
            if (!hasError_) return code;
            return nullptr;
        }
        if (obj->isBytes() || obj->isStr()) {
            const auto& s = obj->asStr();
            code->co_code.assign(s.begin(), s.end());
        }
    }

    // co_consts
    {
        auto obj = readObject();
        if (obj && obj->isTuple()) {
            for (const auto& elem : obj->asTuple()) {
                if (elem) {
                    // Nested code objects are added with kind=Code
                    code->co_consts.push_back(elem->toConst());
                }
            }
        }
    }

    // co_names
    if (!readStrTuple(code->co_names)) return nullptr;

    if (version_.atLeast(3, 11)) {
        // 3.11+: localsplusnames (all locals + free + cell in one tuple)
        //        localspluskinds (bytes indicating which kind each is)
        auto namesObj = readObject();
        if (namesObj && namesObj->isTuple()) {
            for (const auto& elem : namesObj->asTuple()) {
                if (elem && (elem->isStr() || elem->isBytes()))
                    code->co_varnames.push_back(elem->asStr());
                else
                    code->co_varnames.push_back("");
            }
        }
        // localspluskinds (bytes)
        auto kindsObj = readObject();
        (void)kindsObj; // we don't use it for decompilation
    } else {
        // 3.8–3.10: varnames, freevars, cellvars
        if (!readStrTuple(code->co_varnames))  return nullptr;
        if (!readStrTuple(code->co_freevars))  return nullptr;
        if (!readStrTuple(code->co_cellvars))  return nullptr;
    }

    // filename, name
    if (!readStrObj(code->co_filename)) return nullptr;
    if (!readStrObj(code->co_name))     return nullptr;

    // co_qualname (3.11+)
    if (version_.atLeast(3, 11)) {
        if (!readStrObj(code->co_qualname)) return nullptr;
    }

    // firstlineno
    if (!readInt32(code->co_firstlineno)) return nullptr;

    // linetable / lnotab
    {
        auto obj = readObject();
        if (obj && (obj->isBytes() || obj->isStr())) {
            const auto& s = obj->asStr();
            code->co_linetable.assign(s.begin(), s.end());
        }
    }

    // exceptiontable (3.11+)
    if (version_.atLeast(3, 11)) {
        auto obj = readObject();
        if (obj && (obj->isBytes() || obj->isStr())) {
            const auto& s = obj->asStr();
            code->co_exceptiontable.assign(s.begin(), s.end());
        }
    }

    // Decode line number table
    if (!code->co_linetable.empty()) {
        if (version_.atLeast(3, 11)) {
            code->lineTable = decodeLnotab311(
                code->co_linetable, code->co_firstlineno,
                static_cast<uint32_t>(code->co_code.size()));
        } else if (version_.atLeast(3, 10)) {
            code->lineTable = decodeLnotab310(
                code->co_linetable, code->co_firstlineno,
                static_cast<uint32_t>(code->co_code.size()));
        } else {
            code->lineTable = decodeLnotab(
                code->co_linetable, code->co_firstlineno,
                static_cast<uint32_t>(code->co_code.size()));
        }
    }

    // Decode exception table (3.11+)
    if (!code->co_exceptiontable.empty()) {
        code->exceptionTable = decodeExceptionTable(code->co_exceptiontable);
    }

    // Fix up nlocals for 3.11+ (use varnames count)
    if (version_.atLeast(3, 11)) {
        code->co_nlocals = static_cast<int32_t>(code->co_varnames.size());
    }

    return code;
}

std::shared_ptr<MarshalObject> MarshalReader::readCode() {
    auto codeObj = readCodeFields();
    if (!codeObj) return nullptr;

    auto obj = std::make_shared<MarshalObject>();
    obj->type  = MarshalObject::Type::Code;
    obj->value = std::move(codeObj);
    return obj;
}

std::shared_ptr<MarshalObject> MarshalReader::readRef() {
    int32_t idx;
    if (!readS32LE(idx)) return nullptr;
    if (idx < 0 || static_cast<size_t>(idx) >= refs_.size()) {
        setError("Invalid reference index " + std::to_string(idx));
        return nullptr;
    }
    return refs_[static_cast<size_t>(idx)];
}

// ─── readObject ───────────────────────────────────────────────────────────────

std::shared_ptr<MarshalObject> MarshalReader::readObject() {
    if (eof()) return nullptr;

    uint8_t typeByte;
    if (!readByte(typeByte)) return nullptr;

    bool flagRef = (typeByte & 0x80) != 0;
    char typeChar = static_cast<char>(typeByte & 0x7F);

    // Reserve a ref slot before reading the object (for FLAG_REF self-reference)
    size_t refIdx = 0;
    if (flagRef) refIdx = reserveRef();

    std::shared_ptr<MarshalObject> obj;

    switch (typeChar) {
    case '0': {
        // TYPE_NULL — signals end of dict
        obj = std::make_shared<MarshalObject>();
        obj->type = MarshalObject::Type::Null;
        break;
    }
    case 'N': {
        obj = std::make_shared<MarshalObject>();
        obj->type = MarshalObject::Type::None;
        break;
    }
    case 'F': {
        obj = std::make_shared<MarshalObject>();
        obj->type = MarshalObject::Type::False;
        break;
    }
    case 'T': {
        obj = std::make_shared<MarshalObject>();
        obj->type = MarshalObject::Type::True;
        break;
    }
    case '.': {
        obj = std::make_shared<MarshalObject>();
        obj->type = MarshalObject::Type::Ellipsis;
        break;
    }
    case 'i': obj = readInt();              break;
    case 'l': obj = readLong();             break;
    case 'f': obj = readFloat(false);       break;
    case 'g': obj = readFloat(true);        break;
    case 'x': obj = readComplex(false);     break;
    case 'y': obj = readComplex(true);      break;
    case 's': obj = readBytes();            break;  // TYPE_STRING (Python 2 / bytes)
    case 't': obj = readBytes();            break;  // TYPE_INTERNED
    case 'B': obj = readBytes();            break;  // TYPE_BYTES (Python 3)
    case 'u': {
        int32_t len;
        if (!readS32LE(len) || len < 0) break;
        obj = readStr(static_cast<size_t>(len), false);
        break;
    }
    case 'A': {
        int32_t len;
        if (!readS32LE(len) || len < 0) break;
        obj = readStr(static_cast<size_t>(len), true);
        break;
    }
    case 'a': {
        int32_t len;
        if (!readS32LE(len) || len < 0) break;
        obj = readStr(static_cast<size_t>(len), true);
        break;
    }
    case 'z': obj = readShortStr(true);     break;  // TYPE_SHORT_ASCII
    case 'Z': obj = readShortStr(true);     break;  // TYPE_SHORT_ASCII_INTERNED
    case '(': obj = readTuple(false);       break;  // TYPE_TUPLE        (int32 count)
    case ')': obj = readTuple(true);        break;  // TYPE_SMALL_TUPLE  (uint8 count)
    case '[': obj = readList();             break;  // TYPE_LIST
    case '{': obj = readDict();             break;  // TYPE_DICT
    case '<': obj = readSet(false);         break;  // TYPE_SET
    case '>': obj = readSet(true);          break;  // TYPE_FROZENSET
    case 'c': obj = readCode();             break;  // TYPE_CODE
    case 'r': obj = readRef();              break;  // TYPE_REF (no FLAG_REF for refs)
    case 'R': {
        // TYPE_STRINGREF (Python 2 compat)
        int32_t idx;
        if (!readS32LE(idx)) break;
        obj = getRef(static_cast<size_t>(idx));
        break;
    }
    default: {
        std::ostringstream ss;
        ss << "Unknown marshal type byte 0x" << std::hex
           << static_cast<int>(typeChar) << " at offset " << std::dec << (pos_-1)
           << " (pos_=" << pos_ << ")";
        setError(ss.str());
        return nullptr;
    }
    }

    if (flagRef && obj) {
        setRef(refIdx, obj);
    }
    return obj;
}

} // namespace pyc_parser
} // namespace retdec
