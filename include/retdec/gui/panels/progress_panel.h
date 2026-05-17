/**
 * @file include/retdec/gui/panels/progress_panel.h
 * @brief ProgressPanel — full analysis progress dashboard.
 *
 * ## Layout
 *
 * ┌──────────────────────────────────────────────────────────┐
 * │  SUMMARY BAR  ▓▓▓▓▓▓▓▓▓░░░░░  42%  │ 1,234 fn │ 12.4s  │
 * ├──────────────────────────────────────────────────────────┤
 * │ ○ Binary loading         ████████  100ms                 │
 * │ ◉ RTTI reconstruction    ░░░░░░░░  running…             │
 * │ ○ String detection        —                              │
 * │   …                                                      │
 * ├──────────────────────────────────────────────────────────┤
 * │  WATERFALL TIMELINE   (mini Gantt chart)                 │
 * ├──────────────────────────────────────────────────────────┤
 * │  THROUGHPUT             Functions/s  │  Instrs/s         │
 * └──────────────────────────────────────────────────────────┘
 *
 * ## Signals (emitted by ProgressPanel)
 *
 *   stageStarted(stageName)          — stage transitioned to Running
 *   stageCompleted(stageName, ms)    — stage transitioned to Done
 *   analysisCompleted(totalMs)       — all stages done
 *
 * ## AnalysisBridge
 *
 * Provides a thread-safe bridge between the (possibly non-Qt) analysis
 * pipeline and the dashboard.  Backend threads call:
 *
 *   bridge->reportStageStarted("SSA construction");
 *   bridge->reportProgress("SSA construction", 50);   // 0-100
 *   bridge->reportStageCompleted("SSA construction", 1245);  // duration in ms
 *   bridge->reportFunctionCount(312);
 *   bridge->reportThroughput(42.0, 1200.0);  // fn/s, instr/s
 */

#ifndef RETDEC_GUI_PANELS_PROGRESS_H
#define RETDEC_GUI_PANELS_PROGRESS_H

#include "retdec/gui/panels/panel_base.h"

#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QVBoxLayout;
class QHBoxLayout;
class QLabel;
class QProgressBar;
class QGraphicsView;
class QGraphicsScene;
class QPushButton;
class QScrollArea;
class QFrame;
QT_END_NAMESPACE

namespace retdec::gui::panels {

// ─── StageState ───────────────────────────────────────────────────────────────

enum class StageState { Pending, Running, Done, Error };

// ─── StageTiming (for waterfall chart) ───────────────────────────────────────

struct StageTiming {
    QString  name;
    qint64   startMs  = -1;  ///< ms since analysis start (-1 = not started)
    qint64   durationMs = -1; ///< -1 = not finished
};

// ─── AnalysisBridge ──────────────────────────────────────────────────────────

/**
 * @brief Thread-safe bridge: non-Qt analysis threads → progress dashboard.
 *
 * Analysis pipeline code (possibly running on worker threads) calls the
 * report*() methods.  These are marshalled via Qt queued signals to the
 * main thread where the GUI updates occur.
 *
 * Ownership: instantiated by RetDecMainWindow, connected to ProgressPanel.
 */
class AnalysisBridge : public QObject {
    Q_OBJECT
public:
    explicit AnalysisBridge(QObject* parent = nullptr);

    // ── Pipeline-facing API (thread-safe) ────────────────────────────────────

    void reportStageStarted(const QString& stageName);
    void reportProgress(const QString& stageName, int percent);
    void reportStageCompleted(const QString& stageName, qint64 elapsedMs);
    void reportStageError(const QString& stageName, const QString& msg);
    void reportFunctionCount(int count);
    void reportInstructionCount(qint64 count);
    void reportThroughput(double fnPerSec, double instrPerSec);
    void reportAnalysisStarted();
    void reportAnalysisCompleted(qint64 totalMs);
    void reportAnalysisCancelled();

signals:
    // ── GUI-facing signals (always on main thread via Qt::QueuedConnection) ──
    void stageStarted(const QString& stageName);
    void progressUpdated(const QString& stageName, int percent);
    void stageCompleted(const QString& stageName, qint64 elapsedMs);
    void stageErrored(const QString& stageName, const QString& msg);
    void functionCountChanged(int count);
    void instructionCountChanged(qint64 count);
    void throughputChanged(double fnPerSec, double instrPerSec);
    void analysisStarted();
    void analysisCompleted(qint64 totalMs);
    void analysisCancelled();

private:
    QMutex mutex_;
};

// ─── ProgressPanel ────────────────────────────────────────────────────────────

/**
 * @brief Full analysis progress dashboard panel.
 *
 * Displays:
 *   1. Summary header: overall progress, function count, elapsed time
 *   2. Per-stage rows: state icon, name, progress bar, elapsed time
 *   3. Waterfall timeline: mini Gantt chart of stage durations
 *   4. Throughput statistics: functions/s, instructions/s
 */
class ProgressPanel : public PanelBase {
    Q_OBJECT
public:
    explicit ProgressPanel(QWidget* parent = nullptr);

    // ── Connect to the analysis bridge ───────────────────────────────────────

    void connectBridge(AnalysisBridge* bridge);

    // ── Direct update API (for same-thread use / testing) ────────────────────

    void setStageState(const QString& stageName, StageState state,
                       int percent = -1);
    void setStageElapsed(const QString& stageName, qint64 ms);
    void setFunctionCount(int count);
    void setInstructionCount(qint64 count);
    void setThroughput(double fnPerSec, double instrPerSec);
    void setOverallProgress(int percent);

    void resetAll();
    void clear() override { resetAll(); }

    // ── Export ───────────────────────────────────────────────────────────────

    QString exportJson() const;

signals:
    void stageStarted(const QString& stageName);
    void stageCompleted(const QString& stageName, qint64 ms);
    void analysisCompleted(qint64 totalMs);
    void exportRequested();

private slots:
    void onStageStarted(const QString& stageName);
    void onProgressUpdated(const QString& stageName, int percent);
    void onStageCompleted(const QString& stageName, qint64 elapsedMs);
    void onStageErrored(const QString& stageName, const QString& msg);
    void onFunctionCountChanged(int count);
    void onInstructionCountChanged(qint64 count);
    void onThroughputChanged(double fnPerSec, double instrPerSec);
    void onAnalysisStarted();
    void onAnalysisCompleted(qint64 totalMs);
    void onAnalysisCancelled();
    void onElapsedTick();
    void onExportClicked();

private:
    // ── Internal stage row ────────────────────────────────────────────────────

    struct StageRow {
        QString       name;
        QLabel*       stateIcon  = nullptr;
        QLabel*       nameLabel  = nullptr;
        QProgressBar* bar        = nullptr;
        QLabel*       timeLabel  = nullptr;
        StageState    state      = StageState::Pending;
        qint64        elapsedMs  = -1;
    };

    // ── Build UI ──────────────────────────────────────────────────────────────

    void setupUI();
    QWidget* buildSummaryHeader();
    QWidget* buildStagesWidget();
    QWidget* buildWaterfallWidget();
    QWidget* buildThroughputWidget();

    StageRow*  findRow(const QString& name);
    StageRow*  addRow(const QString& name);
    void       renderWaterfall();
    void       updateOverallBar();
    QString    formatMs(qint64 ms) const;

    // ── Summary header ────────────────────────────────────────────────────────

    QProgressBar* overallBar_    = nullptr;
    QLabel*       pctLabel_      = nullptr;
    QLabel*       fnCountLabel_  = nullptr;
    QLabel*       elapsedLabel_  = nullptr;
    QLabel*       instrLabel_    = nullptr;

    // ── Stages list ───────────────────────────────────────────────────────────

    QVBoxLayout*    stagesLayout_ = nullptr;
    QList<StageRow> rows_;

    // ── Waterfall chart ───────────────────────────────────────────────────────

    QGraphicsScene* waterfallScene_ = nullptr;
    QGraphicsView*  waterfallView_  = nullptr;

    // ── Throughput stats ──────────────────────────────────────────────────────

    QLabel* fnRateLabel_    = nullptr;
    QLabel* instrRateLabel_ = nullptr;

    // ── State ─────────────────────────────────────────────────────────────────

    QTimer*       elapsedTimer_  = nullptr;
    QElapsedTimer wallClock_;
    bool          running_       = false;
    int           functionCount_ = 0;
    qint64        instrCount_    = 0;
    QList<StageTiming> timings_;

    static const QStringList kDefaultStages;
};

} // namespace retdec::gui::panels

#endif // RETDEC_GUI_PANELS_PROGRESS_H
