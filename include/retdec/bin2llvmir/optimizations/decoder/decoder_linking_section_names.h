/**
 * @file include/retdec/bin2llvmir/optimizations/decoder/decoder_linking_section_names.h
 * @brief PLT/GOT-like section name helpers (P1.5 loader/binding) shared by decoder passes.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_DECODER_DECODER_LINKING_SECTION_NAMES_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_DECODER_DECODER_LINKING_SECTION_NAMES_H

#include <string>

#include "retdec/fileformat/file_format/file_format.h"
#include "retdec/utils/string.h"

namespace retdec {
namespace bin2llvmir {

/// PLT or PLT-like thunk sections (same treatment as @c .plt in range/import logic).
inline bool sectionNameIsPltLike(const std::string &name)
{
	using namespace retdec::utils;
	return name == ".plt" || name == ".plt.sec" || name == ".plt.got"
			|| startsWith(name, ".plt.");
}

/// GOT / Mach-O symbol-pointer tables — import addresses live in pointer slots.
inline bool segmentNameIsGlobalOffsetTableLike(const std::string &name)
{
	using namespace retdec::utils;
	if (name == ".got" || name == ".got.plt")
		return true;
	if (startsWith(name, ".got."))
		return true;
	if (contains(name, "__got"))
		return true;
	if (contains(name, "la_symbol_ptr") || contains(name, "nl_symbol_ptr"))
		return true;
	return false;
}

/// PE import directory / IAT (incl. @c .idata$N) — pointer slots, not “main GOT”.
inline bool segmentNameIsPeImportDataLike(const std::string &name)
{
	using namespace retdec::utils;
	return name == ".idata" || startsWith(name, ".idata");
}

/**
 * @c .got.plt or another GOT-like section whose name indicates PLT slots.
 */
inline const fileformat::Section *findGotPltSection(const fileformat::FileFormat *ff)
{
	if (!ff)
	{
		return nullptr;
	}
	if (auto *s = ff->getSection(".got.plt"))
	{
		return s;
	}
	for (auto *sec : ff->getSections())
	{
		if (!sec)
		{
			continue;
		}
		const std::string &n = sec->getName();
		if (n == ".got.plt")
		{
			return sec;
		}
		if (segmentNameIsGlobalOffsetTableLike(n) && retdec::utils::contains(n, "plt"))
		{
			return sec;
		}
	}
	return nullptr;
}

/**
 * Main global offset table (not the PLT slot table): @c .got or first GOT-like
 * section whose name does not indicate PLT linkage.
 */
inline const fileformat::Section *findMainGotSection(const fileformat::FileFormat *ff)
{
	if (!ff)
	{
		return nullptr;
	}
	if (auto *s = ff->getSection(".got"))
	{
		return s;
	}
	for (auto *sec : ff->getSections())
	{
		if (!sec)
		{
			continue;
		}
		const std::string &n = sec->getName();
		if (retdec::utils::contains(n, "plt"))
		{
			continue;
		}
		if (segmentNameIsGlobalOffsetTableLike(n))
		{
			return sec;
		}
	}
	return nullptr;
}

} // namespace bin2llvmir
} // namespace retdec

#endif
