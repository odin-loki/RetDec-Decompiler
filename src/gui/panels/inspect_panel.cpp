/**
 * @file inspect_panel.cpp
 */

#include "retdec/gui/panels/inspect_panel.h"
#include "retdec/gui/cli_tool_paths.h"

#include <algorithm>

#include <QCheckBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace retdec {
namespace gui {
namespace panels {

namespace {

QString summaryHtmlFromJson(const QJsonObject& root) {
    QString html = QStringLiteral("<html><body><table cellspacing=\"6\">");
    QStringList keys;
    for (auto it = root.begin(); it != root.end(); ++it)
        keys.append(it.key());
    std::sort(keys.begin(), keys.end());
    int rows = 0;
    constexpr int kMaxRows = 48;
    for (const QString& k : keys) {
        if (rows >= kMaxRows)
            break;
        const QJsonValue v = root.value(k);
        if (v.isObject() || v.isArray())
            continue;
        QString val;
        if (v.isString())
            val = v.toString().toHtmlEscaped();
        else if (v.isDouble())
            val = QString::number(v.toDouble());
        else if (v.isBool())
            val = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (v.isNull())
            val = QStringLiteral("null");
        else
            continue;
        html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                        .arg(k.toHtmlEscaped(), val);
        ++rows;
    }
    html += QStringLiteral("</table>");
    if (rows == 0)
        html += QStringLiteral("<p><i>No flat key/value fields at root; see JSON tab.</i></p>");
    html += QStringLiteral("</body></html>");
    return html;
}

} // namespace

InspectPanel::InspectPanel(QWidget* parent)
    : PanelBase(QStringLiteral("Inspect"), parent) {
    setupUi();
    fileinfoProc_ = new QProcess(this);
    connect(fileinfoProc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &InspectPanel::onFileinfoFinished);
    connect(fileinfoProc_, &QProcess::errorOccurred, this, &InspectPanel::onFileinfoError);

    unpackProc_ = new QProcess(this);
    connect(unpackProc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &InspectPanel::onUnpackFinished);
    connect(unpackProc_, &QProcess::errorOccurred, this, &InspectPanel::onUnpackError);
}

void InspectPanel::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);

    auto* row = new QHBoxLayout();
    status_ = new QLabel(QStringLiteral("No fileinfo run yet."), this);
    status_->setWordWrap(true);
    refreshBtn_ = new QPushButton(QStringLiteral("Refresh fileinfo"), this);
    decompileModeBtn_ = new QPushButton(QStringLiteral("Decompile mode"), this);
    decompileModeBtn_->setToolTip(
            QStringLiteral("Switch workspace to Decompile (centre code tabs)."));
    row->addWidget(status_, 1);
    row->addWidget(refreshBtn_);
    row->addWidget(decompileModeBtn_);
    root->addLayout(row);

    auto* unpackRow = new QHBoxLayout();
    unpackRow->addWidget(new QLabel(QStringLiteral("Unpack output:"), this));
    unpackOutEdit_ = new QLineEdit(this);
    unpackOutEdit_->setPlaceholderText(QStringLiteral("Defaults to &lt;binary&gt;-unpacked"));
    unpackBrute_ = new QCheckBox(QStringLiteral("Brute"), this);
    unpackBrute_->setToolTip(QStringLiteral("Pass -b|--brute to retdec-unpacker."));
    unpackBtn_ = new QPushButton(QStringLiteral("Run unpacker"), this);
    openUnpackedBtn_ = new QPushButton(QStringLiteral("Open unpacked"), this);
    openUnpackedBtn_->setToolTip(QStringLiteral("Load the unpack output path as the current binary."));
    openUnpackedBtn_->setEnabled(false);
    unpackRow->addWidget(unpackOutEdit_, 1);
    unpackRow->addWidget(unpackBrute_);
    unpackRow->addWidget(unpackBtn_);
    unpackRow->addWidget(openUnpackedBtn_);
    root->addLayout(unpackRow);

    tabs_ = new QTabWidget(this);
    summary_ = new QTextBrowser(this);
    summary_->setOpenExternalLinks(false);
    raw_ = new QPlainTextEdit(this);
    raw_->setReadOnly(true);
    raw_->setUndoRedoEnabled(false);
    // Large binaries can produce 100s of KB of fileinfo JSON. Cap the
    // visible scrollback so loading + scrolling remain instant.
    raw_->setMaximumBlockCount(20'000);
    raw_->setLineWrapMode(QPlainTextEdit::NoWrap);
    raw_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    raw_->setPlaceholderText(QStringLiteral("JSON output from retdec-fileinfo --json"));
    tabs_->addTab(summary_, QStringLiteral("Summary"));
    tabs_->addTab(raw_, QStringLiteral("JSON"));
    root->addWidget(tabs_, 1);

    connect(refreshBtn_, &QPushButton::clicked, this, &InspectPanel::onRefreshClicked);
    connect(decompileModeBtn_, &QPushButton::clicked, this, &InspectPanel::onDecompileShortcutClicked);
    connect(unpackBtn_, &QPushButton::clicked, this, &InspectPanel::onUnpackClicked);
    connect(openUnpackedBtn_, &QPushButton::clicked, this, &InspectPanel::onOpenUnpackedClicked);
    connect(unpackOutEdit_, &QLineEdit::textChanged, this, [this] { updateButtonStates(); });
}

void InspectPanel::updateUnpackOutputDefault() {
    if (lastBinaryPath_.isEmpty()) {
        unpackOutEdit_->clear();
        return;
    }
    QString base = lastBinaryPath_;
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        base = base.left(dot);
    unpackOutEdit_->setText(base + QStringLiteral("-unpacked"));
}

void InspectPanel::updateButtonStates() {
    const bool fiBusy = fileinfoProc_->state() != QProcess::NotRunning;
    const bool unBusy = unpackProc_->state() != QProcess::NotRunning;
    refreshBtn_->setEnabled(!fiBusy && !unBusy);
    unpackBtn_->setEnabled(!lastBinaryPath_.isEmpty() && !fiBusy && !unBusy);
    const QString out = unpackOutEdit_->text().trimmed();
    openUnpackedBtn_->setEnabled(!out.isEmpty() && QFileInfo::exists(out) && !fiBusy && !unBusy);
}

void InspectPanel::setFileinfoRunning(bool on) {
    if (on) {
        status_->setText(QStringLiteral("Running retdec-fileinfo…"));
        summary_->setHtml(QStringLiteral("<i>Running…</i>"));
        raw_->clear();
    }
    updateButtonStates();
}

void InspectPanel::setUnpackRunning(bool on) {
    if (on) {
        status_->setText(QStringLiteral("Running retdec-unpacker…"));
        openUnpackedBtn_->setEnabled(false);
    }
    unpackBtn_->setEnabled(!on && !lastBinaryPath_.isEmpty());
    refreshBtn_->setEnabled(!on && fileinfoProc_->state() == QProcess::NotRunning);
    unpackOutEdit_->setReadOnly(on);
    unpackBrute_->setEnabled(!on);
}

void InspectPanel::runFileinfo(const QString& binaryPath, const QString& fileinfoExecutable) {
    lastBinaryPath_ = binaryPath;
    fileinfoExe_ = fileinfoExecutable;
    updateUnpackOutputDefault();
    if (binaryPath.isEmpty()) {
        updateButtonStates();
        return;
    }

    if (fileinfoProc_->state() != QProcess::NotRunning) {
        fileinfoProc_->kill();
        fileinfoProc_->waitForFinished(3000);
    }

    if (fileinfoExecutable.isEmpty()) {
        status_->setText(QStringLiteral("retdec-fileinfo not found (install or add to PATH)."));
        summary_->setHtml(QStringLiteral("<p style=\"color:#c00\">No fileinfo executable.</p>"));
        raw_->clear();
        updateButtonStates();
        return;
    }

    setFileinfoRunning(true);
    fileinfoTimer_.start();
    tabs_->setCurrentIndex(0);

    QStringList args;
    args << QStringLiteral("--json") << binaryPath;
    fileinfoProc_->setProgram(fileinfoExecutable);
    fileinfoProc_->setArguments(args);
    fileinfoProc_->setWorkingDirectory(QFileInfo(fileinfoExecutable).absolutePath());
    // Separate channels so the live console can colour stderr differently.
    // (JSON parsing still uses stdout only.)
    fileinfoProc_->setProcessChannelMode(QProcess::SeparateChannels);
    emit processStarting(fileinfoProc_, QStringLiteral("retdec-fileinfo"));
    fileinfoProc_->start();
    updateButtonStates();
}

void InspectPanel::clear() {
    if (fileinfoProc_->state() != QProcess::NotRunning) {
        fileinfoProc_->kill();
        fileinfoProc_->waitForFinished(2000);
    }
    if (unpackProc_->state() != QProcess::NotRunning) {
        unpackProc_->kill();
        unpackProc_->waitForFinished(2000);
    }
    lastBinaryPath_.clear();
    status_->setText(QStringLiteral("No binary loaded."));
    summary_->clear();
    raw_->clear();
    unpackOutEdit_->clear();
    openUnpackedBtn_->setEnabled(false);
    updateButtonStates();
}

void InspectPanel::onRefreshClicked() {
    if (!lastBinaryPath_.isEmpty())
        runFileinfo(lastBinaryPath_, fileinfoExe_);
}

void InspectPanel::onDecompileShortcutClicked() {
    emit requestDecompileMode();
}

void InspectPanel::onUnpackClicked() {
    if (lastBinaryPath_.isEmpty())
        return;
    const QString dec = resolveRetdecUnpackerExecutable();
    if (dec.isEmpty()) {
        status_->setText(QStringLiteral("retdec-unpacker not found."));
        return;
    }
    if (unpackProc_->state() != QProcess::NotRunning)
        return;

    // Bug fix #6: get the trimmed text once, fall back to default if empty.
    QString outPath = unpackOutEdit_->text().trimmed();
    if (outPath.isEmpty()) {
        updateUnpackOutputDefault();
        outPath = unpackOutEdit_->text().trimmed();
    }
    if (outPath.isEmpty()) {
        status_->setText(QStringLiteral("Set an unpack output path."));
        return;
    }

    QStringList args;
    if (unpackBrute_->isChecked())
        args << QStringLiteral("-b");
    args << lastBinaryPath_ << QStringLiteral("-o") << outPath;

    setUnpackRunning(true);
    unpackTimer_.start();
    unpackProc_->setProgram(dec);
    unpackProc_->setArguments(args);
    unpackProc_->setWorkingDirectory(QFileInfo(dec).absolutePath());
    unpackProc_->setProcessChannelMode(QProcess::SeparateChannels);
    emit processStarting(unpackProc_, QStringLiteral("retdec-unpacker"));
    unpackProc_->start();
}

void InspectPanel::onUnpackFinished(int exitCode, QProcess::ExitStatus) {
    setUnpackRunning(false);
    const QByteArray combined = unpackProc_->readAllStandardOutput()
                              + unpackProc_->readAllStandardError();
    QString text = QString::fromUtf8(combined);
    if (text.size() > 4000)
        text = text.left(4000) + QStringLiteral("\n…");

    QStringList args;
    if (unpackBrute_->isChecked())
        args << QStringLiteral("-b");
    args << lastBinaryPath_ << QStringLiteral("-o") << unpackOutEdit_->text().trimmed();

    emit cliToolFinished(QStringLiteral("retdec-unpacker"), args,
                          QFileInfo(unpackProc_->program()).absolutePath(), exitCode,
                          unpackTimer_.elapsed(), text);

    if (exitCode == 0) {
        status_->setText(QStringLiteral("Unpacker finished (exit 0). Output: %1")
                                 .arg(unpackOutEdit_->text().trimmed()));
        const QString outp = unpackOutEdit_->text().trimmed();
        openUnpackedBtn_->setEnabled(!outp.isEmpty() && QFileInfo::exists(outp));
    } else {
        status_->setText(QStringLiteral("Unpacker exit %1 — see Command log / Diagnostics.")
                                 .arg(exitCode));
        openUnpackedBtn_->setEnabled(false);
    }
    updateButtonStates();
}

void InspectPanel::onOpenUnpackedClicked() {
    const QString p = unpackOutEdit_->text().trimmed();
    if (!p.isEmpty() && QFileInfo::exists(p))
        emit requestOpenBinary(QFileInfo(p).absoluteFilePath());
}

void InspectPanel::onUnpackError(QProcess::ProcessError err) {
    Q_UNUSED(err);
    setUnpackRunning(false);
    QStringList args;
    if (unpackBrute_->isChecked())
        args << QStringLiteral("-b");
    args << lastBinaryPath_ << QStringLiteral("-o") << unpackOutEdit_->text().trimmed();
    emit cliToolFinished(QStringLiteral("retdec-unpacker"), args,
                          QFileInfo(unpackProc_->program()).absolutePath(), -1, unpackTimer_.elapsed(),
                          unpackProc_->errorString());
    status_->setText(QStringLiteral("Unpacker error: %1").arg(unpackProc_->errorString()));
    openUnpackedBtn_->setEnabled(false);
    updateButtonStates();
}

void InspectPanel::onFileinfoFinished(int exitCode, QProcess::ExitStatus) {
    setFileinfoRunning(false);
    const QByteArray out = fileinfoProc_->readAllStandardOutput();
    const QString text = QString::fromUtf8(out);
    const qint64 elapsed = fileinfoTimer_.elapsed();

    QString tail = text;
    if (tail.size() > 4000)
        tail = tail.left(4000) + QStringLiteral("\n…");

    emit cliToolFinished(QStringLiteral("retdec-fileinfo"),
                         QStringList{QStringLiteral("--json"), lastBinaryPath_},
                         QFileInfo(fileinfoExe_).absolutePath(), exitCode, elapsed, tail);

    if (exitCode != 0) {
        status_->setText(QStringLiteral("fileinfo failed (exit %1)").arg(exitCode));
        summary_->setHtml(QStringLiteral("<p style=\"color:#c00\">Non-zero exit. See JSON tab.</p>"));
        if (out.isEmpty())
            raw_->setPlainText(QStringLiteral("(no output)"));
        else
            raw_->setPlainText(QString::fromUtf8(
                    out.size() > 256 * 1024 ? out.left(256 * 1024) : out));
        updateButtonStates();
        return;
    }

    status_->setText(QStringLiteral("fileinfo OK — see Summary / JSON tabs."));
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(out, &pe);

    // Performance: very large binaries can produce multi-megabyte fileinfo
    // JSON. Pretty-printing + dumping it all into a QPlainTextEdit blocks
    // the GUI thread for hundreds of ms. Cap the raw view to a sane size
    // (the full JSON is still available on disk via Command log / Console).
    constexpr int kMaxRawBytes = 512 * 1024;
    auto setRawCapped = [this](const QByteArray& payload) {
        if (payload.size() <= kMaxRawBytes) {
            raw_->setPlainText(QString::fromUtf8(payload));
        } else {
            QString preview = QString::fromUtf8(payload.left(kMaxRawBytes));
            preview += QStringLiteral(
                    "\n\n…[truncated — %1 KiB total, showing first %2 KiB]")
                    .arg(payload.size() / 1024).arg(kMaxRawBytes / 1024);
            raw_->setPlainText(preview);
        }
    };

    if (doc.isObject()) {
        summary_->setHtml(summaryHtmlFromJson(doc.object()));
        // Pretty-print only if the payload is modest; otherwise show the raw
        // bytes (which fileinfo already formatted reasonably).
        if (out.size() <= kMaxRawBytes) {
            setRawCapped(QJsonDocument(doc.object()).toJson(QJsonDocument::Indented));
        } else {
            setRawCapped(out);
        }
    } else {
        summary_->setHtml(
                QStringLiteral("<p>Could not parse JSON (%1). Raw output in JSON tab.</p>")
                        .arg(pe.errorString().toHtmlEscaped()));
        setRawCapped(out);
    }
    updateButtonStates();
}

void InspectPanel::onFileinfoError(QProcess::ProcessError err) {
    Q_UNUSED(err);
    setFileinfoRunning(false);
    status_->setText(QStringLiteral("fileinfo error: %1").arg(fileinfoProc_->errorString()));
    summary_->setHtml(QStringLiteral("<p style=\"color:#c00\">Process error.</p>"));
    emit cliToolFinished(QStringLiteral("retdec-fileinfo"),
                         QStringList{QStringLiteral("--json"), lastBinaryPath_},
                         QFileInfo(fileinfoExe_).absolutePath(), -1, fileinfoTimer_.elapsed(),
                         fileinfoProc_->errorString());
    updateButtonStates();
}

} // namespace panels
} // namespace gui
} // namespace retdec
