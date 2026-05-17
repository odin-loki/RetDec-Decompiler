/**
 * @file src/wasm_parser/wasm_reader.cpp
 * @brief WebAssembly binary format reader implementation.
 */

#include "retdec/wasm_parser/wasm_reader.h"

#include <cassert>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace retdec {
namespace wasm_parser {

// ─── Magic + version ─────────────────────────────────────────────────────────

static constexpr uint32_t kWasmMagic   = 0x6D736100; // '\0asm'
static constexpr uint32_t kWasmVersion = 0x00000001;

// ─── Constructor ─────────────────────────────────────────────────────────────

WasmReader::WasmReader(std::vector<uint8_t> bytes)
    : data_(std::move(bytes)) {}

WasmReader::WasmReader(const uint8_t* data, size_t size)
    : data_(data, data + size) {}

// ─── Primitive readers ────────────────────────────────────────────────────────

uint8_t WasmReader::readU8() {
    if (pos_ >= data_.size()) throw ParseError{"Unexpected end of file"};
    return data_[pos_++];
}

uint16_t WasmReader::readU16Le() {
    uint8_t lo = readU8(), hi = readU8();
    return static_cast<uint16_t>(lo | (hi << 8));
}

uint32_t WasmReader::readU32Le() {
    uint8_t a = readU8(), b = readU8(), c = readU8(), d = readU8();
    return (uint32_t)a | ((uint32_t)b<<8) | ((uint32_t)c<<16) | ((uint32_t)d<<24);
}

uint32_t WasmReader::readULEB128() {
    uint32_t result = 0;
    int      shift  = 0;
    while (true) {
        uint8_t byte = readU8();
        result |= (uint32_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
        if (shift >= 35) throw ParseError{"LEB128 overflow (u32)"};
    }
    return result;
}

uint64_t WasmReader::readULEB128_64() {
    uint64_t result = 0;
    int      shift  = 0;
    while (true) {
        uint8_t byte = readU8();
        result |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
        if (shift >= 70) throw ParseError{"LEB128 overflow (u64)"};
    }
    return result;
}

int32_t WasmReader::readSLEB128() {
    int32_t result = 0;
    int     shift  = 0;
    uint8_t byte   = 0;
    do {
        byte    = readU8();
        result |= (int32_t)(byte & 0x7F) << shift;
        shift  += 7;
    } while (byte & 0x80);
    // Sign extend
    if (shift < 32 && (byte & 0x40))
        result |= -(1 << shift);
    return result;
}

int64_t WasmReader::readSLEB128_64() {
    int64_t result = 0;
    int     shift  = 0;
    uint8_t byte   = 0;
    do {
        byte    = readU8();
        result |= (int64_t)(byte & 0x7F) << shift;
        shift  += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40))
        result |= -(int64_t(1) << shift);
    return result;
}

float WasmReader::readF32() {
    auto b = readBytes(4);
    float v;
    std::memcpy(&v, b.data(), 4);
    return v;
}

double WasmReader::readF64() {
    auto b = readBytes(8);
    double v;
    std::memcpy(&v, b.data(), 8);
    return v;
}

void WasmReader::skip(size_t n) {
    if (pos_ + n > data_.size()) throw ParseError{"Skip past end"};
    pos_ += n;
}

std::vector<uint8_t> WasmReader::readBytes(size_t n) {
    if (pos_ + n > data_.size()) throw ParseError{"Read past end"};
    std::vector<uint8_t> v(data_.begin() + pos_, data_.begin() + pos_ + n);
    pos_ += n;
    return v;
}

std::string WasmReader::readUtf8(uint32_t len) {
    if (pos_ + len > data_.size()) throw ParseError{"String read past end"};
    std::string s(reinterpret_cast<const char*>(&data_[pos_]), len);
    pos_ += len;
    return s;
}

// ─── Type readers ─────────────────────────────────────────────────────────────

ValType WasmReader::readValType() {
    uint8_t b = readU8();
    switch (b) {
    case 0x7F: return ValType::I32;
    case 0x7E: return ValType::I64;
    case 0x7D: return ValType::F32;
    case 0x7C: return ValType::F64;
    case 0x7B: return ValType::V128;
    case 0x70: return ValType::FuncRef;
    case 0x6F: return ValType::ExternRef;
    default:
        warn("Unknown valtype 0x" + std::to_string(b) + ", treating as i32");
        return ValType::I32;
    }
}

FuncType WasmReader::readFuncType() {
    uint8_t tag = readU8();
    if (tag != 0x60) throw ParseError{"Expected functype tag 0x60"};
    FuncType ft;
    uint32_t paramCount = readULEB128();
    for (uint32_t i = 0; i < paramCount; ++i)
        ft.params.push_back(readValType());
    uint32_t resultCount = readULEB128();
    for (uint32_t i = 0; i < resultCount; ++i)
        ft.results.push_back(readValType());
    return ft;
}

Limits WasmReader::readLimits() {
    Limits lim;
    uint8_t flags = readU8();
    lim.min    = readULEB128();
    lim.shared = (flags & 2) != 0;
    if (flags & 1) lim.max = readULEB128();
    return lim;
}

MemType WasmReader::readMemType() {
    return MemType{readLimits()};
}

TableType WasmReader::readTableType() {
    TableType tt;
    tt.refType = readValType();
    tt.limits  = readLimits();
    return tt;
}

GlobalType WasmReader::readGlobalType() {
    GlobalType gt;
    gt.valType   = readValType();
    gt.isMutable = (readU8() == 1);
    return gt;
}

std::vector<uint8_t> WasmReader::readConstExpr() {
    // Read bytes until END opcode (0x0B)
    std::vector<uint8_t> expr;
    while (!eof()) {
        uint8_t b = readU8();
        expr.push_back(b);
        if (b == 0x0B) break; // END
    }
    return expr;
}

// ─── Section parsers ─────────────────────────────────────────────────────────

void WasmReader::parseTypeSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i)
        mod.types.push_back(readFuncType());
}

void WasmReader::parseImportSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i) {
        Import imp;
        uint32_t modLen = readULEB128();
        imp.module = readUtf8(modLen);
        uint32_t nameLen = readULEB128();
        imp.name = readUtf8(nameLen);
        imp.kind = static_cast<ExternKind>(readU8());
        switch (imp.kind) {
        case ExternKind::Func:
            imp.index = readULEB128(); // type index
            break;
        case ExternKind::Table:
            imp.tableType = readTableType();
            imp.index = 0;
            break;
        case ExternKind::Memory:
            imp.memType = readMemType();
            imp.index = 0;
            break;
        case ExternKind::Global:
            imp.globalType = readGlobalType();
            imp.index = 0;
            break;
        default:
            throw ParseError{"Unknown import kind"};
        }
        mod.imports.push_back(std::move(imp));
    }
}

void WasmReader::parseFunctionSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i)
        mod.funcTypeIndices.push_back(readULEB128());
}

void WasmReader::parseTableSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i)
        mod.tables.push_back(readTableType());
}

void WasmReader::parseMemorySection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i)
        mod.memories.push_back(readMemType());
}

void WasmReader::parseGlobalSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i) {
        WasmGlobal g;
        g.type     = readGlobalType();
        g.initExpr = readConstExpr();
        mod.globals.push_back(std::move(g));
    }
}

void WasmReader::parseExportSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i) {
        Export exp;
        uint32_t nameLen = readULEB128();
        exp.name  = readUtf8(nameLen);
        exp.kind  = static_cast<ExternKind>(readU8());
        exp.index = readULEB128();
        mod.exports.push_back(std::move(exp));
    }
}

void WasmReader::parseStartSection(WasmModule& mod, uint32_t /*size*/) {
    mod.startFunc = readULEB128();
}

void WasmReader::parseElementSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i) {
        ElementSegment seg;
        uint32_t flags = readULEB128();

        if (flags == 0) {
            // Legacy: active, implicit table 0, funcref
            seg.offsetExpr = readConstExpr();
            uint32_t n = readULEB128();
            for (uint32_t j = 0; j < n; ++j)
                seg.funcIndices.push_back(readULEB128());
        } else if (flags == 1) {
            // Passive, elemkind funcref
            readU8(); // elemkind
            uint32_t n = readULEB128();
            for (uint32_t j = 0; j < n; ++j)
                seg.funcIndices.push_back(readULEB128());
            seg.isPassive = true;
        } else if (flags == 2) {
            // Active, explicit table
            seg.tableIndex = readULEB128();
            seg.offsetExpr = readConstExpr();
            readU8(); // elemkind
            uint32_t n = readULEB128();
            for (uint32_t j = 0; j < n; ++j)
                seg.funcIndices.push_back(readULEB128());
        } else if (flags == 4) {
            // Active, table 0, elem exprs
            seg.offsetExpr = readConstExpr();
            seg.refType    = ValType::FuncRef;
            uint32_t n = readULEB128();
            for (uint32_t j = 0; j < n; ++j)
                seg.elemExprs.push_back(readConstExpr());
        } else {
            // Skip unknown flags variant
            warn("Unknown element segment flags: " + std::to_string(flags));
            // best-effort: skip to end of section
            pos_ = end;
            break;
        }
        mod.elements.push_back(std::move(seg));
    }
}

void WasmReader::parseCodeSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i) {
        uint32_t bodySize = readULEB128();
        size_t   bodyEnd  = pos_ + bodySize;

        FuncCode fc;
        uint32_t localCount = readULEB128();
        for (uint32_t j = 0; j < localCount; ++j) {
            WasmLocal loc;
            loc.count = readULEB128();
            loc.type  = readValType();
            fc.locals.push_back(loc);
        }
        size_t exprLen = bodyEnd > pos_ ? bodyEnd - pos_ : 0;
        fc.body = readBytes(exprLen);
        mod.codes.push_back(std::move(fc));
        pos_ = bodyEnd; // ensure alignment
    }
}

void WasmReader::parseDataSection(WasmModule& mod, uint32_t size) {
    size_t end = pos_ + size;
    uint32_t count = readULEB128();
    for (uint32_t i = 0; i < count && pos_ < end; ++i) {
        DataSegment seg;
        uint32_t flags = readULEB128();
        if (flags == 0) {
            // Active, memory 0
            seg.offsetExpr = readConstExpr();
        } else if (flags == 1) {
            // Passive
            seg.isPassive = true;
        } else if (flags == 2) {
            // Active, explicit memory
            seg.memIndex   = readULEB128();
            seg.offsetExpr = readConstExpr();
        }
        uint32_t len = readULEB128();
        seg.bytes = readBytes(len);
        mod.dataSegments.push_back(std::move(seg));
    }
}

void WasmReader::parseDataCountSection(WasmModule& /*mod*/, uint32_t /*size*/) {
    readULEB128(); // data count; informational only
}

void WasmReader::parseCustomSection(WasmModule& mod, uint32_t size) {
    size_t sectionEnd = pos_ + size;
    uint32_t nameLen  = readULEB128();
    std::string name  = readUtf8(nameLen);

    size_t dataSize = sectionEnd > pos_ ? sectionEnd - pos_ : 0;
    std::vector<uint8_t> sectionData = readBytes(dataSize);

    if (name == "name") {
        parseNameSection(mod, sectionData);
    } else {
        CustomSection cs;
        cs.name = std::move(name);
        cs.data = std::move(sectionData);
        mod.customSections.push_back(std::move(cs));
    }
    pos_ = sectionEnd;
}

// ─── Name section ────────────────────────────────────────────────────────────

static uint32_t readULEB_local(const uint8_t* d, size_t& pos, size_t end) {
    uint32_t result = 0;
    int shift = 0;
    while (pos < end) {
        uint8_t b = d[pos++];
        result |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return result;
}

static uint32_t readULEB_local_u32(const uint8_t* d, size_t& pos, size_t end) {
    return readULEB_local(d, pos, end);
}

std::vector<NameMap> WasmReader::readNameMap(const uint8_t* d,
                                               size_t& pos, size_t end) {
    uint32_t count = readULEB_local(d, pos, end);
    std::vector<NameMap> nms;
    for (uint32_t i = 0; i < count && pos < end; ++i) {
        NameMap nm;
        nm.index = readULEB_local(d, pos, end);
        uint32_t nameLen = readULEB_local(d, pos, end);
        if (pos + nameLen <= end) {
            nm.name = std::string(reinterpret_cast<const char*>(d + pos), nameLen);
            pos += nameLen;
        }
        nms.push_back(std::move(nm));
    }
    return nms;
}

std::vector<IndirectNameMap> WasmReader::readIndirectNameMap(
        const uint8_t* d, size_t& pos, size_t end) {
    uint32_t count = readULEB_local(d, pos, end);
    std::vector<IndirectNameMap> nms;
    for (uint32_t i = 0; i < count && pos < end; ++i) {
        IndirectNameMap nm;
        nm.index = readULEB_local(d, pos, end);
        nm.names = readNameMap(d, pos, end);
        nms.push_back(std::move(nm));
    }
    return nms;
}

void WasmReader::parseNameSection(WasmModule& mod,
                                    const std::vector<uint8_t>& data) {
    NameSection ns;
    size_t pos = 0;
    const uint8_t* d = data.data();
    size_t end = data.size();

    while (pos < end) {
        if (pos + 1 > end) break;
        uint8_t subsectionId = d[pos++];
        uint32_t subsectionLen = readULEB_local(d, pos, end);
        size_t subsectionEnd = pos + subsectionLen;
        if (subsectionEnd > end) subsectionEnd = end;

        switch (subsectionId) {
        case 0: { // module name
            uint32_t nameLen = readULEB_local(d, pos, subsectionEnd);
            if (pos + nameLen <= subsectionEnd) {
                ns.moduleName = std::string(
                    reinterpret_cast<const char*>(d + pos), nameLen);
                pos += nameLen;
            }
            break;
        }
        case 1: // func names
            ns.funcNames = readNameMap(d, pos, subsectionEnd);
            break;
        case 2: // local names
            ns.localNames = readIndirectNameMap(d, pos, subsectionEnd);
            break;
        case 7: // global names
            ns.globalNames = readNameMap(d, pos, subsectionEnd);
            break;
        default:
            break;
        }
        pos = subsectionEnd;
    }

    mod.names = std::move(ns);
}

// ─── read (top-level) ────────────────────────────────────────────────────────

WasmReadResult WasmReader::read() {
    WasmReadResult result;
    result.ok = false;

    try {
        // Magic + version
        uint32_t magic   = readU32Le();
        uint32_t version = readU32Le();
        if (magic != kWasmMagic)
            throw ParseError{"Not a WebAssembly binary (bad magic)"};
        if (version != kWasmVersion)
            warn("Unexpected Wasm version: " + std::to_string(version));

        WasmModule mod;

        // Parse sections
        while (!eof()) {
            uint8_t  secId  = readU8();
            uint32_t secLen = readULEB128();
            size_t   secEnd = pos_ + secLen;

            switch (static_cast<SectionId>(secId)) {
            case SectionId::Custom:    parseCustomSection(mod, secLen); break;
            case SectionId::Type:      parseTypeSection(mod, secLen); break;
            case SectionId::Import:    parseImportSection(mod, secLen); break;
            case SectionId::Function:  parseFunctionSection(mod, secLen); break;
            case SectionId::Table:     parseTableSection(mod, secLen); break;
            case SectionId::Memory:    parseMemorySection(mod, secLen); break;
            case SectionId::Global:    parseGlobalSection(mod, secLen); break;
            case SectionId::Export:    parseExportSection(mod, secLen); break;
            case SectionId::Start:     parseStartSection(mod, secLen); break;
            case SectionId::Element:   parseElementSection(mod, secLen); break;
            case SectionId::Code:      parseCodeSection(mod, secLen); break;
            case SectionId::Data:      parseDataSection(mod, secLen); break;
            case SectionId::DataCount: parseDataCountSection(mod, secLen); break;
            default:
                warn("Unknown section id: " + std::to_string(secId));
                break;
            }
            pos_ = secEnd; // ensure we advance
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

} // namespace wasm_parser
} // namespace retdec
