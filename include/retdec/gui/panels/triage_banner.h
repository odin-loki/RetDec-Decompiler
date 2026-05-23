/**
 * @file include/retdec/gui/panels/triage_banner.h
 * @brief Dismissible header strip above the document tabs.
 *
 * Shows at-a-glance metadata for the currently-opened binary (format,
 * architecture, OS, size) plus a packed-content warning when fileinfo
 * detected something, and surfaces the two most common next actions
 * ("Unpack first", "Decompile anyway") inline so the user does not have
 * to hunt for them in menus.
 *
 * The banner is small (~44 px) and lives inside the central widget,
 * above the document QTabWidget.  It only shows when there is an
 * active binary; clicking the close button hides it for the rest of
 * the session until a new binary is opened.
 */

#ifndef RETDEC_GUI_PANELS_TRIAGE_BANNER_H
#define RETDEC_GUI_PANELS_TRIAGE_BANNER_H

#include <QFrame>
#include <QString>

QT_BEGIN_NAMESPACE
class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;
QT_END_NAMESPACE

namespace retdec {
namespace gui {
namespace panels {

class TriageBanner : public QFrame {
    Q_OBJECT
public:
    explicit TriageBanner(QWidget* parent = nullptr);

    /// Populate from a binary path; clears badges, refreshes label.
    /// Pass an empty path to hide the banner (e.g. when a project is closed).
    void setBinary(const QString& path);

    /// Set the file format / architecture / OS / size badges. Empty values are
    /// rendered as "—". @p packerName, if non-empty, shows the orange "packed"
    /// badge with that text (e.g. "UPX").
    void setMetadata(const QString& format, const QString& arch,
                     const QString& os, qint64 sizeBytes,
                     const QString& packerName = {});

    /// Toggle action-button enablement (e.g. greyed out while a child runs).
    void setActionsEnabled(bool on);

signals:
    /// User clicked "Unpack first".
    void unpackRequested();
    /// User clicked "Decompile anyway".
    void decompileRequested();
    /// User clicked "More ▼" — host should open a popup menu.
    void moreActionsRequested();
    /// User clicked "✕" — the host should hide the widget.
    void dismissed();
    /// User clicked the architecture or OS badge — host should open Target panel.
    void targetDetailsRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    QLabel*       title_       = nullptr;
    QLabel*       formatBadge_ = nullptr;
    QLabel*       archBadge_   = nullptr;
    QLabel*       osBadge_     = nullptr;
    QLabel*       sizeBadge_   = nullptr;
    QLabel*       packerBadge_ = nullptr;
    QPushButton*  unpackBtn_   = nullptr;
    QPushButton*  decompileBtn_ = nullptr;
    QPushButton*  moreBtn_     = nullptr;
    QToolButton*  closeBtn_    = nullptr;
};

} // namespace panels
} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_PANELS_TRIAGE_BANNER_H
