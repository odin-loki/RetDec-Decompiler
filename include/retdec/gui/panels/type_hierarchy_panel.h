/**
 * @file include/retdec/gui/panels/type_hierarchy_panel.h
 * @brief Type hierarchy browser with vtable viewer (Task 52).
 *
 * Layout: QSplitter — class hierarchy tree (left) | vtable viewer (right).
 *
 * Features
 * --------
 *  - ClassHierarchyModel  : QAbstractItemModel driving a QTreeView
 *  - Inheritance type display : public / protected / private / virtual
 *    shown via colour-coded badges in column 1
 *  - VtableModel          : QAbstractTableModel (slot, name, address, pure?)
 *  - VtableViewer         : sub-panel that updates when a class is selected
 *  - Click class          → emit classSelected(name) for FunctionListPanel filter
 *  - Click vtable slot    → emit addressNavigated(address) for TriPane navigation
 */

#ifndef RETDEC_GUI_PANELS_TYPE_HIERARCHY_H
#define RETDEC_GUI_PANELS_TYPE_HIERARCHY_H

#include "retdec/gui/panels/panel_base.h"

#include <QAbstractItemModel>
#include <QAbstractTableModel>
#include <QList>
#include <QModelIndex>
#include <QString>
#include <QVariant>
#include <cstdint>
#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QSplitter;
class QTableView;
class QTreeView;
QT_END_NAMESPACE

namespace retdec::gui::panels {

// ─── Data model ───────────────────────────────────────────────────────────────

/** One base-class relationship. */
struct InheritanceLink {
    enum class Kind : uint8_t { Public, Protected, Private, Virtual };
    QString base;
    Kind    kind = Kind::Public;
};

/** One vtable slot. */
struct VtableSlot {
    int      index       = 0;
    QString  funcName;
    uint64_t funcAddress = 0;
    bool     isPure      = false;  ///< pure virtual (= 0)
    bool     isOverride  = false;  ///< overrides a base-class slot
};

/** All recovered information about one class. */
struct ClassInfo {
    QString                   name;
    uint64_t                  vtableAddress = 0;
    QString                   compiler;      ///< "MSVC", "GCC", "Clang", …
    QString                   moduleName;    ///< source module / compilation unit
    QList<InheritanceLink>    bases;
    QList<VtableSlot>         vtable;
    bool                      isAbstract    = false;
    int                       methodCount   = 0;  ///< total recovered methods
};

// ─── ClassHierarchyModel ─────────────────────────────────────────────────────

/**
 * @brief QAbstractItemModel for the class hierarchy tree.
 *
 * Columns: Class Name | Inherits | vtable | Methods | Compiler
 */
class ClassHierarchyModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit ClassHierarchyModel(QObject* parent = nullptr);
    ~ClassHierarchyModel() override;

    void setClasses(const QList<ClassInfo>& classes);
    const ClassInfo* classAt(const QModelIndex& index) const;

    // QAbstractItemModel interface
    QModelIndex   index(int row, int col,
                        const QModelIndex& parent = QModelIndex{}) const override;
    QModelIndex   parent(const QModelIndex& child)       const override;
    int           rowCount(const QModelIndex& parent = QModelIndex{})  const override;
    int           columnCount(const QModelIndex& = {})      const override;
    QVariant      data(const QModelIndex&, int role = Qt::DisplayRole) const override;
    QVariant      headerData(int, Qt::Orientation,
                             int role = Qt::DisplayRole)     const override;

private:
    struct Node {
        const ClassInfo*      info   = nullptr;
        Node*                 parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        int                   row    = 0;   // row within parent
    };

    void buildTree(const QList<ClassInfo>& classes);

    std::vector<std::unique_ptr<Node>> roots_;  // top-level nodes (no parents)
    QList<ClassInfo>                   classes_;
};

// ─── VtableModel ─────────────────────────────────────────────────────────────

/** QAbstractTableModel for vtable slots.  Columns: #, Function, Address, Flags */
class VtableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit VtableModel(QObject* parent = nullptr);

    void setSlots(const QList<VtableSlot>& newSlots);
    const VtableSlot* slotAt(int row) const;

    int     rowCount(const QModelIndex& = {})    const override;
    int     columnCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex&, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation,
                        int role = Qt::DisplayRole) const override;

private:
    QList<VtableSlot> slots_;
};

// ─── TypeHierarchyPanel ──────────────────────────────────────────────────────

class TypeHierarchyPanel : public PanelBase {
    Q_OBJECT
public:
    explicit TypeHierarchyPanel(QWidget* parent = nullptr);

    /** Replace the displayed hierarchy with new class data. */
    void setHierarchy(const QList<ClassInfo>& classes);

    /** Legacy compat overload. */
    struct ClassEntry {
        QString  name;
        uint64_t vtableAddress = 0;
        QString  compiler;
        QStringList parents;
    };
    void setHierarchy(const QList<ClassEntry>& classes);

    void clear() override;

signals:
    /** Emitted when the user selects a class; for FunctionListPanel filtering. */
    void classSelected(const QString& className);

    /** Emitted when the user double-clicks a vtable slot. */
    void vtableSlotNavigated(uint64_t address);

private slots:
    void onFilterChanged(const QString& text);
    void onClassClicked(const QModelIndex& index);
    void onVtableSlotDoubleClicked(const QModelIndex& index);

private:
    void setupUI();

    QLineEdit*             searchBox_      = nullptr;
    QTreeView*             treeView_       = nullptr;
    QTableView*            vtableView_     = nullptr;
    QSplitter*             splitter_       = nullptr;
    ClassHierarchyModel*   hierarchyModel_ = nullptr;
    VtableModel*           vtableModel_    = nullptr;
};

} // namespace retdec::gui::panels
#endif // RETDEC_GUI_PANELS_TYPE_HIERARCHY_H
