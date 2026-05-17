/**
 * @file src/gui/panels/ai_assistant_panel.cpp
 * @brief AI Assistant panel — full implementation with Qwen3 backend.
 *
 * Thread model
 * ────────────
 * The Qt main thread owns the AIAssistantPanel and all its widgets.
 * A QThread is created at construction; the InferenceWorker is moved to it.
 * Inference is started via a queued signal-slot connection:
 *   GUI thread    emit startInferenceRequest(prompt)
 *   Worker thread InferenceWorker::startInference(prompt)
 *   Worker thread emit tokenGenerated(piece)   → queued → GUI thread slot
 *   GUI thread    onTokenGenerated(piece)       → appends to active bubble
 *
 * The abort flag is a std::atomic<bool> so it is safe to set from the GUI
 * thread while the worker reads it.
 *
 * Qwen3Pipeline is used only on the worker thread (load, CUDA, generate).
 *
 * Chat log rendering
 * ──────────────────
 * Messages are stored in history_ as ChatMessage structs.  The chat log is
 * a QTextBrowser that is rebuilt in full (rebuildChatLog()) whenever a
 * message is added or updated.  For streaming, the last (incomplete)
 * assistant message is appended to on each token.  A full rebuild is only
 * triggered on message boundaries to avoid flicker during streaming.
 *
 * HTML bubbles use a simple inline-style approach for maximum portability
 * across Qt stylesheet themes.
 */

#include <memory>
#include <new>
#include <stdexcept>
#include "retdec/gui/panels/ai_assistant_panel.h"
#include "retdec/gui/settings/settings.h"
#include "retdec/qwen3/qwen3_pipeline.h"
#include "retdec/qwen3/qwen3_sampler.h"

#include <atomic>

#include <QByteArray>
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QSignalBlocker>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace retdec::gui::panels {

// ─── InferenceWorker ──────────────────────────────────────────────────────────

InferenceWorker::InferenceWorker(QObject* parent)
    : QObject(parent) {}

InferenceWorker::~InferenceWorker() = default;

void InferenceWorker::loadModelSlot(const QString& path, bool useGpu, int ctxLen) {
    if (pipeline_) {
        pipeline_->unloadModel();
        pipeline_.reset();
    }

    pipeline_ = std::make_unique<retdec::qwen3::Qwen3Pipeline>();
    pipeline_->setMaxContextLen(ctxLen);

    if (useGpu) {
        emit systemStatusMessage("Initialising CUDA GPU acceleration…");
        if (!pipeline_->enableCUDA()) {
            emit systemStatusMessage(
                "Warning: CUDA unavailable – " +
                QString::fromStdString(pipeline_->lastError()) +
                "  Falling back to CPU-only.");
        }
    }

    emit systemStatusMessage(
        QString("Loading model (context=%1 tokens)…").arg(ctxLen));

    try {
        if (!pipeline_->load(std::string(path.toUtf8().constData()))) {
            QString err = QString::fromStdString(pipeline_->lastError());
            emit systemStatusMessage("Failed to load model: " + err);
            emit loadModelFinished(false, QString(), QString());
            pipeline_.reset();
            return;
        }
    } catch (const std::bad_alloc&) {
        emit systemStatusMessage(
            "Out of memory loading model!  Try reducing the Context Length "
            "in ⚙ Settings before loading, or close other applications.");
        emit loadModelFinished(false, QString(), QStringLiteral("OOM"));
        pipeline_.reset();
        return;
    } catch (const std::exception& ex) {
        emit systemStatusMessage(QString("Exception loading model: ") +
                                 QString::fromLatin1(ex.what()));
        emit loadModelFinished(false, QString(), QString());
        pipeline_.reset();
        return;
    }

    const std::string& warn = pipeline_->lastError();
    if (!warn.empty() && warn.rfind("Warning:", 0) == 0)
        emit systemStatusMessage(QString::fromStdString(warn));

    QString shortName = QFileInfo(path).fileName();
    const QString devNote =
        (useGpu && pipeline_->isCUDAEnabled())
            ? QStringLiteral(" (CUDA device OK; matmuls on CPU)")
            : (useGpu ? QStringLiteral(" (CPU — CUDA init failed)")
                      : QStringLiteral(" (CPU only)"));
    emit systemStatusMessage("Model loaded: " + shortName + devNote +
                             QString("  [context=%1 tok]").arg(ctxLen));
    emit loadModelFinished(true, path, QString());
}

void InferenceWorker::resetKvCacheSlot() {
    if (pipeline_) pipeline_->resetKvCache();
}

void InferenceWorker::unloadModelSlot() {
    if (pipeline_) {
        pipeline_->unloadModel();
        pipeline_.reset();
    }
}

void InferenceWorker::abortInference() {
    abort_.store(true, std::memory_order_relaxed);
}

void InferenceWorker::startInference(const QString& prompt) {
    if (!pipeline_ || !pipeline_->isLoaded()) {
        emit errorOccurred("No model loaded. Use 'Load Model' to select a GGUF file.");
        return;
    }

    abort_.store(false, std::memory_order_relaxed);

    retdec::qwen3::PipelineGenerateOptions opts;
    opts.temperature       = temperature_;
    opts.topP              = topP_;
    opts.topK              = topK_;
    opts.maxNewTokens      = maxTokens_;
    opts.enableThinking    = thinkingMode_;
    opts.seed              = 42;

    // Batch decoded pieces before cross-thread emit so we do not queue one Qt event per token
    // (that floods the GUI event loop). Flush by token count so UTF-8 is never split mid-codepoint.
    std::string streamBatch;
    streamBatch.reserve(1024);
    int tokensSinceEmit = 0;
    constexpr int kTokensPerEmit = 8;
    auto* self = this;
    opts.streamCallback = [self, &streamBatch, &tokensSinceEmit](const std::string& piece) -> bool {
        if (self->abort_.load(std::memory_order_relaxed)) return false;
        streamBatch += piece;
        if (++tokensSinceEmit >= kTokensPerEmit) {
            emit self->tokenGenerated(QString::fromUtf8(
                QByteArray(streamBatch.data(), static_cast<int>(streamBatch.size()))));
            streamBatch.clear();
            tokensSinceEmit = 0;
        }
        return true;
    };

    try {
        auto result = pipeline_->generate(prompt.toUtf8().constData(), opts);
        if (!streamBatch.empty()) {
            emit tokenGenerated(QString::fromUtf8(
                QByteArray(streamBatch.data(), static_cast<int>(streamBatch.size()))));
        }
        if (!abort_.load(std::memory_order_relaxed)) {
            emit responseComplete(result.newTokens, result.tokPerSec);
        } else {
            emit responseComplete(result.newTokens, 0.0);
        }
    } catch (const std::exception& ex) {
        emit errorOccurred(QString::fromUtf8(ex.what()));
    } catch (...) {
        emit errorOccurred("Unknown inference error.");
    }
}

// ─── AIAssistantPanel ─────────────────────────────────────────────────────────

AIAssistantPanel::AIAssistantPanel(QWidget* parent)
    : PanelBase("AI Assistant", parent) {
    setupUI();

    // Create worker and move to background thread (pipeline lives on worker only)
    workerThread_ = new QThread(this);
    worker_       = new InferenceWorker();  // no parent — will be moved
    worker_->moveToThread(workerThread_);

    // Cross-thread signal/slot wiring (all queued automatically)
    connect(this,   &AIAssistantPanel::startInferenceRequest,
            worker_, &InferenceWorker::startInference,
            Qt::QueuedConnection);
    connect(worker_, &InferenceWorker::tokenGenerated,
            this,    &AIAssistantPanel::onTokenGenerated,
            Qt::QueuedConnection);
    connect(worker_, &InferenceWorker::responseComplete,
            this,    &AIAssistantPanel::onResponseComplete,
            Qt::QueuedConnection);
    connect(worker_, &InferenceWorker::errorOccurred,
            this,    &AIAssistantPanel::onInferenceError,
            Qt::QueuedConnection);
    connect(worker_, &InferenceWorker::systemStatusMessage,
            this,    &AIAssistantPanel::onWorkerSystemMessage,
            Qt::QueuedConnection);
    connect(worker_, &InferenceWorker::loadModelFinished,
            this,    &AIAssistantPanel::onLoadModelFinished,
            Qt::QueuedConnection);

    // Clean up worker when thread finishes
    connect(workerThread_, &QThread::finished,
            worker_,       &QObject::deleteLater);
    workerThread_->start();

    streamCoalesceTimer_ = new QTimer(this);
    streamCoalesceTimer_->setSingleShot(true);
    streamCoalesceTimer_->setInterval(48);
    connect(streamCoalesceTimer_, &QTimer::timeout,
            this, &AIAssistantPanel::flushStreamBufferToBubble);
}

AIAssistantPanel::~AIAssistantPanel() {
    // Do not BlockingQueuedConnection into the worker here: if inference is
    // running, the worker thread may be blocked inside generate() and would
    // not process unload until generate returns (deadlock with abort path).
    if (worker_) worker_->abortInference();
    if (workerThread_) {
        workerThread_->quit();
        // Avoid destroying a QThread child while isRunning() (undefined behavior).
        // Bounded wait so a stuck worker does not hang shutdown forever.
        if (!workerThread_->wait(QDeadlineTimer(60000)))
            workerThread_->terminate();
        workerThread_->wait(QDeadlineTimer(5000));
    }
    // Worker is deleteLater-connected to thread finished; ~InferenceWorker
    // destroys the pipeline on the worker thread while the loop still runs
    // pending deferred deletes before exit, releasing GPU/RAM.
}

// ── UI setup ──────────────────────────────────────────────────────────────────

void AIAssistantPanel::setupUI() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    setupTopBar(root);
    setupSettingsBar(root);
    setupChatArea(root);

    root->addWidget(thinkingBar_ = new QProgressBar(this));
    thinkingBar_->setRange(0, 0);   // indeterminate
    thinkingBar_->setFixedHeight(3);
    thinkingBar_->hide();

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("statusLabel");
    statusLabel_->hide();
    root->addWidget(statusLabel_);

    setupInputRow(root);
}

void AIAssistantPanel::setupTopBar(QVBoxLayout* root) {
    auto* bar    = new QWidget(this);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    modelPathLabel_ = new QLabel("No model loaded", bar);
    modelPathLabel_->setObjectName("aiAssistantModelPathLabel");
    modelPathLabel_->setStyleSheet("color: #585b70; font-style: italic;");

    loadModelButton_ = new QPushButton("Load Model…", bar);
    loadModelButton_->setFixedWidth(110);

    gpuButton_ = new QPushButton("GPU: OFF", bar);
    gpuButton_->setCheckable(true);
    gpuButton_->setFixedWidth(80);

    settingsButton_ = new QPushButton("⚙", bar);
    settingsButton_->setCheckable(true);
    settingsButton_->setFixedWidth(30);
    settingsButton_->setToolTip("Show/hide generation settings");

    modelCombo_ = new QComboBox(bar);
    modelCombo_->addItems({"Local (GGUF)", "GPT-4o", "Claude 3.5 Sonnet"});
    modelCombo_->setFixedWidth(150);
    modelCombo_->setVisible(false); // Hidden when real model is loaded

    clearButton_ = new QPushButton("Clear", bar);
    clearButton_->setFixedWidth(60);

    layout->addWidget(loadModelButton_);
    layout->addWidget(gpuButton_);
    layout->addWidget(modelPathLabel_, 1);
    layout->addWidget(settingsButton_);
    layout->addWidget(clearButton_);
    root->addWidget(bar);

    connect(loadModelButton_, &QPushButton::clicked,
            this, &AIAssistantPanel::onLoadModel);
    connect(gpuButton_, &QPushButton::toggled, this, [this](bool on) {
        gpuEnabled_ = on;
        gpuButton_->setText(on ? "GPU: ON" : "GPU: OFF");
    });
    connect(settingsButton_, &QPushButton::toggled,
            this, &AIAssistantPanel::onSettingsToggled);
    connect(clearButton_, &QPushButton::clicked,
            this, &AIAssistantPanel::onClearHistory);
    connect(modelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIAssistantPanel::onModelChanged);
}

void AIAssistantPanel::setupSettingsBar(QVBoxLayout* root) {
    settingsBar_ = new QWidget(this);
    settingsBar_->hide();

    auto* layout = new QHBoxLayout(settingsBar_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto addSpin = [&](const QString& label, QDoubleSpinBox*& spin,
                       double min, double max, double val, double step) {
        layout->addWidget(new QLabel(label, settingsBar_));
        spin = new QDoubleSpinBox(settingsBar_);
        spin->setRange(min, max);
        spin->setValue(val);
        spin->setSingleStep(step);
        spin->setDecimals(2);
        spin->setFixedWidth(70);
        layout->addWidget(spin);
    };

    addSpin("Temp:", tempSpin_, 0.0, 2.0, 0.7, 0.05);
    addSpin("Top-P:", topPSpin_, 0.0, 1.0, 0.9, 0.05);

    layout->addWidget(new QLabel("Max tokens:", settingsBar_));
    maxTokensSpin_ = new QSpinBox(settingsBar_);
    maxTokensSpin_->setRange(32, 8192);
    maxTokensSpin_->setValue(512);
    maxTokensSpin_->setFixedWidth(70);
    layout->addWidget(maxTokensSpin_);

    thinkCheck_ = new QCheckBox("Thinking mode", settingsBar_);
    layout->addWidget(thinkCheck_);

    layout->addWidget(new QLabel("Context:", settingsBar_));
    contextLenSpin_ = new QSpinBox(settingsBar_);
    contextLenSpin_->setRange(512, 32768);
    contextLenSpin_->setValue(4096);
    contextLenSpin_->setSingleStep(512);
    contextLenSpin_->setSuffix(" tok");
    contextLenSpin_->setFixedWidth(100);
    contextLenSpin_->setToolTip(
        "Maximum KV cache context length.\n"
        "Lower = less RAM used.  Must be set before loading the model.\n"
        "4096 is safe for most systems;  8192 for 32 GB+ RAM.");
    layout->addWidget(contextLenSpin_);

    layout->addStretch();

    connect(thinkCheck_, &QCheckBox::toggled,
            this, &AIAssistantPanel::onThinkingToggled);

    root->addWidget(settingsBar_);
}

void AIAssistantPanel::setupChatArea(QVBoxLayout* root) {
    chatLog_ = new QTextBrowser(this);
    chatLog_->setObjectName("aiAssistantChatLog");
    chatLog_->setOpenLinks(false);
    chatLog_->setStyleSheet(
        "QTextBrowser { background: #1e1e2e; color: #cdd6f4; "
        "border: none; font-family: 'Segoe UI', sans-serif; font-size: 13px; }");
    root->addWidget(chatLog_, 1);

    // Welcome message
    appendSystemMessage("Welcome to the RetDec AI Assistant powered by Qwen3.\n"
                        "Load a GGUF model to enable intelligent decompilation assistance.");
}

void AIAssistantPanel::setupInputRow(QVBoxLayout* root) {
    auto* row    = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    queryInput_ = new QLineEdit(row);
    queryInput_->setObjectName("aiAssistantQueryInput");
    queryInput_->setPlaceholderText("Ask about the binary or the active function…");

    sendButton_ = new QPushButton("Send", row);
    sendButton_->setObjectName("aiAssistantSendButton");
    sendButton_->setDefault(true);
    sendButton_->setFixedWidth(70);

    stopButton_ = new QPushButton("Stop", row);
    stopButton_->setFixedWidth(60);
    stopButton_->setEnabled(false);

    layout->addWidget(queryInput_);
    layout->addWidget(sendButton_);
    layout->addWidget(stopButton_);
    root->addWidget(row);

    connect(sendButton_,  &QPushButton::clicked,
            this, &AIAssistantPanel::onSendQuery);
    connect(stopButton_,  &QPushButton::clicked,
            this, &AIAssistantPanel::onStopGeneration);
    connect(queryInput_,  &QLineEdit::returnPressed,
            this, &AIAssistantPanel::onSendQuery);
}

// ── Context / function ────────────────────────────────────────────────────────

void AIAssistantPanel::setActiveFunction(const QString& decompiledSource,
                                          const QString& functionName) {
    activeDecompiled_   = decompiledSource;
    activeFunctionName_ = functionName;
    contextPending_     = !decompiledSource.isEmpty();
}

// ── Chat log helpers ──────────────────────────────────────────────────────────

QString AIAssistantPanel::buildHtmlBubble(ChatRole role, const QString& text,
                                           bool /*complete*/) const {
    QString bg, nameColor, roleName;
    switch (role) {
    case ChatRole::User:
        bg = "#313244"; nameColor = "#89b4fa"; roleName = "You";
        break;
    case ChatRole::Assistant:
        bg = "#1e1e2e"; nameColor = "#a6e3a1"; roleName = "Qwen3";
        break;
    case ChatRole::System:
        bg = "#181825"; nameColor = "#585b70"; roleName = "System";
        break;
    case ChatRole::Thinking:
        bg = "#11111b"; nameColor = "#f9e2af"; roleName = "Thinking…";
        break;
    }

    QString escaped = text.toHtmlEscaped().replace("\n", "<br>");

    return QString(
        "<div style='margin:6px 0; padding:8px 12px; "
        "background:%1; border-radius:8px;'>"
        "<b style='color:%2;'>%3</b><br>"
        "<span style='color:#cdd6f4;'>%4</span>"
        "</div>")
        .arg(bg, nameColor, roleName, escaped);
}

void AIAssistantPanel::refreshCompleteMessagesHtmlPrefix() {
    htmlPrefixCompleteMessages_.clear();
    const int n = static_cast<int>(history_.size());
    for (int i = 0; i < n; ++i) {
        const auto& msg = history_[static_cast<std::size_t>(i)];
        if (i == n - 1 && !msg.isComplete)
            break;
        htmlPrefixCompleteMessages_ += buildHtmlBubble(msg.role, msg.text, msg.isComplete);
    }
}

void AIAssistantPanel::rebuildChatLog() {
    refreshCompleteMessagesHtmlPrefix();
    QString html = "<html><body style='margin:0; padding:0;'>" + htmlPrefixCompleteMessages_;
    if (!history_.empty() && !history_.back().isComplete) {
        html += buildHtmlBubble(history_.back().role, history_.back().text, false);
    }
    html += "</body></html>";
    chatLog_->setHtml(html);
    scrollToBottom();
}

void AIAssistantPanel::scrollToBottom() {
    chatLog_->verticalScrollBar()->setValue(
        chatLog_->verticalScrollBar()->maximum());
}

void AIAssistantPanel::appendUserBubble(const QString& text) {
    history_.push_back({ChatRole::User, text, true});
    rebuildChatLog();
}

void AIAssistantPanel::beginAssistantBubble() {
    history_.push_back({ChatRole::Assistant, QString(), false});
    rebuildChatLog();
}

void AIAssistantPanel::appendToActiveBubble(const QString& piece) {
    if (!history_.empty() && !history_.back().isComplete) {
        history_.back().text += piece;
        // Only rebuild the streaming tail; prefix is fixed until the bubble completes.
        QString html = "<html><body style='margin:0;padding:0;'>" + htmlPrefixCompleteMessages_
            + buildHtmlBubble(history_.back().role, history_.back().text, false) + "</body></html>";
        chatLog_->setHtml(html);
        scrollToBottom();
    }
}

void AIAssistantPanel::flushStreamBufferToBubble() {
    if (streamCoalesceBuffer_.isEmpty()) return;
    QString chunk;
    chunk.swap(streamCoalesceBuffer_);
    appendToActiveBubble(chunk);
}

void AIAssistantPanel::closeActiveBubble() {
    if (!history_.empty() && !history_.back().isComplete)
        history_.back().isComplete = true;
    rebuildChatLog();
}

void AIAssistantPanel::appendSystemMessage(const QString& text) {
    history_.push_back({ChatRole::System, text, true});
    rebuildChatLog();
}

// ── Model management ──────────────────────────────────────────────────────────

void AIAssistantPanel::applyMlSettingsFromApp() {
    using IL = retdec::gui::MLSettings::InferenceDevice;
    const auto& ml = retdec::gui::AppSettings::instance().ml;

    if (tempSpin_)
        tempSpin_->setValue(ml.temperature);
    if (topPSpin_)
        topPSpin_->setValue(ml.topP);
    if (maxTokensSpin_)
        maxTokensSpin_->setValue(ml.maxNewTokens);
    if (contextLenSpin_) {
        int ctx = ml.contextLength;
        const int lo = contextLenSpin_->minimum();
        const int hi = contextLenSpin_->maximum();
        if (ctx < lo) ctx = lo;
        if (ctx > hi) ctx = hi;
        contextLenSpin_->setValue(ctx);
    }

    const bool useGpu =
        (ml.inferenceDevice == IL::GPU || ml.inferenceDevice == IL::Auto);
    gpuEnabled_ = useGpu;
    if (gpuButton_) {
        QSignalBlocker b(gpuButton_);
        gpuButton_->setChecked(useGpu);
        gpuButton_->setText(useGpu ? QStringLiteral("GPU: ON")
                                   : QStringLiteral("GPU: OFF"));
    }

    if (worker_) {
        worker_->setTemperature(static_cast<float>(ml.temperature));
        worker_->setTopP(static_cast<float>(ml.topP));
        worker_->setTopK(ml.topK);
        worker_->setMaxTokens(ml.maxNewTokens);
    }
}

bool AIAssistantPanel::loadModel(const QString& ggufPath, bool useGpu) {
    if (!worker_) return false;
    int ctxLen = contextLenSpin_ ? contextLenSpin_->value() : 4096;
    QMetaObject::invokeMethod(
        worker_, "loadModelSlot", Qt::QueuedConnection,
        Q_ARG(QString, ggufPath), Q_ARG(bool, useGpu), Q_ARG(int, ctxLen));
    return true;
}

void AIAssistantPanel::unloadModel() {
    if (worker_) {
        QMetaObject::invokeMethod(worker_, "unloadModelSlot",
                                  Qt::BlockingQueuedConnection);
    }
    modelLoaded_ = false;
    modelPath_.clear();
    modelPathLabel_->setText("No model loaded");
    modelPathLabel_->setStyleSheet("color: #585b70; font-style: italic;");
}

bool AIAssistantPanel::isModelLoaded() const {
    return modelLoaded_;
}

// ── Slot implementations ──────────────────────────────────────────────────────

void AIAssistantPanel::onLoadModel() {
    QString path = QFileDialog::getOpenFileName(
        this, "Load Qwen3 GGUF Model", QString(),
        "GGUF Models (*.gguf);;All Files (*)");
    if (path.isEmpty()) return;
    loadModel(path, gpuEnabled_);
}

void AIAssistantPanel::onSettingsToggled() {
    settingsBar_->setVisible(settingsButton_->isChecked());
}

void AIAssistantPanel::onThinkingToggled(bool enabled) {
    if (worker_) worker_->setThinkingMode(enabled);
}

void AIAssistantPanel::onSendQuery() {
    if (inferenceRunning_) return;
    QString query = queryInput_->text().trimmed();
    if (query.isEmpty()) return;

    queryInput_->clear();
    appendUserBubble(query);

    // Propagate settings to the worker
    if (worker_) {
        worker_->setTemperature(tempSpin_ ? static_cast<float>(tempSpin_->value()) : 0.7f);
        worker_->setTopP       (topPSpin_ ? static_cast<float>(topPSpin_->value()) : 0.9f);
        worker_->setTopK       (retdec::gui::AppSettings::instance().ml.topK);
        worker_->setMaxTokens  (maxTokensSpin_ ? maxTokensSpin_->value() : 512);
    }

    beginAssistantBubble();
    setInferenceBusy(true);

    emit startInferenceRequest(buildPrompt(query));
}

void AIAssistantPanel::onStopGeneration() {
    if (worker_) worker_->abortInference();
}

void AIAssistantPanel::onClearHistory() {
    history_.clear();
    chatLog_->clear();
    appendSystemMessage("Conversation cleared.");
    if (worker_) {
        QMetaObject::invokeMethod(worker_, "resetKvCacheSlot",
                                  Qt::QueuedConnection);
    }
}

void AIAssistantPanel::onModelChanged(int /*index*/) {
    // Combo-box selection — only relevant when actual API backends are wired.
}

void AIAssistantPanel::onTokenGenerated(const QString& piece) {
    streamCoalesceBuffer_ += piece;
    if (streamCoalesceTimer_ && !streamCoalesceTimer_->isActive())
        streamCoalesceTimer_->start();
}

void AIAssistantPanel::onResponseComplete(int newTokens, double tokPerSec) {
    if (streamCoalesceTimer_) streamCoalesceTimer_->stop();
    flushStreamBufferToBubble();
    closeActiveBubble();
    setInferenceBusy(false);
    QString stats = QString("Generated %1 tokens").arg(newTokens);
    if (tokPerSec > 0.0)
        stats += QString(" (%.1f tok/s)").arg(tokPerSec);
    statusLabel_->setText(stats);
    statusLabel_->show();
}

void AIAssistantPanel::onInferenceError(const QString& error) {
    if (streamCoalesceTimer_) streamCoalesceTimer_->stop();
    flushStreamBufferToBubble();
    closeActiveBubble();
    appendSystemMessage("Error: " + error);
    setInferenceBusy(false);
}

void AIAssistantPanel::onWorkerSystemMessage(const QString& text) {
    appendSystemMessage(text);
}

void AIAssistantPanel::onLoadModelFinished(bool ok, const QString& path,
                                           const QString& failKind) {
    if (!ok) {
        modelLoaded_ = false;
        modelPath_.clear();
        modelPathLabel_->setText(failKind == QStringLiteral("OOM")
                                     ? "Load failed (OOM)"
                                     : "Load failed");
        modelPathLabel_->setStyleSheet("color: #585b70; font-style: italic;");
        return;
    }
    modelLoaded_ = true;
    modelPath_   = path;
    QString shortName = QFileInfo(path).fileName();
    modelPathLabel_->setText(shortName);
    modelPathLabel_->setStyleSheet("color: #a6e3a1;");
}

// Called from legacy connection in remaining_panels.cpp slot form
void AIAssistantPanel::onResponseChunk(const QString& chunk) {
    onTokenGenerated(chunk);
}
void AIAssistantPanel::onResponseFinished() {
    if (streamCoalesceTimer_) streamCoalesceTimer_->stop();
    flushStreamBufferToBubble();
    closeActiveBubble();
    setInferenceBusy(false);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

QString AIAssistantPanel::buildPrompt(const QString& userQuery) const {
    QString prior;
    const int n = static_cast<int>(history_.size());
    if (n >= 3) {
        const int upTo = n - 2;
        for (int i = 0; i + 1 < upTo;) {
            const auto& a = history_[static_cast<std::size_t>(i)];
            const auto& b = history_[static_cast<std::size_t>(i + 1)];
            if (a.role == ChatRole::User && a.isComplete && b.role == ChatRole::Assistant &&
                b.isComplete) {
                prior += QStringLiteral("User: %1\n\nAssistant: %2\n\n").arg(a.text, b.text);
                i += 2;
            } else {
                ++i;
            }
        }
    }

    QString body = userQuery;
    if (!prior.isEmpty())
        body = QStringLiteral("Previous conversation:\n%1---\n%2").arg(prior, userQuery);

    if (contextPending_ && !activeDecompiled_.isEmpty()) {
        const_cast<AIAssistantPanel*>(this)->contextPending_ = false;
        // Cap context size: huge functions + full HTML chat would OOM the tokenizer / UI.
        static constexpr int kMaxDecompiledChars = 200000;
        QString code = activeDecompiled_;
        if (code.size() > kMaxDecompiledChars) {
            code.truncate(kMaxDecompiledChars);
            code += QStringLiteral("\n\n/* … truncated (%1 chars) for assistant context … */\n")
                        .arg(activeDecompiled_.size());
        }
        return QString("Context — decompiled function '%1':\n```c\n%2\n```\n\n%3")
            .arg(activeFunctionName_, code, body);
    }
    return body;
}

void AIAssistantPanel::setInferenceBusy(bool busy) {
    inferenceRunning_ = busy;
    if (busy) {
        if (streamCoalesceTimer_) streamCoalesceTimer_->stop();
        streamCoalesceBuffer_.clear();
    }
    sendButton_->setEnabled(!busy);
    stopButton_->setEnabled(busy);
    queryInput_->setEnabled(!busy);
    if (busy) {
        thinkingBar_->show();
        statusLabel_->hide();
    } else {
        thinkingBar_->hide();
    }
}

void AIAssistantPanel::clear() {
    onClearHistory();
    queryInput_->clear();
    activeDecompiled_.clear();
    activeFunctionName_.clear();
    contextPending_ = false;
    statusLabel_->hide();
}

} // namespace retdec::gui::panels
