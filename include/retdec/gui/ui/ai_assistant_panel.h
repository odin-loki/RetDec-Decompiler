/**
 * @file include/retdec/gui/ui/ai_assistant_panel.h
 * @brief AIAssistantPanel — chat about the selected function with a local
 *        model (Tools → AI Assistant).
 *
 *   Model: (none)                [Load] [GPU: OFF] [Settings] [Clear]
 *   (Settings) Temperature [0.60] Top-P [0.95] Top-K [20] Max tokens [512] [ ] Thinking mode
 *   ┌ chat log (User blue, Assistant green, System grey) ────────────┐
 *   status
 *   [ query …                                   ] [Send] [Stop]
 *
 * The first query after setActiveFunction() carries the function's
 * decompiled source as context. Inference runs on model::InferenceWorker's
 * thread; tokens stream into the log as they arrive (redrawn per token when
 * Settings → ML → Stream output is on, else once at the end). With
 * RETDEC_GUI_HAS_NEURAL the worker drives retdec::neural; without it the panel
 * says so and explains how to build with the backend, and every request
 * answers with that message.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_AI_ASSISTANT_PANEL_H
#define RETDEC_GUI_UI_AI_ASSISTANT_PANEL_H

#include "retdec/gui/model/dlg_ai_assistant.h"
#include "retdec/gui/model/settings.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

class AIAssistantPanel {
public:
	AIAssistantPanel(
		fuse::ui::UiContext& ui,
		fuse::ui::NodeHandle parent,
		model::AssistantBackendFactory factory = model::defaultAssistantBackendFactory(),
		model::AppSettings& settings = model::AppSettings::instance());
	/// Aborts a running generation and stops the worker thread.
	~AIAssistantPanel();
	AIAssistantPanel(const AIAssistantPanel&) = delete;
	AIAssistantPanel& operator=(const AIAssistantPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}
	std::string panelTitle() const
	{
		return "AI Assistant";
	}

	/// The function the next query is about (resets the KV cache when it changes).
	void setActiveFunction(std::string_view decompiledSource, std::string_view functionName);
	/// Clear history and context.
	void clear();

	/// Queue a model load (asynchronous; the label shows the outcome).
	bool loadModel(const std::string& ggufPath, bool useGpu = false);
	void unloadModel();
	bool isModelLoaded() const
	{
		return modelLoaded_;
	}
	/// Settings → ML into the worker, the settings bar and RETDEC_NEURAL_*.
	void applyMlSettingsFromApp();

	// ── User actions ─────────────────────────────────────────────────────────
	/// Send the query field (no-op when it is blank).
	void sendQuery();
	void stopGeneration();
	void clearHistory()
	{
		clear();
	}
	/// The Load button: a *.gguf file prompt, then loadModel().
	void browseModel();
	void setGpuEnabled(bool on);
	bool gpuEnabled() const
	{
		return gpuEnabled_;
	}
	void setSettingsBarVisible(bool on);
	bool settingsBarVisible() const
	{
		return settingsShown_;
	}
	void setThinkingMode(bool on);

	/// Apply what the worker reported (the panel posts this to itself; tests
	/// may call it). True when anything was applied.
	bool pumpEvents();

	// ── State ────────────────────────────────────────────────────────────────
	const std::vector<model::ChatMessage>& history() const
	{
		return history_;
	}
	/// The chat log as plain text, one message per paragraph.
	std::string chatText() const;
	std::string modelLabel() const;
	std::string statusText() const;
	bool inferenceRunning() const
	{
		return inferenceRunning_;
	}
	bool contextPending() const
	{
		return contextPending_;
	}
	/// The prompt the last send produced (context + query).
	const std::string& lastPrompt() const
	{
		return lastPrompt_;
	}
	model::InferenceWorker& worker()
	{
		return *worker_;
	}

	// ── Events ───────────────────────────────────────────────────────────────
	std::function<void(uint64_t address)> onAddressNavigated;

	// ── Test hooks ───────────────────────────────────────────────────────────
	fuse::ui::NodeHandle modelPathLabel() const
	{
		return modelLabel_;
	}
	fuse::ui::NodeHandle loadButton() const
	{
		return loadBtn_;
	}
	fuse::ui::NodeHandle gpuButton() const
	{
		return gpuBtn_;
	}
	fuse::ui::NodeHandle settingsButton() const
	{
		return settingsBtn_;
	}
	fuse::ui::NodeHandle clearButton() const
	{
		return clearBtn_;
	}
	fuse::ui::NodeHandle settingsBar() const
	{
		return settingsBar_;
	}
	fuse::ui::NodeHandle temperatureSpin() const
	{
		return tempSpin_;
	}
	fuse::ui::NodeHandle topPSpin() const
	{
		return topPSpin_;
	}
	fuse::ui::NodeHandle topKSpin() const
	{
		return topKSpin_;
	}
	fuse::ui::NodeHandle maxTokensSpin() const
	{
		return maxTokSpin_;
	}
	fuse::ui::NodeHandle thinkingCheck() const
	{
		return thinking_;
	}
	fuse::ui::NodeHandle chatLog() const
	{
		return chat_;
	}
	fuse::ui::NodeHandle statusLabel() const
	{
		return status_;
	}
	fuse::ui::NodeHandle queryInput() const
	{
		return input_;
	}
	fuse::ui::NodeHandle sendButton() const
	{
		return sendBtn_;
	}
	fuse::ui::NodeHandle stopButton() const
	{
		return stopBtn_;
	}
	FilePrompt& filePrompt()
	{
		return files_;
	}

private:
	void appendSystemMessage(std::string_view text);
	void rebuildChatLog();
	void updateLastMessageNode();
	void setInferenceBusy(bool busy);
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);

	fuse::ui::UiContext& ui_;
	model::AppSettings& settings_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, modelLabel_, loadBtn_, gpuBtn_, settingsBtn_, clearBtn_;
	fuse::ui::NodeHandle settingsBar_, tempSpin_, topPSpin_, topKSpin_, maxTokSpin_, thinking_;
	fuse::ui::NodeHandle chat_, status_, input_, sendBtn_, stopBtn_;
	std::vector<fuse::ui::NodeHandle> chatNodes_;
	FilePrompt files_;
	std::vector<model::ChatMessage> history_;
	std::string activeDecompiled_, activeFunctionName_, modelPath_, lastPrompt_;
	bool contextPending_ = false;
	bool gpuEnabled_ = false;
	bool inferenceRunning_ = false;
	bool modelLoaded_ = false;
	bool settingsShown_ = false;
	std::unique_ptr<model::InferenceWorker> worker_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_AI_ASSISTANT_PANEL_H
