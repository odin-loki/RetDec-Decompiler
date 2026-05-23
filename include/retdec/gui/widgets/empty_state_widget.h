#ifndef RETDEC_GUI_WIDGETS_EMPTY_STATE_WIDGET_H
#define RETDEC_GUI_WIDGETS_EMPTY_STATE_WIDGET_H

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QIcon;
QT_END_NAMESPACE

namespace retdec::gui::widgets {

/**
 * @brief Centered icon + title + hint for panels with no content yet.
 */
class EmptyStateWidget : public QWidget {
    Q_OBJECT

public:
    explicit EmptyStateWidget(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setHint(const QString& hint);
    void setIcon(const QIcon& icon);

private:
    QLabel* iconLabel_  = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* hintLabel_  = nullptr;
};

} // namespace retdec::gui::widgets

#endif // RETDEC_GUI_WIDGETS_EMPTY_STATE_WIDGET_H
