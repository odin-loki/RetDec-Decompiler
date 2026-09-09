/**
 * @file fuzz_loader_sim.cpp
 * @brief libFuzzer harness for the loader simulator.
 *
 * LoaderSim walks a PE or ELF image's section table, base relocations,
 * imports, delay imports and TLS callbacks -- every one of them a structure
 * whose offsets and counts come out of the file. Reading it by hand had
 * already turned up an ELF32 addend read at the wrong offset; nothing fuzzed
 * it.
 *
 * The first input byte carries the three flags the constructor takes (64-bit,
 * ELF, little-endian) so that one corpus reaches all six combinations; the
 * rest is the image.
 *
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/loader_sim/loader_sim.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size < 2)
	{
		return 0;
	}

	const uint8_t flags = data[0];
	const uint8_t* image = data + 1;
	const size_t imageSize = size - 1;

	retdec::loader_sim::LoaderSim sim(
		image,
		imageSize,
		/*imageBase=*/0x400000,
		/*is64Bit=*/(flags & 1u) != 0,
		/*isELF=*/(flags & 2u) != 0,
		/*isLE=*/(flags & 4u) != 0);

	// Must not abort, hang or read out of bounds, whatever the bytes are.
	(void)sim.load();
	return 0;
}
