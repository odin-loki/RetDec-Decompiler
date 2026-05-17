/**
 * @file src/fileformat/utils/format_detection.cpp
 * @brief File format detection.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 *
 * Implements Signature-Lattice Parsing (Stage 1 of decompilation-specific pipeline):
 * - Single read of first 512 bytes, O(1) dispatch via magic-byte decision lattice
 * - Fault-tolerant handling for malformed headers
 * - Polyglot disambiguation (e.g. Java vs Mach-O fat)
 * - Optional computeFormatLatticeHints() for PE/ELF + CAFE polyglot scoring (diagnostics / ties)
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <system_error>

#include <llvm/Object/COFF.h>
#include <llvm/Support/Host.h>

#include "retdec/utils/conversion.h"
#include "retdec/utils/io/log.h"
#include "retdec/utils/string.h"
#include "retdec/fileformat/utils/byte_array_buffer.h"
#include "retdec/fileformat/utils/format_detection.h"
#include "retdec/pelib/PeFile.h"
#include "retdec/pelib/ImageLoader.h"

using namespace retdec::utils;
using namespace llvm;
using namespace llvm::object;
using namespace PeLib;

namespace retdec {
namespace fileformat {

namespace
{

/// Size of buffer for signature-lattice parsing (decision over first N bytes).
const std::size_t LATTICE_BUFFER_SIZE = 512;

const std::size_t COFF_FILE_HEADER_BYTE_SIZE = 20;

bool formatLatticeDiagEnabled()
{
	const char *e = std::getenv("RETDEC_FORMAT_LATTICE_DIAG");
	return e != nullptr && e[0] != '\0' && e[0] != '0';
}

const char *formatLatticeFormatTag(Format f)
{
	switch (f)
	{
	case Format::UNDETECTABLE:
		return "UNDETECTABLE";
	case Format::UNKNOWN:
		return "UNKNOWN";
	case Format::PE:
		return "PE";
	case Format::ELF:
		return "ELF";
	case Format::COFF:
		return "COFF";
	case Format::MACHO:
		return "MACHO";
	case Format::INTEL_HEX:
		return "INTEL_HEX";
	case Format::RAW_DATA:
		return "RAW_DATA";
	}
	return "FORMAT?";
}

const std::map<std::pair<std::size_t, std::string>, Format> unknownFormatMap =
{
	{{0, "\x7""\x1""\x64""\x00"}, Format::UNKNOWN}, // a.out
	{{0, "PS-X EXE"}, Format::UNKNOWN}, // PS-X
	{{257, "ustar"}, Format::UNKNOWN} // tar
};

void resetStream(std::istream& stream)
{
	stream.clear();
	stream.seekg(0, std::ios::beg);
}

std::uint64_t streamSize(std::istream& stream)
{
	stream.seekg(0, std::ios::end);
	std::uint64_t sz =stream.tellg();
	resetStream(stream);
	return sz;
}

/**
 * Check if input file contains PE signature
 * @param stream Input stream
 * @return @c true if input file contains PE signature, @c false otherwise
 */
bool isPe(std::istream& stream)
{
	// Create instance of the ImageLoader with most benevolent flags
	ImageLoader imgLoader(0);

	// Load the image from stream. Only load headers
	return (imgLoader.Load(stream, 0, true) == ERROR_NONE);
}

/**
 * Check if file is Java class
 * @param stream Input stream
 * @return @c true if input file is Java class file, @c false otherwise
 */
bool isJava(std::istream& stream)
{
	resetStream(stream);

	if (!stream)
	{
		return false;
	}

	std::uint32_t magic = 0;
	stream.read(reinterpret_cast<char*>(&magic), 4);

	// Same for both Java and fat Mach-O
	if (magic == 0xcafebabe || magic == 0xbebafeca)
	{
		std::uint32_t fatCount = 0;
		stream.read(reinterpret_cast<char*>(&fatCount), 4);

		if (sys::IsLittleEndianHost)
		{
			// Both are in big endian std::uint8_t order
			fatCount = sys::SwapByteOrder_32(fatCount);
		}

		// Mach-O currently supports up to 18 architectures
		// Java version starts at 39. However file utility uses value 30
		return fatCount > 30;
	}

	return false;
}

/**
 * Check if file is strange format with Mach-O magic.
 * @param stream Input stream
 * @return @c true if input file is likely not Mach-O, @c false otherwise
 */
bool isStrangeFeedface(std::istream& stream)
{
	resetStream(stream);

	if (!stream)
	{
		return false;
	}

	std::uint32_t ints[4];
	stream.read(reinterpret_cast<char*>(&ints), 16);

	if (sys::IsBigEndianHost)
	{
		// All such files found were in little endian std::uint8_t order
		for (int i = 0; i < 4; ++i)
		{
			ints[i] = sys::SwapByteOrder_32(ints[i]);
		}
	}

	if (ints[0] == 0xfeedface && ints[1] == 0x10 && ints[2] == 0x02)
	{
		// Maximal valid Mach-O value is 0x0b but 0x10 will be safer and
		// still remove all unwanted files
		return ints[3] > 0x10;
	}

	return false;
}

bool isKnownCoffMachineLittleEndian(std::uint16_t machine)
{
	switch (machine)
	{
	case 0x014C: case 0x014D: case 0x014E: case 0x0184: case 0x01A2:
	case 0x01A3: case 0x01A4: case 0x01A6: case 0x01A8: case 0x01C0:
	case 0x01C2: case 0x01C4: case 0x01D3: case 0x01F0: case 0x0200:
	case 0x0268: case 0x0290: case 0x0284: case 0x0160:
	case 0x0162: case 0x0163: case 0x0166: case 0x0140: case 0x0142:
	case 0x0266: case 0x0168: case 0x0169: case 0x0366: case 0x0466:
	case 0x0520: case 0x0EBC: case 0x8664: case 0x9041: case 0xAA64:
	case 0xC0EE:
		return true;
	default:
		return false;
	}
}

bool hasCoffBigObjMagicAt12(const std::uint8_t *buf, std::size_t size)
{
	if (size < 28)
	{
		return false;
	}
	static const char bigObjMagic[] =
			"\xc7\xa1\xba\xd1\xee\xba\xa9\x4b\xaf\x20\xfa\xf6\x6a\xa4\xdc\xb8";
	return std::equal(bigObjMagic, bigObjMagic + 16, buf + 12);
}

std::string formatLatticeHintsDiagSuffix(const FormatLatticeHints& h)
{
	std::ostringstream oss;
	oss << "peStrength=" << h.peStrength
			<< " elfStrength=" << h.elfStrength
			<< " ihex=" << h.ihexStrength
			<< " machoSlice=" << h.machoSliceStrength
			<< " coff=" << h.coffStrength
			<< " ar=" << h.arArchiveStrength
			<< " cafe2nd=" << h.cafeBabeSecondWord
			<< " javaLattice=" << h.javaClassLatticeStrength
			<< " machoFatLattice=" << h.machoFatLatticeStrength;
	return oss.str();
}

/**
 * Signature-lattice dispatch: O(1) format detection from magic bytes.
 * Decision tree over first bytes: ELF (7F 45 4C 46), PE (4D 5A), Mach-O, COFF, Intel HEX.
 */
Format dispatchByLattice(const std::string& buf)
{
	if (buf.size() < 4)
	{
		if (buf.size() >= 1 && buf[0] == ':')
			return Format::INTEL_HEX;
		return Format::UNKNOWN;
	}

	// Offset 0: Intel HEX
	if (buf[0] == ':')
		return Format::INTEL_HEX;

	// Offset 0: ELF (7F 45 4C 46)
	if (buf[0] == '\x7F' && buf.size() >= 4 &&
	    buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')
		return Format::ELF;

	// Offset 0: PE (MZ or ZM)
	if (buf.size() >= 2)
	{
		if ((buf[0] == 'M' && buf[1] == 'Z') || (buf[0] == 'Z' && buf[1] == 'M'))
			return Format::PE;
	}

	// Offset 0: Unix ar / thin archive (not a single object — explicit lattice leaf)
	if (buf.size() >= 8
			&& (std::memcmp(buf.data(), "!<arch>\n", 8) == 0
				|| std::memcmp(buf.data(), "!<thin>\n", 8) == 0))
	{
		return Format::UNKNOWN;
	}

	// Offset 0: Mach-O variants (32/64 slice magic vs fat / Java polyglot)
	if (buf.size() >= 4)
	{
		const std::uint32_t magic = static_cast<std::uint8_t>(buf[0]) |
			(static_cast<std::uint8_t>(buf[1]) << 8) |
			(static_cast<std::uint8_t>(buf[2]) << 16) |
			(static_cast<std::uint8_t>(buf[3]) << 24);
		const std::uint32_t magicBE = static_cast<std::uint8_t>(buf[3]) |
			(static_cast<std::uint8_t>(buf[2]) << 8) |
			(static_cast<std::uint8_t>(buf[1]) << 16) |
			(static_cast<std::uint8_t>(buf[0]) << 24);

		// MH_MAGIC, MH_MAGIC_64, MH_CIGAM, MH_CIGAM_64 — no Java ambiguity
		if (magic == 0xFEEDFACE || magic == 0xFEEDFACF ||
		    magicBE == 0xFEEDFACE || magicBE == 0xFEEDFACF)
		{
			return Format::MACHO;
		}

		// FAT_MAGIC / FAT_CIGAM share the first word with Java .class files.
		// Polyglot rule (same as isJava below): second big-endian word is
		// Mach-O architecture count (≤ ~18 today) vs Java minor/major pack; values
		// > 30 are treated as Java — reject here so we never run Mach-O validators.
		if (magic == 0xCAFEBABE || magicBE == 0xCAFEBABE)
		{
			// Need the second word to disambiguate; otherwise treat as unknown (not Mach-O).
			if (buf.size() < 8)
			{
				return Format::UNKNOWN;
			}
			std::uint32_t w =
				static_cast<std::uint8_t>(buf[4]) |
				(static_cast<std::uint8_t>(buf[5]) << 8) |
				(static_cast<std::uint8_t>(buf[6]) << 16) |
				(static_cast<std::uint8_t>(buf[7]) << 24);
			if (sys::IsLittleEndianHost)
			{
				w = sys::SwapByteOrder_32(w);
			}
			if (w > 30)
			{
				return Format::UNKNOWN;
			}
			return Format::MACHO;
		}
	}

	// Offset 0: COFF (2-byte machine type, little-endian)
	if (buf.size() >= 2)
	{
		const std::uint16_t machine = static_cast<std::uint8_t>(buf[0]) |
			(static_cast<std::uint8_t>(buf[1]) << 8);
		if (isKnownCoffMachineLittleEndian(machine))
		{
			return Format::COFF;
		}
	}

	// Offset 0x0C: COFF BigObj
	if (hasCoffBigObjMagicAt12(reinterpret_cast<const std::uint8_t *>(buf.data()),
				buf.size()))
	{
		return Format::COFF;
	}

	return Format::UNKNOWN;
}

} // anonymous namespace

FormatLatticeHints computeFormatLatticeHints(
		const std::uint8_t *data,
		std::size_t size)
{
	FormatLatticeHints h;
	if (data == nullptr || size == 0)
	{
		return h;
	}

	if (data[0] == ':')
	{
		h.ihexStrength = 100;
	}

	if (size >= 4 && data[0] == '\x7F' && data[1] == 'E' && data[2] == 'L'
			&& data[3] == 'F')
	{
		h.elfStrength = 100;
	}

	if (size >= 2
			&& ((data[0] == 'M' && data[1] == 'Z')
					|| (data[0] == 'Z' && data[1] == 'M')))
	{
		h.peStrength = 25;
		if (size >= 0x40)
		{
			const std::uint32_t peOff = static_cast<std::uint32_t>(data[0x3C])
					| (static_cast<std::uint32_t>(data[0x3D]) << 8)
					| (static_cast<std::uint32_t>(data[0x3E]) << 16)
					| (static_cast<std::uint32_t>(data[0x3F]) << 24);
			if (peOff + 4 <= size && data[peOff] == 'P' && data[peOff + 1] == 'E'
					&& data[peOff + 2] == 0 && data[peOff + 3] == 0)
			{
				h.peStrength = 100;
			}
			else if (peOff < size)
			{
				h.peStrength = 40;
			}
		}
	}

	if (size >= 8
			&& (std::memcmp(data, "!<arch>\n", 8) == 0
				|| std::memcmp(data, "!<thin>\n", 8) == 0))
	{
		h.arArchiveStrength = 100;
	}

	// Mach-O thin slice (MH_MAGIC / MH_MAGIC_64 / swapped) — not CAFE fat/Java
	if (size >= 4)
	{
		const std::uint32_t magic = static_cast<std::uint8_t>(data[0])
				| (static_cast<std::uint8_t>(data[1]) << 8)
				| (static_cast<std::uint8_t>(data[2]) << 16)
				| (static_cast<std::uint8_t>(data[3]) << 24);
		const std::uint32_t magicBE = static_cast<std::uint8_t>(data[3])
				| (static_cast<std::uint8_t>(data[2]) << 8)
				| (static_cast<std::uint8_t>(data[1]) << 16)
				| (static_cast<std::uint8_t>(data[0]) << 24);
		if (magic == 0xFEEDFACE || magic == 0xFEEDFACF ||
				magicBE == 0xFEEDFACE || magicBE == 0xFEEDFACF)
		{
			h.machoSliceStrength = 100;
		}
	}

	// CAFE polyglot (Mach-O fat vs Java .class) — mirror dispatchByLattice word test
	if (size >= 8)
	{
		const std::uint32_t magic = static_cast<std::uint8_t>(data[0])
				| (static_cast<std::uint8_t>(data[1]) << 8)
				| (static_cast<std::uint8_t>(data[2]) << 16)
				| (static_cast<std::uint8_t>(data[3]) << 24);
		const std::uint32_t magicBE = static_cast<std::uint8_t>(data[3])
				| (static_cast<std::uint8_t>(data[2]) << 8)
				| (static_cast<std::uint8_t>(data[1]) << 16)
				| (static_cast<std::uint8_t>(data[0]) << 24);
		if (magic == 0xCAFEBABE || magicBE == 0xCAFEBABE)
		{
			std::uint32_t w = static_cast<std::uint8_t>(data[4])
					| (static_cast<std::uint8_t>(data[5]) << 8)
					| (static_cast<std::uint8_t>(data[6]) << 16)
					| (static_cast<std::uint8_t>(data[7]) << 24);
			if (sys::IsLittleEndianHost)
			{
				w = sys::SwapByteOrder_32(w);
			}
			h.cafeBabeSecondWord = w;
			if (w > 30)
			{
				h.javaClassLatticeStrength = 100;
			}
			else
			{
				h.machoFatLatticeStrength = 100;
			}
		}
	}

	if (size >= 2)
	{
		const std::uint16_t machine = static_cast<std::uint8_t>(data[0])
				| (static_cast<std::uint8_t>(data[1]) << 8);
		if (isKnownCoffMachineLittleEndian(machine))
		{
			h.coffStrength = 100;
		}
	}
	if (h.coffStrength == 0 && hasCoffBigObjMagicAt12(data, size))
	{
		h.coffStrength = 100;
	}

	return h;
}

Format detectFileFormat(std::istream &inputStream, bool isRaw)
{
	if (isRaw)
	{
		return Format::RAW_DATA;
	}

	// Signature-lattice: single read of first 512 bytes
	resetStream(inputStream);
	std::string latticeBuf;
	try
	{
		latticeBuf.resize(LATTICE_BUFFER_SIZE);
		inputStream.read(&latticeBuf[0], LATTICE_BUFFER_SIZE);
		latticeBuf.resize(static_cast<std::size_t>(inputStream.gcount()));
	}
	catch (...)
	{
		return Format::UNDETECTABLE;
	}

	// Unknown formats (a.out, PS-X, tar) — check before known formats
	for (const auto& item : unknownFormatMap)
	{
		if (item.first.first + item.first.second.length() <= latticeBuf.size() &&
		    hasSubstringOnPosition(latticeBuf, item.first.second, item.first.first))
		{
			return Format::UNKNOWN;
		}
	}

	// Lattice dispatch — O(1) based on magic bytes
	Format candidate = dispatchByLattice(latticeBuf);

	FormatLatticeHints latticeHints;
	if (!latticeBuf.empty())
	{
		latticeHints = computeFormatLatticeHints(
				reinterpret_cast<const std::uint8_t *>(latticeBuf.data()),
				latticeBuf.size());
	}

	if (formatLatticeDiagEnabled() && !latticeBuf.empty())
	{
		retdec::utils::io::Log::info()
				<< "format lattice: dispatch_candidate=" << formatLatticeFormatTag(candidate)
				<< "(" << static_cast<int>(candidate) << ") "
				<< formatLatticeHintsDiagSuffix(latticeHints) << std::endl;
	}

	// Format-specific validation (requires full stream)
	resetStream(inputStream);

	switch (candidate)
	{
		case Format::PE:
		{
			const bool peOk = isPe(inputStream);
			if (formatLatticeDiagEnabled())
			{
				if (peOk)
				{
					retdec::utils::io::Log::info()
							<< "format lattice: PE ImageLoader validation ok (peStrength="
							<< latticeHints.peStrength << ")" << std::endl;
				}
				else
				{
					retdec::utils::io::Log::info()
							<< "format lattice: PE ImageLoader validation failed; "
							<< formatLatticeHintsDiagSuffix(latticeHints) << std::endl;
				}
			}
			return peOk ? Format::PE : Format::UNKNOWN;
		}
		case Format::COFF:
		{
			if (streamSize(inputStream) < COFF_FILE_HEADER_BYTE_SIZE)
			{
				if (formatLatticeDiagEnabled())
				{
					retdec::utils::io::Log::info()
							<< "format lattice: COFF rejected (stream smaller than "
							<< COFF_FILE_HEADER_BYTE_SIZE << " bytes); "
							<< formatLatticeHintsDiagSuffix(latticeHints) << std::endl;
				}
				return Format::UNKNOWN;
			}
			if (formatLatticeDiagEnabled())
			{
				retdec::utils::io::Log::info()
						<< "format lattice: COFF minimum size check ok; "
						<< formatLatticeHintsDiagSuffix(latticeHints) << std::endl;
			}
			return Format::COFF;
		}
		case Format::MACHO:
		{
			const bool reject = isStrangeFeedface(inputStream)
					|| isJava(inputStream);
			if (formatLatticeDiagEnabled())
			{
				retdec::utils::io::Log::info()
						<< "format lattice: MACHO polyglot guard "
						<< (reject ? "rejected (Java/strange FEEDFACE)"
							    : "passed")
						<< "; " << formatLatticeHintsDiagSuffix(latticeHints) << std::endl;
			}
			return reject ? Format::UNKNOWN : Format::MACHO;
		}
		case Format::ELF:
		case Format::INTEL_HEX:
			if (formatLatticeDiagEnabled())
			{
				retdec::utils::io::Log::info()
						<< "format lattice: " << formatLatticeFormatTag(candidate)
						<< " accepted (no extra stream validation); "
						<< formatLatticeHintsDiagSuffix(latticeHints) << std::endl;
			}
			return candidate;
		default:
			if (formatLatticeDiagEnabled())
			{
				retdec::utils::io::Log::info()
						<< "format lattice: UNKNOWN (no validation branch); "
						<< formatLatticeHintsDiagSuffix(latticeHints) << std::endl;
			}
			return Format::UNKNOWN;
	}
}

/**
 * Detects file format of input file
 * @param filePath Path to input file
 * @param isRaw Is the input is a raw binary?
 * @return Detected file format in enumeration representation
 */
Format detectFileFormat(const std::string &filePath, bool isRaw)
{
	std::ifstream stream(filePath, std::ifstream::in | std::ifstream::binary);
	if(!stream.is_open())
	{
		return Format::UNDETECTABLE;
	}

	return detectFileFormat(stream, isRaw);
}

Format detectFileFormat(const std::uint8_t* data, std::size_t size, bool isRaw)
{
	byte_array_buffer bab(data, size);
	std::istream istream(&bab);

	return detectFileFormat(istream, isRaw);
}

} // namespace fileformat
} // namespace retdec
