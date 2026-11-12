/**
 * @file src/gui/ui/main_window_actions.cpp
 * @brief MainWindow: project management and the File / Tools / Help / View
 *        actions (everything that is not running the decompiler).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/main_window.h"

#include "main_window_internal.h"

#include "retdec/gui/model/cli_tool_paths.h"
#include "retdec/gui/model/decompiler_launch.h"
#include "retdec/gui/model/dlg_help.h"
#include "retdec/gui/model/export_bundle.h"
#include "retdec/gui/model/inspect_model.h"
#include "retdec/gui/model/plugins.h"
#include "retdec/gui/ui/about_dialog.h"
#include "retdec/gui/ui/address_menu.h"
#include "retdec/gui/ui/ai_assistant_panel.h"
#include "retdec/gui/ui/assembly_panel.h"
#include "retdec/gui/ui/batch_decompile_dialog.h"
#include "retdec/gui/ui/binary_browser_panel.h"
#include "retdec/gui/ui/call_graph_panel.h"
#include "retdec/gui/ui/cfg_panel.h"
#include "retdec/gui/ui/command_log_panel.h"
#include "retdec/gui/ui/decompiled_c_panel.h"
#include "retdec/gui/ui/diagnostics_panel.h"
#include "retdec/gui/ui/diff_panel.h"
#include "retdec/gui/ui/docs_viewer.h"
#include "retdec/gui/ui/function_list_panel.h"
#include "retdec/gui/ui/goto_address_dialog.h"
#include "retdec/gui/ui/inspect_panel.h"
#include "retdec/gui/ui/ir_panel.h"
#include "retdec/gui/ui/live_console_panel.h"
#include "retdec/gui/ui/progress_panel.h"
#include "retdec/gui/ui/run_stage_dialog.h"
#include "retdec/gui/ui/settings_dialog.h"
#include "retdec/gui/ui/shortcuts_dialog.h"
#include "retdec/gui/ui/signature_studio_panel.h"
#include "retdec/gui/ui/strings_browser_panel.h"
#include "retdec/gui/ui/target_panel.h"
#include "retdec/gui/ui/tri_pane_code_view.h"
#include "retdec/gui/ui/triage_banner.h"
#include "retdec/gui/ui/type_hierarchy_panel.h"

#include <forge/os/os.hpp>
#include <forge/os/process.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <thread>

namespace retdec::gui::ui {

namespace fs = std::filesystem;
namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

std::string readText(const fs::path& p)
{
	auto r = forge::os::read_file(p.string());
	return r ? std::move(r.value) : std::string();
}

bool endsWithCi(std::string_view s, std::string_view suffix)
{
	if (s.size() < suffix.size()) return false;
	for (std::size_t i = 0; i < suffix.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i]))
		    != std::tolower(static_cast<unsigned char>(suffix[i])))
			return false;
	return true;
}

std::string lowerExt(const fs::path& p)
{
	std::string e = p.extension().string();
	if (!e.empty() && e[0] == '.') e.erase(0, 1);
	std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return e;
}

/// QDesktopServices::openUrl(folder): the desktop's opener, detached.
void openInFileManager(const fs::path& dir)
{
#if defined(_WIN32)
	const std::vector<std::string> argv = {"explorer", dir.string()};
#elif defined(__APPLE__)
	const std::vector<std::string> argv = {"open", dir.string()};
#else
	const std::vector<std::string> argv = {"xdg-open", dir.string()};
#endif
	if (forge::os::find_executable(argv[0]).empty()) return;
	std::thread([argv] {
		forge::os::spawn_spec s;
		s.argv = argv;
		s.capture_stdout = false;
		s.capture_stderr = false;
		(void)forge::os::spawn_wait(s);
	}).detach();
}

} // namespace

// ─── Message boxes / question ────────────────────────────────────────────────

void MainWindow::warn(const std::string& title, const std::string& text)
{
	if (quitWhenDecompileFinishes_)
	{
		diagnostics_->addMessage(model::Severity::Warning, title, text);
		return;
	}
	message_->show(title, text, MessageDialog::Kind::Warning);
}

void MainWindow::info(const std::string& title, const std::string& text)
{
	if (quitWhenDecompileFinishes_) return;
	message_->show(title, text, MessageDialog::Kind::Information);
}

void MainWindow::copyText(const std::string& text, const std::string& what)
{
	if (text.empty())
	{
		showStatusMessage(std::format("No {} yet.", what), 3000);
		return;
	}
	ui_.setClipboardText(text);
	showStatusMessage("Copied " + what, 3000);
}

void MainWindow::askQuestion(const std::string& title, const std::string& text, std::vector<std::string> buttons,
                             std::function<void(int)> onAnswer)
{
	Question& q = *question_;
	q.dlg.setTitle(title);
	q.message = text;
	ui_.setText(q.text, text);
	for (NodeHandle b: q.buttonNodes)
		ui_.destroy(b);
	q.buttonNodes.clear();
	for (std::size_t i = 0; i < buttons.size(); ++i)
	{
		const NodeHandle b = w::button(ui_, q.buttons, buttons[i]);
		if (i == 0) ui_.addClass(b, "primary");
		const int index = int(i);
		ui_.connect(b, SignalKind::Clicked, [this, index](UiContext&, NodeHandle, const SignalArgs&) { answerQuestion(index); });
		q.buttonNodes.push_back(b);
	}
	q.onAnswer = std::move(onAnswer);
	q.dlg.open();
}

bool MainWindow::questionOpen() const
{
	return question_ && question_->dlg.isOpen();
}

std::string MainWindow::questionText() const
{
	return question_ ? question_->message : std::string();
}

void MainWindow::answerQuestion(int index)
{
	if (!questionOpen()) return;
	question_->dlg.close();
	auto cb = std::move(question_->onAnswer);
	question_->onAnswer = nullptr;
	if (cb) cb(index);
}

// ─── Small helpers ───────────────────────────────────────────────────────────

std::string MainWindow::fileDialogStartDir() const
{
	return model::fileDialogStartDir(settings_.general.lastOpenDir);
}

void MainWindow::rememberLastOpenDir(const std::string& filePath)
{
	if (filePath.empty()) return;
	settings_.general.lastOpenDir = fs::absolute(fs::path(filePath)).parent_path().string();
}

fs::path MainWindow::guessDecompiledCPath() const
{
	if (!project_) return {};
	std::error_code ec;
	if (!project_->decompiledPath().empty() && fs::exists(project_->decompiledPath(), ec))
		return fs::absolute(project_->decompiledPath());
	return model::locateGuiDecompiledCPath(project_->binaryPath(), settings_.decompiler.decompileOutputDir);
}

// ─── Project management ──────────────────────────────────────────────────────

void MainWindow::resetDecompileUiForOpen()
{
	loadedArtifacts_.reset();
	shownFunction_ = ~uint64_t(0);
	setStatusFunctionCount(-1);
	setStatusDecompileState("");
	functionList_->clear();
	decompiledC_->clear();
	assembly_->clear();
	ir_->clear();
	cfg_->clear();
	triPane_->clear();
	strings_->clear();
	callGraph_->clear();
	typeHierarchy_->clear();
	diagnostics_->clear();
	commandLog_->clear();
	console_->clear();
	progress_->resetAll();
	ai_->clear();
}

void MainWindow::openBinary(const std::string& path)
{
	const std::string absPath = fs::absolute(fs::path(path)).lexically_normal().string();
	setTitle("RetDec \xE2\x80\x94 " + absPath);
	project_ = std::make_unique<model::ProjectFile>(absPath);
	lastProjectSavePath_.clear();
	resetDecompileUiForOpen();
	setStatusFile(absPath);
	addRecentFile(absPath);
	settings_.general.lastBinaryPath = absPath;
	settings_.general.lastOpenDir = fs::path(absPath).parent_path().string();
	(void)settings_.save();
	binary_->loadBinary(absPath);
	target_->setFromProject(project_.get());
	sigStudio_->setActiveBinary(absPath);
	triage_->setBinary(absPath);
	triage_->setActionsEnabled(true);
	bannerWanted_ = true;
	setActionChecked("view.show_banner", true);
	updateProjectFileActions();
	updateCentralEmptyState();
	updateRedecompileEnabled();
	// Cache reuse: a large binary takes minutes to decompile; reopening it must not.
	if (!tryLoadCachedDecompile(absPath)) setStatusDecompileState("not decompiled");
	// Always run fileinfo so the Binary browser gets its section table (not headless).
	if (!quitWhenDecompileFinishes_ && !opts_.headless)
		inspect_->runFileinfo(absPath, model::resolveRetdecFileinfoExecutable().string());
}

bool MainWindow::openProject(const std::string& path)
{
	auto pf = std::make_unique<model::ProjectFile>();
	if (auto r = pf->load(path); !r)
	{
		warn("Open Project", "Failed to load project:\n" + r.error());
		return false;
	}
	project_ = std::move(pf);
	lastProjectSavePath_ = fs::absolute(fs::path(path)).string();
	resetDecompileUiForOpen();
	const std::string bin = project_->binaryPath();
	setTitle("RetDec \xE2\x80\x94 " + bin);
	setStatusFile(bin);
	addRecentFile(lastProjectSavePath_);
	binary_->loadBinary(bin);
	target_->setFromProject(project_.get());
	sigStudio_->setActiveBinary(bin);
	bool anyDone = false;
	for (const std::string& name: project_->stageNames())
	{
		const std::string st = project_->stageStatus(name);
		if (st == "done")
		{
			progress_->setStageState(name, model::StageState::Done);
			anyDone = true;
		}
		else if (st == "running")
			progress_->setStageState(name, model::StageState::Running);
	}
	if (anyDone) setOutputTabVisible(kOutProgress, true);
	triage_->setBinary(bin);
	triage_->setActionsEnabled(true);
	bannerWanted_ = true;
	setActionChecked("view.show_banner", true);
	updateProjectFileActions();
	updateCentralEmptyState();
	functionList_->setNameFilterText(project_->functionListFilter());
	callGraph_->setCallGraphDepth(project_->callGraphDepth());
	if (!tryLoadCachedDecompile(bin))
	{
		setStatusDecompileState("not decompiled");
		if (!quitWhenDecompileFinishes_ && !opts_.headless)
			inspect_->runFileinfo(bin, model::resolveRetdecFileinfoExecutable().string());
	}
	// Loading the project is not a modification.
	project_->clearModified();
	return true;
}

bool MainWindow::saveProject(const std::string& path)
{
	if (!project_) return false;
	project_->setFunctionListFilter(functionList_->nameFilterText());
	project_->setCallGraphDepth(callGraph_->callGraphDepth());
	for (const model::FunctionEntry& e: functionList_->functions())
	{
		const auto existing = project_->annotation(e.address);
		const bool nameUnchanged = e.name.empty() || e.name == e.rawName;
		const bool emptyAnn = e.notes.empty() && e.tags.empty() && e.signature.empty() && nameUnchanged;
		if (emptyAnn)
		{
			if (existing) project_->clearAnnotation(e.address);
			continue;
		}
		auto ann = existing.value_or(model::ProjectFile::Annotation{});
		if (!e.name.empty()) ann.name = e.name;
		ann.comment = e.notes;
		ann.tags = e.tags;
		ann.signatureOverride = e.signature;
		project_->setAnnotation(e.address, ann);
	}
	if (auto r = project_->save(path); !r)
	{
		warn("Save Project", "Failed to save:\n" + r.error());
		return false;
	}
	return true;
}

void MainWindow::openPath(const std::string& path)
{
	if (endsWithCi(path, ".retdec"))
		openProject(path);
	else
		openBinary(path);
}

bool MainWindow::dropFiles(const std::vector<std::string>& paths)
{
	for (const std::string& p: paths)
	{
		if (p.empty()) continue;
		std::error_code ec;
		if (!fs::is_regular_file(p, ec))
		{
			showStatusMessage("Skipped (not a file): " + p, 4000);
			continue;
		}
		openPath(p);
		return true;
	}
	return false;
}

// ─── File menu ───────────────────────────────────────────────────────────────

void MainWindow::onOpenBinary()
{
	// All files: most ELF binaries have no extension.
	files_->show(FilePrompt::Mode::Open, "Open Binary", fileDialogStartDir(), {}, [this](const std::string& path) {
		if (path.empty()) return;
		openBinary(path);
	});
}

void MainWindow::onOpenProject()
{
	files_->show(FilePrompt::Mode::Open, "Open RetDec Project", fileDialogStartDir(), "*.retdec",
	             [this](const std::string& path) {
		             if (path.empty()) return;
		             rememberLastOpenDir(path);
		             openProject(path);
	             });
}

void MainWindow::onSaveProject()
{
	if (!project_) return;
	if (lastProjectSavePath_.empty())
	{
		onSaveProjectAs();
		return;
	}
	if (saveProject(lastProjectSavePath_)) showStatusMessage("Project saved: " + lastProjectSavePath_, 5000);
}

void MainWindow::onSaveProjectAs()
{
	if (!project_) return;
	std::string start = lastProjectSavePath_;
	if (start.empty())
	{
		const fs::path b(project_->binaryPath());
		start = (b.parent_path() / (b.stem().string() + ".retdec")).string();
	}
	files_->show(FilePrompt::Mode::Save, "Save RetDec Project", start, "*.retdec", [this](const std::string& chosen) {
		if (chosen.empty()) return;
		std::string path = chosen;
		if (!endsWithCi(path, ".retdec")) path += ".retdec";
		if (saveProject(path))
		{
			lastProjectSavePath_ = fs::absolute(path).string();
			showStatusMessage("Project saved: " + lastProjectSavePath_, 5000);
		}
	});
}

void MainWindow::onSaveDecompiled()
{
	decompiledC_->saveAs();
}

void MainWindow::onExportCMake()
{
	if (!project_)
	{
		info("Export CMake", "Open a binary or project first.");
		return;
	}
	const fs::path cAbs = guessDecompiledCPath();
	std::error_code ec;
	if (cAbs.empty() || !fs::exists(cAbs, ec))
	{
		info("Export CMake", std::format("No decompiled C file found:\n{}\n\nRun Analysis \xE2\x86\x92 Run Full Analysis first.",
		                                 cAbs.string()));
		return;
	}
	const std::string defaultCmake = (fs::absolute(cAbs).parent_path() / "CMakeLists.txt").string();
	files_->show(FilePrompt::Mode::Save, "Export CMakeLists.txt", defaultCmake, {}, [this, cAbs](const std::string& path) {
		if (path.empty()) return;
		const fs::path cmakeDir = fs::absolute(path).parent_path();
		std::string rel = fs::absolute(cAbs).lexically_relative(cmakeDir).generic_string();
		if (rel.empty()) rel = fs::absolute(cAbs).generic_string();
		std::string target = cAbs.stem().string();
		for (char& c: target)
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
		if (target.empty() || target[0] == '_') target = "retdec_recovered";
		if (std::isdigit(static_cast<unsigned char>(target[0]))) target = "t_" + target;
		const std::string content = std::format(
			"# Skeleton to build recovered C produced by RetDec \xE2\x80\x94 adjust flags and toolchain.\n"
			"cmake_minimum_required(VERSION 3.16)\n"
			"project({0} LANGUAGES C)\n"
			"add_executable({0} \"{1}\")\n"
			"target_compile_features({0} PRIVATE c_std_11)\n",
			target, rel);
		if (auto r = forge::os::write_file(path, content); !r)
		{
			warn("Export CMake", r.err.what);
			return;
		}
		showStatusMessage("Wrote " + path, 5000);
	});
}

void MainWindow::onExportDecompileBundle()
{
	const bool hasLoaded = loadedArtifacts_.has_value();
	const fs::path cGuess = guessDecompiledCPath();
	std::error_code ec;
	const bool hasOnDisk = !cGuess.empty() && fs::exists(cGuess, ec);
	if (!hasLoaded && !hasOnDisk)
	{
		info("Export Decompile Bundle", "No decompiled artifacts found.\n\nRun Analysis \xE2\x86\x92 Run Full Analysis first.");
		return;
	}
	const fs::path cPath = (hasLoaded && !loadedArtifacts_->cPath.empty()) ? loadedArtifacts_->cPath : cGuess;
	const std::string defaultZip = (fs::absolute(cPath).parent_path() / (cPath.stem().string() + "-bundle.zip")).string();
	files_->show(FilePrompt::Mode::Save, "Export Decompile Bundle", defaultZip, "*.zip", [this, cPath](const std::string& zip) {
		if (zip.empty()) return;
		model::DecompileBundleInput in;
		in.cPath = cPath;
		in.decompilerExe = lastDecompilerExe_;
		in.decompilerArgs = lastDecompilerArgs_;
		in.decompilerCwd = lastDecompilerCwd_;
		std::error_code e;
		if (run_ && fs::exists(run_->logPath(), e))
			in.logFilePath = run_->logPath();
		else if (!console_->isEmpty())
			in.logInline = console_->text();
		if (auto r = model::exportDecompileBundle(in, zip); !r)
		{
			warn("Export Decompile Bundle", r.error());
			return;
		}
		showStatusMessage("Exported bundle " + zip, 5000);
	});
}

void MainWindow::onExportThreatIntel()
{
	const bool hasLoaded = loadedArtifacts_.has_value();
	const fs::path cGuess = guessDecompiledCPath();
	std::error_code ec;
	const bool hasOnDisk = !cGuess.empty() && fs::exists(cGuess, ec);
	if (!hasLoaded && !hasOnDisk)
	{
		info("Export Threat Intel", "No decompiled artifacts found.\n\nRun Analysis \xE2\x86\x92 Run Full Analysis first.");
		return;
	}
	if (intelRun_ && intelRun_->running())
	{
		showStatusMessage("Threat intel export already running", 3000);
		return;
	}
	const fs::path script = model::resolveRetdecExportIntelScript();
	const fs::path python = model::resolvePythonInterpreter();
	if (script.empty() || python.empty())
	{
		warn("Export Threat Intel", "Could not locate retdec_export_intel.py or a Python interpreter.\n"
		                            "Use a full RetDec install with scripts/ on disk.");
		return;
	}
	const fs::path cPath = (hasLoaded && !loadedArtifacts_->cPath.empty()) ? loadedArtifacts_->cPath : cGuess;
	const fs::path configPath = model::pathsFromOutputC(cPath).configPath;
	if (!fs::exists(configPath, ec))
	{
		info("Export Threat Intel", "Missing decompiler config sidecar:\n" + configPath.string());
		return;
	}
	const std::string defaultJson = (fs::absolute(cPath).parent_path() / (cPath.stem().string() + "-intel.json")).string();
	files_->show(FilePrompt::Mode::Save, "Export Threat Intel", defaultJson, "*.json",
	             [this, script, python, cPath, configPath](const std::string& outPath) {
		             if (outPath.empty()) return;
		             std::vector<std::string> args = {script.string(), "--c", cPath.string(), "--config",
		                                              configPath.string(), "-o", outPath};
		             if (project_ && !project_->binaryPath().empty())
		             {
			             args.push_back("--binary");
			             args.push_back(project_->binaryPath());
		             }
		             intelFileinfo_.reset();
		             if (!inspect_->fileinfoJson().empty())
		             {
			             std::error_code e;
			             const fs::path tmp = fs::temp_directory_path(e)
			                                  / std::format("retdec-fileinfo-{}.json", forge::os::pid());
			             if (forge::os::write_file(tmp.string(), inspect_->fileinfoJson()))
			             {
				             intelFileinfo_ = std::make_unique<model::TemporaryFile>(tmp);
				             args.push_back("--fileinfo");
				             args.push_back(tmp.string());
			             }
		             }
		             model::DecompileRunSpec spec;
		             spec.executable = python;
		             spec.arguments = args;
		             spec.workingDirectory = script.parent_path();
		             spec.timeoutMs = 300000; // the Qt kExternalScriptWaitMs ceiling
		             intelRun_ = std::make_unique<model::DecompileRun>();
		             console_->appendBanner("retdec_export_intel", args, spec.workingDirectory.string());
		             if (auto r = intelRun_->start(spec); !r)
		             {
			             warn("Export Threat Intel", r.error());
			             intelRun_.reset();
			             return;
		             }
		             intelOutPath_ = outPath;
		             showStatusMessage("Exporting threat intel\xE2\x80\xA6");
		             if (intelTimerOn_) ui_.cancelTimer(intelTimer_);
		             intelTimerOn_ = true;
		             intelTimer_ = ui_.startTimer(0.2f, true, [this](UiContext&) {
			             if (!intelRun_ || !intelRun_->poll()) return;
			             ui_.cancelTimer(intelTimer_);
			             intelTimerOn_ = false;
			             const auto log = model::readDecompilerLogForConsole(intelRun_->logPath());
			             if (!log.text.empty()) console_->appendChunk(LiveConsolePanel::Stream::Stdout, log.text);
			             const int code = intelRun_->exitCode().value_or(-1);
			             const int64_t ms = intelRun_->elapsed().count();
			             console_->appendFooter("retdec_export_intel", code, ms);
			             commandLog_->appendRun("retdec_export_intel", intelRun_->arguments(),
			                                    intelRun_->workingDirectory().string(), code, ms,
			                                    model::readDecompilerLogTail(intelRun_->logPath()));
			             const bool timedOut = intelRun_->timedOut();
			             const auto state = intelRun_->state();
			             const std::string err = intelRun_->errorString();
			             intelRun_.reset();
			             intelFileinfo_.reset();
			             if (state == model::DecompileRun::State::FailedToStart)
				             warn("Export Threat Intel", err);
			             else if (timedOut)
				             warn("Export Threat Intel",
				                  "Export did not finish within 300 s and was stopped.\nSee Console for details.");
			             else if (code != 0)
				             warn("Export Threat Intel", std::format("Export failed (exit {}).\nSee Console for details.", code));
			             else
				             showStatusMessage("Exported threat intel " + intelOutPath_, 5000);
		             });
	             });
}

void MainWindow::onExportAs(const std::string& pluginId)
{
	const model::Plugin* plug = model::PluginManager::instance().findPlugin(pluginId);
	if (!plug) return;
	const std::string text = decompiledC_->documentText();
	if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }))
	{
		info("Export As", "Decompiled C is empty.");
		return;
	}
	const std::string ext = plug->fileExtension();
	files_->show(FilePrompt::Mode::Save, "Export As", fileDialogStartDir(), ext.empty() ? std::string() : "*" + ext,
	             [this, pluginId](const std::string& path) {
		             if (path.empty()) return;
		             const model::Plugin* p = model::PluginManager::instance().findPlugin(pluginId);
		             if (!p) return; // unloaded while the dialog was open
		             auto out = p->transform(decompiledC_->documentText());
		             if (!out)
		             {
			             warn("Export As", out.error());
			             return;
		             }
		             if (auto r = forge::os::write_file(path, *out); !r)
		             {
			             warn("Export As", r.err.what);
			             return;
		             }
		             rememberLastOpenDir(path);
		             showStatusMessage("Exported " + path, 5000);
	             });
}

void MainWindow::onBatchDecompile()
{
	if (quitWhenDecompileFinishes_) return;
	if (batchRunning_)
	{
		showStatusMessage("Batch decompile already running", 2500);
		return;
	}
	if (decompilerRunning())
	{
		showStatusMessage("Decompiler already running", 2500);
		return;
	}
	batchDlg_->show([this](const std::vector<std::string>& paths) {
		if (!paths.empty()) startBatchDecompile(paths);
	}, fileDialogStartDir());
}

// ─── Analysis menu ───────────────────────────────────────────────────────────

void MainWindow::onRunStage()
{
	auto choose = [this] {
		runStageDlg_->show([this](model::RunStageChoice c) {
			using C = model::RunStageChoice;
			switch (c)
			{
			case C::Decompile: runFullAnalysis(); return;
			case C::FastDecompile: {
				const bool prev = fastDecompile_;
				fastDecompile_ = true;
				runFullAnalysis();
				fastDecompile_ = prev;
				return;
			}
			case C::Unpacker:
				inspect_->runUnpacker();
				focusOutputTab(kOutConsole);
				return;
			case C::SignatureStudio: onShowSignatureStudio(); return;
			case C::Fileinfo: {
				const fs::path fi = model::resolveRetdecFileinfoExecutable();
				if (fi.empty())
				{
					warn("fileinfo", "retdec-fileinfo was not found next to the GUI or on PATH.");
					return;
				}
				if (project_) inspect_->runFileinfo(project_->binaryPath(), fi.string());
				focusOutputTab(kOutConsole);
				return;
			}
			}
		});
	};
	if (project_)
	{
		choose();
		return;
	}
	files_->show(FilePrompt::Mode::Open, "Open Binary", fileDialogStartDir(), {}, [this, choose](const std::string& path) {
		if (path.empty()) return;
		openBinary(path);
		if (project_) choose();
	});
}

void MainWindow::onConfigure()
{
	std::string start = settings_.decompiler.extraConfigPath;
	if (start.empty() && project_) start = fs::path(project_->binaryPath()).parent_path().string();
	files_->show(FilePrompt::Mode::Open, "Decompiler configuration JSON", start, model::kJsonFileFilter,
	             [this](const std::string& path) {
		             if (path.empty()) return;
		             settings_.decompiler.extraConfigPath = path;
		             (void)settings_.save();
		             settings_.notifySettingsChanged();
		             showStatusMessage("Decompiler --config: " + path, 5000);
	             });
}

// ─── Tools menu ──────────────────────────────────────────────────────────────

void MainWindow::onSettings()
{
	settingsDlg_->show();
}

void MainWindow::onCompare()
{
	const std::string left = decompiledC_->documentText();
	if (std::all_of(left.begin(), left.end(), [](unsigned char c) { return std::isspace(c); }))
	{
		info("Compare", "The Decompiled C view is empty. Run decompilation first.");
		return;
	}
	files_->show(FilePrompt::Mode::Open, "Compare with file", fileDialogStartDir(), "*.c;*.cpp;*.h;*.hpp",
	             [this](const std::string& rightPath) {
		             if (rightPath.empty()) return;
		             rememberLastOpenDir(rightPath);
		             auto r = forge::os::read_file(rightPath);
		             if (!r)
		             {
			             warn("Compare", r.err.what);
			             return;
		             }
		             compare_->setDiff(decompiledC_->documentText(), r.value, fs::path(rightPath).filename().string());
		             toolWindow("Compare", 1100.f, 700.f).dlg->setTitle("Compare \xE2\x80\x94 Decompiled C vs file");
		             showToolWindow("Compare");
	             });
}

void MainWindow::onCompareRefined()
{
	fs::path orig = decompilerOutputPath_;
	if (orig.empty() && project_)
		orig = model::locateGuiDecompiledCPath(fs::absolute(project_->binaryPath()), settings_.decompiler.decompileOutputDir);
	const fs::path refined = orig.string() + ".refined.c";
	std::error_code ec;
	if (orig.empty() || !fs::exists(orig, ec) || !fs::exists(refined, ec))
	{
		showStatusMessage("Need both the decompiled .c and its .refined.c sidecar.", 4000);
		return;
	}
	auto l = forge::os::read_file(orig.string());
	auto r = forge::os::read_file(refined.string());
	if (!l || !r)
	{
		showStatusMessage("Could not read C files for compare.", 4000);
		return;
	}
	compare_->addStage("Original vs refined", l.value, r.value);
	toolWindow("Compare", 1100.f, 700.f).dlg->setTitle("Compare \xE2\x80\x94 original vs refined");
	showToolWindow("Compare");
}

void MainWindow::onCopyFunctionC()
{
	if (!loadedArtifacts_)
	{
		showStatusMessage("Load a decompile first, then select a function.", 4000);
		return;
	}
	const auto entry = functionList_->selectedEntry();
	if (!entry)
	{
		showStatusMessage("Select a function in the Functions list.", 4000);
		return;
	}
	std::string snippet = model::extractCForFunction(loadedArtifacts_->cPath, entry->startLine, entry->endLine,
	                                                 entry->name.empty() ? entry->rawName : entry->name);
	auto blank = [](const std::string& s) {
		return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
	};
	if (blank(snippet))
	{
		const std::string all = decompiledC_->documentText();
		snippet = model::extractCForFunctionByName(all, entry->name.empty() ? entry->rawName : entry->name);
	}
	if (blank(snippet))
	{
		showStatusMessage("Could not extract C for this function.", 4000);
		return;
	}
	ui_.setClipboardText(snippet);
	showStatusMessage("Copied " + entry->name, 3000);
}

void MainWindow::onShowSignatureStudio()
{
	showToolWindow("Signature Studio");
}

void MainWindow::onShowCallGraph()
{
	if (loadedArtifacts_ && callGraph_->graph().nodes().empty() && !loadedArtifacts_->callGraphNodes.empty())
		callGraph_->setCallGraph(loadedArtifacts_->callGraphNodes, loadedArtifacts_->callGraphEdges);
	showToolWindow("Call Graph");
}

void MainWindow::onShowTypeHierarchy()
{
	showToolWindow("Type Hierarchy");
	refreshTypeHierarchyFromArtifacts();
}

void MainWindow::onShowAiAssistant()
{
	showToolWindow("AI Assistant");
	ai_->applyMlSettingsFromApp();
}

void MainWindow::onShowVisualisationPlugin(const std::string& pluginId)
{
	const model::Plugin* plug = model::PluginManager::instance().findPlugin(pluginId);
	if (!plug) return;
	const std::string title = plug->panelTitle().empty() ? plug->metadata().name : plug->panelTitle();
	ToolWindow& tw = toolWindow(title, 700.f, 500.f);
	tw.pluginId = pluginId;
	model::PipelineContext ctx;
	ctx.inputBinaryPath = project_ ? project_->binaryPath() : std::string();
	ctx.decompiledText = decompiledC_->documentText();
	if (loadedArtifacts_)
	{
		ctx.irText = loadedArtifacts_->fullLl;
		ctx.asmText = loadedArtifacts_->fullDsm;
		ctx.analysisComplete = true;
	}
	tw.pluginText = plug->renderPanel(ctx);
	if (!ui_.alive(tw.pluginView))
	{
		const NodeHandle scroll = w::scrollArea(ui_, tw.body);
		ui_.setCss(scroll, "flex-grow", "1");
		tw.pluginView = w::markdownView(ui_, scroll, tw.pluginText);
	}
	else
		w::setMarkdown(ui_, tw.pluginView, tw.pluginText);
	showToolWindow(title);
}

void MainWindow::dropPluginUi(const std::string& id)
{
	// The panel's text came from the plugin; nothing of its code stays behind,
	// but its window must not outlive it (Qt deleted the panel and its host).
	for (auto it = toolWindows_.begin(); it != toolWindows_.end(); ++it)
	{
		if ((*it)->pluginId != id) continue;
		if ((*it)->dlg->isOpen()) (*it)->dlg->close();
		toolWindows_.erase(it);
		break;
	}
}

void MainWindow::pluginsChanged()
{
	// Export As / Visualisation submenus are built when the menu opens; an open
	// popup still lists the old set, so close it.
	openMenu_.runs.clear();
}

// ─── Help menu ───────────────────────────────────────────────────────────────

void MainWindow::onOpenDocumentation()
{
	docsDlg_->show();
}

void MainWindow::onKeyboardShortcuts()
{
	shortcutsDlg_->show();
}

void MainWindow::onAbout()
{
	aboutDlg_->show();
}

// ─── View menu ───────────────────────────────────────────────────────────────

void MainWindow::onGotoAddress()
{
	gotoDlg_->show([this](uint64_t addr) { navigateToAddress(addr, kDocAssembly); });
}

void MainWindow::onGotoLine()
{
	gotoLineDlg_->show([this](int line) {
		raiseDocumentTab(kDocDecompiledC);
		decompiledC_->scrollToFunction("", line, line);
	});
}

// ─── Triage banner ───────────────────────────────────────────────────────────

void MainWindow::onTriageMoreActions(Vec2 anchor)
{
	std::vector<MenuAction> items;
	items.push_back({"Re-run fileinfo", project_ != nullptr, [this] {
		                 if (project_)
			                 inspect_->runFileinfo(project_->binaryPath(), model::resolveRetdecFileinfoExecutable().string());
	                 }});
	items.push_back({"Open in Inspect panel", true, [this] { focusWorkspaceTab(kWsInspect); }});
	items.push_back({"Configure decompiler\xE2\x80\xA6", true, [this] { onConfigure(); }});
	const auto& fi = inspect_->fileinfo();
	const std::string sha = fi ? fi->sha256 : std::string();
	const std::string md5 = fi ? fi->md5 : std::string();
	const std::string crc = fi ? fi->crc32 : std::string();
	items.push_back({"Copy SHA-256", true, [this, sha] { copyText(sha, "SHA-256"); }});
	items.push_back({"Copy MD5", true, [this, md5] { copyText(md5, "MD5"); }});
	items.push_back({"Copy CRC32", true, [this, crc] { copyText(crc, "CRC32"); }});
	const std::string all = fi ? model::formatHashesForClipboard(*fi) : std::string();
	items.push_back({"Copy all hashes", true, [this, all] {
		                 if (all.empty())
			                 showStatusMessage("No fileinfo hashes yet.", 3000);
		                 else
			                 copyText(all, "file hashes");
	                 }});
	if (project_ && !project_->binaryPath().empty())
	{
		const std::string p = project_->binaryPath();
		items.push_back({"Copy binary path", true, [this, p] { copyText(p, "binary path"); }});
	}
	items.push_back({"Open backend graph files\xE2\x80\xA6", true, [this] {
		                 if (decompilerOutputPath_.empty())
		                 {
			                 showStatusMessage("No decompile output yet.", 3000);
			                 return;
		                 }
		                 const fs::path dir = fs::absolute(decompilerOutputPath_).parent_path();
		                 const std::string stem = decompilerOutputPath_.stem().string();
		                 int dots = 0;
		                 std::error_code ec;
		                 for (const auto& de: fs::directory_iterator(dir, ec))
		                 {
			                 const std::string n = de.path().filename().string();
			                 if (n == stem + ".cg.dot" || (n.starts_with(stem + ".cfg.") && n.ends_with(".dot"))) ++dots;
		                 }
		                 if (dots == 0)
		                 {
			                 showStatusMessage("No backend CFG/CG DOT files next to the decompiled C.", 4000);
			                 return;
		                 }
		                 openInFileManager(dir);
		                 showStatusMessage(std::format("Opened {} ({} graph file(s))", dir.string(), dots), 4000);
	                 }});
	triageMenu_->open(root_, anchor, std::move(items));
}

void MainWindow::refreshTriageFromInspect()
{
	if (!project_) return;
	const auto& fi = inspect_->fileinfo();
	std::string arch = fi ? fi->arch : std::string();
	if (arch.empty()) arch = project_->arch();
	std::string os = fi ? fi->os : std::string();
	if (os.empty()) os = project_->os();
	std::error_code ec;
	const auto size = fs::file_size(project_->binaryPath(), ec);
	const std::string packer = fi ? fi->packer : std::string();
	std::string format = fi ? fi->format : std::string();
	if (format.empty())
	{
		const std::string ext = lowerExt(project_->binaryPath());
		if (ext == "exe" || ext == "dll")
			format = "PE";
		else if (ext == "so" || ext == "elf")
			format = "ELF";
		else if (ext == "dylib")
			format = "Mach-O";
	}
	if (project_->arch().empty() && !arch.empty()) project_->setArch(arch);
	if (project_->os().empty() && !os.empty()) project_->setOs(os);
	if (!project_->arch().empty() || !project_->os().empty()) target_->setFromProject(project_.get());
	triage_->setMetadata(format, arch, os, ec ? 0 : int64_t(size), packer);
	if (fi && !fi->sha256.empty()) showStatusMessage("SHA-256 " + fi->sha256.substr(0, 16) + "\xE2\x80\xA6", 6000);
}

void MainWindow::refreshTypeHierarchyFromArtifacts()
{
	if (!loadedArtifacts_) return;
	if (!settings_.analysis.enableCxxLifter || !settings_.recovery.detectRTTI)
	{
		typeHierarchy_->setHierarchy(std::vector<model::ClassInfo>{});
		return;
	}
	typeHierarchy_->setHierarchy(loadedArtifacts_->typeHierarchyClasses);
}

// ─── Settings applied live ───────────────────────────────────────────────────

void MainWindow::applyEditorFont()
{
	const auto& g = settings_.general;
	const float px = editorFontPx(g.fontSize);
	decompiledC_->applyEditorFont(px);
	assembly_->applyEditorFont(px);
	ir_->applyEditorFont(px);
	triPane_->applyEditorFont(px);
	for (CodeView* v: {&decompiledC_->view(), &assembly_->view(), &ir_->view(), &triPane_->asmPane().editor(),
	                   &triPane_->irPane().editor(), &triPane_->cPane().editor()})
		v->setShowGutter(g.showLineNumbers);
}

void MainWindow::applySettings()
{
	if (opts_.applyTheme)
	{
		const ThemeKind want = themeKindFor(settings_.general.theme);
		const bool first = !ui_.hasClass(ui_.root(), "retdec-theme-mocha") && !ui_.hasClass(ui_.root(), "retdec-theme-latte");
		if (first || want != theme_)
		{
			if (auto r = applyTheme(ui_, want))
				theme_ = *r;
			else
				diagnostics_->addMessage(model::Severity::Warning, "theme", r.error());
		}
		const CodePalette pal = codePaletteFor(theme_);
		for (CodeView* v: {&decompiledC_->view(), &assembly_->view(), &ir_->view(), &triPane_->asmPane().editor(),
		                   &triPane_->irPane().editor(), &triPane_->cPane().editor(), &console_->view(),
		                   &commandLog_->view(), &binary_->hexView()})
			v->setPalette(pal);
	}
	applyEditorFont();
	target_->syncDecompilerConfigFromAppSettings();
	triPane_->syncLanguageFromSettings(settings_.decompiler.outputLang);
	functionList_->setDemangleNames(settings_.advanced.demangleNames);
	progress_->setAnalysisBudgetSecs(settings_.analysis.maxAnalysisTimeSecs);
	std::error_code ec;
	const bool model = !settings_.ml.modelPath.empty() && fs::is_regular_file(settings_.ml.modelPath, ec);
	ui_.setText(statusNeural_, model ? "Neural: ready" : "Neural: off");
}

// ─── Quit ────────────────────────────────────────────────────────────────────

void MainWindow::requestQuit()
{
	auto afterRun = [this] {
		if (project_ && project_->isModified())
		{
			askQuestion("Unsaved Changes", "You have unsaved project changes. Save before quitting?",
			            {"Save", "Discard", "Cancel"}, [this](int b) {
				            if (b == 2) return;
				            if (b == 1)
				            {
					            finishQuit();
					            return;
				            }
				            if (!lastProjectSavePath_.empty())
				            {
					            if (saveProject(lastProjectSavePath_)) finishQuit();
					            return;
				            }
				            const fs::path bin(project_->binaryPath());
				            const std::string start = (bin.parent_path() / (bin.stem().string() + ".retdec")).string();
				            files_->show(FilePrompt::Mode::Save, "Save Project", start, "*.retdec",
				                         [this](const std::string& chosen) {
					                         if (chosen.empty()) return;
					                         std::string path = chosen;
					                         if (!endsWithCi(path, ".retdec")) path += ".retdec";
					                         if (!saveProject(path)) return;
					                         lastProjectSavePath_ = fs::absolute(path).string();
					                         finishQuit();
				                         });
			            });
			return;
		}
		finishQuit();
	};
	if (analysisRunning_)
	{
		askQuestion("Quit", "A decompilation or analysis run is in progress. Quit anyway?", {"Yes", "No"},
		            [this, afterRun](int b) {
			            if (b != 0) return;
			            stopAnalysis();
			            afterRun();
		            });
		return;
	}
	afterRun();
}

void MainWindow::finishQuit()
{
	quitting_ = true;
	saveLayout();
	if (onQuitRequested) onQuitRequested(0);
}

void MainWindow::shutdown()
{
	if (shutDown_) return;
	shutDown_ = true;
	if (run_ && run_->running()) stopAnalysis();
	if (intelRun_) intelRun_->cancel();
	saveLayout();
}

} // namespace retdec::gui::ui
