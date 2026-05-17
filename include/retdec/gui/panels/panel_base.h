#ifndef RETDEC_GUI_PANELS_PANEL_BASE_H
#define RETDEC_GUI_PANELS_PANEL_BASE_H

#include <QWidget>
#include <QString>

namespace retdec {
namespace gui {
namespace panels {

/**
 * @brief Base class for all RetDec analysis panels.
 *
 * Each panel has a human-readable title used for dock widget labels and
 * view menu toggle actions, and an optional icon name for the toolbar.
 */
class PanelBase : public QWidget {
    Q_OBJECT
public:
    explicit PanelBase(const QString& title, QWidget* parent = nullptr);
    ~PanelBase() override = default;

    QString panelTitle() const { return title_; }

    /// Called when the active binary / project changes.
    virtual void onProjectChanged() {}

    /// Called when analysis completes a stage.
    virtual void onAnalysisStageComplete(const QString& stage) { Q_UNUSED(stage) }

    /// Clear all displayed content (e.g. when closing a project).
    virtual void clear() {}

signals:
    void addressNavigated(uint64_t address);
    void functionSelected(uint64_t address, const QString& name);
    void statusMessage(const QString& msg, int timeoutMs = 3000);

protected:
    QString title_;
};

} // namespace panels
} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_PANELS_PANEL_BASE_H
