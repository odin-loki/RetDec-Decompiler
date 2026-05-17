/**
 * @file include/retdec/cpdetect/packer_section_hints.h
 * @brief Section-name heuristics for known packer / protector layouts (P1.2).
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_CPDETECT_PACKER_SECTION_HINTS_H
#define RETDEC_CPDETECT_PACKER_SECTION_HINTS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "retdec/utils/string.h"

namespace retdec {
namespace cpdetect {

/**
 * @param nameLower section name, already lowercased
 * @return true if name matches common packed/protected PE/COFF section prefixes
 *         (conservative — prefix-based except where noted).
 */
inline bool sectionNameSuggestsEmbeddedPackerSection(const std::string &nameLower)
{
	using namespace utils;
	return startsWith(nameLower, ".upx") || startsWith(nameLower, ".nsp")
			|| startsWith(nameLower, ".aspack") || startsWith(nameLower, ".mpress")
			|| startsWith(nameLower, ".packed")
			|| startsWith(nameLower, ".themida") || startsWith(nameLower, ".tmd")
			|| startsWith(nameLower, ".petite") || startsWith(nameLower, ".fsg")
			|| startsWith(nameLower, ".neolite")
			|| startsWith(nameLower, ".vmp") // VMProtect-style
			|| startsWith(nameLower, ".enigma") // Enigma protector-style
			|| startsWith(nameLower, ".pec") // PECompact (.pec, .pec1, …)
			|| startsWith(nameLower, ".svkp") // SVKP
			|| startsWith(nameLower, ".pebundle")
			|| startsWith(nameLower, ".upack") // UPack (distinct from UPX)
			|| startsWith(nameLower, ".npack")
			|| startsWith(nameLower, ".kkrunchy")
			|| startsWith(nameLower, ".rlpack")
			|| startsWith(nameLower, ".adata") // ASPack-style data section
			|| startsWith(nameLower, ".udata") // ASPack-style unpacked data
			|| startsWith(nameLower, ".mew") // MEW / similar packers
			|| startsWith(nameLower, ".dsst") // Diablo / Delphi stub-style (.dsstext, …)
			|| startsWith(nameLower, ".dyndata") // ASProtect-style dynamic data
			|| startsWith(nameLower, ".alien") // AlienExe / similar layouts
			|| startsWith(nameLower, ".spin") // PESpin-style
			|| startsWith(nameLower, ".epack") // exe-pack stubs (.epack, …)
			|| startsWith(nameLower, ".shrink") // Shrinker / similar
			|| startsWith(nameLower, ".exef") // ExeFog-style
			|| startsWith(nameLower, ".pklite") // PKLite (.pklite, …)
			|| startsWith(nameLower, ".xt") // eXPressor-style (.xt, .xt0, …)
			|| startsWith(nameLower, ".winup") // WinUpack-style
			|| startsWith(nameLower, ".pearmor") // PEArmor-style
			|| startsWith(nameLower, ".yoda") // Yoda's Crypter / similar
			|| startsWith(nameLower, ".bob") // BobSoft packer-style
			|| startsWith(nameLower, ".rpcrypt") // RPCrypt-style
			|| startsWith(nameLower, ".telock") // TELOCK / similar
			|| startsWith(nameLower, ".pediy") // PE editing / DIY pack-style stubs
			|| startsWith(nameLower, ".dark") // DarkCrypt / similar
			|| startsWith(nameLower, ".fish") // FishPE / similar
			|| startsWith(nameLower, ".packman") // Packman-style (distinct from `.packed*`)
			|| startsWith(nameLower, ".wwpack"); // WWPack / similar
}

/**
 * @param nameLower section name, already lowercased
 * @return true if name resembles a normal compiler code section (.text, __TEXT,__text, …)
 */
inline bool sectionNameLooksLikeTypicalCodeSection(const std::string &nameLower)
{
	using namespace utils;
	return nameLower == ".text" || nameLower == "__text" || nameLower == "code"
			|| nameLower == ".code" || startsWith(nameLower, ".text")
			|| startsWith(nameLower, "__text") || contains(nameLower, "__text");
}

/**
 * P1.2 section name vs permission mismatch: name resembles a normal compiler code section
 * (.text, …) but the section is not marked code/executable — uncommon in benign PE/ELF.
 */
inline bool typicalCodeSectionNameButNotMarkedCode(bool sectionIsCode, const std::string &nameLower)
{
	return !sectionIsCode && sectionNameLooksLikeTypicalCodeSection(nameLower);
}

/**
 * PE-only: section is marked code/executable but name looks like a writable data segment
 * (packers sometimes place stub code in ".data").
 */
inline bool peExecutableSectionNamedLikeWritableData(
		bool isPe,
		bool sectionIsCode,
		const std::string &nameLower)
{
	if (!isPe || !sectionIsCode)
	{
		return false;
	}
	using namespace utils;
	return startsWith(nameLower, ".data") || nameLower == ".bss";
}

/**
 * Estimate how full the PE IAT data-directory region is (import thunks vs directory size).
 * @return 0–100, or nullopt if inputs are unusable (no imports, zero size, or IAT smaller than one slot).
 */
inline std::optional<unsigned> peIatThunkFillPercent(
		std::size_t importCount,
		std::uint64_t iatDirectorySize,
		bool is64Bit)
{
	if (importCount == 0 || iatDirectorySize == 0)
	{
		return std::nullopt;
	}
	const unsigned ptrB = is64Bit ? 8u : 4u;
	if (iatDirectorySize < ptrB)
	{
		return std::nullopt;
	}
	const std::uint64_t usedBytes = importCount * static_cast<std::uint64_t>(ptrB);
	return static_cast<unsigned>(
			std::min<std::uint64_t>(100, (usedBytes * 100ULL) / iatDirectorySize));
}

} // namespace cpdetect
} // namespace retdec

#endif
