#include "retdec/gui/panels/binary_browser_panel.h"

#include <QVBoxLayout>
#include <QSplitter>
#include <QTreeWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QHeaderView>

namespace retdec::gui::panels {

BinaryBrowserPanel::BinaryBrowserPanel(QWidget* parent)
    : PanelBase("Binary Browser", parent) {
    setupUI();
}

void BinaryBrowserPanel::setupUI() {
    splitter_    = new QSplitter(Qt::Vertical, this);
    sectionTree_ = new QTreeWidget(splitter_);
    sectionTree_->setColumnCount(3);
    sectionTree_->setHeaderLabels({"Name", "Address", "Size"});
    sectionTree_->header()->setStretchLastSection(false);
    sectionTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    sectionTree_->setAlternatingRowColors(true);
    sectionTree_->setRootIsDecorated(true);

    auto* hexWidget = new QWidget(splitter_);
    auto* hexLayout = new QVBoxLayout(hexWidget);
    hexLayout->setContentsMargins(0, 0, 0, 0);
    hexLayout->setSpacing(4);
    hexHeader_ = new QLabel("Hex View", hexWidget);
    hexHeader_->setProperty("role", "muted");
    hexView_   = new QPlainTextEdit(hexWidget);
    hexView_->setReadOnly(true);
    hexView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    hexView_->setPlaceholderText("Select a section to view its hex dump…");
    hexLayout->addWidget(hexHeader_);
    hexLayout->addWidget(hexView_);

    splitter_->addWidget(sectionTree_);
    splitter_->addWidget(hexWidget);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter_);

    connect(sectionTree_, &QTreeWidget::itemDoubleClicked,
            this, &BinaryBrowserPanel::onItemDoubleClicked);
}

void BinaryBrowserPanel::loadBinary(const QString& path) {
    sectionTree_->clear();
    hexView_->clear();
    // Populate with placeholder sections until the real binary loader is wired in.
    auto* root = new QTreeWidgetItem(sectionTree_, {path, "", ""});
    root->setExpanded(true);
    auto addSection = [&](const QString& name, const QString& addr, const QString& size) {
        new QTreeWidgetItem(root, {name, addr, size});
    };
    addSection(".text",   "0x401000", "42 KB");
    addSection(".rodata", "0x40b000", "12 KB");
    addSection(".data",   "0x40e000", " 4 KB");
    addSection(".bss",    "0x40f000", " 2 KB");
    sectionTree_->addTopLevelItem(root);
}

void BinaryBrowserPanel::clear() {
    sectionTree_->clear();
    hexView_->clear();
}

void BinaryBrowserPanel::onItemDoubleClicked() {
    auto* item = sectionTree_->currentItem();
    if (!item) return;
    bool ok = false;
    uint64_t addr = item->text(1).toULongLong(&ok, 16);
    if (ok) emit addressNavigated(addr);
}

} // namespace retdec::gui::panels
