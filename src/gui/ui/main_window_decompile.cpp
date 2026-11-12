/**
 * @file src/gui/ui/main_window_decompile.cpp
 * @brief MainWindow: running retdec-decompiler (single, range, selected
 *        function, batch), polling its log, loading the artefacts and
 *        populating the panels.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/main_window.h"

#include "main_window_internal.h"

#include "retdec/gui/model/batch_decompile_queue.h"
#include "retdec/gui/model/cli_tool_paths.h"
#include "retdec/gui/model/decompiler_launch.h"
#include "retdec/gui/model/plugins.h"
#include "retdec/gui/ui/ai_assistant_panel.h"
#include "retdec/gui/ui/assembly_panel.h"
#include "retdec/gui/ui/call_graph_panel.h"
#include "retdec/gui/ui/cfg_panel.h"
#include "retdec/gui/ui/command_log_panel.h"
#include "retdec/gui/ui/decompiled_c_panel.h"
#include "retdec/gui/ui/diagnostics_panel.h"
#include "retdec/gui/ui/diff_panel.h"
#include "retdec/gui/ui/function_list_panel.h"
#include "retdec/gui/ui/ir_panel.h"
#include "retdec/gui/ui/live_console_panel.h"
#include "retdec/gui/ui/progress_panel.h"
#include "retdec/gui/ui/strings_browser_panel.h"
#include "retdec/gui/ui/tri_pane_code_view.h"
#include "retdec/gui/ui/type_hierarchy_panel.h"

#include <forge/json/json.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>

namespace retdec::gui::ui {

namespace fs = std::filesystem;
using namespace fuse::ui;
using model::Severity;

namespace {

constexpr float kPollSeconds = 0.1f;   // the Qt poll was 500 ms; finishing is noticed sooner
constexpr int kProgressEveryTicks = 5; // log progress / console tail every 500 ms, as Qt

int64_t msSince(std::chrono::steady_clock::time_point t)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

std::string readText(const fs::path& p)
{
	auto r = forge::os::read_file(p.string());
	return r ? std::move(r.value) : std::string();
}

std::optional<forge::json::value> parseJson(std::string_view text)
{
	try
	{
		return forge::json::parse(text);
	}
	catch (...)
	{
		return std::nullopt;
	}
}

} // namespace

// ─── Lifecycle ───────────────────────────────────────────────────────────────

bool MainWindow::decompilerRunning() const
{
	return run_ && run_->running();
}

void MainWindow::startElapsedTimer()
{
	wallStart_ = std::chrono::steady_clock::now();
	if (elapsedTimerOn_) ui_.cancelTimer(elapsedTimer_);
	elapsedTimerOn_ = true;
	elapsedTimer_ = ui_.startTimer(0.5f, true, [this](UiContext&) { updateElapsedTime(); });
	updateElapsedTime();
}

void MainWindow::stopElapsedTimer()
{
	if (elapsedTimerOn_) ui_.cancelTimer(elapsedTimer_);
	elapsedTimerOn_ = false;
}

void MainWindow::stopPollTimer()
{
	if (pollTimerOn_) ui_.cancelTimer(pollTimer_);
	pollTimerOn_ = false;
}

void MainWindow::updateElapsedTime()
{
	const int64_t ms = msSince(wallStart_);
	ui_.setText(
		statusElapsed_,
		ms < 60000 ? std::format("{}.{}s", ms / 1000, (ms % 1000) / 100)
				   : std::format("{}m {}s", ms / 60000, (ms % 60000) / 1000));
}

void MainWindow::startAnalysis()
{
	if (analysisRunning_ || !project_) return;
	analysisRunning_ = true;
	setActionEnabled("analysis.run_full", false);
	setActionEnabled("analysis.stop", true);
	progressShown_ = true;
	setShown(ui_, statusProgress_, true);
	setAnalysisProgress(0);
	fuse::ui::widgets::setProgressIndeterminate(ui_, statusProgress_, true);
	startElapsedTimer();
	progress_->resetAll();
	setStatusStage("Running\xE2\x80\xA6");
	setOutputTabVisible(kOutProgress, true);
	focusOutputTab(kOutProgress);
}

void MainWindow::stopAnalysis()
{
	const bool stopBatch = batchRunning_;
	if (run_ && run_->running()) run_->cancel();
	const bool hadRun = static_cast<bool>(run_);
	if (hadRun && run_->state() == model::DecompileRun::State::Cancelled)
	{
		console_->appendFooter("retdec-decompiler", -1, run_->elapsed().count());
		commandLog_->appendRun(
			"retdec-decompiler", lastDecompilerArgs_, lastDecompilerCwd_, -1, run_->elapsed().count(), "(stopped)");
	}
	stopPollTimer();
	logProgressOffset_ = logConsoleOffset_ = logDiagOffset_ = 0;
	run_.reset();
	llvmPassesFile_.reset();
	pendingReselectAddress_.reset();
	if (stopBatch) finishBatchDecompile(true);
	if (!analysisRunning_) return;
	analysisRunning_ = false;
	setActionEnabled("analysis.run_full", true);
	setActionEnabled("analysis.stop", false);
	updateRedecompileEnabled();
	stopElapsedTimer();
	progressShown_ = false;
	setShown(ui_, statusProgress_, false);
	setStatusStage("Stopped");
	progress_->analysisCancelled();
	if (!currentProgressStage_.empty()) progress_->setStageState(currentProgressStage_, model::StageState::Error);
	currentProgressStage_.clear();
}

void MainWindow::runFullAnalysis()
{
	if (!project_)
	{
		if (quitWhenDecompileFinishes_)
		{
			headlessFail(2, "No binary to decompile");
			return;
		}
		files_->show(FilePrompt::Mode::Open, "Open Binary", fileDialogStartDir(), {}, [this](const std::string& path) {
			if (path.empty()) return;
			openBinary(path);
			if (project_) runFullAnalysis();
		});
		return;
	}
	if (batchRunning_)
	{
		showStatusMessage("Batch decompile in progress", 2500);
		return;
	}
	if (decompilerRunning())
	{
		showStatusMessage("Decompiler already running", 2500);
		return;
	}
	pendingReselectAddress_.reset();
	launchDecompilerForBinary(fs::absolute(project_->binaryPath()).string(), project_->arch());
}

void MainWindow::redecompileSelectedFunction()
{
	if (!project_)
	{
		showStatusMessage("Open a binary first", 2500);
		return;
	}
	if (batchRunning_)
	{
		showStatusMessage("Batch decompile in progress", 2500);
		return;
	}
	if (decompilerRunning())
	{
		showStatusMessage("Decompiler already running", 2500);
		return;
	}
	const auto entry = functionList_->selectedEntry();
	if (!entry)
	{
		showStatusMessage("Select a function in the Functions list", 2500);
		return;
	}
	pendingReselectAddress_ = entry->address;
	if (!launchDecompilerForBinary(
			fs::absolute(project_->binaryPath()).string(), project_->arch(), {entry->selectFunctionsCliName()}))
		pendingReselectAddress_.reset();
}

void MainWindow::headlessFail(int exitCode, const std::string& message)
{
	diagnostics_->addMessage(Severity::Error, "retdec-decompiler", message);
	std::fprintf(stderr, "retdec-gui: %s\n", message.c_str());
	writeHeadlessDecompileReport(msSince(wallStart_), exitCode, decompilerOutputPath_.string());
	lastExitCode_ = exitCode;
	if (onQuitRequested) onQuitRequested(exitCode);
}

void MainWindow::writeHeadlessDecompileReport(
	int64_t decompileMs, int exitCode, const std::string& outputPath, const std::vector<std::string>& args)
{
	std::error_code ec;
	const fs::path path = fs::temp_directory_path(ec) / "retdec-gui-decompile-timing.txt";
	if (ec) return;
	std::string text = std::format("DECOMPILE_MS={}\nEXIT={}\nOUTPUT={}\n", decompileMs, exitCode, outputPath);
	if (!args.empty())
	{
		text += "ARGS=";
		for (std::size_t i = 0; i < args.size(); ++i)
			text += (i ? " " : "") + args[i];
		text += '\n';
	}
	(void)forge::os::write_file(path.string(), text);
}

bool MainWindow::launchDecompilerForBinary(
	const std::string& absBinary,
	const std::string& arch,
	const std::vector<std::string>& selectedFunctions,
	const std::vector<std::string>& selectedRanges)
{
	if (decompilerRunning()) return false;
	const bool quit = quitWhenDecompileFinishes_;
	wallStart_ = std::chrono::steady_clock::now();

	const fs::path decExe = model::resolveRetdecDecompilerExecutablePath();
	if (decExe.empty())
	{
		const std::string msg = "retdec-decompiler was not found next to the GUI or on PATH.";
		if (quit)
			headlessFail(127, msg);
		else
			warn("Decompiler", msg);
		return false;
	}

	auto& st = settings_;
	// Headless / parity runs must match a plain CLI invocation: no saved
	// decompiler overrides (--config, custom LLVM passes, output dir, ...).
	std::string outDir = quit ? std::string() : st.decompiler.decompileOutputDir;
	while (!outDir.empty() && std::isspace(static_cast<unsigned char>(outDir.back())))
		outDir.pop_back();
	if (!outDir.empty())
	{
		std::error_code ec;
		fs::create_directories(outDir, ec);
		if (ec || !fs::is_directory(outDir, ec))
		{
			const std::string msg = "Could not create decompile output directory:\n" + outDir;
			if (quit)
				headlessFail(1, msg);
			else
				warn("Decompiler", msg);
			return false;
		}
	}
	decompilerOutputPath_ = model::resolveGuiDecompiledCPath(absBinary, outDir);

	using V = model::AdvancedSettings::Verbosity;
	model::DecompilerLaunchRequest req;
	req.binaryPath = absBinary;
	req.outputPath = decompilerOutputPath_.string();
	req.arch = arch;
	req.fastDecompile = fastDecompile_;
	req.printAfterAll = !quit && (printAfterAll_ || st.advanced.dumpIR || st.advanced.verbosity == V::Debug);
	req.emitCfg = !quit && st.advanced.dumpCFG;
	req.emitCg = req.emitCfg;
	req.disableStaticCodeDetection = !quit && !st.recovery.detectPatterns;
	req.selectedRanges = selectedRanges;
	req.entryPoint = (!quit && project_) ? project_->entryPoint() : 0;
	if (!quit) model::applyNextRunOptions(nextRun_, req);
	req.silent = quit || st.advanced.verbosity == V::Quiet || st.advanced.verbosity == V::Normal;
	req.selectDecodeOnly = !quit && !selectedFunctions.empty();
	req.selectedFunctions = selectedFunctions;
	req.decompiler = quit ? model::DecompilerSettings{} : st.decompiler;

	auto built = model::buildDecompilerArguments(req);
	if (!built)
	{
		if (quit)
			headlessFail(1, built.error());
		else
			warn("Decompiler", built.error().empty() ? "Could not build decompiler arguments." : built.error());
		return false;
	}
	if (!built->warning.empty()) diagnostics_->addMessage(Severity::Warning, "retdec-decompiler", built->warning);

	const model::ProcessEnvironment env = model::buildDecompilerProcessEnvironment(st, !quit);
	model::DecompileRunSpec spec;
	spec.executable = decExe;
	spec.arguments = built->args;
	spec.workingDirectory = decExe.parent_path();
	spec.environment = env;

	lastDecompilerExe_ = decExe.string();
	lastDecompilerArgs_ = built->args;
	lastDecompilerCwd_ = decExe.parent_path().string();
	llvmPassesFile_ = std::move(built->llvmPassesFile);

	run_ = std::make_unique<model::DecompileRun>();
	if (auto started = run_->start(spec); !started)
	{
		run_.reset();
		llvmPassesFile_.reset();
		if (quit)
			headlessFail(126, "Failed to start retdec-decompiler: " + started.error());
		else
			warn("Decompiler", "Failed to start retdec-decompiler.\n" + started.error());
		return false;
	}
	lastLogPath_ = run_->logPath();

	analysisRunning_ = true;
	setActionEnabled("analysis.run_full", false);
	setActionEnabled("analysis.redecompile", false);
	setActionEnabled("analysis.stop", true);
	progressShown_ = true;
	setShown(ui_, statusProgress_, true);
	setAnalysisProgress(0);
	fuse::ui::widgets::setProgressIndeterminate(ui_, statusProgress_, true);
	startElapsedTimer();
	logProgressOffset_ = logConsoleOffset_ = logDiagOffset_ = 0;
	currentProgressStage_.clear();
	if (!quit)
	{
		progress_->resetAll();
		progress_->analysisStarted();
		setOutputTabVisible(kOutProgress, true);
		focusOutputTab(kOutProgress);
	}
	if (batchRunning_)
		updateBatchStatusMessage("Decompiling\xE2\x80\xA6");
	else if (!selectedFunctions.empty())
		setStatusStage(std::format("Re-decompiling {}\xE2\x80\xA6", selectedFunctions.front()));
	else if (!selectedRanges.empty())
	{
		std::string r;
		for (const auto& s: selectedRanges)
			r += (r.empty() ? "" : ",") + s;
		setStatusStage(std::format("Range decompile {}\xE2\x80\xA6", r));
	}
	else
		setStatusStage("Decompiling (retdec-decompiler)\xE2\x80\xA6");

	console_->setFlushHz(!quit && st.advanced.verbosity == V::Debug ? 40 : 20);
	console_->appendBanner("retdec-decompiler", built->args, lastDecompilerCwd_);
	const bool streamLog = st.decompiler.liveConsoleTail && !quit;
	console_->appendLine(
		LiveConsolePanel::Stream::Stdout,
		streamLog ? std::format(
						"(stdout/stderr redirected to {} \xE2\x80\x94 streaming log to console)", lastLogPath_.string())
				  : std::format(
						"(stdout/stderr redirected to {} \xE2\x80\x94 console updates when the run finishes)",
						lastLogPath_.string()));
	if (!quit) focusOutputTab(kOutConsole);

	const bool neural = model::processEnvironmentValue(env, "RETDEC_NEURAL_REFINE") == "1";
	ui_.setText(statusNeural_, neural ? "Neural: on" : "Neural: off");
	if (neural)
		console_->appendLine(
			LiveConsolePanel::Stream::Stdout,
			std::format(
				"Neural refine: model={} gpu_layers={} ctx={} max_tokens={}",
				model::processEnvironmentValue(env, "RETDEC_NEURAL_MODEL"),
				model::processEnvironmentValue(env, "RETDEC_NEURAL_N_GPU_LAYERS"),
				model::processEnvironmentValue(env, "RETDEC_NEURAL_CTX"),
				model::processEnvironmentValue(env, "RETDEC_NEURAL_MAX_TOKENS")));

	stopPollTimer();
	pollTimerOn_ = true;
	pollTimer_ = ui_.startTimer(kPollSeconds, true, [this](UiContext&) { pollDecompile(); });
	return true;
}

void MainWindow::pollDecompile()
{
	if (!run_) return;
	static int tick = 0;
	const bool finished = run_->poll();
	if (!quitWhenDecompileFinishes_ && (finished || ++tick % kProgressEveryTicks == 0))
	{
		const fs::path logPath = run_->logPath();
		if (auto prog = model::pollDecompileLogProgress(logPath, &logProgressOffset_))
		{
			if (batchRunning_)
				updateBatchStatusMessage(prog->stage);
			else
				setStatusStage(prog->stage);
			setAnalysisProgress(prog->percent);
			const int64_t now = msSince(wallStart_);
			const std::string prev = currentProgressStage_;
			if (!prev.empty() && prev != prog->stage)
			{
				const int64_t elapsed = now >= currentStageStartMs_ ? now - currentStageStartMs_ : 0;
				progress_->stageCompleted(prev, elapsed);
				if (project_) project_->setStageStatus(prev, "done");
				currentStageStartMs_ = now;
			}
			else if (prev.empty())
				currentStageStartMs_ = now;
			currentProgressStage_ = prog->stage;
			if (prev != prog->stage) progress_->stageStarted(prog->stage);
			progress_->setStageState(prog->stage, model::StageState::Running, prog->percent);
			progress_->setOverallProgress(prog->percent);
		}
		if (settings_.decompiler.liveConsoleTail)
		{
			const std::string chunk = model::readDecompilerLogIncremental(logPath, &logConsoleOffset_);
			if (!chunk.empty()) console_->appendChunk(LiveConsolePanel::Stream::Stdout, chunk);
		}
		for (auto& d: model::scanDecompilerLogDiagnosticsIncremental(logPath, &logDiagOffset_))
			diagnostics_->addDiagnostic(std::move(d));
	}
	if (finished) onDecompileFinished();
}

void MainWindow::finishExternalDecompileUi()
{
	stopPollTimer();
	logProgressOffset_ = logConsoleOffset_ = logDiagOffset_ = 0;
	analysisRunning_ = false;
	setActionEnabled("analysis.run_full", true);
	setActionEnabled("analysis.stop", false);
	updateRedecompileEnabled();
	stopElapsedTimer();
	progressShown_ = false;
	setShown(ui_, statusProgress_, false);
	if (!currentProgressStage_.empty())
	{
		progress_->setStageState(currentProgressStage_, model::StageState::Done);
		if (project_) project_->setStageStatus(currentProgressStage_, "done");
	}
	progress_->setOverallProgress(100);
	currentProgressStage_.clear();
	setStatusStage("Idle");
}

void MainWindow::onDecompileFinished()
{
	using State = model::DecompileRun::State;
	const State state = run_->state();
	const int64_t decompileMs = run_->elapsed().count();
	lastDecompileSubprocessMs_ = decompileMs;
	const fs::path logPath = run_->logPath();
	const bool quit = quitWhenDecompileFinishes_;
	const bool wasBatch = batchRunning_;
	if (wasBatch) model::batchDecompilePopFront(&batchQueue_);

	if (state == State::FailedToStart)
	{
		const std::string err = run_->errorString();
		diagnostics_->addMessage(Severity::Error, "retdec-decompiler", err);
		commandLog_->appendRun("retdec-decompiler", lastDecompilerArgs_, lastDecompilerCwd_, -1, decompileMs, err);
		console_->appendLine(LiveConsolePanel::Stream::Stderr, err);
		run_.reset();
		llvmPassesFile_.reset();
		pendingReselectAddress_.reset();
		finishExternalDecompileUi();
		if (quit)
		{
			headlessFail(126, err);
			return;
		}
		if (!wasBatch) warn("Decompiler", err);
		if (wasBatch && batchRunning_)
		{
			if (!batchQueue_.empty())
				ui_.startTimer(0.f, false, [this](UiContext&) { processNextBatchItem(); });
			else
				finishBatchDecompile(false);
		}
		return;
	}

	const int exitCode = run_->exitCode().value_or(-1);
	lastExitCode_ = exitCode;

	// Console: the rest of the streamed log, or the whole log now.
	if (settings_.decompiler.liveConsoleTail && !quit)
	{
		for (;;)
		{
			const std::string chunk = model::readDecompilerLogIncremental(logPath, &logConsoleOffset_);
			if (chunk.empty()) break;
			console_->appendChunk(LiveConsolePanel::Stream::Stdout, chunk);
		}
	}
	else
	{
		const auto log = model::readDecompilerLogForConsole(logPath);
		if (!log.notice.empty()) console_->appendLine(LiveConsolePanel::Stream::Stdout, log.notice);
		if (!log.text.empty()) console_->appendChunk(LiveConsolePanel::Stream::Stdout, log.text);
	}
	console_->appendFooter("retdec-decompiler", exitCode, decompileMs);
	for (auto& d: model::scanDecompilerLogDiagnosticsIncremental(logPath, &logDiagOffset_, 200))
		diagnostics_->addDiagnostic(std::move(d));
	commandLog_->appendRun(
		"retdec-decompiler",
		lastDecompilerArgs_,
		lastDecompilerCwd_,
		exitCode,
		decompileMs,
		model::readDecompilerLogTail(logPath));
	if (run_->timedOut())
		diagnostics_->addMessage(Severity::Error, "retdec-decompiler", "The decompiler run timed out and was stopped.");

	run_.reset();
	llvmPassesFile_.reset();
	const auto reselect = pendingReselectAddress_;
	pendingReselectAddress_.reset();

	if (exitCode == 0)
	{
		if (!wasBatch)
		{
			const auto loadStart = std::chrono::steady_clock::now();
			const bool ok = loadDecompileArtifacts(decompilerOutputPath_, reselect);
			const int64_t loadMs = msSince(loadStart);
			if (!ok)
				diagnostics_->addMessage(
					Severity::Warning,
					"retdec-decompiler",
					"Finished but could not read: " + decompilerOutputPath_.string());
			else
			{
				setStatusDecompileState("decompiled");
				showStatusMessage(
					std::format("Decompile {:.1f} s \xC2\xB7 load {:.1f} s", decompileMs / 1000.0, loadMs / 1000.0),
					8000);
			}
		}
	}
	else
	{
		diagnostics_->addMessage(Severity::Error, "retdec-decompiler", std::format("Exit code {}", exitCode));
		if (!quit && !wasBatch)
			warn("Decompiler", std::format("retdec-decompiler failed (exit {}). See Problems.", exitCode));
	}
	progress_->analysisCompleted(decompileMs);
	finishExternalDecompileUi();

	if (wasBatch && batchRunning_)
	{
		if (!batchQueue_.empty())
			ui_.startTimer(0.f, false, [this](UiContext&) { processNextBatchItem(); });
		else
			finishBatchDecompile(false);
		return;
	}
	if (quit)
	{
		writeHeadlessDecompileReport(decompileMs, exitCode, decompilerOutputPath_.string(), lastDecompilerArgs_);
		if (onQuitRequested) onQuitRequested(exitCode);
	}
}

void MainWindow::onAnalysisStageChanged(const std::string& stage)
{
	setStatusStage(stage);
	if (project_ && !stage.empty()) project_->setStageStatus(stage, "running");
}

void MainWindow::onAnalysisFinished()
{
	analysisRunning_ = false;
	setActionEnabled("analysis.run_full", true);
	setActionEnabled("analysis.stop", false);
	updateRedecompileEnabled();
	stopElapsedTimer();
	setAnalysisProgress(100);
	progress_->setOverallProgress(100);
	setStatusStage("Analysis complete");
	if (project_) project_->setStageStatus("decompile", "done");
}

// ─── Batch ───────────────────────────────────────────────────────────────────

void MainWindow::startBatchDecompile(const std::vector<std::string>& paths)
{
	if (paths.empty() || batchRunning_ || decompilerRunning()) return;
	batchQueue_ = paths;
	batchTotal_ = int(paths.size());
	batchRunning_ = true;
	processNextBatchItem();
}

void MainWindow::processNextBatchItem()
{
	if (!batchRunning_ || batchQueue_.empty())
	{
		finishBatchDecompile(false);
		return;
	}
	const std::string absBinary = fs::absolute(batchQueue_.front()).string();
	const std::string arch =
		(project_ && fs::absolute(project_->binaryPath()).string() == absBinary) ? project_->arch() : std::string();
	updateBatchStatusMessage("Starting\xE2\x80\xA6");
	if (launchDecompilerForBinary(absBinary, arch)) return;
	model::batchDecompilePopFront(&batchQueue_);
	if (batchRunning_ && !batchQueue_.empty())
		ui_.startTimer(0.f, false, [this](UiContext&) { processNextBatchItem(); });
	else
		finishBatchDecompile(false);
}

void MainWindow::finishBatchDecompile(bool cancelled)
{
	if (!batchRunning_ && batchQueue_.empty() && batchTotal_ == 0) return;
	const int total = batchTotal_;
	batchQueue_.clear();
	batchTotal_ = 0;
	batchRunning_ = false;
	if (cancelled)
		showStatusMessage("Batch decompile cancelled", 5000);
	else if (total > 0)
		showStatusMessage(std::format("Batch decompile finished ({} files)", total), 8000);
}

void MainWindow::updateBatchStatusMessage(const std::string& stageSuffix)
{
	if (!batchRunning_ || batchQueue_.empty()) return;
	const int index = model::batchDecompileCurrentIndex(batchTotal_, int(batchQueue_.size()));
	std::string msg = model::batchDecompileStatusLabel(index, batchTotal_, batchQueue_.front());
	if (!stageSuffix.empty()) msg += " \xE2\x80\x94 " + stageSuffix;
	showStatusMessage(msg);
}

// ─── Artefacts ───────────────────────────────────────────────────────────────

bool MainWindow::tryLoadCachedDecompile(const std::string& binaryPath)
{
	if (binaryPath.empty()) return false;
	const fs::path cPath = model::locateGuiDecompiledCPath(binaryPath, settings_.decompiler.decompileOutputDir);
	std::error_code ec;
	if (!fs::exists(cPath, ec) || !fs::exists(binaryPath, ec)) return false;
	if (!fs::exists(model::pathsFromOutputC(cPath).configPath, ec)) return false;
	const auto cTime = forge::os::fs::mtime_ns(cPath.string());
	const auto bTime = forge::os::fs::mtime_ns(binaryPath);
	const bool stale = cTime && bTime && cTime.value < bTime.value;
	if (!loadDecompileArtifacts(cPath)) return false;
	decompilerOutputPath_ = fs::absolute(cPath);
	setStatusDecompileState("cached");
	if (stale)
	{
		showStatusMessage("Cache may be stale \xE2\x80\x94 re-decompile recommended", 8000);
		diagnostics_->addMessage(
			Severity::Warning,
			"retdec-decompiler",
			"Cached decompile is older than the binary \xE2\x80\x94 press F5 to refresh.");
	}
	else
		showStatusMessage("Loaded cached decompile \xE2\x80\x94 press F5 to re-decompile.", 8000);
	return true;
}

bool MainWindow::loadDecompileArtifacts(const fs::path& cPath, std::optional<uint64_t> reselectAddress)
{
	showStatusMessage("Loading artifacts...");
	const fs::path absC = fs::absolute(cPath).lexically_normal();
	auto art = model::loadDecompileArtifactsFromPaths(model::pathsFromOutputC(absC), settings_.recovery);
	if (!art)
	{
		diagnostics_->addMessage(Severity::Warning, "retdec-decompiler", art.error());
		showStatusMessage("");
		return false;
	}
	if (project_) project_->setDecompiledPath(absC.string());
	loadedArtifacts_ = std::move(*art);
	populateFromArtifacts(absC, reselectAddress);
	return true;
}

void MainWindow::populateFromArtifacts(const fs::path& absC, std::optional<uint64_t> reselect)
{
	if (!loadedArtifacts_) return;
	const model::DecompileArtifacts& art = *loadedArtifacts_;
	std::error_code ec;

	const fs::path refinedC = absC.string() + ".refined.c";
	const bool hasRefined = fs::exists(refinedC, ec);
	auto original = forge::os::read_file(absC.string());
	if (!original)
	{
		diagnostics_->addMessage(Severity::Warning, "retdec-decompiler", "Could not read: " + absC.string());
		return;
	}
	decompiledC_->setSource(original.value);
	if (hasRefined)
	{
		decompiledC_->setRefinedSource(readText(refinedC));
		decompiledC_->setShowRefined(true);
	}
	else
		decompiledC_->setRefinedSource("");

	const fs::path manifestPath = absC.string() + ".refinement-manifest.json";
	if (fs::exists(manifestPath, ec))
		if (auto m = parseJson(readText(manifestPath)); m && m->is_obj())
		{
			const forge::json::value* acc = m->find("accepted");
			const bool accepted = acc && acc->is_bool() && std::get<bool>(acc->v);
			const std::string reason = m->str_or("reason", "");
			diagnostics_->addMessage(
				accepted ? Severity::Info : Severity::Warning,
				"retdec-neural",
				accepted ? "Neural refine accepted \xE2\x80\x94 showing " + refinedC.string()
						 : "Neural refine rejected: " + (reason.empty() ? std::string("see manifest") : reason));
		}

	std::unordered_map<std::string, float> tiByName;
	const fs::path tiPath = absC.string() + ".type-inference.json";
	if (settings_.analysis.enableTyping && fs::exists(tiPath, ec))
		if (auto doc = parseJson(readText(tiPath)))
		{
			const forge::json::value* arrV = doc->is_arr() ? &*doc : doc->find("functions");
			if (arrV && arrV->is_arr())
			{
				int n = 0;
				int known = 0;
				for (const auto& v: arrV->arr())
				{
					if (!v.is_obj()) continue;
					++n;
					const int k = int(v.num_or("known", 0));
					known += k;
					const std::string name = v.str_or("name", "");
					if (name.empty()) continue;
					const double s = k + v.num_or("structs", 0) + v.num_or("arrays", 0);
					tiByName[name] = std::clamp(float(s / 8.0), 0.f, 1.f);
				}
				if (n > 0)
					diagnostics_->addMessage(
						Severity::Info,
						"type-inference",
						std::format("{} functions, {} values with known types", n, known));
			}
		}

	std::vector<model::FunctionEntry> shown = art.functions;
	const int cap = settings_.advanced.maxFunctions;
	if (cap > 0 && int(shown.size()) > cap)
	{
		shown.resize(std::size_t(cap));
		diagnostics_->addMessage(
			Severity::Info,
			"retdec-decompiler",
			std::format(
				"Showing first {} of {} functions (Settings \xE2\x86\x92 Advanced \xE2\x86\x92 max functions)",
				cap,
				art.functions.size()));
	}
	for (auto& f: shown)
	{
		if (project_)
			if (auto ann = project_->annotation(f.address))
			{
				if (!ann->name.empty()) f.name = ann->name;
				if (!ann->comment.empty()) f.notes = ann->comment;
				if (!ann->signatureOverride.empty()) f.signature = ann->signatureOverride;
				if (!ann->tags.empty()) f.tags = ann->tags;
			}
		if (auto it = tiByName.find(f.name.empty() ? f.rawName : f.name); it != tiByName.end())
			f.confidence.typeInference = it->second;
	}
	shownFunction_ = ~uint64_t(0);
	functionList_->setFunctions(shown);
	setStatusFunctionCount(int(art.functions.size()));

	int64_t instrs = 0;
	for (const auto& f: art.functions)
		instrs += f.instrCount;
	progress_->setFunctionCount(int(art.functions.size()));
	progress_->setInstructionCount(instrs);
	if (lastDecompileSubprocessMs_ > 0)
	{
		const double sec = double(lastDecompileSubprocessMs_) / 1000.0;
		progress_->setThroughput(double(art.functions.size()) / sec, double(instrs) / sec);
	}
	if (hasRefined && !quitWhenDecompileFinishes_)
	{
		compare_->clear();
		compare_->addStage("Original vs refined", original.value, readText(refinedC));
	}

	for (auto& d: model::semanticDetectionDiagnostics(art.config, settings_))
		diagnostics_->addDiagnostic(std::move(d));

	{
		const auto side = model::pathsFromOutputC(absC);
		const bool hasDsm = fs::exists(side.dsmPath, ec);
		const bool hasLl = fs::exists(side.llPath, ec);
		diagnostics_->addMessage(
			hasDsm && hasLl ? Severity::Info : Severity::Warning,
			"retdec-decompiler",
			std::format(
				"Sidecars: {} {}, {} {}",
				side.dsmPath.filename().string(),
				hasDsm ? "ok" : "missing",
				side.llPath.filename().string(),
				hasLl ? "ok" : "missing"));
		std::string dumpTo = settings_.advanced.irDumpPath;
		while (!dumpTo.empty() && std::isspace(static_cast<unsigned char>(dumpTo.back())))
			dumpTo.pop_back();
		if (!dumpTo.empty() && hasLl)
		{
			fs::path dest = dumpTo;
			if (fs::is_directory(dest, ec) || dumpTo.back() == '/' || dumpTo.back() == '\\')
				dest = dest / side.llPath.filename();
			fs::remove(dest, ec);
			const bool copied = fs::copy_file(side.llPath, dest, ec) && !ec;
			diagnostics_->addMessage(
				copied ? Severity::Info : Severity::Warning,
				"retdec-decompiler",
				(copied ? "Copied IR to " : "Could not copy IR to ") + dest.string());
		}
	}

	{
		model::PipelineContext ctx;
		ctx.inputBinaryPath = project_ ? project_->binaryPath() : std::string();
		ctx.decompiledText = decompiledC_->documentText();
		ctx.irText = art.fullLl;
		ctx.asmText = art.fullDsm;
		ctx.analysisComplete = true;
		auto& pm = model::PluginManager::instance();
		pm.runDecompilerPlugins(ctx);
		pm.runAnalysisPlugins(ctx);
		if (!ctx.decompiledText.empty() && ctx.decompiledText != decompiledC_->documentText())
			decompiledC_->setSource(ctx.decompiledText);
		for (const model::Plugin* ap: pm.analysisPlugins())
		{
			const std::string sum = ap->summary();
			if (!sum.empty()) diagnostics_->addMessage(Severity::Info, "plugin", sum);
		}
	}

	strings_->setStrings(art.strings);
	strings_->setConstants(art.constants);
	if (!art.callGraphNodes.empty() && !quitWhenDecompileFinishes_)
		callGraph_->setCallGraph(art.callGraphNodes, art.callGraphEdges);
	else
		callGraph_->clear();
	if (toolWindowOpen("Type Hierarchy")) refreshTypeHierarchyFromArtifacts();

	const auto cSize = fs::file_size(absC, ec);
	const uint64_t kib = ec ? 0 : cSize / 1024;
	diagnostics_->addMessage(
		Severity::Info, "retdec-decompiler", std::format("Output: {} ({} KiB)", absC.string(), kib));
	showStatusMessage(
		std::format("Loaded decompile artifacts ({} KiB C, {} functions)", kib, art.functions.size()), 5000);

	raiseDocumentTab(kDocDecompiledC);
	if (quitWhenDecompileFinishes_) return;
	if (reselect)
		functionList_->selectFunction(*reselect);
	else if (!art.functions.empty())
	{
		const auto& f0 = art.functions.front();
		showFunction(f0.address, f0.name.empty() ? f0.rawName : f0.name);
	}
}

void MainWindow::showFunction(uint64_t address, const std::string& name)
{
	shownFunction_ = address;
	decompiledC_->onFunctionSelected(address, name);
	ir_->onFunctionSelected(address, name);
	callGraph_->setFunction(address, name);
	triPane_->onFunctionSelected(address, name);
	if (!loadedArtifacts_)
	{
		assembly_->navigateTo(address);
		return;
	}
	const model::DecompileArtifacts& art = *loadedArtifacts_;
	const model::FunctionViewData data = model::buildFunctionViewData(art, address, name, true);

	assembly_->setAssemblyText(data.assemblyText.empty() ? art.fullDsm : data.assemblyText);
	assembly_->navigateTo(address);

	if (!art.fullLl.empty()) ir_->setIRText(art.fullLl, "Module");
	ir_->setIRText(data.irText, "Function");

	if (data.cfgBlocks.empty())
		cfg_->clear();
	else
		cfg_->setCfg(data.cfgBlocks, data.cfgEdges);
	cfg_->setFunction(address, name);

	triPane_->loadFunction(address, data.assemblyText, data.irText, data.cText);
	model::LineMapping mapping;
	for (const auto& e: data.lineMapping)
		mapping.addEntry(e.asmLine, e.irLine, e.cLine);
	triPane_->setLineMapping(mapping);

	decompiledC_->scrollToFunction(name, data.startLine, data.endLine);
	ai_->setActiveFunction(data.cText, name);
}

} // namespace retdec::gui::ui
