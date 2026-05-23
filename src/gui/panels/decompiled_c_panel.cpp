#include "retdec/gui/panels/decompiled_c_panel.h"

#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QLabel>
#include <QToolBar>
#include <QAction>
#include <QClipboard>
#include <QApplication>
#include <QFileDialog>
#include <QFile>
#include <QStringView>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>

namespace retdec::gui::panels {

DecompiledCPanel::DecompiledCPanel(QWidget* parent)
    : PanelBase("Decompiled C", parent) {
    setupUI();
}

void DecompiledCPanel::setupUI() {
    toolbar_ = new QToolBar(this);
    toolbar_->setIconSize({16, 16});
    auto* copyAction = toolbar_->addAction("Copy");
    auto* saveAction = toolbar_->addAction("Save As…");
    connect(copyAction, &QAction::triggered, this, &DecompiledCPanel::onCopyToClipboard);
    connect(saveAction, &QAction::triggered, this, &DecompiledCPanel::onSaveAs);

    funcLabel_ = new QLabel("No function selected", this);
    funcLabel_->setProperty("role", "muted");

    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setUndoRedoEnabled(false);
    // QPlainTextEdit copes with very large documents (it's block-based), but
    // QSyntaxHighlighter passes and clipboard ops grow with line count. Cap
    // the visible scrollback to keep typing/scrolling smooth on huge decompiles.
    view_->setMaximumBlockCount(200'000);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->setPlaceholderText("Decompiled C output will appear here after analysis…");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar_);
    layout->addWidget(funcLabel_);
    layout->addWidget(view_);
}

void DecompiledCPanel::setSource(const QString& cSource) {
    //  1. Small (≤ 256 KiB):     one setPlainText, instant.
    //  2. Medium (≤ 2 MiB):      synchronous chunked insert (~hundreds of ms,
    //                            acceptable as a one-time pause at end of run).
    //  3. Large (> 2 MiB):       asynchronous chunked insert via QTimer so
    //                            the GUI stays responsive while loading; the
    //                            user sees lines stream in.
    constexpr int kSmallThreshold = 256 * 1024;
    constexpr int kAsyncThreshold = 2   * 1024 * 1024;
    constexpr int kSyncChunk      = 128 * 1024;

    view_->clear();
    pendingLoad_.clear();
    pendingLoadPos_   = 0;
    pendingLoadTotal_ = 0;

    if (cSource.size() <= kSmallThreshold) {
        view_->setPlainText(cSource);
        return;
    }

    if (cSource.size() <= kAsyncThreshold) {
        // Synchronous chunked.
        int pos = 0;
        QTextCursor c(view_->document());
        c.movePosition(QTextCursor::End);
        while (pos < cSource.size()) {
            int end = qMin(pos + kSyncChunk, cSource.size());
            const int nl = cSource.indexOf(QLatin1Char('\n'), end - 1);
            if (nl != -1 && nl - pos < kSyncChunk * 2)
                end = nl + 1;
            c.insertText(QStringView(cSource).mid(pos, end - pos).toString());
            pos = end;
        }
        return;
    }

    // Large: stream asynchronously.
    pendingLoadTotal_ = cSource.size();
    scheduleAsyncLoad(cSource);
}

bool DecompiledCPanel::setSourceFromPath(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    setSource(QString::fromUtf8(f.readAll()));
    return true;
}

void DecompiledCPanel::scheduleAsyncLoad(QString remainder) {
    pendingLoad_    = std::move(remainder);
    pendingLoadPos_ = 0;
    // Single-shot rearm: lets the event loop process input / repaint
    // between each chunk.
    QTimer::singleShot(0, this, &DecompiledCPanel::onAsyncLoadTick);
}

void DecompiledCPanel::onAsyncLoadTick() {
    const int remaining = pendingLoad_.size() - pendingLoadPos_;
    if (remaining <= 0) {
        funcLabel_->setText(QStringLiteral("Loaded %1 KiB")
                                    .arg(pendingLoadTotal_ / 1024));
        pendingLoad_.clear();
        pendingLoadPos_   = 0;
        pendingLoadTotal_ = 0;
        return;
    }
    // 32 KiB / tick keeps a single cursor.insertText under the 16 ms frame
    // budget on a modern desktop. The async chain runs ~30 ticks/s so a
    // 4 MiB .c finishes streaming in ~4 s — but the GUI never stalls.
    constexpr int kAsyncChunk = 32 * 1024;
    int take = qMin(kAsyncChunk, remaining);
    // Prefer to cut on a newline within the next kAsyncChunk window.
    const int searchEnd = pendingLoadPos_ + take;
    const int nl = pendingLoad_.indexOf(QLatin1Char('\n'), searchEnd - 1);
    if (nl != -1 && nl - pendingLoadPos_ < kAsyncChunk * 2)
        take = nl - pendingLoadPos_ + 1;

    QTextCursor c(view_->document());
    c.movePosition(QTextCursor::End);
    c.insertText(QStringView(pendingLoad_).mid(pendingLoadPos_, take).toString());
    pendingLoadPos_ += take;

    const qint64 doneKiB = static_cast<qint64>(pendingLoadPos_) / 1024;
    const qint64 totKiB  = pendingLoadTotal_ / 1024;
    funcLabel_->setText(QStringLiteral("Loading decompiled C… %1 / %2 KiB")
                                .arg(doneKiB).arg(totKiB));

    if (pendingLoadPos_ < pendingLoad_.size()) {
        QTimer::singleShot(0, this, &DecompiledCPanel::onAsyncLoadTick);
    } else {
        funcLabel_->setText(QStringLiteral("Loaded %1 KiB").arg(totKiB));
        pendingLoad_.clear();
        pendingLoadPos_   = 0;
        pendingLoadTotal_ = 0;
    }
}

void DecompiledCPanel::clear() {
    view_->clear();
    funcLabel_->setText("No function selected");
}

QString DecompiledCPanel::documentText() const {
    return view_->toPlainText();
}

void DecompiledCPanel::onFunctionSelected(uint64_t address, const QString& name) {
    funcLabel_->setText(QString("%1 @ 0x%2").arg(name).arg(address, 0, 16));
}

void DecompiledCPanel::onSaveAs() {
    QString path = QFileDialog::getSaveFileName(this, "Save Decompiled C",
        QString(), "C Source Files (*.c);;All Files (*)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream ts(&f);
        ts << view_->toPlainText();
        emit statusMessage("Saved: " + path, 3000);
    }
}

void DecompiledCPanel::onCopyToClipboard() {
    QApplication::clipboard()->setText(view_->toPlainText());
    emit statusMessage("Copied to clipboard", 2000);
}

} // namespace retdec::gui::panels
