#include "retdec/gui/panels/assembly_panel.h"

#include "retdec/gui/address_context_menu.h"
#include "retdec/gui/widgets/empty_state_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QLabel>
#include <QLineEdit>
#include <QToolBar>
#include <QAction>
#include <QStackedWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextBlock>

namespace retdec::gui::panels {

AssemblyPanel::AssemblyPanel(QWidget* parent)
    : PanelBase("Assembly", parent) {
    setupUI();
}

void AssemblyPanel::setupUI() {
    toolbar_   = new QToolBar(this);
    toolbar_->setIconSize({16, 16});
    auto* goToAction = toolbar_->addAction("Go to Address (G)");
    auto* findAction = toolbar_->addAction("Find (F)");
    goToAction->setShortcut(Qt::Key_G);
    findAction->setShortcut(Qt::Key_F);
    connect(goToAction, &QAction::triggered, this, &AssemblyPanel::onGoToAddress);
    connect(findAction, &QAction::triggered, this, &AssemblyPanel::onFind);

    funcLabel_ = new QLabel("No function selected", this);
    funcLabel_->setProperty("role", "muted");

    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QPlainTextEdit::customContextMenuRequested,
            this, &AssemblyPanel::onContextMenu);

    emptyState_ = new retdec::gui::widgets::EmptyStateWidget(this);
    emptyState_->setTitle(QStringLiteral("No disassembly yet"));
    emptyState_->setHint(QStringLiteral("Select a function in the Functions list to view its assembly."));

    bodyStack_ = new QStackedWidget(this);
    bodyStack_->addWidget(emptyState_);
    bodyStack_->addWidget(view_);

    searchBar_ = new QLineEdit(this);
    searchBar_->setPlaceholderText("Search in disassembly…");
    searchBar_->setClearButtonEnabled(true);
    searchBar_->hide();
    connect(searchBar_, &QLineEdit::returnPressed,
            this, &AssemblyPanel::onSearchReturnPressed);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar_);
    layout->addWidget(funcLabel_);
    layout->addWidget(bodyStack_, 1);
    layout->addWidget(searchBar_);
    updateEmptyState();
}

void AssemblyPanel::updateEmptyState() {
    if (!bodyStack_ || !view_) return;
    bodyStack_->setCurrentIndex(view_->toPlainText().isEmpty() ? 0 : 1);
}

void AssemblyPanel::scrollToAddress(uint64_t address) {
    if (view_->document()->isEmpty())
        return;

    const QString needle = QStringLiteral("0x%1").arg(address, 0, 16);
    const QString text = view_->toPlainText();
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        if (!lines[i].contains(needle, Qt::CaseInsensitive))
            continue;
        QTextBlock block = view_->document()->findBlockByNumber(i);
        if (!block.isValid())
            continue;
        QTextCursor c(block);
        view_->setTextCursor(c);
        view_->centerCursor();
        return;
    }
}

void AssemblyPanel::navigateTo(uint64_t address) {
    pendingScrollAddr_ = address;
    funcLabel_->setText(QString("Address: 0x%1").arg(address, 0, 16));
    scrollToAddress(address);
}

void AssemblyPanel::setAssemblyText(const QString& text) {
    view_->setPlainText(text);
    updateEmptyState();
    if (pendingScrollAddr_ != 0)
        scrollToAddress(pendingScrollAddr_);
}

void AssemblyPanel::clear() {
    view_->clear();
    funcLabel_->setText("No function selected");
    pendingScrollAddr_ = 0;
    updateEmptyState();
}

void AssemblyPanel::applyEditorFont(const QFont& font) {
    if (view_)
        view_->setFont(font);
}

void AssemblyPanel::onAddressNavigated(uint64_t address) {
    navigateTo(address);
}

void AssemblyPanel::onGoToAddress() {
    searchMode_ = SearchMode::GoTo;
    searchBar_->setPlaceholderText("Go to address (hex)…");
    searchBar_->setVisible(true);
    searchBar_->setFocus();
    searchBar_->clear();
}

void AssemblyPanel::onFind() {
    searchMode_ = SearchMode::Find;
    searchBar_->setPlaceholderText("Find in disassembly…");
    searchBar_->setVisible(true);
    searchBar_->setFocus();
    searchBar_->clear();
}

void AssemblyPanel::onSearchReturnPressed() {
    const QString query = searchBar_->text().trimmed();
    if (query.isEmpty())
        return;

    if (searchMode_ == SearchMode::GoTo) {
        bool ok = false;
        QString hex = query;
        if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            hex = hex.mid(2);
        const uint64_t addr = hex.toULongLong(&ok, 16);
        if (ok)
            navigateTo(addr);
        return;
    }

    QTextDocument::FindFlags flags;
    if (!view_->find(query, flags)) {
        QTextCursor c = view_->textCursor();
        c.movePosition(QTextCursor::Start);
        view_->setTextCursor(c);
        view_->find(query, flags);
    }
}

void AssemblyPanel::onContextMenu(const QPoint& pos)
{
    const QTextCursor cursor = view_->cursorForPosition(pos);
    const QString line = cursor.block().text();
    const auto addr = retdec::gui::parseFirstAddress(line);

    QMenu menu(this);
    auto* copyAct = menu.addAction(retdec::gui::kCopyAddressLabel);
    copyAct->setEnabled(addr.has_value());
    auto* goAct = menu.addAction(retdec::gui::kGoToFunctionLabel);
    goAct->setEnabled(addr.has_value());

    QAction* chosen = menu.exec(view_->mapToGlobal(pos));
    if (!chosen || !addr.has_value())
        return;
    if (chosen == copyAct)
        retdec::gui::copyAddressToClipboard(*addr);
    else if (chosen == goAct)
        emit addressNavigated(*addr);
}

} // namespace retdec::gui::panels
