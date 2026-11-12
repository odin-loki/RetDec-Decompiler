#ifndef RETDEC_GUI_PANELS_IR_H
#define RETDEC_GUI_PANELS_IR_H

#include "retdec/gui/panels/panel_base.h"

#include <QFont>

QT_BEGIN_NAMESPACE
class QPlainTextEdit;
class QComboBox;
class QLabel;
class QStackedWidget;
QT_END_NAMESPACE

namespace retdec::gui::widgets {
class EmptyStateWidget;
}

namespace retdec::gui::panels {

/**
 * @brief IRPanel — SSA intermediate representation viewer.
 *
 * Displays the SSA IR for a selected function, with:
 *   - Basic block headers (bold, dimmed background)
 *   - Value definitions highlighted in accent colour
 *   - Phi nodes in a distinct colour
 *   - Use-def cross-references (hover to highlight all uses of a value)
 *   - Stage selector combo (pre-SSA / SSA-renamed / after-DCE / after-IPA)
 */
class IRPanel : public PanelBase {
    Q_OBJECT
public:
    explicit IRPanel(QWidget* parent = nullptr);

    void setIRText(const QString& text, const QString& stage = "SSA");
    void clear() override;

    void applyEditorFont(const QFont& font);

public slots:
    void onFunctionSelected(uint64_t address, const QString& name);

private slots:
    void onStageChanged(int index);

private:
    void setupUI();
    void updateEmptyState();

    QComboBox*     stageCombo_ = nullptr;
    QLabel*        funcLabel_  = nullptr;
    QStackedWidget* bodyStack_ = nullptr;
    retdec::gui::widgets::EmptyStateWidget* emptyState_ = nullptr;
    QPlainTextEdit* view_      = nullptr;
};

} // namespace retdec::gui::panels
#endif
