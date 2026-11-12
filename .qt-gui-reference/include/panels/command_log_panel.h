#ifndef RETDEC_GUI_PANELS_COMMAND_LOG_PANEL_H
#define RETDEC_GUI_PANELS_COMMAND_LOG_PANEL_H

#include "retdec/gui/panels/panel_base.h"

QT_BEGIN_NAMESPACE
class QPlainTextEdit;
class QPushButton;
QT_END_NAMESPACE

namespace retdec {
namespace gui {
namespace panels {

/**
 * @brief Append-only log of external tool invocations (command, cwd, exit, timing).
 */
class CommandLogPanel : public PanelBase {
    Q_OBJECT
public:
    explicit CommandLogPanel(QWidget* parent = nullptr);

    void appendRun(const QString& tool, const QStringList& args, const QString& cwd,
                   int exitCode, qint64 elapsedMs, const QString& outputTail);
    void clear() override;

    /// Cap on retained QTextBlocks (default 20000). Prevents long sessions
    /// from accumulating unbounded memory.
    void setMaxBlocks(int blocks);
    int  maxBlocks() const;

private slots:
    void onCopyAll();
    void onClearClicked();

private:
    QPlainTextEdit* text_ = nullptr;
    QPushButton* copyBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    int maxBlocks_ = 20'000;
};

} // namespace panels
} // namespace gui
} // namespace retdec

#endif
