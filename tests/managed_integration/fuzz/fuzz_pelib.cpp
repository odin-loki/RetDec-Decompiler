/**
 * @file fuzz_pelib.cpp
 * @brief libFuzzer harness for the PeLib PE parser.
 *
 * src/pelib is 9,791 lines that read attacker-controlled bytes, and until this
 * harness it had neither a unit suite nor any fuzzing. fuzz_pe.cpp does not
 * cover it: that one drives retdec::fileformat, which publicly links LLVM, so
 * it only runs behind -DRETDEC_FUZZ=ON. PeLib itself needs nothing but a C++17
 * compiler.
 *
 * loadPeHeaders takes a ByteBuffer directly, so the whole header and directory
 * surface is reachable from a flat input with no file system involved.
 *
 * Build and run through scripts/standalone_fuzz.sh (target: pelib).
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/pelib/PeFile.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <sstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// A 32-bit PE header is 24 bytes past e_lfanew; below that there is nothing
	// to parse and every input is the same rejection.
	if (size < 64) return 0;

	try
	{
		PeLib::ByteBuffer buf(data, data + size);

		// Drive it through a stream, which is how PeFileT is actually used --
		// the directory readers below take their bytes from m_iStream, not from
		// the ByteBuffer, so a default-constructed file would have them reading
		// an unopened stream rather than the input.
		std::string asString(reinterpret_cast<const char*>(data), size);
		std::istringstream stream(asString, std::ios::binary);
		PeLib::PeFileT file(stream);

		// Headers first: everything below depends on them, and each directory
		// reader is exercised whether or not the headers parsed, since a
		// partially-initialised file is exactly the state a malformed input
		// leaves behind.
		(void)file.loadPeHeaders(buf);

		(void)file.readExportDirectory();
		(void)file.readImportDirectory();
		(void)file.readBoundImportDirectory();
		(void)file.readResourceDirectory();
		(void)file.readRelocationsDirectory();
		(void)file.readComHeaderDirectory();
		(void)file.readIatDirectory();
		(void)file.readDebugDirectory();
		(void)file.readTlsDirectory();
		(void)file.readDelayImportDirectory();
		(void)file.readSecurityDirectory();

		// The Rich header is addressed by an explicit offset and size, which is
		// its own bounds surface. Drive it with values taken from the input.
		const std::size_t offset = data[0] | (std::size_t(data[1]) << 8);
		const std::size_t len = data[2] | (std::size_t(data[3]) << 8);
		(void)file.readRichHeader(offset, len);
	}
	catch (const std::exception&)
	{
		// Rejecting malformed input is correct; only a crash is a bug.
	}
	catch (...)
	{}

	return 0;
}
