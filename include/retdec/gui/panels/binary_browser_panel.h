#ifndef RETDEC_GUI_PANELS_BINARY_BROWSER_H
#define RETDEC_GUI_PANELS_BINARY_BROWSER_H

#include "retdec/gui/panels/panel_base.h"

QT_BEGIN_NAMESPACE
class QTreeWidget;
class QLabel;
class QSplitter;
class QPlainTextEdit;
QT_END_NAMESPACE

namespace retdec::gui::panels {

/**
 * @brief BinaryBrowserPanel — section/segment explorer + hex dump.
 *
 * Top section: collapsible tree of ELF/PE sections, symbols, imports, exports.
 * Bottom section: hex+ASCII dump of the selected region.
 *
 * The panel emits addressNavigated() when the user double-clicks an item so
 * the AssemblyPanel can scroll to that address.
 */
class BinaryBrowserPanel : public PanelBase {
    Q_OBJECT
public:
    explicit BinaryBrowserPanel(QWidget* parent = nullptr);

    void loadBinary(const QString& path);
    void clear() override;

private slots:
    void onItemDoubleClicked();

private:
    void setupUI();

    QSplitter*     splitter_     = nullptr;
    QTreeWidget*   sectionTree_  = nullptr;
    QLabel*        hexHeader_    = nullptr;
    QPlainTextEdit* hexView_     = nullptr;
};

} // namespace retdec::gui::panels
#endif
