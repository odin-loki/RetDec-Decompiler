/**
 * @file fuzz_wasm.cpp
 * @brief libFuzzer harness for the WebAssembly binary parser and disassembler.
 *
 * Build:
 *   clang++ -std=c++17 -fsanitize=fuzzer,address \
 *     -I<repo>/include \
 *     fuzz_wasm.cpp \
 *     -L<build>/src/wasm_parser -lretdec-wasm-parser \
 *     -o fuzz_wasm
 *
 * Run:
 *   mkdir -p corpus_wasm && cp <fixtures>/wasm/*.wasm corpus_wasm/
 *   ./fuzz_wasm corpus_wasm/ -max_total_time=600 -jobs=4 -runs=1000000
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#include "retdec/wasm_parser/wasm_reader.h"
#include "retdec/wasm_parser/wat_emitter.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	retdec::wasm_parser::WasmReader reader(data, size);
	// Must not abort/crash regardless of input; error returns are acceptable.
	auto parsed = reader.read();

	// The disassembler is where a module's declared counts become loop bounds
	// and output length, so stopping at the reader left the larger half of the
	// attack surface unfuzzed.
	if (parsed.ok)
	{
		retdec::wasm_parser::WatEmitter emitter;
		(void)emitter.emit(parsed.module);
	}
	return 0;
}
