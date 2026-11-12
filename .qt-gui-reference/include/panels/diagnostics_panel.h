#ifndef RETDEC_GUI_PANELS_DIAGNOSTICS_H
#define RETDEC_GUI_PANELS_DIAGNOSTICS_H

#include "retdec/gui/panels/panel_base.h"
#include <QAbstractTableModel>

QT_BEGIN_NAMESPACE
class QTableView;
class QComboBox;
class QLabel;
class QSortFilterProxyModel;
QT_END_NAMESPACE

namespace retdec::gui::panels {

struct DiagnosticEntry {
    enum class Severity { Muted, Info, Warning, Error };
    Severity severity  = Severity::Info;
    QString  stage;
    QString  message;
    uint64_t address   = 0;
};

class DiagnosticsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit DiagnosticsModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& = {}) const override;
    int columnCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void addEntry(const DiagnosticEntry& entry);
    void clear();
    /// Row index in the source model (not the proxy). Returns 0 if invalid or entry has no address.
    uint64_t addressAtRow(int row) const;

    /// Cap on retained entries (default 10000). Older entries are evicted in FIFO order.
    void setMaxEntries(int n);
    int  maxEntries() const { return maxEntries_; }

private:
    QList<DiagnosticEntry> entries_;
    int maxEntries_ = 10'000;
};

/**
 * @brief DiagnosticsPanel — analysis log and error reporter.
 *
 * Collects informational messages, warnings, and errors emitted by each
 * analysis stage.  Colour-coded severity (green info, yellow warning, red error).
 * Filter combo to show only a specific severity or stage.
 */
class DiagnosticsPanel : public PanelBase {
    Q_OBJECT
public:
    explicit DiagnosticsPanel(QWidget* parent = nullptr);

    void addMessage(DiagnosticEntry::Severity severity,
                    const QString& stage,
                    const QString& message,
                    uint64_t address = 0);
    void clear() override;

signals:
    /// Emitted when an Error-severity message is appended (not warnings/info).
    void errorMessageAdded();

private slots:
    void onFilterChanged(int index);
    void onDoubleClicked(const QModelIndex& index);

private:
    void setupUI();

    QComboBox*            filterCombo_   = nullptr;
    QLabel*               countLabel_    = nullptr;
    QTableView*           tableView_     = nullptr;
    DiagnosticsModel*     model_         = nullptr;
    QSortFilterProxyModel* filterProxy_  = nullptr;
};

} // namespace retdec::gui::panels
#endif
