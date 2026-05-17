/**
 * @file src/gui/panels/triage_banner.cpp
 */

#include "retdec/gui/panels/triage_banner.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>

namespace retdec {
namespace gui {
namespace panels {

namespace {

QString makeBadgeQss(const QString& bgHex, const QString& fgHex,
                     const QString& borderHex) {
    return QStringLiteral(
            "QLabel { background-color: %1; color: %2; border: 1px solid %3; "
            "border-radius: 4px; padding: 2px 8px; font-size: 11px; "
            "font-weight: 500; }")
            .arg(bgHex, fgHex, borderHex);
}

QString humanSize(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("—");
    const double kib = 1024.0;
    if (bytes < kib)         return QStringLiteral("%1 B").arg(bytes);
    if (bytes < kib * kib)   return QStringLiteral("%1 KiB").arg(bytes / kib, 0, 'f', 1);
    if (bytes < kib*kib*kib) return QStringLiteral("%1 MiB").arg(bytes / (kib*kib), 0, 'f', 1);
    return QStringLiteral("%1 GiB").arg(bytes / (kib*kib*kib), 0, 'f', 2);
}

} // namespace

TriageBanner::TriageBanner(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("triageBanner"));
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral(
            "#triageBanner { background-color: #313244; border: 1px solid #45475a; "
            "border-radius: 6px; }"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(44);
    setupUi();
    setBinary({}); // start hidden
}

void TriageBanner::setupUi() {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 6, 6, 6);
    lay->setSpacing(8);

    title_ = new QLabel(this);
    title_->setStyleSheet(QStringLiteral("color: #cdd6f4; font-weight: 600;"));
    title_->setToolTip(QStringLiteral("Currently loaded binary"));

    formatBadge_ = new QLabel(this);
    archBadge_   = new QLabel(this);
    osBadge_     = new QLabel(this);
    sizeBadge_   = new QLabel(this);
    const QString neutral = makeBadgeQss(QStringLiteral("#45475a"),
                                         QStringLiteral("#cdd6f4"),
                                         QStringLiteral("#585b70"));
    formatBadge_->setStyleSheet(neutral);
    archBadge_->setStyleSheet(neutral);
    osBadge_->setStyleSheet(neutral);
    sizeBadge_->setStyleSheet(neutral);
    formatBadge_->setToolTip(QStringLiteral("Container format reported by retdec-fileinfo"));
    archBadge_->setToolTip(QStringLiteral("Target architecture"));
    osBadge_->setToolTip(QStringLiteral("Target operating system"));
    sizeBadge_->setToolTip(QStringLiteral("File size on disk"));

    packerBadge_ = new QLabel(this);
    packerBadge_->setStyleSheet(makeBadgeQss(QStringLiteral("#4d3a1f"),
                                             QStringLiteral("#f9e2af"),
                                             QStringLiteral("#fab387")));
    packerBadge_->setToolTip(QStringLiteral(
            "fileinfo detected a packer signature. Use 'Unpack first' or unpack "
            "manually before decompiling for best results."));
    packerBadge_->hide();

    unpackBtn_    = new QPushButton(QStringLiteral("Unpack first"), this);
    decompileBtn_ = new QPushButton(QStringLiteral("Decompile anyway"), this);
    moreBtn_      = new QPushButton(QStringLiteral("More ▼"), this);
    unpackBtn_->setToolTip(QStringLiteral(
            "Run retdec-unpacker on the current binary and reopen the result."));
    decompileBtn_->setToolTip(QStringLiteral(
            "Run retdec-decompiler on the current binary as-is (Ctrl+R / F5)."));
    moreBtn_->setToolTip(QStringLiteral(
            "Show secondary triage tools (re-run fileinfo, copy hash, view raw output…)."));

    closeBtn_ = new QToolButton(this);
    closeBtn_->setText(QStringLiteral("✕"));
    closeBtn_->setAutoRaise(true);
    closeBtn_->setToolTip(QStringLiteral("Hide this banner for the current binary."));
    closeBtn_->setStyleSheet(QStringLiteral(
            "QToolButton { color: #6c7086; font-weight: bold; padding: 2px 6px; }"
            "QToolButton:hover { color: #f38ba8; }"));

    lay->addWidget(title_);
    lay->addSpacing(8);
    lay->addWidget(formatBadge_);
    lay->addWidget(archBadge_);
    lay->addWidget(osBadge_);
    lay->addWidget(sizeBadge_);
    lay->addWidget(packerBadge_);
    lay->addStretch(1);
    lay->addWidget(unpackBtn_);
    lay->addWidget(decompileBtn_);
    lay->addWidget(moreBtn_);
    lay->addWidget(closeBtn_);

    connect(unpackBtn_,    &QPushButton::clicked, this, &TriageBanner::unpackRequested);
    connect(decompileBtn_, &QPushButton::clicked, this, &TriageBanner::decompileRequested);
    connect(moreBtn_,      &QPushButton::clicked, this, &TriageBanner::moreActionsRequested);
    connect(closeBtn_,     &QToolButton::clicked, this, &TriageBanner::dismissed);
}

void TriageBanner::setBinary(const QString& path) {
    if (path.isEmpty()) {
        hide();
        title_->clear();
        formatBadge_->setText(QStringLiteral("—"));
        archBadge_->setText(QStringLiteral("—"));
        osBadge_->setText(QStringLiteral("—"));
        sizeBadge_->setText(QStringLiteral("—"));
        packerBadge_->hide();
        return;
    }
    const QFileInfo fi(path);
    title_->setText(fi.fileName());
    title_->setToolTip(fi.absoluteFilePath());
    sizeBadge_->setText(humanSize(fi.size()));
    // Format/arch/OS start as "—" and are updated by setMetadata() once
    // fileinfo finishes.
    formatBadge_->setText(QStringLiteral("…"));
    archBadge_->setText(QStringLiteral("…"));
    osBadge_->setText(QStringLiteral("…"));
    packerBadge_->hide();
    show();
}

void TriageBanner::setMetadata(const QString& format, const QString& arch,
                               const QString& os, qint64 sizeBytes,
                               const QString& packerName) {
    formatBadge_->setText(format.isEmpty() ? QStringLiteral("—") : format);
    archBadge_->setText(arch.isEmpty()     ? QStringLiteral("—") : arch);
    osBadge_->setText(os.isEmpty()         ? QStringLiteral("—") : os);
    if (sizeBytes > 0)
        sizeBadge_->setText(humanSize(sizeBytes));
    if (!packerName.isEmpty()) {
        packerBadge_->setText(QStringLiteral("⚠ %1-packed").arg(packerName));
        packerBadge_->show();
    } else {
        packerBadge_->hide();
    }
}

void TriageBanner::setActionsEnabled(bool on) {
    unpackBtn_->setEnabled(on);
    decompileBtn_->setEnabled(on);
    moreBtn_->setEnabled(on);
}

} // namespace panels
} // namespace gui
} // namespace retdec
