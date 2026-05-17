/**
 * @file include/retdec/gui/panels/ai_assistant_panel.h
 * @brief AI Assistant panel — Qwen3 inference integrated into the RetDec Qt GUI.
 *
 * ## Features
 *
 *   - Chat-style message log with styled HTML bubbles (user / assistant / thinking)
 *   - Background inference thread — UI never blocks during generation
 *   - Streaming token output: each token is appended to the in-progress bubble
 *   - Qwen3Pipeline integration (local GGUF, optional GPU via OpenCL)
 *   - Context injection: active function's decompiled C source is prepended
 *   - "Thinking mode" toggle (Qwen3 `<think>…</think>` reasoning tokens)
 *   - Stop button to abort mid-generation
 *   - Model path picker + generation settings (temperature, top-P, max tokens)
 *
 * ## Thread model
 *
 *   InferenceWorker lives on a dedicated QThread and **owns** the Qwen3Pipeline.
 *   Model load, CUDA init, KV reset, and generate() all run on that thread so the
 *   pipeline is never used from two threads (Qwen3Pipeline is not thread-safe;
 *   CUDA contexts are also thread-bound).  Streaming emits tokenGenerated(QString)
 *   via queued connection to the GUI thread.
 *
 * ## Context prompt
 *
 *   When setActiveFunction() is called, the panel stores the decompiled source.
 *   The next user query is prefixed with:
 *     "Context: the following C function was recovered by RetDec:\n<code>\n\n"
 *   This context is sent only once per function change.
 */

#ifndef RETDEC_GUI_PANELS_AI_ASSISTANT_H
#define RETDEC_GUI_PANELS_AI_ASSISTANT_H

#include "retdec/gui/panels/panel_base.h"

#include <memory>
#include <string>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QScrollArea;
class QSpinBox;
class QTextBrowser;
class QTextEdit;
class QThread;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

namespace retdec::qwen3 {
class Qwen3Pipeline;
}

namespace retdec::gui::panels {

// ─── ChatRole ─────────────────────────────────────────────────────────────────

enum class ChatRole { User, Assistant, System, Thinking };

// ─── ChatMessage ──────────────────────────────────────────────────────────────

struct ChatMessage {
    ChatRole role;
    QString  text;
    bool     isComplete = false;  ///< false while streaming
};

// ─── InferenceWorker ──────────────────────────────────────────────────────────

/**
 * @brief Owns Qwen3Pipeline and runs load / generate on a background QThread.
 *
 * Emits tokenGenerated() for each streamed token (queued connection),
 * and responseComplete() or errorOccurred() when finished.
 */
class InferenceWorker : public QObject {
    Q_OBJECT

public:
    explicit InferenceWorker(QObject* parent = nullptr);
    ~InferenceWorker() override;

    // ── Inference settings (set before calling startInference) ────────────────
    void setTemperature(float t)      { temperature_ = t; }
    void setTopP(float p)             { topP_        = p; }
    void setTopK(int k)               { topK_        = k; }
    void setMaxTokens(int n)          { maxTokens_   = n; }
    void setThinkingMode(bool enable) { thinkingMode_ = enable; }

public slots:
    /// Begin inference on the given prompt string (called from GUI thread).
    void startInference(const QString& prompt);
    /// Abort the running inference (thread-safe via flag).
    void abortInference();
    /// Load or replace GGUF on the worker thread (queued from GUI).
    void loadModelSlot(const QString& path, bool useGpu, int ctxLen);
    void resetKvCacheSlot();
    /// Unload GGUF and release resources on the worker thread.
    void unloadModelSlot();

signals:
    /// Emitted for each generated text piece (queued → GUI thread).
    void tokenGenerated(const QString& piece);
    /// Emitted when generation completes normally.
    void responseComplete(int newTokens, double tokPerSec);
    /// Emitted on error (model not loaded, etc.).
    void errorOccurred(const QString& error);
    /// Chat log line from the worker (load progress, warnings).
    void systemStatusMessage(const QString& text);
    /// Load outcome: on failure \p path empty and \p failKind is "" or "OOM".
    void loadModelFinished(bool ok, const QString& path, const QString& failKind);

private:
    std::unique_ptr<retdec::qwen3::Qwen3Pipeline> pipeline_;
    float temperature_ = 0.7f;
    float topP_        = 0.9f;
    int   topK_        = 0;
    int   maxTokens_   = 512;
    bool  thinkingMode_ = false;
    std::atomic_bool abort_{false};
};

// ─── AIAssistantPanel ─────────────────────────────────────────────────────────

/**
 * @brief Full AI assistant panel with chat UI and Qwen3 backend.
 *
 * Layout:
 *   ┌─────────────────────────────────────────┐
 *   │  [Model: …] [Load] [GPU] [⚙ Settings]  │ ← top bar
 *   ├─────────────────────────────────────────┤
 *   │  chat log (scrollable HTML bubbles)      │
 *   │  ····································   │
 *   ├──────────────────────── progress bar ───│
 *   │  [input field …………]  [Send] [Stop]    │ ← input row
 *   └─────────────────────────────────────────┘
 */
class AIAssistantPanel : public PanelBase {
    Q_OBJECT

public:
    explicit AIAssistantPanel(QWidget* parent = nullptr);
    ~AIAssistantPanel() override;

    // ── Context injection ─────────────────────────────────────────────────────

    /**
     * @brief Provide decompiled context for the next query.
     *
     * The next user message will be prefixed with the function context.
     */
    void setActiveFunction(const QString& decompiledSource,
                           const QString& functionName);

    // ── PanelBase ─────────────────────────────────────────────────────────────

    void clear() override;

    // ── Model management (exposed for testing / main window) ──────────────────

    /// Queues load on the inference thread; returns true if the request was posted.
    bool loadModel(const QString& ggufPath, bool useGpu = false);
    void unloadModel();
    bool isModelLoaded() const;

    /**
     * @brief Apply ML settings from AppSettings to panel controls and worker sampling params.
     *
     * Call before loadModel() so context length and GPU toggle match the Settings dialog.
     */
    void applyMlSettingsFromApp();

signals:
    /// Emitted when a function address in the assistant's output is clicked.
    void addressNavigated(uint64_t address);
    /// Forwarded to InferenceWorker (cross-thread).
    void startInferenceRequest(const QString& prompt);

private slots:
    void onSendQuery();
    void onStopGeneration();
    void onClearHistory();
    void onLoadModel();
    void onSettingsToggled();
    void onThinkingToggled(bool enabled);
    void onTokenGenerated(const QString& piece);
    void onResponseComplete(int newTokens, double tokPerSec);
    void onInferenceError(const QString& error);
    void onWorkerSystemMessage(const QString& text);
    void onLoadModelFinished(bool ok, const QString& path, const QString& failKind);
    void onModelChanged(int index);  // for the combo box
    void onResponseChunk(const QString& chunk);
    void onResponseFinished();

private:
    // ── UI setup ─────────────────────────────────────────────────────────────

    void setupUI();
    void setupTopBar(QVBoxLayout* root);
    void setupSettingsBar(QVBoxLayout* root);
    void setupChatArea(QVBoxLayout* root);
    void setupInputRow(QVBoxLayout* root);

    // ── Chat log helpers ──────────────────────────────────────────────────────

    void appendUserBubble(const QString& text);
    void beginAssistantBubble();
    void appendToActiveBubble(const QString& piece);
    void closeActiveBubble();
    void appendSystemMessage(const QString& text);

    QString buildHtmlBubble(ChatRole role, const QString& text, bool complete) const;
    void rebuildChatLog();
    /// Rebuild HTML for all complete messages (excludes trailing incomplete assistant bubble).
    void refreshCompleteMessagesHtmlPrefix();
    void scrollToBottom();

    // ── Inference management ──────────────────────────────────────────────────

    QString buildPrompt(const QString& userQuery) const;
    void setInferenceBusy(bool busy);
    /// Apply buffered streamed tokens to the chat (coalesced to avoid setHtml storms).
    void flushStreamBufferToBubble();

    // ── Widgets ───────────────────────────────────────────────────────────────

    // Top bar
    QLabel*      modelPathLabel_   = nullptr;
    QPushButton* loadModelButton_  = nullptr;
    QPushButton* gpuButton_        = nullptr;  ///< Toggle GPU
    QPushButton* settingsButton_   = nullptr;

    // Settings bar (shown / hidden via settingsButton_)
    QWidget*        settingsBar_    = nullptr;
    QDoubleSpinBox* tempSpin_       = nullptr;
    QDoubleSpinBox* topPSpin_       = nullptr;
    QSpinBox*       maxTokensSpin_  = nullptr;
    QSpinBox*       contextLenSpin_ = nullptr;  ///< KV cache context window (tokens)
    QCheckBox*      thinkCheck_     = nullptr;

    // Chat area
    QTextBrowser* chatLog_     = nullptr;

    // Progress / status
    QProgressBar* thinkingBar_  = nullptr;
    QLabel*       statusLabel_  = nullptr;

    // Input row
    QLineEdit*   queryInput_  = nullptr;
    QPushButton* sendButton_  = nullptr;
    QPushButton* stopButton_  = nullptr;
    QPushButton* clearButton_ = nullptr;

    // ── State ─────────────────────────────────────────────────────────────────

    // For stub / combo-based selection (legacy compat)
    QComboBox* modelCombo_ = nullptr;

    std::vector<ChatMessage> history_;
    /// Cached bubbles for complete messages only; streaming appends one tail bubble in setHtml.
    QString htmlPrefixCompleteMessages_;
    bool  contextPending_     = false;  ///< Next query should include code context
    bool  gpuEnabled_         = false;
    bool  inferenceRunning_   = false;

    QString activeDecompiled_;
    QString activeFunctionName_;
    QString modelPath_;
    bool    modelLoaded_ = false;

    // ── Backend ───────────────────────────────────────────────────────────────

    InferenceWorker* worker_       = nullptr;  ///< Owned by workerThread_
    QThread*         workerThread_ = nullptr;

    /// Batched UI updates: one setHtml every ~50ms instead of per token (prevents Qt freezes/crashes).
    QTimer* streamCoalesceTimer_   = nullptr;
    QString streamCoalesceBuffer_;
};

} // namespace retdec::gui::panels

#endif // RETDEC_GUI_PANELS_AI_ASSISTANT_H
