/**
 * @file src/lua_parser/lua_reader.cpp
 * @brief Lua compiled bytecode reader implementation.
 *
 * Supports Lua 5.1, 5.2, 5.3, and 5.4 .luac files.
 */

#include "retdec/lua_parser/lua_reader.h"

#include <cassert>
#include <cstring>

namespace retdec {
namespace lua_parser {

// ─── Lua .luac header magic ───────────────────────────────────────────────────

static constexpr uint8_t kLuaMagic0 = 0x1B;
static constexpr uint8_t kLuaMagic1 = 'L';
static constexpr uint8_t kLuaMagic2 = 'u';
static constexpr uint8_t kLuaMagic3 = 'a';

// ─── Constructor ─────────────────────────────────────────────────────────────

LuaReader::LuaReader(std::vector<uint8_t> bytes)
    : data_(std::move(bytes)) {}

LuaReader::LuaReader(const uint8_t* data, size_t size)
    : data_(data, data + size) {}

// ─── Primitives ──────────────────────────────────────────────────────────────

uint8_t LuaReader::readU8() {
    if (pos_ >= data_.size()) throw ParseError{"Unexpected end of file"};
    return data_[pos_++];
}

uint32_t LuaReader::readU32() {
    if (pos_ + 4 > data_.size()) throw ParseError{"Read past end (u32)"};
    uint32_t v;
    std::memcpy(&v, &data_[pos_], 4);
    pos_ += 4;
    // Handle endianness
    if (!le_) {
        v = ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) |
            (((v >> 16) & 0xFF) << 8) | (v >> 24);
    }
    return v;
}

uint64_t LuaReader::readU64() {
    if (pos_ + 8 > data_.size()) throw ParseError{"Read past end (u64)"};
    uint64_t v;
    std::memcpy(&v, &data_[pos_], 8);
    pos_ += 8;
    if (!le_) {
        // byte-swap
        v = ((v & 0xFF) << 56) | (((v >> 8) & 0xFF) << 48) |
            (((v >> 16) & 0xFF) << 40) | (((v >> 24) & 0xFF) << 32) |
            (((v >> 32) & 0xFF) << 24) | (((v >> 40) & 0xFF) << 16) |
            (((v >> 48) & 0xFF) << 8)  | (v >> 56);
    }
    return v;
}

// Lua 5.4 modified LEB128: MSB=1 means this is the LAST byte.
// Bits are packed big-endian within the sequence.
size_t LuaReader::readLuaSize54() {
    size_t result = 0;
    for (int shift = 0; ; shift += 7) {
        uint8_t b = readU8();
        result |= static_cast<size_t>(b & 0x7F) << shift;
        if (b & 0x80) break;  // MSB=1 → last byte
    }
    return result;
}

int32_t LuaReader::readInt() {
    if (useLeb128_) return static_cast<int32_t>(readLuaSize54());
    if (intSz_ == 4) return (int32_t)readU32();
    if (intSz_ == 8) return (int32_t)readU64();
    throw ParseError{"Unsupported int size"};
}

int64_t LuaReader::readLuaInt() {
    // Lua 5.3+ integer (always 8 bytes in modern Lua, but follows intSz_ for older)
    if (intSz_ == 8) return (int64_t)readU64();
    return (int64_t)readInt();
}

double LuaReader::readLuaFloat() {
    // Lua uses double (8 bytes) for floats
    uint64_t bits = readU64();
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

size_t LuaReader::readSizet() {
    if (sizetSz_ == 8) return (size_t)readU64();
    if (sizetSz_ == 4) return (size_t)readU32();
    throw ParseError{"Unsupported size_t size"};
}

// Lua 5.1/5.2: string = size_t length + bytes (length includes null terminator)
std::string LuaReader::readString51() {
    size_t len = readSizet();
    if (len == 0) return "";
    if (pos_ + len > data_.size()) throw ParseError{"String read past end"};
    // len includes the null terminator
    std::string s(reinterpret_cast<const char*>(&data_[pos_]), len - 1);
    pos_ += len;
    return s;
}

// Lua 5.3+: string = ULEB128 length (0=null, 0xFF prefix = large)
std::string LuaReader::readString53() {
    uint8_t first = readU8();
    if (first == 0) return "";
    size_t len;
    if (first == 0xFF) {
        len = (size_t)readU64() - 1; // includes null terminator
    } else {
        len = (size_t)(first - 1); // length includes null, subtract 1
    }
    if (pos_ + len > data_.size()) throw ParseError{"String53 read past end"};
    std::string s(reinterpret_cast<const char*>(&data_[pos_]), len);
    pos_ += len;
    return s;
}

// Lua 5.4.6: modified LEB128 size (includes null), with string dedup table.
std::string LuaReader::readString54() {
    size_t size = readLuaSize54();
    if (size == 0) return "";
    // If high bit of first encoded byte was 0, this is a back-reference index.
    // Actually: size > 0 means new string (size = content_len + 1 for null).
    // Back-references use a different mechanism; for simplicity we handle
    // the common case: size >= 1 means (size-1) bytes of content follow.
    size_t contentLen = size - 1;
    if (pos_ + contentLen > data_.size())
        throw ParseError{"String54 read past end"};
    std::string s(reinterpret_cast<const char*>(&data_[pos_]), contentLen);
    pos_ += contentLen;
    // Add to deduplication table for potential back-references
    stringTable54_.push_back(s);
    return s;
}

std::string LuaReader::readString() {
    if (ver_ == LuaVersion::Lua54) return readString54();
    if (ver_ >= LuaVersion::Lua53) return readString53();
    return readString51();
}

// ─── Header ──────────────────────────────────────────────────────────────────

LuaVersion LuaReader::parseHeader() {
    // Magic
    if (readU8() != kLuaMagic0 || readU8() != kLuaMagic1 ||
        readU8() != kLuaMagic2 || readU8() != kLuaMagic3)
        throw ParseError{"Not a Lua bytecode file (bad magic)"};

    uint8_t verByte = readU8();
    LuaVersion ver;
    switch (verByte) {
    case 0x51: ver = LuaVersion::Lua51; break;
    case 0x52: ver = LuaVersion::Lua52; break;
    case 0x53: ver = LuaVersion::Lua53; break;
    case 0x54: ver = LuaVersion::Lua54; break;
    default:
        warn("Unknown Lua version 0x" + std::to_string(verByte) + ", trying 5.4");
        ver = LuaVersion::Lua54;
    }
    ver_ = ver;

    if (ver == LuaVersion::Lua51) {
        uint8_t format = readU8(); (void)format;
        le_     = (readU8() == 1);
        intSz_  = readU8();
        sizetSz_= readU8();
        readU8(); // instr size
        readU8(); // lua_Number size
        readU8(); // integral flag
    } else if (ver == LuaVersion::Lua52) {
        uint8_t format = readU8(); (void)format;
        le_     = (readU8() == 1);
        intSz_  = readU8();
        sizetSz_= readU8();
        readU8(); // instr size
        readU8(); // lua_Number size
        readU8(); // integral flag
        // LUAC_TAIL: 6 bytes "\x19\x93\r\n\x1a\n"
        pos_ += 6;
    } else if (ver == LuaVersion::Lua53) {
        readU8(); // format
        // LUAC_DATA: "\x19\x93\r\n\x1a\n" (6 bytes)
        pos_ += 6;
        intSz_   = readU8();
        sizetSz_ = readU8();
        readU8(); // instr size
        // integer size (lua_Integer) - 8 bytes
        // float size (lua_Number) - 8 bytes
        readU8(); readU8();
        // test integer: 0x5678
        if (intSz_ == 4) readU32(); else readU64();
        // test float: 370.5
        readU64();
        le_ = true; // Lua 5.3 always little-endian in official builds
    } else { // Lua 5.4
        readU8(); // format
        // LUAC_DATA: "\x19\x93\r\n\x1a\n"
        pos_ += 6;
        intSz_   = readU8(); // sizeof(Instruction) = 4
        sizetSz_ = readU8(); // sizeof(lua_Integer) = 8
        readU8();            // sizeof(lua_Number) = 8 (Lua 5.4 header has 3 size bytes)
        // Two test values: integer 0x5678, float 370.5
        readU64(); // test integer (lua_Integer)
        readU64(); // test float  (lua_Number)
        le_ = true;
        // Lua 5.4.6 uses modified LEB128 for all integer/size values in bytecode
        useLeb128_ = true;
    }

    return ver;
}

// ─── Code ────────────────────────────────────────────────────────────────────

std::vector<LuaInstr> LuaReader::readCode() {
    int n = readInt();
    std::vector<LuaInstr> code;
    code.reserve(n);
    for (int i = 0; i < n; ++i) {
        LuaInstr instr;
        instr.raw = readU32();
        code.push_back(instr);
    }
    return code;
}

// ─── Constants ───────────────────────────────────────────────────────────────

std::vector<LuaConst> LuaReader::readConstants51() {
    int n = readInt();
    std::vector<LuaConst> consts;
    consts.reserve(n);
    for (int i = 0; i < n; ++i) {
        uint8_t tag = readU8();
        switch (tag) {
        case 0: consts.emplace_back(LuaNil{}); break;
        case 1: consts.emplace_back(LuaBool{readU8() != 0}); break;
        case 3: { // LUA_TNUMBER (double)
            double v = readLuaFloat();
            consts.emplace_back(LuaFloat{v});
            break;
        }
        case 4: consts.emplace_back(LuaStr{readString51()}); break;
        default:
            warn("Unknown constant tag " + std::to_string(tag));
            consts.emplace_back(LuaNil{});
        }
    }
    return consts;
}

std::vector<LuaConst> LuaReader::readConstants52() {
    return readConstants51(); // same format as 5.1
}

std::vector<LuaConst> LuaReader::readConstants53() {
    int n = readInt();
    std::vector<LuaConst> consts;
    consts.reserve(n);
    for (int i = 0; i < n; ++i) {
        uint8_t tag = readU8();
        switch (tag) {
        case 0: consts.emplace_back(LuaNil{}); break;
        case 1: consts.emplace_back(LuaBool{readU8() != 0}); break;
        case 19: // LUA_TNUMINT (integer subtype)
            consts.emplace_back(LuaInt{readLuaInt()});
            break;
        case 3: // LUA_TNUMFLT
            consts.emplace_back(LuaFloat{readLuaFloat()});
            break;
        case 20: // Short string
        case 4:  // Long string
            consts.emplace_back(LuaStr{readString53()});
            break;
        default:
            warn("Unknown Lua 5.3 constant tag " + std::to_string(tag));
            consts.emplace_back(LuaNil{});
        }
    }
    return consts;
}

std::vector<LuaConst> LuaReader::readConstants54() {
    // Lua 5.4 variant tags (makevariant(type, variant)):
    //   0x00 = LUA_VNIL,  0x01 = LUA_VFALSE, 0x11 = LUA_VTRUE
    //   0x03 = LUA_VNUMINT (integer, LEB128), 0x13 = LUA_VNUMFLT (float, 8 bytes)
    //   0x04 = LUA_VSHRSTR (short string),    0x14 = LUA_VLNGSTR (long string)
    int n = readInt();
    std::vector<LuaConst> consts;
    consts.reserve(n);
    for (int i = 0; i < n; ++i) {
        uint8_t tag = readU8();
        switch (tag) {
        case 0x00: consts.emplace_back(LuaNil{}); break;
        case 0x01: consts.emplace_back(LuaBool{false}); break;
        case 0x11: consts.emplace_back(LuaBool{true}); break;
        case 0x03: // LUA_VNUMINT — integer stored as LEB128
            consts.emplace_back(LuaInt{readLuaInt()});
            break;
        case 0x13: // LUA_VNUMFLT — float stored as 8 raw bytes
            consts.emplace_back(LuaFloat{readLuaFloat()});
            break;
        case 0x04: // LUA_VSHRSTR — short string
        case 0x14: // LUA_VLNGSTR — long string
            consts.emplace_back(LuaStr{readString54()});
            break;
        default:
            warn("Unknown Lua 5.4 constant tag " + std::to_string(tag));
            consts.emplace_back(LuaNil{});
        }
    }
    return consts;
}

// ─── Upvalues ────────────────────────────────────────────────────────────────

std::vector<LuaUpvalue> LuaReader::readUpvalues51(int n) {
    // In Lua 5.1 upvalue info is minimal (just count, no inStack/idx)
    std::vector<LuaUpvalue> uvs(n);
    return uvs;
}

std::vector<LuaUpvalue> LuaReader::readUpvalues52plus(int n) {
    std::vector<LuaUpvalue> uvs;
    uvs.reserve(n);
    for (int i = 0; i < n; ++i) {
        LuaUpvalue uv;
        uv.inStack = readU8();
        uv.idx     = readU8();
        if (ver_ == LuaVersion::Lua54) uv.kind = readU8();
        uvs.push_back(uv);
    }
    return uvs;
}

// ─── Proto readers ───────────────────────────────────────────────────────────

void LuaReader::readDebugInfo51(LuaProto& proto) {
    // Line info
    int n = readInt();
    proto.lineInfo.reserve(n);
    for (int i = 0; i < n; ++i) proto.lineInfo.push_back(readInt());
    // Locals
    int nl = readInt();
    for (int i = 0; i < nl; ++i) {
        LuaLocal loc;
        loc.name     = readString51();
        loc.startPc  = readInt();
        loc.endPc    = readInt();
        proto.locals.push_back(std::move(loc));
    }
    // Upvalue names — must be read in full to keep the stream aligned,
    // even if proto.upvalues hasn't been pre-populated.
    int nu = readInt();
    for (int i = 0; i < nu; ++i) {
        std::string name = readString51();
        if (i < (int)proto.upvalues.size())
            proto.upvalues[i].name = std::move(name);
    }
}

void LuaReader::readDebugInfo52plus(LuaProto& proto) {
    // Line info
    int n = readInt();
    proto.lineInfo.reserve(n);
    for (int i = 0; i < n; ++i) proto.lineInfo.push_back(readInt());
    // Locals
    int nl = readInt();
    for (int i = 0; i < nl; ++i) {
        LuaLocal loc;
        loc.name     = readString();
        loc.startPc  = readInt();
        loc.endPc    = readInt();
        proto.locals.push_back(std::move(loc));
    }
    // Upvalue names — must be read in full to keep stream aligned.
    int nu = readInt();
    for (int i = 0; i < nu; ++i) {
        std::string name = readString();
        if (i < (int)proto.upvalues.size())
            proto.upvalues[i].name = std::move(name);
    }
}

void LuaReader::readDebugInfo54(LuaProto& proto) {
    // Lua 5.4 debug format (ldump.c DumpDebug):
    //   DumpInt(n_lineinfo) + n_lineinfo × raw int8_t bytes
    //   DumpInt(n_abslineinfo) + n × (DumpInt(pc) + DumpInt(line))
    //   DumpInt(n_locvars) + n × (DumpString(name) + DumpInt(startpc) + DumpInt(endpc))
    //   DumpInt(n_upvalue_names) + n × DumpString(name)

    // Line info: raw bytes (one signed byte per instruction = delta from previous abs line)
    int nLine = static_cast<int>(readLuaSize54());
    proto.lineInfo.reserve(nLine);
    for (int i = 0; i < nLine; ++i)
        proto.lineInfo.push_back(static_cast<int>(static_cast<int8_t>(readU8())));

    // Absolute line info: LEB128 pairs (pc, line)
    int nAbs = static_cast<int>(readLuaSize54());
    for (int i = 0; i < nAbs; ++i) {
        (void)readLuaSize54(); // pc
        (void)readLuaSize54(); // line
    }

    // Local variable names
    int nl = static_cast<int>(readLuaSize54());
    for (int i = 0; i < nl; ++i) {
        LuaLocal loc;
        loc.name    = readString54();
        loc.startPc = static_cast<int>(readLuaSize54());
        loc.endPc   = static_cast<int>(readLuaSize54());
        proto.locals.push_back(std::move(loc));
    }

    // Upvalue names — read all to keep stream aligned
    int nu = static_cast<int>(readLuaSize54());
    for (int i = 0; i < nu; ++i) {
        std::string name = readString54();
        if (i < (int)proto.upvalues.size())
            proto.upvalues[i].name = std::move(name);
    }
}

LuaProto LuaReader::readProto51() {
    LuaProto proto;
    proto.version = LuaVersion::Lua51;

    proto.source = readString51();
    proto.lineDefined     = readInt();
    proto.lastLineDefined = readInt();
    readU8(); // numUpvalues
    proto.numParams  = readU8();
    proto.isVarArg   = readU8() != 0;
    proto.maxStackSize = readU8();

    proto.code      = readCode();
    proto.constants = readConstants51();

    // Sub-protos
    int np = readInt();
    for (int i = 0; i < np; ++i)
        proto.protos.push_back(readProto51());

    readDebugInfo51(proto);
    return proto;
}

LuaProto LuaReader::readProto52() {
    LuaProto proto;
    proto.version = LuaVersion::Lua52;

    proto.lineDefined     = readInt();
    proto.lastLineDefined = readInt();
    int numUpvals = readU8();
    proto.numParams   = readU8();
    proto.isVarArg    = readU8() != 0;
    proto.maxStackSize = readU8();

    proto.code      = readCode();
    proto.constants = readConstants52();

    // Sub-protos
    int np = readInt();
    for (int i = 0; i < np; ++i)
        proto.protos.push_back(readProto52());

    proto.upvalues = readUpvalues52plus(numUpvals);
    proto.source = readString51();
    readDebugInfo52plus(proto);
    return proto;
}

LuaProto LuaReader::readProto53() {
    LuaProto proto;
    proto.version = LuaVersion::Lua53;

    proto.source = readString53();
    proto.lineDefined     = readInt();
    proto.lastLineDefined = readInt();
    proto.numParams   = readU8();
    proto.isVarArg    = readU8() != 0;
    proto.maxStackSize = readU8();

    proto.code      = readCode();
    proto.constants = readConstants53();

    // Upvalues count comes before sub-protos in 5.3
    int numUpvals = readInt();
    proto.upvalues = readUpvalues52plus(numUpvals);

    // Sub-protos
    int np = readInt();
    for (int i = 0; i < np; ++i)
        proto.protos.push_back(readProto53());

    readDebugInfo52plus(proto);
    return proto;
}

LuaProto LuaReader::readProto54() {
    LuaProto proto;
    proto.version = LuaVersion::Lua54;

    proto.source = readString54();
    proto.lineDefined     = readInt();
    proto.lastLineDefined = readInt();
    proto.numParams   = readU8();
    proto.isVarArg    = readU8() != 0;
    proto.maxStackSize = readU8();

    proto.code      = readCode();
    proto.constants = readConstants54();

    // Upvalues
    int numUpvals = readInt();
    proto.upvalues = readUpvalues52plus(numUpvals);

    // Sub-protos
    int np = readInt();
    for (int i = 0; i < np; ++i)
        proto.protos.push_back(readProto54());

    readDebugInfo54(proto);
    return proto;
}

// ─── read ────────────────────────────────────────────────────────────────────

LuaReadResult LuaReader::read() {
    LuaReadResult result;
    result.ok = false;

    try {
        LuaVersion ver = parseHeader();

        LuaModule mod;
        mod.version     = ver;
        mod.littleEndian = le_;
        mod.intSize     = intSz_;
        mod.sizetSize   = sizetSz_;

        switch (ver) {
        case LuaVersion::Lua51:
            mod.topLevel = readProto51();
            break;
        case LuaVersion::Lua52:
            // 5.2: top-level has no source in header
            mod.topLevel = readProto52();
            break;
        case LuaVersion::Lua53:
            // 5.3: skip upvalue count byte before top-level proto
            readU8(); // numUpvalues of main chunk (always 1: _ENV)
            mod.topLevel = readProto53();
            break;
        case LuaVersion::Lua54:
            readU8(); // sizeUpvalues
            mod.topLevel = readProto54();
            break;
        default:
            throw ParseError{"Unsupported Lua version"};
        }

        result.module   = std::move(mod);
        result.warnings = warnings_;
        result.ok       = true;

    } catch (const ParseError& e) {
        result.error    = e.msg;
        result.warnings = warnings_;
    } catch (const std::exception& e) {
        result.error    = e.what();
        result.warnings = warnings_;
    }

    return result;
}

} // namespace lua_parser
} // namespace retdec
