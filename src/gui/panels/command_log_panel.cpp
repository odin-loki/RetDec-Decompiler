#include "retdec/gui/panels/command_log_panel.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextCursor>
#include <QVBoxLayout>

namespace retdec {
namespace gui {
namespace panels {

CommandLogPanel::CommandLogPanel(QWidget* parent)
    : PanelBase(QStringLiteral("Command log"), parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* row = new QHBoxLayout();
    copyBtn_ = new QPushButton(QStringLiteral("Copy all"), this);
    clearBtn_ = new QPushButton(QStringLiteral("Clear"), this);
    copyBtn_->setToolTip(
            QStringLiteral("Copy the full log to the clipboard. Ctrl+Shift+C when this panel has focus."));
    clearBtn_->setToolTip(QStringLiteral("Clear this view (does not affect Diagnostics)."));
    row->addWidget(copyBtn_);
    row->addWidget(clearBtn_);
    row->addStretch(1);
    lay->addLayout(row);
    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    text_->setUndoRedoEnabled(false);
    text_->setMaximumBlockCount(maxBlocks_);
    text_->setPlaceholderText(
            QStringLiteral("External tools (retdec-fileinfo, retdec-decompiler, …) are logged here."));
    lay->addWidget(text_);
    auto* copySc = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this);
    copySc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copySc, &QShortcut::activated, this, &CommandLogPanel::onCopyAll);
    connect(copyBtn_, &QPushButton::clicked, this, &CommandLogPanel::onCopyAll);
    connect(clearBtn_, &QPushButton::clicked, this, &CommandLogPanel::onClearClicked);
}

void CommandLogPanel::onCopyAll() {
    QGuiApplication::clipboard()->setText(text_->toPlainText());
}

void CommandLogPanel::onClearClicked() {
    text_->clear();
}

void CommandLogPanel::appendRun(const QString& tool, const QStringList& args, const QString& cwd,
                                int exitCode, qint64 elapsedMs, const QString& outputTail) {
    const QString ts = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QString line = QStringLiteral("[%1] %2\n  cwd: %3\n  exit: %4  time: %5 ms\n  cmd:")
                           .arg(ts, tool, cwd)
                           .arg(exitCode)
                           .arg(elapsedMs);
    for (const QString& a : args)
        line += QStringLiteral(" ") + a;
    line += QLatin1Char('\n');
    if (!outputTail.trimmed().isEmpty())
        line += QStringLiteral("  --- output (tail) ---\n") + outputTail + QLatin1Char('\n');
    line += QLatin1Char('\n');
    // appendPlainText() already implicitly moves the cursor to the end and
    // scrolls; an explicit setTextCursor(End) here was clearing selections.
    text_->appendPlainText(line);
}

void CommandLogPanel::clear() {
    text_->clear();
}

void CommandLogPanel::setMaxBlocks(int blocks) {
    maxBlocks_ = qMax(100, blocks);
    if (text_)
        text_->setMaximumBlockCount(maxBlocks_);
}

int CommandLogPanel::maxBlocks() const {
    return maxBlocks_;
}

} // namespace panels
} // namespace gui
} // namespace retdec
