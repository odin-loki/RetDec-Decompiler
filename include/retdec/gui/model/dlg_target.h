/**
 * @file include/retdec/gui/model/dlg_target.h
 * @brief Target panel logic: writing the arch / OS / entry-point overrides
 *        into a ProjectFile, the decompiler --config shortcuts it offers, and
 *        the JSON check of its config editor. Also the batch-decompile
 *        dialog's path list rule.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DLG_TARGET_H
#define RETDEC_GUI_MODEL_DLG_TARGET_H

#include "retdec/gui/model/project_file.h"
#include "retdec/gui/model/settings.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// The Target panel's text fields.
struct TargetFields
{
	std::string arch;
	std::string os;
	std::string entry;
};

/// Fields shown for @p pf (entry "0x…" or empty for 0).
TargetFields targetFieldsFromProject(const ProjectFile& pf);

/**
 * @brief Apply the fields to @p pf (arch / OS trimmed, entry parsed with
 *        dlgParseEntryPoint). On an invalid entry point the arch and OS are
 *        already written (as the Qt main window did) and the error is
 *        "Invalid entry point (use 0x… hex or decimal).".
 */
std::expected<void, std::string> applyTargetFields(ProjectFile& pf, const TargetFields& f);

/// Status after a successful apply.
inline constexpr std::string_view kTargetAppliedStatus = "Target fields applied to project (save .retdec to persist).";
inline constexpr std::string_view kNoProjectStatus = "No project loaded.";

/// Set / clear decompiler.extraConfigPath, save and notify (the Target panel's
/// Browse / Use bundled / Clear buttons). Browse stores the absolute path.
/// Returns the save error, if any (the setting is changed either way).
std::expected<void, std::string> setDecompilerConfigPath(AppSettings& s, const std::filesystem::path& path);

/// The config path is editable: non-empty and the file exists.
bool decompilerConfigEditable(std::string_view path);

/// Where the Target panel's Browse starts: the configured file, else the
/// directory of @p binaryPath ("—" = none), else "".
std::string targetConfigBrowseStart(std::string_view configuredPath, std::string_view binaryPath);

/// Check @p text is a JSON document: "MESSAGE at offset N" on error.
std::expected<void, std::string> validateJsonText(std::string_view text);

/// Read / overwrite a UTF-8 text file ("Cannot open:\nPATH" on read failure).
std::expected<std::string, std::string> readTextFile(const std::filesystem::path& path);
std::expected<void, std::string> writeTextFile(const std::filesystem::path& path, std::string_view text);

// ─── Batch decompile dialog ──────────────────────────────────────────────────

/// Append @p added to @p list as absolute paths, skipping ones already
/// present. Returns how many were appended.
int batchAddPaths(std::vector<std::string>& list, const std::vector<std::string>& added);
/// Remove the entries at @p indices (any order, out-of-range ignored).
void batchRemoveIndices(std::vector<std::string>& list, std::vector<int> indices);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DLG_TARGET_H
