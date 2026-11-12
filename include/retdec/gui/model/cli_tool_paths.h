/**
 * @file include/retdec/gui/model/cli_tool_paths.h
 * @brief Locate the RetDec command-line tools, scripts and GUI resources that
 *        ship beside retdec-gui.
 *
 * Search order for a tool `name` (`name.exe` on Windows):
 *   1. beside the GUI binary (`<bin>/name`);
 *   2. macOS bundle: when the GUI runs from `X.app/Contents/MacOS`, the tools
 *      in `X.app/Contents/MacOS` (step 1) and then `<prefix>/bin` beside the
 *      bundle (`X.app/../bin`), where `install(TARGETS ... RUNTIME)` puts them;
 *   3. PATH (forge::os::find_executable).
 *
 * Every function returns an absolute path, or an empty path when nothing is
 * found.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_CLI_TOOL_PATHS_H
#define RETDEC_GUI_MODEL_CLI_TOOL_PATHS_H

#include <filesystem>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// Directory of the running executable (forge::os::exe_path()), or the
/// override set by setApplicationDirPathOverride().
std::filesystem::path applicationDirPath();

/// Pretend the GUI binary lives in @p dir (tests). An empty path clears it.
void setApplicationDirPathOverride(const std::filesystem::path& dir);

/// Directories that may hold `share/` relative to the GUI binary: `<bin>/..`,
/// and for a macOS bundle also `Contents/` (i.e. `Contents/MacOS/..`) and the
/// bundle's install prefix (`X.app/..`). The first entry is always `<bin>/..`.
std::vector<std::filesystem::path> installPrefixCandidates();

/// Resolve `name` / `name.exe` next to the GUI binary (or beside the macOS
/// bundle), then on PATH.
std::filesystem::path resolveCliTool(std::string_view toolBaseName);

std::filesystem::path resolveRetdecFileinfoExecutable(); ///< retdec-fileinfo, then fileinfo
std::filesystem::path resolveRetdecUnpackerExecutable(); ///< retdec-unpacker, then unpacker
std::filesystem::path resolveRetdecDecompilerExecutablePath();
std::filesystem::path resolveRetdecArExtractorExecutable(); ///< retdec-ar-extractor, then ar_extractor
std::filesystem::path resolveRetdecMachoExtractorExecutable();
std::filesystem::path resolveRetdecBin2patExecutable();
std::filesystem::path resolveRetdecPat2yaraExecutable();

/// Install-relative `share/retdec/decompiler-config.json` next to
/// `retdec-decompiler` (`<dec>/../share/...`, `<dec>/../../share/...`), if present.
std::filesystem::path resolveBundledDecompilerConfigPath();

/// `python3` or `python` on PATH (`py.exe` too on Windows).
std::filesystem::path resolvePythonInterpreter();

/// Installed `retdec-signature-from-library-creator.py` (empty if not found).
std::filesystem::path resolveSignatureFromLibraryCreatorScript();

/// Installed `retdec_export_intel.py` (empty if not found).
std::filesystem::path resolveRetdecExportIntelScript();

/// The GUI's run-time resource directory (themes, LLVM pass presets, fonts):
/// `<prefix>/share/retdec/gui` for each installPrefixCandidates() entry, then
/// the source tree's `src/gui/resources` (RETDEC_GUI_SOURCE_RESOURCE_DIR) so a
/// build-tree binary or test finds them unstaged. Empty if none exists.
std::filesystem::path resolveGuiResourceDir();

/// `resolveGuiResourceDir() / name` when that file exists (searching every
/// candidate directory, not just the first one found), else empty.
std::filesystem::path resolveGuiResource(std::string_view name);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_CLI_TOOL_PATHS_H
