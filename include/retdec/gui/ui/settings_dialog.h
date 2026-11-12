/**
 * @file include/retdec/gui/ui/settings_dialog.h
 * @brief SettingsDialog — the tabbed dialog for every model::AppSettings
 *        group (Tools → Settings…, Ctrl+,).
 *
 * Tabs (as the Qt dialog, in its order):
 *   General    — theme, editor font + size, language, line numbers, word wrap, session restore
 *   Analysis   — stage toggles, confidence thresholds, max analysis time, thread count
 *   CUDA       — device, prefer GPU, work-group size, kernel cache dir, profiling
 *   ML         — GGUF model path, quantisation, inference device, temperature,
 *                top-P / top-K, max new tokens, context length, streaming,
 *                model SHA-256, batch refine (unused)
 *   Recovery   — detector toggles and confidence thresholds
 *   Advanced   — verbosity, dump options, IR dump path, intermediate dir, max functions, demangling
 *   Decompiler — profile, custom LLVM pass list (+ all / none / defaults), output
 *                directory, output language, live console tail, --config JSON
 *   Plugins    — search paths (add / remove / rescan), installed plugins
 *                (enable, install, unload, details)
 *
 * Buttons: Restore Defaults, Export…, Import…, OK, Cancel, Apply.
 *
 * Edits stay in the widgets until Apply / OK: Apply validates (output
 * directory creatable, --config file exists — a warning box otherwise and
 * nothing is written), copies every field into the AppSettings, notifies its
 * listeners, saves and calls onApplied (the main window re-applies theme and
 * fonts there). OK = Apply, then close when it succeeded. Cancel closes
 * without writing. As in the Qt dialog, picking an editor font applies it at
 * once (live preview) and toggling a plugin saves immediately; Restore
 * Defaults resets the AppSettings and reloads the fields.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_SETTINGS_DIALOG_H
#define RETDEC_GUI_UI_SETTINGS_DIALOG_H

#include "retdec/gui/model/plugins.h"
#include "retdec/gui/model/settings.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

class SettingsDialog {
public:
	enum Tab
	{
		General,
		Analysis,
		CUDA,
		ML,
		Recovery,
		Advanced,
		Decompiler,
		Plugins,
		kTabCount
	};

	explicit SettingsDialog(fuse::ui::UiContext& ui,
	                        model::AppSettings& settings = model::AppSettings::instance(),
	                        model::PluginManager& plugins = model::PluginManager::instance());
	~SettingsDialog();
	SettingsDialog(const SettingsDialog&) = delete;
	SettingsDialog& operator=(const SettingsDialog&) = delete;

	/// Load the fields from the settings and open the dialog.
	void show();
	bool isOpen() const { return dlg_.isOpen(); }
	std::string windowTitle() const { return "RetDec Settings"; }
	/// The Qt dialog's minimum size (720 x 540); the card is 760 x 580.
	fuse::ui::Size minimumSize() const { return {720.f, 540.f}; }

	// ── Buttons ──────────────────────────────────────────────────────────────
	/// Validate, write, notify, save. False (and a warning) when validation fails.
	bool apply();
	/// apply() and close on success.
	void ok();
	void cancel();
	void restoreDefaults();
	/// Write the current fields to the settings, then export them (Qt Export…).
	std::expected<void, std::string> exportTo(const std::filesystem::path& path);
	/// Import settings from @p path and reload the fields (Qt Import…).
	std::expected<void, std::string> importFrom(const std::filesystem::path& path);

	/// Fields <- settings / settings <- fields (no validation, no save; the
	/// latter notifies the settings' listeners).
	void populateFromSettings();
	void applyToSettings();

	// ── Decompiler tab actions ───────────────────────────────────────────────
	void enableAllPasses();
	void disableAllPasses();
	/// "Defaults (stock pipeline)": custom list off, every pass on.
	void defaultPasses();

	// ── Plugins tab actions ──────────────────────────────────────────────────
	/// "Add path…" result.
	void addSearchPath(const std::string& path);
	/// "Remove": the selected search path.
	void removeSelectedSearchPath();
	/// "Rescan": load plugins from the listed paths.
	int rescanPlugins();
	/// "Install Plugin…" result: load the library and list it (enabled).
	std::expected<std::string, std::string> installPlugin(const std::filesystem::path& file);
	/// "Unload Selected".
	void unloadSelectedPlugin();
	/// Enable / disable plugin row @p row (saves enabledPlugins immediately).
	void setPluginEnabled(int row, bool enabled);
	void selectPlugin(int row);
	std::vector<std::string> searchPaths() const { return searchPaths_; }
	/// (id, enabled) per row, in order.
	std::vector<std::pair<std::string, bool>> pluginRows() const;
	std::string pluginDetail() const;

	// ── Events ───────────────────────────────────────────────────────────────
	/// After Apply / OK wrote and saved the settings (re-apply theme, fonts).
	std::function<void()> onApplied;
	/// OK closed the dialog (QDialog::Accepted).
	std::function<void()> onAccepted;
	/// Cancel closed it (QDialog::Rejected).
	std::function<void()> onRejected;
	/// A warning was shown (title, text); the dialog also shows it in a box.
	std::function<void(const std::string& title, const std::string& message)> onWarning;

	// ── Test hooks ───────────────────────────────────────────────────────────
	/// A field by the Qt member / object name without the trailing underscore,
	/// e.g. "themeCombo", "typeConfSpin", "mlModelSha256", "decompileProfileCombo".
	fuse::ui::NodeHandle field(std::string_view name) const;
	std::vector<std::string> fieldNames() const;
	fuse::ui::NodeHandle tabs() const { return tabs_; }
	int currentTab() const;
	void setCurrentTab(int tab);
	std::vector<std::string> tabTitles() const;
	const std::vector<std::string>& llvmPassNames() const { return passNames_; }
	fuse::ui::NodeHandle llvmPassCheck(std::size_t i) const { return i < passChecks_.size() ? passChecks_[i] : fuse::ui::NodeHandle{}; }
	/// The pass-list error label text ("" when the list loaded).
	std::string llvmPassError() const { return passError_; }
	fuse::ui::NodeHandle okButton() const { return ok_; }
	fuse::ui::NodeHandle applyButton() const { return apply_; }
	fuse::ui::NodeHandle cancelButton() const { return cancel_; }
	fuse::ui::NodeHandle restoreDefaultsButton() const { return restore_; }
	fuse::ui::NodeHandle exportButton() const { return export_; }
	fuse::ui::NodeHandle importButton() const { return import_; }
	fuse::ui::NodeHandle pluginList() const { return pluginList_; }
	fuse::ui::NodeHandle pluginEnabledCheck() const { return pluginEnabled_; }
	fuse::ui::NodeHandle searchPathList() const { return pathList_; }
	FilePrompt& filePrompt() { return files_; }
	MessageDialog& message() { return message_; }
	fuse::ui::NodeHandle card() const { return dlg_.card(); }
	model::AppSettings& settings() { return settings_; }

private:
	void buildGeneral(fuse::ui::NodeHandle page);
	void buildAnalysis(fuse::ui::NodeHandle page);
	void buildCuda(fuse::ui::NodeHandle page);
	void buildMl(fuse::ui::NodeHandle page);
	void buildRecovery(fuse::ui::NodeHandle page);
	void buildAdvanced(fuse::ui::NodeHandle page);
	void buildDecompiler(fuse::ui::NodeHandle page);
	void buildPlugins(fuse::ui::NodeHandle page);

	fuse::ui::NodeHandle reg(std::string_view name, fuse::ui::NodeHandle n);
	fuse::ui::NodeHandle check(fuse::ui::NodeHandle parent, std::string_view name, std::string_view text,
	                           std::string_view tooltip = {});
	fuse::ui::NodeHandle spin(fuse::ui::NodeHandle parent, std::string_view name, std::string_view label, float lo,
	                          float hi, float step, std::string_view hint = {}, std::string_view suffix = {},
	                          std::string_view specialValue = {});
	fuse::ui::NodeHandle combo(fuse::ui::NodeHandle parent, std::string_view name, std::string_view label,
	                           const std::vector<std::string>& items, std::string_view hint = {});
	/// Label + line edit + "…" browse button that opens the file prompt.
	fuse::ui::NodeHandle pathRow(fuse::ui::NodeHandle parent, std::string_view name, std::string_view label,
	                             FilePrompt::Mode mode, std::string_view promptTitle, std::string_view filter);
	void warn(const std::string& text);
	void refreshPluginList();
	void refreshSearchPaths();
	void onFontPicked();
	bool checked(std::string_view name) const;
	void setChecked(std::string_view name, bool on);
	double number(std::string_view name, int decimals = 2) const;
	void setNumber(std::string_view name, double v);
	std::string text(std::string_view name) const;
	void setText(std::string_view name, std::string_view v);

	fuse::ui::UiContext& ui_;
	model::AppSettings& settings_;
	model::PluginManager& plugins_;
	ModalDialog dlg_;
	MessageDialog message_;
	FilePrompt files_;
	fuse::ui::NodeHandle tabs_, ok_, apply_, cancel_, restore_, export_, import_;
	fuse::ui::NodeHandle fontPicker_, fontList_, fontSizeSpin_;
	fuse::ui::NodeHandle pluginList_, pluginEnabled_, pluginDetail_, pathList_;
	std::map<std::string, fuse::ui::NodeHandle, std::less<>> fields_;
	std::vector<std::string> fontFamilies_;
	std::vector<std::string> passNames_;
	std::vector<fuse::ui::NodeHandle> passChecks_;
	std::string passError_;
	std::vector<std::string> searchPaths_;
	std::vector<std::string> pluginIds_;
	struct PathRow
	{
		fuse::ui::NodeHandle edit;
		FilePrompt::Mode mode;
		std::string title, filter;
	};
	std::vector<PathRow> pathRows_;
	bool populating_ = false;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_SETTINGS_DIALOG_H
