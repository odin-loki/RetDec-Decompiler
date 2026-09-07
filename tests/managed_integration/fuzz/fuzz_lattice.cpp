/**
 * @file fuzz_lattice.cpp
 * @brief libFuzzer harness for the signature-lattice format parser.
 *
 * src/fileformat/lattice/format_lattice.cpp is the first thing that reads a
 * file the decompiler did not write: it walks the PE optional header and
 * section table, the ELF program and section headers, and the Mach-O load
 * commands, all from fields the input controls. It sits under src/fileformat/,
 * whose other translation units publicly link LLVM -- so fuzz_pe.cpp only runs
 * behind -DRETDEC_FUZZ=ON and the hours-long LLVM build, and this file had no
 * fuzzing at all. The lattice parser itself includes nothing but the standard
 * library, so it needs neither.
 *
 * classify() takes the bytes directly, so the whole header surface is reachable
 * from a flat input with no file system involved.
 *
 * Build and run through scripts/standalone_fuzz.sh (target: lattice).
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/fileformat/lattice/format_lattice.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// The lattice dispatch reads a magic at offset 0; below four bytes every
	// input is the same rejection.
	if (size < 4) return 0;

	try
	{
		const retdec::fileformat::lattice::FormatLattice lattice;
		const auto res = lattice.classify(data, size, "fuzz");

		// Touch the result so nothing above can be optimised away.
		volatile std::size_t sink = res.sections.size() + res.imports.size();
		(void)sink;
	}
	catch (const std::exception&)
	{
		// A malformed file is expected to be rejected. A crash is not.
	}

	return 0;
}
