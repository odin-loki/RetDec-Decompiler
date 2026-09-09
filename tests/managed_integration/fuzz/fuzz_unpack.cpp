/**
 * @file fuzz_unpack.cpp
 * @brief libFuzzer harness for the emulating unpacker, fed by the lattice.
 *
 * This is the pipeline as the decompiler runs it: the signature lattice
 * classifies the bytes and hands a FormatResult to MiniUnpacker, which maps
 * the image and emulates from its entry point. Every address the emulator
 * touches -- the entry point, the section bases, whatever the emulated code
 * writes -- comes out of the file. Reading it by hand had already turned up a
 * dump that was sized from a capped allocation while the section walk still
 * indexed past it; nothing fuzzed the two together.
 *
 * The instruction budget is deliberately small. The default is ten million,
 * which at fuzzing rates buys nothing: the interesting states are reached in
 * the first few thousand, and a budget that ends the run keeps the executions
 * per second high enough to find them.
 *
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/fileformat/lattice/format_lattice.h"
#include "retdec/mini_emu/mini_unpacker.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size == 0 || size > 1u << 20)
	{
		return 0;
	}

	static const retdec::fileformat::lattice::FormatLattice lattice;
	const auto fmt = lattice.classify(data, size, std::string("fuzz_input"));

	const retdec::mini_emu::MiniUnpacker unpacker;
	(void)unpacker.unpack(data, size, fmt, /*maxInsns=*/4000);
	return 0;
}
