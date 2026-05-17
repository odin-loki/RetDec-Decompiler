/**
* @file src/bin2llvmir/optimizations/decoder/decoder.cpp
* @brief Various decoder initializations.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <memory>
#include "retdec/bin2llvmir/optimizations/decoder/decoder.h"
#include "retdec/bin2llvmir/optimizations/decoder/decoder_linking_section_names.h"
#include "retdec/utils/io/log.h"
#include "retdec/utils/string.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>

#include "retdec/fileformat/types/import_table/pe_import.h"
#include "retdec/fileformat/types/sec_seg/elf_section.h"
#include "retdec/fileformat/types/sec_seg/pe_coff_section.h"
#include "retdec/loader/loader/elf/elf_image.h"
#include "retdec/pelib/PeLibAux.h"

#include <llvm/DebugInfo/DWARF/DWARFDataExtractor.h>
#include <llvm/DebugInfo/DWARF/DWARFDebugFrame.h>
#include <llvm/Support/Error.h>
#include <llvm/ADT/Triple.h>

#include <elfio/elf_types.hpp>

using namespace llvm;
using namespace retdec::capstone2llvmir;
using namespace retdec::common;
using namespace retdec::fileformat;
using namespace retdec::utils;

namespace retdec {
namespace bin2llvmir {

namespace
{

bool decoderTlsDiagEnabled()
{
	const char *e = std::getenv("RETDEC_DECODER_TLS_DIAG");
	return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
}

bool decoderImportDiagEnabled()
{
	const char *e = std::getenv("RETDEC_DECODER_IMPORT_DIAG");
	return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
}

bool decoderExportDiagEnabled()
{
	const char *e = std::getenv("RETDEC_DECODER_EXPORT_DIAG");
	return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
}

bool decoderEntryDiagEnabled()
{
	const char *e = std::getenv("RETDEC_DECODER_ENTRY_DIAG");
	return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
}

bool decoderConfigFncDiagEnabled()
{
	const char *e = std::getenv("RETDEC_DECODER_CONFIG_FNC_DIAG");
	return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
}

bool decoderExternDiagEnabled()
{
	const char *e = std::getenv("RETDEC_DECODER_EXTERN_DIAG");
	return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
}

/// PE delay-import directory entries are flagged on @c PeImport; annotate decoder logs.
std::string importDelayLoadLogSuffix(const fileformat::Import *imp)
{
	const auto *pe = dynamic_cast<const PeImport *>(imp);
	if (pe && pe->isDelayed())
	{
		return " [delay-load]";
	}
	return {};
}

} // namespace

/**
 * Initialize capstone2llvmir translator according to the architecture of
 * file to decompile.
 */
void Decoder::initTranslator()
{
	auto& a = _config->getConfig().architecture;

	cs_arch arch = CS_ARCH_ALL;
	cs_mode basicMode = CS_MODE_LITTLE_ENDIAN;
	cs_mode extraMode = a.isEndianBig()
			? CS_MODE_BIG_ENDIAN
			: CS_MODE_LITTLE_ENDIAN;

	if (a.isX86())
	{
		arch = CS_ARCH_X86;
		switch (a.getBitSize())
		{
			case 16: basicMode = CS_MODE_16; break;
			case 64: basicMode = CS_MODE_64; break;
			default:
			case 32: basicMode = CS_MODE_32; break;
		}
	}
	else if (a.isMipsOrPic32())
	{
		arch = CS_ARCH_MIPS;
		switch (a.getBitSize())
		{
			case 64: basicMode = CS_MODE_MIPS64; break;
			default:
			case 32: basicMode = CS_MODE_MIPS32; break;
		}
	}
	else if (a.isPpc())
	{
		arch = CS_ARCH_PPC;
		switch (a.getBitSize())
		{
			case 64: basicMode = CS_MODE_64; break;
			default:
			case 32: basicMode = CS_MODE_32; break;
		}
	}
	else if (a.isArm32OrThumb()
			&& a.getBitSize() == 32)
	{
		arch = CS_ARCH_ARM;
		basicMode = CS_MODE_ARM; // We start with ARM mode even for THUMB.
	}
	else if (a.isArm64())
	{
		arch = CS_ARCH_ARM64;
		basicMode = CS_MODE_ARM;
	}
	else
	{
		throw std::runtime_error("Unsupported architecture.");
	}

	_c2l = Capstone2LlvmIrTranslator::createArch(
			arch,
			_module,
			basicMode,
			extraMode);
}

/**
 * Initialize instruction used in dry run disassembly.
 */
void Decoder::initDryRunCsInstruction()
{
	csh ce = _c2l->getCapstoneEngine();
	_dryCsInsn = cs_malloc(ce);
}

/**
 * Synchronize metadata between capstone2llvmir and bin2llvmir.
 */
void Decoder::initEnvironment()
{
	initEnvironmentAsm2LlvmMapping();
	initEnvironmentPseudoFunctions();
	initEnvironmentRegisters();
}

/**
 * Find out from capstone2llvmir which global is used for
 * LLVM IR <-> Capstone ASM mapping.
 * 1. Set its name.
 * 2. Set it to config.
 * 3. Create metadata for it, so it can be quickly recognized without querying
 *    config.
 */
void Decoder::initEnvironmentAsm2LlvmMapping()
{
	auto* a2lGv = _c2l->getAsm2LlvmMapGlobalVariable();
	a2lGv->setName(names::asm2llvmGv);

	AsmInstruction::setLlvmToAsmGlobalVariable(_module, a2lGv);
}

/**
 * Set pseudo functions' names in LLVM IR and set them to config.
 */
void Decoder::initEnvironmentPseudoFunctions()
{
	auto* cf = _c2l->getCallFunction();
	cf->setName(names::pseudoCallFunction);
	_config->setLlvmCallPseudoFunction(cf);

	auto* rf = _c2l->getReturnFunction();
	rf->setName(names::pseudoReturnFunction);
	_config->setLlvmReturnPseudoFunction(rf);

	auto* bf = _c2l->getBranchFunction();
	bf->setName(names::pseudoBranchFunction);
	_config->setLlvmBranchPseudoFunction(bf);

	auto* cbf = _c2l->getCondBranchFunction();
	cbf->setName(names::pseudoCondBranchFunction);
	_config->setLlvmCondBranchPseudoFunction(cbf);

	if (auto* c2lX86 = dynamic_cast<Capstone2LlvmIrTranslatorX86*>(_c2l.get()))
	{
		auto* dl = c2lX86->getX87DataLoadFunction();
		dl->setName(names::pseudoX87dataLoadFunction);
		_config->setLlvmX87DataLoadPseudoFunction(dl);

		auto* ds = c2lX86->getX87DataStoreFunction();
		ds->setName(names::pseudoX87dataStoreFunction);
		_config->setLlvmX87DataStorePseudoFunction(ds);
	}
}

/**
 * Create config objects for HW registers.
 * Initialize ABI with registers.
 */
void Decoder::initEnvironmentRegisters()
{
	for (GlobalVariable& gv : _module->globals())
	{
		if (_c2l->isRegister(&gv))
		{
			unsigned regNum = _c2l->getCapstoneRegister(&gv);
			auto s = common::Storage::inRegister(gv.getName(), regNum);

			common::Object cr(gv.getName(), s);
			cr.type.setLlvmIr(llvmObjToString(gv.getValueType()));
			cr.setRealName(gv.getName());
			_config->getConfig().registers.insert(cr);

			_abi->addRegister(regNum, &gv);
		}
	}
}

/**
 * Find address ranges to decode.
 */
void Decoder::initRanges()
{
	JumpTarget::config = _config;
	JumpTargets::config = _config;

	auto& arch = _config->getConfig().architecture;
	unsigned a = 0;
	a = arch.isArm32OrThumb() ? 2 : a;
	a = arch.isArm64() ? 4 : a;
	a = arch.isMipsOrPic32() ? 4 : a;
	a = arch.isPpc() ? 4 : a;
	_ranges.setArchitectureInstructionAlignment(a);

	if (_config->getConfig().parameters.isSelectedDecodeOnly())
	{
		initAllowedRangesWithConfig();
	}
	else
	{
		initAllowedRangesWithSegments();
	}

	_ranges.removeZeroSequences(_image);

	if (_ranges.primaryEmpty())
	{
		_ranges.promoteAlternativeToPrimary();
	}
}

namespace {

/// Stage 6: Multi-evidence code vs data — section has executable flag (PE/ELF)
bool sectionHasExecutableFlag(const fileformat::SecSeg* sec)
{
	if (auto* pe = dynamic_cast<const fileformat::PeCoffSection*>(sec))
		return (pe->getPeCoffFlags() & PeLib::PELIB_IMAGE_SCN_MEM_EXECUTE) != 0;
	if (auto* elf = dynamic_cast<const fileformat::ElfSection*>(sec))
		return (elf->getElfFlags() & 0x4) != 0;  // SHF_EXECINSTR
	return false;
}

/// Stage 6: UNDEFINED section name suggests code
bool sectionNameSuggestsCode(const std::string& name)
{
	return name == ".text" || name == ".init" || name == ".fini"
			|| sectionNameIsPltLike(name);
}

} // namespace

/**
 * Initialize address ranges to decode from image segments/sections.
 */
void Decoder::initAllowedRangesWithSegments()
{
	LOG << "\n" << "initAllowedRangesWithSegments():" << std::endl;

	auto* epSeg = _image->getImage()->getEpSegment();
	for (auto& seg : _image->getSegments())
	{
		auto* sec = seg->getSecSeg();
		Address start = seg->getAddress();
		Address end = seg->getPhysicalEndAddress();

		LOG << "\t" << seg->getName() << " @ " << start << " -- "
				<< end << std::endl;

		if (start == end)
		{
			LOG << "\t\t" << "size == 0 -> skipped" << std::endl;
			continue;
		}

		if (seg.get() != epSeg && sec)
		{
			if (auto* s = dynamic_cast<const PeCoffSection*>(sec))
			{
				if (s->getPeCoffFlags() & PeLib::PELIB_IMAGE_SCN_MEM_DISCARDABLE)
				{
					LOG << "\t\t" << "PeLib::PELIB_IMAGE_SCN_MEM_DISCARDABLE"
							" -> skipped" << std::endl;
					continue;
				}
			}
		}

		if (sec)
		{
			switch (sec->getType())
			{
				case SecSeg::Type::CODE:
					LOG << "\t\t" << "code -> allowed ranges"
							<< std::endl;
					if (sectionNameIsPltLike(sec->getName())
							|| segmentNameIsGlobalOffsetTableLike(
									sec->getName())
							|| segmentNameIsPeImportDataLike(
									sec->getName())) // often code; GOT/IAT slots are data
					{
						_ranges.addAlternative(start, end);
					}
					else
					{
						_ranges.addPrimary(start, end);
					}
					break;
				case SecSeg::Type::DATA:
					LOG << "\t\t" << "data -> alternative ranges"
							<< std::endl;
					_ranges.addAlternative(start, end);
					break;
				case SecSeg::Type::CODE_DATA:
				{
					// PLT/GOT/.idata may be typed CODE_DATA + exec but hold pointers/thunks.
					const bool linkingSpecial = sectionNameIsPltLike(sec->getName())
							|| segmentNameIsGlobalOffsetTableLike(sec->getName())
							|| segmentNameIsPeImportDataLike(sec->getName());
					if (sectionHasExecutableFlag(sec))
					{
						if (linkingSpecial)
						{
							LOG << "\t\t" << "code/data + exec + PLT/GOT/idata -> "
									"alternative ranges" << std::endl;
							_ranges.addAlternative(start, end);
						}
						else
						{
							LOG << "\t\t" << "code/data + exec flag -> primary ranges"
									<< std::endl;
							_ranges.addPrimary(start, end);
						}
					}
					else
					{
						LOG << "\t\t" << "code/data -> alternative ranges"
								<< std::endl;
						_ranges.addAlternative(start, end);
					}
					break;
				}
				case SecSeg::Type::CONST_DATA:
					if (seg.get() == epSeg)
					{
						LOG << "\t\t" << "const data == ep seg "
								"-> alternative ranges" << std::endl;
						_ranges.addAlternative(start, end);
					}
					else
					{
						LOG << "\t\t" << "const data -> alternative ranges"
								<< std::endl;
						continue;
					}
					break;
				case SecSeg::Type::UNDEFINED_SEC_SEG:
				{
					const bool linkingSpecial = sectionNameIsPltLike(sec->getName())
							|| segmentNameIsGlobalOffsetTableLike(sec->getName())
							|| segmentNameIsPeImportDataLike(sec->getName());
					const bool codeEvidence = sectionHasExecutableFlag(sec)
							|| sectionNameSuggestsCode(sec->getName());
					if (codeEvidence)
					{
						if (linkingSpecial)
						{
							LOG << "\t\t" << "undef + code evidence + PLT/GOT/idata -> "
									"alternative ranges" << std::endl;
							_ranges.addAlternative(start, end);
						}
						else
						{
							LOG << "\t\t" << "undef + code evidence -> primary ranges"
									<< std::endl;
							_ranges.addPrimary(start, end);
						}
					}
					else
					{
						LOG << "\t\t" << "undef -> alternative ranges"
								<< std::endl;
						_ranges.addAlternative(start, end);
					}
					break;
				}
				case SecSeg::Type::BSS:
					LOG << "\t\t" << "bss -> skipped" << std::endl;
					continue;
				case SecSeg::Type::DEBUG:
					LOG << "\t\t" << "debug -> skipped" << std::endl;
					continue;
				case SecSeg::Type::INFO:
					LOG << "\t\t" << "info -> skipped" << std::endl;
					continue;
				default:
					assert(false && "unhandled section type");
					continue;
			}
		}
		else if (seg.get() == epSeg)
		{
			LOG << "\t\t" << "no underlying section or segment && ep seg "
					"-> alternative ranges" << std::endl;
			_ranges.addAlternative(start, end);
		}
		else
		{
			LOG << "\t\t" << "no underlying section or segment -> skipped"
					<< std::endl;
			continue;
		}
	}

	for (auto& seg : _image->getSegments())
	{
		auto& rc = seg->getNonDecodableAddressRanges();
		for (auto& r : rc)
		{
			if (!r.contains(_config->getConfig().parameters.getEntryPoint()))
			{
				_ranges.remove(r.getStart(), r.getEnd());
			}
		}
	}
}

void Decoder::initAllowedRangesWithConfig()
{
	LOG << "\n" << "initAllowedRangesWithConfig():" << std::endl;

	std::set<std::string> foundFs;

	for (auto &p : _config->getConfig().parameters.selectedRanges)
	{
		_ranges.addPrimary(p);
		LOG << "\t" << "[+] selected range @ " << p << std::endl;

		if (auto* jt = _jumpTargets.push(
				p.getStart(),
				JumpTarget::eType::SELECTED_RANGE_START,
				_c2l->getBasicMode(),
				Address::Undefined))
		{
			createFunction(jt->getAddress());
			LOG << "\t" << "[+] " << p.getStart() << std::endl;
		}
		else
		{
			LOG << "\t" << "[-] " << p.getStart() << " (no JT)" << std::endl;
		}
	}

	auto& selectedFs = _config->getConfig().parameters.selectedFunctions;

	if (!selectedFs.empty())
	{
		for (auto& dfp : _debug->functions)
		{
			auto& df = dfp.second;
			auto fIt = selectedFs.find(df.getName());
			if (fIt == selectedFs.end())
			{
				fIt = selectedFs.find(df.getDemangledName());
			}

			if (fIt == selectedFs.end())
			{
				continue;
			}

			Address start = df.getStart();
			Address end = df.getEnd();

			_ranges.addPrimary(start, end);
			LOG << "\t" << "[+] selected range from debug @ "
					<< AddressRange(start, end) << std::endl;

			std::optional<std::size_t> sz;
			auto tmpSz = dfp.second.getSize();
			if (tmpSz.isDefined() && tmpSz > 0)
			{
				sz = tmpSz.getValue();
			}

			if (auto* jt = _jumpTargets.push(
					start,
					JumpTarget::eType::SELECTED_RANGE_START,
					df.isThumb() ? CS_MODE_THUMB : _c2l->getBasicMode(),
					Address::Undefined,
					sz))
			{
				foundFs.insert(*fIt);
				auto* nf = createFunction(jt->getAddress());
				addFunctionSize(nf, sz, FunctionSizePriority::DEBUG);
				LOG << "\t" << "[+] " << start << std::endl;
			}
			else
			{
				LOG << "\t" << "[-] " << start << " (no JT)" << std::endl;
			}
		}

		std::map<
				retdec::common::Address,
				std::shared_ptr<const retdec::fileformat::Symbol>> symtab;

		for (const auto* t : _image->getFileFormat()->getSymbolTables())
		for (const auto& s : *t)
		{
			unsigned long long a = 0;
			if (!s->getRealAddress(a))
			{
				continue;
			}

			auto fIt = symtab.find(a);
			if (fIt == symtab.end())
			{
				symtab.emplace(a, s);
			}
			else
			{
				if (selectedFs.count(fIt->second->getName())
						|| selectedFs.count(fIt->second->getNormalizedName())
						|| selectedFs.count(
								removeLeadingCharacter(
										fIt->second->getName(), '_'))
						|| selectedFs.count(
								removeLeadingCharacter(
										fIt->second->getNormalizedName(), '_')))
				{
					// name in map is the name we are searching for.
				}
				else
				{
					symtab[a] = s;
				}
			}
		}

		for (auto sIt = symtab.begin(); sIt != symtab.end(); ++sIt)
		{
			auto& s = sIt->second;

			retdec::common::Address start = sIt->first;
			if (start.isUndefined())
			{
				continue;
			}

			std::optional<std::size_t> knownSz;
			unsigned long long size = 0;
			if (!s->getSize(size))
			{
				++sIt;
				if (sIt == symtab.end())
				{
					--sIt;
					continue;
				}
				size = sIt->first - start;
				--sIt;
			}
			else if (size > 0)
			{
				knownSz = size;
			}

			retdec::common::Address end = start + size;
			std::string name = s->getNormalizedName();

			// Exact name match.
			auto fIt = selectedFs.find(name);

			// Without leading '_' name match.
			if (fIt == selectedFs.end())
			{
				auto tmp1 = removeLeadingCharacter(name, '_');
				for (fIt = selectedFs.begin(); fIt != selectedFs.end(); ++fIt)
				{
					std::string tmp2 = removeLeadingCharacter(*fIt, '_');

					if (tmp1 == tmp2)
						break;
				}
			}

			if (fIt != selectedFs.end() && foundFs.find(*fIt) == foundFs.end())
			{
				_ranges.addPrimary(start, end);
				LOG << "\t" << "[+] selected range from symbol: "
						<< start << std::endl;

				if (auto* jt = _jumpTargets.push(
						start,
						JumpTarget::eType::SELECTED_RANGE_START,
						s->isThumbSymbol() ? CS_MODE_THUMB :_c2l->getBasicMode(),
						Address::Undefined,
						knownSz))
				{
					foundFs.insert(*fIt);
					auto* nf = createFunction(jt->getAddress());
					addFunctionSize(nf, knownSz, FunctionSizePriority::SYMBOL);
					LOG << "\t" << "[+] " << start << std::endl;
				}
				else
				{
					LOG << "\t" << "[-] " << start << " (no JT)" << std::endl;
				}
			}
		}
	}

	// Find out which selected functions have not been found.
	//
	auto &sbnf = _config->getConfig().parameters.selectedNotFoundFunctions;
	std::set_difference(
			selectedFs.begin(), selectedFs.end(),
			foundFs.begin(), foundFs.end(),
			std::inserter(sbnf, sbnf.end())
	);

	auto* plt = _image->getImage()->getSegment(".plt");
	if (!_ranges.primaryEmpty() && plt)
	{
		_ranges.addPrimary(plt->getAddress(), plt->getPhysicalEndAddress());
	}
}

/**
 * Find jump targets to decode.
 */
void Decoder::initJumpTargets()
{
	initJumpTargetsConfig();
	if (_config->getConfig().parameters.isDetectStaticCode())
	{
		initStaticCode();
	}
	initJumpTargetsEntryPoint();
	initJumpTargetsTls();  // Stage 5: TLS callbacks as synthetic entry points
	initJumpTargetsExterns();
	initJumpTargetsImports();
	initJumpTargetsDebug();
	initJumpTargetsPdata();    // Stage 15: PE .pdata exception directory
	initJumpTargetsEhFrame(); // Stage 15: ELF .eh_frame exception handling
	initJumpTargetsSymbols(); // MUST be before exports
	initJumpTargetsExports();
	initVtables();
}

void Decoder::initJumpTargetsConfig()
{
	const bool cfgDiag = decoderConfigFncDiagEnabled();
	LOG << "\n" << "initJumpTargetsConfig():" << std::endl;

	const auto &fnList = _config->getConfig().functions;
	if (cfgDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_CONFIG_FNC_DIAG: config_functions_total="
				<< fnList.size() << "\n";
	}

	std::size_t skipUndefinedStart = 0;
	std::size_t pushed = 0;
	std::size_t pushFailed = 0;

	for (auto& f : fnList)
	{
		if (f.getStart().isUndefined())
		{
			++skipUndefinedStart;
			continue;
		}

		auto tmpSz = f.getSize();
		auto sz = tmpSz.isDefined() && tmpSz > 0
				? std::optional<std::size_t>(tmpSz)
				: std::nullopt;

		if (auto* jt = _jumpTargets.push(
				f.getStart(),
				JumpTarget::eType::CONFIG,
				f.isThumb() ? CS_MODE_THUMB : _c2l->getBasicMode(),
				Address::Undefined,
				sz))
		{
			auto* nf = createFunction(jt->getAddress());
			addFunctionSize(nf, jt->getSize(), FunctionSizePriority::CONFIG);
			++pushed;

			LOG << "\t" << "[+] " << f.getStart() << " @ "
					<< nf->getName().str() << std::endl;
		}
		else
		{
			++pushFailed;
			LOG << "\t" << "[-] " << f.getStart() << " (no JT)" << std::endl;
		}
	}

	if (cfgDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_CONFIG_FNC_DIAG: with_defined_start="
				<< (pushed + pushFailed) << " jump_targets_pushed=" << pushed
				<< " skip_undefined_start=" << skipUndefinedStart
				<< " jump_target_push_failed=" << pushFailed << "\n";
	}
}

void Decoder::initJumpTargetsEntryPoint()
{
	const bool entryDiag = decoderEntryDiagEnabled();
	LOG << "\n" << "initJumpTargetsEntryPoint():" << std::endl;

	auto ep = _config->getConfig().parameters.getEntryPoint();
	if (entryDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_ENTRY_DIAG: config_entry_point=" << ep
				<< " is_defined=" << (!ep.isUndefined()) << "\n";
		auto mainA = _config->getConfig().parameters.getMainAddress();
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_ENTRY_DIAG: config_main_address=" << mainA
				<< " is_defined=" << (!mainA.isUndefined()) << "\n";
	}
	if (auto* jt = _jumpTargets.push(
			ep,
			JumpTarget::eType::ENTRY_POINT,
			_c2l->getBasicMode(),
			Address::Undefined))
	{
		_entryPointFunction = createFunction(jt->getAddress());

		// TODO: bugs.bin2llvmir-branch-analysis-segfault.Test
		// _image->getImage()->hasDataOnAddress(a) == false -> created fnc is emtpy
		// But we still can get and decode some data.
		if (_entryPointFunction && _entryPointFunction->empty())
		{
			createBasicBlock(jt->getAddress(), _entryPointFunction);
		}

		LOG << "\t" << "[+] " << ep << " @ "
				<< _entryPointFunction->getName().str() << std::endl;
	}
	else
	{
		LOG << "\t" << "[-] " << ep << " (no JT)" << std::endl;
	}
}

/**
 * Add TLS callbacks as synthetic entry points (Stage 5).
 * TLS callbacks run before main; add them to the decompilation queue.
 */
void Decoder::initJumpTargetsTls()
{
	const bool tlsDiag = decoderTlsDiagEnabled();
	auto* ff = _image->getFileFormat();
	if (!ff)
	{
		if (tlsDiag)
		{
			retdec::utils::io::Log::info()
					<< "RETDEC_DECODER_TLS_DIAG: no file format\n";
		}
		return;
	}

	const auto* tlsInfo = ff->getTlsInfo();
	if (tlsDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_TLS_DIAG: tls_info="
				<< (tlsInfo ? "present" : "absent") << "\n";
	}
	if (!tlsInfo)
	{
		return;
	}

	const auto& callbacks = tlsInfo->getCallBacks();
	if (tlsDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_TLS_DIAG: callback_count="
				<< callbacks.size() << "\n";
	}
	if (callbacks.empty())
	{
		return;
	}

	// PE stores TLS callback addresses as RVAs; add image base
	std::uint64_t base = 0;
	if (ff->isPe())
	{
		base = _image->getImage()->getBaseAddress();
	}

	if (tlsDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_TLS_DIAG: pe_image_base=0x" << std::hex << base
				<< std::dec << "\n";
		std::size_t shown = 0;
		for (auto addrVal : callbacks)
		{
			if (shown++ >= 64)
			{
				break;
			}
			retdec::utils::io::Log::info()
					<< "RETDEC_DECODER_TLS_DIAG:   rva=0x" << std::hex << addrVal
					<< " va=0x" << (addrVal + base) << std::dec << "\n";
		}
	}

	LOG << "\n" << "initJumpTargetsTls():" << std::endl;

	std::size_t idx = 0;
	for (auto addrVal : callbacks)
	{
		Address a(addrVal + base);
		if (a.isUndefined())
			continue;

		std::string name = "__tls_callback_" + std::to_string(idx++);
		if (auto* jt = _jumpTargets.push(
				a,
				JumpTarget::eType::ENTRY_POINT,
				_c2l->getBasicMode(),
				Address::Undefined))
		{
			auto* f = createFunction(jt->getAddress());
			if (f)
				f->setName(name);
			LOG << "\t" << "[+] " << a << " @ " << name << std::endl;
		}
		else
		{
			LOG << "\t" << "[-] " << a << " @ " << name << " (no JT)" << std::endl;
		}
	}
}

void Decoder::initJumpTargetsExterns()
{
	const bool extDiag = decoderExternDiagEnabled();
	// This section applies only for elf files
	if (auto* elf_image = dynamic_cast<retdec::loader::ElfImage *>(_image->getImage()))
	{

		LOG << "\n" << "initJumpTargetsExterns():" << std::endl;

		const auto &extTab = elf_image->getExternFncTable();
		if (extDiag)
		{
			retdec::utils::io::Log::info()
					<< "RETDEC_DECODER_EXTERN_DIAG: elf_extern_function_table_entries="
					<< extTab.size() << "\n";
		}

		std::size_t skipUndefined = 0;
		std::size_t pushed = 0;
		std::size_t pushFailed = 0;

		for (const auto& ext : extTab)
		{
			Address a = ext.second;
			if (a.isUndefined())
			{
				++skipUndefined;
				continue;
			}

			if (auto* jt = _jumpTargets.push(
					a,
					JumpTarget::eType::IMPORT,
					_c2l->getBasicMode(),
					Address::Undefined))
			{
				auto* f = createFunction(jt->getAddress(), true);

				// We should not alter symbols tables, so we set the name here
				f->setName(ext.first);

				_externs.emplace(ext.first);
				++pushed;

				LOG << "\t" << "[+] " << a << " @ " << f->getName().str()
						<< std::endl;
			}
			else
			{
				++pushFailed;
				LOG << "\t" << "[-] " << a << " @ " << ext.first
						<< " (no JT)" << std::endl;
			}
		}

		if (extDiag)
		{
			retdec::utils::io::Log::info()
					<< "RETDEC_DECODER_EXTERN_DIAG: jump_targets_pushed=" << pushed
					<< " skip_undefined_address=" << skipUndefined
					<< " jump_target_push_failed=" << pushFailed << "\n";
		}
	}
	else if (extDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_EXTERN_DIAG: not an ELF image, extern table skipped\n";
	}
}

void Decoder::initJumpTargetsImports()
{
	const bool impDiag = decoderImportDiagEnabled();
	LOG << "\n" << "initJumpTargetsImports():" << std::endl;

	auto* impTbl = _image->getFileFormat()->getImportTable();
	if (impTbl == nullptr)
	{
		if (impDiag)
		{
			retdec::utils::io::Log::info()
					<< "RETDEC_DECODER_IMPORT_DIAG: no import table\n";
		}
		LOG << "\t" << "no import table -> skip" << std::endl;
		return;
	}

	std::size_t delayLoadImportMarkers = 0;
	for (const auto &imp : *impTbl)
	{
		const auto *pe = dynamic_cast<const PeImport *>(imp.get());
		if (pe != nullptr && pe->isDelayed())
		{
			++delayLoadImportMarkers;
		}
	}

	if (impDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_IMPORT_DIAG: imports=" << impTbl->getNumberOfImports()
				<< " libraries=" << impTbl->getNumberOfLibraries()
				<< " delay_load_marked_imports=" << delayLoadImportMarkers << "\n";
	}

	// Non-pointer imports are preferred.
	// We should solve this somehow better.
	//
	std::set<std::string> usedNames;
	std::set<const fileformat::Import*> ptrs;

	for (const auto &imp : *impTbl)
	{
		common::Address a = imp->getAddress();
		if (a.isUndefined())
		{
			continue;
		}

		if (_externs.count(imp->getName()))
		{
			continue;
		}

		auto* ciVal = _image->getConstantDefault(a);
		auto* sec = _image->getImage()->getSegmentFromAddress(a);

		bool isPtr = false;
		isPtr |= _image->getFileFormat()->isPointer(a);
		isPtr |= ciVal && ciVal->isZero();
		isPtr |= sec && segmentNameIsGlobalOffsetTableLike(sec->getName());
		isPtr |= sec && segmentNameIsPeImportDataLike(sec->getName());
		if (isPtr)
		{
			ptrs.insert(imp.get());
			continue;
		}

		bool declaration = _config->getConfig().fileFormat.isPe()
				|| _config->getConfig().fileFormat.isCoff();
		if (declaration)
		{
			auto* f = createFunction(a, true);
			_imports.emplace(a);
			if (_image->isImportTerminating(impTbl, imp.get()))
			{
				_terminatingFncs.insert(f);
			}
			usedNames.insert(imp->getName());

			_ranges.remove(a, a+_config->getConfig().architecture.getByteSize());

			continue;
		}

		if (auto* jt = _jumpTargets.push(
				a,
				JumpTarget::eType::IMPORT,
				_c2l->getBasicMode(),
				Address::Undefined))
		{
			auto* f = createFunction(jt->getAddress());
			_imports.emplace(jt->getAddress());
			if (_image->isImportTerminating(impTbl, imp.get()))
			{
				_terminatingFncs.insert(f);
			}
			usedNames.insert(imp->getName());

			LOG << "\t" << "[+] " << a << " @ " << f->getName().str()
					<< importDelayLoadLogSuffix(imp.get()) << std::endl;
		}
		else
		{
			LOG << "\t" << "[-] " << a << " @ " << imp->getName()
					<< importDelayLoadLogSuffix(imp.get()) << " (no JT)"
					<< std::endl;
		}
	}

	for (const auto* imp : ptrs)
	{
		Address a = imp->getAddress();

		if (usedNames.count(imp->getName()))
		{
			LOG << "\t" << "[-] " << a << " @ " << imp->getName()
					<< " (already used name)" << std::endl;
			continue;
		}

		if (auto* jt = _jumpTargets.push(
				a,
				JumpTarget::eType::IMPORT,
				_c2l->getBasicMode(),
				Address::Undefined))
		{
			auto* f = createFunction(jt->getAddress());
			_imports.emplace(jt->getAddress());
			if (_image->isImportTerminating(impTbl, imp))
			{
				_terminatingFncs.insert(f);
			}

			LOG << "\t" << "[+] " << a << " @ " << f->getName().str()
					<< importDelayLoadLogSuffix(imp) << std::endl;
		}
		else
		{
			LOG << "\t" << "[-] " << a << " @ " << imp->getName()
					<< importDelayLoadLogSuffix(imp) << " (no JT)"
					<< std::endl;
		}
	}

	if (impDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_IMPORT_DIAG: deferred_pointer_thunk_pass="
				<< ptrs.size() << " (GOT/.idata-style slots pass 2)\n";
	}
}

void Decoder::initJumpTargetsExports()
{
	// TODO: These might be THUMBs without us knowing - no odd address.
	// e.g.
	// features.macho-archives.TestExtractArchiveDecompilation (archive --ar-index 3)
	// run dry run and determine arm/thumb.
	// TODO: also in that sample, it looks like THUMB symbols are -1 not +1.

	const bool exDiag = decoderExportDiagEnabled();
	LOG << "\n" << "initJumpTargetsExports():" << std::endl;

	auto* ff = _image->getFileFormat();
	if (ff == nullptr)
	{
		if (exDiag)
		{
			retdec::utils::io::Log::info()
					<< "RETDEC_DECODER_EXPORT_DIAG: no file format\n";
		}
		LOG << "\t" << "no file format -> skip" << std::endl;
		return;
	}

	auto* exTbl = ff->getExportTable();
	if (exTbl == nullptr)
	{
		if (exDiag)
		{
			retdec::utils::io::Log::info()
					<< "RETDEC_DECODER_EXPORT_DIAG: no export table\n";
		}
		LOG << "\t" << "no export table -> skip" << std::endl;
		return;
	}

	if (exDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_EXPORT_DIAG: export_table_entries="
				<< exTbl->getNumberOfExports() << "\n";
	}

	std::size_t pushed = 0;
	std::size_t skipUndefined = 0;
	std::size_t skipElfNoSymbol = 0;
	std::size_t pushFailed = 0;

	for (const auto& exp : *exTbl)
	{
		common::Address addr = exp.getAddress();
		if (addr.isUndefined())
		{
			++skipUndefined;
			continue;
		}
		// On ELF, there is no export table. It was reconstructed from
		// symbols. Exports do not have to be functions, they can be
		// data objects. Skip those exports that were not added to symbols.
		//
		if (_config->getConfig().fileFormat.isElf()
				&& _symbols.count(addr) == 0)
		{
			++skipElfNoSymbol;
			LOG << "\t" << "[-] " << addr << " (no symbol)" << std::endl;
			continue;
		}

		if (auto* jt = _jumpTargets.push(
				addr,
				JumpTarget::eType::EXPORT,
				_c2l->getBasicMode(),
				Address::Undefined))
		{
			auto* nf = createFunction(jt->getAddress());
			_exports.emplace(jt->getAddress());
			++pushed;

			LOG << "\t" << "[+] " << addr << " @ " << nf->getName().str()
					<< std::endl;
		}
		else
		{
			++pushFailed;
			LOG << "\t" << "[-] " << addr << " (no JT)" << std::endl;
		}
	}

	if (exDiag)
	{
		retdec::utils::io::Log::info()
				<< "RETDEC_DECODER_EXPORT_DIAG: jump_targets_pushed=" << pushed
				<< " skip_undefined_address=" << skipUndefined
				<< " skip_elf_no_symbol=" << skipElfNoSymbol
				<< " jump_target_push_failed=" << pushFailed << "\n";
	}
}

void Decoder::initJumpTargetsSymbols()
{
	LOG << "\n" << "initJumpTargetsSymbols():" << std::endl;

	for (const auto* t : _image->getFileFormat()->getSymbolTables())
	for (const auto& s : *t)
	{
		if (!s->isFunction())
		{
			continue;
		}
		unsigned long long a = 0;
		if (!s->getRealAddress(a))
		{
			continue;
		}
		common::Address addr = a;

		std::optional<std::size_t> sz;
		unsigned long long tmpSz = 0;
		if (s->getSize(tmpSz) && tmpSz > 0)
		{
			sz = tmpSz;
		}

		if (auto* jt = _jumpTargets.push(
				addr,
				JumpTarget::eType::SYMBOL,
				s->isThumbSymbol() ? CS_MODE_THUMB :_c2l->getBasicMode(),
				Address::Undefined,
				sz))
		{
			auto* nf = createFunction(jt->getAddress());
			_symbols.insert(jt->getAddress());
			addFunctionSize(nf, sz, FunctionSizePriority::SYMBOL);

			LOG << "\t" << "[+] " << addr << " @ " << nf->getName().str()
					 << " (" << s->getName() << ")" << std::endl;
		}
		else
		{
			LOG << "\t" << "[-] " << addr << " (no JT)" << std::endl;
		}
	}
}

/**
 * Stage 15: Parse PE .pdata exception directory for function boundaries.
 * RUNTIME_FUNCTION entries provide BeginAddress/EndAddress (RVAs) for each function.
 */
void Decoder::initJumpTargetsPdata()
{
	LOG << "\n" << "initJumpTargetsPdata():" << std::endl;

	auto* ff = _image->getFileFormat();
	if (ff == nullptr || ff->getFileFormat() != Format::PE)
	{
		LOG << "\t" << "not PE -> skip" << std::endl;
		return;
	}

	auto& arch = _config->getConfig().architecture;
	if (!arch.isX86() || arch.getBitSize() != 64)
	{
		if (!arch.isArm64())
		{
			LOG << "\t" << "not x64/ARM64 -> skip" << std::endl;
			return;
		}
	}

	const auto* pdata = ff->getSection(".pdata");
	if (pdata == nullptr)
	{
		LOG << "\t" << "no .pdata section -> skip" << std::endl;
		return;
	}

	std::uint64_t base = _image->getImage()->getBaseAddress();
	std::size_t entrySize = 12;  // RUNTIME_FUNCTION: BeginAddress, EndAddress, UnwindInfoAddress
	std::size_t nEntries = pdata->getSizeInFile() / entrySize;
	if (nEntries == 0)
	{
		LOG << "\t" << ".pdata empty -> skip" << std::endl;
		return;
	}

	for (std::size_t i = 0; i < nEntries; ++i)
	{
		std::uint32_t beginRva = pdata->getBytesAtOffsetAsNumber<std::uint32_t>(i * entrySize + 0);
		std::uint32_t endRva   = pdata->getBytesAtOffsetAsNumber<std::uint32_t>(i * entrySize + 4);
		if (endRva <= beginRva)
			continue;
		std::size_t sz = endRva - beginRva;
		Address addr(base + beginRva);

		if (auto* jt = _jumpTargets.push(
				addr,
				JumpTarget::eType::PDATA,
				_c2l->getBasicMode(),
				Address::Undefined,
				sz))
		{
			auto* nf = createFunction(jt->getAddress());
			addFunctionSize(nf, sz, FunctionSizePriority::PDATA);
			LOG << "\t" << "[+] " << addr << " sz=" << sz << std::endl;
		}
	}
}

/**
 * Stage 15: Parse ELF .eh_frame for function boundaries.
 * FDE entries provide initial_location and address_range for each function.
 */
void Decoder::initJumpTargetsEhFrame()
{
	LOG << "\n" << "initJumpTargetsEhFrame():" << std::endl;

	auto* ff = _image->getFileFormat();
	if (ff == nullptr || ff->getFileFormat() != Format::ELF)
	{
		LOG << "\t" << "not ELF -> skip" << std::endl;
		return;
	}

	const auto* ehSec = ff->getSection(".eh_frame");
	if (ehSec == nullptr)
	{
		LOG << "\t" << "no .eh_frame section -> skip" << std::endl;
		return;
	}

	llvm::StringRef bytes = ehSec->getBytes();
	if (bytes.empty())
	{
		LOG << "\t" << ".eh_frame empty -> skip" << std::endl;
		return;
	}

	std::uint64_t ehFrameAddr = ehSec->getAddress();
	bool isLittleEndian = !_config->getConfig().architecture.isEndianBig();

	llvm::DWARFDataExtractor data(
			bytes,
			isLittleEndian,
			_config->getConfig().architecture.getByteSize());

	llvm::Triple::ArchType arch = llvm::Triple::UnknownArch;
	auto& a = _config->getConfig().architecture;
	if (a.isX86() && a.getBitSize() == 64)
		arch = llvm::Triple::x86_64;
	else if (a.isX86() && a.getBitSize() == 32)
		arch = llvm::Triple::x86;
	else if (a.isArm64())
		arch = llvm::Triple::aarch64;
	else if (a.isArm32OrThumb())
		arch = llvm::Triple::arm;
	else if (a.isMipsOrPic32() && a.getBitSize() == 64)
		arch = llvm::Triple::mips64;
	else if (a.isMipsOrPic32())
		arch = llvm::Triple::mips;
	else if (a.isPpc() && a.getBitSize() == 64)
		arch = llvm::Triple::ppc64;
	else if (a.isPpc())
		arch = llvm::Triple::ppc;

	if (arch == llvm::Triple::UnknownArch)
	{
		LOG << "\t" << "unsupported arch for .eh_frame -> skip" << std::endl;
		return;
	}

	llvm::DWARFDebugFrame ehFrame(arch, true, ehFrameAddr);
	try
	{
		ehFrame.parse(data);
	}
	catch (...)
	{
		LOG << "\t" << "failed to parse .eh_frame -> skip" << std::endl;
		return;
	}

	for (const llvm::dwarf::FrameEntry& entry : ehFrame.entries())
	{
		const auto* fde = llvm::dyn_cast<llvm::dwarf::FDE>(&entry);
		if (!fde)
			continue;

		std::uint64_t start = fde->getInitialLocation();
		std::uint64_t range = fde->getAddressRange();
		if (range == 0)
			continue;

		Address addr(start);
		std::size_t sz = range;

		if (auto* jt = _jumpTargets.push(
				addr,
				JumpTarget::eType::EHFRAME,
				_c2l->getBasicMode(),
				Address::Undefined,
				sz))
		{
			auto* nf = createFunction(jt->getAddress());
			addFunctionSize(nf, sz, FunctionSizePriority::PDATA);
			LOG << "\t" << "[+] " << addr << " sz=" << sz << std::endl;
		}
	}
}

void Decoder::initJumpTargetsDebug()
{
	LOG << "\n" << "initJumpTargetsDebug():" << std::endl;

	if (_debug == nullptr)
	{
		LOG << "\t" << "no debug info -> skip" << std::endl;
		return;
	}

	for (const auto& p : _debug->functions)
	{
		common::Address addr = p.first;
		if (addr.isUndefined())
		{
			continue;
		}
		auto& f = p.second;

		std::optional<std::size_t> sz;
		auto tmpSz = p.second.getSize();
		if (tmpSz.isDefined() && tmpSz > 0)
		{
			sz = tmpSz.getValue();
		}

		if (auto* jt = _jumpTargets.push(
				addr,
				JumpTarget::eType::DEBUG,
				f.isThumb() ? CS_MODE_THUMB : _c2l->getBasicMode(),
				Address::Undefined,
				sz))
		{
			auto* nf = createFunction(jt->getAddress());
			_debugFncs.emplace(jt->getAddress(), &f);
			addFunctionSize(nf, sz, FunctionSizePriority::DEBUG);

			LOG << "\t" << "[+] " << addr << " @ "
					<< nf->getName().str() << std::endl;
		}
		else
		{
			LOG << "\t" << "[-] " << addr << " (no JT)" << std::endl;
		}
	}
}

void Decoder::initStaticCode()
{
	LOG << "\n" << "initStaticCode():" << std::endl;

	stacofin::Finder SCA;
	SCA.searchAndConfirm(*_image->getImage(), _config->getConfig());

	for (auto& p : SCA.getConfirmedDetections())
	{
		auto* sf = p.second;

		for (auto& n : sf->names)
		{
			if (sf->getAddress().isDefined() && !n.empty())
			{
				_names->addNameForAddress(sf->getAddress(), n, Name::eType::STATIC_CODE);
			}
		}

		for (auto& r : sf->references)
		{
			if (r.address.isDefined() && !r.name.empty())
			{
				_names->addNameForAddress(r.target, r.name, Name::eType::STATIC_CODE);
			}
		}

		if (auto* jt = _jumpTargets.push(
				sf->getAddress(),
				JumpTarget::eType::STATIC_CODE,
				sf->isThumb() ? CS_MODE_THUMB : _c2l->getBasicMode(),
				Address::Undefined,
				sf->size))
		{
			auto* nf = createFunction(jt->getAddress());
			_staticFncs.insert(jt->getAddress());
			if (sf->isTerminating())
			{
				_terminatingFncs.insert(nf);
			}

			// Unreliable - sometimes there are nops, alignment, or other patterns.
			//addFunctionSize(f, sf->size);

			// Speed-up decoding, but we will not be able to diff CFG json
			// with IDA CFG.
			//_ranges.remove(f->address, f->address + f->size);

			LOG << "\t" << "[+] " << sf->getAddress() << " @ "
					<< nf->getName().str() << std::endl;
		}
		else
		{
			LOG << "\t" << "[-] " << sf->getAddress() << " (no JT)" << std::endl;
		}
	}
}

void Decoder::initVtables()
{
	LOG << "\n" << "initVtables():" << std::endl;

	std::vector<const common::Vtable*> vtable;
	for (auto& p : _image->getRtti().getVtablesGcc())
	{
		vtable.push_back(&p.second);
	}
	for (auto& p : _image->getRtti().getVtablesMsvc())
	{
		vtable.push_back(&p.second);
	}

	for (auto* p : vtable)
	{
		auto& vt = *p;
		for (auto& item : vt.items)
		{
			if (auto* jt = _jumpTargets.push(
					item.getTargetFunctionAddress(),
					JumpTarget::eType::VTABLE,
					item.isThumb() ? CS_MODE_THUMB : _c2l->getBasicMode(),
					Address::Undefined))
			{
				auto* nf = createFunction(jt->getAddress());
				_vtableFncs.insert(jt->getAddress());

				LOG << "\t" << "[+] " << item.getTargetFunctionAddress()
						<< " @ " << nf->getName().str() << std::endl;
			}
			else
			{
				LOG << "\t" << "[-] " << item.getTargetFunctionAddress()
						<< " (no JT)" << std::endl;
			}
		}
	}
}

void Decoder::initConfigFunctions()
{
	for (auto& p : _fnc2addr)
	{
		llvm::Function* f = p.first;

		if (_config->getConfigFunction(p.second)) // functions from IDA
		{
			continue;
		}

		Address start = p.second;
		Address end = getFunctionEndAddress(f);
		end = end > start ? end : Address(start + 1);

		// TODO: this is really bad, should be solved by better design of config
		// updates
		common::Function* cf = const_cast<common::Function*>(
				_config->insertFunction(f, start, end));

		if (_imports.count(start))
		{
			cf->setIsDynamicallyLinked();
			f->deleteBody();
		}
		else if (_staticFncs.count(start))
		{
			cf->setIsStaticallyLinked();
			// Can not delete body here because of main detection.
			//f->deleteBody();
		}

		std::string realName = _names->getPreferredNameForAddress(start);
		if (cf->getName() != realName)
		{
			cf->setRealName(realName);
		}

		cf->setIsExported(_exports.count(start));

		auto dbgIt = _debugFncs.find(start);
		if (dbgIt != _debugFncs.end())
		{
			auto* df = dbgIt->second;
			cf->setIsFromDebug(true);
			cf->setStartLine(df->getStartLine());
			cf->setEndLine(df->getEndLine());
			cf->setSourceFileName(df->getSourceFileName());
		}
	}

	for (auto* f : _c2l->getPseudoAsmFunctions())
	{
		_config->addPseudoAsmFunction(f);
	}
}

} // namespace bin2llvmir
} // namespace retdec
