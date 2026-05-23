#include "retdec/gui/panels/ir_panel.h"

#include "retdec/gui/widgets/empty_state_widget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QLabel>
#include <QComboBox>
#include <QStackedWidget>

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
    stageCombo_->setEnabled(false);
    stageCombo_->setToolTip(QStringLiteral("Stage selection not yet available"));
    topLayout->addWidget(funcLabel_, 1);
    topLayout->addWidget(new QLabel("Stage:", topBar));
    topLayout->addWidget(stageCombo_);

    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);

    emptyState_ = new retdec::gui::widgets::EmptyStateWidget(this);
    emptyState_->setTitle(QStringLiteral("No IR loaded"));
    emptyState_->setHint(QStringLiteral("Select a function after decompilation to inspect its SSA IR."));

    bodyStack_ = new QStackedWidget(this);
    bodyStack_->addWidget(emptyState_);
    bodyStack_->addWidget(view_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(topBar);
    layout->addWidget(bodyStack_, 1);

    connect(stageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &IRPanel::onStageChanged);
    updateEmptyState();
}

void IRPanel::updateEmptyState() {
    if (!bodyStack_ || !view_) return;
    bodyStack_->setCurrentIndex(view_->toPlainText().isEmpty() ? 0 : 1);
}

void IRPanel::setIRText(const QString& text, const QString& stage) {
    view_->setPlainText(text);
    int idx = stageCombo_->findText(stage);
    if (idx >= 0) stageCombo_->setCurrentIndex(idx);
    updateEmptyState();
}

void IRPanel::clear() {
    view_->clear();
    funcLabel_->setText("No function selected");
    updateEmptyState();
}

void IRPanel::applyEditorFont(const QFont& font) {
    if (view_)
        view_->setFont(font);
}

void IRPanel::onFunctionSelected(uint64_t address, const QString& name) {
    funcLabel_->setText(QString("%1 @ 0x%2").arg(name).arg(address, 0, 16));
}

void IRPanel::onStageChanged(int index) {
    // Trigger re-load of IR for the selected stage from the analysis pipeline.
    emit statusMessage(QString("IR stage: %1").arg(stageCombo_->itemText(index)));
}

} // namespace retdec::gui::panels
