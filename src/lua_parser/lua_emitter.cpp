/**
 * @file src/lua_parser/lua_emitter.cpp
 * @brief Lua source emitter — converts LuaModule to readable .lua source.
 */

#include "retdec/lua_parser/lua_emitter.h"

#include <cassert>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace retdec {
namespace lua_parser {

// ─── Expression factories ─────────────────────────────────────────────────────

LuaExpr makeNil()   { LuaExpr e; e.kind = LuaExpr::Kind::Nil; return e; }
LuaExpr makeTrue()  { LuaExpr e; e.kind = LuaExpr::Kind::True; return e; }
LuaExpr makeFalse() { LuaExpr e; e.kind = LuaExpr::Kind::False; return e; }
LuaExpr makeInt(int64_t v)  { LuaExpr e; e.kind = LuaExpr::Kind::Int; e.intVal = v; return e; }
LuaExpr makeFloat(double v) { LuaExpr e; e.kind = LuaExpr::Kind::Float; e.numVal = v; return e; }
LuaExpr makeStr(const std::string& s) { LuaExpr e; e.kind = LuaExpr::Kind::String; e.raw = s; return e; }
LuaExpr makeLocal(const std::string& n) { LuaExpr e; e.kind = LuaExpr::Kind::Local; e.raw = n; return e; }
LuaExpr makeRaw(const std::string& s)  { LuaExpr e; e.kind = LuaExpr::Kind::Raw; e.raw = s; return e; }

// ─── LuaEmitter ──────────────────────────────────────────────────────────────

LuaEmitter::LuaEmitter(LuaEmitOptions opts)
    : opts_(std::move(opts)) {}

std::string LuaEmitter::indent(int level) const {
    return std::string(level * opts_.indentWidth, ' ');
}

std::string LuaEmitter::constStr(const LuaConst& c) const {
    return luaConstStr(c);
}

// Extract the raw string value from a string constant (no quotes).
// Used for global variable names where the constant is an identifier.
static std::string rawStr(const LuaConst& c) {
    if (auto* s = std::get_if<LuaStr>(&c)) return s->value;
    return luaConstStr(c);
}

std::string LuaEmitter::exprStr(const LuaExpr& e) const {
    switch (e.kind) {
    case LuaExpr::Kind::Nil:    return "nil";
    case LuaExpr::Kind::True:   return "true";
    case LuaExpr::Kind::False:  return "false";
    case LuaExpr::Kind::Int:    return std::to_string(e.intVal);
    case LuaExpr::Kind::Float: {
        std::ostringstream ss;
        ss << std::setprecision(14) << e.numVal;
        std::string s = ss.str();
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
            s += ".0";
        return s;
    }
    case LuaExpr::Kind::String: return e.raw;
    default: return e.raw;
    }
}

// Decode register-or-constant (RK) into a source string
std::string LuaEmitter::rkExpr(const LuaProto& proto, uint16_t rk,
                                  const std::vector<std::string>& regs) const {
    if (rk & 0x100) {
        // Constant index
        uint16_t ki = rk & 0xFF;
        if (ki < proto.constants.size())
            return constStr(proto.constants[ki]);
        return "nil";
    }
    if (rk < regs.size()) return regs[rk];
    return "reg" + std::to_string(rk);
}

std::string LuaEmitter::regOrConst(const LuaProto& proto, uint16_t x,
                                     const std::vector<std::string>& regs) const {
    return rkExpr(proto, x, regs);
}

std::string LuaEmitter::localName(const LuaProto& proto, int reg, int pc) const {
    if (!opts_.useLocalNames) return "reg" + std::to_string(reg);
    // Scan locals that are live at pc
    int liveIdx = 0;
    for (const auto& loc : proto.locals) {
        if (loc.startPc <= pc && pc < loc.endPc) {
            if (liveIdx == reg) return loc.name;
            ++liveIdx;
        }
    }
    return "reg" + std::to_string(reg);
}

std::string LuaEmitter::upvalName(const LuaProto& proto, int idx) const {
    return proto.upvalueName(idx);
}

// ─── Lua 5.1 opcode decoder ──────────────────────────────────────────────────
// Lua 5.1 opcode table (opcodes 0-37, no LOADKX).

std::string LuaEmitter::decodeInstrLua51(const LuaProto& proto, int pc,
                                           const std::vector<std::string>& regs) const {
    const LuaInstr& ins = proto.code[pc];
    uint8_t  op  = ins.opcode51();
    uint8_t  A   = ins.fieldA51();
    uint16_t B   = ins.fieldB51();
    uint16_t C   = ins.fieldC51();
    int32_t  Bx  = ins.fieldBx();
    int32_t  sBx = ins.fieldsBx();

    auto reg = [&](int r) -> std::string { return localName(proto, r, pc); };
    auto rk  = [&](uint16_t x) -> std::string { return rkExpr(proto, x, regs); };
    auto kstr = [&](int ki) -> std::string {
        if (ki >= 0 && ki < (int)proto.constants.size())
            return constStr(proto.constants[ki]);
        return "nil";
    };

    std::string dst = reg(A);

    switch (op) {
    // 0  MOVE      A B    R(A) := R(B)
    case 0:  return dst + " = " + reg(B);
    // 1  LOADK     A Bx   R(A) := Kst(Bx)
    case 1:  return dst + " = " + kstr(Bx);
    // 2  LOADBOOL  A B C  R(A) := (Bool)B; if (C) skip next
    case 2:  return dst + " = " + (B ? "true" : "false") + (C ? "; -- skip next" : "");
    // 3  LOADNIL   A B    R(A) := ... := R(B) := nil
    case 3: {
        std::string s = reg(A) + " = nil";
        for (int r = (int)A + 1; r <= (int)B; ++r)
            s += "; " + reg(r) + " = nil";
        return s;
    }
    // 4  GETUPVAL  A B    R(A) := UpValue[B]
    case 4:  return dst + " = " + upvalName(proto, B);
    // 5  GETGLOBAL A Bx   R(A) := Gbl[Kst(Bx)]
    case 5: {
        std::string gname = (Bx < (int)proto.constants.size())
                            ? rawStr(proto.constants[Bx]) : kstr(Bx);
        return dst + " = " + gname;
    }
    // 6  GETTABLE  A B C  R(A) := R(B)[RK(C)]
    case 6:  return dst + " = " + reg(B) + "[" + rk(C) + "]";
    // 7  SETGLOBAL A Bx   Gbl[Kst(Bx)] := R(A)
    case 7: {
        std::string gname = (Bx < (int)proto.constants.size())
                            ? rawStr(proto.constants[Bx]) : kstr(Bx);
        return gname + " = " + reg(A);
    }
    // 8  SETUPVAL  A B    UpValue[B] := R(A)
    case 8:  return upvalName(proto, B) + " = " + reg(A);
    // 9  SETTABLE  A B C  R(A)[RK(B)] := RK(C)
    case 9:  return reg(A) + "[" + rk(B) + "] = " + rk(C);
    // 10 NEWTABLE  A B C  R(A) := {}
    case 10: return dst + " = {}";
    // 11 SELF      A B C  R(A+1) := R(B); R(A) := R(B)[RK(C)]
    case 11: return reg(A+1) + " = " + reg(B) + "; " + dst + " = " + reg(B) + "[" + rk(C) + "]";
    // 12-17 Arithmetic
    case 12: return dst + " = " + rk(B) + " + "  + rk(C);  // ADD
    case 13: return dst + " = " + rk(B) + " - "  + rk(C);  // SUB
    case 14: return dst + " = " + rk(B) + " * "  + rk(C);  // MUL
    case 15: return dst + " = " + rk(B) + " / "  + rk(C);  // DIV
    case 16: return dst + " = " + rk(B) + " % "  + rk(C);  // MOD
    case 17: return dst + " = " + rk(B) + " ^ "  + rk(C);  // POW
    case 18: return dst + " = -" + reg(B);                  // UNM
    case 19: return dst + " = not " + reg(B);               // NOT
    case 20: return dst + " = #"   + reg(B);                // LEN
    // 21 CONCAT    A B C  R(A) := R(B).. ... ..R(C)
    case 21: {
        std::string s = dst + " = " + reg(B);
        for (int r = (int)B + 1; r <= (int)C; ++r)
            s += " .. " + reg(r);
        return s;
    }
    // 22 JMP       sBx    pc += sBx
    case 22: return "goto " + std::to_string(pc + 1 + sBx);
    // 23 EQ        A B C  if ((RK(B) == RK(C)) ~= A) then pc++
    case 23: return "if (" + rk(B) + " == " + rk(C) + ") ~= " + std::to_string(A) + " then skip";
    // 24 LT        A B C  if ((RK(B) <  RK(C)) ~= A) then pc++
    case 24: return "if (" + rk(B) + " < "  + rk(C) + ") ~= " + std::to_string(A) + " then skip";
    // 25 LE        A B C  if ((RK(B) <= RK(C)) ~= A) then pc++
    case 25: return "if (" + rk(B) + " <= " + rk(C) + ") ~= " + std::to_string(A) + " then skip";
    // 26 TEST      A C    if not (R(A) <=> C) then pc++
    case 26: return "if not (" + reg(A) + " == " + std::to_string(C) + ") then skip";
    // 27 TESTSET   A B C  if (R(B) <=> C) then R(A) := R(B) else pc++
    case 27: return "if " + reg(B) + " == " + std::to_string(C) + " then " + dst + " = " + reg(B) + " else skip";
    // 28 CALL      A B C
    case 28: {
        std::string args;
        int numArgs = (int)B - 1;
        for (int i = 1; i <= numArgs; ++i) {
            if (!args.empty()) args += ", ";
            args += reg(A + i);
        }
        if (B == 0) args = "...";
        int numRets = (int)C - 1;
        if (numRets > 0) {
            std::string rets;
            for (int i = 0; i < numRets; ++i) {
                if (!rets.empty()) rets += ", ";
                rets += reg(A + i);
            }
            return rets + " = " + reg(A) + "(" + args + ")";
        }
        return reg(A) + "(" + args + ")";
    }
    // 29 TAILCALL  A B C
    case 29: {
        std::string args;
        int numArgs = (int)B - 1;
        for (int i = 1; i <= numArgs; ++i) {
            if (!args.empty()) args += ", ";
            args += reg(A + i);
        }
        if (B == 0) args = "...";
        return "return " + reg(A) + "(" + args + ")";
    }
    // 30 RETURN    A B
    case 30: {
        int numRets = (int)B - 1;
        if (numRets == 0) return "return";
        if (numRets < 0) return "return ...";
        std::string rets;
        for (int i = 0; i < numRets; ++i) {
            if (!rets.empty()) rets += ", ";
            rets += reg(A + i);
        }
        return "return " + rets;
    }
    // 31 FORLOOP   A sBx
    case 31:
        return "-- for step: " + reg(A) + " += " + reg(A+2) +
               "; if <= " + reg(A+1) + " { " + reg(A+3) + " = " + reg(A) +
               "; goto " + std::to_string(pc + 1 + sBx) + " }";
    // 32 FORPREP   A sBx
    case 32:
        return "-- for init: " + reg(A) + " -= " + reg(A+2) +
               "; goto " + std::to_string(pc + 1 + sBx);
    // 33 TFORLOOP  A C    R(A+3)..R(A+2+C) = R(A)(R(A+1), R(A+2))
    case 33: {
        std::string rets;
        for (int i = 0; i < (int)C; ++i) {
            if (!rets.empty()) rets += ", ";
            rets += reg(A + 3 + i);
        }
        return rets + " = " + reg(A) + "(" + reg(A+1) + ", " + reg(A+2) + ")";
    }
    // 34 SETLIST   A B C  R(A)[(C-1)*FPF+i] = R(A+i)
    case 34:
        return "-- setlist: " + reg(A) + "[" + std::to_string(C) + "*50+i] = R(" +
               std::to_string(A) + "+i), i=1.." + std::to_string(B);
    // 35 CLOSE     A      close upvalues >= R(A)
    case 35:
        return "-- close upvalues from " + reg(A);
    // 36 CLOSURE   A Bx   R(A) = closure(KPROTO[Bx])
    case 36:
        return dst + " = function() --[[ proto[" + std::to_string(Bx) + "] ]] end";
    // 37 VARARG    A B    R(A)..R(A+B-1) = vararg
    case 37: {
        int numRets = (int)B - 1;
        if (numRets <= 0) return dst + " = ...";
        std::string rets;
        for (int i = 0; i < numRets; ++i) {
            if (!rets.empty()) rets += ", ";
            rets += reg(A + i);
        }
        return rets + " = ...";
    }
    default:
        return "-- op51_" + std::to_string(op) + " A=" + std::to_string(A) +
               " B=" + std::to_string(B) + " C=" + std::to_string(C);
    }
}

// ─── Lua 5.2/5.3 opcode decoder ──────────────────────────────────────────────
// Lua 5.2 added LOADKX at opcode 2; opcodes 3+ are shifted by 1 vs 5.1.

std::string LuaEmitter::decodeInstrLua52(const LuaProto& proto, int pc,
                                           const std::vector<std::string>& regs) const {
    const LuaInstr& ins = proto.code[pc];
    uint8_t  op  = ins.opcode51();
    uint8_t  A   = ins.fieldA51();
    uint16_t B   = ins.fieldB51();
    uint16_t C   = ins.fieldC51();
    int32_t  Bx  = ins.fieldBx();
    int32_t  sBx = ins.fieldsBx();

    auto reg = [&](int r) -> std::string { return localName(proto, r, pc); };
    auto rk  = [&](uint16_t x) -> std::string { return rkExpr(proto, x, regs); };
    auto kstr = [&](int ki) -> std::string {
        if (ki >= 0 && ki < (int)proto.constants.size())
            return constStr(proto.constants[ki]);
        return "nil";
    };

    std::string dst = reg(A);

    switch (op) {
    case 0:  return dst + " = " + reg(B);                                   // MOVE
    case 1:  return dst + " = " + kstr(Bx);                                 // LOADK
    case 2:  return dst + " = K[extra]";                                     // LOADKX
    case 3:  return dst + " = " + (B ? "true" : "false") + (C ? "; -- skip next" : ""); // LOADBOOL
    case 4: {                                                                 // LOADNIL
        std::string s = reg(A) + " = nil";
        for (int r = (int)A + 1; r <= (int)A + (int)B; ++r)
            s += "; " + reg(r) + " = nil";
        return s;
    }
    case 5:  return dst + " = " + upvalName(proto, B);                      // GETUPVAL
    case 6:  return dst + " = " + upvalName(proto, B) + "[" + rk(C) + "]"; // GETTABUP
    case 7:  return dst + " = " + reg(B) + "[" + rk(C) + "]";              // GETTABLE
    case 8:  return upvalName(proto, A) + "[" + rk(B) + "] = " + rk(C);   // SETTABUP
    case 9:  return upvalName(proto, B) + " = " + reg(A);                   // SETUPVAL
    case 10: return reg(A) + "[" + rk(B) + "] = " + rk(C);                // SETTABLE
    case 11: return dst + " = {}";                                           // NEWTABLE
    case 12: return reg(A+1) + " = " + reg(B) + "; " + dst + " = " + reg(B) + "[" + rk(C) + "]"; // SELF
    case 13: return dst + " = " + rk(B) + " + "  + rk(C);                 // ADD
    case 14: return dst + " = " + rk(B) + " - "  + rk(C);                 // SUB
    case 15: return dst + " = " + rk(B) + " * "  + rk(C);                 // MUL
    case 16: return dst + " = " + rk(B) + " / "  + rk(C);                 // DIV
    case 17: return dst + " = " + rk(B) + " % "  + rk(C);                 // MOD
    case 18: return dst + " = " + rk(B) + " ^ "  + rk(C);                 // POW
    case 19: return dst + " = -" + reg(B);                                  // UNM
    case 20: return dst + " = not " + reg(B);                               // NOT
    case 21: return dst + " = #" + reg(B);                                  // LEN
    case 22: {                                                                // CONCAT
        std::string s = dst + " = " + reg(B);
        for (int r = (int)B + 1; r <= (int)C; ++r) s += " .. " + reg(r);
        return s;
    }
    case 23: return "goto " + std::to_string(pc + 1 + sBx);                // JMP
    case 24: return "if (" + rk(B) + " == " + rk(C) + ") ~= " + std::to_string(A) + " then skip"; // EQ
    case 25: return "if (" + rk(B) + " < "  + rk(C) + ") ~= " + std::to_string(A) + " then skip"; // LT
    case 26: return "if (" + rk(B) + " <= " + rk(C) + ") ~= " + std::to_string(A) + " then skip"; // LE
    case 27: return "if not (" + reg(A) + " == " + std::to_string(C) + ") then skip";              // TEST
    case 28: return dst + " = " + reg(B) + " if " + std::to_string(C);                             // TESTSET
    case 29: {                                                                // CALL
        std::string args;
        int numArgs = (int)B - 1;
        for (int i = 1; i <= numArgs; ++i) {
            if (!args.empty()) args += ", ";
            args += reg(A + i);
        }
        if (B == 0) args = "...";
        int numRets = (int)C - 1;
        if (numRets > 0) {
            std::string rets;
            for (int i = 0; i < numRets; ++i) {
                if (!rets.empty()) rets += ", ";
                rets += reg(A + i);
            }
            return rets + " = " + reg(A) + "(" + args + ")";
        }
        return reg(A) + "(" + args + ")";
    }
    case 30: {                                                                // TAILCALL
        std::string args;
        int numArgs = (int)B - 1;
        for (int i = 1; i <= numArgs; ++i) {
            if (!args.empty()) args += ", ";
            args += reg(A + i);
        }
        if (B == 0) args = "...";
        return "return " + reg(A) + "(" + args + ")";
    }
    case 31: {                                                                // RETURN
        int numRets = (int)B - 1;
        if (numRets == 0) return "return";
        if (numRets < 0) return "return ...";
        std::string rets;
        for (int i = 0; i < numRets; ++i) {
            if (!rets.empty()) rets += ", ";
            rets += reg(A + i);
        }
        return "return " + rets;
    }
    case 32: return "-- for step: " + reg(A) + " += " + reg(A+2) +
                    "; if <= " + reg(A+1) + " goto " + std::to_string(pc + 1 + sBx); // FORLOOP
    case 33: return "-- for init: " + reg(A) + " -= " + reg(A+2) +
                    "; goto " + std::to_string(pc + 1 + sBx);               // FORPREP
    case 34: {                                                                // TFORCALL (5.2)
        std::string rets;
        for (int i = 0; i < (int)C; ++i) {
            if (!rets.empty()) rets += ", ";
            rets += reg(A + 3 + i);
        }
        return rets + " = " + reg(A) + "(" + reg(A+1) + ", " + reg(A+2) + ")";
    }
    case 35: return "-- tforloop goto " + std::to_string(pc + 1 + sBx);   // TFORLOOP (5.2)
    case 36: return "-- setlist: " + reg(A) + "[" + std::to_string(C) + "*50+i]"; // SETLIST
    case 37: return "-- close upvalues from " + reg(A);                     // CLOSE
    case 38: return dst + " = function() --[[ proto[" + std::to_string(Bx) + "] ]] end"; // CLOSURE
    case 39: {                                                                // VARARG
        int numRets = (int)B - 1;
        if (numRets <= 0) return dst + " = ...";
        std::string rets;
        for (int i = 0; i < numRets; ++i) {
            if (!rets.empty()) rets += ", ";
            rets += reg(A + i);
        }
        return rets + " = ...";
    }
    default:
        return "-- op52_" + std::to_string(op) + " A=" + std::to_string(A) +
               " B=" + std::to_string(B) + " C=" + std::to_string(C);
    }
}

// ─── Dispatcher for non-5.4 versions ─────────────────────────────────────────

std::string LuaEmitter::decodeInstr51(const LuaProto& proto, int pc,
                                        const std::vector<std::string>& regs) const {
    if (proto.version == LuaVersion::Lua51)
        return decodeInstrLua51(proto, pc, regs);
    return decodeInstrLua52(proto, pc, regs);
}

// ─── Lua 5.4 opcode decoder ───────────────────────────────────────────────────
// Lua 5.4 reorganized opcodes and field layouts.

std::string LuaEmitter::decodeInstr54(const LuaProto& proto, int pc,
                                        const std::vector<std::string>& regs) const {
    const LuaInstr& ins = proto.code[pc];
    uint8_t  op  = ins.opcode54();
    uint8_t  A   = ins.fieldA54();
    uint8_t  B   = ins.fieldB54();  // bits 16-23
    uint8_t  C   = ins.fieldC54();  // bits 24-31
    int32_t  k   = (ins.raw >> 15) & 1; // bit 15
    int32_t  sBx = ins.fieldSBx54(); // signed Bx (17-bit, bias 65535)
    int32_t  sB  = (int32_t)B - 127; // signed B for EQI/LTI/LEI/GTI/GEI/ADDI/MMBINI
    int32_t  sC  = (int32_t)C - 127; // signed C for ADDI/SHRI/SHLI

    auto reg = [&](int r) -> std::string {
        return localName(proto, r, pc);
    };
    auto kstr = [&](int ki) -> std::string {
        if (ki >= 0 && ki < (int)proto.constants.size())
            return constStr(proto.constants[ki]);
        return "nil";
    };
    // Format a signed immediate cleanly (e.g., + -1 → - 1)
    auto fmtImm = [](int32_t v) -> std::string {
        return (v >= 0 ? " + " : " - ") + std::to_string(std::abs(v));
    };

    std::string dst = reg(A);

    switch (op) {
    case 0:  return dst + " = " + reg(B);                     // MOVE
    case 1:  return dst + " = " + std::to_string(sBx);        // LOADI A sBx
    case 2:  return dst + " = " + std::to_string((double)sBx);// LOADF A sBx
    case 3:  return dst + " = " + kstr((int)ins.fieldBx54()); // LOADK A Bx
    case 4:  return dst + " = " + kstr(0);                    // LOADKX
    case 5:  return dst + " = " + (B ? "true" : "false");     // LOADTRUE
    case 6:  return dst + " = false";                         // LOADFALSE
    case 7:  return dst + " = false -- lfalseskip";           // LFALSESKIP
    case 8:  return dst + " = nil";                           // LOADNIL
    case 9:  return dst + " = " + upvalName(proto, B);        // GETUPVAL
    case 10: return upvalName(proto, A) + " = " + reg(B);     // SETUPVAL
    case 11: // GETTABUP
        return dst + " = " + upvalName(proto, B) + "[" + kstr(C) + "]";
    case 12: // GETTABLE
        return dst + " = " + reg(B) + "[" + reg(C) + "]";
    case 13: // GETI
        return dst + " = " + reg(B) + "[" + std::to_string(C) + "]";
    case 14: // GETFIELD
        return dst + " = " + reg(B) + "." + kstr(C);
    case 15: // SETTABUP
        return upvalName(proto, A) + "[" + kstr(B) + "] = " + (k ? kstr(C) : reg(C));
    case 16: // SETTABLE
        return reg(A) + "[" + reg(B) + "] = " + (k ? kstr(C) : reg(C));
    case 17: // SETI
        return reg(A) + "[" + std::to_string(B) + "] = " + (k ? kstr(C) : reg(C));
    case 18: // SETFIELD
        return reg(A) + "." + kstr(B) + " = " + (k ? kstr(C) : reg(C));
    case 19: return dst + " = {}";                            // NEWTABLE
    case 20: // SELF
        return dst + " = " + reg(B) + "; " + reg(A+1) + " = " + reg(B) + "[" + (k ? kstr(C) : reg(C)) + "]";
    case 21: // ADDI A B sC  →  R[A] = R[B] + sC
        return dst + " = " + reg(B) + fmtImm(sC);
    case 22: return dst + " = " + reg(B) + " + "  + (k ? kstr(C) : reg(C)); // ADDK / ADD
    case 23: return dst + " = " + reg(B) + " - "  + (k ? kstr(C) : reg(C)); // SUBK / SUB
    case 24: return dst + " = " + reg(B) + " * "  + (k ? kstr(C) : reg(C)); // MULK / MUL
    case 25: return dst + " = " + reg(B) + " % "  + (k ? kstr(C) : reg(C)); // MODK / MOD
    case 26: return dst + " = " + reg(B) + " ^ "  + (k ? kstr(C) : reg(C)); // POWK / POW
    case 27: return dst + " = " + reg(B) + " / "  + (k ? kstr(C) : reg(C)); // DIVK / DIV
    case 28: return dst + " = " + reg(B) + " // " + (k ? kstr(C) : reg(C)); // IDIVK / IDIV
    case 29: return dst + " = " + reg(B) + " & "  + (k ? kstr(C) : reg(C)); // BANDK / BAND
    case 30: return dst + " = " + reg(B) + " | "  + (k ? kstr(C) : reg(C)); // BORK / BOR
    case 31: return dst + " = " + reg(B) + " ~ "  + (k ? kstr(C) : reg(C)); // BXORK / BXOR
    case 32: // SHRI A B sC: R[A] = R[B] >> sC (negative sC = left shift)
        if (sC >= 0)
            return dst + " = " + reg(B) + " >> " + std::to_string(sC);
        else
            return dst + " = " + reg(B) + " << " + std::to_string(-sC);
    case 33: // SHLI A B sC: R[A] = sC << R[B]
        return dst + " = " + std::to_string(sC) + " << " + reg(B);
    case 34: return dst + " = " + reg(B) + " + "  + reg(C); // ADD
    case 35: return dst + " = " + reg(B) + " - "  + reg(C); // SUB
    case 36: return dst + " = " + reg(B) + " * "  + reg(C); // MUL
    case 37: return dst + " = " + reg(B) + " / "  + reg(C); // DIV
    case 38: return dst + " = " + reg(B) + " % "  + reg(C); // MOD
    case 39: return dst + " = " + reg(B) + " ^ "  + reg(C); // POW
    case 40: return dst + " = " + reg(B) + " // " + reg(C); // IDIV
    case 41: return dst + " = " + reg(B) + " & "  + reg(C); // BAND
    case 42: return dst + " = " + reg(B) + " | "  + reg(C); // BOR
    case 43: return dst + " = " + reg(B) + " ~ "  + reg(C); // BXOR
    case 44: return dst + " = " + reg(B) + " << " + reg(C); // SHL
    case 45: return dst + " = " + reg(B) + " >> " + reg(C); // SHR
    case 46: return dst + " = " + reg(B);                   // MMBIN
    case 47: return "-- mmbini " + reg(A) + fmtImm(sB) + " meta=" + std::to_string(C); // MMBINI
    case 48: return dst + " = " + reg(B);                   // MMBINK
    case 49: return dst + " = -" + reg(B);                  // UNM
    case 50: return dst + " = ~" + reg(B);                  // BNOT
    case 51: return dst + " = not " + reg(B);               // NOT
    case 52: return dst + " = #" + reg(B);                  // LEN
    case 53: {  // CONCAT A B: R[A] = R[A]..R[A+1]..…..R[A+B-1]
        std::string s = dst + " = " + reg(A);
        for (int r = (int)A + 1; r < (int)A + (int)B; ++r)
            s += " .. " + reg(r);
        return s;
    }
    case 54: return "-- close " + reg(A);                   // CLOSE
    case 55: return "-- tbc " + reg(A);                     // TBC (to-be-closed)
    case 56: return "goto " + std::to_string(pc + 1 + ins.fieldSJ54()); // JMP sJ
    case 57: return "if (" + reg(A) + " == " + reg(B) + ") ~= " + std::to_string(k) + " then skip"; // EQ
    case 58: return "if (" + reg(A) + " < "  + reg(B) + ") ~= " + std::to_string(k) + " then skip"; // LT
    case 59: return "if (" + reg(A) + " <= " + reg(B) + ") ~= " + std::to_string(k) + " then skip"; // LE
    case 60: return "if (" + reg(A) + " == " + kstr(B) + ") ~= " + std::to_string(k) + " then skip"; // EQK A B k
    case 61: return "if (" + reg(A) + " == " + std::to_string(sB) + ") ~= " + std::to_string(k) + " then skip"; // EQI A sB k
    case 62: return "if (" + reg(A) + " < "  + std::to_string(sB) + ") ~= " + std::to_string(k) + " then skip"; // LTI A sB k
    case 63: return "if (" + reg(A) + " <= " + std::to_string(sB) + ") ~= " + std::to_string(k) + " then skip"; // LEI A sB k
    case 64: return "if (" + reg(A) + " > "  + std::to_string(sB) + ") ~= " + std::to_string(k) + " then skip"; // GTI A sB k
    case 65: return "if (" + reg(A) + " >= " + std::to_string(sB) + ") ~= " + std::to_string(k) + " then skip"; // GEI A sB k
    case 66: return "if not (" + reg(A) + " == " + std::to_string(k) + ") then skip"; // TEST
    case 67: return dst + " = " + reg(B) + " if " + std::to_string(k); // TESTSET
    case 68: { // CALL
        std::string args;
        int numArgs = (int)B - 1;
        for (int i = 1; i <= numArgs; ++i) {
            if (!args.empty()) args += ", ";
            args += reg(A + i);
        }
        if (B == 0) args = "...";
        int numRets = (int)C - 1;
        if (numRets > 0) {
            std::string rets;
            for (int i = 0; i < numRets; ++i) {
                if (!rets.empty()) rets += ", ";
                rets += reg(A + i);
            }
            return rets + " = " + reg(A) + "(" + args + ")";
        }
        return reg(A) + "(" + args + ")";
    }
    case 69: { // TAILCALL
        std::string args;
        int numArgs = (int)B - 1;
        for (int i = 1; i <= numArgs; ++i) {
            if (!args.empty()) args += ", ";
            args += reg(A + i);
        }
        return "return " + reg(A) + "(" + args + ")";
    }
    case 70: { // RETURN
        int numRets = (int)B - 1;
        if (numRets == 0) return "return";
        if (numRets < 0) return "return ...";
        std::string rets;
        for (int i = 0; i < numRets; ++i) {
            if (!rets.empty()) rets += ", ";
            rets += reg(A + i);
        }
        return "return " + rets;
    }
    case 71: return "return";                                // RETURN0
    case 72: return "return " + reg(A);                     // RETURN1
    case 73: { // FORLOOP A Bx (=73 in Lua 5.4): step + test; if continuing: pc -= Bx
        int bx = (int)ins.fieldBx54();
        return "-- forloop " + reg(A) + " += " + reg(A+2) + "; if <= " + reg(A+1) +
               " goto " + std::to_string(pc + 1 - bx);
    }
    case 74: { // FORPREP A Bx (=74 in Lua 5.4): check values; if not to run then pc += Bx+1
        int bx = (int)ins.fieldBx54();
        return "-- forprep " + reg(A) + " goto " + std::to_string(pc + bx + 2);
    }
    case 75: { // TFORPREP A Bx
        int bx = (int)ins.fieldBx54();
        return "-- tforprep " + reg(A) + " goto " + std::to_string(pc + 1 + bx);
    }
    case 76: // TFORCALL
        {
            std::string rets;
            for (int i = 0; i < (int)C; ++i) {
                if (!rets.empty()) rets += ", ";
                rets += reg(A + 4 + i);
            }
            return rets + " = " + reg(A) + "(" + reg(A+1) + ", " + reg(A+2) + ")";
        }
    case 77: { // TFORLOOP A Bx: jump back by Bx if loop continues
        int bx = (int)ins.fieldBx54();
        return "-- tforloop " + reg(A) + " goto " + std::to_string(pc + 1 - bx);
    }
    case 78: return "-- setlist " + reg(A);                 // SETLIST
    case 79: return dst + " = function() --[[ proto " + std::to_string(B) + " ]] end"; // CLOSURE
    case 80: return dst + " = ...";                         // VARARG
    case 81: return "-- varargprep " + std::to_string(A);  // VARARGPREP
    case 82: return "-- extraarg";                          // EXTRAARG
    default:
        return "-- op54_" + std::to_string(op) +
               " A=" + std::to_string(A) + " B=" + std::to_string(B) +
               " C=" + std::to_string(C);
    }
}

// ─── Prototype disassembly ────────────────────────────────────────────────────

void LuaEmitter::disassemble(const LuaProto& proto,
                               std::ostream& out, int indent_) const {
    // Build register name table from locals
    int nregs = proto.maxStackSize;
    std::vector<std::string> regs(nregs > 0 ? nregs : 256, "");
    for (int r = 0; r < nregs; ++r)
        regs[r] = "reg" + std::to_string(r);

    // Initialise from param names
    for (int i = 0; i < (int)proto.numParams && i < (int)proto.locals.size(); ++i)
        if (i < nregs) regs[i] = proto.locals[i].name;

    out << indent(indent_) << "-- instructions: " << proto.code.size() << "\n";
    for (int pc = 0; pc < (int)proto.code.size(); ++pc) {
        // Update live locals
        for (int r = 0; r < nregs; ++r)
            regs[r] = localName(proto, r, pc);

        std::string line;
        if (proto.version == LuaVersion::Lua54)
            line = decodeInstr54(proto, pc, regs);
        else
            line = decodeInstr51(proto, pc, regs);

        if (opts_.emitLineInfo) {
            int ln = proto.lineForPc(pc);
            if (ln > 0)
                out << indent(indent_) << "--[[ line " << ln << " ]] ";
            else
                out << indent(indent_);
        } else {
            out << indent(indent_);
        }
        out << line << "\n";
    }
}

// ─── Proto emitter ───────────────────────────────────────────────────────────

void LuaEmitter::emitProto(const LuaProto& proto, std::ostream& out,
                             int ind, const std::string& funcName) const {
    // Build parameter list
    std::string params;
    for (int i = 0; i < (int)proto.numParams; ++i) {
        if (!params.empty()) params += ", ";
        if (i < (int)proto.locals.size())
            params += proto.locals[i].name;
        else
            params += "p" + std::to_string(i);
    }
    if (proto.isVarArg) {
        if (!params.empty()) params += ", ";
        params += "...";
    }

    // Function header
    if (!funcName.empty())
        out << indent(ind) << "local function " << funcName
            << "(" << params << ")\n";
    else
        out << indent(ind) << "-- anonymous function (" << params << ")\n";

    // Source info
    if (!proto.source.empty())
        out << indent(ind + 1) << "-- source: " << proto.source << "\n";
    if (proto.lineDefined > 0)
        out << indent(ind + 1) << "-- lines: " << proto.lineDefined
            << "-" << proto.lastLineDefined << "\n";

    // Upvalue declarations
    for (int i = 0; i < (int)proto.upvalues.size(); ++i)
        out << indent(ind + 1) << "-- upvalue[" << i << "]: "
            << proto.upvalueName(i) << "\n";

    // Decompiled body
    disassemble(proto, out, ind + 1);

    // Nested functions
    for (int i = 0; i < (int)proto.protos.size(); ++i) {
        out << "\n";
        std::string nested = funcName.empty()
            ? "inner" + std::to_string(i)
            : funcName + "_f" + std::to_string(i);
        emitProto(proto.protos[i], out, ind + 1, nested);
    }

    if (!funcName.empty())
        out << indent(ind) << "end\n";
}

// ─── Module emitter ──────────────────────────────────────────────────────────

void LuaEmitter::emitModule(const LuaModule& mod, std::ostream& out) const {
    if (opts_.emitFileHeader) {
        out << "-- Generated by RetDec Lua decompiler\n";
        out << "-- Lua version: " << luaVersionStr(mod.version) << "\n";
        if (!mod.topLevel.source.empty())
            out << "-- Source: " << mod.topLevel.source << "\n";
        out << "\n";
    }

    // If top-level has no defined line range it is the chunk itself
    if (mod.topLevel.lineDefined == 0 && mod.topLevel.lastLineDefined == 0) {
        // Emit upvalue info for _ENV
        for (int i = 0; i < (int)mod.topLevel.upvalues.size(); ++i)
            out << "-- upvalue[" << i << "]: " << mod.topLevel.upvalueName(i) << "\n";
        disassemble(mod.topLevel, out, 0);
        // Nested functions
        for (int i = 0; i < (int)mod.topLevel.protos.size(); ++i) {
            out << "\n";
            std::string nm = "func" + std::to_string(i);
            emitProto(mod.topLevel.protos[i], out, 0, nm);
        }
    } else {
        emitProto(mod.topLevel, out, 0, "main");
    }
}

// ─── Public entry point ───────────────────────────────────────────────────────

LuaEmitResult LuaEmitter::emit(const LuaModule& module) const {
    LuaEmitResult result;
    std::ostringstream ss;
    try {
        emitModule(module, ss);
        result.source = ss.str();
    } catch (const std::exception& e) {
        result.warnings.push_back(std::string("emit error: ") + e.what());
        result.source = ss.str();
    }
    return result;
}

} // namespace lua_parser
} // namespace retdec
