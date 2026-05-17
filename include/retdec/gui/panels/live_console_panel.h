/**
 * @file include/retdec/gui/panels/live_console_panel.h
 * @brief LiveConsolePanel — streaming stdout/stderr for retdec CLI subprocesses.
 *
 * Solves the long-standing UX problem where retdec-decompiler / retdec-fileinfo
 * output only appeared in the GUI after the child process exited (because the
 * main window only called readAllStandardOutput in the finished slot).
 *
 * The panel:
 *   * Attaches to one or more QProcess instances and consumes their
 *     readyReadStandardOutput / readyReadStandardError signals.
 *   * Buffers incoming bytes per-stream and flushes to a QPlainTextEdit at
 *     most every kFlushIntervalMs (~50 ms) — this keeps a chatty
 *     `--print-after-all` decompiler run from melting the UI thread.
 *   * Caps total scrollback (setMaximumBlockCount on the editor).
 *   * Colours stderr lines red and stdout lines in the default text colour
 *     using a tiny per-line QSyntaxHighlighter; recognises [ERROR]/[WARN]
 *     prefixes from retdec tools.
 *   * Strips common ANSI escape sequences (Mingw builds occasionally emit
 *     them on Windows).
 *
 * Thread safety: every public method must be called on the GUI thread.
 * QProcess signals always arrive on the thread that started the process,
 * which is always the GUI thread in our usage.
 */

#ifndef RETDEC_GUI_PANELS_LIVE_CONSOLE_PANEL_H
#define RETDEC_GUI_PANELS_LIVE_CONSOLE_PANEL_H

#include "retdec/gui/panels/panel_base.h"

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QLabel;
class QProcess;
class QTimer;
class QSyntaxHighlighter;
QT_END_NAMESPACE

namespace retdec {
namespace gui {
namespace panels {

class LiveConsoleHighlighter; // fwd

class LiveConsolePanel : public PanelBase {
    Q_OBJECT
public:
    enum class Stream { Stdout, Stderr };

    explicit LiveConsolePanel(QWidget* parent = nullptr);
    ~LiveConsolePanel() override;

    // ── Process attach/detach ────────────────────────────────────────────────

    /**
     * Hook `readyReadStandardOutput` / `readyReadStandardError` on @p proc.
     * @p label is shown in the banner emitted when work starts (e.g. "retdec-decompiler").
     * Reattaches cleanly if @p proc is already attached.
     */
    void attachProcess(QProcess* proc, const QString& label);
    void detachProcess(QProcess* proc);

    // ── Direct text ingress (also used by tests) ─────────────────────────────

    void appendBanner(const QString& tool, const QStringList& args, const QString& cwd);
    void appendFooter(const QString& tool, int exitCode, qint64 elapsedMs);
    void appendChunk(Stream stream, const QByteArray& bytes);
    void appendLine(Stream stream, const QString& line);

    // ── Configuration ────────────────────────────────────────────────────────

    /// Maximum number of QTextBlocks retained in the editor (default 50 000).
    void setMaxBlocks(int blocks);
    int  maxBlocks() const;

    /// True if all text content is currently empty.
    bool isEmpty() const;

    /// Approximate flush-timer frequency in Hz (default ~20). Larger value = lower latency, more CPU.
    void setFlushHz(int hz);

    /// Maximum bytes inserted into the editor per flush tick (default 256 KiB).
    /// If a process floods more than this between ticks, the surplus is held
    /// in the per-stream buffer and a follow-up flush is scheduled — this
    /// keeps a chatty `--print-after-all` decompiler from freezing the UI.
    void setMaxBytesPerFlush(int bytes);
    int  maxBytesPerFlush() const { return maxBytesPerFlush_; }

    /// Save full visible buffer to a UTF-8 file. Returns false on error (use lastError()).
    bool saveAs(const QString& filePath);
    QString lastError() const { return lastError_; }

    /// Erase all output (banners, lines, buffers, footers).
    void clear() override;

private slots:
    void onProcessStdoutReady();
    void onProcessStderrReady();
    void onProcessFinished();
    void onProcessDestroyed(QObject* obj);
    void onCopyAll();
    void onSaveAs();
    void onClearClicked();
    void onAutoScrollToggled(bool on);
    void onFlushTick();

private:
    struct ProcEntry {
        QString    label;
        QByteArray pendingOut;
        QByteArray pendingErr;
    };

    void setupUi();
    /// Drain pending buffers into the editor. Inserts text in large chunks.
    void flushAll();
    /// Strip common ANSI CSI sequences in-place (cheap state machine).
    static QString stripAnsi(QString in);

    QPlainTextEdit*       text_         = nullptr;
    LiveConsoleHighlighter* highlighter_ = nullptr;
    QPushButton*          copyBtn_      = nullptr;
    QPushButton*          saveBtn_      = nullptr;
    QPushButton*          clearBtn_     = nullptr;
    QCheckBox*            autoScrollChk_ = nullptr;
    QLabel*               statusLabel_  = nullptr;
    QTimer*               flushTimer_   = nullptr;

    QHash<QObject*, ProcEntry> procs_;
    int   maxBlocks_  = 50'000;
    int   flushMs_    = 16;
    // 16 KiB is the empirical sweet spot — at this size each cursor.insertText
    // takes ~4 ms on a modern desktop (under a 60 Hz frame), and the flush
    // timer's auto-rearm drains floods without ever blocking input/repaint.
    int   maxBytesPerFlush_ = 16 * 1024;
    bool  autoScroll_ = true;
    QString lastError_;
};

} // namespace panels
} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_PANELS_LIVE_CONSOLE_PANEL_H
