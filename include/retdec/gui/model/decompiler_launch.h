/**
 * @file include/retdec/gui/model/decompiler_launch.h
 * @brief Build retdec-decompiler CLI arguments and read its log while it runs.
 *
 * The GUI must launch the decompiler with the same flags as a manual CLI
 * invocation. Child stdout/stderr go to a log file (not a pipe, see
 * DecompileRun) so a slow console can never block the decompiler on a full
 * pipe buffer; the helpers here read that file incrementally.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DECOMPILER_LAUNCH_H
#define RETDEC_GUI_MODEL_DECOMPILER_LAUNCH_H

#include "retdec/gui/model/settings.h"
#include "retdec/gui/model/types.h"

#include <forge/json/json.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

struct DecompilerLaunchRequest
{
	std::string binaryPath;
	std::string outputPath;
	std::string arch;
	bool fastDecompile = false;
	bool printAfterAll = false;
	/// Adds `--backend-emit-cfg` (Settings → Advanced → dump CFG).
	bool emitCfg = false;
	/// Adds `--disable-static-code-detection` when not already in fast mode.
	bool disableStaticCodeDetection = false;
	/// Default true keeps `-s` (silent). Verbose/Debug interactive runs set false.
	bool silent = true;
	/// Adds `--select-decode-only` (faster partial re-decompile).
	bool selectDecodeOnly = false;
	/// Adds `--print-before-all` (very verbose).
	bool printBeforeAll = false;
	/// Adds `-k` / `--keep-unreachable-funcs`.
	bool keepUnreachableFuncs = false;
	/// Adds `--backend-emit-cg` (call-graph DOT next to CFG dumps).
	bool emitCg = false;
	/// Non-empty → adds `--select-functions` (comma-separated LLVM/config names).
	std::vector<std::string> selectedFunctions;
	/// Non-empty → adds `--select-ranges` (comma-separated `0xSTART-0xEND`).
	std::vector<std::string> selectedRanges;
	/// Non-zero → `--raw-entry-point` (Target panel override).
	uint64_t entryPoint = 0;
	/// Existing file → `-p` / `--pdb`.
	std::string pdbPath;
	/// Non-empty and not `readable` → `--backend-var-renamer`.
	std::string varRenamer;
	/// Existing file → `--static-code-sigfile`.
	std::string staticCodeSigFile;
	/// Adds `--cleanup` (delete intermediates after the run).
	bool cleanup = false;
	/// Adds `--try-emulation` (experimental unpacking). Default off.
	bool tryEmulation = false;
	/// Non-zero → `--max-memory` (bytes). Zero omits the flag.
	uint64_t maxMemoryBytes = 0;
	/// Adds `--backend-keep-library-funcs`.
	bool keepLibraryFuncs = false;
	/// Adds `--backend-keep-all-brackets`.
	bool keepAllBrackets = false;
	/// Adds `--backend-no-time-varying-info`.
	bool noTimeVaryingInfo = false;
	/// Adds `--backend-no-var-renaming`.
	bool noVarRenaming = false;
	/// Adds `--backend-no-compound-operators`.
	bool noCompoundOperators = false;
	/// Adds `--backend-no-symbolic-names`.
	bool noSymbolicNames = false;
	/// `optim` or `pessim` → `--backend-call-info-obtainer`. Empty omits.
	std::string callInfoObtainer;
	/// Non-empty → `--ar-name` (archive member).
	std::string arName;
	/// `little` or `big` → `-e` / `--endian`. Empty omits.
	std::string endian;
	/// 16, 32, or 64 → `-b` / `--bit-size`. Zero omits.
	int bitSize = 0;
	/// Non-zero → `--raw-section-vma` as 0xHEX.
	uint64_t rawSectionVma = 0;
	/// Adds `-m raw` (firmware / raw image). Default false = omit (bin).
	bool rawMode = false;
	/// Adds `--no-memory-limit`.
	bool noMemoryLimit = false;
	/// Non-empty → `--backend-disabled-opts`.
	std::string backendDisabledOpts;
	/// Non-empty → `--backend-enabled-opts`.
	std::string backendEnabledOpts;
	/// >= 0 → `--ar-index` (archive member by index). -1 omits. Ignored if arName is set.
	int arIndex = -1;
	/// Adds `--backend-no-opts` when not already in fast mode (fast already emits it).
	bool backendNoOpts = false;
	DecompilerSettings decompiler;
};

/// A file deleted when the object is destroyed (the --llvm-passes-json input
/// must outlive the decompiler run: keep it alongside the DecompileRun).
class TemporaryFile {
public:
	explicit TemporaryFile(std::filesystem::path path): path_(std::move(path)) {}
	~TemporaryFile();
	TemporaryFile(const TemporaryFile&) = delete;
	TemporaryFile& operator=(const TemporaryFile&) = delete;
	const std::filesystem::path& path() const
	{
		return path_;
	}

private:
	std::filesystem::path path_;
};

struct DecompilerArguments
{
	/// argv after the executable.
	std::vector<std::string> args;
	/// Non-fatal problem (e.g. the configured --config file is missing and was skipped).
	std::string warning;
	/// The --llvm-passes-json temp file, when one was written.
	std::unique_ptr<TemporaryFile> llvmPassesFile;
};

/**
 * @brief Build the argument vector passed to retdec-decompiler (same order
 *        and spelling as the Qt GUI).
 *
 * With @p writeLlvmPassesFile and either custom LLVM passes or the fast preset,
 * the preset (`llvm_passes_fast.json` / `llvm_passes_default.json` from
 * resolveGuiResource()) minus `decompiler.llvmPassesDisabled` is written to a
 * temporary file and passed with `--llvm-passes-json`. Fails only when that
 * file cannot be produced.
 */
std::expected<DecompilerArguments, std::string>
buildDecompilerArguments(const DecompilerLaunchRequest& req, bool writeLlvmPassesFile = true);

/// The LLVM pass list a run would use: the preset minus @p disabled (every occurrence).
std::expected<std::vector<std::string>, std::string>
loadLlvmPassPreset(bool fastPreset, const std::vector<std::string>& disabled = {});

/// Variables to set on top of the inherited environment (spawn_spec::env_set).
using ProcessEnvironment = std::map<std::string, std::string>;

/**
 * @brief Child-process environment overrides for retdec-decompiler.
 *
 * When @p applyInteractiveOverrides is false (headless / quit-when-done) the
 * result is empty, so the child sees the GUI's environment unchanged (CLI
 * parity). Interactive runs copy ML / CUDA settings into `RETDEC_NEURAL_*`
 * (when the GGUF model file exists), `RETDEC_OCL_HOST`, `RETDEC_OCL_CACHE_DIR`
 * and `RETDEC_PROFILE_JSON`.
 */
ProcessEnvironment buildDecompilerProcessEnvironment(const AppSettings& settings, bool applyInteractiveOverrides);

/// Value the child would see for @p name: the override, else the GUI's own environment.
std::string processEnvironmentValue(const ProcessEnvironment& env, const std::string& name);

/// Resolved `<basename>.gui-decompiled.c` path (lexical join, '/' separators).
/// Empty @p outputDir = beside the binary.
std::filesystem::path
resolveGuiDecompiledCPath(const std::filesystem::path& binaryPath, std::string_view outputDir = {});

/// Prefer configured @p outputDir, then beside-binary; an existing file's
/// absolute path, else the preferred default (not made absolute).
std::filesystem::path
locateGuiDecompiledCPath(const std::filesystem::path& binaryPath, std::string_view outputDir = {});

// ─── Log reading ─────────────────────────────────────────────────────────────

/// Parsed decompile progress from the log.
struct DecompileLogProgress
{
	std::string stage; ///< GUI pipeline stage name (ProgressPanel's list)
	int percent = 0;   ///< approximate, 1..99
};

/**
 * @brief Read @p logPath from @p *ioFileOffset to its end and map
 *        retdec-decompiler log lines ("Running phase: ...", " -> ...",
 *        "[progress] ... NN%") to a GUI stage and percent. The offset moves to
 *        the end of the file. Nothing when no new line maps to a stage.
 */
std::optional<DecompileLogProgress>
pollDecompileLogProgress(const std::filesystem::path& logPath, uint64_t* ioFileOffset);

/// The whole log for the console after a run, or only its tail when huge.
struct DecompilerLogText
{
	/// "… (N KiB log omitted, showing last M KiB) …" when truncated, else empty.
	std::string notice;
	/// The log, or its last @p maxBytes from the first complete line.
	std::string text;
};

/// Read the log for the console (tail only if larger than @p maxBytes).
DecompilerLogText readDecompilerLogForConsole(const std::filesystem::path& logPath, uint64_t maxBytes = 512 * 1024);

/// Bytes appended to @p logPath since @p *ioFileOffset (at most
/// @p maxBytesPerTick); advances the offset. Empty when nothing is new.
std::string readDecompilerLogIncremental(
	const std::filesystem::path& logPath, uint64_t* ioFileOffset, uint64_t maxBytesPerTick = 32 * 1024);

/// Last @p maxBytes of the log ("…\n" prefixed when cut), for the command history.
std::string readDecompilerLogTail(const std::filesystem::path& logPath, uint64_t maxBytes = 4000);

/// Warning / error lines of a log file (source "retdec-decompiler"), at most @p maxEntries.
std::vector<Diagnostic> scanDecompilerLogDiagnostics(const std::filesystem::path& logPath, int maxEntries = 200);

/// Same for the complete lines appended since @p *ioFileOffset; advances the
/// offset past them (a partial last line is left for the next call).
std::vector<Diagnostic> scanDecompilerLogDiagnosticsIncremental(
	const std::filesystem::path& logPath, uint64_t* ioFileOffset, int maxEntries = 50);

/// Severity of one log line: Error / Warning, or nothing for an ordinary line.
std::optional<Severity> classifyDecompilerLogLine(std::string_view line);

/**
 * @brief Problems-panel rows from the `semanticDetections` arrays of a
 *        decompile config, filtered by the Recovery / Analysis settings
 *        (detector switches and confidence floors), at most @p maxEntries.
 *
 * Severity: confidence > 0.8 Info, > 0.5 Warning, otherwise Info (the Qt
 * "muted" level; Severity has no such level). Message: "LABEL (confidence N%)"
 * plus " — DETAIL". source = "semantic", function = the config function name,
 * address = its startAddr.
 */
std::vector<Diagnostic> semanticDetectionDiagnostics(
	const forge::json::value& configRoot, const AppSettings& settings = AppSettings::instance(), int maxEntries = 500);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DECOMPILER_LAUNCH_H
