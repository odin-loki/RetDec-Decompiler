/**
 * @file include/retdec/gui/model/dlg_run_options.h
 * @brief The per-run decompiler options the Analysis menu's small dialogs
 *        collect (C output style, raw image / archive options, max memory,
 *        PDB, static-code signature file, Run Stage) and how they map onto a
 *        DecompilerLaunchRequest.
 *
 * In the Qt GUI these lived as QObject properties of the main window; here
 * they are one plain struct the main window keeps and applies to every
 * interactive launch request.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DLG_RUN_OPTIONS_H
#define RETDEC_GUI_MODEL_DLG_RUN_OPTIONS_H

#include "retdec/gui/model/decompiler_launch.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

// ─── C output style ──────────────────────────────────────────────────────────

/// Analysis → C output style… (backend C flags for the next run).
struct CStyleOptions
{
	bool keepAllBrackets = false;     ///< --backend-keep-all-brackets
	bool noTimeVaryingInfo = false;   ///< --backend-no-time-varying-info
	bool noVarRenaming = false;       ///< --backend-no-var-renaming
	bool noCompoundOperators = false; ///< --backend-no-compound-operators
	bool noSymbolicNames = false;     ///< --backend-no-symbolic-names
	bool backendNoOpts = false;       ///< --backend-no-opts
	std::string callInfoObtainer;     ///< "", "optim" or "pessim"
	std::string backendDisabledOpts;  ///< comma-separated, trimmed
	std::string backendEnabledOpts;   ///< comma-separated, trimmed

	bool operator==(const CStyleOptions&) const = default;
};

/// The call-info combo's items: "omit", "optim", "pessim".
const std::vector<std::string>& cStyleCallInfoItems();
/// Combo text -> stored value ("optim" / "pessim", anything else "").
std::string cStyleCallInfoFromText(std::string_view comboText);
/// Stored value -> combo index (0 = omit).
int cStyleCallInfoIndex(std::string_view stored);

// ─── Raw image / archive ─────────────────────────────────────────────────────

/// Analysis → Raw image / archive options… (and the "-m raw" toggle).
struct RawArchiveOptions
{
	std::string endian;          ///< "", "little" or "big" → -e
	int bitSize = 0;             ///< 0, 16, 32, 64 → -b
	uint64_t rawSectionVma = 0;  ///< 0 omits --raw-section-vma
	std::string arName;          ///< --ar-name (empty omits)
	std::optional<int> arIndex;  ///< --ar-index (ignored when arName is set)

	bool operator==(const RawArchiveOptions&) const = default;
};

/// Endian combo items ("omit", "little", "big") and bit-size items ("omit", "16", "32", "64").
const std::vector<std::string>& rawEndianItems();
const std::vector<std::string>& rawBitSizeItems();

/// What the dialog's fields hold, as text.
struct RawArchiveForm
{
	std::string endianText = "omit";
	std::string bitSizeText = "omit";
	std::string vmaText;
	std::string arNameText;
	std::string arIndexText;
};

/// Form -> options exactly as the Qt dialog read it: unknown endian / bit
/// size become "omitted", the VMA parses with base auto-detection (0x… hex,
/// 0… octal, decimal; unparsable = 0), the archive name is trimmed, the index
/// is set only for an int >= 0.
RawArchiveOptions rawArchiveOptionsFromForm(const RawArchiveForm& form);
/// Options -> the text the dialog shows when reopened.
RawArchiveForm rawArchiveFormFromOptions(const RawArchiveOptions& opts);

// ─── Everything "for the next run" ───────────────────────────────────────────

/// The Analysis menu's per-run state (Qt main-window properties).
struct NextRunOptions
{
	CStyleOptions cStyle;
	RawArchiveOptions raw;
	bool rawMode = false;            ///< "Treat input as raw image (-m raw)"
	uint64_t maxMemoryBytes = 0;     ///< Set max memory…
	bool noMemoryLimit = false;
	std::string pdbPath;             ///< Set PDB symbols…
	std::string staticCodeSigFile;   ///< Set static-code signature file… / Signature Studio
	std::string varRenamer;          ///< Variable renamer submenu ("" = readable)
	bool printBeforeAll = false;
	bool keepUnreachableFuncs = false;
	bool cleanup = false;
	bool tryEmulation = false;
	bool keepLibraryFuncs = false;

	bool operator==(const NextRunOptions&) const = default;
};

/// Copy the per-run options into @p req (fields not covered are left alone).
void applyCStyleOptions(const CStyleOptions& o, DecompilerLaunchRequest& req);
void applyRawArchiveOptions(const RawArchiveOptions& o, bool rawMode, DecompilerLaunchRequest& req);
void applyNextRunOptions(const NextRunOptions& o, DecompilerLaunchRequest& req);

// ─── Run Stage ───────────────────────────────────────────────────────────────

enum class RunStageChoice
{
	Decompile,     ///< "Decompile (retdec-decompiler)"
	FastDecompile, ///< "Fast decompile"
	Fileinfo,      ///< "fileinfo — binary format report"
	Unpacker,      ///< "unpacker — try packed-binary recovery"
	SignatureStudio,
};

/// The Run Stage list, in order.
const std::vector<std::string>& runStageItems();
/// Item text -> choice (nothing for an unknown item).
std::optional<RunStageChoice> runStageFromText(std::string_view item);
std::optional<RunStageChoice> runStageFromIndex(int index);

// ─── File prompts ────────────────────────────────────────────────────────────

/// Glob filters of the Analysis menu's file prompts (FuseUI fileDialog
/// syntax: ';'-separated, empty = all files).
inline constexpr std::string_view kPdbFileFilter = "*.pdb";
inline constexpr std::string_view kSignatureFileFilter = "*.yar;*.yara;*.sig";
inline constexpr std::string_view kExecutableFileFilter = "*.exe;*.dll;*.elf;*.so;*.dylib;*.bin";
inline constexpr std::string_view kJsonFileFilter = "*.json";

/// Status-bar lines after the PDB / signature prompts.
std::string pdbPathStatus(std::string_view path);
std::string staticSigFileStatus(std::string_view path);

/// The directory a file prompt starts in: @p lastOpenDir when it exists, else "".
std::string fileDialogStartDir(std::string_view lastOpenDir);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DLG_RUN_OPTIONS_H
