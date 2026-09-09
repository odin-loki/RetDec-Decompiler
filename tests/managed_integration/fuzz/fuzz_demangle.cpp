/**
 * @file fuzz_demangle.cpp
 * @brief libFuzzer harness for the mangled-symbol seeders.
 *
 * A mangled symbol is attacker-controlled text: it comes out of the binary's
 * symbol table, and every one of the four seeders is a hand-written parser
 * over it. Nothing fuzzed them until now, and reading them had already turned
 * up unbounded recursion in the Itanium parser, a length that could be read
 * past the end of a Rust symbol, and a Swift builtin match that was not
 * longest-first.
 *
 * The input is taken as the symbol itself rather than as a file, so the
 * corpus is one symbol per file.
 *
 * Build (or let scripts/standalone_fuzz.sh do it):
 *   clang++ -std=c++17 -fsanitize=fuzzer,address,undefined -Iinclude \
 *     fuzz_demangle.cpp src/type_seed/[a-z]*.cpp -o fuzz_demangle
 *
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/type_seed/type_seed.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// A symbol table entry is bounded in practice; a megabyte of one byte
	// repeated says nothing about the parsers and costs the whole time budget.
	if (size > 4096)
	{
		return 0;
	}

	// Built once. The dispatcher holds the four seeders and no per-call state.
	static const auto dispatcher = retdec::type_seed::makeDefaultDispatcher();

	const std::string symbol(reinterpret_cast<const char*>(data), size);

	// Must not abort, hang or read out of bounds, whatever the bytes are.
	(void)dispatcher.tryExtract(symbol);
	return 0;
}
