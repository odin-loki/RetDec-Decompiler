/**
 * @file include/retdec/wasm_parser/wat_emitter.h
 * @brief WebAssembly Text Format (.wat) emitter.
 *
 * Converts a parsed WasmModule into WebAssembly Text Format (WAT) as
 * defined in the WebAssembly specification §6 (Text Format).
 *
 * Output features:
 *   - Full s-expression text format: (module (func ...) ...)
 *   - Type section folded inline or as named type aliases
 *   - Named functions/locals/params from name section / export names
 *   - Inline import/export annotations: (import "m" "n" ...)
 *   - Structured control flow: block/loop/if-then-else with labels
 *   - All numeric types and instructions (i32/i64/f32/f64/v128)
 *   - Memory/data/element segments
 *   - Global initialiser expressions
 *   - Pretty-printed with configurable indentation
 */

#ifndef RETDEC_WASM_PARSER_WAT_EMITTER_H
#define RETDEC_WASM_PARSER_WAT_EMITTER_H

#include "retdec/wasm_parser/wasm_types.h"

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace wasm_parser {

struct WatEmitOptions {
    int  indentWidth      = 2;
    bool foldTypes        = true;    ///< Emit (type ...) section + ref by index
    bool inlineImports    = true;    ///< Inline import on the item declaration
    bool inlineExports    = true;    ///< Inline export on the item declaration
    bool useNames         = true;    ///< Use $names from name/export section
    bool emitDataStrings  = true;    ///< Emit data as quoted strings where possible
    bool omitEmptyBodies  = false;   ///< Omit unreachable function bodies
    bool emitFileComment  = true;    ///< Opening comment with module info
};

struct WatEmitResult {
    std::string              source;
    std::vector<std::string> warnings;
};

class WatEmitter {
public:
    explicit WatEmitter(WatEmitOptions opts = WatEmitOptions{});

    WatEmitResult emit(const WasmModule& module) const;

private:
    WatEmitOptions opts_;

    // ── High-level section emitters ──────────────────────────────────────────

    void emitModule(const WasmModule& mod, std::ostream& out) const;
    void emitTypeSection(const WasmModule& mod, std::ostream& out,
                         int indent) const;
    void emitImports(const WasmModule& mod, std::ostream& out,
                     int indent) const;
    void emitFunctions(const WasmModule& mod, std::ostream& out,
                       int indent) const;
    void emitTables(const WasmModule& mod, std::ostream& out,
                    int indent) const;
    void emitMemories(const WasmModule& mod, std::ostream& out,
                      int indent) const;
    void emitGlobals(const WasmModule& mod, std::ostream& out,
                     int indent) const;
    void emitExports(const WasmModule& mod, std::ostream& out,
                     int indent) const;
    void emitElements(const WasmModule& mod, std::ostream& out,
                      int indent) const;
    void emitDataSegments(const WasmModule& mod, std::ostream& out,
                          int indent) const;
    void emitStart(const WasmModule& mod, std::ostream& out,
                   int indent) const;

    // ── Function body disassembler ───────────────────────────────────────────

    void emitFuncBody(const WasmModule& mod, uint32_t funcIdx,
                      const FuncCode& code,
                      std::ostream& out, int indent) const;

    void disassemble(const WasmModule& mod, uint32_t funcIdx,
                     const std::vector<uint8_t>& expr,
                     std::ostream& out, int indent,
                     uint32_t& localParamTotal) const;

    // ── Type/name helpers ────────────────────────────────────────────────────

    std::string valTypeName(ValType vt) const;
    std::string funcTypeSig(const FuncType& ft) const;
    std::string funcTypeRef(const WasmModule& mod, uint32_t typeIdx) const;
    std::string funcId(const WasmModule& mod, uint32_t funcIdx) const;
    std::string globalId(const WasmModule& mod, uint32_t idx) const;
    std::string tableId(const WasmModule& mod, uint32_t idx) const;
    std::string memId(const WasmModule& mod, uint32_t idx) const;
    std::string localId(const WasmModule& mod, uint32_t funcIdx,
                        uint32_t localIdx) const;

    std::string exportAnnotations(const WasmModule& mod,
                                   ExternKind kind, uint32_t idx) const;
    std::string importAnnotation(const Import& imp) const;

    std::string limitsStr(const Limits& lim) const;
    std::string constExprStr(const WasmModule& mod,
                              const std::vector<uint8_t>& expr) const;
    std::string dataStr(const std::vector<uint8_t>& bytes) const;

    std::string indentStr(int level) const;

    // ── Mutable state passed through disassembly ──────────────────────────────
    // (passed by ref to disassemble helper)
    struct DisState {
        const WasmModule&  mod;
        uint32_t           funcIdx    = 0;
        uint32_t           paramCount = 0;  // params already counted
        int                blockDepth = 0;
        uint32_t           labelCounter = 0;
    };

    std::string decodeInstr(DisState& st,
                             const std::vector<uint8_t>& code,
                             size_t& pc, int indent) const;
};

} // namespace wasm_parser
} // namespace retdec

#endif // RETDEC_WASM_PARSER_WAT_EMITTER_H
