/**
 * @file fuzz_lua.cpp
 * @brief libFuzzer harness for the Lua bytecode reader (LuaReader).
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON ... && cmake --build build/linux --target fuzz_lua
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/lua_parser/lua_reader.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    retdec::lua_parser::LuaReader reader(data, size);
    // Must not abort/crash regardless of input; error returns are acceptable.
    (void)reader.read();
    return 0;
}
