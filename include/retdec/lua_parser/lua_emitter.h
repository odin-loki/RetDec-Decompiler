/**
 * @file include/retdec/lua_parser/lua_emitter.h
 * @brief Lua source (.lua) emitter from a parsed LuaModule.
 *
 * Performs a structural decompilation pass: decodes Lua VM instructions
 * back into high-level Lua source code, recovering:
 *
 *   - Function definitions (local function / function declarations)
 *   - Local variable declarations with inferred names
 *   - Assignment statements (simple, multi-value)
 *   - Arithmetic, comparison, logical, string concatenation expressions
 *   - Table constructors and field access
 *   - Function calls (including method calls)
 *   - Control-flow: if/elseif/else/end, while/do/end, repeat/until,
 *                   numeric and generic for loops
 *   - Return statements, break statements
 *   - Upvalue / closure references
 *   - Vararg (...) in function signatures and calls
 *   - String, number, boolean, nil constants
 *   - Comment headers with source/line info
 */

#ifndef RETDEC_LUA_PARSER_LUA_EMITTER_H
#define RETDEC_LUA_PARSER_LUA_EMITTER_H

#include "retdec/lua_parser/lua_types.h"

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace retdec {
namespace lua_parser {

struct LuaEmitOptions {
    int  indentWidth    = 4;       ///< spaces per indent level
    bool emitLineInfo   = false;   ///< emit --[[ line N ]] comments
    bool emitFileHeader = true;    ///< emit top comment with version/source
    bool useLocalNames  = true;    ///< use debug local names when available
};

struct LuaEmitResult {
    std::string              source;
    std::vector<std::string> warnings;
};

// ─── Per-prototype emission context ──────────────────────────────────────────

/**
 * @brief Represents a decoded "value" held in a Lua register or as an
 *        expression during the decompilation of one prototype.
 */
struct LuaExpr {
    enum class Kind {
        Nil, True, False,
        Int, Float, String,
        Local,    ///< register reference → local name
        Upvalue,  ///< upvalue name
        Global,   ///< _ENV["name"] or global ref
        Index,    ///< table[key]
        Field,    ///< table.field
        Call,     ///< function call expression
        VarArg,   ///< ...
        BinOp, UnaryOp,
        Concat,
        Closure,  ///< sub-prototype reference
        Raw,      ///< pre-formatted string
    };

    Kind        kind = Kind::Nil;
    std::string raw;      // for Raw / names / strings
    double      numVal = 0.0;
    int64_t     intVal = 0;
};

LuaExpr makeNil();
LuaExpr makeTrue();
LuaExpr makeFalse();
LuaExpr makeInt(int64_t v);
LuaExpr makeFloat(double v);
LuaExpr makeStr(const std::string& s);
LuaExpr makeLocal(const std::string& name);
LuaExpr makeRaw(const std::string& s);

// ─── LuaEmitter ─────────────────────────────────────────────────────────────

class LuaEmitter {
public:
    explicit LuaEmitter(LuaEmitOptions opts = LuaEmitOptions{});

    LuaEmitResult emit(const LuaModule& module) const;

private:
    LuaEmitOptions opts_;

    // ── Top-level ─────────────────────────────────────────────────────────────
    void emitModule(const LuaModule& mod, std::ostream& out) const;

    // ── Prototype (function body) ─────────────────────────────────────────────
    void emitProto(const LuaProto& proto, std::ostream& out,
                   int indent, const std::string& funcName) const;

    // ── Instruction-level disassembly + pseudo-decompilation ─────────────────
    void disassemble(const LuaProto& proto, std::ostream& out, int indent) const;

    // ── Decompilation helpers ─────────────────────────────────────────────────
    std::string constStr(const LuaConst& c) const;
    std::string exprStr(const LuaExpr& e) const;
    std::string rkExpr(const LuaProto& proto, uint16_t rk,
                       const std::vector<std::string>& regs) const;
    std::string regOrConst(const LuaProto& proto, uint16_t x,
                            const std::vector<std::string>& regs) const;

    std::string localName(const LuaProto& proto, int reg, int pc) const;
    std::string upvalName(const LuaProto& proto, int idx) const;

    std::string indent(int level) const;

    // ── Lua 5.1/5.2/5.3/5.4 opcode dispatch ─────────────────────────────────
    std::string decodeInstr51(const LuaProto& proto, int pc,
                               const std::vector<std::string>& regs) const;
    std::string decodeInstrLua51(const LuaProto& proto, int pc,
                                  const std::vector<std::string>& regs) const;
    std::string decodeInstrLua52(const LuaProto& proto, int pc,
                                  const std::vector<std::string>& regs) const;
    std::string decodeInstr54(const LuaProto& proto, int pc,
                               const std::vector<std::string>& regs) const;
};

} // namespace lua_parser
} // namespace retdec

#endif // RETDEC_LUA_PARSER_LUA_EMITTER_H
