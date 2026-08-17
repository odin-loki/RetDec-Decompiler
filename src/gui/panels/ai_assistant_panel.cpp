/**
 * @file src/gui/panels/ai_assistant_panel.cpp
 * @brief AI Assistant panel — InferenceWorker talks to retdec::neural when linked.
 */

#include "retdec/gui/panels/ai_assistant_panel.h"

#ifdef RETDEC_GUI_HAS_NEURAL
#include "retdec/neural/inference.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTextBrowser>
#include <QThread>
#include <QVBoxLayout>

#ifdef RETDEC_GUI_HAS_NEURAL
#include <memory>
#include <mutex>
#include <unordered_map>
#endif

namespace retdec::gui::panels {

namespace {

constexpr const char* kNoBackend =
	"No inference backend configured. Neural refinement (llama.cpp) is not yet available.";

#ifdef RETDEC_GUI_HAS_NEURAL
constexpr const char* kLoadGgufHint = "Load a Qwen 3.5 9B Q4_K_M GGUF to use the assistant.";

std::mutex g_inferenceMutex;
std::unordered_map<InferenceWorker*, std::shared_ptr<retdec::neural::Inference>> g_backends;
#endif

} // namespace

InferenceWorker::InferenceWorker(QObject* parent): QObject(parent) {}

InferenceWorker::~InferenceWorker()
{
#ifdef RETDEC_GUI_HAS_NEURAL
	std::lock_guard<std::mutex> lock(g_inferenceMutex);
	g_backends.erase(this);
#endif
}

void InferenceWorker::startInference(const QString& prompt)
{
	if (abort_.load())
	{
		abort_.store(false);
		return;
	}

#ifdef RETDEC_GUI_HAS_NEURAL
	std::shared_ptr<retdec::neural::Inference> backend;
	{
		std::lock_guard<std::mutex> lock(g_inferenceMutex);
		auto it = g_backends.find(this);
		if (it != g_backends.end() && it->second && it->second->isLoaded()) backend = it->second;
	}
	if (!backend)
	{
		emit errorOccurred(QStringLiteral("No model loaded"));
		return;
	}

	retdec::neural::GenerationConfig cfg;
	cfg.temperature = temperature_;
	cfg.topP = topP_;
	cfg.topK = topK_;
	cfg.maxTokens = maxTokens_;
	cfg.thinkingMode = thinkingMode_;

	const retdec::neural::GenerationResult result = backend->generate(prompt.toStdString(), cfg);
	if (!result.ok)
	{
		const QString err =
			result.error.empty() ? QStringLiteral("generation failed") : QString::fromStdString(result.error);
		emit errorOccurred(err);
		return;
	}

	emit tokenGenerated(QString::fromStdString(result.text));
	emit responseComplete(result.tokensGenerated, 0.0);
#else
	Q_UNUSED(prompt);
	emit errorOccurred(QString::fromUtf8(kNoBackend));
#endif
}

void InferenceWorker::abortInference()
{
	abort_.store(true);
}

void InferenceWorker::loadModelSlot(const QString& path, bool /*useGpu*/, int ctxLen)
{
#ifdef RETDEC_GUI_HAS_NEURAL
	if (!QFileInfo::exists(path))
	{
		emit systemStatusMessage(QStringLiteral("Model file missing"));
		emit loadModelFinished(false, path, QStringLiteral("missing"));
		return;
	}

	auto backend = retdec::neural::createLlamaInference();
	if (!backend) backend = retdec::neural::createMockInference();

	const int resolvedCtx = ctxLen >= 512 ? ctxLen : 4096;
	if (!backend || !backend->loadModel(path.toStdString(), resolvedCtx))
	{
		emit loadModelFinished(false, path, QStringLiteral("load"));
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_inferenceMutex);
		g_backends[this] = std::move(backend);
	}
	emit loadModelFinished(true, path, QString());
#else
	Q_UNUSED(ctxLen);
	emit systemStatusMessage(QString::fromUtf8(kNoBackend));
	emit loadModelFinished(false, path, QString());
#endif
}

void InferenceWorker::resetKvCacheSlot() {}

void InferenceWorker::unloadModelSlot()
{
#ifdef RETDEC_GUI_HAS_NEURAL
	std::lock_guard<std::mutex> lock(g_inferenceMutex);
	auto it = g_backends.find(this);
	if (it == g_backends.end()) return;
	if (it->second) it->second->unloadModel();
	g_backends.erase(it);
#endif
}

AIAssistantPanel::AIAssistantPanel(QWidget* parent): PanelBase(QStringLiteral("AI Assistant"), parent)
{
	setObjectName(QStringLiteral("ai_assistant_panel"));
	setWindowTitle(tr("AI Assistant"));

	workerThread_ = new QThread(this);
	worker_ = new InferenceWorker();
	worker_->moveToThread(workerThread_);
	workerThread_->start();

	connect(
		this,
		&AIAssistantPanel::startInferenceRequest,
		worker_,
		&InferenceWorker::startInference,
		Qt::QueuedConnection);
	connect(worker_, &InferenceWorker::tokenGenerated, this, &AIAssistantPanel::onTokenGenerated, Qt::QueuedConnection);
	connect(
		worker_, &InferenceWorker::responseComplete, this, &AIAssistantPanel::onResponseComplete, Qt::QueuedConnection);
	connect(worker_, &InferenceWorker::errorOccurred, this, &AIAssistantPanel::onInferenceError, Qt::QueuedConnection);
	connect(
		worker_,
		&InferenceWorker::systemStatusMessage,
		this,
		&AIAssistantPanel::onWorkerSystemMessage,
		Qt::QueuedConnection);
	connect(
		worker_,
		&InferenceWorker::loadModelFinished,
		this,
		&AIAssistantPanel::onLoadModelFinished,
		Qt::QueuedConnection);

	setupUI();
#ifdef RETDEC_GUI_HAS_NEURAL
	appendSystemMessage(QString::fromUtf8(kLoadGgufHint));
#else
	appendSystemMessage(QString::fromUtf8(kNoBackend));
#endif
}

AIAssistantPanel::~AIAssistantPanel()
{
	if (workerThread_)
	{
		workerThread_->quit();
		workerThread_->wait(3000);
	}
	delete worker_;
}

void AIAssistantPanel::setupUI()
{
	auto* root = new QVBoxLayout(this);
	setupTopBar(root);
	setupSettingsBar(root);
	setupChatArea(root);
	setupInputRow(root);
}

void AIAssistantPanel::setupTopBar(QVBoxLayout* root)
{
	auto* row = new QHBoxLayout();
	modelPathLabel_ = new QLabel(tr("Model: (none)"), this);
	modelPathLabel_->setObjectName(QStringLiteral("aiAssistantModelPathLabel"));
	loadModelButton_ = new QPushButton(tr("Load"), this);
	loadModelButton_->setObjectName(QStringLiteral("load_model_button"));
	gpuButton_ = new QPushButton(tr("GPU: OFF"), this);
	gpuButton_->setObjectName(QStringLiteral("gpu_button"));
	gpuButton_->setCheckable(true);
	settingsButton_ = new QPushButton(QStringLiteral("⚙"), this);
	settingsButton_->setObjectName(QStringLiteral("settings_button"));
	settingsButton_->setCheckable(true);
	settingsButton_->setChecked(false);
	clearButton_ = new QPushButton(tr("Clear"), this);
	clearButton_->setObjectName(QStringLiteral("clear_button"));

	connect(loadModelButton_, &QPushButton::clicked, this, &AIAssistantPanel::onLoadModel);
	connect(gpuButton_, &QPushButton::toggled, this, [this](bool on) {
		gpuEnabled_ = on;
		gpuButton_->setText(on ? tr("GPU: ON") : tr("GPU: OFF"));
	});
	connect(settingsButton_, &QPushButton::toggled, this, [this](bool on) {
		if (settingsBar_) settingsBar_->setVisible(on);
	});
	connect(clearButton_, &QPushButton::clicked, this, &AIAssistantPanel::onClearHistory);

	row->addWidget(modelPathLabel_, 1);
	row->addWidget(loadModelButton_);
	row->addWidget(gpuButton_);
	row->addWidget(settingsButton_);
	row->addWidget(clearButton_);
	root->addLayout(row);
}

void AIAssistantPanel::setupSettingsBar(QVBoxLayout* root)
{
	settingsBar_ = new QWidget(this);
	settingsBar_->setObjectName(QStringLiteral("settings_bar"));
	settingsBar_->setVisible(false);
	auto* row = new QHBoxLayout(settingsBar_);

	row->addWidget(new QLabel(tr("Temperature"), settingsBar_));
	auto* tempSpin = new QDoubleSpinBox(settingsBar_);
	tempSpin->setObjectName(QStringLiteral("aiAssistantTemperatureSpin"));
	tempSpin->setRange(0.0, 2.0);
	tempSpin->setSingleStep(0.05);
	tempSpin->setDecimals(2);
	tempSpin->setValue(0.6);
	row->addWidget(tempSpin);

	row->addWidget(new QLabel(tr("Top-P"), settingsBar_));
	auto* topPSpin = new QDoubleSpinBox(settingsBar_);
	topPSpin->setObjectName(QStringLiteral("aiAssistantTopPSpin"));
	topPSpin->setRange(0.0, 1.0);
	topPSpin->setSingleStep(0.05);
	topPSpin->setDecimals(2);
	topPSpin->setValue(0.95);
	row->addWidget(topPSpin);

	row->addWidget(new QLabel(tr("Top-K"), settingsBar_));
	auto* topKSpin = new QSpinBox(settingsBar_);
	topKSpin->setObjectName(QStringLiteral("aiAssistantTopKSpin"));
	topKSpin->setRange(0, 200);
	topKSpin->setValue(20);
	row->addWidget(topKSpin);

	row->addWidget(new QLabel(tr("Max tokens"), settingsBar_));
	auto* maxTokensSpin = new QSpinBox(settingsBar_);
	maxTokensSpin->setObjectName(QStringLiteral("aiAssistantMaxTokensSpin"));
	maxTokensSpin->setRange(1, 32768);
	maxTokensSpin->setValue(512);
	row->addWidget(maxTokensSpin);

	auto* thinking = new QCheckBox(tr("Thinking mode"), settingsBar_);
	thinking->setObjectName(QStringLiteral("aiAssistantThinkingCheck"));
	row->addWidget(thinking);

	if (worker_)
	{
		worker_->setTemperature(0.6f);
		worker_->setTopP(0.95f);
		worker_->setTopK(20);
		worker_->setMaxTokens(512);
	}

	connect(tempSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		if (worker_) worker_->setTemperature(static_cast<float>(v));
	});
	connect(topPSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		if (worker_) worker_->setTopP(static_cast<float>(v));
	});
	connect(topKSpin, &QSpinBox::valueChanged, this, [this](int v) {
		if (worker_) worker_->setTopK(v);
	});
	connect(maxTokensSpin, &QSpinBox::valueChanged, this, [this](int v) {
		if (worker_) worker_->setMaxTokens(v);
	});
	connect(thinking, &QCheckBox::toggled, this, &AIAssistantPanel::onThinkingToggled);

	root->addWidget(settingsBar_);
}

void AIAssistantPanel::setupChatArea(QVBoxLayout* root)
{
	chatLog_ = new QTextBrowser(this);
	chatLog_->setObjectName(QStringLiteral("aiAssistantChatLog"));
	root->addWidget(chatLog_, 1);
	statusLabel_ = new QLabel(this);
	statusLabel_->setObjectName(QStringLiteral("status_label"));
	root->addWidget(statusLabel_);
}

void AIAssistantPanel::setupInputRow(QVBoxLayout* root)
{
	auto* row = new QHBoxLayout();
	queryInput_ = new QLineEdit(this);
	queryInput_->setObjectName(QStringLiteral("aiAssistantQueryInput"));
	sendButton_ = new QPushButton(tr("Send"), this);
	sendButton_->setObjectName(QStringLiteral("aiAssistantSendButton"));
	stopButton_ = new QPushButton(tr("Stop"), this);
	stopButton_->setObjectName(QStringLiteral("stop_button"));
	stopButton_->setEnabled(false);

	connect(sendButton_, &QPushButton::clicked, this, &AIAssistantPanel::onSendQuery);
	connect(stopButton_, &QPushButton::clicked, this, &AIAssistantPanel::onStopGeneration);
	connect(queryInput_, &QLineEdit::returnPressed, this, &AIAssistantPanel::onSendQuery);

	row->addWidget(queryInput_, 1);
	row->addWidget(sendButton_);
	row->addWidget(stopButton_);
	root->addLayout(row);
}

void AIAssistantPanel::setActiveFunction(const QString& decompiledSource, const QString& functionName)
{
	activeDecompiled_ = decompiledSource;
	activeFunctionName_ = functionName;
	contextPending_ = !decompiledSource.isEmpty();
}

void AIAssistantPanel::clear()
{
	history_.clear();
	activeDecompiled_.clear();
	activeFunctionName_.clear();
	contextPending_ = false;
	rebuildChatLog();
}

bool AIAssistantPanel::loadModel(const QString& ggufPath, bool useGpu)
{
	gpuEnabled_ = useGpu;
	gpuButton_->setChecked(useGpu);
	QMetaObject::invokeMethod(
		worker_,
		"loadModelSlot",
		Qt::QueuedConnection,
		Q_ARG(QString, ggufPath),
		Q_ARG(bool, useGpu),
		Q_ARG(int, 4096));
	return true;
}

void AIAssistantPanel::unloadModel()
{
	QMetaObject::invokeMethod(worker_, "unloadModelSlot", Qt::QueuedConnection);
	modelLoaded_ = false;
	modelPath_.clear();
	modelPathLabel_->setText(tr("Model: (none)"));
}

bool AIAssistantPanel::isModelLoaded() const
{
	return modelLoaded_;
}

void AIAssistantPanel::applyMlSettingsFromApp() {}

void AIAssistantPanel::appendSystemMessage(const QString& text)
{
	history_.push_back({ChatRole::System, text, true});
	rebuildChatLog();
}

void AIAssistantPanel::rebuildChatLog()
{
	QString html;
	for (const auto& msg: history_)
	{
		const char* color = "#888";
		if (msg.role == ChatRole::User)
			color = "#4af";
		else if (msg.role == ChatRole::Assistant)
			color = "#afa";
		html += QString("<p style='color:%1'>%2</p>").arg(color).arg(msg.text.toHtmlEscaped());
	}
	chatLog_->setHtml(html);
	scrollToBottom();
}

void AIAssistantPanel::scrollToBottom()
{
	if (chatLog_) chatLog_->verticalScrollBar()->setValue(chatLog_->verticalScrollBar()->maximum());
}

QString AIAssistantPanel::buildPrompt(const QString& userQuery) const
{
	if (!contextPending_ || activeDecompiled_.isEmpty()) return userQuery;
	return QStringLiteral("Context: function %1\n%2\n\nQuery: %3")
		.arg(activeFunctionName_, activeDecompiled_, userQuery);
}

void AIAssistantPanel::setInferenceBusy(bool busy)
{
	inferenceRunning_ = busy;
	if (sendButton_) sendButton_->setEnabled(!busy);
	if (stopButton_) stopButton_->setEnabled(busy);
}

void AIAssistantPanel::onSendQuery()
{
	const QString q = queryInput_->text().trimmed();
	if (q.isEmpty()) return;
	history_.push_back({ChatRole::User, q, true});
	rebuildChatLog();
	queryInput_->clear();
	setInferenceBusy(true);
	emit startInferenceRequest(buildPrompt(q));
	contextPending_ = false;
}

void AIAssistantPanel::onStopGeneration()
{
	QMetaObject::invokeMethod(worker_, "abortInference", Qt::QueuedConnection);
	setInferenceBusy(false);
}

void AIAssistantPanel::onClearHistory()
{
	clear();
}

void AIAssistantPanel::onLoadModel()
{
	const QString path =
		QFileDialog::getOpenFileName(this, tr("Open GGUF model"), QString(), tr("GGUF models (*.gguf)"));
	if (path.isEmpty()) return;
	loadModel(path, gpuEnabled_);
}

void AIAssistantPanel::onSettingsToggled()
{
	if (settingsBar_) settingsBar_->setVisible(!settingsBar_->isVisible());
}

void AIAssistantPanel::onThinkingToggled(bool enabled)
{
	if (worker_) worker_->setThinkingMode(enabled);
}

void AIAssistantPanel::onTokenGenerated(const QString& piece)
{
	if (!history_.empty() && history_.back().role == ChatRole::Assistant && !history_.back().isComplete)
		history_.back().text += piece;
	else
		history_.push_back({ChatRole::Assistant, piece, false});
	rebuildChatLog();
}

void AIAssistantPanel::onResponseComplete(int /*newTokens*/, double /*tokPerSec*/)
{
	if (!history_.empty() && history_.back().role == ChatRole::Assistant) history_.back().isComplete = true;
	setInferenceBusy(false);
}

void AIAssistantPanel::onInferenceError(const QString& error)
{
	appendSystemMessage(error.contains(QStringLiteral("Error:")) ? error : QStringLiteral("Error: ") + error);
	setInferenceBusy(false);
}

void AIAssistantPanel::onWorkerSystemMessage(const QString& text)
{
	appendSystemMessage(text);
}

void AIAssistantPanel::onLoadModelFinished(bool ok, const QString& path, const QString& /*failKind*/)
{
	modelLoaded_ = ok;
	if (ok)
	{
		modelPath_ = path;
		modelPathLabel_->setText(tr("Model: %1").arg(path));
	}
	else
	{
		modelPathLabel_->setText(tr("Load failed"));
	}
}

void AIAssistantPanel::onModelChanged(int /*index*/) {}

void AIAssistantPanel::onResponseChunk(const QString& /*chunk*/) {}

void AIAssistantPanel::onResponseFinished()
{
	setInferenceBusy(false);
}

} // namespace retdec::gui::panels
