/**
 * @file src/wasm_parser/wat_emitter.cpp
 * @brief WebAssembly Text Format (.wat) emitter implementation.
 */

#include "retdec/wasm_parser/wat_emitter.h"

#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace wasm_parser {

// ─── Helpers ─────────────────────────────────────────────────────────────────

WatEmitter::WatEmitter(WatEmitOptions opts)
    : opts_(std::move(opts)) {}

std::string WatEmitter::indentStr(int level) const {
    return std::string(level * opts_.indentWidth, ' ');
}

std::string WatEmitter::valTypeName(ValType vt) const {
    return valTypeStr(vt);
}

std::string WatEmitter::limitsStr(const Limits& lim) const {
    std::string s = std::to_string(lim.min);
    if (lim.max.has_value())
        s += " " + std::to_string(*lim.max);
    if (lim.shared) s += " shared";
    return s;
}

// Encode data as WAT quoted string
std::string WatEmitter::dataStr(const std::vector<uint8_t>& bytes) const {
    std::ostringstream ss;
    ss << '"';
    for (uint8_t b : bytes) {
        if (b == '"')  ss << "\\\"";
        else if (b == '\\') ss << "\\\\";
        else if (b >= 0x20 && b < 0x7F) ss << (char)b;
        else {
            ss << '\\' << std::hex << std::setw(2) << std::setfill('0')
               << (unsigned)b << std::dec;
        }
    }
    ss << '"';
    return ss.str();
}

// Decode a constant expression (init_expr) to WAT
std::string WatEmitter::constExprStr(const WasmModule& /*mod*/,
                                       const std::vector<uint8_t>& expr) const {
    if (expr.empty()) return "i32.const 0";
    uint8_t op = expr[0];
    switch (op) {
    case 0x41: { // i32.const
        // read SLEB128
        int32_t val = 0; int shift = 0; int i = 1;
        uint8_t b;
        do {
            if (i >= (int)expr.size()) break;
            b = expr[i++];
            val |= (int32_t)(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        if (shift < 32 && (b & 0x40)) val |= -(1 << shift);
        return "i32.const " + std::to_string(val);
    }
    case 0x42: { // i64.const
        int64_t val = 0; int shift = 0; int i = 1;
        uint8_t b;
        do {
            if (i >= (int)expr.size()) break;
            b = expr[i++];
            val |= (int64_t)(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) val |= -(int64_t(1) << shift);
        return "i64.const " + std::to_string(val);
    }
    case 0x43: {
        if (expr.size() >= 5) {
            float v; std::memcpy(&v, &expr[1], 4);
            std::ostringstream ss;
            ss << "f32.const " << v;
            return ss.str();
        }
        return "f32.const 0.0";
    }
    case 0x44: {
        if (expr.size() >= 9) {
            double v; std::memcpy(&v, &expr[1], 8);
            std::ostringstream ss;
            ss << "f64.const " << v;
            return ss.str();
        }
        return "f64.const 0.0";
    }
    case 0xD0: return "ref.null func";
    case 0xD1: return "ref.null extern";
    case 0xD2: { // ref.func idx
        uint32_t idx = 0; int i = 1; int shift = 0; uint8_t b;
        do {
            if (i >= (int)expr.size()) break;
            b = expr[i++]; idx |= (uint32_t)(b & 0x7F) << shift; shift += 7;
        } while (b & 0x80);
        return "ref.func " + std::to_string(idx);
    }
    case 0x23: { // global.get
        uint32_t idx = 0; int i = 1; int shift = 0; uint8_t b;
        do {
            if (i >= (int)expr.size()) break;
            b = expr[i++]; idx |= (uint32_t)(b & 0x7F) << shift; shift += 7;
        } while (b & 0x80);
        return "global.get " + std::to_string(idx);
    }
    default:
        return ";; unknown init expr 0x" + [&](){
            std::ostringstream s; s << std::hex << (int)op; return s.str(); }();
    }
}

// ─── Name helpers ─────────────────────────────────────────────────────────────

static bool isValidId(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '$' &&
            c != '.' && c != '-' && c != '+' && c != '*' && c != '/' &&
            c != '\\' && c != '^' && c != '~' && c != '=' && c != '<' &&
            c != '>' && c != '!' && c != '?' && c != '@' && c != '#' &&
            c != '&' && c != '|' && c != ':' && c != '`' && c != '\'')
            return false;
    }
    return true;
}

static std::string safeId(const std::string& s) {
    if (isValidId(s)) return "$" + s;
    // Escape non-identifier characters
    std::string r = "$";
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c == '_') r += c;
        else r += '_';
    }
    return r;
}

std::string WatEmitter::funcId(const WasmModule& mod, uint32_t funcIdx) const {
    if (!opts_.useNames) return std::to_string(funcIdx);
    std::string n = mod.funcName(funcIdx);
    if (n == "func" + std::to_string(funcIdx)) return std::to_string(funcIdx);
    return safeId(n);
}

std::string WatEmitter::globalId(const WasmModule& mod, uint32_t idx) const {
    if (!opts_.useNames) return std::to_string(idx);
    std::string n = mod.globalName(idx);
    if (n == "global" + std::to_string(idx)) return std::to_string(idx);
    return safeId(n);
}

std::string WatEmitter::tableId(const WasmModule& /*mod*/, uint32_t idx) const {
    return std::to_string(idx);
}

std::string WatEmitter::memId(const WasmModule& /*mod*/, uint32_t idx) const {
    return std::to_string(idx);
}

std::string WatEmitter::localId(const WasmModule& mod, uint32_t funcIdx,
                                   uint32_t localIdx) const {
    if (!opts_.useNames || !mod.names.has_value())
        return std::to_string(localIdx);
    for (const auto& inm : mod.names->localNames) {
        if (inm.index == funcIdx) {
            for (const auto& nm : inm.names) {
                if (nm.index == localIdx) return safeId(nm.name);
            }
        }
    }
    return std::to_string(localIdx);
}

std::string WatEmitter::funcTypeSig(const FuncType& ft) const {
    std::string s;
    for (auto p : ft.params)   s += " (param " + valTypeName(p) + ")";
    for (auto r : ft.results)  s += " (result " + valTypeName(r) + ")";
    return s;
}

std::string WatEmitter::funcTypeRef(const WasmModule& mod,
                                       uint32_t typeIdx) const {
    if (opts_.foldTypes) return "(type " + std::to_string(typeIdx) + ")";
    if (typeIdx < mod.types.size()) return funcTypeSig(mod.types[typeIdx]);
    return "";
}

std::string WatEmitter::exportAnnotations(const WasmModule& mod,
                                            ExternKind kind, uint32_t idx) const {
    if (!opts_.inlineExports) return "";
    std::string s;
    for (const auto& exp : mod.exports) {
        if (exp.kind == kind && exp.index == idx)
            s += " (export " + dataStr({exp.name.begin(), exp.name.end()}) + ")";
    }
    return s;
}

std::string WatEmitter::importAnnotation(const Import& imp) const {
    std::string modStr  = dataStr({imp.module.begin(), imp.module.end()});
    std::string nameStr = dataStr({imp.name.begin(), imp.name.end()});
    return " (import " + modStr + " " + nameStr + ")";
}

// ─── Section emitters ────────────────────────────────────────────────────────

void WatEmitter::emitTypeSection(const WasmModule& mod, std::ostream& out,
                                   int indent) const {
    if (!opts_.foldTypes) return;
    for (size_t i = 0; i < mod.types.size(); ++i) {
        out << indentStr(indent) << "(type (;" << i << ";) (func"
            << funcTypeSig(mod.types[i]) << "))\n";
    }
}

void WatEmitter::emitImports(const WasmModule& mod, std::ostream& out,
                               int indent) const {
    if (!opts_.inlineImports) {
        // Emit separate (import ...) forms
        for (const auto& imp : mod.imports) {
            std::string modStr  = dataStr({imp.module.begin(), imp.module.end()});
            std::string nameStr = dataStr({imp.name.begin(), imp.name.end()});
            out << indentStr(indent) << "(import " << modStr << " " << nameStr;
            switch (imp.kind) {
            case ExternKind::Func:
                out << " (func " << funcTypeRef(mod, imp.index) << ")";
                break;
            case ExternKind::Table:
                if (imp.tableType)
                    out << " (table " << limitsStr(imp.tableType->limits)
                        << " " << valTypeName(imp.tableType->refType) << ")";
                break;
            case ExternKind::Memory:
                if (imp.memType)
                    out << " (memory " << limitsStr(imp.memType->limits) << ")";
                break;
            case ExternKind::Global:
                if (imp.globalType)
                    out << " (global "
                        << (imp.globalType->isMutable ? "(mut " : "")
                        << valTypeName(imp.globalType->valType)
                        << (imp.globalType->isMutable ? ")" : "") << ")";
                break;
            }
            out << ")\n";
        }
    }
    // If inline, imports are emitted inline with the func/table/mem/global decl.
}

void WatEmitter::emitFunctions(const WasmModule& mod, std::ostream& out,
                                  int indent) const {
    uint32_t importedFuncs = mod.importedFuncCount();
    uint32_t impIdx = 0;

    // Imported functions inline form
    if (opts_.inlineImports) {
        for (const auto& imp : mod.imports) {
            if (imp.kind != ExternKind::Func) { ++impIdx; continue; }
            out << indentStr(indent) << "(func " << funcId(mod, impIdx)
                << importAnnotation(imp)
                << exportAnnotations(mod, ExternKind::Func, impIdx)
                << " " << funcTypeRef(mod, imp.index) << ")\n";
            ++impIdx;
        }
    }

    // Defined functions
    for (size_t ci = 0; ci < mod.codes.size(); ++ci) {
        uint32_t funcIdx = importedFuncs + (uint32_t)ci;
        uint32_t typeIdx = funcIdx < mod.totalFuncCount()
                               ? mod.funcTypeIndex(funcIdx) : 0;
        const FuncCode& fc = mod.codes[ci];

        out << indentStr(indent) << "(func " << funcId(mod, funcIdx)
            << exportAnnotations(mod, ExternKind::Func, funcIdx)
            << " " << funcTypeRef(mod, typeIdx);

        // Params (from type)
        if (typeIdx < mod.types.size()) {
            const FuncType& ft = mod.types[typeIdx];
            uint32_t paramCount = (uint32_t)ft.params.size();
            for (uint32_t pi = 0; pi < paramCount; ++pi) {
                std::string lid = localId(mod, funcIdx, pi);
                out << "\n" << indentStr(indent + 1)
                    << "(param " << lid << " " << valTypeName(ft.params[pi]) << ")";
            }

            // Locals
            uint32_t localOffset = paramCount;
            for (const auto& lc : fc.locals) {
                for (uint32_t li = 0; li < lc.count; ++li) {
                    std::string lid = localId(mod, funcIdx, localOffset + li);
                    out << "\n" << indentStr(indent + 1)
                        << "(local " << lid << " " << valTypeName(lc.type) << ")";
                }
                localOffset += lc.count;
            }

            // Body
            emitFuncBody(mod, funcIdx, fc, out, indent + 1);
        }

        out << ")\n";
    }
}

void WatEmitter::emitTables(const WasmModule& mod, std::ostream& out,
                              int indent) const {
    // Imported tables (inline)
    if (opts_.inlineImports) {
        uint32_t tIdx = 0;
        for (const auto& imp : mod.imports) {
            if (imp.kind != ExternKind::Table) continue;
            out << indentStr(indent) << "(table " << tableId(mod, tIdx)
                << importAnnotation(imp)
                << exportAnnotations(mod, ExternKind::Table, tIdx);
            if (imp.tableType)
                out << " " << limitsStr(imp.tableType->limits)
                    << " " << valTypeName(imp.tableType->refType);
            out << ")\n";
            ++tIdx;
        }
    }
    // Defined tables
    uint32_t importedTables = 0;
    for (const auto& imp : mod.imports)
        if (imp.kind == ExternKind::Table) ++importedTables;

    for (size_t i = 0; i < mod.tables.size(); ++i) {
        uint32_t tIdx = importedTables + (uint32_t)i;
        const TableType& tt = mod.tables[i];
        out << indentStr(indent) << "(table " << tableId(mod, tIdx)
            << exportAnnotations(mod, ExternKind::Table, tIdx)
            << " " << limitsStr(tt.limits)
            << " " << valTypeName(tt.refType) << ")\n";
    }
}

void WatEmitter::emitMemories(const WasmModule& mod, std::ostream& out,
                                int indent) const {
    if (opts_.inlineImports) {
        uint32_t mIdx = 0;
        for (const auto& imp : mod.imports) {
            if (imp.kind != ExternKind::Memory) continue;
            out << indentStr(indent) << "(memory " << memId(mod, mIdx)
                << importAnnotation(imp)
                << exportAnnotations(mod, ExternKind::Memory, mIdx);
            if (imp.memType)
                out << " " << limitsStr(imp.memType->limits);
            out << ")\n";
            ++mIdx;
        }
    }
    uint32_t importedMems = 0;
    for (const auto& imp : mod.imports)
        if (imp.kind == ExternKind::Memory) ++importedMems;

    for (size_t i = 0; i < mod.memories.size(); ++i) {
        uint32_t mIdx = importedMems + (uint32_t)i;
        out << indentStr(indent) << "(memory " << memId(mod, mIdx)
            << exportAnnotations(mod, ExternKind::Memory, mIdx)
            << " " << limitsStr(mod.memories[i].limits) << ")\n";
    }
}

void WatEmitter::emitGlobals(const WasmModule& mod, std::ostream& out,
                               int indent) const {
    if (opts_.inlineImports) {
        uint32_t gIdx = 0;
        for (const auto& imp : mod.imports) {
            if (imp.kind != ExternKind::Global) continue;
            out << indentStr(indent) << "(global " << globalId(mod, gIdx)
                << importAnnotation(imp)
                << exportAnnotations(mod, ExternKind::Global, gIdx);
            if (imp.globalType) {
                const auto& gt = *imp.globalType;
                if (gt.isMutable) out << " (mut " << valTypeName(gt.valType) << ")";
                else              out << " " << valTypeName(gt.valType);
            }
            out << ")\n";
            ++gIdx;
        }
    }
    uint32_t importedGlobals = 0;
    for (const auto& imp : mod.imports)
        if (imp.kind == ExternKind::Global) ++importedGlobals;

    for (size_t i = 0; i < mod.globals.size(); ++i) {
        uint32_t gIdx = importedGlobals + (uint32_t)i;
        const WasmGlobal& g = mod.globals[i];
        out << indentStr(indent) << "(global " << globalId(mod, gIdx)
            << exportAnnotations(mod, ExternKind::Global, gIdx);
        if (g.type.isMutable)
            out << " (mut " << valTypeName(g.type.valType) << ")";
        else
            out << " " << valTypeName(g.type.valType);
        out << " (" << constExprStr(mod, g.initExpr) << "))\n";
    }
}

void WatEmitter::emitExports(const WasmModule& mod, std::ostream& out,
                               int indent) const {
    if (opts_.inlineExports) return; // Already inlined
    for (const auto& exp : mod.exports) {
        std::string kindStr;
        switch (exp.kind) {
        case ExternKind::Func:   kindStr = "func";   break;
        case ExternKind::Table:  kindStr = "table";  break;
        case ExternKind::Memory: kindStr = "memory"; break;
        case ExternKind::Global: kindStr = "global"; break;
        }
        out << indentStr(indent) << "(export "
            << dataStr({exp.name.begin(), exp.name.end()})
            << " (" << kindStr << " " << exp.index << "))\n";
    }
}

void WatEmitter::emitStart(const WasmModule& mod, std::ostream& out,
                             int indent) const {
    if (mod.startFunc.has_value())
        out << indentStr(indent) << "(start " << funcId(mod, *mod.startFunc) << ")\n";
}

void WatEmitter::emitElements(const WasmModule& mod, std::ostream& out,
                                int indent) const {
    for (size_t i = 0; i < mod.elements.size(); ++i) {
        const ElementSegment& seg = mod.elements[i];
        out << indentStr(indent) << "(elem (;" << i << ";)";
        if (!seg.isPassive && !seg.isDeclarative) {
            out << " (table " << seg.tableIndex << ")";
            if (!seg.offsetExpr.empty())
                out << " (" << constExprStr(mod, seg.offsetExpr) << ")";
        } else if (seg.isPassive) {
            out << " passive";
        }
        out << " " << valTypeName(seg.refType);
        for (uint32_t fi : seg.funcIndices)
            out << " (ref.func " << fi << ")";
        out << ")\n";
    }
}

void WatEmitter::emitDataSegments(const WasmModule& mod, std::ostream& out,
                                    int indent) const {
    for (size_t i = 0; i < mod.dataSegments.size(); ++i) {
        const DataSegment& seg = mod.dataSegments[i];
        out << indentStr(indent) << "(data (;" << i << ";)";
        if (!seg.isPassive) {
            out << " (memory " << seg.memIndex << ")";
            if (!seg.offsetExpr.empty())
                out << " (" << constExprStr(mod, seg.offsetExpr) << ")";
        }
        out << " " << dataStr(seg.bytes) << ")\n";
    }
}

// ─── Function body disassembler ───────────────────────────────────────────────

// Read ULEB128 from a byte vector at position pc
static uint32_t readVecULEB(const std::vector<uint8_t>& v, size_t& pc) {
    uint32_t r = 0; int s = 0;
    while (pc < v.size()) {
        uint8_t b = v[pc++];
        r |= (uint32_t)(b & 0x7F) << s;
        if (!(b & 0x80)) break;
        s += 7;
    }
    return r;
}
static int32_t readVecSLEB(const std::vector<uint8_t>& v, size_t& pc) {
    int32_t r = 0; int s = 0; uint8_t b = 0;
    while (pc < v.size()) {
        b = v[pc++]; r |= (int32_t)(b & 0x7F) << s; s += 7;
        if (!(b & 0x80)) break;
    }
    if (s < 32 && (b & 0x40)) r |= -(1 << s);
    return r;
}
static int64_t readVecSLEB64(const std::vector<uint8_t>& v, size_t& pc) {
    int64_t r = 0; int s = 0; uint8_t b = 0;
    while (pc < v.size()) {
        b = v[pc++]; r |= (int64_t)(b & 0x7F) << s; s += 7;
        if (!(b & 0x80)) break;
    }
    if (s < 64 && (b & 0x40)) r |= -(int64_t(1) << s);
    return r;
}
static float readVecF32(const std::vector<uint8_t>& v, size_t& pc) {
    float r; if (pc + 4 <= v.size()) { std::memcpy(&r, &v[pc], 4); pc += 4; }
    else { r = 0.0f; pc = v.size(); }
    return r;
}
static double readVecF64(const std::vector<uint8_t>& v, size_t& pc) {
    double r; if (pc + 8 <= v.size()) { std::memcpy(&r, &v[pc], 8); pc += 8; }
    else { r = 0.0; pc = v.size(); }
    return r;
}

// Memory argument (align + offset)
static std::string memArg(const std::vector<uint8_t>& code, size_t& pc) {
    uint32_t align  = readVecULEB(code, pc);
    uint32_t offset = readVecULEB(code, pc);
    std::string s;
    if (offset) s += " offset=" + std::to_string(offset);
    if (align)  s += " align=" + std::to_string(1u << align);
    return s;
}

// Block type
static std::string blockType(const std::vector<uint8_t>& code, size_t& pc) {
    if (pc >= code.size()) return "";
    uint8_t b = code[pc];
    if (b == 0x40) { ++pc; return ""; } // empty
    if (b == 0x7F) { ++pc; return " (result i32)"; }
    if (b == 0x7E) { ++pc; return " (result i64)"; }
    if (b == 0x7D) { ++pc; return " (result f32)"; }
    if (b == 0x7C) { ++pc; return " (result f64)"; }
    if (b == 0x7B) { ++pc; return " (result v128)"; }
    if (b == 0x70) { ++pc; return " (result funcref)"; }
    if (b == 0x6F) { ++pc; return " (result externref)"; }
    // Otherwise it's a type index (s33)
    int32_t idx = readVecSLEB(code, pc);
    return " (type " + std::to_string(idx) + ")";
}

std::string WatEmitter::decodeInstr(DisState& st,
                                      const std::vector<uint8_t>& code,
                                      size_t& pc, int indent) const {
    if (pc >= code.size()) return "";
    uint8_t op = code[pc++];
    std::ostringstream line;
    line << indentStr(indent);

    switch (op) {
    case 0x00: line << "unreachable"; break;
    case 0x01: line << "nop"; break;
    case 0x02: {
        std::string bt = blockType(code, pc);
        st.blockDepth++;
        line << "block" << bt;
        break;
    }
    case 0x03: {
        std::string bt = blockType(code, pc);
        st.blockDepth++;
        line << "loop" << bt;
        break;
    }
    case 0x04: {
        std::string bt = blockType(code, pc);
        st.blockDepth++;
        line << "if" << bt;
        break;
    }
    case 0x05: line << "else"; break;
    case 0x0B:
        if (st.blockDepth > 0) { st.blockDepth--; line << "end"; }
        else line << ";; end"; // function end
        break;
    case 0x0C: line << "br "         << readVecULEB(code, pc); break;
    case 0x0D: line << "br_if "      << readVecULEB(code, pc); break;
    case 0x0E: {
        uint32_t n = readVecULEB(code, pc);
        line << "br_table";
        for (uint32_t i = 0; i <= n; ++i) line << " " << readVecULEB(code, pc);
        break;
    }
    case 0x0F: line << "return"; break;
    case 0x10: line << "call "         << funcId(st.mod, readVecULEB(code, pc)); break;
    case 0x11: {
        uint32_t typeIdx  = readVecULEB(code, pc);
        uint32_t tableIdx = readVecULEB(code, pc);
        line << "call_indirect (type " << typeIdx << ") " << tableIdx;
        break;
    }
    case 0x12: line << "return_call "          << funcId(st.mod, readVecULEB(code, pc)); break;
    case 0x13: {
        uint32_t typeIdx = readVecULEB(code, pc);
        uint32_t tblIdx  = readVecULEB(code, pc);
        line << "return_call_indirect (type " << typeIdx << ") " << tblIdx;
        break;
    }
    case 0x1A: line << "drop"; break;
    case 0x1B: line << "select"; break;
    case 0x20: line << "local.get "  << localId(st.mod, st.funcIdx, readVecULEB(code, pc)); break;
    case 0x21: line << "local.set "  << localId(st.mod, st.funcIdx, readVecULEB(code, pc)); break;
    case 0x22: line << "local.tee "  << localId(st.mod, st.funcIdx, readVecULEB(code, pc)); break;
    case 0x23: line << "global.get " << globalId(st.mod, readVecULEB(code, pc)); break;
    case 0x24: line << "global.set " << globalId(st.mod, readVecULEB(code, pc)); break;
    case 0x25: line << "table.get "  << readVecULEB(code, pc); break;
    case 0x26: line << "table.set "  << readVecULEB(code, pc); break;

    // Memory instructions
    case 0x28: line << "i32.load"    << memArg(code, pc); break;
    case 0x29: line << "i64.load"    << memArg(code, pc); break;
    case 0x2A: line << "f32.load"    << memArg(code, pc); break;
    case 0x2B: line << "f64.load"    << memArg(code, pc); break;
    case 0x2C: line << "i32.load8_s" << memArg(code, pc); break;
    case 0x2D: line << "i32.load8_u" << memArg(code, pc); break;
    case 0x2E: line << "i32.load16_s"<< memArg(code, pc); break;
    case 0x2F: line << "i32.load16_u"<< memArg(code, pc); break;
    case 0x30: line << "i64.load8_s" << memArg(code, pc); break;
    case 0x31: line << "i64.load8_u" << memArg(code, pc); break;
    case 0x32: line << "i64.load16_s"<< memArg(code, pc); break;
    case 0x33: line << "i64.load16_u"<< memArg(code, pc); break;
    case 0x34: line << "i64.load32_s"<< memArg(code, pc); break;
    case 0x35: line << "i64.load32_u"<< memArg(code, pc); break;
    case 0x36: line << "i32.store"   << memArg(code, pc); break;
    case 0x37: line << "i64.store"   << memArg(code, pc); break;
    case 0x38: line << "f32.store"   << memArg(code, pc); break;
    case 0x39: line << "f64.store"   << memArg(code, pc); break;
    case 0x3A: line << "i32.store8"  << memArg(code, pc); break;
    case 0x3B: line << "i32.store16" << memArg(code, pc); break;
    case 0x3C: line << "i64.store8"  << memArg(code, pc); break;
    case 0x3D: line << "i64.store16" << memArg(code, pc); break;
    case 0x3E: line << "i64.store32" << memArg(code, pc); break;
    case 0x3F: { readVecULEB(code, pc); line << "memory.size"; break; }
    case 0x40: { readVecULEB(code, pc); line << "memory.grow"; break; }

    // Constants
    case 0x41: line << "i32.const " << readVecSLEB(code, pc); break;
    case 0x42: line << "i64.const " << readVecSLEB64(code, pc); break;
    case 0x43: { float v = readVecF32(code, pc); line << "f32.const " << v; break; }
    case 0x44: { double v = readVecF64(code, pc); line << "f64.const " << v; break; }

    // i32 ops
    case 0x45: line << "i32.eqz";  break;
    case 0x46: line << "i32.eq";   break;
    case 0x47: line << "i32.ne";   break;
    case 0x48: line << "i32.lt_s"; break;
    case 0x49: line << "i32.lt_u"; break;
    case 0x4A: line << "i32.gt_s"; break;
    case 0x4B: line << "i32.gt_u"; break;
    case 0x4C: line << "i32.le_s"; break;
    case 0x4D: line << "i32.le_u"; break;
    case 0x4E: line << "i32.ge_s"; break;
    case 0x4F: line << "i32.ge_u"; break;
    // i64 ops
    case 0x50: line << "i64.eqz";  break;
    case 0x51: line << "i64.eq";   break;
    case 0x52: line << "i64.ne";   break;
    case 0x53: line << "i64.lt_s"; break;
    case 0x54: line << "i64.lt_u"; break;
    case 0x55: line << "i64.gt_s"; break;
    case 0x56: line << "i64.gt_u"; break;
    case 0x57: line << "i64.le_s"; break;
    case 0x58: line << "i64.le_u"; break;
    case 0x59: line << "i64.ge_s"; break;
    case 0x5A: line << "i64.ge_u"; break;
    // f32 ops
    case 0x5B: line << "f32.eq"; break;
    case 0x5C: line << "f32.ne"; break;
    case 0x5D: line << "f32.lt"; break;
    case 0x5E: line << "f32.gt"; break;
    case 0x5F: line << "f32.le"; break;
    case 0x60: line << "f32.ge"; break;
    // f64 ops
    case 0x61: line << "f64.eq"; break;
    case 0x62: line << "f64.ne"; break;
    case 0x63: line << "f64.lt"; break;
    case 0x64: line << "f64.gt"; break;
    case 0x65: line << "f64.le"; break;
    case 0x66: line << "f64.ge"; break;
    // i32 arith
    case 0x67: line << "i32.clz";    break;
    case 0x68: line << "i32.ctz";    break;
    case 0x69: line << "i32.popcnt"; break;
    case 0x6A: line << "i32.add";    break;
    case 0x6B: line << "i32.sub";    break;
    case 0x6C: line << "i32.mul";    break;
    case 0x6D: line << "i32.div_s";  break;
    case 0x6E: line << "i32.div_u";  break;
    case 0x6F: line << "i32.rem_s";  break;
    case 0x70: line << "i32.rem_u";  break;
    case 0x71: line << "i32.and";    break;
    case 0x72: line << "i32.or";     break;
    case 0x73: line << "i32.xor";    break;
    case 0x74: line << "i32.shl";    break;
    case 0x75: line << "i32.shr_s";  break;
    case 0x76: line << "i32.shr_u";  break;
    case 0x77: line << "i32.rotl";   break;
    case 0x78: line << "i32.rotr";   break;
    // i64 arith
    case 0x79: line << "i64.clz";    break;
    case 0x7A: line << "i64.ctz";    break;
    case 0x7B: line << "i64.popcnt"; break;
    case 0x7C: line << "i64.add";    break;
    case 0x7D: line << "i64.sub";    break;
    case 0x7E: line << "i64.mul";    break;
    case 0x7F: line << "i64.div_s";  break;
    case 0x80: line << "i64.div_u";  break;
    case 0x81: line << "i64.rem_s";  break;
    case 0x82: line << "i64.rem_u";  break;
    case 0x83: line << "i64.and";    break;
    case 0x84: line << "i64.or";     break;
    case 0x85: line << "i64.xor";    break;
    case 0x86: line << "i64.shl";    break;
    case 0x87: line << "i64.shr_s";  break;
    case 0x88: line << "i64.shr_u";  break;
    case 0x89: line << "i64.rotl";   break;
    case 0x8A: line << "i64.rotr";   break;
    // f32 arith
    case 0x8B: line << "f32.abs";     break;
    case 0x8C: line << "f32.neg";     break;
    case 0x8D: line << "f32.ceil";    break;
    case 0x8E: line << "f32.floor";   break;
    case 0x8F: line << "f32.trunc";   break;
    case 0x90: line << "f32.nearest"; break;
    case 0x91: line << "f32.sqrt";    break;
    case 0x92: line << "f32.add";     break;
    case 0x93: line << "f32.sub";     break;
    case 0x94: line << "f32.mul";     break;
    case 0x95: line << "f32.div";     break;
    case 0x96: line << "f32.min";     break;
    case 0x97: line << "f32.max";     break;
    case 0x98: line << "f32.copysign";break;
    // f64 arith
    case 0x99: line << "f64.abs";     break;
    case 0x9A: line << "f64.neg";     break;
    case 0x9B: line << "f64.ceil";    break;
    case 0x9C: line << "f64.floor";   break;
    case 0x9D: line << "f64.trunc";   break;
    case 0x9E: line << "f64.nearest"; break;
    case 0x9F: line << "f64.sqrt";    break;
    case 0xA0: line << "f64.add";     break;
    case 0xA1: line << "f64.sub";     break;
    case 0xA2: line << "f64.mul";     break;
    case 0xA3: line << "f64.div";     break;
    case 0xA4: line << "f64.min";     break;
    case 0xA5: line << "f64.max";     break;
    case 0xA6: line << "f64.copysign";break;
    // Conversions
    case 0xA7: line << "i32.wrap_i64";        break;
    case 0xA8: line << "i32.trunc_f32_s";     break;
    case 0xA9: line << "i32.trunc_f32_u";     break;
    case 0xAA: line << "i32.trunc_f64_s";     break;
    case 0xAB: line << "i32.trunc_f64_u";     break;
    case 0xAC: line << "i64.extend_i32_s";    break;
    case 0xAD: line << "i64.extend_i32_u";    break;
    case 0xAE: line << "i64.trunc_f32_s";     break;
    case 0xAF: line << "i64.trunc_f32_u";     break;
    case 0xB0: line << "i64.trunc_f64_s";     break;
    case 0xB1: line << "i64.trunc_f64_u";     break;
    case 0xB2: line << "f32.convert_i32_s";   break;
    case 0xB3: line << "f32.convert_i32_u";   break;
    case 0xB4: line << "f32.convert_i64_s";   break;
    case 0xB5: line << "f32.convert_i64_u";   break;
    case 0xB6: line << "f32.demote_f64";      break;
    case 0xB7: line << "f64.convert_i32_s";   break;
    case 0xB8: line << "f64.convert_i32_u";   break;
    case 0xB9: line << "f64.convert_i64_s";   break;
    case 0xBA: line << "f64.convert_i64_u";   break;
    case 0xBB: line << "f64.promote_f32";     break;
    case 0xBC: line << "i32.reinterpret_f32"; break;
    case 0xBD: line << "i64.reinterpret_f64"; break;
    case 0xBE: line << "f32.reinterpret_i32"; break;
    case 0xBF: line << "f64.reinterpret_i64"; break;
    // Sign extension
    case 0xC0: line << "i32.extend8_s";  break;
    case 0xC1: line << "i32.extend16_s"; break;
    case 0xC2: line << "i64.extend8_s";  break;
    case 0xC3: line << "i64.extend16_s"; break;
    case 0xC4: line << "i64.extend32_s"; break;
    // Reference types
    case 0xD0: { readVecULEB(code, pc); line << "ref.null"; break; }
    case 0xD1: line << "ref.is_null"; break;
    case 0xD2: line << "ref.func " << readVecULEB(code, pc); break;

    // FC extended ops (bulk-memory, saturating trunc)
    case 0xFC: {
        uint32_t subOp = readVecULEB(code, pc);
        switch (subOp) {
        case 0: line << "i32.trunc_sat_f32_s"; break;
        case 1: line << "i32.trunc_sat_f32_u"; break;
        case 2: line << "i32.trunc_sat_f64_s"; break;
        case 3: line << "i32.trunc_sat_f64_u"; break;
        case 4: line << "i64.trunc_sat_f32_s"; break;
        case 5: line << "i64.trunc_sat_f32_u"; break;
        case 6: line << "i64.trunc_sat_f64_s"; break;
        case 7: line << "i64.trunc_sat_f64_u"; break;
        case 8: { uint32_t si = readVecULEB(code, pc);
                  readVecULEB(code, pc); // memory 0
                  line << "memory.init " << si; break; }
        case 9: line << "data.drop " << readVecULEB(code, pc); break;
        case 10: { readVecULEB(code, pc); readVecULEB(code, pc);
                   line << "memory.copy"; break; }
        case 11: { readVecULEB(code, pc); line << "memory.fill"; break; }
        case 12: { uint32_t ei = readVecULEB(code, pc);
                   readVecULEB(code, pc);
                   line << "table.init " << ei; break; }
        case 13: line << "elem.drop " << readVecULEB(code, pc); break;
        case 14: { readVecULEB(code, pc); readVecULEB(code, pc);
                   line << "table.copy"; break; }
        case 15: line << "table.grow " << readVecULEB(code, pc); break;
        case 16: line << "table.size " << readVecULEB(code, pc); break;
        case 17: line << "table.fill " << readVecULEB(code, pc); break;
        default: line << ";; fc." << subOp; break;
        }
        break;
    }

    // FD SIMD ops (abbreviated)
    case 0xFD: {
        uint32_t subOp = readVecULEB(code, pc);
        line << "v128."; // simplified; full SIMD not expanded
        switch (subOp) {
        case 0: line.str(""); line << indentStr(indent) << "v128.load" << memArg(code, pc); break;
        case 11: line.str(""); line << indentStr(indent) << "v128.store" << memArg(code, pc); break;
        case 12: {
            // v128.const (16 bytes)
            if (pc + 16 <= code.size()) {
                line.str(""); line << indentStr(indent) << "v128.const i8x16";
                for (int bi = 0; bi < 16; ++bi) line << " " << (int)code[pc++];
            }
            break;
        }
        default: line << "op_" << subOp; break;
        }
        break;
    }

    default:
        line << ";; unknown_op 0x" << std::hex << (int)op << std::dec;
        break;
    }

    return line.str();
}

void WatEmitter::emitFuncBody(const WasmModule& mod, uint32_t funcIdx,
                                const FuncCode& code,
                                std::ostream& out, int indent) const {
    DisState st{mod, funcIdx, 0, 0, 0};

    // Count params
    uint32_t typeIdx = mod.funcTypeIndex(funcIdx);
    if (typeIdx < mod.types.size())
        st.paramCount = (uint32_t)mod.types[typeIdx].params.size();

    size_t pc = 0;
    while (pc < code.body.size()) {
        uint8_t peek = code.body[pc];
        if (peek == 0x0B && st.blockDepth == 0) {
            ++pc; // consume final END
            break;
        }
        std::string line = decodeInstr(st, code.body, pc, indent);
        if (!line.empty())
            out << "\n" << line;
    }
}

// ─── Top-level emit ───────────────────────────────────────────────────────────

void WatEmitter::emitModule(const WasmModule& mod, std::ostream& out) const {
    if (opts_.emitFileComment) {
        out << ";; Generated by RetDec WebAssembly disassembler\n";
        if (mod.names.has_value() && mod.names->moduleName.has_value())
            out << ";; Module: " << *mod.names->moduleName << "\n";
    }
    out << "(module\n";
    int indent = 1;
    emitTypeSection(mod, out, indent);
    if (opts_.inlineImports) emitFunctions(mod, out, indent);
    else { emitImports(mod, out, indent); emitFunctions(mod, out, indent); }
    emitTables(mod, out, indent);
    emitMemories(mod, out, indent);
    emitGlobals(mod, out, indent);
    if (!opts_.inlineExports) emitExports(mod, out, indent);
    emitStart(mod, out, indent);
    emitElements(mod, out, indent);
    emitDataSegments(mod, out, indent);
    out << ")\n";
}

WatEmitResult WatEmitter::emit(const WasmModule& module) const {
    WatEmitResult result;
    std::ostringstream ss;
    try {
        emitModule(module, ss);
        result.source = ss.str();
    } catch (const std::exception& e) {
        result.warnings.push_back(std::string("emit error: ") + e.what());
        result.source = ss.str(); // partial
    }
    return result;
}

} // namespace wasm_parser
} // namespace retdec
