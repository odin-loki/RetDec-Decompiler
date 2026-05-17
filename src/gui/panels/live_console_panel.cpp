/**
 * @file src/gui/panels/live_console_panel.cpp
 * @brief LiveConsolePanel implementation — see header for design notes.
 */

#include "retdec/gui/panels/live_console_panel.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QStringView>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace retdec {
namespace gui {
namespace panels {

// ─── Highlighter ─────────────────────────────────────────────────────────────

class LiveConsoleHighlighter : public QSyntaxHighlighter {
public:
    explicit LiveConsoleHighlighter(QTextDocument* parent) : QSyntaxHighlighter(parent) {
        errFmt_.setForeground(QColor(0xf3, 0x8b, 0xa8));
        warnFmt_.setForeground(QColor(0xf9, 0xe2, 0xaf));
        bannerFmt_.setForeground(QColor(0x89, 0xb4, 0xfa));
        bannerFmt_.setFontWeight(QFont::Bold);
        footerFmt_.setForeground(QColor(0xa6, 0xe3, 0xa1));
        footerFmt_.setFontWeight(QFont::Bold);
    }

protected:
    void highlightBlock(const QString& text) override {
        if (text.isEmpty())
            return;
        // Cheap O(1) prefix checks first.
        const QChar c0 = text.at(0);
        if (c0 == QLatin1Char('=') && text.startsWith(QLatin1String("==> "))) {
            setFormat(0, text.size(), bannerFmt_);
            return;
        }
        if (c0 == QLatin1Char('<') && text.startsWith(QLatin1String("<== "))) {
            setFormat(0, text.size(), footerFmt_);
            return;
        }
        // Cheap substring scans for severity keywords. With merged channels
        // we no longer differentiate stderr/stdout by prefix — anything
        // containing "error"/"warn"/etc. is coloured based on content.
        if (text.contains(QLatin1String("error"), Qt::CaseInsensitive) ||
            text.contains(QLatin1String("fatal"), Qt::CaseInsensitive) ||
            text.contains(QLatin1String("abort"), Qt::CaseInsensitive)) {
            setFormat(0, text.size(), errFmt_);
            return;
        }
        if (text.contains(QLatin1String("warn"),       Qt::CaseInsensitive) ||
            text.contains(QLatin1String("deprecated"), Qt::CaseInsensitive)) {
            setFormat(0, text.size(), warnFmt_);
            return;
        }
    }

private:
    QTextCharFormat errFmt_;
    QTextCharFormat warnFmt_;
    QTextCharFormat bannerFmt_;
    QTextCharFormat footerFmt_;
};

// ─── Construction ────────────────────────────────────────────────────────────

LiveConsolePanel::LiveConsolePanel(QWidget* parent)
    : PanelBase(QStringLiteral("Console"), parent) {
    setupUi();
    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(flushMs_);
    connect(flushTimer_, &QTimer::timeout, this, &LiveConsolePanel::onFlushTick);
    flushTimer_->start();
}

LiveConsolePanel::~LiveConsolePanel() = default;

void LiveConsolePanel::setupUi() {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);

    auto* row = new QHBoxLayout();
    row->setSpacing(6);
    copyBtn_  = new QPushButton(QStringLiteral("Copy all"), this);
    saveBtn_  = new QPushButton(QStringLiteral("Save…"), this);
    clearBtn_ = new QPushButton(QStringLiteral("Clear"), this);
    autoScrollChk_ = new QCheckBox(QStringLiteral("Auto-scroll"), this);
    autoScrollChk_->setChecked(autoScroll_);
    statusLabel_ = new QLabel(QStringLiteral("(idle)"), this);
    statusLabel_->setProperty("role", "muted");
    statusLabel_->setStyleSheet(QStringLiteral("color: #6c7086;"));
    copyBtn_->setToolTip(QStringLiteral("Copy entire console to clipboard."));
    saveBtn_->setToolTip(QStringLiteral("Save console output to a text file."));
    clearBtn_->setToolTip(QStringLiteral("Clear the console buffer."));
    autoScrollChk_->setToolTip(
        QStringLiteral("Follow new lines as they arrive. Untick to scroll back without losing position."));
    row->addWidget(copyBtn_);
    row->addWidget(saveBtn_);
    row->addWidget(clearBtn_);
    row->addWidget(autoScrollChk_);
    row->addStretch();
    row->addWidget(statusLabel_, 1);
    lay->addLayout(row);

    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    text_->setUndoRedoEnabled(false);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    text_->setMaximumBlockCount(maxBlocks_);
    text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text_->setPlaceholderText(QStringLiteral(
        "Live output from retdec-decompiler / retdec-fileinfo / retdec-unpacker will appear here as it runs."));
    text_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background-color: #11111b; color: #cdd6f4; }"));
    lay->addWidget(text_, 1);

    highlighter_ = new LiveConsoleHighlighter(text_->document());

    connect(copyBtn_,  &QPushButton::clicked, this, &LiveConsolePanel::onCopyAll);
    connect(saveBtn_,  &QPushButton::clicked, this, &LiveConsolePanel::onSaveAs);
    connect(clearBtn_, &QPushButton::clicked, this, &LiveConsolePanel::onClearClicked);
    connect(autoScrollChk_, &QCheckBox::toggled, this, &LiveConsolePanel::onAutoScrollToggled);

    // Copy shortcut when console has focus.
    auto* copySc = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this);
    copySc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copySc, &QShortcut::activated, this, &LiveConsolePanel::onCopyAll);
}

// ─── Configuration ───────────────────────────────────────────────────────────

void LiveConsolePanel::setMaxBlocks(int blocks) {
    maxBlocks_ = qMax(1000, blocks);
    if (text_)
        text_->setMaximumBlockCount(maxBlocks_);
}

int LiveConsolePanel::maxBlocks() const {
    return maxBlocks_;
}

void LiveConsolePanel::setFlushHz(int hz) {
    hz = qBound(2, hz, 250);
    flushMs_ = 1000 / hz;
    if (flushTimer_)
        flushTimer_->setInterval(flushMs_);
}

void LiveConsolePanel::setMaxBytesPerFlush(int bytes) {
    maxBytesPerFlush_ = qMax(4 * 1024, bytes);
}

bool LiveConsolePanel::isEmpty() const {
    return text_ ? text_->document()->isEmpty() : true;
}

// ─── Process attach ──────────────────────────────────────────────────────────

void LiveConsolePanel::attachProcess(QProcess* proc, const QString& label) {
    if (!proc)
        return;
    // Re-attaching cleanly resets buffers but keeps the existing entry visible.
    detachProcess(proc);
    ProcEntry entry;
    entry.label = label.isEmpty() ? QStringLiteral("subprocess") : label;
    procs_.insert(proc, entry);
    connect(proc, &QProcess::readyReadStandardOutput, this,
            &LiveConsolePanel::onProcessStdoutReady, Qt::UniqueConnection);
    connect(proc, &QProcess::readyReadStandardError, this,
            &LiveConsolePanel::onProcessStderrReady, Qt::UniqueConnection);
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) { onProcessFinished(); },
            Qt::UniqueConnection);
    connect(proc, &QObject::destroyed, this, &LiveConsolePanel::onProcessDestroyed,
            Qt::UniqueConnection);
}

void LiveConsolePanel::detachProcess(QProcess* proc) {
    if (!proc)
        return;
    if (auto it = procs_.find(proc); it != procs_.end()) {
        // Drain anything left in the buffers so nothing is lost.
        if (!it->pendingOut.isEmpty()) {
            appendChunk(Stream::Stdout, it->pendingOut);
            it->pendingOut.clear();
        }
        if (!it->pendingErr.isEmpty()) {
            appendChunk(Stream::Stderr, it->pendingErr);
            it->pendingErr.clear();
        }
        procs_.erase(it);
    }
    disconnect(proc, nullptr, this, nullptr);
}

void LiveConsolePanel::onProcessDestroyed(QObject* obj) {
    if (auto it = procs_.find(obj); it != procs_.end()) {
        procs_.erase(it);
    }
}

// ─── Read slots ──────────────────────────────────────────────────────────────

void LiveConsolePanel::onProcessStdoutReady() {
    auto* proc = qobject_cast<QProcess*>(sender());
    if (!proc) return;
    auto it = procs_.find(proc);
    if (it == procs_.end()) return;
    it->pendingOut += proc->readAllStandardOutput();
}

void LiveConsolePanel::onProcessStderrReady() {
    auto* proc = qobject_cast<QProcess*>(sender());
    if (!proc) return;
    auto it = procs_.find(proc);
    if (it == procs_.end()) return;
    it->pendingErr += proc->readAllStandardError();
}

void LiveConsolePanel::onProcessFinished() {
    auto* proc = qobject_cast<QProcess*>(sender());
    if (!proc) return;
    // Drain any final bytes synchronously so the footer order is correct.
    auto it = procs_.find(proc);
    if (it == procs_.end()) return;
    it->pendingOut += proc->readAllStandardOutput();
    it->pendingErr += proc->readAllStandardError();
    flushAll();
}

// ─── Banner / footer / chunk ─────────────────────────────────────────────────

void LiveConsolePanel::appendBanner(const QString& tool, const QStringList& args,
                                    const QString& cwd) {
    const QString ts = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QString cmd = tool;
    for (const QString& a : args) {
        cmd += QLatin1Char(' ');
        // Quote if there's whitespace; cheap shell-quoting for log readability.
        if (a.contains(QLatin1Char(' ')) || a.contains(QLatin1Char('\t'))) {
            cmd += QLatin1Char('"');
            cmd += QString(a).replace(QLatin1Char('"'), QStringLiteral("\\\""));
            cmd += QLatin1Char('"');
        } else {
            cmd += a;
        }
    }
    QString line = QStringLiteral("==> [%1] %2\n").arg(ts, cmd);
    if (!cwd.isEmpty())
        line += QStringLiteral("==>   cwd: %1\n").arg(cwd);
    appendLine(Stream::Stdout, line.left(line.size() - 1)); // strip trailing \n; appendLine adds blocks naturally
    statusLabel_->setText(QStringLiteral("running: %1").arg(tool));
}

void LiveConsolePanel::appendFooter(const QString& tool, int exitCode, qint64 elapsedMs) {
    QString line = QStringLiteral("<== %1 finished (exit=%2, time=%3 ms)")
                           .arg(tool)
                           .arg(exitCode)
                           .arg(elapsedMs);
    appendLine(Stream::Stdout, line);
    statusLabel_->setText(QStringLiteral("idle (last: %1, exit=%2)").arg(tool).arg(exitCode));
}

void LiveConsolePanel::appendChunk(Stream stream, const QByteArray& bytes) {
    Q_UNUSED(stream);
    if (bytes.isEmpty() || text_ == nullptr)
        return;

    // Decode permissively: tool output is usually UTF-8 but may contain stray bytes.
    QString text = QString::fromUtf8(bytes);
    text = stripAnsi(std::move(text));
    if (text.isEmpty())
        return;

    QTextCursor c(text_->document());
    c.movePosition(QTextCursor::End);

    // Channels are now merged at the QProcess level, so stderr and stdout
    // both flow through this path with no distinguishing prefix. The
    // highlighter still colours lines containing "error"/"warning"/etc.

    // QTextCursor::insertText on >32 KiB blocks Qt's layout for ~10+ ms
    // (visible 60 Hz stutter). Break large inputs into ≤kInsertBudget
    // pieces, preferring newline boundaries so the syntax highlighter sees
    // whole lines. This keeps each cursor.insertText() call cheap.
    constexpr int kInsertBudget = 16 * 1024;
    const int total = text.size();
    if (total <= kInsertBudget) {
        c.insertText(text);
    } else {
        int pos = 0;
        while (pos < total) {
            int end = qMin(pos + kInsertBudget, total);
            // Prefer a newline boundary inside the next kInsertBudget bytes.
            const int nl = text.indexOf(QLatin1Char('\n'), end - 1);
            if (nl >= 0 && nl - pos < kInsertBudget * 2)
                end = nl + 1;
            // QStringView::mid avoids the QString allocation/copy.
            c.insertText(QStringView(text).mid(pos, end - pos).toString());
            pos = end;
        }
    }

    if (autoScroll_) {
        QScrollBar* sb = text_->verticalScrollBar();
        if (sb)
            sb->setValue(sb->maximum());
    }
}

void LiveConsolePanel::appendLine(Stream stream, const QString& line) {
    QString withLf = line;
    if (!withLf.endsWith(QLatin1Char('\n')))
        withLf += QLatin1Char('\n');
    appendChunk(stream, withLf.toUtf8());
}

// ─── Flush timer ─────────────────────────────────────────────────────────────

void LiveConsolePanel::onFlushTick() {
    flushAll();
}

void LiveConsolePanel::flushAll() {
    // Cap the per-tick insert size so a flood (e.g. retdec-decompiler with
    // --print-after-all on a 1 MB binary) doesn't freeze the GUI thread for
    // hundreds of ms inserting megabytes of text in one shot.
    //
    // We prefer to break at a newline boundary so highlighter rules and
    // [stderr] prefixing remain stable across the split. If no newline
    // exists within the cap, we cut on a UTF-8-safe boundary (back off
    // until we find a byte < 0x80 or a leading byte).
    const int cap = qMax(4096, maxBytesPerFlush_);
    bool moreWork = false;

    auto takeCapped = [cap](QByteArray& src) -> QByteArray {
        if (src.size() <= cap)
            return std::exchange(src, {});
        int cut = cap;
        // Prefer a newline split.
        const int lastNl = src.lastIndexOf('\n', cap - 1);
        if (lastNl > cap / 2)
            cut = lastNl + 1;
        else {
            // Back off to a UTF-8 boundary: skip continuation bytes (10xxxxxx).
            while (cut > 0 && (static_cast<unsigned char>(src[cut]) & 0xC0) == 0x80)
                --cut;
            if (cut <= 0) cut = cap;
        }
        QByteArray head = src.left(cut);
        src.remove(0, cut);
        return head;
    };

    for (auto& e : procs_) {
        if (!e.pendingOut.isEmpty()) {
            QByteArray head = takeCapped(e.pendingOut);
            if (!head.isEmpty())
                appendChunk(Stream::Stdout, head);
            if (!e.pendingOut.isEmpty()) moreWork = true;
        }
        if (!e.pendingErr.isEmpty()) {
            QByteArray head = takeCapped(e.pendingErr);
            if (!head.isEmpty())
                appendChunk(Stream::Stderr, head);
            if (!e.pendingErr.isEmpty()) moreWork = true;
        }
    }

    if (moreWork && flushTimer_) {
        // Re-arm immediately so the next tick happens at the next event loop
        // iteration, draining the rest without blocking input/repaint.
        QTimer::singleShot(0, this, [this] { onFlushTick(); });
    }
}

// ─── Buttons ─────────────────────────────────────────────────────────────────

void LiveConsolePanel::onCopyAll() {
    if (!text_) return;
    QGuiApplication::clipboard()->setText(text_->toPlainText());
}

void LiveConsolePanel::onSaveAs() {
    const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save console output"), QString(),
            QStringLiteral("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    if (!saveAs(path)) {
        statusLabel_->setText(QStringLiteral("save error: %1").arg(lastError_));
    } else {
        statusLabel_->setText(QStringLiteral("saved %1").arg(path));
    }
}

bool LiveConsolePanel::saveAs(const QString& filePath) {
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        lastError_ = f.errorString();
        return false;
    }
    if (text_)
        f.write(text_->toPlainText().toUtf8());
    f.close();
    lastError_.clear();
    return true;
}

void LiveConsolePanel::onClearClicked() {
    clear();
}

void LiveConsolePanel::onAutoScrollToggled(bool on) {
    autoScroll_ = on;
}

void LiveConsolePanel::clear() {
    if (text_)
        text_->clear();
    for (auto& e : procs_) {
        e.pendingOut.clear();
        e.pendingErr.clear();
    }
    if (statusLabel_)
        statusLabel_->setText(QStringLiteral("(idle)"));
}

// ─── ANSI strip ──────────────────────────────────────────────────────────────

QString LiveConsolePanel::stripAnsi(QString in) {
    // Cover the two most common forms: CSI ('\x1b[...m') and OSC ('\x1b]...\x07').
    // Cheap regex pass — input is small chunks.
    static const QRegularExpression csi(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]"));
    static const QRegularExpression osc(QStringLiteral("\x1b\\][^\x07]*\x07"));
    in.remove(csi);
    in.remove(osc);
    return in;
}

} // namespace panels
} // namespace gui
} // namespace retdec
