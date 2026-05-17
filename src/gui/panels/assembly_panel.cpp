#include "retdec/gui/panels/assembly_panel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QLabel>
#include <QLineEdit>
#include <QToolBar>
#include <QAction>

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
    view_->setPlaceholderText("Open a binary and select a function to view disassembly…");

    searchBar_ = new QLineEdit(this);
    searchBar_->setPlaceholderText("Search in disassembly…");
    searchBar_->setClearButtonEnabled(true);
    searchBar_->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar_);
    layout->addWidget(funcLabel_);
    layout->addWidget(view_);
    layout->addWidget(searchBar_);
}

void AssemblyPanel::navigateTo(uint64_t address) {
    // In the full implementation, scroll to the instruction at `address`.
    funcLabel_->setText(QString("Address: 0x%1").arg(address, 0, 16));
}

void AssemblyPanel::setAssemblyText(const QString& text) {
    view_->setPlainText(text);
}

void AssemblyPanel::clear() {
    view_->clear();
    funcLabel_->setText("No function selected");
}

void AssemblyPanel::onAddressNavigated(uint64_t address) {
    navigateTo(address);
}

void AssemblyPanel::onGoToAddress() {
    searchBar_->setPlaceholderText("Go to address (hex)…");
    searchBar_->setVisible(true);
    searchBar_->setFocus();
    searchBar_->clear();
}

void AssemblyPanel::onFind() {
    searchBar_->setPlaceholderText("Find in disassembly…");
    searchBar_->setVisible(true);
    searchBar_->setFocus();
    searchBar_->clear();
}

} // namespace retdec::gui::panels
