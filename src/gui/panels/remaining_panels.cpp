/**
 * @file src/gui/panels/remaining_panels.cpp
 * @brief Remaining panel implementations.
 *
 * StringsBrowserPanel → strings_browser_panel.cpp (Task 55)
 * AIAssistantPanel    → ai_assistant_panel.cpp     (Task 54)
 * TypeHierarchyPanel  → type_hierarchy_panel.cpp   (Task 52)
 * CallGraphPanel      → call_graph_panel.cpp        (Task 52)
 * ProgressPanel       → progress_panel.cpp          (Task 49)
 * TriPaneCodeView     → tri_pane_code_view.cpp      (Task 50)
 */

// AIAssistantPanel is implemented in ai_assistant_panel.cpp (Task 54)
// StringsBrowserPanel is implemented in strings_browser_panel.cpp (Task 55)

// ── DiagnosticsPanel ──────────────────────────────────────────────────────────
#include "retdec/gui/panels/diagnostics_panel.h"
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

namespace retdec::gui::panels {

namespace {

QString severityLabel(DiagnosticEntry::Severity severity)
{
    switch (severity) {
    case DiagnosticEntry::Severity::Info:    return QStringLiteral("Info");
    case DiagnosticEntry::Severity::Warning: return QStringLiteral("Warning");
    case DiagnosticEntry::Severity::Error:   return QStringLiteral("Error");
    }
    return {};
}

QString severityIcon(DiagnosticEntry::Severity severity)
{
    switch (severity) {
    case DiagnosticEntry::Severity::Info:    return QStringLiteral("\u2139");  // ℹ
    case DiagnosticEntry::Severity::Warning: return QStringLiteral("\u26A0");  // ⚠
    case DiagnosticEntry::Severity::Error:   return QStringLiteral("\u2716"); // ✖
    }
    return {};
}

} // namespace

DiagnosticsModel::DiagnosticsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int DiagnosticsModel::rowCount(const QModelIndex&) const {
    return entries_.size();
}
int DiagnosticsModel::columnCount(const QModelIndex&) const { return 4; }

QVariant DiagnosticsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= entries_.size()) return {};
    const auto& e = entries_[index.row()];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return severityLabel(e.severity);
        case 1: return e.stage;
        case 2: return e.message;
        case 3: return e.address ? QString("0x%1").arg(e.address, 0, 16) : "";
        }
    }
    if (role == Qt::DecorationRole && index.column() == 0)
        return severityIcon(e.severity);
    if (role == Qt::ForegroundRole) {
        switch (e.severity) {
        case DiagnosticEntry::Severity::Info:    return QColor("#a6e3a1");
        case DiagnosticEntry::Severity::Warning: return QColor("#f9e2af");
        case DiagnosticEntry::Severity::Error:   return QColor("#f38ba8");
        }
    }
    return {};
}

QVariant DiagnosticsModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return "Severity";
    case 1: return "Stage";
    case 2: return "Message";
    case 3: return "Address";
    }
    return {};
}

void DiagnosticsModel::addEntry(const DiagnosticEntry& entry) {
    // Evict oldest entry first if we're at the cap, to keep memory bounded
    // during long sessions (a very chatty decompiler with --print-after-all
    // can otherwise emit tens of thousands of info lines).
    if (maxEntries_ > 0 && entries_.size() >= maxEntries_) {
        beginRemoveRows({}, 0, 0);
        entries_.removeFirst();
        endRemoveRows();
    }
    beginInsertRows({}, entries_.size(), entries_.size());
    entries_.append(entry);
    endInsertRows();
}

void DiagnosticsModel::clear() {
    beginResetModel();
    entries_.clear();
    endResetModel();
}

void DiagnosticsModel::setMaxEntries(int n) {
    maxEntries_ = qMax(100, n);
    if (entries_.size() > maxEntries_) {
        beginResetModel();
        while (entries_.size() > maxEntries_)
            entries_.removeFirst();
        endResetModel();
    }
}

uint64_t DiagnosticsModel::addressAtRow(int row) const {
    if (row < 0 || row >= entries_.size())
        return 0;
    return entries_.at(row).address;
}

DiagnosticsPanel::DiagnosticsPanel(QWidget* parent)
    : PanelBase("Diagnostics", parent) {
    setupUI();
}

void DiagnosticsPanel::setupUI() {
    auto* topBar    = new QWidget(this);
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(4, 2, 4, 2);
    filterCombo_ = new QComboBox(topBar);
    filterCombo_->addItems({"All", "Info", "Warnings", "Errors"});
    countLabel_ = new QLabel("0 entries", topBar);
    countLabel_->setProperty("role", "muted");
    topLayout->addWidget(new QLabel("Show:", topBar));
    topLayout->addWidget(filterCombo_);
    topLayout->addStretch();
    topLayout->addWidget(countLabel_);

    model_        = new DiagnosticsModel(this);
    filterProxy_  = new QSortFilterProxyModel(this);
    filterProxy_->setSourceModel(model_);
    filterProxy_->setFilterKeyColumn(0);
    filterProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    tableView_ = new QTableView(this);
    tableView_->setModel(filterProxy_);
    tableView_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tableView_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    tableView_->setAlternatingRowColors(true);
    tableView_->verticalHeader()->hide();
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(topBar);
    layout->addWidget(tableView_);

    connect(filterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DiagnosticsPanel::onFilterChanged);
    connect(tableView_, &QTableView::doubleClicked,
            this, &DiagnosticsPanel::onDoubleClicked);
    onFilterChanged(filterCombo_->currentIndex());
}

void DiagnosticsPanel::addMessage(DiagnosticEntry::Severity severity,
                                   const QString& stage,
                                   const QString& message,
                                   uint64_t address) {
    DiagnosticEntry e;
    e.severity = severity;
    e.stage    = stage;
    e.message  = message;
    e.address  = address;
    model_->addEntry(e);
    countLabel_->setText(QString("%1 entries").arg(filterProxy_->rowCount()));
    if (severity == DiagnosticEntry::Severity::Error)
        emit errorMessageAdded();
}

void DiagnosticsPanel::clear() {
    model_->clear();
    countLabel_->setText(QStringLiteral("0 entries"));
}

void DiagnosticsPanel::onFilterChanged(int index) {
    if (!filterProxy_)
        return;
    switch (index) {
    case 1:
        filterProxy_->setFilterRegularExpression(
                QRegularExpression(QStringLiteral("^Info$")));
        break;
    case 2:
        filterProxy_->setFilterRegularExpression(
                QRegularExpression(QStringLiteral("^Warning$")));
        break;
    case 3:
        filterProxy_->setFilterRegularExpression(
                QRegularExpression(QStringLiteral("^Error$")));
        break;
    default:
        filterProxy_->setFilterRegularExpression(QRegularExpression());
        break;
    }
    countLabel_->setText(QStringLiteral("%1 entries").arg(filterProxy_->rowCount()));
}

void DiagnosticsPanel::onDoubleClicked(const QModelIndex& index) {
    if (!index.isValid() || !filterProxy_ || !model_)
        return;
    const QModelIndex src = filterProxy_->mapToSource(index);
    if (!src.isValid())
        return;
    const uint64_t addr = model_->addressAtRow(src.row());
    if (addr != 0)
        emit addressNavigated(addr);
}

} // namespace retdec::gui::panels

// ProgressPanel is implemented in progress_panel.cpp (Task 49).
