/**
 * @file include/retdec/lua_parser/lua_types.h
 * @brief Lua bytecode type system and intermediate representation.
 *
 * Supports Lua 5.1, 5.2, 5.3, and 5.4 compiled bytecode (.luac) files.
 *
 * References:
 *   - Lua 5.1 source: ldump.c, lopcodes.h, lobject.h, lfunc.h
 *   - Lua 5.4 source: ldump.c, lopcodes.h
 *   - http://luaforge.net/docman/83/98/ANoFrillsIntroToLua51VMInstructions.pdf
 */

#ifndef RETDEC_LUA_PARSER_LUA_TYPES_H
#define RETDEC_LUA_PARSER_LUA_TYPES_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace lua_parser {

// ─── Lua version ─────────────────────────────────────────────────────────────

enum class LuaVersion {
    Unknown,
    Lua51,  ///< 0x51
    Lua52,  ///< 0x52
    Lua53,  ///< 0x53
    Lua54,  ///< 0x54
};

std::string luaVersionStr(LuaVersion v);

// ─── Lua constant ─────────────────────────────────────────────────────────────

struct LuaNil   {};
struct LuaBool  { bool value; };
struct LuaInt   { int64_t value; };   // Lua 5.3+ integer subtype
struct LuaFloat { double  value; };
struct LuaStr   { std::string value; };

using LuaConst = std::variant<LuaNil, LuaBool, LuaInt, LuaFloat, LuaStr>;

std::string luaConstStr(const LuaConst& c);

// ─── Lua upvalue descriptor ───────────────────────────────────────────────────

struct LuaUpvalue {
    std::string name;     // from debug info
    uint8_t     inStack;  // 5.2+: 1 = in enclosing func's register
    uint8_t     idx;      // 5.2+: register or upvalue index
    uint8_t     kind;     // 5.4+: upvalue kind
};

// ─── Local variable debug info ────────────────────────────────────────────────

struct LuaLocal {
    std::string name;
    int         startPc;
    int         endPc;
};

// ─── Lua instruction ─────────────────────────────────────────────────────────
// All Lua versions use 32-bit instructions (4 bytes each).

struct LuaInstr {
    uint32_t raw;

    // Lua 5.1-5.3 field layout (ABC format):
    //   bits  0- 5: opcode (6 bits)
    //   bits  6-13: A      (8 bits)
    //   bits 14-22: C      (9 bits)
    //   bits 23-31: B      (9 bits)
    // Lua 5.4 field layout:
    //   bits  0- 6: opcode (7 bits)
    //   bits  7-14: A      (8 bits)
    // Lua 5.4 instruction layout:
    //   bits  0-6:  OP  (7 bits)
    //   bits  7-14: A   (8 bits)
    //   bit   15:   k   (1 bit, flag)
    //   bits 16-23: B   (8 bits)
    //   bits 24-31: C   (8 bits)
    //   Bx = bits 15-31 (k+B+C = 17 bits, unsigned bias form for LOADI/LOADF/LOADK)
    //   sJ = bits  7-31 (25 bits, unsigned bias form for JMP)

    uint8_t  opcode51() const { return raw & 0x3F; }
    uint8_t  opcode54() const { return raw & 0x7F; }
    uint8_t  fieldA51() const { return (raw >> 6)  & 0xFF; }
    uint16_t fieldB51() const { return (raw >> 23) & 0x1FF; }
    uint16_t fieldC51() const { return (raw >> 14) & 0x1FF; }
    int32_t  fieldBx()  const { return (raw >> 14) & 0x3FFFF; }
    int32_t  fieldsBx() const {
        int32_t bx = fieldBx();
        return bx - ((1 << 17) - 1); // bias = MAXARG_sBx
    }
    uint8_t  fieldA54()  const { return (raw >> 7)  & 0xFF; }
    uint8_t  fieldB54()  const { return (raw >> 16) & 0xFF; }  // bits 16-23
    uint8_t  fieldC54()  const { return (raw >> 24) & 0xFF; }  // bits 24-31
    // Bx = k(1) | B(8) | C(8) = 17 bits from bit 15
    uint32_t fieldBx54() const { return (raw >> 15) & 0x1FFFF; }
    int32_t  fieldSBx54() const {
        return static_cast<int32_t>(fieldBx54()) - ((1 << 16) - 1);
    }
    // sJ: 25-bit signed offset for JMP (bits 7-31, bias = (1<<24)-1)
    int32_t  fieldSJ54() const {
        int32_t sj = static_cast<int32_t>((raw >> 7) & 0x1FFFFFF);
        return sj - ((1 << 24) - 1);
    }
    int32_t fieldAx() const { return (raw >> 7) & 0x1FFFFFF; }  // 5.4

    bool isRK(uint16_t x) const { return (x & 0x100) != 0; }  // K flag
    uint16_t rkIdx(uint16_t x) const { return x & 0xFF; }
};

// ─── Lua prototype (function prototype) ──────────────────────────────────────

struct LuaProto {
    std::string source;     ///< source file name (from @...)
    int         lineDefined = 0;
    int         lastLineDefined = 0;
    uint8_t     numParams   = 0;
    bool        isVarArg    = false;
    uint8_t     maxStackSize= 0;

    std::vector<LuaInstr>  code;
    std::vector<LuaConst>  constants;
    std::vector<LuaProto>  protos;     ///< nested functions
    std::vector<LuaUpvalue> upvalues;
    std::vector<LuaLocal>  locals;
    std::vector<int>       lineInfo;   ///< pc → line number

    LuaVersion version = LuaVersion::Unknown;

    // Debug helpers
    int lineForPc(int pc) const;
    std::string localName(int reg, int pc) const;
    std::string upvalueName(int idx) const;
};

// ─── LuaModule ───────────────────────────────────────────────────────────────

struct LuaModule {
    LuaVersion version = LuaVersion::Unknown;
    bool       littleEndian = true;
    int        intSize   = 4;
    int        sizetSize = 8;
    LuaProto   topLevel;
};

} // namespace lua_parser
} // namespace retdec

#endif // RETDEC_LUA_PARSER_LUA_TYPES_H
