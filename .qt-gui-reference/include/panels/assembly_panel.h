#ifndef RETDEC_GUI_PANELS_ASSEMBLY_H
#define RETDEC_GUI_PANELS_ASSEMBLY_H

#include "retdec/gui/panels/panel_base.h"

#include <QFont>

QT_BEGIN_NAMESPACE
class QPlainTextEdit;
class QLabel;
class QLineEdit;
class QToolBar;
class QStackedWidget;
QT_END_NAMESPACE

namespace retdec::gui::widgets {
class EmptyStateWidget;
}

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

    void applyEditorFont(const QFont& font);

public slots:
    void onAddressNavigated(uint64_t address);

private slots:
    void onGoToAddress();
    void onFind();
    void onSearchReturnPressed();
    void onContextMenu(const QPoint& pos);

private:
    enum class SearchMode { Find, GoTo };

    void setupUI();
    void scrollToAddress(uint64_t address);
    void updateEmptyState();

    QToolBar*      toolbar_   = nullptr;
    QLabel*        funcLabel_ = nullptr;
    QStackedWidget* bodyStack_ = nullptr;
    retdec::gui::widgets::EmptyStateWidget* emptyState_ = nullptr;
    QPlainTextEdit* view_     = nullptr;
    QLineEdit*     searchBar_ = nullptr;
    SearchMode     searchMode_ = SearchMode::Find;
    uint64_t       pendingScrollAddr_ = 0;
};

} // namespace retdec::gui::panels
#endif
