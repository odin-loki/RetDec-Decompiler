/**
 * @file include/retdec/gui/model/dlg_settings_form.h
 * @brief Everything the Settings dialog computes that is not drawing: combo
 *        item lists and their stored values, the LLVM pass list, the checks
 *        Apply runs before writing, and plugin row / detail text.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DLG_SETTINGS_FORM_H
#define RETDEC_GUI_MODEL_DLG_SETTINGS_FORM_H

#include "retdec/gui/model/plugins.h"
#include "retdec/gui/model/settings.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// A combo entry: what is shown and what is stored.
struct ComboEntry
{
	std::string label;
	std::string value;
};

/// "Dark", "Light", "System Default" (index = GeneralSettings::Theme).
const std::vector<std::string>& settingsThemeItems();
/// "English (en)" … "Chinese (zh)" with codes en, de, fr, es, zh.
const std::vector<ComboEntry>& settingsLanguageItems();
/// Code -> index (0 = English for an unknown code).
int settingsLanguageIndex(std::string_view code);
/// Index -> code ("en" when out of range).
std::string settingsLanguageCode(int index);

/// "Q4_0" … "F32" (index = MLSettings::QuantLevel).
const std::vector<std::string>& settingsQuantItems();
/// "CPU", "GPU", "Auto" (index = MLSettings::InferenceDevice).
const std::vector<std::string>& settingsInferenceDeviceItems();
/// "Quiet", "Normal", "Verbose", "Debug" (index = AdvancedSettings::Verbosity).
const std::vector<std::string>& settingsVerbosityItems();

/// Fast / Balanced / Quality -> fast / balanced / quality.
const std::vector<ComboEntry>& settingsProfileItems();
/// Stored value -> index; unknown -> 1 (Balanced), as the Qt dialog did.
int settingsProfileIndex(std::string_view value);
/// C / Python / C# / Java / WAT -> c / python / csharp / java / wat.
const std::vector<ComboEntry>& settingsOutputLangItems();
/// Stored value -> index; unknown -> 0 (C).
int settingsOutputLangIndex(std::string_view value);

/// "Auto (default)" then the CUDA devices the GUI knows of (none yet).
const std::vector<std::string>& settingsCudaDeviceItems();

/// Monospace families offered by the editor-font picker: @p current first
/// (when not empty), then the usual programming fonts, without duplicates.
std::vector<std::string> settingsEditorFontFamilies(std::string_view current);

/**
 * @brief Unique pass names of the stock pipeline
 *        (`llvm_passes_default.json` from resolveGuiResource()), in first-seen
 *        order. Fails with "Could not load pass list resource
 *        (llvm_passes_default.json)." when the resource is missing or bad.
 */
std::expected<std::vector<std::string>, std::string> settingsLlvmPassNames();

/// Names whose checkbox is off, in list order (the stored llvmPassesDisabled).
std::vector<std::string> settingsDisabledPasses(
	const std::vector<std::string>& names, const std::vector<bool>& checked);

/**
 * @brief The checks the Qt dialog ran before applying: a non-empty decompile
 *        output directory must exist or be creatable (it is created here), and
 *        a non-empty --config file must exist. Errors (verbatim):
 *        "Decompile output directory cannot be created:\nDIR" and
 *        "Decompiler configuration file does not exist:\nFILE".
 */
std::expected<void, std::string> validateSettingsForApply(const DecompilerSettings& d);

/// Plugin list row: "Name  vVERSION".
std::string pluginRowLabel(const PluginMetadata& m);
/// Detail line under the plugin list:
/// "Name vVERSION — DESCRIPTION\nAuthor: AUTHOR\nFile: PATH".
std::string pluginDetailText(const PluginMetadata& m, const std::string& filePath);
/// Shared-library glob for "Install Plugin…" ("*.dll", "*.dylib;*.so", "*.so").
std::string_view pluginLibraryFilter();

/// Trimmed, non-empty entries (search paths list -> settings).
std::vector<std::string> settingsNonEmptyTrimmed(const std::vector<std::string>& in);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DLG_SETTINGS_FORM_H
