/**
 * @file include/retdec/gui/panels/function_list_panel.h
 * @brief Sortable/filterable function roster with recovery metadata (Task 53).
 *
 * Columns
 * -------
 *   Address | Name | Size | Instrs | Confidence | Patterns | CC | Notes
 *
 * Features
 * --------
 *  - FunctionEntry        : full metadata struct including confidence scores
 *  - RecoveryConfidence   : per-category scores (type/structure/variable/algorithm)
 *  - PatternFlags         : STL / crypto / algorithm / design-pattern / library
 *  - FunctionListModel    : QAbstractTableModel driving the view
 *  - FunctionPatternDelegate : renders small colour-coded pattern badges
 *  - FunctionFilterProxy  : multi-criterion filtering (name glob, address range,
 *                           confidence threshold, pattern checkboxes)
 *  - FunctionListPanel    : full panel with filter bar, inline rename, context menu,
 *                           CSV/JSON export, batch "Add Tag" annotation
 *
 * Signals
 * -------
 *  functionSelected(address, name)       – double-click / enter
 *  functionRenamed(address, oldName, newName) – after inline rename
 */

#ifndef RETDEC_GUI_PANELS_FUNCTION_LIST_H
#define RETDEC_GUI_PANELS_FUNCTION_LIST_H

#include "retdec/gui/panels/panel_base.h"

#include <QAbstractTableModel>
#include <QHBoxLayout>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>

#include <cstdint>
#include <vector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
QT_END_NAMESPACE

namespace retdec::gui::panels {

// ─── Data model ───────────────────────────────────────────────────────────────

/** Composite confidence score from four recovery categories. */
struct RecoveryConfidence {
    float typeInference = 0.f;  ///< type inference quality (0–1)
    float structure     = 0.f;  ///< control-flow structuring quality (0–1)
    float variable      = 0.f;  ///< variable recovery quality (0–1)
    float algorithm     = 0.f;  ///< algorithm/pattern recognition quality (0–1)

    /** Weighted composite: 30% type, 30% struct, 20% var, 20% algo */
    float composite() const noexcept {
        return typeInference * 0.30f + structure * 0.30f
             + variable      * 0.20f + algorithm * 0.20f;
    }
};

/** Detected semantic patterns for a function. */
struct PatternFlags {
    bool stl     = false;  ///< STL algorithm / container detected
    bool crypto  = false;  ///< cryptographic primitive detected
    bool algo    = false;  ///< generic well-known algorithm (sort, hash, …)
    bool design  = false;  ///< OO design pattern (singleton, factory, …)
    bool library = false;  ///< statically linked library code
};

/** All information about one recovered function. */
struct FunctionEntry {
    uint64_t           address     = 0;
    QString            name;               ///< demangled name (if available)
    QString            rawName;            ///< mangled / raw symbol name
    QString            signature;          ///< full C/C++ signature
    int                sizeBytes   = 0;
    int                instrCount  = 0;
    QString            cc;                 ///< calling convention string
    PatternFlags       patterns;
    RecoveryConfidence confidence;
    QStringList        tags;               ///< user-applied annotation tags
    QString            notes;
    bool               isLibrary   = false;
};

// ─── FunctionListModel ────────────────────────────────────────────────────────

/**
 * @brief QAbstractTableModel with 8 columns of function metadata.
 *
 * Supports in-place rename via Qt::EditRole on column 1 (Name).
 */
class FunctionListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColAddress    = 0,
        ColName       = 1,
        ColSize       = 2,
        ColInstrs     = 3,
        ColConfidence = 4,
        ColPatterns   = 5,
        ColCC         = 6,
        ColNotes      = 7,
        ColCount      = 8,
    };

    explicit FunctionListModel(QObject* parent = nullptr);

    int      rowCount(const QModelIndex& parent = QModelIndex{}) const override;
    int      columnCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex&, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role = Qt::DisplayRole) const override;
    bool     setData(const QModelIndex&, const QVariant&, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex&) const override;

    void setFunctions(std::vector<FunctionEntry> fns);
    const FunctionEntry& entry(int row) const;
    FunctionEntry&       entry(int row);

    /** Rename by address; returns true if found. */
    bool renameFunction(uint64_t address, const QString& newName);

    /** Apply a tag to a set of addresses. */
    void applyTag(const std::vector<uint64_t>& addresses, const QString& tag);

    void clearAll();

    const std::vector<FunctionEntry>& functions() const { return fns_; }

signals:
    void functionRenamed(uint64_t address, const QString& oldName, const QString& newName);

private:
    std::vector<FunctionEntry> fns_;
};

// ─── FunctionPatternDelegate ──────────────────────────────────────────────────

/**
 * @brief Custom delegate that renders small colour-coded pattern badges
 *        and a confidence progress bar.
 *
 * Column 4 (Confidence): horizontal progress bar coloured green→amber→red.
 * Column 5 (Patterns):   row of 5 small icon squares (STL/Crypto/Algo/Design/Lib).
 */
class FunctionPatternDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit FunctionPatternDelegate(FunctionListModel* model,
                                     QObject* parent = nullptr);

    void paint(QPainter*, const QStyleOptionViewItem&,
               const QModelIndex&) const override;
    QSize sizeHint(const QStyleOptionViewItem&,
                   const QModelIndex&) const override;

private:
    void paintConfidence(QPainter*, const QStyleOptionViewItem&,
                         float confidence) const;
    void paintPatterns(QPainter*, const QStyleOptionViewItem&,
                       const PatternFlags&) const;

    FunctionListModel* model_ = nullptr;
};

// ─── FunctionFilterProxy ──────────────────────────────────────────────────────

/**
 * @brief Multi-criterion filter proxy.
 *
 * Accepts rows where ALL active criteria are satisfied:
 *  - name glob (empty = pass all)
 *  - address ≥ addrLow  (0 = no lower bound)
 *  - address ≤ addrHigh (0 = no upper bound)
 *  - confidence ≥ minConfidence
 *  - pattern matches at least one enabled category (if any enabled)
 *  - class filter: name must start with className_ + "::"
 */
class FunctionFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit FunctionFilterProxy(FunctionListModel* model,
                                 QObject* parent = nullptr);

    void setNameGlob(const QString& glob);
    void setAddressRange(uint64_t low, uint64_t high);
    void setMinConfidence(float minConf);
    void setPatternFilter(bool stl, bool crypto, bool algo, bool design, bool lib);
    void setClassFilter(const QString& className);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    FunctionListModel* model_         = nullptr;
    QString            nameGlob_;
    uint64_t           addrLow_       = 0;
    uint64_t           addrHigh_      = 0;  // 0 = no bound
    float              minConfidence_ = 0.f;
    bool               filterStl_    = false;
    bool               filterCrypto_ = false;
    bool               filterAlgo_   = false;
    bool               filterDesign_ = false;
    bool               filterLib_    = false;
    bool               anyPatternActive_ = false;
    QString            classFilter_;
};

// ─── FunctionListPanel ────────────────────────────────────────────────────────

/**
 * @brief Full function list panel widget (Task 53).
 *
 * Layout:
 *   [Filter bar: name | address range | confidence | pattern checkboxes]
 *   [QTableView — sortable, inline-rename on col 1, multi-select]
 *   [Status bar: N functions | M visible]
 */
class FunctionListPanel : public PanelBase {
    Q_OBJECT
public:
    explicit FunctionListPanel(QWidget* parent = nullptr);

    void setFunctions(std::vector<FunctionEntry> fns);
    void clear() override;

    /** Filter list to only functions belonging to className (from TypeHierarchyPanel). */
    void filterByClass(const QString& className);

signals:
    void functionSelected(uint64_t address, const QString& name);
    void functionRenamed(uint64_t address,
                         const QString& oldName, const QString& newName);

public slots:
    void onFunctionSelected(uint64_t address, const QString& name);

private slots:
    void onFilterChanged();
    void onDoubleClicked(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);
    void onExportCsv();
    void onExportJson();
    void onBatchTag();
    void onRenameFunction();
    void updateStatusLabel();

private:
    void setupUI();
    void setupFilterBar(QWidget* parent, QHBoxLayout* layout);
    std::vector<uint64_t> selectedAddresses() const;

    // Filter bar widgets
    QLineEdit*      nameFilter_     = nullptr;
    QLineEdit*      addrLow_        = nullptr;
    QLineEdit*      addrHigh_       = nullptr;
    QDoubleSpinBox* confSpin_       = nullptr;
    QCheckBox*      ckStl_          = nullptr;
    QCheckBox*      ckCrypto_       = nullptr;
    QCheckBox*      ckAlgo_         = nullptr;
    QCheckBox*      ckDesign_       = nullptr;
    QCheckBox*      ckLib_          = nullptr;

    // Toolbar
    QPushButton*    csvButton_      = nullptr;
    QPushButton*    jsonButton_     = nullptr;

    // Table
    QTableView*            tableView_  = nullptr;
    FunctionListModel*     model_      = nullptr;
    FunctionFilterProxy*   proxy_      = nullptr;
    FunctionPatternDelegate* delegate_ = nullptr;

    // Status
    QLabel*         statusLabel_    = nullptr;
};

} // namespace retdec::gui::panels
#endif // RETDEC_GUI_PANELS_FUNCTION_LIST_H
