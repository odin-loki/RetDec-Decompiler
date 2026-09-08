/**
 * @file fuzz_lua.cpp
 * @brief libFuzzer harness for the Lua bytecode reader and emitter.
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON ... && cmake --build build/linux --target fuzz_lua
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/lua_parser/lua_emitter.h"
#include "retdec/lua_parser/lua_reader.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	retdec::lua_parser::LuaReader reader(data, size);
	// Must not abort/crash regardless of input; error returns are acceptable.
	auto parsed = reader.read();

	// Reading a prototype only checks the counts; the emitter is what walks
	// the instructions and the nesting the file declared, so it needs the same
	// exposure.
	if (parsed.ok)
	{
		retdec::lua_parser::LuaEmitter emitter;
		(void)emitter.emit(parsed.module);
	}
	return 0;
}
