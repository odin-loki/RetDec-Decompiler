#ifndef RETDEC_GUI_PANELS_INSPECT_PANEL_H
#define RETDEC_GUI_PANELS_INSPECT_PANEL_H

#include "retdec/gui/panels/panel_base.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QProcess>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QTextBrowser;
class QPlainTextEdit;
class QTabWidget;
class QLineEdit;
class QCheckBox;
QT_END_NAMESPACE

namespace retdec {
namespace gui {
namespace panels {

/**
 * @brief Runs retdec-fileinfo (--json) and optional retdec-unpacker on the loaded binary.
 */
class InspectPanel : public PanelBase {
    Q_OBJECT
public:
    explicit InspectPanel(QWidget* parent = nullptr);

    void runFileinfo(const QString& binaryPath, const QString& fileinfoExecutable);
    const QJsonObject& fileinfoJson() const { return lastFileinfoJson_; }
    void clear() override;

signals:
    void requestDecompileMode();
    /// Load a different binary into the main window (e.g. unpacked output).
    void requestOpenBinary(const QString& absolutePath);
    /// Emitted when fileinfo or unpacker completes (for Command log).
    void cliToolFinished(const QString& tool, const QStringList& args, const QString& cwd,
                         int exitCode, qint64 elapsedMs, const QString& outputTail);
    /**
     * Emitted just before a QProcess is `start()`-ed. The main window hooks the
     * process into LiveConsolePanel so stdout/stderr stream live; the panel
     * itself never owns the console.
     * @param proc  The process (not null; lives in this panel's QObject tree).
     * @param label Display label, e.g. "retdec-fileinfo".
     */
    void processStarting(QProcess* proc, const QString& label);
    /// Parsed fileinfo JSON for the binary at @a absPath (empty object on failure).
    void fileinfoReady(const QJsonObject& root, const QString& absPath);

private slots:
    void onRefreshClicked();
    void onDecompileShortcutClicked();
    void onFileinfoFinished(int exitCode, QProcess::ExitStatus status);
    void onFileinfoError(QProcess::ProcessError err);
    void onUnpackClicked();
    void onUnpackFinished(int exitCode, QProcess::ExitStatus status);
    void onUnpackError(QProcess::ProcessError err);
    void onOpenUnpackedClicked();

private:
    void setupUi();
    void setFileinfoRunning(bool on);
    void setUnpackRunning(bool on);
    void updateUnpackOutputDefault();
    void updateButtonStates();

    QString lastBinaryPath_;
    QString lastFileinfoBinaryPath_;
    QJsonObject lastFileinfoJson_;
    QString fileinfoExe_;
    QProcess* fileinfoProc_ = nullptr;
    QProcess* unpackProc_ = nullptr;

    QLabel* status_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;
    QPushButton* decompileModeBtn_ = nullptr;
    QLineEdit* unpackOutEdit_ = nullptr;
    QCheckBox* unpackBrute_ = nullptr;
    QPushButton* unpackBtn_ = nullptr;
    QPushButton* openUnpackedBtn_ = nullptr;
    QTextBrowser* summary_ = nullptr;
    QPlainTextEdit* raw_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QElapsedTimer fileinfoTimer_;
    QElapsedTimer unpackTimer_;
};

} // namespace panels
} // namespace gui
} // namespace retdec

#endif
