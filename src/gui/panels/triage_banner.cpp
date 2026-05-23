/**
 * @file src/gui/panels/triage_banner.cpp
 */

#include "retdec/gui/panels/triage_banner.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>

namespace retdec {
namespace gui {
namespace panels {

namespace {

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
    title_->setProperty("role", "title");
    title_->setToolTip(QStringLiteral("Currently loaded binary"));

    formatBadge_ = new QLabel(this);
    archBadge_   = new QLabel(this);
    osBadge_     = new QLabel(this);
    sizeBadge_   = new QLabel(this);
    for (QLabel* badge : {formatBadge_, archBadge_, osBadge_, sizeBadge_})
        badge->setProperty("role", "badge");
    formatBadge_->setToolTip(QStringLiteral("Container format reported by retdec-fileinfo"));
    archBadge_->setToolTip(QStringLiteral(
            "Target architecture — click to open the Target panel"));
    osBadge_->setToolTip(QStringLiteral(
            "Target operating system — click to open the Target panel"));
    sizeBadge_->setToolTip(QStringLiteral("File size on disk"));
    archBadge_->setCursor(Qt::PointingHandCursor);
    osBadge_->setCursor(Qt::PointingHandCursor);
    archBadge_->installEventFilter(this);
    osBadge_->installEventFilter(this);

    packerBadge_ = new QLabel(this);
    packerBadge_->setProperty("role", "badge");
    packerBadge_->setProperty("variant", "warning");
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
    closeBtn_->setObjectName(QStringLiteral("triageCloseBtn"));
    closeBtn_->setText(QStringLiteral("✕"));
    closeBtn_->setAutoRaise(true);
    closeBtn_->setToolTip(QStringLiteral("Hide this banner for the current binary."));

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

bool TriageBanner::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == archBadge_ || watched == osBadge_) &&
        event->type() == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            emit targetDetailsRequested();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace panels
} // namespace gui
} // namespace retdec
