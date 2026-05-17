/**
 * @file include/retdec/wasm_parser/wasm_reader.h
 * @brief WebAssembly binary (.wasm) module reader.
 *
 * Parses all standard WebAssembly 2.0 sections and the "name" custom section.
 * Supports both MVP (1.0) and extended proposals (bulk-memory, SIMD,
 * reference-types, threads, tail-calls).
 *
 * Usage:
 *   WasmReader reader(bytes);
 *   auto result = reader.read();
 *   if (!result.ok) { ... result.error ... }
 *   const WasmModule& module = result.module;
 */

#ifndef RETDEC_WASM_PARSER_WASM_READER_H
#define RETDEC_WASM_PARSER_WASM_READER_H

#include "retdec/wasm_parser/wasm_types.h"

#include <span>
#include <string>
#include <vector>

namespace retdec {
namespace wasm_parser {

struct WasmReadResult {
    bool        ok    = false;
    std::string error;
    WasmModule  module;
    std::vector<std::string> warnings;
};

class WasmReader {
public:
    explicit WasmReader(std::vector<uint8_t> bytes);
    explicit WasmReader(const uint8_t* data, size_t size);

    WasmReadResult read();

private:
    std::vector<uint8_t> data_;
    size_t               pos_  = 0;

    // ── Error handling ───────────────────────────────────────────────────────
    struct ParseError { std::string msg; };
    std::vector<std::string> warnings_;

    void warn(const std::string& msg) { warnings_.push_back(msg); }

    // ── Primitive readers ────────────────────────────────────────────────────
    uint8_t  readU8();
    uint16_t readU16Le();
    uint32_t readU32Le();
    uint32_t readULEB128();
    int32_t  readSLEB128();
    int64_t  readSLEB128_64();
    uint64_t readULEB128_64();
    float    readF32();
    double   readF64();

    bool     eof() const { return pos_ >= data_.size(); }
    size_t   remaining() const { return pos_ < data_.size() ? data_.size() - pos_ : 0; }
    void     skip(size_t n);
    std::vector<uint8_t> readBytes(size_t n);
    std::string readUtf8(uint32_t len);

    // ── Type readers ─────────────────────────────────────────────────────────
    ValType    readValType();
    FuncType   readFuncType();
    Limits     readLimits();
    MemType    readMemType();
    TableType  readTableType();
    GlobalType readGlobalType();
    std::vector<uint8_t> readConstExpr();  ///< reads until END opcode

    // ── Section parsers ──────────────────────────────────────────────────────
    void parseTypeSection(WasmModule& mod, uint32_t size);
    void parseImportSection(WasmModule& mod, uint32_t size);
    void parseFunctionSection(WasmModule& mod, uint32_t size);
    void parseTableSection(WasmModule& mod, uint32_t size);
    void parseMemorySection(WasmModule& mod, uint32_t size);
    void parseGlobalSection(WasmModule& mod, uint32_t size);
    void parseExportSection(WasmModule& mod, uint32_t size);
    void parseStartSection(WasmModule& mod, uint32_t size);
    void parseElementSection(WasmModule& mod, uint32_t size);
    void parseCodeSection(WasmModule& mod, uint32_t size);
    void parseDataSection(WasmModule& mod, uint32_t size);
    void parseDataCountSection(WasmModule& mod, uint32_t size);
    void parseCustomSection(WasmModule& mod, uint32_t size);

    // ── Name section ─────────────────────────────────────────────────────────
    void parseNameSection(WasmModule& mod, const std::vector<uint8_t>& data);
    std::vector<NameMap> readNameMap(const uint8_t* d, size_t& pos, size_t end);
    std::vector<IndirectNameMap> readIndirectNameMap(const uint8_t* d,
                                                       size_t& pos, size_t end);
};

} // namespace wasm_parser
} // namespace retdec

#endif // RETDEC_WASM_PARSER_WASM_READER_H
