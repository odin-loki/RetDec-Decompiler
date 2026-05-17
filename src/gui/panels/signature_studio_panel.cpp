#include "retdec/gui/panels/signature_studio_panel.h"
#include "retdec/gui/cli_tool_paths.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

namespace retdec {
namespace gui {
namespace panels {

SignatureStudioPanel::SignatureStudioPanel(QWidget* parent)
    : PanelBase(QStringLiteral("Signature Studio"), parent) {
    proc_ = new QProcess(this);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &SignatureStudioPanel::onProcessFinished);
    connect(proc_, &QProcess::errorOccurred, this, &SignatureStudioPanel::onProcessError);
    setupUi();
}

void SignatureStudioPanel::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    auto* hint = new QLabel(
            QStringLiteral("<i>Uses RetDec tools next to the GUI or on PATH. "
                           "Pick inputs below; outputs go to the Command log on completion.</i>"),
            this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    tabs_ = new QTabWidget(this);

    // ── Express: retdec-signature-from-library-creator.py ──────────────────
    auto* exPage = new QWidget();
    auto* exLay = new QVBoxLayout(exPage);
    auto* exBox = new QGroupBox(
            QStringLiteral("retdec-signature-from-library-creator.py (Python 3)"), exPage);
    auto* exForm = new QFormLayout(exBox);
    expressLib_ = new QLineEdit(exBox);
    auto* exBrowseL = new QPushButton(QStringLiteral("Browse…"), exBox);
    auto* exLRow = new QWidget(exBox);
    auto* exLL = new QHBoxLayout(exLRow);
    exLL->setContentsMargins(0, 0, 0, 0);
    exLL->addWidget(expressLib_, 1);
    exLL->addWidget(exBrowseL);
    exForm->addRow(QStringLiteral("Static library / archive:"), exLRow);
    expressOut_ = new QLineEdit(exBox);
    auto* exBrowseO = new QPushButton(QStringLiteral("Browse…"), exBox);
    auto* exORow = new QWidget(exBox);
    auto* exOL = new QHBoxLayout(exORow);
    exOL->setContentsMargins(0, 0, 0, 0);
    exOL->addWidget(expressOut_, 1);
    exOL->addWidget(exBrowseO);
    exForm->addRow(QStringLiteral("Output YARA:"), exORow);
    expressRun_ = new QPushButton(QStringLiteral("Run express pipeline"), exBox);
    expressRun_->setToolTip(
            QStringLiteral("Requires Python 3, installed RetDec scripts directory, and tools next to the script."));
    exForm->addRow(expressRun_);
    auto* exNote = new QLabel(
            QStringLiteral("<i>Script must live in the installed <code>scripts</code> folder with "
                           "<code>retdec-ar-extractor</code>, <code>retdec-bin2pat</code>, "
                           "<code>retdec-pat2yara</code> beside it (standard install layout).</i>"),
            exBox);
    exNote->setWordWrap(true);
    exForm->addRow(exNote);
    exLay->addWidget(exBox);
    exLay->addStretch(1);
    connect(exBrowseL, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowseExpressLib);
    connect(exBrowseO, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowseExpressOut);
    connect(expressRun_, &QPushButton::clicked, this, &SignatureStudioPanel::onRunExpressPipeline);
    tabs_->addTab(exPage, QStringLiteral("Express"));

    // ── AR archive ───────────────────────────────────────────────────────
    auto* arPage = new QWidget();
    auto* arLay = new QVBoxLayout(arPage);
    auto* arBox = new QGroupBox(QStringLiteral("retdec-ar-extractor"), arPage);
    auto* arForm = new QFormLayout(arBox);
    arArchive_ = new QLineEdit(arBox);
    auto* arBrowseA = new QPushButton(QStringLiteral("Browse…"), arBox);
    auto* arArchRow = new QWidget(arBox);
    auto* arArchL = new QHBoxLayout(arArchRow);
    arArchL->setContentsMargins(0, 0, 0, 0);
    arArchL->addWidget(arArchive_, 1);
    arArchL->addWidget(arBrowseA);
    arForm->addRow(QStringLiteral("Archive (.a):"), arArchRow);
    arOutDir_ = new QLineEdit(arBox);
    auto* arBrowseO = new QPushButton(QStringLiteral("Browse…"), arBox);
    auto* arOutRow = new QWidget(arBox);
    auto* arOutL = new QHBoxLayout(arOutRow);
    arOutL->setContentsMargins(0, 0, 0, 0);
    arOutL->addWidget(arOutDir_, 1);
    arOutL->addWidget(arBrowseO);
    arForm->addRow(QStringLiteral("Output directory:"), arOutRow);
    arRun_ = new QPushButton(QStringLiteral("Extract all (-e)"), arBox);
    arForm->addRow(arRun_);
    arLay->addWidget(arBox);
    arLay->addStretch(1);
    connect(arBrowseA, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowseArArchive);
    connect(arBrowseO, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowseArOut);
    connect(arRun_, &QPushButton::clicked, this, &SignatureStudioPanel::onRunArExtractor);
    tabs_->addTab(arPage, QStringLiteral("AR"));

    // ── Mach-O fat / universal ─────────────────────────────────────────────
    auto* moPage = new QWidget();
    auto* moLay = new QVBoxLayout(moPage);
    auto* moBox = new QGroupBox(QStringLiteral("retdec-macho-extractor"), moPage);
    auto* moForm = new QFormLayout(moBox);
    machoFile_ = new QLineEdit(moBox);
    auto* moBrowseF = new QPushButton(QStringLiteral("Browse…"), moBox);
    auto* moFRow = new QWidget(moBox);
    auto* moFL = new QHBoxLayout(moFRow);
    moFL->setContentsMargins(0, 0, 0, 0);
    moFL->addWidget(machoFile_, 1);
    moFL->addWidget(moBrowseF);
    moForm->addRow(QStringLiteral("Mach-O / archive:"), moFRow);
    machoOutDir_ = new QLineEdit(moBox);
    auto* moBrowseO = new QPushButton(QStringLiteral("Browse…"), moBox);
    auto* moORow = new QWidget(moBox);
    auto* moOL = new QHBoxLayout(moORow);
    moOL->setContentsMargins(0, 0, 0, 0);
    moOL->addWidget(machoOutDir_, 1);
    moOL->addWidget(moBrowseO);
    moForm->addRow(QStringLiteral("Output directory:"), moORow);
    machoRun_ = new QPushButton(QStringLiteral("Extract all (--all)"), moBox);
    moForm->addRow(machoRun_);
    moLay->addWidget(moBox);
    moLay->addStretch(1);
    connect(moBrowseF, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowseMachoFile);
    connect(moBrowseO, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowseMachoOut);
    connect(machoRun_, &QPushButton::clicked, this, &SignatureStudioPanel::onRunMachoExtractor);
    tabs_->addTab(moPage, QStringLiteral("Mach-O"));

    // ── bin2pat / pat2yara ───────────────────────────────────────────────
    auto* patPage = new QWidget();
    auto* patLay = new QVBoxLayout(patPage);
    auto* b2pBox = new QGroupBox(QStringLiteral("retdec-bin2pat"), patPage);
    auto* b2pForm = new QFormLayout(b2pBox);
    bin2patIn_ = new QLineEdit(b2pBox);
    auto* b2pBi = new QPushButton(QStringLiteral("Browse…"), b2pBox);
    auto* b2pIRow = new QWidget(b2pBox);
    auto* b2pIL = new QHBoxLayout(b2pIRow);
    b2pIL->setContentsMargins(0, 0, 0, 0);
    b2pIL->addWidget(bin2patIn_, 1);
    b2pIL->addWidget(b2pBi);
    b2pForm->addRow(QStringLiteral("Object / binary:"), b2pIRow);
    bin2patOut_ = new QLineEdit(b2pBox);
    auto* b2pBo = new QPushButton(QStringLiteral("Browse…"), b2pBox);
    auto* b2pORow = new QWidget(b2pBox);
    auto* b2pOL = new QHBoxLayout(b2pORow);
    b2pOL->setContentsMargins(0, 0, 0, 0);
    b2pOL->addWidget(bin2patOut_, 1);
    b2pOL->addWidget(b2pBo);
    b2pForm->addRow(QStringLiteral("Output (.pat / rules):"), b2pORow);
    bin2patRun_ = new QPushButton(QStringLiteral("Run bin2pat"), b2pBox);
    b2pForm->addRow(bin2patRun_);
    connect(b2pBi, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowseBin2patIn);
    connect(b2pBo, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowseBin2patOut);
    connect(bin2patRun_, &QPushButton::clicked, this, &SignatureStudioPanel::onRunBin2pat);
    patLay->addWidget(b2pBox);

    auto* p2yBox = new QGroupBox(QStringLiteral("retdec-pat2yara"), patPage);
    auto* p2yForm = new QFormLayout(p2yBox);
    pat2yaraIn_ = new QLineEdit(p2yBox);
    auto* p2yBi = new QPushButton(QStringLiteral("Browse…"), p2yBox);
    auto* p2yIRow = new QWidget(p2yBox);
    auto* p2yIL = new QHBoxLayout(p2yIRow);
    p2yIL->setContentsMargins(0, 0, 0, 0);
    p2yIL->addWidget(pat2yaraIn_, 1);
    p2yIL->addWidget(p2yBi);
    p2yForm->addRow(QStringLiteral("Input from bin2pat:"), p2yIRow);
    pat2yaraOut_ = new QLineEdit(p2yBox);
    auto* p2yBo = new QPushButton(QStringLiteral("Browse…"), p2yBox);
    auto* p2yORow = new QWidget(p2yBox);
    auto* p2yOL = new QHBoxLayout(p2yORow);
    p2yOL->setContentsMargins(0, 0, 0, 0);
    p2yOL->addWidget(pat2yaraOut_, 1);
    p2yOL->addWidget(p2yBo);
    p2yForm->addRow(QStringLiteral("Output YARA:"), p2yORow);
    pat2yaraRun_ = new QPushButton(QStringLiteral("Run pat2yara"), p2yBox);
    p2yForm->addRow(pat2yaraRun_);
    connect(p2yBi, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowsePat2yaraIn);
    connect(p2yBo, &QPushButton::clicked, this, &SignatureStudioPanel::onBrowsePat2yaraOut);
    connect(pat2yaraRun_, &QPushButton::clicked, this, &SignatureStudioPanel::onRunPat2yara);
    patLay->addWidget(p2yBox);
    patLay->addStretch(1);
    tabs_->addTab(patPage, QStringLiteral("PAT / YARA"));

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(tabs_);
    root->addWidget(scroll, 1);

    lastLog_ = new QPlainTextEdit(this);
    lastLog_->setReadOnly(true);
    lastLog_->setMaximumBlockCount(200);
    lastLog_->setPlaceholderText(QStringLiteral("Last tool output tail…"));
    lastLog_->setFixedHeight(100);
    root->addWidget(lastLog_);
}

void SignatureStudioPanel::setActiveBinary(const QString& path) {
    activeBinary_ = path;
    expressLib_->clear();
    expressOut_->clear();
    syncDefaultsFromBinary();
}

void SignatureStudioPanel::syncDefaultsFromBinary() {
    if (activeBinary_.isEmpty())
        return;
    if (expressLib_->text().trimmed().isEmpty())
        expressLib_->setText(activeBinary_);
    if (expressOut_->text().trimmed().isEmpty()) {
        const QFileInfo fi(activeBinary_);
        expressOut_->setText(
                QDir(fi.absolutePath()).filePath(fi.completeBaseName() + QStringLiteral("-libsig.yar")));
    }
    if (arArchive_->text().trimmed().isEmpty())
        arArchive_->setText(activeBinary_);
    if (machoFile_->text().trimmed().isEmpty())
        machoFile_->setText(activeBinary_);
    const QString dir = QFileInfo(activeBinary_).absolutePath();
    const QString sub = QDir(dir).filePath(QStringLiteral("retdec-sig-work"));
    if (arOutDir_->text().trimmed().isEmpty())
        arOutDir_->setText(sub);
    if (machoOutDir_->text().trimmed().isEmpty())
        machoOutDir_->setText(sub);
}

void SignatureStudioPanel::clear() {
    activeBinary_.clear();
    lastLog_->clear();
}

void SignatureStudioPanel::setBusy(bool on) {
    const bool en = !on;
    expressRun_->setEnabled(en);
    arRun_->setEnabled(en);
    machoRun_->setEnabled(en);
    bin2patRun_->setEnabled(en);
    pat2yaraRun_->setEnabled(en);
}

bool SignatureStudioPanel::startCli(const QString& toolLabel, const QString& exe,
                                    const QStringList& args) {
    if (exe.isEmpty()) {
        lastLog_->appendPlainText(QStringLiteral("Tool not found: ") + toolLabel);
        return false;
    }
    if (proc_->state() != QProcess::NotRunning)
        return false;
    pendingTool_ = toolLabel;
    pendingArgs_ = args;
    pendingCwd_ = QFileInfo(exe).absolutePath();
    runTimer_.start();
    proc_->setProgram(exe);
    proc_->setArguments(args);
    proc_->setWorkingDirectory(pendingCwd_);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    setBusy(true);
    proc_->start();
    if (!proc_->waitForStarted(8000)) {
        lastLog_->appendPlainText(QStringLiteral("Failed to start: ") + toolLabel);
        setBusy(false);
        return false;
    }
    return true;
}

bool SignatureStudioPanel::startPythonCli(const QString& toolLabel, const QString& pythonExe,
                                          const QString& scriptPath, const QStringList& scriptArgs) {
    if (pythonExe.isEmpty() || scriptPath.isEmpty()) {
        lastLog_->appendPlainText(
                QStringLiteral("Express: Python 3 or retdec-signature-from-library-creator.py not found. "
                               "Use a full RetDec install with scripts/ on disk."));
        return false;
    }
    if (proc_->state() != QProcess::NotRunning)
        return false;
    QStringList argv;
    argv << scriptPath << scriptArgs;
    pendingTool_ = toolLabel;
    pendingArgs_ = argv;
    pendingCwd_ = QFileInfo(scriptPath).absolutePath();
    runTimer_.start();
    proc_->setProgram(pythonExe);
    proc_->setArguments(argv);
    proc_->setWorkingDirectory(pendingCwd_);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    setBusy(true);
    proc_->start();
    if (!proc_->waitForStarted(12000)) {
        lastLog_->appendPlainText(QStringLiteral("Failed to start: ") + toolLabel);
        setBusy(false);
        return false;
    }
    return true;
}

void SignatureStudioPanel::onProcessFinished(int exitCode, QProcess::ExitStatus) {
    setBusy(false);
    QByteArray out = proc_->readAllStandardOutput();
    QString tail = QString::fromUtf8(out);
    if (tail.size() > 3500)
        tail = tail.right(3500);
    lastLog_->appendPlainText(QStringLiteral("--- %1 exit %2 ---\n%3")
                                      .arg(pendingTool_)
                                      .arg(exitCode)
                                      .arg(tail));
    emit cliToolFinished(pendingTool_, pendingArgs_, pendingCwd_, exitCode, runTimer_.elapsed(),
                         tail);
}

void SignatureStudioPanel::onProcessError(QProcess::ProcessError err) {
    Q_UNUSED(err);
    setBusy(false);
    const QString msg = proc_->errorString();
    lastLog_->appendPlainText(QStringLiteral("Error: ") + msg);
    emit cliToolFinished(pendingTool_, pendingArgs_, pendingCwd_, -1, runTimer_.elapsed(), msg);
}

void SignatureStudioPanel::onBrowseExpressLib() {
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Library / archive"),
                                                   expressLib_->text(), QStringLiteral("All (*.*)"));
    if (!p.isEmpty())
        expressLib_->setText(p);
}

void SignatureStudioPanel::onBrowseExpressOut() {
    const QString p = QFileDialog::getSaveFileName(this, QStringLiteral("YARA output"), expressOut_->text(),
                                                   QStringLiteral("YARA (*.yar);;All (*.*)"));
    if (!p.isEmpty())
        expressOut_->setText(p);
}

void SignatureStudioPanel::onRunExpressPipeline() {
    const QString py = resolvePythonInterpreter();
    const QString script = resolveSignatureFromLibraryCreatorScript();
    const QString lib = expressLib_->text().trimmed();
    const QString out = expressOut_->text().trimmed();
    if (lib.isEmpty() || out.isEmpty()) {
        lastLog_->appendPlainText(QStringLiteral("Express: set library input and YARA output path."));
        return;
    }
    const QStringList scriptArgs{QStringLiteral("-o"), out, lib};
    startPythonCli(QStringLiteral("retdec-signature-from-library-creator.py"), py, script, scriptArgs);
}

void SignatureStudioPanel::onBrowseArArchive() {
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Archive"), arArchive_->text(),
                                                   QStringLiteral("All (*.*)"));
    if (!p.isEmpty())
        arArchive_->setText(p);
}

void SignatureStudioPanel::onBrowseArOut() {
    const QString p = QFileDialog::getExistingDirectory(this, QStringLiteral("Output directory"),
                                                        arOutDir_->text());
    if (!p.isEmpty())
        arOutDir_->setText(p);
}

void SignatureStudioPanel::onBrowseMachoFile() {
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Mach-O file"),
                                                    machoFile_->text(), QStringLiteral("All (*.*)"));
    if (!p.isEmpty())
        machoFile_->setText(p);
}

void SignatureStudioPanel::onBrowseMachoOut() {
    const QString p = QFileDialog::getExistingDirectory(this, QStringLiteral("Output directory"),
                                                        machoOutDir_->text());
    if (!p.isEmpty())
        machoOutDir_->setText(p);
}

void SignatureStudioPanel::onBrowseBin2patIn() {
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Input for bin2pat"),
                                                    bin2patIn_->text(), QStringLiteral("All (*.*)"));
    if (!p.isEmpty())
        bin2patIn_->setText(p);
}

void SignatureStudioPanel::onBrowseBin2patOut() {
    const QString p = QFileDialog::getSaveFileName(this, QStringLiteral("bin2pat output"),
                                                    bin2patOut_->text(), QStringLiteral("All (*.*)"));
    if (!p.isEmpty())
        bin2patOut_->setText(p);
}

void SignatureStudioPanel::onBrowsePat2yaraIn() {
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("pat2yara input"),
                                                    pat2yaraIn_->text(), QStringLiteral("All (*.*)"));
    if (!p.isEmpty())
        pat2yaraIn_->setText(p);
}

void SignatureStudioPanel::onBrowsePat2yaraOut() {
    const QString p = QFileDialog::getSaveFileName(this, QStringLiteral("YARA output"),
                                                    pat2yaraOut_->text(),
                                                    QStringLiteral("YARA (*.yar);;All (*.*)"));
    if (!p.isEmpty())
        pat2yaraOut_->setText(p);
}

void SignatureStudioPanel::onRunArExtractor() {
    const QString exe = resolveRetdecArExtractorExecutable();
    const QString arch = arArchive_->text().trimmed();
    const QString outd = arOutDir_->text().trimmed();
    if (arch.isEmpty() || outd.isEmpty()) {
        lastLog_->appendPlainText(QStringLiteral("Set archive and output directory."));
        return;
    }
    const QStringList args{QStringLiteral("-e"), QStringLiteral("-o"), outd, arch};
    startCli(QStringLiteral("retdec-ar-extractor"), exe, args);
}

void SignatureStudioPanel::onRunMachoExtractor() {
    const QString exe = resolveRetdecMachoExtractorExecutable();
    const QString f = machoFile_->text().trimmed();
    const QString outd = machoOutDir_->text().trimmed();
    if (f.isEmpty() || outd.isEmpty()) {
        lastLog_->appendPlainText(QStringLiteral("Set Mach-O file and output directory."));
        return;
    }
    const QStringList args{QStringLiteral("--all"), QStringLiteral("-o"), outd, f};
    startCli(QStringLiteral("retdec-macho-extractor"), exe, args);
}

void SignatureStudioPanel::onRunBin2pat() {
    const QString exe = resolveRetdecBin2patExecutable();
    const QString in = bin2patIn_->text().trimmed();
    QString out = bin2patOut_->text().trimmed();
    if (in.isEmpty()) {
        lastLog_->appendPlainText(QStringLiteral("Set bin2pat input file."));
        return;
    }
    if (out.isEmpty()) {
        out = QFileInfo(in).absoluteFilePath() + QStringLiteral(".pat");
        bin2patOut_->setText(out);
    }
    const QStringList args{QStringLiteral("-o"), out, in};
    startCli(QStringLiteral("retdec-bin2pat"), exe, args);
}

void SignatureStudioPanel::onRunPat2yara() {
    const QString exe = resolveRetdecPat2yaraExecutable();
    const QString in = pat2yaraIn_->text().trimmed();
    QString out = pat2yaraOut_->text().trimmed();
    if (in.isEmpty()) {
        lastLog_->appendPlainText(QStringLiteral("Set pat2yara input file."));
        return;
    }
    if (out.isEmpty()) {
        out = QFileInfo(in).absoluteFilePath() + QStringLiteral(".yar");
        pat2yaraOut_->setText(out);
    }
    const QStringList args{QStringLiteral("-o"), out, in};
    startCli(QStringLiteral("retdec-pat2yara"), exe, args);
}

} // namespace panels
} // namespace gui
} // namespace retdec
