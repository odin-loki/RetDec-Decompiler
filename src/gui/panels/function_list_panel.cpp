/**
 * @file src/gui/panels/function_list_panel.cpp
 * @brief Function list panel with recovery metadata — full implementation (Task 53).
 */

#include "retdec/gui/panels/function_list_panel.h"

#include "retdec/gui/widgets/empty_state_widget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace retdec::gui::panels {

// ─── Catppuccin Mocha palette ─────────────────────────────────────────────────
namespace clrFL {
static const QColor base     {0x1e, 0x1e, 0x2e};
static const QColor surface0 {0x31, 0x32, 0x44};
static const QColor surface1 {0x45, 0x47, 0x5a};
static const QColor overlay0 {0x6c, 0x70, 0x86};
static const QColor text     {0xcd, 0xd6, 0xf4};
static const QColor subtext0 {0xa6, 0xad, 0xc8};
static const QColor red      {0xf3, 0x8b, 0xa8};
static const QColor green    {0xa6, 0xe3, 0xa1};
static const QColor yellow   {0xf9, 0xe2, 0xaf};
static const QColor blue     {0x89, 0xb4, 0xfa};
static const QColor mauve    {0xcb, 0xa6, 0xf7};
static const QColor peach    {0xfa, 0xb3, 0x87};
static const QColor teal     {0x94, 0xe2, 0xd5};
} // namespace clrFL

// ─── Pattern badge metadata ───────────────────────────────────────────────────
struct PatternBadge {
    QColor  color;
    QString label;  // 2-char abbreviation
    QString tip;
};

static const PatternBadge kBadges[5] = {
    {clrFL::blue,   "St", "STL container / algorithm"},
    {clrFL::red,    "Cr", "Cryptographic primitive"},
    {clrFL::yellow, "Al", "Generic well-known algorithm"},
    {clrFL::mauve,  "Dp", "Design pattern (singleton, factory, …)"},
    {clrFL::teal,   "Lb", "Statically linked library code"},
};

static bool badgeActive(const PatternFlags& p, int i) {
    switch (i) {
    case 0: return p.stl;
    case 1: return p.crypto;
    case 2: return p.algo;
    case 3: return p.design;
    case 4: return p.library;
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// FunctionListModel
// ═════════════════════════════════════════════════════════════════════════════

FunctionListModel::FunctionListModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int FunctionListModel::rowCount(const QModelIndex&) const
{
    return static_cast<int>(fns_.size());
}

int FunctionListModel::columnCount(const QModelIndex&) const { return ColCount; }

QVariant FunctionListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const auto& e = fns_[static_cast<size_t>(index.row())];

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case ColAddress:    return QString("0x%1").arg(e.address, 16, 16, QChar('0'));
        case ColName:       return e.name.isEmpty() ? e.rawName : e.name;
        case ColSize:       return e.sizeBytes > 0
                                ? QString("%1 B").arg(e.sizeBytes) : "—";
        case ColInstrs:     return e.instrCount > 0
                                ? QString::number(e.instrCount) : "—";
        case ColConfidence: return QString("%1 %").arg(
                                static_cast<int>(e.confidence.composite() * 100));
        case ColPatterns:   return {}; // painted by delegate
        case ColCC:         return e.cc.isEmpty() ? "—" : e.cc;
        case ColNotes: {
            QString s = e.notes;
            if (!e.tags.isEmpty()) s += QString(s.isEmpty() ? "" : "  ") +
                                        "[" + e.tags.join(", ") + "]";
            return s;
        }
        }
        break;

    case Qt::EditRole:
        if (index.column() == ColName)  return e.name;
        if (index.column() == ColNotes) return e.notes;
        break;

    case Qt::ForegroundRole:
        switch (index.column()) {
        case ColAddress:    return clrFL::overlay0;
        case ColName:       return e.isLibrary ? clrFL::teal : clrFL::text;
        case ColSize:       return clrFL::subtext0;
        case ColInstrs:     return clrFL::subtext0;
        case ColConfidence: {
            float c = e.confidence.composite();
            if (c >= 0.75f) return clrFL::green;
            if (c >= 0.40f) return clrFL::yellow;
            return clrFL::red;
        }
        case ColCC:         return clrFL::mauve;
        case ColNotes:      return clrFL::subtext0;
        }
        break;

    case Qt::FontRole:
        if (index.column() == ColAddress || index.column() == ColName) {
            return QFont("Cascadia Code,Consolas,Monospace", 9);
        }
        break;

    case Qt::ToolTipRole:
        switch (index.column()) {
        case ColName: {
            QString tip = e.name;
            if (!e.rawName.isEmpty() && e.rawName != e.name)
                tip += "\n" + e.rawName;
            if (!e.signature.isEmpty())
                tip += "\n" + e.signature;
            return tip;
        }
        case ColConfidence:
            return QString("Type inference: %1%\nStructure: %2%\n"
                           "Variable: %3%\nAlgorithm: %4%\nComposite: %5%")
                   .arg(static_cast<int>(e.confidence.typeInference * 100))
                   .arg(static_cast<int>(e.confidence.structure     * 100))
                   .arg(static_cast<int>(e.confidence.variable      * 100))
                   .arg(static_cast<int>(e.confidence.algorithm     * 100))
                   .arg(static_cast<int>(e.confidence.composite()   * 100));
        case ColPatterns: {
            QStringList active;
            for (int i = 0; i < 5; ++i)
                if (badgeActive(e.patterns, i)) active << kBadges[i].tip;
            return active.isEmpty() ? "No patterns detected" : active.join("\n");
        }
        case ColCC:
            return "Calling convention: " + e.cc;
        }
        break;

    case Qt::UserRole:
        // Return the raw address for sorting
        if (index.column() == ColAddress)
            return static_cast<qulonglong>(e.address);
        if (index.column() == ColConfidence)
            return e.confidence.composite();
        break;

    // Custom role for the full FunctionEntry (used by delegate)
    case Qt::UserRole + 1: {
        QVariant v;
        v.setValue(static_cast<const void*>(&e));
        return v;
    }
    }
    return {};
}

QVariant FunctionListModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case ColAddress:    return "Address";
    case ColName:       return "Name";
    case ColSize:       return "Size";
    case ColInstrs:     return "Instrs";
    case ColConfidence: return "Confidence";
    case ColPatterns:   return "Patterns";
    case ColCC:         return "CC";
    case ColNotes:      return "Notes";
    }
    return {};
}

bool FunctionListModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || !index.isValid()) return false;
    auto& e = fns_[static_cast<size_t>(index.row())];

    if (index.column() == ColName) {
        QString oldName = e.name;
        QString newName = value.toString().trimmed();
        if (newName.isEmpty() || newName == oldName) return false;
        e.name = newName;
        emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
        emit functionRenamed(e.address, oldName, newName);
        return true;
    }
    if (index.column() == ColNotes) {
        e.notes = value.toString();
        emit dataChanged(index, index);
        return true;
    }
    return false;
}

Qt::ItemFlags FunctionListModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.column() == ColName || index.column() == ColNotes)
        f |= Qt::ItemIsEditable;
    return f;
}

void FunctionListModel::setFunctions(std::vector<FunctionEntry> fns)
{
    beginResetModel();
    fns_ = std::move(fns);
    endResetModel();
}

const FunctionEntry& FunctionListModel::entry(int row) const
{
    return fns_[static_cast<size_t>(row)];
}

FunctionEntry& FunctionListModel::entry(int row)
{
    return fns_[static_cast<size_t>(row)];
}

bool FunctionListModel::renameFunction(uint64_t address, const QString& newName)
{
    for (int i = 0; i < static_cast<int>(fns_.size()); ++i) {
        if (fns_[static_cast<size_t>(i)].address == address) {
            QModelIndex idx = createIndex(i, ColName);
            return setData(idx, newName, Qt::EditRole);
        }
    }
    return false;
}

void FunctionListModel::applyTag(const std::vector<uint64_t>& addresses,
                                  const QString& tag)
{
    for (auto& fn : fns_) {
        if (std::find(addresses.begin(), addresses.end(), fn.address) != addresses.end()) {
            if (!fn.tags.contains(tag)) fn.tags.append(tag);
        }
    }
    emit dataChanged(createIndex(0, ColNotes),
                     createIndex(static_cast<int>(fns_.size()) - 1, ColNotes));
}

void FunctionListModel::clearAll()
{
    beginResetModel();
    fns_.clear();
    endResetModel();
}

// ═════════════════════════════════════════════════════════════════════════════
// FunctionPatternDelegate
// ═════════════════════════════════════════════════════════════════════════════

FunctionPatternDelegate::FunctionPatternDelegate(FunctionListModel* model,
                                                  QObject* parent)
    : QStyledItemDelegate(parent), model_(model) {}

QSize FunctionPatternDelegate::sizeHint(const QStyleOptionViewItem& opt,
                                         const QModelIndex& index) const
{
    if (index.column() == FunctionListModel::ColPatterns)
        return QSize(5 * 16 + 4, 20);
    if (index.column() == FunctionListModel::ColConfidence)
        return QSize(80, 20);
    return QStyledItemDelegate::sizeHint(opt, index);
}

void FunctionPatternDelegate::paint(QPainter* painter,
                                     const QStyleOptionViewItem& opt,
                                     const QModelIndex& index) const
{
    QStyledItemDelegate::paint(painter, opt, index);

    if (index.column() == FunctionListModel::ColConfidence) {
        // Get real source index to avoid proxy confusion
        QVariant v = index.data(Qt::UserRole + 1);
        if (!v.isValid()) return;
        const auto* entry = static_cast<const FunctionEntry*>(v.value<const void*>());
        if (!entry) return;
        paintConfidence(painter, opt, entry->confidence.composite());
        return;
    }

    if (index.column() == FunctionListModel::ColPatterns) {
        QVariant v = index.data(Qt::UserRole + 1);
        if (!v.isValid()) return;
        const auto* entry = static_cast<const FunctionEntry*>(v.value<const void*>());
        if (!entry) return;
        paintPatterns(painter, opt, entry->patterns);
        return;
    }
}

void FunctionPatternDelegate::paintConfidence(QPainter* painter,
                                               const QStyleOptionViewItem& opt,
                                               float confidence) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    QRectF r = opt.rect.adjusted(4, 4, -4, -4);
    QRectF fill = r;
    fill.setWidth(r.width() * static_cast<double>(confidence));

    // Background track
    painter->setBrush(QColor(0x31, 0x32, 0x44));
    painter->setPen(Qt::NoPen);
    painter->drawRoundedRect(r, 3, 3);

    // Filled portion — colour shifts green → yellow → red as confidence decreases
    QColor barColor;
    if (confidence >= 0.75f)       barColor = clrFL::green;
    else if (confidence >= 0.40f)  barColor = clrFL::yellow;
    else                            barColor = clrFL::red;
    barColor.setAlpha(200);

    painter->setBrush(barColor);
    if (fill.width() > 0)
        painter->drawRoundedRect(fill, 3, 3);

    painter->restore();
}

void FunctionPatternDelegate::paintPatterns(QPainter* painter,
                                             const QStyleOptionViewItem& opt,
                                             const PatternFlags& patterns) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    const int kBadgeW  = 14;
    const int kBadgeH  = 14;
    const int kSpacing = 2;
    int x = opt.rect.left() + 4;
    int y = opt.rect.top() + (opt.rect.height() - kBadgeH) / 2;

    painter->setFont(QFont("Monospace", 6, QFont::Bold));

    for (int i = 0; i < 5; ++i) {
        QRectF r(x, y, kBadgeW, kBadgeH);
        bool active = badgeActive(patterns, i);
        if (active) {
            painter->setBrush(kBadges[i].color);
            painter->setPen(Qt::NoPen);
        } else {
            painter->setBrush(QColor(0x31, 0x32, 0x44));
            painter->setPen(QPen(QColor(0x45, 0x47, 0x5a), 0.5));
        }
        painter->drawRoundedRect(r, 2, 2);

        if (active) {
            painter->setPen(QColor(0x1e, 0x1e, 0x2e));
            painter->drawText(r, Qt::AlignCenter, kBadges[i].label);
        }
        x += kBadgeW + kSpacing;
    }

    painter->restore();
}

// ═════════════════════════════════════════════════════════════════════════════
// FunctionFilterProxy
// ═════════════════════════════════════════════════════════════════════════════

FunctionFilterProxy::FunctionFilterProxy(FunctionListModel* model, QObject* parent)
    : QSortFilterProxyModel(parent), model_(model)
{
    setSourceModel(model);
    setSortRole(Qt::UserRole);
}

void FunctionFilterProxy::setNameGlob(const QString& glob)
{
    nameGlob_ = glob;
    invalidateFilter();
}

void FunctionFilterProxy::setAddressRange(uint64_t low, uint64_t high)
{
    addrLow_  = low;
    addrHigh_ = high;
    invalidateFilter();
}

void FunctionFilterProxy::setMinConfidence(float minConf)
{
    minConfidence_ = minConf;
    invalidateFilter();
}

void FunctionFilterProxy::setPatternFilter(bool stl, bool crypto, bool algo,
                                            bool design, bool lib)
{
    filterStl_    = stl;
    filterCrypto_ = crypto;
    filterAlgo_   = algo;
    filterDesign_ = design;
    filterLib_    = lib;
    anyPatternActive_ = stl || crypto || algo || design || lib;
    invalidateFilter();
}

void FunctionFilterProxy::setClassFilter(const QString& className)
{
    classFilter_ = className;
    invalidateFilter();
}

bool FunctionFilterProxy::filterAcceptsRow(int sourceRow,
                                             const QModelIndex&) const
{
    if (sourceRow < 0 || sourceRow >= model_->rowCount()) return false;
    const FunctionEntry& e = model_->entry(sourceRow);

    // Address range
    if (addrLow_ != 0 && e.address < addrLow_)   return false;
    if (addrHigh_ != 0 && e.address > addrHigh_)  return false;

    // Confidence threshold
    if (e.confidence.composite() < minConfidence_) return false;

    // Pattern filter
    if (anyPatternActive_) {
        bool match = (filterStl_    && e.patterns.stl)
                  || (filterCrypto_ && e.patterns.crypto)
                  || (filterAlgo_   && e.patterns.algo)
                  || (filterDesign_ && e.patterns.design)
                  || (filterLib_    && e.patterns.library);
        if (!match) return false;
    }

    // Class filter (function name must start with ClassName::)
    if (!classFilter_.isEmpty()) {
        QString prefix = classFilter_ + "::";
        if (!e.name.startsWith(prefix) && !e.rawName.startsWith(prefix))
            return false;
    }

    // Name glob (empty = pass all)
    if (!nameGlob_.isEmpty()) {
        QRegularExpression re(
            QRegularExpression::wildcardToRegularExpression(nameGlob_),
            QRegularExpression::CaseInsensitiveOption);
        if (!re.match(e.name).hasMatch() && !re.match(e.rawName).hasMatch())
            return false;
    }

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// FunctionListPanel
// ═════════════════════════════════════════════════════════════════════════════

FunctionListPanel::FunctionListPanel(QWidget* parent)
    : PanelBase("Functions", parent)
{
    setupUI();
}

void FunctionListPanel::setupUI()
{
    // ── Model stack ───────────────────────────────────────────────────────
    model_    = new FunctionListModel(this);
    proxy_    = new FunctionFilterProxy(model_, this);
    delegate_ = new FunctionPatternDelegate(model_, this);

    // ── Filter bar ────────────────────────────────────────────────────────
    auto* filterBar    = new QWidget(this);
    filterBar->setProperty("role", "filter-bar");
    auto* fbLayout     = new QHBoxLayout(filterBar);
    fbLayout->setContentsMargins(4, 3, 4, 3);
    fbLayout->setSpacing(4);
    setupFilterBar(filterBar, fbLayout);

    // ── Table ─────────────────────────────────────────────────────────────
    tableView_ = new QTableView(this);
    tableView_->setModel(proxy_);
    tableView_->setItemDelegate(delegate_);
    tableView_->setAlternatingRowColors(true);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tableView_->setSortingEnabled(true);
    tableView_->sortByColumn(FunctionListModel::ColAddress, Qt::AscendingOrder);
    tableView_->verticalHeader()->hide();
    tableView_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                QAbstractItemView::EditKeyPressed);
    tableView_->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* hdr = tableView_->horizontalHeader();
    hdr->setSectionResizeMode(FunctionListModel::ColAddress,    QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(FunctionListModel::ColName,       QHeaderView::Stretch);
    hdr->setSectionResizeMode(FunctionListModel::ColSize,       QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(FunctionListModel::ColInstrs,     QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(FunctionListModel::ColConfidence, QHeaderView::Fixed);
    hdr->resizeSection(FunctionListModel::ColConfidence, 88);
    hdr->setSectionResizeMode(FunctionListModel::ColPatterns,   QHeaderView::Fixed);
    hdr->resizeSection(FunctionListModel::ColPatterns, 90);
    hdr->setSectionResizeMode(FunctionListModel::ColCC,         QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(FunctionListModel::ColNotes,      QHeaderView::Stretch);

    // ── Row height ────────────────────────────────────────────────────────
    tableView_->verticalHeader()->setDefaultSectionSize(22);

    emptyState_ = new retdec::gui::widgets::EmptyStateWidget(this);
    emptyState_->setTitle(QStringLiteral("No functions yet"));
    emptyState_->setHint(QStringLiteral("Open a binary and run analysis (F5) to populate the function list."));

    tableStack_ = new QStackedWidget(this);
    tableStack_->addWidget(emptyState_);
    tableStack_->addWidget(tableView_);

    // ── Status bar ────────────────────────────────────────────────────────
    statusLabel_ = new QLabel("Ready", this);
    statusLabel_->setProperty("role", "status-muted");

    // ── Overall layout ────────────────────────────────────────────────────
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(filterBar);
    layout->addWidget(tableStack_, 1);
    layout->addWidget(statusLabel_);

    // ── Connections ───────────────────────────────────────────────────────
    connect(tableView_, &QTableView::doubleClicked,
            this,       &FunctionListPanel::onDoubleClicked);
    connect(tableView_, &QWidget::customContextMenuRequested,
            this,       &FunctionListPanel::onContextMenu);
    connect(tableView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this,       &FunctionListPanel::selectionChanged);
    connect(model_,     &FunctionListModel::functionRenamed,
            this,       &FunctionListPanel::functionRenamed);
    connect(proxy_,     &QSortFilterProxyModel::rowsInserted,
            this,       &FunctionListPanel::updateStatusLabel);
    connect(proxy_,     &QSortFilterProxyModel::rowsRemoved,
            this,       &FunctionListPanel::updateStatusLabel);
    connect(proxy_,     &QSortFilterProxyModel::modelReset,
            this,       &FunctionListPanel::updateStatusLabel);
    updateStatusLabel();
}

void FunctionListPanel::setupFilterBar(QWidget* parent, QHBoxLayout* layout)
{
    // Name filter
    nameFilter_ = new QLineEdit(parent);
    nameFilter_->setPlaceholderText("Search…");
    nameFilter_->setClearButtonEnabled(true);
    nameFilter_->setFixedWidth(140);

    // Address range
    auto* addrLabel = new QLabel("Addr:", parent);
    addrLabel->setProperty("role", "subtext");
    addrLow_ = new QLineEdit(parent);
    addrLow_->setPlaceholderText("0x0");
    addrLow_->setFixedWidth(80);
    auto* dashLabel = new QLabel("–", parent);
    dashLabel->setProperty("role", "muted");
    addrHigh_ = new QLineEdit(parent);
    addrHigh_->setPlaceholderText("0xFFFF…");
    addrHigh_->setFixedWidth(80);

    // Confidence threshold
    auto* confLabel = new QLabel("Conf≥", parent);
    confLabel->setProperty("role", "subtext");
    confSpin_ = new QDoubleSpinBox(parent);
    confSpin_->setRange(0.0, 1.0);
    confSpin_->setSingleStep(0.05);
    confSpin_->setValue(0.0);
    confSpin_->setFixedWidth(56);

    // Pattern checkboxes
    ckStl_    = new QCheckBox("STL",    parent);
    ckCrypto_ = new QCheckBox("Crypto", parent);
    ckAlgo_   = new QCheckBox("Algo",   parent);
    ckDesign_ = new QCheckBox("Pat",    parent);
    ckLib_    = new QCheckBox("Lib",    parent);

    // Export buttons
    csvButton_  = new QPushButton("CSV",  parent);
    csvButton_->setProperty("compact", true);
    jsonButton_ = new QPushButton("JSON", parent);
    jsonButton_->setProperty("compact", true);

    layout->addWidget(nameFilter_);
    layout->addWidget(addrLabel);
    layout->addWidget(addrLow_);
    layout->addWidget(dashLabel);
    layout->addWidget(addrHigh_);
    layout->addWidget(confLabel);
    layout->addWidget(confSpin_);
    layout->addWidget(ckStl_);
    layout->addWidget(ckCrypto_);
    layout->addWidget(ckAlgo_);
    layout->addWidget(ckDesign_);
    layout->addWidget(ckLib_);
    layout->addStretch(1);
    layout->addWidget(csvButton_);
    layout->addWidget(jsonButton_);

    // Connect all filter controls
    auto onFilter = [this]() { onFilterChanged(); };
    connect(nameFilter_,  &QLineEdit::textChanged,    this, [onFilter](auto) { onFilter(); });
    connect(addrLow_,     &QLineEdit::textChanged,    this, [onFilter](auto) { onFilter(); });
    connect(addrHigh_,    &QLineEdit::textChanged,    this, [onFilter](auto) { onFilter(); });
    connect(confSpin_,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [onFilter](double) { onFilter(); });
    for (auto* ck : {ckStl_, ckCrypto_, ckAlgo_, ckDesign_, ckLib_})
        connect(ck, &QCheckBox::toggled, this, [onFilter](bool) { onFilter(); });

    connect(csvButton_,  &QPushButton::clicked, this, &FunctionListPanel::onExportCsv);
    connect(jsonButton_, &QPushButton::clicked, this, &FunctionListPanel::onExportJson);
}

void FunctionListPanel::setFunctions(std::vector<FunctionEntry> fns)
{
    model_->setFunctions(std::move(fns));
    updateStatusLabel();
}

void FunctionListPanel::clear()
{
    model_->clearAll();
    nameFilter_->clear();
    addrLow_->clear();
    addrHigh_->clear();
    confSpin_->setValue(0.0);
    for (auto* ck : {ckStl_, ckCrypto_, ckAlgo_, ckDesign_, ckLib_})
        ck->setChecked(false);
    updateStatusLabel();
}

void FunctionListPanel::filterByClass(const QString& className)
{
    proxy_->setClassFilter(className);
    updateStatusLabel();
}

void FunctionListPanel::onFunctionSelected(uint64_t /*address*/, const QString& /*name*/) {}

std::optional<FunctionEntry> FunctionListPanel::selectedEntry() const
{
    const auto selected = tableView_->selectionModel()->selectedRows();
    if (selected.isEmpty())
        return std::nullopt;
    const QModelIndex src = proxy_->mapToSource(selected.first());
    if (!src.isValid())
        return std::nullopt;
    return model_->entry(src.row());
}

void FunctionListPanel::selectFunction(uint64_t address)
{
    for (int r = 0; r < model_->rowCount(); ++r) {
        const auto& e = model_->entry(r);
        if (e.address != address)
            continue;
        const QModelIndex srcIdx = model_->index(r, 0);
        const QModelIndex proxyIdx = proxy_->mapFromSource(srcIdx);
        if (!proxyIdx.isValid())
            return;
        tableView_->selectionModel()->select(
                proxyIdx,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        tableView_->scrollTo(proxyIdx);
        emit functionSelected(e.address, e.name.isEmpty() ? e.rawName : e.name);
        return;
    }
}

void FunctionListPanel::onFilterChanged()
{
    proxy_->setNameGlob(nameFilter_->text());

    // Parse address range
    bool okL = false, okH = false;
    uint64_t lo = addrLow_->text().toULongLong(&okL, 16);
    uint64_t hi = addrHigh_->text().toULongLong(&okH, 16);
    proxy_->setAddressRange(okL ? lo : 0, okH ? hi : 0);

    proxy_->setMinConfidence(static_cast<float>(confSpin_->value()));
    proxy_->setPatternFilter(ckStl_->isChecked(), ckCrypto_->isChecked(),
                              ckAlgo_->isChecked(), ckDesign_->isChecked(),
                              ckLib_->isChecked());
    updateStatusLabel();
}

void FunctionListPanel::onDoubleClicked(const QModelIndex& index)
{
    if (index.column() == FunctionListModel::ColName) return; // let edit trigger handle
    QModelIndex src = proxy_->mapToSource(index);
    if (!src.isValid()) return;
    const auto& e = model_->entry(src.row());
    emit functionSelected(e.address, e.name.isEmpty() ? e.rawName : e.name);
}

void FunctionListPanel::onContextMenu(const QPoint& pos)
{
    QModelIndex idx = tableView_->indexAt(pos);
    bool hasSelection = !tableView_->selectionModel()->selectedRows().isEmpty();

    QMenu menu;

    auto* navigateAct = menu.addAction("Navigate to Function");
    navigateAct->setEnabled(idx.isValid());
    auto* redecompileAct = menu.addAction("Re-decompile Selected Function");
    redecompileAct->setEnabled(idx.isValid());
    redecompileAct->setToolTip(
            QStringLiteral("Run retdec-decompiler with --select-functions for this function."));
    auto* renameAct   = menu.addAction("Rename…");
    renameAct->setEnabled(idx.isValid());
    menu.addSeparator();
    auto* tagAct      = menu.addAction("Add Tag to Selected…");
    tagAct->setEnabled(hasSelection);
    menu.addSeparator();
    auto* copyAct     = menu.addAction("Copy Address");
    copyAct->setEnabled(idx.isValid());
    auto* copySigAct  = menu.addAction("Copy Signature");
    copySigAct->setEnabled(idx.isValid());
    menu.addSeparator();
    menu.addAction("Export CSV",  this, &FunctionListPanel::onExportCsv);
    menu.addAction("Export JSON", this, &FunctionListPanel::onExportJson);

    QAction* chosen = menu.exec(tableView_->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == navigateAct && idx.isValid()) {
        onDoubleClicked(idx);
    } else if (chosen == redecompileAct && idx.isValid()) {
        const QModelIndex src = proxy_->mapToSource(idx);
        if (src.isValid()) {
            const auto& e = model_->entry(src.row());
            emit functionSelected(e.address, e.name.isEmpty() ? e.rawName : e.name);
            emit redecompileRequested(e.address, e.selectFunctionsCliName());
        }
    } else if (chosen == renameAct && idx.isValid()) {
        onRenameFunction();
    } else if (chosen == tagAct) {
        onBatchTag();
    } else if (chosen == copyAct && idx.isValid()) {
        QModelIndex src = proxy_->mapToSource(idx);
        if (src.isValid()) {
            const auto& e = model_->entry(src.row());
            QApplication::clipboard()->setText(
                QString("0x%1").arg(e.address, 0, 16));
        }
    } else if (chosen == copySigAct && idx.isValid()) {
        QModelIndex src = proxy_->mapToSource(idx);
        if (src.isValid()) {
            const auto& e = model_->entry(src.row());
            QApplication::clipboard()->setText(e.signature);
        }
    }
}

void FunctionListPanel::onRenameFunction()
{
    auto selected = tableView_->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;
    QModelIndex src = proxy_->mapToSource(selected.first());
    if (!src.isValid()) return;
    const auto& e = model_->entry(src.row());

    bool ok = false;
    QString newName = QInputDialog::getText(
        this, "Rename Function",
        QString("New name for  0x%1:").arg(e.address, 0, 16),
        QLineEdit::Normal,
        e.name.isEmpty() ? e.rawName : e.name,
        &ok);
    if (ok && !newName.trimmed().isEmpty())
        model_->renameFunction(e.address, newName.trimmed());
}

void FunctionListPanel::onBatchTag()
{
    bool ok = false;
    QString tag = QInputDialog::getText(
        this, "Add Tag", "Tag name:", QLineEdit::Normal, "", &ok);
    if (!ok || tag.trimmed().isEmpty()) return;

    model_->applyTag(selectedAddresses(), tag.trimmed());
}

void FunctionListPanel::onExportCsv()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Export Function List (CSV)", QString(), "CSV files (*.csv)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed", "Cannot open " + path);
        return;
    }

    QTextStream ts(&file);
    ts << "Address,Name,SizeBytes,Instrs,Confidence,CC,STL,Crypto,Algo,Design,Lib,Notes\n";

    for (int r = 0; r < proxy_->rowCount(); ++r) {
        QModelIndex src = proxy_->mapToSource(proxy_->index(r, 0));
        const auto& e   = model_->entry(src.row());
        ts << QString("0x%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12\n")
               .arg(e.address, 0, 16)
               .arg(e.name)
               .arg(e.sizeBytes)
               .arg(e.instrCount)
               .arg(e.confidence.composite(), 0, 'f', 3)
               .arg(e.cc)
               .arg(e.patterns.stl    ? 1 : 0)
               .arg(e.patterns.crypto ? 1 : 0)
               .arg(e.patterns.algo   ? 1 : 0)
               .arg(e.patterns.design ? 1 : 0)
               .arg(e.patterns.library? 1 : 0)
               .arg(e.notes);
    }
}

void FunctionListPanel::onExportJson()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Export Function List (JSON)", QString(), "JSON files (*.json)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Export Failed", "Cannot open " + path);
        return;
    }

    QJsonArray arr;
    for (int r = 0; r < proxy_->rowCount(); ++r) {
        QModelIndex src = proxy_->mapToSource(proxy_->index(r, 0));
        const auto& e   = model_->entry(src.row());
        QJsonObject obj;
        obj["address"]    = QString("0x%1").arg(e.address, 0, 16);
        obj["name"]       = e.name;
        obj["rawName"]    = e.rawName;
        obj["signature"]  = e.signature;
        obj["sizeBytes"]  = e.sizeBytes;
        obj["instrCount"] = e.instrCount;
        obj["confidence"] = static_cast<double>(e.confidence.composite());
        obj["cc"]         = e.cc;
        obj["notes"]      = e.notes;
        QJsonObject conf;
        conf["typeInference"] = static_cast<double>(e.confidence.typeInference);
        conf["structure"]     = static_cast<double>(e.confidence.structure);
        conf["variable"]      = static_cast<double>(e.confidence.variable);
        conf["algorithm"]     = static_cast<double>(e.confidence.algorithm);
        obj["confidenceDetail"] = conf;
        QJsonObject pats;
        pats["stl"]     = e.patterns.stl;
        pats["crypto"]  = e.patterns.crypto;
        pats["algo"]    = e.patterns.algo;
        pats["design"]  = e.patterns.design;
        pats["library"] = e.patterns.library;
        obj["patterns"] = pats;
        QJsonArray tags;
        for (auto& t : e.tags) tags.append(t);
        obj["tags"] = tags;
        arr.append(obj);
    }

    file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

std::vector<uint64_t> FunctionListPanel::selectedAddresses() const
{
    std::vector<uint64_t> addrs;
    for (auto& proxyIdx : tableView_->selectionModel()->selectedRows()) {
        QModelIndex src = proxy_->mapToSource(proxyIdx);
        if (src.isValid())
            addrs.push_back(model_->entry(src.row()).address);
    }
    return addrs;
}

void FunctionListPanel::updateStatusLabel()
{
    int total   = model_->rowCount();
    int visible = proxy_->rowCount();
    if (total == 0) {
        statusLabel_->setText(QStringLiteral("No functions — run Analysis (F5)"));
    } else {
        statusLabel_->setText(
            visible == total
                ? QString("%1 functions").arg(total)
                : QString("%1 / %2 functions").arg(visible).arg(total));
    }
    updateEmptyState();
}

void FunctionListPanel::updateEmptyState()
{
    if (!tableStack_) return;
    tableStack_->setCurrentIndex(model_->rowCount() == 0 ? 0 : 1);
}

} // namespace retdec::gui::panels
