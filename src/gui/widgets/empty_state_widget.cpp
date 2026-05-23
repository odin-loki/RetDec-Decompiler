#include "retdec/gui/widgets/empty_state_widget.h"

#include <QIcon>
#include <QLabel>
#include <QVBoxLayout>

namespace retdec::gui::widgets {

EmptyStateWidget::EmptyStateWidget(QWidget* parent)
    : QWidget(parent) {
    iconLabel_ = new QLabel(this);
    iconLabel_->setAlignment(Qt::AlignCenter);
    iconLabel_->setProperty("role", "emptyIcon");

    titleLabel_ = new QLabel(this);
    titleLabel_->setAlignment(Qt::AlignCenter);
    titleLabel_->setProperty("role", "emptyTitle");
    titleLabel_->setWordWrap(true);

    hintLabel_ = new QLabel(this);
    hintLabel_->setAlignment(Qt::AlignCenter);
    hintLabel_->setProperty("role", "emptyHint");
    hintLabel_->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(8);
    layout->addStretch(1);
    layout->addWidget(iconLabel_, 0, Qt::AlignHCenter);
    layout->addWidget(titleLabel_, 0, Qt::AlignHCenter);
    layout->addWidget(hintLabel_, 0, Qt::AlignHCenter);
    layout->addStretch(2);
}

void EmptyStateWidget::setTitle(const QString& title) {
    titleLabel_->setText(title);
}

void EmptyStateWidget::setHint(const QString& hint) {
    hintLabel_->setText(hint);
    hintLabel_->setVisible(!hint.isEmpty());
}

void EmptyStateWidget::setIcon(const QIcon& icon) {
    if (icon.isNull()) {
        iconLabel_->clear();
        iconLabel_->setVisible(false);
        return;
    }
    const QPixmap px = icon.pixmap(48, 48);
    iconLabel_->setPixmap(px);
    iconLabel_->setVisible(!px.isNull());
}

} // namespace retdec::gui::widgets
