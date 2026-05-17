/**
 * @file src/gui/panels/progress_panel.cpp
 * @brief ProgressPanel — full analysis progress dashboard implementation.
 *
 * Provides a scrollable stage-by-stage breakdown (with state icons, per-stage
 * progress bars, and elapsed-time annotations), a waterfall (Gantt) chart of
 * stage durations, live throughput statistics, and a JSON export button.
 *
 * The AnalysisBridge helper lets worker threads post updates to the panel
 * safely using Qt's queued-connection mechanism.
 */

#include "retdec/gui/panels/progress_panel.h"

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QFrame>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMutexLocker>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyle>
#include <QVBoxLayout>

namespace retdec::gui::panels {

// ─── Catppuccin Mocha palette helpers ────────────────────────────────────────

namespace {
constexpr QColor kSurface0  {0x31, 0x32, 0x44};
constexpr QColor kSurface1  {0x45, 0x47, 0x5a};
constexpr QColor kOverlay0  {0x6c, 0x70, 0x86};
constexpr QColor kText      {0xcd, 0xd6, 0xf4};
constexpr QColor kBlue      {0x89, 0xb4, 0xfa};
constexpr QColor kGreen     {0xa6, 0xe3, 0xa1};
constexpr QColor kRed       {0xf3, 0x8b, 0xa8};
constexpr QColor kYellow    {0xf9, 0xe2, 0xaf};
constexpr QColor kPeach     {0xfa, 0xb3, 0x87};
constexpr QColor kMauve     {0xcb, 0xa6, 0xf7};
constexpr QColor kBase      {0x1e, 0x1e, 0x2e};

// Returns colour for a stage bar based on pipeline position (for waterfall).
QColor stageColor(int index) {
    static const QColor palette[] = {
        kBlue, kMauve, kGreen, kPeach, kYellow,
        QColor(0x89, 0xdc, 0xeb),  // sky
        QColor(0xf5, 0xc2, 0xe7),  // pink
        QColor(0x74, 0xc7, 0xec),  // sapphire
        QColor(0x94, 0xe2, 0xd5),  // teal
        QColor(0xa6, 0xe3, 0xa1),  // green
        kBlue, kMauve, kGreen, kPeach,
    };
    return palette[index % static_cast<int>(std::size(palette))];
}
} // anonymous namespace

// ─── AnalysisBridge ──────────────────────────────────────────────────────────

AnalysisBridge::AnalysisBridge(QObject* parent)
    : QObject(parent) {}

void AnalysisBridge::reportStageStarted(const QString& stageName) {
    emit stageStarted(stageName);
}
void AnalysisBridge::reportProgress(const QString& stageName, int percent) {
    emit progressUpdated(stageName, percent);
}
void AnalysisBridge::reportStageCompleted(const QString& stageName, qint64 elapsedMs) {
    emit stageCompleted(stageName, elapsedMs);
}
void AnalysisBridge::reportStageError(const QString& stageName, const QString& msg) {
    emit stageErrored(stageName, msg);
}
void AnalysisBridge::reportFunctionCount(int count) {
    emit functionCountChanged(count);
}
void AnalysisBridge::reportInstructionCount(qint64 count) {
    emit instructionCountChanged(count);
}
void AnalysisBridge::reportThroughput(double fnPerSec, double instrPerSec) {
    emit throughputChanged(fnPerSec, instrPerSec);
}
void AnalysisBridge::reportAnalysisStarted() {
    emit analysisStarted();
}
void AnalysisBridge::reportAnalysisCompleted(qint64 totalMs) {
    emit analysisCompleted(totalMs);
}
void AnalysisBridge::reportAnalysisCancelled() {
    emit analysisCancelled();
}

// ─── Default stage list ───────────────────────────────────────────────────────

const QStringList ProgressPanel::kDefaultStages = {
    "Binary loading",
    "RTTI reconstruction",
    "String detection",
    "SSA construction",
    "Variable recovery",
    "Alias analysis",
    "Type inference",
    "CFG structuring",
    "Calling convention",
    "Dead code elimination",
    "Inter-procedural analysis",
    "Code generation",
    "STL recovery",
    "Pattern / crypto detection",
};

// ─── Construction ─────────────────────────────────────────────────────────────

ProgressPanel::ProgressPanel(QWidget* parent)
    : PanelBase("Progress", parent) {
    setupUI();
}

// ─── Bridge connection ────────────────────────────────────────────────────────

void ProgressPanel::connectBridge(AnalysisBridge* bridge) {
    if (!bridge) return;

    connect(bridge, &AnalysisBridge::stageStarted,
            this,   &ProgressPanel::onStageStarted,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::progressUpdated,
            this,   &ProgressPanel::onProgressUpdated,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::stageCompleted,
            this,   &ProgressPanel::onStageCompleted,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::stageErrored,
            this,   &ProgressPanel::onStageErrored,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::functionCountChanged,
            this,   &ProgressPanel::onFunctionCountChanged,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::instructionCountChanged,
            this,   &ProgressPanel::onInstructionCountChanged,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::throughputChanged,
            this,   &ProgressPanel::onThroughputChanged,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::analysisStarted,
            this,   &ProgressPanel::onAnalysisStarted,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::analysisCompleted,
            this,   &ProgressPanel::onAnalysisCompleted,
            Qt::QueuedConnection);
    connect(bridge, &AnalysisBridge::analysisCancelled,
            this,   &ProgressPanel::onAnalysisCancelled,
            Qt::QueuedConnection);
}

// ─── UI construction ──────────────────────────────────────────────────────────

void ProgressPanel::setupUI() {
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setHandleWidth(4);
    splitter->addWidget(buildSummaryHeader());
    splitter->addWidget(buildStagesWidget());
    splitter->addWidget(buildWaterfallWidget());
    splitter->addWidget(buildThroughputWidget());
    splitter->setStretchFactor(1, 3);  // stages get most space
    splitter->setStretchFactor(2, 2);  // waterfall secondary

    auto* exportBtn = new QPushButton("Export JSON", this);
    exportBtn->setToolTip("Copy analysis stats as JSON to clipboard");
    connect(exportBtn, &QPushButton::clicked,
            this,       &ProgressPanel::onExportClicked);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(splitter, 1);
    layout->addWidget(exportBtn);

    // Elapsed time tick (1 second)
    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(1000);
    connect(elapsedTimer_, &QTimer::timeout,
            this,          &ProgressPanel::onElapsedTick);
}

QWidget* ProgressPanel::buildSummaryHeader() {
    auto* w = new QFrame(this);
    w->setFrameShape(QFrame::StyledPanel);
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(w);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(12);

    // Overall progress bar + percentage
    overallBar_ = new QProgressBar(w);
    overallBar_->setRange(0, 100);
    overallBar_->setValue(0);
    overallBar_->setFixedHeight(12);
    overallBar_->setTextVisible(false);
    overallBar_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    pctLabel_ = new QLabel("0%", w);
    pctLabel_->setFixedWidth(36);
    pctLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pctLabel_->setStyleSheet("color: #cdd6f4; font-weight: bold;");

    // Function count
    auto* fnIcon = new QLabel("ƒ", w);
    fnIcon->setStyleSheet("color: #89b4fa; font-weight: bold;");
    fnCountLabel_ = new QLabel("0 fn", w);
    fnCountLabel_->setStyleSheet("color: #cdd6f4;");

    // Instruction count
    auto* instrIcon = new QLabel("⬡", w);
    instrIcon->setStyleSheet("color: #cba6f7;");
    instrLabel_ = new QLabel("0 instr", w);
    instrLabel_->setStyleSheet("color: #cdd6f4;");

    // Elapsed time
    auto* clockIcon = new QLabel("⏱", w);
    clockIcon->setStyleSheet("color: #f9e2af;");
    elapsedLabel_ = new QLabel("0.0s", w);
    elapsedLabel_->setStyleSheet("color: #cdd6f4;");
    elapsedLabel_->setFixedWidth(56);

    layout->addWidget(overallBar_, 3);
    layout->addWidget(pctLabel_);
    layout->addWidget(fnIcon);
    layout->addWidget(fnCountLabel_);
    layout->addWidget(instrIcon);
    layout->addWidget(instrLabel_);
    layout->addWidget(clockIcon);
    layout->addWidget(elapsedLabel_);

    return w;
}

QWidget* ProgressPanel::buildStagesWidget() {
    auto* scroll = new QScrollArea(this);
    auto* inner  = new QWidget(scroll);
    stagesLayout_ = new QVBoxLayout(inner);
    stagesLayout_->setContentsMargins(4, 4, 4, 4);
    stagesLayout_->setSpacing(4);

    for (const auto& name : kDefaultStages) {
        addRow(name);
    }
    stagesLayout_->addStretch();

    inner->setLayout(stagesLayout_);
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);
    return scroll;
}

QWidget* ProgressPanel::buildWaterfallWidget() {
    auto* w = new QWidget(this);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(2);

    auto* title = new QLabel("Stage Timeline", w);
    title->setStyleSheet("color: #6c7086; font-size: 10px; font-weight: bold;");

    waterfallScene_ = new QGraphicsScene(this);
    waterfallView_  = new QGraphicsView(waterfallScene_, w);
    waterfallView_->setRenderHint(QPainter::Antialiasing);
    waterfallView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    waterfallView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    waterfallView_->setBackgroundBrush(QBrush(kBase));
    waterfallView_->setFrameShape(QFrame::NoFrame);
    waterfallView_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    waterfallView_->setMinimumHeight(80);

    layout->addWidget(title);
    layout->addWidget(waterfallView_);
    return w;
}

QWidget* ProgressPanel::buildThroughputWidget() {
    auto* w = new QFrame(this);
    w->setFrameShape(QFrame::StyledPanel);
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(w);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(16);

    auto* throughputTitle = new QLabel("Throughput:", w);
    throughputTitle->setStyleSheet("color: #6c7086; font-weight: bold;");

    auto* fnLabel = new QLabel("Functions/s:", w);
    fnLabel->setStyleSheet("color: #6c7086;");
    fnRateLabel_ = new QLabel("—", w);
    fnRateLabel_->setStyleSheet("color: #89b4fa; font-family: monospace;");

    auto* instrLabel = new QLabel("Instructions/s:", w);
    instrLabel->setStyleSheet("color: #6c7086;");
    instrRateLabel_ = new QLabel("—", w);
    instrRateLabel_->setStyleSheet("color: #cba6f7; font-family: monospace;");

    layout->addWidget(throughputTitle);
    layout->addWidget(fnLabel);
    layout->addWidget(fnRateLabel_);
    layout->addStretch();
    layout->addWidget(instrLabel);
    layout->addWidget(instrRateLabel_);

    return w;
}

// ─── StageRow management ──────────────────────────────────────────────────────

ProgressPanel::StageRow* ProgressPanel::addRow(const QString& name) {
    auto* rowWidget = new QWidget(this);
    auto* hl = new QHBoxLayout(rowWidget);
    hl->setContentsMargins(2, 1, 2, 1);
    hl->setSpacing(6);

    StageRow sr;
    sr.name      = name;
    sr.stateIcon = new QLabel("○", rowWidget);
    sr.stateIcon->setFixedWidth(16);
    sr.stateIcon->setStyleSheet("color: #6c7086;");
    sr.stateIcon->setAlignment(Qt::AlignCenter);

    sr.nameLabel = new QLabel(name, rowWidget);
    sr.nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    sr.nameLabel->setFixedWidth(172);
    sr.nameLabel->setStyleSheet("color: #cdd6f4;");

    sr.bar = new QProgressBar(rowWidget);
    sr.bar->setRange(0, 100);
    sr.bar->setValue(0);
    sr.bar->setFixedHeight(6);
    sr.bar->setTextVisible(false);
    sr.bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    sr.timeLabel = new QLabel("", rowWidget);
    sr.timeLabel->setStyleSheet("color: #6c7086; font-family: monospace;");
    sr.timeLabel->setFixedWidth(64);
    sr.timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    hl->addWidget(sr.stateIcon);
    hl->addWidget(sr.nameLabel);
    hl->addWidget(sr.bar, 1);
    hl->addWidget(sr.timeLabel);

    stagesLayout_->addWidget(rowWidget);
    rows_.append(sr);

    // Add a placeholder timing entry
    StageTiming timing;
    timing.name = name;
    timings_.append(timing);

    return &rows_.last();
}

ProgressPanel::StageRow* ProgressPanel::findRow(const QString& name) {
    for (auto& r : rows_) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

// ─── Direct update API ────────────────────────────────────────────────────────

void ProgressPanel::setStageState(const QString& stageName,
                                   StageState state, int percent) {
    auto* r = findRow(stageName);
    if (!r) r = addRow(stageName);

    r->state = state;
    switch (state) {
    case StageState::Pending:
        r->stateIcon->setText("○");
        r->stateIcon->setStyleSheet("color: #6c7086;");
        r->bar->setRange(0, 100);
        r->bar->setValue(0);
        break;

    case StageState::Running:
        r->stateIcon->setText("◉");
        r->stateIcon->setStyleSheet("color: #89b4fa;");
        r->nameLabel->setStyleSheet("color: #cdd6f4; font-weight: bold;");
        if (percent < 0) {
            r->bar->setRange(0, 0);  // indeterminate
        } else {
            r->bar->setRange(0, 100);
            r->bar->setValue(percent);
        }
        break;

    case StageState::Done:
        r->stateIcon->setText("✓");
        r->stateIcon->setStyleSheet("color: #a6e3a1;");
        r->nameLabel->setStyleSheet("color: #cdd6f4;");
        r->bar->setRange(0, 100);
        r->bar->setValue(100);
        break;

    case StageState::Error:
        r->stateIcon->setText("✗");
        r->stateIcon->setStyleSheet("color: #f38ba8;");
        r->nameLabel->setStyleSheet("color: #f38ba8;");
        r->bar->setRange(0, 100);
        r->bar->setValue(percent > 0 ? percent : 0);
        // Visual error tint via property
        r->bar->setProperty("error", true);
        r->bar->style()->polish(r->bar);
        break;
    }
    updateOverallBar();
}

void ProgressPanel::setStageElapsed(const QString& stageName, qint64 ms) {
    auto* r = findRow(stageName);
    if (!r) return;
    r->elapsedMs = ms;
    r->timeLabel->setText(formatMs(ms));

    // Update timing entry for waterfall
    for (auto& t : timings_) {
        if (t.name == stageName) {
            t.durationMs = ms;
            break;
        }
    }
    renderWaterfall();
}

void ProgressPanel::setFunctionCount(int count) {
    functionCount_ = count;
    fnCountLabel_->setText(QString("%1 fn").arg(count));
}

void ProgressPanel::setInstructionCount(qint64 count) {
    instrCount_ = count;
    if (count < 1000)
        instrLabel_->setText(QString("%1 instr").arg(count));
    else if (count < 1'000'000)
        instrLabel_->setText(QString("%1K instr").arg(count / 1000));
    else
        instrLabel_->setText(QString("%1M instr").arg(count / 1'000'000));
}

void ProgressPanel::setThroughput(double fnPerSec, double instrPerSec) {
    fnRateLabel_->setText(QString::number(fnPerSec, 'f', 1));
    instrRateLabel_->setText(
        instrPerSec >= 1e6
        ? QString("%1M").arg(instrPerSec / 1e6, 0, 'f', 1)
        : QString::number(instrPerSec, 'f', 0));
}

void ProgressPanel::setOverallProgress(int percent) {
    overallBar_->setValue(qBound(0, percent, 100));
    pctLabel_->setText(QString("%1%").arg(percent));
}

// ─── Slots (from bridge) ──────────────────────────────────────────────────────

void ProgressPanel::onStageStarted(const QString& stageName) {
    setStageState(stageName, StageState::Running, -1);

    // Record wall-clock start offset for waterfall
    for (auto& t : timings_) {
        if (t.name == stageName) {
            t.startMs    = running_ ? wallClock_.elapsed() : 0;
            t.durationMs = -1;
            break;
        }
    }
    emit stageStarted(stageName);
}

void ProgressPanel::onProgressUpdated(const QString& stageName, int percent) {
    setStageState(stageName, StageState::Running, percent);
}

void ProgressPanel::onStageCompleted(const QString& stageName, qint64 elapsedMs) {
    setStageState(stageName, StageState::Done);
    setStageElapsed(stageName, elapsedMs);
    emit stageCompleted(stageName, elapsedMs);
}

void ProgressPanel::onStageErrored(const QString& stageName, const QString& msg) {
    setStageState(stageName, StageState::Error);
    auto* r = findRow(stageName);
    if (r) r->timeLabel->setToolTip(msg);
}

void ProgressPanel::onFunctionCountChanged(int count) {
    setFunctionCount(count);
}

void ProgressPanel::onInstructionCountChanged(qint64 count) {
    setInstructionCount(count);
}

void ProgressPanel::onThroughputChanged(double fnPerSec, double instrPerSec) {
    setThroughput(fnPerSec, instrPerSec);
}

void ProgressPanel::onAnalysisStarted() {
    running_ = true;
    wallClock_.start();
    elapsedTimer_->start();
}

void ProgressPanel::onAnalysisCompleted(qint64 totalMs) {
    running_ = false;
    elapsedTimer_->stop();
    elapsedLabel_->setText(formatMs(totalMs));
    overallBar_->setValue(100);
    pctLabel_->setText("100%");
    renderWaterfall();
    emit analysisCompleted(totalMs);
}

void ProgressPanel::onAnalysisCancelled() {
    running_ = false;
    elapsedTimer_->stop();
    // Mark all still-running stages as pending
    for (auto& r : rows_) {
        if (r.state == StageState::Running)
            setStageState(r.name, StageState::Pending);
    }
}

void ProgressPanel::onElapsedTick() {
    if (!running_) return;
    qint64 ms = wallClock_.elapsed();
    elapsedLabel_->setText(formatMs(ms));
}

void ProgressPanel::onExportClicked() {
    QApplication::clipboard()->setText(exportJson());
    emit exportRequested();
}

// ─── Reset ────────────────────────────────────────────────────────────────────

void ProgressPanel::resetAll() {
    for (auto& r : rows_) {
        r.state = StageState::Pending;
        r.elapsedMs = -1;
        r.stateIcon->setText("○");
        r.stateIcon->setStyleSheet("color: #6c7086;");
        r.nameLabel->setStyleSheet("color: #cdd6f4;");
        r.bar->setRange(0, 100);
        r.bar->setValue(0);
        r.bar->setProperty("error", false);
        r.bar->style()->polish(r.bar);
        r.timeLabel->clear();
        r.timeLabel->setToolTip({});
    }
    for (auto& t : timings_) {
        t.startMs    = -1;
        t.durationMs = -1;
    }
    waterfallScene_->clear();
    overallBar_->setValue(0);
    pctLabel_->setText("0%");
    fnCountLabel_->setText("0 fn");
    instrLabel_->setText("0 instr");
    elapsedLabel_->setText("0.0s");
    fnRateLabel_->setText("—");
    instrRateLabel_->setText("—");
    functionCount_ = 0;
    instrCount_    = 0;
    running_       = false;
    elapsedTimer_->stop();
}

// ─── Overall progress bar ─────────────────────────────────────────────────────

void ProgressPanel::updateOverallBar() {
    if (rows_.isEmpty()) return;
    int done = 0;
    for (const auto& r : rows_) {
        if (r.state == StageState::Done || r.state == StageState::Error)
            ++done;
    }
    int pct = done * 100 / rows_.size();
    overallBar_->setValue(pct);
    pctLabel_->setText(QString("%1%").arg(pct));
}

// ─── Waterfall chart ──────────────────────────────────────────────────────────

void ProgressPanel::renderWaterfall() {
    waterfallScene_->clear();

    // Collect completed timings with a start offset
    qint64 maxEnd = 0;
    for (const auto& t : timings_) {
        if (t.startMs >= 0 && t.durationMs > 0)
            maxEnd = qMax(maxEnd, t.startMs + t.durationMs);
    }
    if (maxEnd <= 0) return;

    qreal viewW = waterfallView_->viewport()->width();
    if (viewW <= 1.0) {
        // Widget not yet shown / sized — defer rendering. (No throw, no divide-by-zero.)
        return;
    }
    const qreal rowH  = 14.0;
    const qreal labelW = 120.0;
    const qreal barArea = qMax(qreal(40.0), viewW - labelW - 8.0);
    const qreal scale  = barArea / static_cast<qreal>(maxEnd);

    int idx = 0;
    for (const auto& t : timings_) {
        if (t.startMs < 0 || t.durationMs <= 0) { ++idx; continue; }

        qreal y = idx * (rowH + 2.0) + 2.0;

        // Stage name label
        auto* label = waterfallScene_->addText(
            t.name.left(18), QFont("monospace", 7));
        label->setDefaultTextColor(kOverlay0);
        label->setPos(2.0, y);

        // Duration bar
        qreal x = labelW + t.startMs * scale;
        qreal w = qMax(2.0, t.durationMs * scale);
        auto* rect = waterfallScene_->addRect(
            x, y + 1, w, rowH - 2,
            QPen(Qt::NoPen),
            QBrush(stageColor(idx)));
        (void)rect;

        // Duration annotation
        auto* ann = waterfallScene_->addText(
            formatMs(t.durationMs), QFont("monospace", 7));
        ann->setDefaultTextColor(kText);
        ann->setPos(x + w + 3.0, y);

        ++idx;
    }

    const qreal totalH = idx * (rowH + 2.0) + 4.0;
    waterfallScene_->setSceneRect(0, 0, viewW, totalH);
    waterfallView_->fitInView(waterfallScene_->sceneRect(), Qt::KeepAspectRatio);
}

// ─── JSON export ──────────────────────────────────────────────────────────────

QString ProgressPanel::exportJson() const {
    QJsonObject root;
    root["elapsed_ms"] = wallClock_.isValid() ? (qint64)wallClock_.elapsed() : 0;
    root["function_count"] = functionCount_;
    root["instruction_count"] = instrCount_;

    QJsonArray stages;
    for (const auto& r : rows_) {
        QJsonObject s;
        s["name"] = r.name;
        s["state"] = [](StageState st) -> QString {
            switch (st) {
            case StageState::Pending: return "pending";
            case StageState::Running: return "running";
            case StageState::Done:    return "done";
            case StageState::Error:   return "error";
            }
            return "unknown";
        }(r.state);
        s["elapsed_ms"] = r.elapsedMs;
        stages.append(s);
    }
    root["stages"] = stages;

    QJsonArray timeline;
    for (const auto& t : timings_) {
        if (t.startMs < 0) continue;
        QJsonObject e;
        e["name"]        = t.name;
        e["start_ms"]    = t.startMs;
        e["duration_ms"] = t.durationMs;
        timeline.append(e);
    }
    root["timeline"] = timeline;

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// ─── Utility ──────────────────────────────────────────────────────────────────

QString ProgressPanel::formatMs(qint64 ms) const {
    if (ms < 0)       return "";
    if (ms < 1000)    return QString("%1ms").arg(ms);
    if (ms < 60'000)  return QString("%1.%2s")
                           .arg(ms / 1000)
                           .arg((ms % 1000) / 100);
    return QString("%1m%2s").arg(ms / 60'000).arg((ms % 60'000) / 1000);
}

} // namespace retdec::gui::panels
