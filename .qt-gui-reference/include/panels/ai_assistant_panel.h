/**
 * @file include/retdec/gui/panels/ai_assistant_panel.h
 * @brief AI Assistant Tools window — InferenceWorker talks to retdec::neural when linked.
 */

#ifndef RETDEC_GUI_PANELS_AI_ASSISTANT_H
#define RETDEC_GUI_PANELS_AI_ASSISTANT_H

#include "retdec/gui/panels/panel_base.h"

#include <atomic>
#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QThread;
class QVBoxLayout;
QT_END_NAMESPACE

namespace retdec::gui::panels {

enum class ChatRole { User, Assistant, System, Thinking };

struct ChatMessage {
    ChatRole role;
    QString  text;
    bool     isComplete = false;
};

class InferenceWorker : public QObject {
    Q_OBJECT

public:
    explicit InferenceWorker(QObject* parent = nullptr);
    ~InferenceWorker() override;

    void setTemperature(float t)      { temperature_ = t; }
    void setTopP(float p)             { topP_        = p; }
    void setTopK(int k)               { topK_        = k; }
    void setMaxTokens(int n)          { maxTokens_   = n; }
    void setThinkingMode(bool enable) { thinkingMode_ = enable; }

public slots:
    void startInference(const QString& prompt);
    void abortInference();
    void loadModelSlot(const QString& path, bool useGpu, int ctxLen);
    void resetKvCacheSlot();
    void unloadModelSlot();

signals:
    void tokenGenerated(const QString& piece);
    void responseComplete(int newTokens, double tokPerSec);
    void errorOccurred(const QString& error);
    void systemStatusMessage(const QString& text);
    void loadModelFinished(bool ok, const QString& path, const QString& failKind);

private:
    float temperature_ = 0.7f;
    float topP_        = 0.9f;
    int   topK_        = 0;
    int   maxTokens_   = 512;
    bool  thinkingMode_ = false;
    std::atomic_bool abort_{false};
};

class AIAssistantPanel : public PanelBase {
    Q_OBJECT

public:
    explicit AIAssistantPanel(QWidget* parent = nullptr);
    ~AIAssistantPanel() override;

    void setActiveFunction(const QString& decompiledSource,
                           const QString& functionName);
    void clear() override;

    bool loadModel(const QString& ggufPath, bool useGpu = false);
    void unloadModel();
    bool isModelLoaded() const;
    void applyMlSettingsFromApp();

signals:
    void addressNavigated(uint64_t address);
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
    void onModelChanged(int index);
    void onResponseChunk(const QString& chunk);
    void onResponseFinished();

private:
    void setupUI();
    void setupTopBar(QVBoxLayout* root);
    void setupSettingsBar(QVBoxLayout* root);
    void setupChatArea(QVBoxLayout* root);
    void setupInputRow(QVBoxLayout* root);
    void appendSystemMessage(const QString& text);
    void rebuildChatLog();
    void scrollToBottom();
    QString buildPrompt(const QString& userQuery) const;
    void setInferenceBusy(bool busy);

    QLabel*       statusLabel_     = nullptr;
    QLabel*       modelPathLabel_  = nullptr;
    QPushButton*  loadModelButton_ = nullptr;
    QPushButton*  gpuButton_       = nullptr;
    QPushButton*  settingsButton_  = nullptr;
    QWidget*      settingsBar_     = nullptr;
    QTextBrowser* chatLog_         = nullptr;
    QLineEdit*    queryInput_      = nullptr;
    QPushButton*  sendButton_      = nullptr;
    QPushButton*  stopButton_      = nullptr;
    QPushButton*  clearButton_     = nullptr;

    std::vector<ChatMessage> history_;
    QString activeDecompiled_;
    QString activeFunctionName_;
    QString modelPath_;
    bool contextPending_   = false;
    bool gpuEnabled_       = false;
    bool inferenceRunning_ = false;
    bool modelLoaded_      = false;

    InferenceWorker* worker_        = nullptr;
    QThread*         workerThread_  = nullptr;
};

} // namespace retdec::gui::panels

#endif // RETDEC_GUI_PANELS_AI_ASSISTANT_H
