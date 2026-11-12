/**
 * @file include/retdec/gui/model/dlg_ai_assistant.h
 * @brief AI Assistant logic: chat history types, prompt building, the
 *        RETDEC_NEURAL_* environment the panel publishes, and the inference
 *        worker (one background thread; talks to retdec::neural when the GUI
 *        is built with RETDEC_GUI_HAS_NEURAL, otherwise reports how to enable
 *        it).
 *
 * The worker replaces the Qt InferenceWorker + QThread pair. Requests are
 * queued to its thread; results come back as InferenceEvents the UI thread
 * drains with takeEvents() (onEventsReady fires on the worker thread so the
 * UI can wake its loop, e.g. with UiContext::post).
 *
 * The backend is an AssistantBackend made by a factory, so tests (and later
 * backends that stream tokens) plug in without retdec::neural.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DLG_AI_ASSISTANT_H
#define RETDEC_GUI_MODEL_DLG_AI_ASSISTANT_H

#include "retdec/gui/model/settings.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

enum class ChatRole
{
	User,
	Assistant,
	System,
	Thinking
};

struct ChatMessage
{
	ChatRole role = ChatRole::System;
	std::string text;
	bool isComplete = false;
};

/// Shown when the build has no inference backend (and returned as the error
/// of every request then).
inline constexpr std::string_view kAssistantNoBackend =
	"This build has no inference backend. Rebuild with neural support, then load a GGUF to use the assistant.";
/// The extra line the FuseUI panel shows in that case: how to get a backend.
inline constexpr std::string_view kAssistantEnableHint =
	"To enable it, configure RetDec with -DRETDEC_ENABLE_NEURAL=ON (the default in a full build; the standalone "
	"GUI build has no backend), rebuild retdec-gui, then load a .gguf model here or set its path in "
	"Settings -> ML.";
/// First message of a build with a backend.
inline constexpr std::string_view kAssistantLoadHint = "Load a Qwen 3.5 9B Q4_K_M GGUF to use the assistant.";

/// True when this build links retdec::neural (RETDEC_GUI_HAS_NEURAL).
bool assistantHasNeuralBackend();

/// The prompt sent for @p query: with pending context
/// "Context: function NAME\nSOURCE\n\nQuery: QUERY", else the query alone.
std::string buildAssistantPrompt(
	std::string_view query, std::string_view functionName, std::string_view decompiled, bool contextPending);

/// Chat line for a worker error: "Error: " prefixed unless it already holds "Error:".
std::string assistantErrorText(std::string_view error);

/// Chat colour of a role (as the Qt log's HTML): User #44aaff, Assistant
/// #aaffaa, others #888888.
uint32_t chatRoleColor(ChatRole role);

/**
 * @brief RETDEC_NEURAL_* variables the panel sets from Settings → ML (the Qt
 *        panel's qputenv calls): N_GPU_LAYERS (0 for CPU, else -1), CTX (when
 *        >= 512), MAX_TOKENS (when > 0), TEMPERATURE / TOP_P (4 decimals),
 *        TOP_K and THINKING ("1"/"0").
 */
std::map<std::string, std::string> assistantNeuralEnvironment(const MLSettings& ml, bool thinking);
/// Set every variable in the GUI's own environment (forge::os::setenv).
void applyProcessEnvironmentVariables(const std::map<std::string, std::string>& env);

/// Sampling parameters of one request.
struct AssistantGenerationParams
{
	float temperature = 0.6f;
	float topP = 0.95f;
	int topK = 20;
	int maxTokens = 512;
	bool thinking = false;
	bool reuseKvPrefix = false;
};

struct AssistantGenerationResult
{
	bool ok = false;
	std::string text; ///< the whole answer (also delivered piecewise through onPiece)
	int tokensGenerated = 0;
	std::string error;
};

/// A model the worker drives. Called only on the worker thread.
class AssistantBackend {
public:
	virtual ~AssistantBackend() = default;
	virtual bool loadModel(const std::string& path, int contextLength) = 0;
	virtual void unloadModel() = 0;
	virtual bool isLoaded() const = 0;
	/// Generate; call @p onPiece for each chunk of text as it is produced
	/// (return false from it to stop early).
	virtual AssistantGenerationResult generate(
		const std::string& prompt,
		const AssistantGenerationParams& params,
		const std::function<bool(std::string_view piece)>& onPiece) = 0;
};

/// Makes the backend for a model load; empty when the build has none.
using AssistantBackendFactory = std::function<std::unique_ptr<AssistantBackend>()>;
/// retdec::neural's Forge backend (mock when it is unavailable) with
/// RETDEC_GUI_HAS_NEURAL; an empty factory otherwise.
AssistantBackendFactory defaultAssistantBackendFactory();

/// What the worker reports back.
struct InferenceEvent
{
	enum class Kind
	{
		Token,         ///< text = piece
		Complete,      ///< tokens, tokPerSec
		Error,         ///< text = message
		SystemMessage, ///< text
		LoadFinished,  ///< ok, path, failKind ("missing", "load" or "")
	};
	Kind kind = Kind::Token;
	std::string text;
	int tokens = 0;
	double tokPerSec = 0.0;
	bool ok = false;
	std::string path;
	std::string failKind;
};

class InferenceWorker {
public:
	explicit InferenceWorker(AssistantBackendFactory factory = defaultAssistantBackendFactory());
	/// Aborts, then waits up to 10 s for the thread; a thread still inside a
	/// generation after that is detached (it owns its state) rather than
	/// blocking shutdown.
	~InferenceWorker();
	InferenceWorker(const InferenceWorker&) = delete;
	InferenceWorker& operator=(const InferenceWorker&) = delete;

	void setTemperature(float t);
	void setTopP(float p);
	void setTopK(int k);
	void setMaxTokens(int n);
	void setThinkingMode(bool on);
	AssistantGenerationParams params() const;

	/// Queue a generation. A pending abort cancels it (empty Complete).
	void startInference(std::string prompt);
	/// Ask the running generation (or, if none, the next request) to stop.
	void abortInference();
	/// Queue a model load (sets RETDEC_NEURAL_N_GPU_LAYERS first).
	void loadModel(std::string path, bool useGpu, int contextLength);
	void resetKvCache();
	void unloadModel();

	/// Events produced since the last call, in order.
	std::vector<InferenceEvent> takeEvents();
	/// Block until an event is available or @p timeout passes.
	bool waitForEvents(std::chrono::milliseconds timeout);
	/// Block until the request queue is empty and nothing runs.
	bool waitIdle(std::chrono::milliseconds timeout);

	/// Called on the worker thread whenever events were queued. Set it before
	/// queueing requests; it must be thread-safe (e.g. UiContext::post).
	void setEventsReadyCallback(std::function<void()> fn);

	struct State;

private:
	std::shared_ptr<State> st_;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DLG_AI_ASSISTANT_H
