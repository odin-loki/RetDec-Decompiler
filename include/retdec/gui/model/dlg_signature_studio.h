/**
 * @file include/retdec/gui/model/dlg_signature_studio.h
 * @brief Signature Studio logic: the argv of each RetDec signature tool
 *        (express Python pipeline, ar / Mach-O extractors, bin2pat, pat2yara),
 *        defaults derived from the active binary, and a runner that launches
 *        one tool at a time through DecompileRun (forge::os spawn, output to
 *        a log file) and reports what the Command log needs.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DLG_SIGNATURE_STUDIO_H
#define RETDEC_GUI_MODEL_DLG_SIGNATURE_STUDIO_H

#include "retdec/gui/model/decompile_run.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// One tool invocation.
struct SigToolCommand
{
	std::string toolLabel;              ///< e.g. "retdec-bin2pat"
	std::filesystem::path executable;   ///< program (python for the express pipeline)
	std::vector<std::string> arguments; ///< argv after the program
	std::filesystem::path workingDirectory;
	/// When the command filled in a default output path (bin2pat / pat2yara with
	/// an empty output field), the path, so the panel can show it.
	std::string defaultedOutput;
};

/// Defaults the panel fills into empty fields when a binary becomes active.
struct SigStudioDefaults
{
	std::string expressLib; ///< the binary
	std::string expressOut; ///< <dir>/<completeBaseName>-libsig.yar
	std::string arArchive;  ///< the binary
	std::string machoFile;  ///< the binary
	std::string workDir;    ///< <dir>/retdec-sig-work (ar and Mach-O output)
};
SigStudioDefaults sigStudioDefaultsForBinary(const std::filesystem::path& binary);

/// Express: `python <script> -o OUT LIB`, cwd = the script's directory.
/// Errors (verbatim Qt log lines): "Express: set library input and YARA output
/// path." and "Express: Python 3 or retdec-signature-from-library-creator.py
/// not found. Use a full RetDec install with scripts/ on disk."
std::expected<SigToolCommand, std::string> buildExpressCommand(
	std::string_view lib,
	std::string_view out,
	const std::filesystem::path& python,
	const std::filesystem::path& script);

/// `retdec-ar-extractor -e -o OUTDIR ARCHIVE`. Error: "Set archive and output directory."
std::expected<SigToolCommand, std::string>
buildArExtractorCommand(const std::filesystem::path& exe, std::string_view archive, std::string_view outDir);

/// `retdec-macho-extractor --all -o OUTDIR FILE`. Error: "Set Mach-O file and output directory."
std::expected<SigToolCommand, std::string>
buildMachoExtractorCommand(const std::filesystem::path& exe, std::string_view file, std::string_view outDir);

/// `retdec-bin2pat -o OUT IN`; an empty OUT becomes "<abs IN>.pat". Error: "Set bin2pat input file."
std::expected<SigToolCommand, std::string>
buildBin2patCommand(const std::filesystem::path& exe, std::string_view in, std::string_view out);

/// `retdec-pat2yara -o OUT IN`; an empty OUT becomes "<abs IN>.yar". Error: "Set pat2yara input file."
std::expected<SigToolCommand, std::string>
buildPat2yaraCommand(const std::filesystem::path& exe, std::string_view in, std::string_view out);

/// "Use YARA as static-code sigfile": the express YARA output when it exists,
/// else the pat2yara output when it exists; absolute. Nothing when neither is
/// on disk (status: "No YARA output file on disk yet.").
std::optional<std::string> pickStaticSigFile(std::string_view expressYaraOut, std::string_view patYaraOut);

/// Last @p maxChars bytes of @p text (Qt: QString::right(3500)), cut at a
/// UTF-8 character boundary.
std::string sigToolOutputTail(std::string_view text, std::size_t maxChars = 3500);

/// What a finished (or failed) tool run reports to the Command log.
struct SigToolResult
{
	std::string tool;
	std::vector<std::string> arguments;
	std::string workingDirectory;
	int exitCode = -1; ///< -1 when it failed to start
	int64_t elapsedMs = 0;
	std::string outputTail; ///< merged stdout+stderr tail, or the error text
	bool failedToStart = false;
};

/// The line the panel's log gets: "--- TOOL exit N ---\nTAIL" (or "Error: …").
std::string sigToolLogEntry(const SigToolResult& r);

/**
 * @brief Runs one signature tool at a time (DecompileRun underneath).
 *
 * start() fails when a run is in progress ("busy"), when the executable is
 * empty ("Tool not found: LABEL") or when the spawn fails ("Failed to start:
 * LABEL"). Call poll() from a UI timer; it returns the result once.
 */
class SigToolRunner {
public:
	SigToolRunner();
	~SigToolRunner();
	SigToolRunner(const SigToolRunner&) = delete;
	SigToolRunner& operator=(const SigToolRunner&) = delete;

	std::expected<void, std::string> start(const SigToolCommand& cmd);
	bool running() const;
	/// The result once the run has ended (returned once), else nothing.
	std::optional<SigToolResult> poll();
	/// Block up to @p timeout for the run to end, then poll().
	std::optional<SigToolResult> waitFor(std::chrono::milliseconds timeout);
	void cancel();

private:
	std::optional<SigToolResult> collect();

	std::unique_ptr<DecompileRun> run_;
	SigToolCommand cmd_;
	bool reported_ = true;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DLG_SIGNATURE_STUDIO_H
