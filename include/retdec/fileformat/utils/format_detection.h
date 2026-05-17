/**
 * @file include/retdec/fileformat/utils/format_detection.h
 * @brief File format detection.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_FILEFORMAT_UTILS_FORMAT_DETECTION_H
#define RETDEC_FILEFORMAT_UTILS_FORMAT_DETECTION_H

#include <cstdint>

#include "retdec/fileformat/fftypes.h"

namespace retdec {
namespace fileformat {

/**
 * Polyglot / lattice hints from the first bytes only (no full stream validation).
 * Strength 0–100: higher = stronger independent signal for that container family.
 * Used for diagnostics and tie-breaking; see detectFileFormat() for CAFE short-header handling.
 */
struct FormatLatticeHints
{
	unsigned elfStrength = 0; ///< ELF magic at offset 0
	unsigned peStrength = 0;  ///< MZ / ZM + optional PE signature via e_lfanew
	unsigned ihexStrength = 0; ///< First byte ':' (Intel HEX record)
	/// Mach-O 32/64 slice magic at 0 (MH_MAGIC / MH_CIGAM family), not fat/Java CAFE.
	unsigned machoSliceStrength = 0;
	unsigned coffStrength = 0; ///< COFF machine id at 0 or BigObj magic at 0x0C
	unsigned arArchiveStrength = 0; ///< Unix `ar` / thin archive signature at 0
	/// When magic at 0 is 0xCAFEBABE (LE or BE layout), second u32 interpreted like
	/// dispatchByLattice (big-endian field value); 0 if not applicable.
	unsigned cafeBabeSecondWord = 0;
	unsigned javaClassLatticeStrength = 0;   ///< CAFE + second word > 30 (Java .class pack)
	unsigned machoFatLatticeStrength = 0; ///< CAFE + second word <= 30 (Mach-O fat nfat_arch)
};

FormatLatticeHints computeFormatLatticeHints(
		const std::uint8_t *data,
		std::size_t size);

Format detectFileFormat(
		const std::string& filePath,
		bool isRaw = false);

Format detectFileFormat(
		std::istream &inputStream,
		bool isRaw = false);

Format detectFileFormat(
		const std::uint8_t* data,
		std::size_t size,
		bool isRaw = false);

} // namespace fileformat
} // namespace retdec

#endif
