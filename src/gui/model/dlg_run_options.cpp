/**
 * @file src/gui/model/dlg_run_options.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/dlg_run_options.h"

#include "retdec/gui/model/dlg_input.h"

#include <filesystem>
#include <format>
#include <system_error>

namespace retdec::gui::model {

const std::vector<std::string>& cStyleCallInfoItems()
{
	static const std::vector<std::string> k{"omit", "optim", "pessim"};
	return k;
}

std::string cStyleCallInfoFromText(std::string_view t)
{
	return (t == "optim" || t == "pessim") ? std::string(t) : std::string();
}

int cStyleCallInfoIndex(std::string_view stored)
{
	if (stored == "optim") return 1;
	if (stored == "pessim") return 2;
	return 0;
}

const std::vector<std::string>& rawEndianItems()
{
	static const std::vector<std::string> k{"omit", "little", "big"};
	return k;
}

const std::vector<std::string>& rawBitSizeItems()
{
	static const std::vector<std::string> k{"omit", "16", "32", "64"};
	return k;
}

RawArchiveOptions rawArchiveOptionsFromForm(const RawArchiveForm& f)
{
	RawArchiveOptions o;
	o.endian = (f.endianText == "little" || f.endianText == "big") ? f.endianText : std::string();
	const int n = dlgParseInt(f.bitSizeText).value_or(0);
	o.bitSize = (n == 16 || n == 32 || n == 64) ? n : 0;
	o.rawSectionVma = dlgParseUnsignedAuto(f.vmaText).value_or(0);
	o.arName = std::string(dlgTrim(f.arNameText));
	const std::string_view idx = dlgTrim(f.arIndexText);
	if (!idx.empty())
	{
		if (const auto v = dlgParseInt(idx); v && *v >= 0) o.arIndex = *v;
	}
	return o;
}

RawArchiveForm rawArchiveFormFromOptions(const RawArchiveOptions& o)
{
	RawArchiveForm f;
	f.endianText = o.endian.empty() ? "omit" : o.endian;
	f.bitSizeText = o.bitSize > 0 ? std::to_string(o.bitSize) : "omit";
	f.vmaText = o.rawSectionVma > 0 ? std::format("0x{:x}", o.rawSectionVma) : std::string();
	f.arNameText = o.arName;
	f.arIndexText = (o.arIndex && *o.arIndex >= 0) ? std::to_string(*o.arIndex) : std::string();
	return f;
}

void applyCStyleOptions(const CStyleOptions& o, DecompilerLaunchRequest& req)
{
	req.keepAllBrackets = o.keepAllBrackets;
	req.noTimeVaryingInfo = o.noTimeVaryingInfo;
	req.noVarRenaming = o.noVarRenaming;
	req.noCompoundOperators = o.noCompoundOperators;
	req.noSymbolicNames = o.noSymbolicNames;
	req.backendNoOpts = o.backendNoOpts;
	req.callInfoObtainer = cStyleCallInfoFromText(o.callInfoObtainer);
	req.backendDisabledOpts = std::string(dlgTrim(o.backendDisabledOpts));
	req.backendEnabledOpts = std::string(dlgTrim(o.backendEnabledOpts));
}

void applyRawArchiveOptions(const RawArchiveOptions& o, bool rawMode, DecompilerLaunchRequest& req)
{
	req.rawMode = rawMode;
	req.endian = o.endian;
	req.bitSize = o.bitSize;
	req.rawSectionVma = o.rawSectionVma;
	req.arName = o.arName;
	req.arIndex = o.arIndex.value_or(-1);
}

void applyNextRunOptions(const NextRunOptions& o, DecompilerLaunchRequest& req)
{
	applyCStyleOptions(o.cStyle, req);
	applyRawArchiveOptions(o.raw, o.rawMode, req);
	req.maxMemoryBytes = o.maxMemoryBytes;
	req.noMemoryLimit = o.noMemoryLimit;
	req.pdbPath = o.pdbPath;
	req.staticCodeSigFile = o.staticCodeSigFile;
	req.varRenamer = o.varRenamer;
	req.printBeforeAll = o.printBeforeAll;
	req.keepUnreachableFuncs = o.keepUnreachableFuncs;
	req.cleanup = o.cleanup;
	req.tryEmulation = o.tryEmulation;
	req.keepLibraryFuncs = o.keepLibraryFuncs;
}

const std::vector<std::string>& runStageItems()
{
	static const std::vector<std::string> k{
		"Decompile (retdec-decompiler)",
		"Fast decompile",
		"fileinfo — binary format report",
		"unpacker — try packed-binary recovery",
		"Signature Studio",
	};
	return k;
}

std::optional<RunStageChoice> runStageFromIndex(int index)
{
	switch (index)
	{
	case 0: return RunStageChoice::Decompile;
	case 1: return RunStageChoice::FastDecompile;
	case 2: return RunStageChoice::Fileinfo;
	case 3: return RunStageChoice::Unpacker;
	case 4: return RunStageChoice::SignatureStudio;
	default: return std::nullopt;
	}
}

std::optional<RunStageChoice> runStageFromText(std::string_view item)
{
	const auto& items = runStageItems();
	for (std::size_t i = 0; i < items.size(); ++i)
		if (items[i] == item) return runStageFromIndex(int(i));
	return std::nullopt;
}

std::string pdbPathStatus(std::string_view path)
{
	return std::format("Next decompile will pass --pdb {}", path);
}

std::string staticSigFileStatus(std::string_view path)
{
	return std::format("Next decompile will pass --static-code-sigfile {}", path);
}

std::string fileDialogStartDir(std::string_view lastOpenDir)
{
	if (lastOpenDir.empty()) return {};
	std::error_code ec;
	return std::filesystem::is_directory(std::filesystem::path(std::string(lastOpenDir)), ec) ? std::string(lastOpenDir)
																							  : std::string();
}

} // namespace retdec::gui::model
