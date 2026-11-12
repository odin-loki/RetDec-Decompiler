#include "retdec/gui/panels/target_panel.h"
#include "retdec/gui/cli_tool_paths.h"
#include "retdec/gui/project_file.h"
#include "retdec/gui/settings/settings.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace retdec {
namespace gui {
namespace panels {

namespace {

bool showDecompilerConfigEditor(QWidget* parent, const QString& filePath) {
    QFile inf(filePath);
    if (!inf.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(parent, QStringLiteral("Decompiler config"),
                             QStringLiteral("Cannot open:\n%1").arg(filePath));
        return false;
    }
    const QString initial = QString::fromUtf8(inf.readAll());

    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Edit — %1").arg(QFileInfo(filePath).fileName()));
    dlg.resize(720, 520);
    auto* lay = new QVBoxLayout(&dlg);
    auto* edit = new QPlainTextEdit(&dlg);
    edit->setPlainText(initial);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    lay->addWidget(edit, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(box);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg, [&dlg, edit, filePath, parent] {
        QJsonParseError err{};
        (void)QJsonDocument::fromJson(edit->toPlainText().toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            QMessageBox::warning(
                    parent, QStringLiteral("Invalid JSON"),
                    QStringLiteral("%1 at offset %2").arg(err.errorString()).arg(err.offset));
            return;
        }
        QFile outf(filePath);
        if (!outf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(parent, QStringLiteral("Decompiler config"), outf.errorString());
            return;
        }
        outf.write(edit->toPlainText().toUtf8());
        dlg.accept();
    });
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    return dlg.exec() == QDialog::Accepted;
}

} // namespace

TargetPanel::TargetPanel(QWidget* parent)
    : PanelBase(QStringLiteral("Target"), parent) {
    setupUi();
}

void TargetPanel::setupUi() {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    hint_ = new QLabel(
            QStringLiteral("<i>Overrides live in the <b>.retdec</b> project. "
                           "Non-empty <b>Architecture</b> is passed as <code>-a</code> to "
                           "<code>retdec-decompiler</code>. Optional <b>Decompiler JSON</b> sets "
                           "<code>--config</code> (same as <b>Analysis → Configure…</b>). "
                           "<b>Enter</b> in a target field or <b>Alt+A</b> applies.</i>"),
            this);
    hint_->setWordWrap(true);
    lay->addWidget(hint_);

    binary_ = new QLabel(this);
    binary_->setWordWrap(true);
    binary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(new QLabel(QStringLiteral("<b>Binary</b>"), this));
    lay->addWidget(binary_);

    lay->addWidget(new QLabel(QStringLiteral("Architecture"), this));
    archEdit_ = new QLineEdit(this);
    archEdit_->setPlaceholderText(QStringLiteral("e.g. x86, arm, mips (optional)"));
    lay->addWidget(archEdit_);

    lay->addWidget(new QLabel(QStringLiteral("OS (metadata)"), this));
    osEdit_ = new QLineEdit(this);
    osEdit_->setPlaceholderText(QStringLiteral("e.g. linux, windows — saved to project"));
    lay->addWidget(osEdit_);

    lay->addWidget(new QLabel(QStringLiteral("Entry point"), this));
    entryEdit_ = new QLineEdit(this);
    entryEdit_->setPlaceholderText(QStringLiteral("0x401000 or decimal; empty = 0"));
    lay->addWidget(entryEdit_);

    applyBtn_ = new QPushButton(QStringLiteral("&Apply to project"), this);
    applyBtn_->setToolTip(
            QStringLiteral("Write fields into the in-memory project (mark modified). Alt+A."));
    lay->addWidget(applyBtn_);

    lay->addWidget(new QLabel(QStringLiteral("Decompiler config (--config)"), this));
    decompilerConfigEdit_ = new QLineEdit(this);
    decompilerConfigEdit_->setReadOnly(true);
    decompilerConfigEdit_->setPlaceholderText(
            QStringLiteral("None — decompiler loads built-in default next to binary"));
    lay->addWidget(decompilerConfigEdit_);

    auto* cfgRow = new QHBoxLayout();
    browseCfgBtn_ = new QPushButton(QStringLiteral("&Browse…"), this);
    browseCfgBtn_->setToolTip(QStringLiteral("Pick decompiler JSON. Alt+B."));
    bundledCfgBtn_ = new QPushButton(QStringLiteral("Use b&undled"), this);
    bundledCfgBtn_->setToolTip(
            QStringLiteral("share/retdec/decompiler-config.json next to retdec-decompiler. Alt+U."));
    clearCfgBtn_ = new QPushButton(QStringLiteral("&Clear"), this);
    clearCfgBtn_->setToolTip(QStringLiteral("Remove --config override. Alt+C."));
    editCfgBtn_ = new QPushButton(QStringLiteral("&Edit JSON…"), this);
    editCfgBtn_->setToolTip(QStringLiteral("Edit current JSON when a path is set. Alt+E."));
    cfgRow->addWidget(browseCfgBtn_);
    cfgRow->addWidget(bundledCfgBtn_);
    cfgRow->addWidget(clearCfgBtn_);
    cfgRow->addWidget(editCfgBtn_);
    cfgRow->addStretch(1);
    lay->addLayout(cfgRow);

    lay->addStretch(1);

    connect(applyBtn_, &QPushButton::clicked, this, &TargetPanel::onApplyClicked);
    connect(browseCfgBtn_, &QPushButton::clicked, this, &TargetPanel::onBrowseDecompilerConfig);
    connect(bundledCfgBtn_, &QPushButton::clicked, this, &TargetPanel::onUseBundledDecompilerConfig);
    connect(clearCfgBtn_, &QPushButton::clicked, this, &TargetPanel::onClearDecompilerConfig);
    connect(editCfgBtn_, &QPushButton::clicked, this, &TargetPanel::onEditDecompilerConfig);

    auto applyOnReturn = [this]() {
        if (applyBtn_->isEnabled())
            onApplyClicked();
    };
    connect(archEdit_, &QLineEdit::returnPressed, this, applyOnReturn);
    connect(osEdit_, &QLineEdit::returnPressed, this, applyOnReturn);
    connect(entryEdit_, &QLineEdit::returnPressed, this, applyOnReturn);

    setFieldsEnabled(false);
    syncDecompilerConfigFromAppSettings();
}

void TargetPanel::setFieldsEnabled(bool on) {
    archEdit_->setEnabled(on);
    osEdit_->setEnabled(on);
    entryEdit_->setEnabled(on);
    applyBtn_->setEnabled(on);
    // Decompiler --config is global (AppSettings); keep usable without a loaded project.
    const bool bundledOk = !resolveBundledDecompilerConfigPath().isEmpty();
    decompilerConfigEdit_->setEnabled(true);
    browseCfgBtn_->setEnabled(true);
    bundledCfgBtn_->setEnabled(bundledOk);
    clearCfgBtn_->setEnabled(true);
    updateDecompilerConfigButtons();
}

void TargetPanel::updateDecompilerConfigButtons() {
    const QString p = AppSettings::instance().decompiler.extraConfigPath.trimmed();
    editCfgBtn_->setEnabled(!p.isEmpty() && QFileInfo::exists(p));
}

QString TargetPanel::archText() const {
    return archEdit_->text();
}

QString TargetPanel::osText() const {
    return osEdit_->text();
}

QString TargetPanel::entryText() const {
    return entryEdit_->text();
}

void TargetPanel::onApplyClicked() {
    emit applyRequested();
}

void TargetPanel::syncDecompilerConfigFromAppSettings() {
    const QString p = AppSettings::instance().decompiler.extraConfigPath.trimmed();
    decompilerConfigEdit_->setText(p);
    bundledCfgBtn_->setEnabled(!resolveBundledDecompilerConfigPath().isEmpty());
    updateDecompilerConfigButtons();
}

void TargetPanel::onBrowseDecompilerConfig() {
    QString start = AppSettings::instance().decompiler.extraConfigPath.trimmed();
    if (start.isEmpty()) {
        const QString b = binary_->text();
        if (!b.isEmpty() && b != QStringLiteral("—"))
            start = QFileInfo(b).absolutePath();
    }
    const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Decompiler configuration JSON"), start,
            QStringLiteral("JSON (*.json);;All files (*)"));
    if (path.isEmpty())
        return;
    AppSettings::instance().decompiler.extraConfigPath = QFileInfo(path).absoluteFilePath();
    AppSettings::instance().save();
    AppSettings::instance().notifySettingsChanged();
    syncDecompilerConfigFromAppSettings();
}

void TargetPanel::onClearDecompilerConfig() {
    AppSettings::instance().decompiler.extraConfigPath.clear();
    AppSettings::instance().save();
    AppSettings::instance().notifySettingsChanged();
    syncDecompilerConfigFromAppSettings();
}

void TargetPanel::onUseBundledDecompilerConfig() {
    const QString bundled = resolveBundledDecompilerConfigPath();
    if (bundled.isEmpty()) {
        QMessageBox::information(
                this, QStringLiteral("Bundled config"),
                QStringLiteral("Could not find share/retdec/decompiler-config.json next to "
                             "retdec-decompiler."));
        return;
    }
    AppSettings::instance().decompiler.extraConfigPath = bundled;
    AppSettings::instance().save();
    AppSettings::instance().notifySettingsChanged();
    syncDecompilerConfigFromAppSettings();
}

void TargetPanel::onEditDecompilerConfig() {
    const QString p = AppSettings::instance().decompiler.extraConfigPath.trimmed();
    if (p.isEmpty() || !QFileInfo::exists(p))
        return;
    (void)showDecompilerConfigEditor(this, p);
}

void TargetPanel::setFromProject(const ProjectFile* pf) {
    if (!pf) {
        clear();
        return;
    }
    setFieldsEnabled(true);
    binary_->setText(pf->binaryPath());
    archEdit_->setText(pf->arch());
    osEdit_->setText(pf->os());
    if (pf->entryPoint() != 0)
        entryEdit_->setText(QStringLiteral("0x%1").arg(QString::number(pf->entryPoint(), 16)));
    else
        entryEdit_->clear();
    syncDecompilerConfigFromAppSettings();
}

void TargetPanel::clear() {
    binary_->setText(QStringLiteral("—"));
    archEdit_->clear();
    osEdit_->clear();
    entryEdit_->clear();
    setFieldsEnabled(false);
    syncDecompilerConfigFromAppSettings();
}

} // namespace panels
} // namespace gui
} // namespace retdec
