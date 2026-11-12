#ifndef RETDEC_GUI_PANELS_SIGNATURE_STUDIO_PANEL_H
#define RETDEC_GUI_PANELS_SIGNATURE_STUDIO_PANEL_H

#include "retdec/gui/panels/panel_base.h"

#include <QElapsedTimer>
#include <QProcess>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
QT_END_NAMESPACE

namespace retdec {
namespace gui {
namespace panels {

/**
 * @brief Express (Python sig-from-lib) + stepwise AR / Mach-O / bin2pat / pat2yara.
 */
class SignatureStudioPanel : public PanelBase {
    Q_OBJECT
public:
    explicit SignatureStudioPanel(QWidget* parent = nullptr);

    void setActiveBinary(const QString& path);
    void clear() override;

signals:
    void cliToolFinished(const QString& tool, const QStringList& args, const QString& cwd,
                         int exitCode, qint64 elapsedMs, const QString& outputTail);

private slots:
    void onBrowseExpressLib();
    void onBrowseExpressOut();
    void onRunExpressPipeline();
    void onBrowseArArchive();
    void onBrowseArOut();
    void onBrowseMachoFile();
    void onBrowseMachoOut();
    void onBrowseBin2patIn();
    void onBrowseBin2patOut();
    void onBrowsePat2yaraIn();
    void onBrowsePat2yaraOut();
    void onRunArExtractor();
    void onRunMachoExtractor();
    void onRunBin2pat();
    void onRunPat2yara();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError err);

private:
    void setupUi();
    void setBusy(bool on);
    bool startCli(const QString& toolLabel, const QString& exe, const QStringList& args);
    bool startPythonCli(const QString& toolLabel, const QString& pythonExe, const QString& scriptPath,
                        const QStringList& scriptArgs);
    void syncDefaultsFromBinary();

    QString activeBinary_;
    QString pendingTool_;
    QStringList pendingArgs_;
    QString pendingCwd_;
    QElapsedTimer runTimer_;

    QProcess* proc_ = nullptr;
    QTabWidget* tabs_ = nullptr;

    QLineEdit* expressLib_ = nullptr;
    QLineEdit* expressOut_ = nullptr;
    QPushButton* expressRun_ = nullptr;

    QLineEdit* arArchive_ = nullptr;
    QLineEdit* arOutDir_ = nullptr;
    QPushButton* arRun_ = nullptr;

    QLineEdit* machoFile_ = nullptr;
    QLineEdit* machoOutDir_ = nullptr;
    QPushButton* machoRun_ = nullptr;

    QLineEdit* bin2patIn_ = nullptr;
    QLineEdit* bin2patOut_ = nullptr;
    QPushButton* bin2patRun_ = nullptr;

    QLineEdit* pat2yaraIn_ = nullptr;
    QLineEdit* pat2yaraOut_ = nullptr;
    QPushButton* pat2yaraRun_ = nullptr;

    QPlainTextEdit* lastLog_ = nullptr;
};

} // namespace panels
} // namespace gui
} // namespace retdec

#endif
