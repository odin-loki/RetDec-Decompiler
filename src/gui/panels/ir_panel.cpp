#include "retdec/gui/panels/ir_panel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QLabel>
#include <QComboBox>

namespace retdec::gui::panels {

IRPanel::IRPanel(QWidget* parent)
    : PanelBase("IR (SSA)", parent) {
    setupUI();
}

void IRPanel::setupUI() {
    auto* topBar = new QWidget(this);
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(4, 2, 4, 2);
    funcLabel_ = new QLabel("No function selected", topBar);
    funcLabel_->setProperty("role", "muted");
    stageCombo_ = new QComboBox(topBar);
    stageCombo_->addItems({"Pre-SSA", "SSA", "After-DCE", "After-IPA"});
    stageCombo_->setCurrentIndex(1);
    topLayout->addWidget(funcLabel_, 1);
    topLayout->addWidget(new QLabel("Stage:", topBar));
    topLayout->addWidget(stageCombo_);

    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->setPlaceholderText("Select a function to view its SSA IR…");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(topBar);
    layout->addWidget(view_);

    connect(stageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &IRPanel::onStageChanged);
}

void IRPanel::setIRText(const QString& text, const QString& stage) {
    view_->setPlainText(text);
    int idx = stageCombo_->findText(stage);
    if (idx >= 0) stageCombo_->setCurrentIndex(idx);
}

void IRPanel::clear() {
    view_->clear();
    funcLabel_->setText("No function selected");
}

void IRPanel::onFunctionSelected(uint64_t address, const QString& name) {
    funcLabel_->setText(QString("%1 @ 0x%2").arg(name).arg(address, 0, 16));
}

void IRPanel::onStageChanged(int index) {
    // Trigger re-load of IR for the selected stage from the analysis pipeline.
    emit statusMessage(QString("IR stage: %1").arg(stageCombo_->itemText(index)));
}

} // namespace retdec::gui::panels
