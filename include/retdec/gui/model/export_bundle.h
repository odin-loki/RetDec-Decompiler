/**
 * @file include/retdec/gui/model/export_bundle.h
 * @brief Export decompile artifacts as a ZIP bundle for sharing / debugging.
 *
 * The bundle holds, each under its file name: the .c and its sidecars
 * (.config.json, .dsm, .ll), the neural-refinement outputs (`<c>.refined.c`,
 * `<c>.refinement-manifest.json`, `<c>.type-inference.json`), the CFG / call
 * graph dumps (`<stem>.cfg.*.dot`, `<stem>.cg.dot`), `decompiler-command.txt`
 * (executable, cwd, argv) and `decompiler.log`. Missing optional files are
 * skipped.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_EXPORT_BUNDLE_H
#define RETDEC_GUI_MODEL_EXPORT_BUNDLE_H

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace retdec::gui::model {

struct DecompileBundleInput
{
	std::filesystem::path cPath;
	std::string decompilerExe;
	std::vector<std::string> decompilerArgs;
	std::string decompilerCwd;
	/// Optional on-disk log (e.g. a running / finished DecompileRun's log file).
	std::filesystem::path logFilePath;
	/// Optional inline log, used when the log file is absent or empty (e.g. the console buffer).
	std::string logInline;
};

/// Build @p zipPath from the decompiler sidecars next to @p in.cPath.
std::expected<void, std::string>
exportDecompileBundle(const DecompileBundleInput& in, const std::filesystem::path& zipPath);

/// "executable: X\ncwd: Y\ncommand: a b c\n" (cwd line only when set).
std::string formatDecompilerCommandText(const DecompileBundleInput& in);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_EXPORT_BUNDLE_H
