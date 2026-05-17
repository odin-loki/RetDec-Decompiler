/**
 * @file src/lua_parser/lua_types.cpp
 */

#include "retdec/lua_parser/lua_types.h"

#include <iomanip>
#include <sstream>

namespace retdec {
namespace lua_parser {

std::string luaVersionStr(LuaVersion v) {
    switch (v) {
    case LuaVersion::Lua51: return "5.1";
    case LuaVersion::Lua52: return "5.2";
    case LuaVersion::Lua53: return "5.3";
    case LuaVersion::Lua54: return "5.4";
    default: return "unknown";
    }
}

std::string luaConstStr(const LuaConst& c) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, LuaNil>)   return "nil";
        if constexpr (std::is_same_v<T, LuaBool>)  return v.value ? "true" : "false";
        if constexpr (std::is_same_v<T, LuaInt>)   return std::to_string(v.value);
        if constexpr (std::is_same_v<T, LuaFloat>) {
            double val = v.value;
            // Display whole numbers as integers (Lua 5.1 stores all numbers as doubles)
            if (val == static_cast<double>(static_cast<int64_t>(val))
                    && val >= -9.0e18 && val <= 9.0e18) {
                return std::to_string(static_cast<int64_t>(val));
            }
            std::ostringstream ss;
            ss << std::setprecision(14) << val;
            std::string s = ss.str();
            if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
                s += ".0";
            return s;
        }
        if constexpr (std::is_same_v<T, LuaStr>) {
            // Emit as double-quoted Lua string with escaping
            std::string r = "\"";
            for (char ch : v.value) {
                if (ch == '"')       r += "\\\"";
                else if (ch == '\\') r += "\\\\";
                else if (ch == '\n') r += "\\n";
                else if (ch == '\r') r += "\\r";
                else if (ch == '\t') r += "\\t";
                else if ((unsigned char)ch < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\%d", (unsigned char)ch);
                    r += buf;
                } else r += ch;
            }
            r += '"';
            return r;
        }
        return "nil";
    }, c);
}

// ─── LuaProto helpers ────────────────────────────────────────────────────────

int LuaProto::lineForPc(int pc) const {
    if (pc >= 0 && pc < (int)lineInfo.size()) return lineInfo[pc];
    return 0;
}

std::string LuaProto::localName(int reg, int pc) const {
    // Find the most-specific active local for this register at pc
    for (const auto& loc : locals) {
        if (loc.startPc <= pc && pc < loc.endPc) {
            // Count how many locals map to this register index
            int r = 0;
            for (const auto& l2 : locals) {
                if (l2.startPc <= pc && pc < l2.endPc) {
                    if (r == reg) return l2.name;
                    ++r;
                }
            }
        }
    }
    return "reg" + std::to_string(reg);
}

std::string LuaProto::upvalueName(int idx) const {
    if (idx >= 0 && idx < (int)upvalues.size() && !upvalues[idx].name.empty())
        return upvalues[idx].name;
    return "upval" + std::to_string(idx);
}

} // namespace lua_parser
} // namespace retdec
