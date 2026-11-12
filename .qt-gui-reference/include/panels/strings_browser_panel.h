/**
 * @file include/retdec/gui/panels/strings_browser_panel.h
 * @brief Strings and Constants Browser Panel — Stage 55.
 *
 * Two-tab panel:
 *   Tab 1 — Strings Browser
 *     Columns: Address | Type | Category | Preview (80 chars) | Refs
 *     Category detection:
 *       Plain        — general string literal
 *       FormatString — contains printf-family format specifiers (%d, %s, etc.)
 *       URL          — http/https/ftp scheme prefix
 *       FilePath     — path separators / or \\, drive letter, or /proc /sys
 *       RegEx        — contains common regex metacharacters
 *       CryptoConst  — matches known hash/crypto constant strings
 *     Live regex/text filter across all columns.
 *     Double-click → emits addressNavigated(addr).
 *     Export to CSV.
 *
 *   Tab 2 — Constants Browser
 *     Columns: Address | Size | Hex Value | Dec Value | Semantic Label | Refs
 *     Semantic label detection:
 *       Known magic numbers (MZ, ELF, PNG, ZIP, …)
 *       Crypto round constants (SHA-256 K[], AES Rcon[])
 *       IEEE 754 special values (Inf, NaN, π, e, …)
 *       Port numbers (80, 443, 22, …)
 *     Crypto constants highlighted in amber.
 *     Live text filter.
 *     Double-click → emits addressNavigated(addr).
 *     Export to CSV.
 */

#ifndef RETDEC_GUI_PANELS_STRINGS_BROWSER_H
#define RETDEC_GUI_PANELS_STRINGS_BROWSER_H

#include "retdec/gui/panels/panel_base.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QLabel>

#include <cstdint>
#include <vector>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QComboBox;
class QStatusBar;
QT_END_NAMESPACE

namespace retdec::gui::panels {

// ─── StringEntry ─────────────────────────────────────────────────────────────

enum class StringType {
    ASCII,
    UTF8,
    UTF16LE,
    UTF16BE,
    Wide,       ///< wchar_t (platform-specific width)
    Pascal,     ///< length-prefixed
    Unknown,
};

enum class StringCategory {
    Plain,
    FormatString,
    URL,
    FilePath,
    RegEx,
    CryptoConst,
};

struct StringEntry {
    uint64_t       address  = 0;
    QString        value;
    StringType     type     = StringType::ASCII;
    StringCategory category = StringCategory::Plain;
    int            length   = 0;
    int            refCount = 0;
};

// ─── ConstantEntry ────────────────────────────────────────────────────────────

enum class ConstantSize { Byte=1, Word=2, Dword=4, Qword=8 };

enum class ConstantLabel {
    None,
    MagicNumber,
    CryptoConstant,
    FloatSpecial,
    PortNumber,
    Errno,
    WinError,
    SyscallNr,
    OtherKnown,
};

struct ConstantEntry {
    uint64_t      address  = 0;
    ConstantSize  size     = ConstantSize::Dword;
    uint64_t      value    = 0;
    QString       hexStr;
    QString       decStr;
    QString       label;          ///< human-readable semantic label
    ConstantLabel labelKind = ConstantLabel::None;
    int           refCount  = 0;
};

// ─── CategoryDetector ────────────────────────────────────────────────────────

/**
 * @brief Classifies a string entry into a StringCategory.
 *
 * Called during setStrings() to populate the category field.
 * Also used by the unit tests to verify individual patterns.
 */
class CategoryDetector {
public:
    static StringCategory classify(const QString& value);

    static bool isFormatString(const QString& v);
    static bool isURL(const QString& v);
    static bool isFilePath(const QString& v);
    static bool isRegEx(const QString& v);
    static bool isCryptoConst(const QString& v);
};

// ─── ConstantLabeler ─────────────────────────────────────────────────────────

/**
 * @brief Assigns a semantic label to a numeric constant.
 */
class ConstantLabeler {
public:
    static void label(ConstantEntry& entry);

private:
    static QString lookupMagic(uint64_t val, ConstantSize sz);
    static QString lookupCrypto(uint64_t val);
    static QString lookupFloat(uint64_t val, ConstantSize sz);
    static QString lookupPort(uint64_t val);
};

// ─── StringsModel ────────────────────────────────────────────────────────────

class StringsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit StringsModel(QObject* parent = nullptr);

    int      rowCount   (const QModelIndex& = {}) const override;
    int      columnCount(const QModelIndex& = {}) const override;
    QVariant data       (const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData (int section, Qt::Orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags (const QModelIndex& index) const override;

    void setStrings(std::vector<StringEntry> entries);
    void appendStrings(std::vector<StringEntry> entries);
    const StringEntry& entry(int row) const;
    void clear();

    int totalCount() const;
    QString exportCsv() const;

private:
    std::vector<StringEntry> entries_;
};

// ─── ConstantsModel ──────────────────────────────────────────────────────────

class ConstantsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit ConstantsModel(QObject* parent = nullptr);

    int      rowCount   (const QModelIndex& = {}) const override;
    int      columnCount(const QModelIndex& = {}) const override;
    QVariant data       (const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData (int section, Qt::Orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags (const QModelIndex& index) const override;

    void setConstants(std::vector<ConstantEntry> entries);
    const ConstantEntry& entry(int row) const;
    void clear();

    QString exportCsv() const;

private:
    std::vector<ConstantEntry> entries_;
};

// ─── TypeFilterProxy ─────────────────────────────────────────────────────────

/**
 * @brief Proxy that filters strings by type, category, and free-text/regex.
 */
class TypeFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit TypeFilterProxy(QObject* parent = nullptr);

    void setTypeFilter    (const QString& typeName);   ///< "" = all
    void setCategoryFilter(const QString& catName);    ///< "" = all
    void setTextFilter    (const QString& text, bool useRegex = false);

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& parent) const override;

private:
    QString typeFilter_;
    QString catFilter_;
    QString textFilter_;
    bool    useRegex_   = false;
    mutable QRegularExpression re_;
};

// ─── StringsBrowserPanel ─────────────────────────────────────────────────────

/**
 * @brief Full Strings and Constants Browser with two tabs.
 *
 * Signals:
 *   addressNavigated(uint64_t) — emitted on double-click in either tab.
 */
class StringsBrowserPanel : public PanelBase {
    Q_OBJECT
public:
    explicit StringsBrowserPanel(QWidget* parent = nullptr);

    // ── Data population ─────────────────────────────────────────────────────
    void setStrings  (std::vector<StringEntry>   entries);
    void setConstants(std::vector<ConstantEntry> entries);
    void clear() override;

    // ── Accessors ───────────────────────────────────────────────────────────
    int stringCount()   const;
    int constantCount() const;

signals:
    void addressNavigated(uint64_t address);

private slots:
    void onStringFilterChanged(const QString& text);
    void onConstFilterChanged (const QString& text);
    void onTypeFilterChanged  (int index);
    void onCatFilterChanged   (int index);
    void onRegexToggled       (bool checked);
    void onStringDoubleClicked(const QModelIndex& index);
    void onConstDoubleClicked (const QModelIndex& index);
    void onExportStrings();
    void onExportConstants();

private:
    void setupStringTab (QWidget* tab);
    void setupConstTab  (QWidget* tab);
    void updateStatusBar();

    // ── Strings tab ─────────────────────────────────────────────────────────
    QLineEdit*       strSearchBox_   = nullptr;
    QComboBox*       typeCombo_      = nullptr;
    QComboBox*       catCombo_       = nullptr;
    QToolButton*     regexBtn_       = nullptr;
    QTableView*      strTable_       = nullptr;
    StringsModel*    strModel_       = nullptr;
    TypeFilterProxy* strProxy_       = nullptr;
    QLabel*          strStatusLabel_ = nullptr;

    // ── Constants tab ───────────────────────────────────────────────────────
    QLineEdit*             constSearchBox_  = nullptr;
    QTableView*            constTable_      = nullptr;
    ConstantsModel*        constModel_      = nullptr;
    QSortFilterProxyModel* constProxy_      = nullptr;
    QLabel*                constStatusLabel_= nullptr;

    // ── Tab widget ──────────────────────────────────────────────────────────
    QTabWidget* tabs_ = nullptr;
};

} // namespace retdec::gui::panels

#endif // RETDEC_GUI_PANELS_STRINGS_BROWSER_H
