/**
 * @file src/cpdetect/heuristics/heuristics.cpp
 * @brief Class for heuristics detection.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#include <memory>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <regex>
#include <vector>

#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include "retdec/utils/container.h"
#include "retdec/utils/conversion.h"
#include "retdec/utils/string.h"
#include "retdec/cpdetect/packer_section_hints.h"
#include "retdec/cpdetect/heuristics/heuristics.h"
#include "retdec/fileformat/file_format/pe/pe_format.h"
#include "retdec/fileformat/types/rich_header/rich_header.h"
#include "retdec/fileformat/types/import_table/pe_import.h"
#include "retdec/fileformat/types/sec_seg/elf_section.h"
#include "retdec/fileformat/types/sec_seg/pe_coff_section.h"
#include "retdec/fileformat/utils/conversions.h"
#include "retdec/pelib/PeLibAux.h"
#include "retdec/fileformat/utils/other.h"

using namespace retdec::utils;
using namespace retdec::fileformat;

namespace retdec {
namespace cpdetect {

namespace
{

bool retdecEnvDiagEnabled(const char *name)
{
	const char *e = std::getenv(name);
	return e && e[0] != '\0' && e[0] != '0';
}

/**
 * Append PE Rich record with largest @c count (tool invocation frequency) to @a extra.
 */
void appendPeRichMaxUsesToExtra(
		std::string &extra,
		const retdec::fileformat::RichHeader *rh)
{
	if (rh == nullptr || !rh->getValidStructure() || rh->getNumberOfRecords() == 0)
	{
		return;
	}
	const std::size_t n = rh->getNumberOfRecords();
	const retdec::fileformat::LinkerInfo *maxUseRec = nullptr;
	std::uint32_t maxUseN = 0;
	for (std::size_t ui = 0; ui < n; ++ui)
	{
		const auto *r = rh->getRecord(ui);
		if (r == nullptr)
		{
			break;
		}
		const std::uint32_t u = r->getNumberOfUses();
		if (maxUseRec == nullptr || u > maxUseN)
		{
			maxUseN = u;
			maxUseRec = r;
		}
	}
	if (maxUseRec != nullptr && maxUseN > 0)
	{
		extra += ";pe_rich_max_uses_pid="
				+ std::to_string(maxUseRec->getProductId());
		extra += ";pe_rich_max_uses_build="
				+ std::to_string(maxUseRec->getProductBuild());
		extra += ";pe_rich_max_uses_count=" + std::to_string(maxUseN);
	}
}

const std::size_t MINIMUM_GO_FUNCTIONS = 5;
const std::size_t MINIMUM_RUST_FUNCTIONS = 5;

const std::size_t MINIMUM_GHC_SYMBOLS = 15;
const std::size_t MINIMUM_GHC_RECORD_SIZE = 9; // "GHC X.X.X"

/**
 * Delphi version names
 *
 * Source: http://delphi.wikia.com/wiki/CompilerVersion_Constant
 */
const std::map<std::string, std::string> delphiVersionMap =
{
	{"32.0", "10.2 Tokyo"},
	{"31.0", "10.1 Berlin"},
	{"30.0", "10 Seattle"},
	{"29.0", "XE8"},
	{"28.0", "XE7"},
	{"27.0", "XE6"},
	{"26.0", "XE5"},
	{"25.0", "XE4"},
	{"24.0", "XE3"},
	{"23.0", "XE2"},
	{"22.0", "XE"},
};

/**
 * Delphi compiler version identification strings with version offset
 *
 * Order matters for iPhone strings, we have to look first for longer.
 * If string is found at position x, version is placed at x + offset.
 */
const std::vector<std::pair<std::string, std::size_t>> delphiStrings =
{
	{"for Win", 46},
	{"for Android", 48},
	{"for Mac OS X", 49},
	{"for Linux 64 bit", 53},
	{"Next Generation for iPhone Simulator", 73},
	{"Next Generation for iPhone ARM64", 69},
	{"Next Generation for iPhone", 63}
};

/**
 * Get name of original programming language as string
 */
bool getDwarfLanguageString(uint64_t langCode, std::string &result)
{
	switch(langCode)
	{
		case llvm::dwarf::DW_LANG_C:
		case llvm::dwarf::DW_LANG_C89:
		case llvm::dwarf::DW_LANG_C99:
			result = "C";
			return true;
		case llvm::dwarf::DW_LANG_C_plus_plus:
			result = "C++";
			return true;
		case llvm::dwarf::DW_LANG_ObjC:
			result = "Objective-C";
			return true;
		case llvm::dwarf::DW_LANG_ObjC_plus_plus:
			result = "Objective-C++";
			return true;
		case llvm::dwarf::DW_LANG_Ada83:
		case llvm::dwarf::DW_LANG_Ada95:
			result = "Ada";
			return true;
		case llvm::dwarf::DW_LANG_Cobol74:
		case llvm::dwarf::DW_LANG_Cobol85:
			result = "Cobol";
			return true;
		case llvm::dwarf::DW_LANG_Fortran77:
		case llvm::dwarf::DW_LANG_Fortran90:
		case llvm::dwarf::DW_LANG_Fortran95:
			result = "Fortran";
			return true;
		case llvm::dwarf::DW_LANG_Modula2:
		case llvm::dwarf::DW_LANG_Modula3:
			result = "Modula";
			return true;
		case llvm::dwarf::DW_LANG_Java:
			result = "Java";
			return true;
		case llvm::dwarf::DW_LANG_Pascal83:
			result = "Pascal";
			return true;
		case llvm::dwarf::DW_LANG_PLI:
			result = "PL/I";
			return true;
		case llvm::dwarf::DW_LANG_UPC:
			result = "Unified Parallel C";
			return true;
		case llvm::dwarf::DW_LANG_D:
			result = "D";
			return true;
		case llvm::dwarf::DW_LANG_Python:
			result = "Python";
			return true;
		case llvm::dwarf::DW_LANG_OpenCL:
			result = "Open Computing Language";
			return true;
		case llvm::dwarf::DW_LANG_Go:
			result = "Go";
			return true;
		case llvm::dwarf::DW_LANG_Haskell:
			result = "Haskell";
			return true;
		default:
			return false;
	}
}

/**
 * Is the given symbol a function from the Go language?
 * @param symbol Input symbol
 * @return @c true if symbol is Go symbol, @c false otherwise
 */
bool isGoFunction(const std::shared_ptr<retdec::fileformat::Symbol> &symbol)
{
	if (!symbol->isFunction())
	{
		// Ignore data and other symbols
		return false;
	}

	const auto &name = symbol->getName();
	return startsWith(name, "__go_") || startsWith(name, "__cgo_");
}

/**
 * Is the given symbol a function from the Rust?
 * @param symbol Input symbol
 * @return @c true if symbol is rust symbol, @c false otherwise
 */
bool isRustFunction(const std::shared_ptr<retdec::fileformat::Symbol> &symbol)
{
	if (!symbol->isFunction())
	{
		// Ignore data and other symbols
		return false;
	}

	const auto &name = symbol->getName();
	return startsWith(name, "__rust_") || startsWith(name, "rust_");
}

/**
 * Is the given symbol from the GHC Haskell?
 * @param symbol Input symbol
 * @return @c true if symbol is GHC symbol, @c false otherwise
 */
bool isGhcSymbol(const std::shared_ptr<retdec::fileformat::Symbol> &symbol)
{
	const auto offset = symbol->getName().find("base_GHC");
	return offset == 0 || offset == 1;
}

/**
 * Convert Embarcadero Delphi version to text extra information
 * @param version compiler version
 * @return extra info
 */
std::string embarcaderoVersionToExtra(const std::string &version)
{
	auto pair = delphiVersionMap.find(version);
	if (pair != delphiVersionMap.end())
	{
		return pair->second;
	}

	return std::string();
}

/**
 * Get file format specific comment section name
 * @param format input file format
 * @return typical comment section name
 */
std::string commentSectionNameByFormat(Format format)
{
	switch (format) {
		case Format::PE:
			return ".rdata";

		case Format::ELF:
			return ".comment";

		case Format::MACHO:
			return "__comment";

		default:
			return std::string();
	}
}

} // anonymous namespace

/**
 * Constructor
 * @param parser Parser of input file
 * @param searcher Signature search engine
 * @param toolInfo Structure for information about detected tools
 */
Heuristics::Heuristics(
		retdec::fileformat::FileFormat &parser,
		Search &searcher,
		ToolInformation &toolInfo)
		: fileParser(parser)
		, search(searcher)
		, toolInfo(toolInfo)
{
	canSearch = search.isFileLoaded() && search.isFileSupported();

	const auto secCounter = fileParser.getNumberOfSections();
	sections.reserve(secCounter);
	for (std::size_t i = 0; i < secCounter; ++i)
	{
		const auto *fsec = fileParser.getSection(i);
		if (fsec)
		{
			sections.push_back(fsec);

			// Add names to map
			auto secName = fsec->getName();
			if (!secName.empty()) {
				sectionNameMap[secName]++;
			}
		}
	}

	noOfSections = sections.size();
}

/**
 * Save all information about detected compiler
 * @param source Used detection method
 * @param strength Strength of detection method
 * @param name Name of detected compiler
 * @param version Version of detected compiler
 * @param extra Extra information about compiler
 */
void Heuristics::addCompiler(
		DetectionMethod source,
		DetectionStrength strength,
		const std::string &name,
		const std::string &version,
		const std::string &extra)
{
	toolInfo.addTool(source, strength, ToolType::COMPILER, name, version, extra);
}

/**
 * Save all information about detected linker
 * @param source Used detection method
 * @param strength Strength of detection method
 * @param name Name of detected linker
 * @param version Version of detected linker
 * @param extra Extra information about linker
 */
void Heuristics::addLinker(
		DetectionMethod source,
		DetectionStrength strength,
		const std::string &name,
		const std::string &version,
		const std::string &extra)
{
	toolInfo.addTool(source, strength, ToolType::LINKER, name, version, extra);
}

/**
 * Save all information about detected installer
 * @param source Used detection method
 * @param strength Strength of detection method
 * @param name Name of detected installer
 * @param version Version of detected installer
 * @param extra Extra information about installer
 */
void Heuristics::addInstaller(
		DetectionMethod source,
		DetectionStrength strength,
		const std::string &name,
		const std::string &version,
		const std::string &extra)
{
	toolInfo.addTool(source, strength, ToolType::INSTALLER, name, version, extra);
}

/**
 * Save all information about detected packer
 * @param source Used detection method
 * @param strength Strength of detection method
 * @param name Name of detected packer
 * @param version Version of detected packer
 * @param extra Extra information about packer
 */
void Heuristics::addPacker(
		DetectionMethod source,
		DetectionStrength strength,
		const std::string &name,
		const std::string& version,
		const std::string& extra)
{
	toolInfo.addTool(source, strength, ToolType::PACKER, name, version, extra);
}

/**
 * Save all information about detected compiler
 * @param matchNibbles Number of significant nibbles agreeing with file content
 * @param totalNibbles Total number of significant nibbles of signature
 * @param name Name of detected compiler
 * @param version Version of detected compiler
 * @param extra Extra information about compiler
 *
 * This method implies DetectResultSource::SIGNATURE. Strength is computed.
 */
void Heuristics::addCompiler(
		std::size_t matchNibbles,
		std::size_t totalNibbles,
		const std::string &name,
		const std::string &version,
		const std::string &extra)
{
	toolInfo.addTool(matchNibbles, totalNibbles, ToolType::COMPILER, name, version, extra);
}

/**
 * Save all information about detected packer
 * @param matchNibbles Number of significant nibbles agreeing with file content
 * @param totalNibbles Total number of significant nibbles of signature
 * @param name Name of detected packer
 * @param version Version of detected packer
 * @param extra Extra information about packer
 *
 * This method implies DetectResultSource::SIGNATURE. Strength is computed.
 */
void Heuristics::addPacker(
		std::size_t matchNibbles,
		std::size_t totalNibbles,
		const std::string &name,
		const std::string &version,
		const std::string &extra)
{
	toolInfo.addTool(matchNibbles, totalNibbles, ToolType::PACKER, name, version, extra);
}

/**
 * Add information about detected programming language
 * @param name Name of detected programming language
 * @param extraInfo Additional information about language
 * @param isBytecode @c true if detected language is bytecode,
 *                   @c false otherwise
 */
void Heuristics::addLanguage(
		const std::string &name, const std::string &extraInfo, bool isBytecode)
{
	if (priorityLanguageIsSet)
	{
		return;
	}

	toolInfo.addLanguage(name, extraInfo, isBytecode);
}

/**
 * Add information about detected programming language
 * @param name Name of detected programming language
 * @param extraInfo Additional information about language
 * @param isBytecode @c true if detected language is bytecode,
 *                   @c false otherwise
 *
 * This removes previously detected languages and prevents further detections
 */
void Heuristics::addPriorityLanguage(
		const std::string &name, const std::string &extraInfo, bool isBytecode)
{
	if (priorityLanguageIsSet)
	{
		return;
	}

	priorityLanguageIsSet = true;
	toolInfo.detectedLanguages.clear();
	toolInfo.addLanguage(name, extraInfo, isBytecode);
}

/**
 * Get number of sections which have name equal to @a sectionName
 * @param sectionName Required section name
 * @return Number of sections which have name equal to @a sectionName
 */
std::size_t Heuristics::findSectionName(const std::string &sectionName) const
{
	return mapGetValueOrDefault(sectionNameMap, sectionName, 0);
}

/**
 * Get number of sections with name starting with @a sectionName
 * @param sectionName Required section name
 * @return Number of sections which have name equal to @a sectionName
 */
std::size_t Heuristics::findSectionNameStart(
		const std::string &sectionName) const
{
	std::size_t result = 0;
	for (const Section* section : sections)
	{
		std::string name = section->getName();
		if (startsWith(name, sectionName))
		{
			result++;
		}
	}

	return result;
}

/**
 * Try to detect tools by section names
 */
void Heuristics::getSectionHeuristics()
{
	auto source = DetectionMethod::SECTION_TABLE_H;
	auto strength = DetectionStrength::HIGH;

	if (!noOfSections)
	{
		return;
	}

	// Compiler detections
	if (findSectionName(".go_export"))
	{
		addCompiler(source, strength, "gccgo");
		addPriorityLanguage("Go");
	}
	if (findSectionName(".note.go.buildid"))
	{
		addCompiler(source, strength, "gc");
		addPriorityLanguage("Go");
	}
	if (findSectionName(".gosymtab") || findSectionName(".gopclntab"))
	{
		addPriorityLanguage("Go");
	}
	if (findSectionName(".debug-ghc-link-info"))
	{
		addCompiler(source, strength, "GHC");
		addPriorityLanguage("Haskell");
	}
	if (findSectionName(".HP.init"))
	{
		addCompiler(source, strength, "HP C++");
		addLanguage("C++");
	}
}

/**
 * Parse GCC record from comment section
 * @param record Record from comment section
 * @return @c true if compiler was detected, @c false otherwise
 */
bool Heuristics::parseGccComment(const std::string &record)
{
	auto source = DetectionMethod::COMMENT_H;
	auto strength = DetectionStrength::LOW;

	const std::string prefix = "GCC: ";
	if (!startsWith(record, prefix))
	{
		return false;
	}

	static std::regex e("\\([^\\)]+\\)");
	std::string version = extractVersion(std::regex_replace(record, e, ""));
	if (!version.empty())
	{
		addCompiler(source, strength, "GCC", version);
		return true;
	}

	return false;
}

/**
 * Parse GHC record from comment section
 * @param record Record from comment section
 * @return @c true if GHC was detected, @c false otherwise
 */
bool Heuristics::parseGhcComment(const std::string &record)
{
	auto source = DetectionMethod::COMMENT_H;
	auto strength = DetectionStrength::LOW;

	if (record.size() < MINIMUM_GHC_RECORD_SIZE
			|| !startsWith(record, "GHC"))
	{
		return false;
	}

	const std::string version = record.substr(4);
	if (std::regex_match(
			version,
			std::regex("[[:digit:]]+.[[:digit:]]+.[[:digit:]]+")))
	{
		// Check for prior methods results
		if (isDetected("GHC"))
		{
			source = DetectionMethod::COMBINED;
			strength = DetectionStrength::HIGH;
		}

		addCompiler(source, strength, "GHC", version);
		addPriorityLanguage("Haskell");
		return true;
	}

	return false;
}

/**
 * Parse Open64 record from comment section
 * @param record Record from comment section
 * @return @c true if Open64 was detected, @c false otherwise
 */
bool Heuristics::parseOpen64Comment(const std::string &record)
{
	auto source = DetectionMethod::COMMENT_H;
	auto strength = DetectionStrength::LOW;

	const std::string prefix = "#Open64 Compiler Version ";
	const auto prefixLen = prefix.length();
	if (!startsWith(record, prefix))
	{
		return false;
	}

	const std::string separator = " : ";
	const auto separatorLen = separator.length();
	const auto pos = record.find(separator, prefixLen);
	if (pos == std::string::npos)
	{
		return false;
	}

	std::string additionalInfo;
	if (pos + separatorLen < record.length())
	{
		additionalInfo = record.substr(pos + separatorLen);
	}
	std::string version = record.substr(prefixLen, pos - prefixLen);
	addCompiler(source, strength, "Open64", version, additionalInfo);
	return true;
}

/**
 * Try to detect used compiler based on content of comment sections
 */
void Heuristics::getCommentSectionsHeuristics()
{
	for (const auto *sec : fileParser.getSections({".comment", ".rdata"}))
	{
		std::string secContent;
		if (!sec || !sec->getString(secContent))
		{
			continue;
		}

		std::vector<std::string> records;
		separateStrings(secContent, records);

		for (const auto &item : records)
		{
			parseGccComment(item)
					|| parseGhcComment(item)
					|| parseOpen64Comment(item);
		}
	}
}

/**
 * Parse GCC producer from DWARF debug information
 * @param producer DWARF record
 * @return @c true if compiler was detected, @c false otherwise
 */
bool Heuristics::parseGccProducer(const std::string &producer)
{
	auto source = DetectionMethod::DWARF_DEBUG_H;
	auto strength = DetectionStrength::MEDIUM;

	const auto cpp = startsWith(producer, "GNU C++");
	const auto c = !cpp && startsWith(producer, "GNU C");
	const auto fortran = startsWith(producer, "GNU Fortran");
	if (!c && !cpp && !fortran)
	{
		return false;
	}

	std::string version = extractVersion(producer);

	addCompiler(source, strength, "GCC", version);
	addLanguage((c ? "C" : (cpp ? "C++" : "Fortran")));
	return true;
}

/**
 * Parse clang producer from DWARF debug information
 * @param producer DWARF record
 * @return @c true if clang was detected, @c false otherwise
 */
bool Heuristics::parseClangProducer(const std::string &producer)
{
	auto source = DetectionMethod::DWARF_DEBUG_H;
	auto strength = DetectionStrength::MEDIUM;

	if (!contains(producer, "clang"))
	{
		return false;
	}

	std::string version = extractVersion(producer);
	addCompiler(source, strength, "LLVM", version);
	return true;
}

/**
 * Parse Texas Instruments producer from DWARF debug information
 * @param producer DWARF record
 * @return @c true if Texas Instruments was detected, @c false otherwise
 */
bool Heuristics::parseTmsProducer(const std::string &producer)
{
	auto source = DetectionMethod::DWARF_DEBUG_H;
	auto strength = DetectionStrength::MEDIUM;

	if (!startsWith(producer, "TMS470 C/C++"))
	{
		return false;
	}

	std::string version = extractVersion(producer);
	addCompiler(
			source,
			strength,
			"Texas Instruments C/C++",
			version,
			"for TMS470");
	return true;
}

/**
 * Try to detect compiler based on DWARF debugging information
 */
void Heuristics::getDwarfInfo()
{
	std::string lang;
	std::size_t langIndex;
	std::vector<std::string> languages;
	std::vector<std::size_t> modulesCounter;

	// Open input file as buffer.
	//
	llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffOrErr =
			llvm::MemoryBuffer::getFileOrSTDIN(fileParser.getPathToFile());
	if (buffOrErr.getError())
	{
		return;
	}
	std::unique_ptr<llvm::MemoryBuffer> bufferPtr = std::move(buffOrErr.get());
	llvm::MemoryBufferRef buffer = *bufferPtr;

	// Open buffer as a binary file.
	//
	llvm::Expected<std::unique_ptr<llvm::object::Binary>> binOrErr
			= llvm::object::createBinary(buffer);
	auto binErr = errorToErrorCode(binOrErr.takeError());
	if (binErr)
	{
		return;
	}

	// Handle different flavours of binary files.
	//
	auto* obj = llvm::dyn_cast<llvm::object::ObjectFile>(binOrErr->get());
	if (obj == nullptr)
	{
		// There might be other flavours than llvm::object::ObjectFile.
		// E.g. llvm::object::MachOUniversalBinary, llvm::object::Archive>
		// These are unhandled at the moment.
		return;
	}
	auto DICtx = llvm::DWARFContext::create(*obj);

	// Inspect compilation unit DIEs.
	//
	for (auto& unit : DICtx->compile_units())
	{
		auto unitDie = unit->getUnitDIE(true);

		if (auto p = llvm::dwarf::toString(unitDie.find(
				llvm::dwarf::DW_AT_producer)))
		{
			std::string producer = p.getValue();
			parseGccProducer(producer)
					|| parseClangProducer(producer)
					|| parseTmsProducer(producer);
		}

		if (auto l = llvm::dwarf::toUnsigned(unitDie.find(
				llvm::dwarf::DW_AT_language)))
		{
			uint64_t language = l.getValue();
			if (getDwarfLanguageString(language, lang))
			{
				if (addUniqueValue(languages, lang, langIndex))
				{
					modulesCounter.push_back(1);
				}
				else
				{
					++modulesCounter[langIndex];
				}
			}
		}
	}

	const auto noOfLanguages = modulesCounter.size();
	if (noOfLanguages == 1)
	{
		addLanguage(languages[0]);
	}
	else
	{
		for (std::size_t i = 0; i < noOfLanguages; ++i)
		{
			addLanguage(languages[i], std::to_string(modulesCounter[i])
						+ " module" + (modulesCounter[i] > 1 ? "s" : ""));
		}
	}

	return;
}

/**
 * Get Embarcadero Delphi version
 * @return Delphi version
 */
std::string Heuristics::getEmbarcaderoVersion()
{
	// Get comment section name
	auto sectionName = commentSectionNameByFormat(fileParser.getFileFormat());

	std::string content;
	const Section* section = fileParser.getSection(sectionName);
	if (section && section->getString(content, 0, 0))
	{
		// Get offset to version in compiler ID string
		auto startOffset = content.find("Embarcadero Delphi ");
		if (startOffset != std::string::npos)
		{
			// Search for platform specific string
			std::string::size_type offset = startOffset + 19;
			for (const auto pair : delphiStrings)
			{
				offset = content.find(pair.first, startOffset);
				if (offset != std::string::npos)
				{
					offset = pair.second;
					break;
				}
			}

			// Search for compiler version in xx.x format
			if (offset != std::string::npos)
			{
				auto version = content.substr(startOffset + offset, 4);
				if (std::regex_match(
						version,
						std::regex("[[:digit:]]+.[[:digit:]]")))
				{
					return version;
				}
			}
		}
	}

	return std::string();
}

/**
 * Try to detect Embarcadero Delphi compiler
 */
void Heuristics::getEmbarcaderoHeuristics()
{
	auto source = DetectionMethod::COMMENT_H;
	auto strength = DetectionStrength::MEDIUM;

	// Try to check for version in comment section
	auto version = getEmbarcaderoVersion();
	auto extra = embarcaderoVersionToExtra(version);

	// Special function often exported by Delphi XE5 and higher
	if (fileParser.getExport("TMethodImplementationIntercept"))
	{
		if (!version.empty())
		{
			// Increase detection strength
			source = DetectionMethod::COMBINED;
			strength = DetectionStrength::HIGH;
		}
		else
		{
			source = DetectionMethod::EXPORT_TABLE_H;
			strength = DetectionStrength::MEDIUM;

			version = "26.0+";
			extra = "XE5 or higher";
		}
	}

	if (!version.empty())
	{
		addCompiler(source, strength, "Embarcadero Delphi", version, extra);
		addPriorityLanguage("Delphi");
	}
}

/**
 * Try to detect compilers by specific symbol names
 */
void Heuristics::getSymbolHeuristic()
{
	auto source = DetectionMethod::SYMBOL_TABLE_H;
	auto strength = DetectionStrength::HIGH;

	std::size_t goCount = 0;
	std::size_t ghcCount = 0;
	std::size_t rustCount = 0;

	for (const SymbolTable* symbolTable : fileParser.getSymbolTables())
	{
		for (auto it = symbolTable->begin(), e = symbolTable->end(); it < e; ++it)
		{
			ghcCount += isGhcSymbol(*it) ? 1 : 0;
			goCount += isGoFunction(*it) ? 1 : 0;
			rustCount += isRustFunction(*it) ? 1 : 0;

			if (goCount > MINIMUM_GO_FUNCTIONS)
			{
				addPriorityLanguage("Go");
				return;
			}

			if (ghcCount > MINIMUM_GHC_SYMBOLS)
			{
				addCompiler(source, strength, "GHC");
				addPriorityLanguage("Haskell");
				return;
			}

			if (rustCount > MINIMUM_RUST_FUNCTIONS)
			{
				addCompiler(source, strength, "rustc");
				addPriorityLanguage("Rust");
				return;
			}
		}
	}
}

/**
 * Check if any packer is already detected
 */
static bool hasPackerDetected(const ToolInformation &toolInfo)
{
	for (const auto &tool : toolInfo.detectedTools)
	{
		if (tool.isPacker())
			return true;
	}
	return false;
}

/**
 * Compute fraction of 256-byte blocks with entropy > threshold (packed indicator).
 * Packed code: entropy > 7.2 bits/byte (near-random). Real code: 5.5–6.5.
 */
static double getHighEntropyBlockFraction(
		const std::uint8_t *data,
		std::size_t size,
		double threshold = 7.2)
{
	const std::size_t blockSize = 256;
	if (!data || size < blockSize)
		return 0.0;

	std::size_t highCount = 0;
	std::size_t blockCount = 0;
	for (std::size_t i = 0; i + blockSize <= size; i += blockSize)
	{
		double ent = computeDataEntropy(data + i, blockSize);
		if (ent >= threshold)
			++highCount;
		++blockCount;
	}
	return blockCount ? static_cast<double>(highCount) / blockCount : 0.0;
}

/**
 * Prefer entropy over executable sections only (Stage 2): packed stubs + encrypted
 * payload in .text stay high-entropy, while whole-file entropy is diluted by .data.
 * @return fraction in [0,1], or -1 if not enough executable bytes (caller uses full file).
 */
static double getHighEntropyFractionPreferCodeSections(
		FileFormat &fp,
		double threshold)
{
	const std::size_t maxScan = 4 * 1024 * 1024;
	std::vector<std::uint8_t> codeConcat;

	for (std::size_t i = 0; i < fp.getNumberOfSections(); ++i)
	{
		const Section *sec = fp.getSection(i);
		if (!sec || !sec->isCode())
		{
			continue;
		}

		const auto off = sec->getOffset();
		auto sz = sec->getSizeInFile();
		if (!sz)
		{
			continue;
		}

		std::vector<std::uint8_t> chunk;
		if (!fp.getBytes(chunk, off, sz))
		{
			continue;
		}

		if (codeConcat.size() + chunk.size() > maxScan)
		{
			const std::size_t need = maxScan - codeConcat.size();
			if (!need)
			{
				break;
			}
			codeConcat.insert(
					codeConcat.end(),
					chunk.begin(),
					chunk.begin() + static_cast<std::ptrdiff_t>(need));
			break;
		}
		codeConcat.insert(codeConcat.end(), chunk.begin(), chunk.end());
	}

	if (codeConcat.size() >= 256)
	{
		return getHighEntropyBlockFraction(codeConcat.data(), codeConcat.size(), threshold);
	}

	return -1.0;
}

/**
 * Count rep-prefixed string ops in a code buffer (memcpy/memset-style CRT codegen hint).
 * Non-overlapping: advances past each match. x64: includes REX-prefixed rep movsq/stosq.
 */
static std::size_t countRepStringOpsInBuffer(
		const std::uint8_t *data,
		std::size_t len,
		bool is64)
{
	std::size_t n = 0;
	for (std::size_t i = 0; i + 2 <= len;)
	{
		if (data[i] != 0xF3)
		{
			++i;
			continue;
		}
		// x86-64: F3 48 A5 rep movsq; F3 48 AB rep stosq
		if (is64 && i + 3 <= len && data[i + 1] == 0x48
				&& (data[i + 2] == 0xA5 || data[i + 2] == 0xAB))
		{
			++n;
			i += 3;
			continue;
		}
		// rep movsb / rep movsd (legacy; also common on x64)
		if (data[i + 1] == 0xA4 || data[i + 1] == 0xA5)
		{
			++n;
			i += 2;
			continue;
		}
		// rep stosb / rep stos(d/q) family
		if (data[i + 1] == 0xAA || data[i + 1] == 0xAB)
		{
			++n;
			i += 2;
			continue;
		}
		++i;
	}
	return n;
}

/**
 * Check if section has W+X (writable+executable) permissions — strong packer indicator.
 */
static bool isSectionWritableExecutable(const Section *sec)
{
	if (!sec)
		return false;
	// PeCoffSection
	const auto *peSec = dynamic_cast<const PeCoffSection*>(sec);
	if (peSec)
	{
		const unsigned long long flags = peSec->getPeCoffFlags();
		// IMAGE_SCN_MEM_EXECUTE=0x20000000, IMAGE_SCN_MEM_WRITE=0x80000000
		return (flags & 0xA0000000ULL) == 0xA0000000ULL;
	}
	// ElfSection
	const auto *elfSec = dynamic_cast<const ElfSection*>(sec);
	if (elfSec)
	{
		const unsigned long long flags = elfSec->getElfFlags();
		// SHF_WRITE=1, SHF_EXECINSTR=4
		return (flags & 5ULL) == 5ULL;
	}
	return false;
}

/**
 * Structural entropy packer detection (Stage 2).
 * Detects packers via entropy profile, import sparsity, entry point section, W+X.
 * No byte-sequence signatures — works for custom/modified packers.
 */
void Heuristics::getStructuralEntropyHeuristics()
{
	// Only run when no packer detected by signature
	if (hasPackerDetected(toolInfo))
		return;

	const auto &bytes = fileParser.getBytes();
	if (bytes.size() < 512)
		return;

	double highEntropyFraction = getHighEntropyFractionPreferCodeSections(
			fileParser,
			7.2);
	bool entropyUsedCodeOnly = highEntropyFraction >= 0.0;
	if (!entropyUsedCodeOnly)
	{
		highEntropyFraction = getHighEntropyBlockFraction(
				bytes.data(),
				bytes.size(),
				7.2);
	}

	// Signal 1: >80% high-entropy blocks → packed
	bool entropySignal = highEntropyFraction > 0.80;

	// Signal 2: Import / library sparsity (packed stubs: few thunks / few DLLs)
	std::size_t importCount = 0;
	std::size_t libraryCount = 0;
	std::size_t delayedImportCount = 0;
	std::size_t exportCount = 0;
	if (const auto *impTable = fileParser.getImportTable())
	{
		importCount = impTable->getNumberOfImports();
		libraryCount = impTable->getNumberOfLibraries();
		for (auto it = impTable->begin(); it != impTable->end(); ++it)
		{
			const Import *raw = it->get();
			const auto *peIm = dynamic_cast<const PeImport*>(raw);
			if (peIm && peIm->isDelayed())
			{
				++delayedImportCount;
			}
		}
	}
	if (const auto *exTable = fileParser.getExportTable())
	{
		exportCount = exTable->getNumberOfExports();
	}
	bool importSignal = importCount > 0 && importCount <= 3;
	bool fewLibrariesSignal = libraryCount >= 1 && libraryCount <= 2;

	// Signal 3: Entry point in non-code section or W+X section (flags also in structural extra)
	bool epAtypicalNameSignal = false;
	bool epInWxSection = false;
	if (toolInfo.entryPointSection)
	{
		const auto &epSec = toolInfo.epSection;
		const std::string name = toLower(epSec.getName());
		epAtypicalNameSignal = !sectionNameLooksLikeTypicalCodeSection(name);
		const Section *sec = fileParser.getSection(epSec.getIndex());
		epInWxSection = sec && isSectionWritableExecutable(sec);
	}
	const bool epSectionSignal = epAtypicalNameSignal || epInWxSection;

	// Signal 4: W+X sections — count all vs code-only (structural extra + wx signal)
	std::size_t wxExecSectionHits = 0;
	std::size_t wxAnySectionHits = 0;
	for (std::size_t i = 0; i < fileParser.getNumberOfSections(); ++i)
	{
		const auto *sec = fileParser.getSection(i);
		if (!sec || !isSectionWritableExecutable(sec))
		{
			continue;
		}
		++wxAnySectionHits;
		if (sec->isCode())
		{
			++wxExecSectionHits;
		}
	}
	const bool wxSectionSignal = wxExecSectionHits > 0;

	// Signal 5: Very few sections (packed stubs) — only with another “packed” hint
	// (avoids tiny but legitimate single-segment or minimal PE/ELF binaries).
	const std::size_t sectionCount = fileParser.getNumberOfSections();
	std::size_t executableSectionCount = 0;
	std::size_t execAtypicalNameHits = 0;
	std::size_t codeNameNonExecHits = 0;
	std::size_t execPeDataNamedHits = 0;
	const bool isPeFmt = fileParser.isPe();
	for (std::size_t si = 0; si < sectionCount; ++si)
	{
		const Section *sec = fileParser.getSection(si);
		if (!sec)
		{
			continue;
		}
		const std::string secNameLower = toLower(sec->getName());
		if (typicalCodeSectionNameButNotMarkedCode(sec->isCode(), secNameLower))
		{
			++codeNameNonExecHits;
		}
		if (peExecutableSectionNamedLikeWritableData(isPeFmt, sec->isCode(), secNameLower))
		{
			++execPeDataNamedHits;
		}
		if (sec->isCode())
		{
			++executableSectionCount;
			if (!sectionNameLooksLikeTypicalCodeSection(secNameLower))
			{
				++execAtypicalNameHits;
			}
		}
	}
	const bool fewSectionsSignal = (fileParser.isPe() || fileParser.isElf())
			&& sectionCount >= 1 && sectionCount <= 3
			&& (entropySignal || importSignal || fewLibrariesSignal);

	// Signal 6: Section names used by UPX / MPRESS / ASPack / Themida-style layouts
	bool packerSectionNameSignal = false;
	std::size_t packerSectionHits = 0;
	std::string packerSectionHint;
	for (std::size_t i = 0; i < fileParser.getNumberOfSections(); ++i)
	{
		const Section *sec = fileParser.getSection(i);
		if (!sec)
		{
			continue;
		}
		const std::string n = toLower(sec->getName());
		if (sectionNameSuggestsEmbeddedPackerSection(n))
		{
			packerSectionNameSignal = true;
			++packerSectionHits;
			if (packerSectionHint.empty() && n.length() <= 48)
			{
				packerSectionHint = sec->getName();
			}
		}
	}

	// Weighted voting: 2+ signals → MEDIUM, 3+ → HIGH
	int score = (entropySignal ? 1 : 0) + (importSignal ? 1 : 0)
			+ (fewLibrariesSignal ? 1 : 0) + (epSectionSignal ? 1 : 0)
			+ (wxSectionSignal ? 2 : 0) + (fewSectionsSignal ? 1 : 0)
			+ (packerSectionNameSignal ? 2 : 0);

	if (score >= 2)
	{
		const PeFormat *const peFmt = fileParser.isPe()
				? dynamic_cast<const PeFormat*>(&fileParser)
				: nullptr;

		auto strength = score >= 3 ? DetectionStrength::HIGH : DetectionStrength::MEDIUM;
		std::string extra = "ff_format_id="
				+ std::to_string(static_cast<int>(fileParser.getFileFormat()));
		extra += ";target_arch_id="
				+ std::to_string(static_cast<int>(fileParser.getTargetArchitecture()));
		extra += ";segment_count="
				+ std::to_string(fileParser.getNumberOfSegments());
		extra += ";string_candidate_count="
				+ std::to_string(fileParser.getStrings().size());
		extra += ";file_bytes=" + std::to_string(fileParser.getFileLength());
		extra += ";loaded_file_bytes="
				+ std::to_string(fileParser.getLoadedFileLength());
		if (fileParser.isDll())
		{
			extra += ";is_dll=1";
		}
		if (fileParser.isExecutable())
		{
			extra += ";is_executable=1";
		}
		if (fileParser.isObjectFile())
		{
			extra += ";is_object_file=1";
		}
		if (fileParser.isWindowsDriver())
		{
			extra += ";is_windows_driver=1";
		}
		if (fileParser.isSignaturePresent())
		{
			extra += ";authenticode_checked=1";
			if (fileParser.isSignatureVerified())
			{
				extra += ";authenticode_verified=1";
			}
		}
		extra += ";high_ent_blocks="
				+ std::to_string(static_cast<int>(highEntropyFraction * 100)) + "%";
		if (entropyUsedCodeOnly)
		{
			extra += ";entropy_scope=executable_sections";
		}
		extra += ";structural_vote_score=" + std::to_string(score);
		// Bitmask of which structural signals fired (for tooling / future calibration).
		// b0 entropy b1 import b2 few_libs b3 ep b4 wx b5 few_sections b6 packer_section_name
		unsigned structuralSignalBits = 0;
		if (entropySignal)
		{
			structuralSignalBits |= 1u;
		}
		if (importSignal)
		{
			structuralSignalBits |= 2u;
		}
		if (fewLibrariesSignal)
		{
			structuralSignalBits |= 4u;
		}
		if (epSectionSignal)
		{
			structuralSignalBits |= 8u;
		}
		if (wxSectionSignal)
		{
			structuralSignalBits |= 16u;
		}
		if (fewSectionsSignal)
		{
			structuralSignalBits |= 32u;
		}
		if (packerSectionNameSignal)
		{
			structuralSignalBits |= 64u;
		}
		extra += ";structural_signal_bits=" + std::to_string(structuralSignalBits);
		extra += ";section_count=" + std::to_string(sectionCount);
		extra += ";executable_sections=" + std::to_string(executableSectionCount);
		if (sectionCount > 0)
		{
			extra += ";exec_section_ratio_pct="
					+ std::to_string((executableSectionCount * 100) / sectionCount);
		}
		extra += ";import_count=" + std::to_string(importCount);
		extra += ";library_count=" + std::to_string(libraryCount);
		if (delayedImportCount > 0)
		{
			extra += ";delayed_import_count=" + std::to_string(delayedImportCount);
		}
		extra += ";export_count=" + std::to_string(exportCount);
		extra += ";exec_atypical_name_hits=" + std::to_string(execAtypicalNameHits);
		extra += ";code_name_non_exec_hits=" + std::to_string(codeNameNonExecHits);
		extra += ";exec_pe_data_named_hits=" + std::to_string(execPeDataNamedHits);
		extra += ";wx_exec_section_hits=" + std::to_string(wxExecSectionHits);
		extra += ";wx_any_section_hits=" + std::to_string(wxAnySectionHits);
		if (toolInfo.entryPointSection)
		{
			if (epAtypicalNameSignal)
			{
				extra += ";ep_section_atypical_name=1";
			}
			if (epInWxSection)
			{
				extra += ";ep_section_wx=1";
			}
		}
		if (libraryCount > 0 && importCount > 0)
		{
			extra += ";imports_per_lib_x100="
					+ std::to_string((importCount * 100) / libraryCount);
		}
		if (peFmt && peFmt->isDotNet())
		{
			extra += ";pe_dotnet=1";
		}
		if (peFmt && importCount > 0)
		{
			std::uint64_t iatRva = 0;
			std::uint64_t iatSize = 0;
			if (peFmt->getDataDirectoryRelative(
						PeLib::PELIB_IMAGE_DIRECTORY_ENTRY_IAT,
						iatRva,
						iatSize)
					&& iatSize > 0)
			{
				if (const auto fill = peIatThunkFillPercent(
							importCount,
							iatSize,
							fileParser.isX86_64()))
				{
					extra += ";iat_thunk_fill_pct=" + std::to_string(*fill);
				}
			}
		}
		if (packerSectionNameSignal && packerSectionHits > 0)
		{
			extra += ";packer_section_hits=" + std::to_string(packerSectionHits);
		}
		if (packerSectionNameSignal && !packerSectionHint.empty())
		{
			extra += ";packer_section=" + packerSectionHint;
		}
		if (const auto *impTable = fileParser.getImportTable())
		{
			const std::string &imh = impTable->getImphashCrc32();
			if (!imh.empty())
			{
				extra += ";imphash_crc32=" + imh;
			}
		}
		if (const auto *exTable = fileParser.getExportTable())
		{
			const std::string &exh = exTable->getExphashCrc32();
			if (!exh.empty())
			{
				extra += ";exphash_crc32=" + exh;
			}
		}
		if (const auto *certTab = fileParser.getCertificateTable())
		{
			if (!certTab->empty())
			{
				extra += ";authenticode_sigs="
						+ std::to_string(certTab->signatureCount());
			}
		}
		const std::size_t overlaySz = fileParser.getOverlaySize();
		if (overlaySz > 0)
		{
			extra += ";overlay_bytes=" + std::to_string(overlaySz);
			double overlayEnt = 0.0;
			if (fileParser.getOverlayEntropy(overlayEnt))
			{
				const int entX100 = static_cast<int>(overlayEnt * 100.0 + 0.5);
				extra += ";overlay_entropy_x100=" + std::to_string(entX100);
			}
		}
		if (const auto *tls = fileParser.getTlsInfo())
		{
			const auto &cb = tls->getCallBacks();
			if (!cb.empty())
			{
				extra += ";tls_callback_count=" + std::to_string(cb.size());
			}
		}
		if (const auto *resTab = fileParser.getResourceTable())
		{
			const std::size_t nRes = resTab->getNumberOfResources();
			if (nRes > 0)
			{
				extra += ";resource_count=" + std::to_string(nRes);
			}
		}
		{
			const std::size_t nRelocTabs = fileParser.getNumberOfRelocationTables();
			extra += ";reloc_table_count=" + std::to_string(nRelocTabs);
			std::size_t relocEntryTotal = 0;
			for (std::size_t ri = 0; ri < nRelocTabs; ++ri)
			{
				const auto *rt = fileParser.getRelocationTable(ri);
				if (rt)
				{
					relocEntryTotal += rt->getNumberOfRelocations();
				}
			}
			extra += ";reloc_entry_total=" + std::to_string(relocEntryTotal);
		}
		if (const auto *pdb = fileParser.getPdbInfo())
		{
			if (!pdb->getPath().empty())
			{
				extra += ";pdb_path_present=1";
			}
		}
		if (peFmt)
		{
			if (const auto *rh = fileParser.getRichHeader())
			{
				if (rh->getValidStructure() && rh->getNumberOfRecords() > 0)
				{
					extra += ";pe_rich_record_count="
							+ std::to_string(rh->getNumberOfRecords());
					appendPeRichMaxUsesToExtra(extra, rh);
				}
			}
		}
		extra += ";symbol_table_count="
				+ std::to_string(fileParser.getNumberOfSymbolTables());
		extra += ";dynamic_table_count="
				+ std::to_string(fileParser.getNumberOfDynamicTables());
		if (retdecEnvDiagEnabled("RETDEC_CPDETECT_STRUCTURAL_DIAG"))
		{
			llvm::errs() << "RETDEC_CPDETECT_STRUCTURAL_DIAG: score=" << score
					<< " strength=" << static_cast<int>(strength)
					<< " extra=" << extra << '\n';
		}
		addPacker(
				DetectionMethod::OTHER_H,
				strength,
				"Unknown packer (structural entropy)",
				"",
				extra);
	}
}

/**
 * Try to detect tools
 */
void Heuristics::getCommonToolsHeuristics()
{
	getSymbolHeuristic();
	getEmbarcaderoHeuristics();
	getSectionHeuristics();
	getDwarfInfo();
	getCommentSectionsHeuristics();
	getStructuralEntropyHeuristics();
	getCodegenFingerprintHeuristics();
}

/**
 * Codegen compiler fingerprinting (Stage 4).
 * Prologue patterns: GCC (push rbp; mov rbp,rsp), MSVC (sub rsp,N; shadow space),
 * Clang (similar to GCC, omits mov rbp,rsp more aggressively).
 * x86: GCC/Clang often 55 89 E5; MSVC often 55 8B EC (same prologue, different encoding).
 * x86-64: MSVC large frames use 48 81 EC imm32 (sub rsp, big).
 */
void Heuristics::getCodegenFingerprintHeuristics()
{
	if (!canSearch || !fileParser.isX86OrX86_64())
		return;

	// Only refine when no compiler detected with high confidence
	if (isDetected("GCC", DetectionStrength::MEDIUM)
			|| isDetected("LLVM", DetectionStrength::MEDIUM)
			|| isDetected("MSVC", DetectionStrength::MEDIUM)
			|| isDetected("Visual Studio", DetectionStrength::MEDIUM))
		return;

	const bool is64 = fileParser.isX86_64();
	const bool is32 = fileParser.isX86();

	// Scan code sections for prologue patterns
	// GCC x86-64: 55 48 89 E5 (push rbp; mov rbp, rsp)
	// MSVC x86-64: 48 83 EC ?? (sub rsp, imm8), 48 81 EC imm32, 48 89 5C 24 ?? (mov [rsp+d], rbx)
	// Clang x86-64: 55 41 57 41 56... (push rbp; push r15; push r14...) or 55 48 89 E5
	// x86: 55 89 E5 (GCC/Clang/MinGW), 55 8B EC (MSVC)
	std::size_t gccPrologueCount = 0;
	std::size_t msvcPrologueCount = 0;
	std::size_t clangPrologueCount = 0;
	std::size_t repStringOpsTotal = 0;
	std::size_t nearJmpRel32Total = 0;
	std::size_t codegenCodeSectionsScanned = 0;

	const std::size_t scanBytes = 8192;  // Scan first 8KB of code
	for (std::size_t i = 0; i < noOfSections; ++i)
	{
		const auto *sec = sections[i];
		if (!sec || !sec->isCode())
			continue;

		const auto offset = sec->getOffset();
		const auto size = sec->getSizeInFile();
		if (size < 4)
			continue;

		++codegenCodeSectionsScanned;

		const auto scanSize = std::min<uint64_t>(size, scanBytes);

		std::vector<std::uint8_t> codeChunk;
		if (fileParser.getBytes(codeChunk, offset, scanSize) && !codeChunk.empty())
		{
			repStringOpsTotal += countRepStringOpsInBuffer(
					codeChunk.data(),
					codeChunk.size(),
					is64);
			// Weak tail-call / control-flow proxy: x86 near jmp rel32 (0xE9)
			for (std::size_t b = 0; b + 5 <= codeChunk.size(); ++b)
			{
				if (codeChunk[b] == static_cast<std::uint8_t>(0xE9))
				{
					++nearJmpRel32Total;
				}
			}
		}

		if (is64)
		{
			// GCC-style frame
			if (search.exactComparison("554889E5", offset) != 0)
				++gccPrologueCount;
			// MSVC stack probe / shadow space / large alloca
			if (search.exactComparison("4883EC??", offset) != 0
					|| search.exactComparison("48895C24??", offset) != 0
					|| search.exactComparison("4881EC????????", offset) != 0)
				++msvcPrologueCount;
			// Clang push chain after rbp
			if (search.exactComparison("5541574156", offset) != 0)
				++clangPrologueCount;

			// Scan at 16-byte alignment (typical function start alignment)
			for (std::size_t off = 16; off + 7 <= scanSize; off += 16)
			{
				if (search.exactComparison("554889E5", offset + off) != 0)
					++gccPrologueCount;
				if (search.exactComparison("4883EC??", offset + off) != 0
						|| search.exactComparison("48895C24??", offset + off) != 0
						|| search.exactComparison("4881EC????????", offset + off) != 0)
					++msvcPrologueCount;
				if (search.exactComparison("5541574156", offset + off) != 0)
					++clangPrologueCount;
			}
		}

		if (is32)
		{
			// push ebp; mov ebp,esp — AT&T-style encoding (GCC, Clang, ICC often)
			if (search.exactComparison("5589E5", offset) != 0)
				++gccPrologueCount;
			// push ebp; mov ebp,esp — MSVC’s usual encoding
			if (search.exactComparison("558BEC", offset) != 0)
				++msvcPrologueCount;

			for (std::size_t off = 16; off + 3 <= scanSize; off += 16)
			{
				if (search.exactComparison("5589E5", offset + off) != 0)
					++gccPrologueCount;
				if (search.exactComparison("558BEC", offset + off) != 0)
					++msvcPrologueCount;
			}
		}
	}

	std::string extra = is64 ? "prologue pattern (x86_64)"
			: (is32 ? "prologue pattern (x86)" : "prologue pattern");
	extra += ";codegen_scan_bytes_per_section=" + std::to_string(scanBytes);
	extra += ";codegen_code_sections_scanned="
			+ std::to_string(codegenCodeSectionsScanned);
	if (repStringOpsTotal > 0)
	{
		extra += ";rep_string_ops=" + std::to_string(repStringOpsTotal);
	}
	const std::size_t prologueHitsTotal = gccPrologueCount + msvcPrologueCount
			+ clangPrologueCount;
	if (prologueHitsTotal > 0)
	{
		extra += ";prologue_hits=" + std::to_string(prologueHitsTotal);
	}
	// P1.4 slice: per-family counts (tooling / correlation; same data as RETDEC_CPDETECT_CODEGEN_DIAG)
	if (gccPrologueCount > 0)
	{
		extra += ";codegen_prologue_gcc_hits=" + std::to_string(gccPrologueCount);
	}
	if (msvcPrologueCount > 0)
	{
		extra += ";codegen_prologue_msvc_hits=" + std::to_string(msvcPrologueCount);
	}
	if (clangPrologueCount > 0)
	{
		extra += ";codegen_prologue_clang_hits=" + std::to_string(clangPrologueCount);
	}
	if (nearJmpRel32Total > 0)
	{
		extra += ";near_jmp_rel32_hits=" + std::to_string(nearJmpRel32Total);
	}

	if (fileParser.isPe())
	{
		if (const auto *rh = fileParser.getRichHeader())
		{
			if (rh->getValidStructure())
			{
				extra += ";pe_rich_header=present";
				extra += ";pe_rich_records=" + std::to_string(rh->getNumberOfRecords());
				if (rh->getSuspicious())
				{
					extra += ";pe_rich_suspicious=1";
				}
				const std::string richCrc = rh->getCrc32();
				if (!richCrc.empty())
				{
					extra += ";pe_rich_crc32=" + richCrc;
				}
				// Capped MSVC toolchain / compiland id list (productId:productBuild per record)
				constexpr std::size_t MAX_RICH_PID_BUILDS = 6;
				std::string pidBuilds;
				const std::size_t n = rh->getNumberOfRecords();
				for (std::size_t i = 0; i < n && i < MAX_RICH_PID_BUILDS; ++i)
				{
					const auto *rec = rh->getRecord(i);
					if (rec == nullptr)
					{
						break;
					}
					if (!pidBuilds.empty())
					{
						pidBuilds += ',';
					}
					pidBuilds += std::to_string(rec->getProductId());
					pidBuilds += ':';
					pidBuilds += std::to_string(rec->getProductBuild());
				}
				if (!pidBuilds.empty())
				{
					extra += ";pe_rich_pid_builds=" + pidBuilds;
				}
				appendPeRichMaxUsesToExtra(extra, rh);
				for (std::size_t ri = 0; ri < n; ++ri)
				{
					const auto *rec = rh->getRecord(ri);
					if (rec == nullptr)
					{
						break;
					}
					std::string vsHint = trim(rec->getVisualStudioName());
					if (vsHint.empty())
					{
						continue;
					}
					for (char &ch : vsHint)
					{
						if (ch == ';' || ch == '=' || ch == '\n' || ch == '\r')
						{
							ch = '_';
						}
					}
					constexpr std::size_t MAX_VS_HINT = 48;
					if (vsHint.size() > MAX_VS_HINT)
					{
						vsHint.resize(MAX_VS_HINT);
					}
					extra += ";pe_rich_vs_hint=" + vsHint;
					break;
				}
			}
		}
	}

	if (retdecEnvDiagEnabled("RETDEC_CPDETECT_CODEGEN_DIAG"))
	{
		llvm::errs() << "RETDEC_CPDETECT_CODEGEN_DIAG: gcc_prologue_hits="
				<< gccPrologueCount << " msvc_prologue_hits=" << msvcPrologueCount
				<< " clang_prologue_hits=" << clangPrologueCount
				<< " rep_string_ops=" << repStringOpsTotal
				<< " near_jmp_rel32_hits=" << nearJmpRel32Total
				<< " extra=" << extra << '\n';
	}

	// Pick dominant fingerprint
	if (gccPrologueCount > msvcPrologueCount && gccPrologueCount > clangPrologueCount
			&& gccPrologueCount >= 2)
	{
		addCompiler(DetectionMethod::OTHER_H, DetectionStrength::LOW,
				"GCC (codegen fingerprint)", "", extra);
	}
	else if (msvcPrologueCount > gccPrologueCount && msvcPrologueCount > clangPrologueCount
			&& msvcPrologueCount >= 2)
	{
		addCompiler(DetectionMethod::OTHER_H, DetectionStrength::LOW,
				"MSVC (codegen fingerprint)", "", extra);
	}
	else if (clangPrologueCount > gccPrologueCount && clangPrologueCount > msvcPrologueCount
			&& clangPrologueCount >= 2)
	{
		addCompiler(DetectionMethod::OTHER_H, DetectionStrength::LOW,
				"LLVM/Clang (codegen fingerprint)", "", extra);
	}
}

/**
 * Try to detect original language
 */
void Heuristics::getCommonLanguageHeuristics()
{
}

/**
 * Check if compiler is already detected
 * @param name Name of compiler
 * @param minStrength Minimal strength of used method
 * @return pointer to detection if compiler is detected, @b nullptr otherwise
 */
const DetectResult* Heuristics::isDetected(
		const std::string &name, const DetectionStrength minStrength)
{
	for (const auto &detection : toolInfo.detectedTools)
	{
		if (startsWith(detection.name, name)
				&& detection.strength >= minStrength)
		{
			return &detection;
		}
	}

	return nullptr;
}

/**
 * Try detect version of UPX packer
 * @return Detected version of UPX or empty string is version is not detected
 */
std::string Heuristics::getUpxVersion()
{
	if (fileParser.isElf() || fileParser.isMacho())
	{
		// format: $Id: UPX x.xx
		const std::string pattern = "$Id: UPX ";
		const auto &content = search.getPlainString();
		const auto pos = content.find(pattern);
		const std::size_t versionLen = 4;
		if (pos <= content.length() - pattern.length() - versionLen)
		{
			return content.substr(pos + pattern.length(), versionLen);
		}
	}

	return "";
}

/**
 * Get all compiler heuristics which are specific for one file format
 */
void Heuristics::getFormatSpecificCompilerHeuristics()
{
}

/**
 * Get all language heuristics which are specific for one file format
 */
void Heuristics::getFormatSpecificLanguageHeuristics()
{
}

/**
 * Try detect compiler based on all available heuristics
 */
void Heuristics::getAllHeuristics()
{
	// Detect languages
	getCommonLanguageHeuristics();
	getFormatSpecificLanguageHeuristics();

	// Detect compilers
	getCommonToolsHeuristics();
	getFormatSpecificCompilerHeuristics();
}

} // namespace cpdetect
} // namespace retdec
