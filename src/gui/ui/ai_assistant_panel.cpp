/**
 * @file src/gui/ui/ai_assistant_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/ai_assistant_panel.h"

#include "retdec/gui/model/dlg_input.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <filesystem>
#include <format>
#include <system_error>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

constexpr std::string_view kToggleCss = R"css(
.retdec-toggle:checked { background: var(--accent, #3d8bfd); color: #ffffff; }
)css";

/// Posted by the worker thread: `a` is the panel, valid while its root node lives.
void pumpCmd(UiContext& ui, NodeHandle root, u64 a, u64)
{
	if (!ui.alive(root)) return;
	reinterpret_cast<AIAssistantPanel*>(static_cast<std::uintptr_t>(a))->pumpEvents();
}

const char* roleClass(model::ChatRole r)
{
	switch (r)
	{
		case model::ChatRole::User: return "retdec-chat-user";
		case model::ChatRole::Assistant: return "retdec-chat-assistant";
		default: return "retdec-chat-system";
	}
}

} // namespace

AIAssistantPanel::AIAssistantPanel(UiContext& ui, NodeHandle parent, model::AssistantBackendFactory factory,
                                   model::AppSettings& settings)
	: ui_(ui), settings_(settings), files_(ui)
{
	dlg::ensureDialogStyles(ui);
	if (!ui.hasClass(ui.root(), "retdec-toggle-styled"))
	{
		ui.addClass(ui.root(), "retdec-toggle-styled");
		(void)ui.addStyleSheet(kToggleCss);
	}
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "ai_assistant_panel");
	ui.setCss(root_, "padding", "6px");
	ui.setCss(root_, "gap", "6px");

	// ── Top bar ──
	const NodeHandle top = dlg::hrow(ui, root_);
	modelLabel_ = w::label(ui, top, "Model: (none)", "aiAssistantModelPathLabel");
	ui.setCss(modelLabel_, "flex-grow", "1");
	ui.setCss(modelLabel_, "flex-shrink", "1");
	ui.setCss(modelLabel_, "min-width", "0px");
	loadBtn_ = w::button(ui, top, "Load", "load_model_button");
	gpuBtn_ = w::button(ui, top, "GPU: OFF", "gpu_button");
	ui.addClass(gpuBtn_, "retdec-toggle");
	settingsBtn_ = w::button(ui, top, "⚙", "settings_button");
	ui.addClass(settingsBtn_, "retdec-toggle");
	w::setTooltip(ui, settingsBtn_, "Generation settings");
	clearBtn_ = w::button(ui, top, "Clear", "clear_button");

	// ── Settings bar (hidden by default) ──
	settingsBar_ = dlg::hrow(ui, root_);
	ui.setCss(settingsBar_, "flex-wrap", "wrap");
	auto spin = [&](std::string_view label, float lo, float hi, float v, float step, std::string_view id) {
		w::label(ui, settingsBar_, label);
		const NodeHandle s = w::spinBox(ui, settingsBar_, lo, hi, v, step, id);
		ui.setCss(s, "width", "96px");
		return s;
	};
	tempSpin_ = spin("Temperature", 0.f, 2.f, 0.6f, 0.05f, "aiAssistantTemperatureSpin");
	topPSpin_ = spin("Top-P", 0.f, 1.f, 0.95f, 0.05f, "aiAssistantTopPSpin");
	topKSpin_ = spin("Top-K", 0.f, 200.f, 20.f, 1.f, "aiAssistantTopKSpin");
	maxTokSpin_ = spin("Max tokens", 1.f, 32768.f, 512.f, 1.f, "aiAssistantMaxTokensSpin");
	thinking_ = w::checkBox(ui, settingsBar_, "Thinking mode", false, "aiAssistantThinkingCheck");
	setShown(ui, settingsBar_, false);

	// ── Chat ──
	chat_ = w::scrollArea(ui, root_, "aiAssistantChatLog");
	ui.setCss(chat_, "flex-grow", "1");
	ui.setCss(chat_, "min-height", "60px");
	ui.setCss(chat_, "gap", "6px");
	ui.setCss(chat_, "padding", "6px");
	ui.setCss(chat_, "background", "#15161a");
	ui.setCss(chat_, "border", "1px var(--border, #474955)");
	ui.setCss(chat_, "border-radius", "5px");
	status_ = w::label(ui, root_, "", "status_label");
	ui.addClass(status_, "retdec-note");

	// ── Input row ──
	const NodeHandle inRow = dlg::hrow(ui, root_);
	input_ = dlg::growEdit(ui, inRow);
	sendBtn_ = w::button(ui, inRow, "Send", "aiAssistantSendButton");
	ui.addClass(sendBtn_, "primary");
	stopBtn_ = w::button(ui, inRow, "Stop", "stop_button");
	ui.setDisabled(stopBtn_, true);

	// ── Wiring ──
	worker_ = std::make_unique<model::InferenceWorker>(std::move(factory));
	worker_->setTemperature(0.6f);
	worker_->setTopP(0.95f);
	worker_->setTopK(20);
	worker_->setMaxTokens(512);
	{
		UiContext* u = &ui_;
		const NodeHandle root = root_;
		const u64 self = static_cast<u64>(reinterpret_cast<std::uintptr_t>(this));
		worker_->setEventsReadyCallback([u, root, self] { u->post(&pumpCmd, root, self); });
	}

	ui.connect(loadBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { browseModel(); });
	ui.connect(gpuBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { setGpuEnabled(!gpuEnabled_); });
	ui.connect(settingsBtn_, SignalKind::Clicked,
	           [this](UiContext&, NodeHandle, const SignalArgs&) { setSettingsBarVisible(!settingsShown_); });
	ui.connect(clearBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { clear(); });
	ui.connect(sendBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { sendQuery(); });
	ui.connect(stopBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { stopGeneration(); });
	ui.connect(input_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs&) { sendQuery(); });
	ui.connect(tempSpin_, SignalKind::ValueChanged,
	           [this](UiContext&, NodeHandle, const SignalArgs& a) { worker_->setTemperature(a.value); });
	ui.connect(topPSpin_, SignalKind::ValueChanged,
	           [this](UiContext&, NodeHandle, const SignalArgs& a) { worker_->setTopP(a.value); });
	ui.connect(topKSpin_, SignalKind::ValueChanged,
	           [this](UiContext&, NodeHandle, const SignalArgs& a) { worker_->setTopK(int(a.value)); });
	ui.connect(maxTokSpin_, SignalKind::ValueChanged,
	           [this](UiContext&, NodeHandle, const SignalArgs& a) { worker_->setMaxTokens(int(a.value)); });
	ui.connect(thinking_, SignalKind::Toggled,
	           [this](UiContext&, NodeHandle, const SignalArgs& a) { setThinkingMode(a.flag); });

	applyMlSettingsFromApp();
	if (model::assistantHasNeuralBackend())
		appendSystemMessage(model::kAssistantLoadHint);
	else
	{
		appendSystemMessage(model::kAssistantNoBackend);
		appendSystemMessage(model::kAssistantEnableHint);
	}
}

AIAssistantPanel::~AIAssistantPanel()
{
	// Stop the worker first: after this no event can be posted to the root.
	worker_.reset();
	releasePanelRoot(ui_, root_);
}

bool AIAssistantPanel::handleEvent(EventContext&, const Event&)
{
	return false;
}

// ── Context ──────────────────────────────────────────────────────────────────

void AIAssistantPanel::setActiveFunction(std::string_view decompiledSource, std::string_view functionName)
{
	if (functionName != activeFunctionName_) worker_->resetKvCache();
	activeDecompiled_ = decompiledSource;
	activeFunctionName_ = functionName;
	contextPending_ = !decompiledSource.empty();
}

void AIAssistantPanel::clear()
{
	history_.clear();
	activeDecompiled_.clear();
	activeFunctionName_.clear();
	contextPending_ = false;
	rebuildChatLog();
	worker_->resetKvCache();
}

// ── Model ────────────────────────────────────────────────────────────────────

bool AIAssistantPanel::loadModel(const std::string& ggufPath, bool useGpu)
{
	setGpuEnabled(useGpu);
	worker_->loadModel(ggufPath, useGpu, settings_.ml.contextLength);
	ui_.setText(status_, "Loading model…");
	return true;
}

void AIAssistantPanel::unloadModel()
{
	worker_->unloadModel();
	modelLoaded_ = false;
	modelPath_.clear();
	ui_.setText(modelLabel_, "Model: (none)");
}

void AIAssistantPanel::browseModel()
{
	files_.show(FilePrompt::Mode::Open, "Open GGUF model", settings_.ml.modelPath, "*.gguf",
	            [this](const std::string& path) { loadModel(path, gpuEnabled_); });
}

void AIAssistantPanel::applyMlSettingsFromApp()
{
	const model::MLSettings& ml = settings_.ml;
	const bool cpuOnly = ml.inferenceDevice == model::MLSettings::InferenceDevice::CPU;
	const bool gpuOn = ml.inferenceDevice == model::MLSettings::InferenceDevice::GPU;
	model::applyProcessEnvironmentVariables(model::assistantNeuralEnvironment(ml, ui_.checked(thinking_)));
	if (cpuOnly) setGpuEnabled(false);
	else if (gpuOn) setGpuEnabled(true);

	worker_->setTemperature(float(ml.temperature));
	worker_->setTopP(float(ml.topP));
	worker_->setTopK(ml.topK);
	worker_->setMaxTokens(ml.maxNewTokens);
	ui_.setValue(tempSpin_, float(ml.temperature));
	ui_.setValue(topPSpin_, float(ml.topP));
	ui_.setValue(topKSpin_, float(ml.topK));
	ui_.setValue(maxTokSpin_, float(ml.maxNewTokens));

	std::error_code ec;
	if (!ml.modelPath.empty() && std::filesystem::is_regular_file(ml.modelPath, ec) && !modelLoaded_)
	{
		// Not loaded automatically: a real GGUF would stall start-up and tests.
		ui_.setText(modelLabel_, std::format("Model: {} (not loaded)", ml.modelPath));
	}
}

// ── Toggles ──────────────────────────────────────────────────────────────────

void AIAssistantPanel::setGpuEnabled(bool on)
{
	gpuEnabled_ = on;
	ui_.setChecked(gpuBtn_, on);
	ui_.setText(gpuBtn_, on ? "GPU: ON" : "GPU: OFF");
}

void AIAssistantPanel::setSettingsBarVisible(bool on)
{
	settingsShown_ = on;
	ui_.setChecked(settingsBtn_, on);
	setShown(ui_, settingsBar_, on);
}

void AIAssistantPanel::setThinkingMode(bool on)
{
	ui_.setChecked(thinking_, on);
	model::applyProcessEnvironmentVariables({{"RETDEC_NEURAL_THINKING", on ? "1" : "0"}});
	worker_->setThinkingMode(on);
}

// ── Chat ─────────────────────────────────────────────────────────────────────

void AIAssistantPanel::appendSystemMessage(std::string_view text)
{
	history_.push_back({model::ChatRole::System, std::string(text), true});
	rebuildChatLog();
}

void AIAssistantPanel::rebuildChatLog()
{
	for (NodeHandle n: chatNodes_)
		if (ui_.alive(n)) ui_.destroy(n);
	chatNodes_.clear();
	for (const model::ChatMessage& m: history_)
	{
		const NodeHandle l = w::label(ui_, chat_, m.text);
		ui_.addClass(l, roleClass(m.role));
		chatNodes_.push_back(l);
	}
	// Keep the newest message in view.
	ui_.scrollTo(chat_, {0.f, 1e9f});
}

void AIAssistantPanel::updateLastMessageNode()
{
	if (history_.empty() || chatNodes_.size() != history_.size())
	{
		rebuildChatLog();
		return;
	}
	ui_.setText(chatNodes_.back(), history_.back().text);
	ui_.scrollTo(chat_, {0.f, 1e9f});
}

std::string AIAssistantPanel::chatText() const
{
	std::string out;
	for (NodeHandle n: chatNodes_)
	{
		if (!out.empty()) out += "\n\n";
		out += ui_.text(n);
	}
	return out;
}

std::string AIAssistantPanel::modelLabel() const
{
	return std::string(ui_.text(modelLabel_));
}

std::string AIAssistantPanel::statusText() const
{
	return std::string(ui_.text(status_));
}

void AIAssistantPanel::setInferenceBusy(bool busy)
{
	inferenceRunning_ = busy;
	ui_.setDisabled(sendBtn_, busy);
	ui_.setDisabled(stopBtn_, !busy);
	ui_.setText(status_, busy ? "Generating…" : "");
}

void AIAssistantPanel::sendQuery()
{
	const std::string q(model::dlgTrim(dlg::editText(ui_, input_)));
	if (q.empty() || inferenceRunning_) return;
	history_.push_back({model::ChatRole::User, q, true});
	rebuildChatLog();
	dlg::setEditText(ui_, input_, "");
	setInferenceBusy(true);
	lastPrompt_ = model::buildAssistantPrompt(q, activeFunctionName_, activeDecompiled_, contextPending_);
	worker_->startInference(lastPrompt_);
	contextPending_ = false;
}

void AIAssistantPanel::stopGeneration()
{
	worker_->abortInference();
	setInferenceBusy(false);
}

bool AIAssistantPanel::pumpEvents()
{
	std::vector<model::InferenceEvent> events = worker_->takeEvents();
	if (events.empty()) return false;
	bool streamDirty = false;
	for (model::InferenceEvent& e: events)
	{
		using K = model::InferenceEvent::Kind;
		switch (e.kind)
		{
			case K::Token:
				if (!history_.empty() && history_.back().role == model::ChatRole::Assistant && !history_.back().isComplete)
					history_.back().text += e.text;
				else
					history_.push_back({model::ChatRole::Assistant, std::move(e.text), false});
				streamDirty = true;
				break;
			case K::Complete:
				if (!history_.empty() && history_.back().role == model::ChatRole::Assistant)
					history_.back().isComplete = true;
				updateLastMessageNode();
				streamDirty = false;
				setInferenceBusy(false);
				if (e.tokens > 0)
					ui_.setText(status_, e.tokPerSec > 0.0 ? std::format("{} tokens ({:.1f} tok/s)", e.tokens, e.tokPerSec)
					                                       : std::format("{} tokens", e.tokens));
				break;
			case K::Error:
				appendSystemMessage(model::assistantErrorText(e.text));
				setInferenceBusy(false);
				break;
			case K::SystemMessage: appendSystemMessage(e.text); break;
			case K::LoadFinished:
				modelLoaded_ = e.ok;
				if (e.ok)
				{
					modelPath_ = e.path;
					ui_.setText(modelLabel_, "Model: " + e.path);
				}
				else
					ui_.setText(modelLabel_, "Load failed");
				if (!inferenceRunning_) ui_.setText(status_, "");
				break;
		}
	}
	// Streaming redraws per batch of tokens; without it the text appears on completion.
	if (streamDirty && settings_.ml.streamOutput) updateLastMessageNode();
	return true;
}

} // namespace retdec::gui::ui
