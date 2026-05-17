#ifndef RETDEC_GUI_PANELS_ASSEMBLY_H
#define RETDEC_GUI_PANELS_ASSEMBLY_H

#include "retdec/gui/panels/panel_base.h"

QT_BEGIN_NAMESPACE
class QPlainTextEdit;
class QLabel;
class QLineEdit;
class QToolBar;
QT_END_NAMESPACE

namespace retdec::gui::panels {

/**
 * @brief AssemblyPanel — syntax-highlighted disassembly view.
 *
 * Displays the disassembly of the selected function with:
 *   - Address column (monospace, dimmed)
 *   - Mnemonic column (keyword-highlighted)
 *   - Operands column (register / immediate / memory coloured)
 *   - Inline comments from user annotations
 *   - Cross-reference tooltips on operands
 *
 * Keyboard:
 *   G       → Go to address
 *   F       → Find in current function
 *   Enter   → Follow jump/call target
 *   Escape  → Back in navigation history
 */
class AssemblyPanel : public PanelBase {
    Q_OBJECT
public:
    explicit AssemblyPanel(QWidget* parent = nullptr);

    void navigateTo(uint64_t address);
    void setAssemblyText(const QString& text);
    void clear() override;

public slots:
    void onAddressNavigated(uint64_t address);

private slots:
    void onGoToAddress();
    void onFind();

private:
    void setupUI();

    QToolBar*      toolbar_   = nullptr;
    QLabel*        funcLabel_ = nullptr;
    QPlainTextEdit* view_     = nullptr;
    QLineEdit*     searchBar_ = nullptr;
};

} // namespace retdec::gui::panels
#endif
