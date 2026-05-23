/**
 * @file src/gui/panels/type_hierarchy_panel.cpp
 * @brief Type hierarchy browser with vtable viewer — full implementation (Task 52).
 */

#include <memory>
#include "retdec/gui/panels/type_hierarchy_panel.h"

#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QTreeView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QColor>

#include <algorithm>
#include <unordered_map>

namespace retdec::gui::panels {

// ─── Colours ──────────────────────────────────────────────────────────────────
namespace {
QColor inheritColor(InheritanceLink::Kind kind) {
    switch (kind) {
    case InheritanceLink::Kind::Public:    return QColor(0xa6, 0xe3, 0xa1); // green
    case InheritanceLink::Kind::Protected: return QColor(0xf9, 0xe2, 0xaf); // yellow
    case InheritanceLink::Kind::Private:   return QColor(0xf3, 0x8b, 0xa8); // red
    case InheritanceLink::Kind::Virtual:   return QColor(0xcb, 0xa6, 0xf7); // mauve
    }
    return QColor(0xa6, 0xad, 0xc8);
}
QString inheritLabel(InheritanceLink::Kind kind) {
    switch (kind) {
    case InheritanceLink::Kind::Public:    return "public";
    case InheritanceLink::Kind::Protected: return "protected";
    case InheritanceLink::Kind::Private:   return "private";
    case InheritanceLink::Kind::Virtual:   return "virtual";
    }
    return {};
}
} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
// ClassHierarchyModel
// ═════════════════════════════════════════════════════════════════════════════

ClassHierarchyModel::ClassHierarchyModel(QObject* parent)
    : QAbstractItemModel(parent) {}

ClassHierarchyModel::~ClassHierarchyModel() = default;

void ClassHierarchyModel::setClasses(const QList<ClassInfo>& classes)
{
    beginResetModel();
    classes_ = classes;
    roots_.clear();
    buildTree(classes);
    endResetModel();
}

void ClassHierarchyModel::buildTree(const QList<ClassInfo>& classes)
{
    // Map from class name → Node*
    std::unordered_map<std::string, Node*> byName;

    // Create all nodes first
    std::vector<std::unique_ptr<Node>> allNodes;
    allNodes.reserve(static_cast<size_t>(classes.size()));
    for (const auto& cls : classes) {
        auto node = std::make_unique<Node>();
        node->info = &classes_[static_cast<int>(&cls - &classes[0])];
        byName[cls.name.toStdString()] = node.get();
        allNodes.push_back(std::move(node));
    }

    // Attach children
    for (auto& node : allNodes) {
        bool placed = false;
        for (const auto& link : node->info->bases) {
            auto it = byName.find(link.base.toStdString());
            if (it != byName.end()) {
                node->parent = it->second;
                node->row = static_cast<int>(it->second->children.size());
                it->second->children.push_back(std::move(node));
                placed = true;
                break;
            }
        }
        if (!placed) {
            // Root node
            node->row = static_cast<int>(roots_.size());
            roots_.push_back(std::move(node));
        }
    }
}

QModelIndex ClassHierarchyModel::index(int row, int col,
                                        const QModelIndex& parent) const
{
    if (!hasIndex(row, col, parent)) return {};
    if (!parent.isValid()) {
        // Root level
        if (row < static_cast<int>(roots_.size()))
            return createIndex(row, col, roots_[static_cast<size_t>(row)].get());
        return {};
    }
    auto* parentNode = static_cast<Node*>(parent.internalPointer());
    if (row < static_cast<int>(parentNode->children.size()))
        return createIndex(row, col, parentNode->children[static_cast<size_t>(row)].get());
    return {};
}

QModelIndex ClassHierarchyModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) return {};
    auto* node = static_cast<Node*>(child.internalPointer());
    if (!node->parent) return {};
    auto* gp = node->parent->parent;
    if (!gp) {
        // parent is a root
        return createIndex(node->parent->row, 0, node->parent);
    }
    return createIndex(node->parent->row, 0, node->parent);
}

int ClassHierarchyModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid()) return static_cast<int>(roots_.size());
    auto* node = static_cast<Node*>(parent.internalPointer());
    return static_cast<int>(node->children.size());
}

int ClassHierarchyModel::columnCount(const QModelIndex&) const { return 5; }

QVariant ClassHierarchyModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    auto* node = static_cast<Node*>(index.internalPointer());
    const ClassInfo* cls = node->info;

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: {
            QString name = cls->name;
            if (cls->isAbstract) name += " [abstract]";
            return name;
        }
        case 1: {
            // Inheritance type badges
            if (cls->bases.isEmpty()) return "—";
            QStringList parts;
            for (const auto& b : cls->bases)
                parts << QString("%1 %2").arg(inheritLabel(b.kind)).arg(b.base);
            return parts.join(", ");
        }
        case 2: return cls->vtableAddress
                    ? QString("0x%1").arg(cls->vtableAddress, 0, 16) : "—";
        case 3: return cls->methodCount;
        case 4: return cls->compiler.isEmpty() ? "—" : cls->compiler;
        }
    }

    if (role == Qt::ForegroundRole) {
        switch (index.column()) {
        case 0: return cls->isAbstract
                    ? QColor(0xcb, 0xa6, 0xf7)  // mauve for abstract
                    : QColor(0xcd, 0xd6, 0xf4);  // text
        case 1: {
            if (cls->bases.isEmpty()) return QColor(0x6c, 0x70, 0x86);
            return inheritColor(cls->bases[0].kind);
        }
        case 2: return QColor(0x89, 0xb4, 0xfa); // blue for addresses
        case 3: return QColor(0x94, 0xe2, 0xd5); // teal for counts
        case 4: return QColor(0xa6, 0xad, 0xc8); // subtext0
        }
    }

    if (role == Qt::FontRole && index.column() == 0) {
        QFont f("Cascadia Code,Consolas,Monospace", 9);
        if (cls->isAbstract) f.setItalic(true);
        return f;
    }

    if (role == Qt::ToolTipRole) {
        QString tip = cls->name;
        if (!cls->moduleName.isEmpty()) tip += "\nModule: " + cls->moduleName;
        tip += QString("\nVtable: 0x%1").arg(cls->vtableAddress, 0, 16);
        tip += QString("\nMethods: %1").arg(cls->methodCount);
        for (const auto& b : cls->bases)
            tip += QString("\n  inherits %1 %2").arg(inheritLabel(b.kind)).arg(b.base);
        return tip;
    }

    return {};
}

QVariant ClassHierarchyModel::headerData(int section, Qt::Orientation orientation,
                                          int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return "Class";
    case 1: return "Inherits";
    case 2: return "Vtable";
    case 3: return "Methods";
    case 4: return "Compiler";
    }
    return {};
}

const ClassInfo* ClassHierarchyModel::classAt(const QModelIndex& index) const
{
    if (!index.isValid()) return nullptr;
    return static_cast<Node*>(index.internalPointer())->info;
}

// ═════════════════════════════════════════════════════════════════════════════
// VtableModel
// ═════════════════════════════════════════════════════════════════════════════

VtableModel::VtableModel(QObject* parent) : QAbstractTableModel(parent) {}

void VtableModel::setSlots(const QList<VtableSlot>& newSlots)
{
    beginResetModel();
    slots_ = newSlots;
    endResetModel();
}

const VtableSlot* VtableModel::slotAt(int row) const
{
    if (row < 0 || row >= slots_.size()) return nullptr;
    return &slots_[row];
}

int VtableModel::rowCount(const QModelIndex&) const { return slots_.size(); }
int VtableModel::columnCount(const QModelIndex&) const { return 4; }

QVariant VtableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= slots_.size()) return {};
    const auto& s = slots_[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return s.index;
        case 1: return s.funcName;
        case 2: return s.funcAddress
                    ? QString("0x%1").arg(s.funcAddress, 0, 16) : "—";
        case 3: {
            QString flags;
            if (s.isPure)     flags += "pure ";
            if (s.isOverride) flags += "override";
            return flags.trimmed().isEmpty() ? "—" : flags.trimmed();
        }
        }
    }

    if (role == Qt::ForegroundRole) {
        switch (index.column()) {
        case 0: return QColor(0x6c, 0x70, 0x86);
        case 1: return s.isPure
                    ? QColor(0xcb, 0xa6, 0xf7)  // mauve = pure virtual
                    : QColor(0xcd, 0xd6, 0xf4);
        case 2: return QColor(0x89, 0xb4, 0xfa);
        case 3: return QColor(0xa6, 0xad, 0xc8);
        }
    }

    if (role == Qt::FontRole && index.column() == 1) {
        QFont f("Cascadia Code,Consolas,Monospace", 9);
        if (s.isPure) f.setItalic(true);
        return f;
    }

    if (role == Qt::ToolTipRole && index.column() == 1) {
        return QString("0x%1: %2%3")
               .arg(s.funcAddress, 0, 16)
               .arg(s.funcName)
               .arg(s.isPure ? " = 0" : "");
    }

    return {};
}

QVariant VtableModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return "#";
    case 1: return "Function";
    case 2: return "Address";
    case 3: return "Flags";
    }
    return {};
}

// ═════════════════════════════════════════════════════════════════════════════
// TypeHierarchyPanel
// ═════════════════════════════════════════════════════════════════════════════

TypeHierarchyPanel::TypeHierarchyPanel(QWidget* parent)
    : PanelBase("Type Hierarchy", parent)
{
    setupUI();
}

void TypeHierarchyPanel::setupUI()
{
    // ── Search bar ───────────────────────────────────────────────────────
    searchBox_ = new QLineEdit(this);
    searchBox_->setPlaceholderText("Filter classes…");
    searchBox_->setClearButtonEnabled(true);
    searchBox_->setStyleSheet(
        "QLineEdit { background: #313244; color: #cdd6f4; border: 1px solid #45475a;"
        " border-radius: 4px; padding: 2px 6px; }");

    // ── Hierarchy model & proxy ───────────────────────────────────────────
    hierarchyModel_ = new ClassHierarchyModel(this);
    auto* proxy     = new QSortFilterProxyModel(this);
    proxy->setSourceModel(hierarchyModel_);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy->setFilterKeyColumn(0);
    proxy->setRecursiveFilteringEnabled(true);

    treeView_ = new QTreeView(this);
    treeView_->setModel(proxy);
    treeView_->setAlternatingRowColors(true);
    treeView_->setUniformRowHeights(true);
    treeView_->setAnimated(false);
    treeView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    treeView_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    treeView_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    treeView_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    treeView_->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    treeView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    treeView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    treeView_->expandAll();

    emptyLabel_ = new QLabel(QStringLiteral("No RTTI classes recovered"), this);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setStyleSheet(
            QStringLiteral("color: #6c7086; font-style: italic; padding: 24px;"));
    emptyLabel_->setVisible(true);

    // ── Vtable sub-panel ─────────────────────────────────────────────────
    auto* vtableLabel = new QLabel("Vtable", this);
    vtableLabel->setStyleSheet("color: #a6adc8; font-weight: bold; padding: 4px 6px 2px;");

    vtableModel_ = new VtableModel(this);
    vtableView_  = new QTableView(this);
    vtableView_->setModel(vtableModel_);
    vtableView_->setAlternatingRowColors(true);
    vtableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    vtableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vtableView_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    vtableView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    vtableView_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    vtableView_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    vtableView_->verticalHeader()->hide();

    auto* vtableContainer = new QWidget(this);
    auto* vtableLayout    = new QVBoxLayout(vtableContainer);
    vtableLayout->setContentsMargins(0, 0, 0, 0);
    vtableLayout->setSpacing(0);
    vtableLayout->addWidget(vtableLabel);
    vtableLayout->addWidget(vtableView_);

    // ── Splitter ─────────────────────────────────────────────────────────
    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->addWidget(treeView_);
    splitter_->addWidget(vtableContainer);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setStyleSheet(
        "QSplitter::handle { background: #45475a; height: 2px; }");
    emptyLabel_->setParent(splitter_);
    emptyLabel_->raise();

    // ── Overall layout ────────────────────────────────────────────────────
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(searchBox_);
    layout->addWidget(splitter_);

    // ── Connections ───────────────────────────────────────────────────────
    connect(searchBox_, &QLineEdit::textChanged,
            this,       &TypeHierarchyPanel::onFilterChanged);
    connect(treeView_->selectionModel(), &QItemSelectionModel::currentChanged,
            this,                        &TypeHierarchyPanel::onClassClicked);
    connect(vtableView_, &QAbstractItemView::doubleClicked,
            this,        &TypeHierarchyPanel::onVtableSlotDoubleClicked);
}

void TypeHierarchyPanel::setHierarchy(const QList<ClassInfo>& classes)
{
    hierarchyModel_->setClasses(classes);
    vtableModel_->setSlots({});
    treeView_->expandAll();
    if (emptyLabel_) {
        emptyLabel_->setVisible(classes.isEmpty());
        emptyLabel_->raise();
    }
}

void TypeHierarchyPanel::setHierarchy(const QList<ClassEntry>& entries)
{
    QList<ClassInfo> classes;
    classes.reserve(entries.size());
    for (const auto& e : entries) {
        ClassInfo ci;
        ci.name          = e.name;
        ci.vtableAddress = e.vtableAddress;
        ci.compiler      = e.compiler;
        for (const auto& p : e.parents) {
            InheritanceLink link;
            link.base = p;
            link.kind = InheritanceLink::Kind::Public;
            ci.bases.append(link);
        }
        classes.append(ci);
    }
    setHierarchy(classes);
}

void TypeHierarchyPanel::clear()
{
    hierarchyModel_->setClasses({});
    vtableModel_->setSlots({});
    searchBox_->clear();
    if (emptyLabel_)
        emptyLabel_->setVisible(true);
}

void TypeHierarchyPanel::onFilterChanged(const QString& text)
{
    auto* proxy = qobject_cast<QSortFilterProxyModel*>(treeView_->model());
    if (proxy) {
        proxy->setFilterFixedString(text);
        treeView_->expandAll();
    }
}

void TypeHierarchyPanel::onClassClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;

    auto* proxy = qobject_cast<QSortFilterProxyModel*>(treeView_->model());
    QModelIndex srcIndex = proxy ? proxy->mapToSource(index) : index;
    const ClassInfo* cls = hierarchyModel_->classAt(srcIndex);
    if (!cls) return;

    // Update vtable viewer
    vtableModel_->setSlots(cls->vtable);

    // Emit signal for FunctionListPanel filtering
    emit classSelected(cls->name);
}

void TypeHierarchyPanel::onVtableSlotDoubleClicked(const QModelIndex& index)
{
    const VtableSlot* slot = vtableModel_->slotAt(index.row());
    if (!slot || slot->funcAddress == 0) return;
    emit vtableSlotNavigated(slot->funcAddress);
    emit addressNavigated(slot->funcAddress);
}

} // namespace retdec::gui::panels
