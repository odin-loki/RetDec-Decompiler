/**
 * @file include/retdec/gui/ui/signature_studio_panel.h
 * @brief SignatureStudioPanel — build static-code signatures with the RetDec
 *        tools (Tools → Signature Studio…).
 *
 * Tabs (as the Qt panel):
 *   Express     retdec-signature-from-library-creator.py (Python 3): library → YARA
 *   AR          retdec-ar-extractor -e -o OUTDIR ARCHIVE
 *   Mach-O      retdec-macho-extractor --all -o OUTDIR FILE
 *   PAT / YARA  retdec-bin2pat -o OUT IN, retdec-pat2yara -o OUT IN
 * then the last tool's output tail (200 lines max) and "Use YARA as
 * static-code sigfile".
 *
 * One tool runs at a time (model::SigToolRunner: forge::os spawn, output to a
 * log file, polled from a UI timer); the Run buttons are disabled meanwhile.
 * Each finished run is logged ("--- TOOL exit N ---" + output tail) and
 * reported through onCliToolFinished for the Command log. Tools are found
 * beside the GUI or on PATH (model::resolve*), overridable for tests.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_SIGNATURE_STUDIO_PANEL_H
#define RETDEC_GUI_UI_SIGNATURE_STUDIO_PANEL_H

#include "retdec/gui/model/dlg_signature_studio.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class CodeView;

class SignatureStudioPanel {
public:
	/// Where each tool comes from (defaults: model::resolve* functions).
	struct ToolResolver
	{
		std::function<std::filesystem::path()> python;
		std::function<std::filesystem::path()> expressScript;
		std::function<std::filesystem::path()> arExtractor;
		std::function<std::filesystem::path()> machoExtractor;
		std::function<std::filesystem::path()> bin2pat;
		std::function<std::filesystem::path()> pat2yara;
	};
	static ToolResolver defaultToolResolver();

	SignatureStudioPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~SignatureStudioPanel();
	SignatureStudioPanel(const SignatureStudioPanel&) = delete;
	SignatureStudioPanel& operator=(const SignatureStudioPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}
	std::string panelTitle() const
	{
		return "Signature Studio";
	}

	/// A binary was opened: express fields are reset, empty fields get
	/// defaults (model::sigStudioDefaultsForBinary).
	void setActiveBinary(const std::string& path);
	/// Forget the binary and clear the log.
	void clear();

	// ── Run buttons ──────────────────────────────────────────────────────────
	/// Each validates its fields (a log line otherwise) and starts the tool;
	/// true when it started.
	bool runExpress();
	bool runArExtractor();
	bool runMachoExtractor();
	bool runBin2pat();
	bool runPat2yara();
	/// "Use YARA as static-code sigfile".
	void useAsStaticSigFile();

	bool busy() const;
	/// Collect a finished run (the UI timer calls this); true when one finished.
	bool poll();
	/// Block up to @p timeout for the running tool, then poll() (tests, shutdown).
	bool waitForTool(std::chrono::milliseconds timeout);

	void setToolResolver(ToolResolver r)
	{
		tools_ = std::move(r);
	}

	/// The log pane's text.
	std::string log() const;

	// ── Events ───────────────────────────────────────────────────────────────
	/// A tool finished (or failed to start): the Command log entry.
	std::function<void(const model::SigToolResult& result)> onCliToolFinished;
	/// "Use YARA as static-code sigfile" picked this absolute path.
	std::function<void(const std::string& path)> onUseAsStaticSigFile;
	std::function<void(const std::string& message, int timeoutMs)> onStatusMessage;

	// ── Test hooks ───────────────────────────────────────────────────────────
	fuse::ui::NodeHandle tabs() const
	{
		return tabs_;
	}
	void setCurrentTab(int tab);
	fuse::ui::NodeHandle expressLibEdit() const
	{
		return expressLib_;
	}
	fuse::ui::NodeHandle expressOutEdit() const
	{
		return expressOut_;
	}
	fuse::ui::NodeHandle expressRunButton() const
	{
		return expressRun_;
	}
	fuse::ui::NodeHandle arArchiveEdit() const
	{
		return arArchive_;
	}
	fuse::ui::NodeHandle arOutDirEdit() const
	{
		return arOut_;
	}
	fuse::ui::NodeHandle arRunButton() const
	{
		return arRun_;
	}
	fuse::ui::NodeHandle machoFileEdit() const
	{
		return machoFile_;
	}
	fuse::ui::NodeHandle machoOutDirEdit() const
	{
		return machoOut_;
	}
	fuse::ui::NodeHandle machoRunButton() const
	{
		return machoRun_;
	}
	fuse::ui::NodeHandle bin2patInEdit() const
	{
		return b2pIn_;
	}
	fuse::ui::NodeHandle bin2patOutEdit() const
	{
		return b2pOut_;
	}
	fuse::ui::NodeHandle bin2patRunButton() const
	{
		return b2pRun_;
	}
	fuse::ui::NodeHandle pat2yaraInEdit() const
	{
		return p2yIn_;
	}
	fuse::ui::NodeHandle pat2yaraOutEdit() const
	{
		return p2yOut_;
	}
	fuse::ui::NodeHandle pat2yaraRunButton() const
	{
		return p2yRun_;
	}
	fuse::ui::NodeHandle useAsStaticButton() const
	{
		return useAsStatic_;
	}
	FilePrompt& filePrompt()
	{
		return files_;
	}

private:
	fuse::ui::NodeHandle fileRow(
		fuse::ui::NodeHandle form,
		std::string_view label,
		FilePrompt::Mode mode,
		std::string_view promptTitle,
		std::string_view filter);
	bool start(std::expected<model::SigToolCommand, std::string> cmd);
	void appendLog(std::string_view text);
	void setBusy(bool on);
	void syncDefaultsFromBinary();
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, tabs_;
	fuse::ui::NodeHandle expressLib_, expressOut_, expressRun_;
	fuse::ui::NodeHandle arArchive_, arOut_, arRun_;
	fuse::ui::NodeHandle machoFile_, machoOut_, machoRun_;
	fuse::ui::NodeHandle b2pIn_, b2pOut_, b2pRun_;
	fuse::ui::NodeHandle p2yIn_, p2yOut_, p2yRun_;
	fuse::ui::NodeHandle useAsStatic_;
	std::unique_ptr<CodeView> log_;
	std::vector<std::string> logLines_;
	FilePrompt files_;
	model::SigToolRunner runner_;
	ToolResolver tools_;
	std::string activeBinary_;
	fuse::ui::TimerHandle timer_;
	bool timerActive_ = false;
	struct BrowseRow
	{
		fuse::ui::NodeHandle edit;
		FilePrompt::Mode mode;
		std::string title, filter;
	};
	std::vector<BrowseRow> browseRows_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_SIGNATURE_STUDIO_PANEL_H
