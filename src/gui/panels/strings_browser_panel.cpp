/**
 * @file src/gui/panels/strings_browser_panel.cpp
 * @brief Strings and Constants Browser Panel implementation.
 */

#include "retdec/gui/panels/strings_browser_panel.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_map>

namespace retdec::gui::panels {

// ─── Colour constants ─────────────────────────────────────────────────────────

static const QColor kColAddr    ("#6c7086");
static const QColor kColCrypto  ("#e6a23c"); // amber
static const QColor kColUrl     ("#89b4fa"); // blue
static const QColor kColFmt     ("#a6e3a1"); // green
static const QColor kColPath    ("#f9e2af"); // yellow
static const QColor kColRegex   ("#cba6f7"); // mauve

// ─── CategoryDetector ─────────────────────────────────────────────────────────

bool CategoryDetector::isFormatString(const QString& v) {
    static const QRegularExpression re(
        R"(%[-+0 #]?\d*(\.\d+)?[diouxXeEfgGaAcsSpn%])",
        QRegularExpression::CaseInsensitiveOption);
    return re.match(v).hasMatch();
}

bool CategoryDetector::isURL(const QString& v) {
    return v.startsWith("http://",  Qt::CaseInsensitive) ||
           v.startsWith("https://", Qt::CaseInsensitive) ||
           v.startsWith("ftp://",   Qt::CaseInsensitive) ||
           v.startsWith("ftps://",  Qt::CaseInsensitive) ||
           v.startsWith("file://",  Qt::CaseInsensitive) ||
           v.startsWith("ws://",    Qt::CaseInsensitive) ||
           v.startsWith("wss://",   Qt::CaseInsensitive);
}

bool CategoryDetector::isFilePath(const QString& v) {
    if (v.isEmpty()) return false;
    // Unix absolute path
    if (v.startsWith('/')) return true;
    // Windows drive letter
    if (v.length() >= 3 && v[1] == ':' &&
        (v[2] == '\\' || v[2] == '/')) return true;
    // UNC path
    if (v.startsWith("\\\\")) return true;
    // Common system prefixes
    static const QStringList prefixes = {
        "/proc/", "/sys/", "/dev/", "/etc/", "/usr/", "/var/",
        "C:\\", "C:/", "%SystemRoot%", "%AppData%", "%TEMP%",
    };
    for (const auto& p : prefixes)
        if (v.startsWith(p, Qt::CaseInsensitive)) return true;
    // Ends with a known extension and contains a separator
    static const QStringList exts = {
        ".exe", ".dll", ".so", ".dylib", ".sys", ".conf", ".cfg", ".ini",
        ".log", ".txt", ".xml", ".json", ".yaml"
    };
    if (v.contains('/') || v.contains('\\')) {
        for (const auto& e : exts)
            if (v.endsWith(e, Qt::CaseInsensitive)) return true;
    }
    return false;
}

bool CategoryDetector::isRegEx(const QString& v) {
    // Contains typical regex metacharacters as operators
    static const QRegularExpression re(
        R"([\\^$.|?*+(){}\[\]])");
    int count = 0;
    auto it = re.globalMatch(v);
    while (it.hasNext()) { it.next(); ++count; }
    return count >= 3;  // at least 3 metacharacters
}

bool CategoryDetector::isCryptoConst(const QString& v) {
    // Known crypto-related string patterns
    static const QStringList cryptoKeywords = {
        "SHA", "MD5", "AES", "RSA", "HMAC", "PBKDF", "BLAKE",
        "ChaCha", "Poly1305", "ECDSA", "secp256k1", "Curve25519",
        "BEGIN CERTIFICATE", "BEGIN RSA", "BEGIN PRIVATE KEY",
        "BEGIN PUBLIC KEY", "BEGIN EC", "BEGIN OPENSSH",
        "iv=", "salt=", "key=",
    };
    for (const auto& kw : cryptoKeywords)
        if (v.contains(kw, Qt::CaseInsensitive)) return true;
    return false;
}

StringCategory CategoryDetector::classify(const QString& value) {
    if (isCryptoConst(value))   return StringCategory::CryptoConst;
    if (isURL(value))           return StringCategory::URL;
    if (isFilePath(value))      return StringCategory::FilePath;
    if (isFormatString(value))  return StringCategory::FormatString;
    if (isRegEx(value))         return StringCategory::RegEx;
    return StringCategory::Plain;
}

// ─── ConstantLabeler ──────────────────────────────────────────────────────────

// Known magic numbers: value → label string
static const std::unordered_map<uint32_t, const char*> kMagic32 = {
    {0x5A4D,     "MZ (PE)"},
    {0x4D5A,     "ZM (BE PE)"},
    {0x7F454C46, "ELF magic"},
    {0xFEEDFACE, "Mach-O 32-bit"},
    {0xCEFAEDFE, "Mach-O 32-bit (LE)"},
    {0xFEEDFACF, "Mach-O 64-bit"},
    {0xCFFAEDFE, "Mach-O 64-bit (LE)"},
    {0x89504E47, "PNG magic"},
    {0xFFD8FF,   "JPEG magic"},
    {0x47494638, "GIF magic"},
    {0x504B0304, "ZIP magic"},
    {0x1F8B08,   "gzip magic"},
    {0xFD377A58, "xz magic"},
    {0x52494646, "RIFF (WAV/AVI)"},
    {0x424D,     "BMP magic"},
    {0x25504446, "PDF magic"},
    {0xD0CF11E0, "MS Compound Doc"},
    {0x377ABCAF, "7-Zip magic"},
    {0xCAFEBABE, "Java class / Mach-O fat"},
    {0xBEBAFECA, "Mach-O fat (LE)"},
    {0xDEADBEEF, "DEADBEEF"},
    {0xDEADC0DE, "DEADCODE"},
    {0xBADDCAFE, "BADDCAFE"},
    {0x0BADF00D, "BADFOOD"},
    {0xCCCCCCCC, "MSVC uninit stack"},
    {0xCDCDCDCD, "MSVC uninit heap"},
    {0xFEEEFEEE, "MSVC free heap"},
    {0x8BADF00D, "iOS watchdog"},
    {0xDEADA55,  "DEAD A55"},
};

// SHA-256 round constants (first 10)
static const std::unordered_map<uint32_t, const char*> kCryptoConst32 = {
    {0x428a2f98, "SHA256_K[0]"},
    {0x71374491, "SHA256_K[1]"},
    {0xb5c0fbcf, "SHA256_K[2]"},
    {0xe9b5dba5, "SHA256_K[3]"},
    {0x3956c25b, "SHA256_K[4]"},
    {0x59f111f1, "SHA256_K[5]"},
    {0x923f82a4, "SHA256_K[6]"},
    {0xab1c5ed5, "SHA256_K[7]"},
    {0xd807aa98, "SHA256_K[8]"},
    {0x12835b01, "SHA256_K[9]"},
    // AES Rcon
    {0x01000000, "AES Rcon[1]"},
    {0x02000000, "AES Rcon[2]"},
    {0x04000000, "AES Rcon[3]"},
    {0x08000000, "AES Rcon[4]"},
    {0x10000000, "AES Rcon[5]"},
    {0x1B000000, "AES Rcon[6]"},
    {0x36000000, "AES Rcon[7]"},
    // CRC32 polynomial
    {0xEDB88320, "CRC32 poly (LE)"},
    {0x04C11DB7, "CRC32 poly (BE)"},
    // MD5 constants
    {0xd76aa478, "MD5_T[1]"},
    {0xe8c7b756, "MD5_T[2]"},
    // DJB2 magic
    {5381,       "DJB2 hash seed"},
};

// IEEE 754 special bit patterns
static const std::unordered_map<uint32_t, const char*> kFloat32 = {
    {0x3F800000, "1.0f"},
    {0x40000000, "2.0f"},
    {0x3F000000, "0.5f"},
    {0x40490FDB, "π (3.14159…)"},
    {0x402DF854, "e (2.71828…)"},
    {0x3FB504F3, "√2/2"},
    {0x3F3504F3, "√2-1"},
    {0x7F800000, "+Inf"},
    {0xFF800000, "-Inf"},
    {0x7FC00000, "NaN"},
    {0x00000000, "0.0f"},
    {0x80000000, "-0.0f"},
};

QString ConstantLabeler::lookupMagic(uint64_t val, ConstantSize sz) {
    if (sz == ConstantSize::Dword || sz == ConstantSize::Word) {
        auto it = kMagic32.find(static_cast<uint32_t>(val));
        if (it != kMagic32.end()) return it->second;
    }
    return {};
}

QString ConstantLabeler::lookupCrypto(uint64_t val) {
    auto it32 = kCryptoConst32.find(static_cast<uint32_t>(val));
    if (it32 != kCryptoConst32.end()) return it32->second;
    return {};
}

QString ConstantLabeler::lookupFloat(uint64_t val, ConstantSize sz) {
    if (sz == ConstantSize::Dword) {
        auto it = kFloat32.find(static_cast<uint32_t>(val));
        if (it != kFloat32.end()) return it->second;
    }
    return {};
}

QString ConstantLabeler::lookupPort(uint64_t val) {
    static const std::unordered_map<uint32_t, const char*> ports = {
        {21,   "FTP"}, {22, "SSH"}, {23, "Telnet"}, {25, "SMTP"},
        {53,   "DNS"}, {80, "HTTP"}, {110, "POP3"}, {143, "IMAP"},
        {443,  "HTTPS"}, {465, "SMTPS"}, {993, "IMAPS"}, {995, "POP3S"},
        {3306, "MySQL"}, {5432, "PostgreSQL"}, {6379, "Redis"},
        {8080, "HTTP-Alt"}, {8443, "HTTPS-Alt"}, {27017, "MongoDB"},
        {1433, "MSSQL"}, {1521, "Oracle"},
    };
    auto it = ports.find(static_cast<uint32_t>(val));
    if (it != ports.end()) return it->second;
    return {};
}

void ConstantLabeler::label(ConstantEntry& e) {
    // hexStr is the bare hex (e.g. "5A4D"). Callers / UI add the "0x" prefix
    // when they want to render it. The earlier implementation embedded "0X"
    // which made round-trip equality with the canonical hex string fail
    // and confused downstream regex matches on constant labels.
    e.hexStr = QString("%1").arg(e.value,
                                 static_cast<int>(e.size) * 2,
                                 16, QChar('0')).toUpper();
    e.decStr = QString::number(e.value);

    QString lbl;
    // Try each category in priority order
    lbl = lookupMagic(e.value, e.size);
    if (!lbl.isEmpty()) {
        e.label = lbl; e.labelKind = ConstantLabel::MagicNumber; return;
    }
    lbl = lookupCrypto(e.value);
    if (!lbl.isEmpty()) {
        e.label = lbl; e.labelKind = ConstantLabel::CryptoConstant; return;
    }
    lbl = lookupFloat(e.value, e.size);
    if (!lbl.isEmpty()) {
        e.label = lbl; e.labelKind = ConstantLabel::FloatSpecial; return;
    }
    if (e.value > 0 && e.value <= 65535) {
        lbl = lookupPort(e.value);
        if (!lbl.isEmpty()) {
            e.label = lbl; e.labelKind = ConstantLabel::PortNumber; return;
        }
    }
    e.label     = {};
    e.labelKind = ConstantLabel::None;
}

// ─── StringsModel ─────────────────────────────────────────────────────────────

static const char* typeStr(StringType t) {
    switch (t) {
    case StringType::ASCII:   return "ASCII";
    case StringType::UTF8:    return "UTF-8";
    case StringType::UTF16LE: return "UTF-16 LE";
    case StringType::UTF16BE: return "UTF-16 BE";
    case StringType::Wide:    return "Wide";
    case StringType::Pascal:  return "Pascal";
    default:                  return "?";
    }
}

static const char* catStr(StringCategory c) {
    switch (c) {
    case StringCategory::Plain:       return "Plain";
    case StringCategory::FormatString:return "FormatStr";
    case StringCategory::URL:         return "URL";
    case StringCategory::FilePath:    return "FilePath";
    case StringCategory::RegEx:       return "RegEx";
    case StringCategory::CryptoConst: return "Crypto";
    default:                          return "?";
    }
}

StringsModel::StringsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int StringsModel::rowCount(const QModelIndex&) const {
    return static_cast<int>(entries_.size());
}

int StringsModel::columnCount(const QModelIndex&) const { return 5; }

QVariant StringsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const auto& e = entries_[static_cast<size_t>(index.row())];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return QString("0x%1").arg(e.address, 16, 16, QChar('0'));
        case 1: return typeStr(e.type);
        case 2: return catStr(e.category);
        case 3: return e.value.left(80);
        case 4: return e.refCount;
        }
    }

    if (role == Qt::ForegroundRole) {
        if (index.column() == 0) return kColAddr;
        if (index.column() == 3) {
            switch (e.category) {
            case StringCategory::CryptoConst:  return kColCrypto;
            case StringCategory::URL:          return kColUrl;
            case StringCategory::FormatString: return kColFmt;
            case StringCategory::FilePath:     return kColPath;
            case StringCategory::RegEx:        return kColRegex;
            default: break;
            }
        }
    }

    if (role == Qt::ToolTipRole && index.column() == 3)
        return e.value;

    if (role == Qt::UserRole) {
        // Used for sorting / filtering raw data
        switch (index.column()) {
        case 0: return static_cast<quint64>(e.address);
        case 4: return e.refCount;
        default: return data(index, Qt::DisplayRole);
        }
    }

    return {};
}

QVariant StringsModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return "Address";
    case 1: return "Type";
    case 2: return "Category";
    case 3: return "Preview";
    case 4: return "Refs";
    }
    return {};
}

Qt::ItemFlags StringsModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void StringsModel::setStrings(std::vector<StringEntry> entries) {
    beginResetModel();
    // Auto-classify any entries that have the default category
    for (auto& e : entries)
        if (e.category == StringCategory::Plain)
            e.category = CategoryDetector::classify(e.value);
    entries_ = std::move(entries);
    endResetModel();
}

void StringsModel::appendStrings(std::vector<StringEntry> extra) {
    if (extra.empty()) return;
    const int first = static_cast<int>(entries_.size());
    for (auto& e : extra)
        if (e.category == StringCategory::Plain)
            e.category = CategoryDetector::classify(e.value);
    beginInsertRows({}, first, first + static_cast<int>(extra.size()) - 1);
    for (auto& e : extra)
        entries_.push_back(std::move(e));
    endInsertRows();
}

const StringEntry& StringsModel::entry(int row) const {
    return entries_[static_cast<size_t>(row)];
}

void StringsModel::clear() {
    beginResetModel();
    entries_.clear();
    endResetModel();
}

int StringsModel::totalCount() const {
    return static_cast<int>(entries_.size());
}

QString StringsModel::exportCsv() const {
    QString csv = "Address,Type,Category,Value,Refs\n";
    for (const auto& e : entries_) {
        QString val = e.value;
        val.replace('"', "\"\"");
        csv += QString("0x%1,%2,%3,\"%4\",%5\n")
               .arg(e.address, 16, 16, QChar('0'))
               .arg(typeStr(e.type))
               .arg(catStr(e.category))
               .arg(val)
               .arg(e.refCount);
    }
    return csv;
}

// ─── ConstantsModel ───────────────────────────────────────────────────────────

ConstantsModel::ConstantsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int ConstantsModel::rowCount(const QModelIndex&) const {
    return static_cast<int>(entries_.size());
}

int ConstantsModel::columnCount(const QModelIndex&) const { return 6; }

QVariant ConstantsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const auto& e = entries_[static_cast<size_t>(index.row())];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return QString("0x%1").arg(e.address, 16, 16, QChar('0'));
        case 1: return static_cast<int>(e.size);
        case 2: return e.hexStr;
        case 3: return e.decStr;
        case 4: return e.label;
        case 5: return e.refCount;
        }
    }

    if (role == Qt::ForegroundRole) {
        if (index.column() == 0) return kColAddr;
        if (e.labelKind == ConstantLabel::CryptoConstant) return kColCrypto;
        if (e.labelKind == ConstantLabel::MagicNumber)    return kColUrl;
        if (e.labelKind == ConstantLabel::FloatSpecial)   return kColFmt;
    }

    if (role == Qt::ToolTipRole && index.column() == 4 && !e.label.isEmpty())
        return QString("%1 (0x%2)").arg(e.label).arg(e.value, 0, 16);

    if (role == Qt::UserRole) {
        switch (index.column()) {
        case 0: return static_cast<quint64>(e.address);
        case 1: return static_cast<int>(e.size);
        case 5: return e.refCount;
        default: return data(index, Qt::DisplayRole);
        }
    }

    return {};
}

QVariant ConstantsModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return "Address";
    case 1: return "Size";
    case 2: return "Hex";
    case 3: return "Decimal";
    case 4: return "Label";
    case 5: return "Refs";
    }
    return {};
}

Qt::ItemFlags ConstantsModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void ConstantsModel::setConstants(std::vector<ConstantEntry> entries) {
    beginResetModel();
    for (auto& e : entries)
        ConstantLabeler::label(e);
    entries_ = std::move(entries);
    endResetModel();
}

const ConstantEntry& ConstantsModel::entry(int row) const {
    return entries_[static_cast<size_t>(row)];
}

void ConstantsModel::clear() {
    beginResetModel();
    entries_.clear();
    endResetModel();
}

QString ConstantsModel::exportCsv() const {
    QString csv = "Address,Size,Hex,Decimal,Label,Refs\n";
    for (const auto& e : entries_) {
        csv += QString("0x%1,%2,%3,%4,\"%5\",%6\n")
               .arg(e.address, 16, 16, QChar('0'))
               .arg(static_cast<int>(e.size))
               .arg(e.hexStr)
               .arg(e.decStr)
               .arg(e.label)
               .arg(e.refCount);
    }
    return csv;
}

// ─── TypeFilterProxy ──────────────────────────────────────────────────────────

TypeFilterProxy::TypeFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent) {
    setFilterCaseSensitivity(Qt::CaseInsensitive);
}

void TypeFilterProxy::setTypeFilter(const QString& t) {
    typeFilter_ = t;
    invalidateFilter();
}

void TypeFilterProxy::setCategoryFilter(const QString& c) {
    catFilter_ = c;
    invalidateFilter();
}

void TypeFilterProxy::setTextFilter(const QString& text, bool useRegex) {
    textFilter_ = text;
    useRegex_   = useRegex;
    if (useRegex_)
        re_ = QRegularExpression(text, QRegularExpression::CaseInsensitiveOption);
    invalidateFilter();
}

bool TypeFilterProxy::filterAcceptsRow(int source_row, const QModelIndex& parent) const {
    auto* src = sourceModel();
    if (!src) return true;

    // Type filter (column 1)
    if (!typeFilter_.isEmpty()) {
        QModelIndex idx = src->index(source_row, 1, parent);
        if (!src->data(idx).toString().contains(typeFilter_, Qt::CaseInsensitive))
            return false;
    }

    // Category filter (column 2)
    if (!catFilter_.isEmpty()) {
        QModelIndex idx = src->index(source_row, 2, parent);
        if (!src->data(idx).toString().contains(catFilter_, Qt::CaseInsensitive))
            return false;
    }

    // Text filter (search only the value/preview column — column 3 in
    // StringsModel / ConstantsModel). Searching every column accidentally
    // matched address fields (which always contain digits), making any
    // regex like "\d+" match every row.
    if (!textFilter_.isEmpty()) {
        const int textCol = 3;
        const QString cell = src->data(src->index(source_row, textCol, parent)).toString();
        bool matched = false;
        if (useRegex_) {
            if (re_.isValid() && cell.contains(re_)) matched = true;
        } else {
            if (cell.contains(textFilter_, Qt::CaseInsensitive)) matched = true;
        }
        if (!matched) return false;
    }

    return true;
}

// ─── StringsBrowserPanel ──────────────────────────────────────────────────────

StringsBrowserPanel::StringsBrowserPanel(QWidget* parent)
    : PanelBase("Strings & Constants", parent) {
    tabs_ = new QTabWidget(this);

    auto* strTab   = new QWidget(this);
    auto* constTab = new QWidget(this);
    setupStringTab(strTab);
    setupConstTab(constTab);

    tabs_->addTab(strTab,   "Strings");
    tabs_->addTab(constTab, "Constants");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs_);
}

void StringsBrowserPanel::setupStringTab(QWidget* tab) {
    // ── Toolbar ──────────────────────────────────────────────────────────────
    auto* toolbar = new QToolBar(tab);
    toolbar->setIconSize({16, 16});

    strSearchBox_ = new QLineEdit(tab);
    strSearchBox_->setPlaceholderText("Filter strings…  (text or /regex/)");
    strSearchBox_->setClearButtonEnabled(true);
    strSearchBox_->setMinimumWidth(220);
    toolbar->addWidget(strSearchBox_);

    toolbar->addSeparator();

    typeCombo_ = new QComboBox(tab);
    typeCombo_->addItems({"All Types","ASCII","UTF-8","UTF-16 LE","UTF-16 BE","Wide","Pascal"});
    typeCombo_->setFixedWidth(110);
    toolbar->addWidget(new QLabel("Type: ", tab));
    toolbar->addWidget(typeCombo_);

    toolbar->addSeparator();

    catCombo_ = new QComboBox(tab);
    catCombo_->addItems({"All Cats","Plain","FormatStr","URL","FilePath","RegEx","Crypto"});
    catCombo_->setFixedWidth(110);
    toolbar->addWidget(new QLabel("Cat: ", tab));
    toolbar->addWidget(catCombo_);

    toolbar->addSeparator();

    regexBtn_ = new QToolButton(tab);
    regexBtn_->setText(".*");
    regexBtn_->setToolTip("Enable regex search");
    regexBtn_->setCheckable(true);
    toolbar->addWidget(regexBtn_);

    toolbar->addSeparator();

    auto* exportBtn = new QToolButton(tab);
    exportBtn->setText("CSV");
    exportBtn->setToolTip("Export strings to CSV");
    toolbar->addWidget(exportBtn);

    // ── Model + proxy ────────────────────────────────────────────────────────
    strModel_ = new StringsModel(tab);
    strProxy_ = new TypeFilterProxy(tab);
    strProxy_->setSourceModel(strModel_);
    strProxy_->setSortRole(Qt::UserRole);

    strTable_ = new QTableView(tab);
    strTable_->setModel(strProxy_);
    strTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    strTable_->setColumnWidth(0, 130);
    strTable_->setColumnWidth(1, 80);
    strTable_->setColumnWidth(2, 80);
    strTable_->setColumnWidth(4, 50);
    strTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    strTable_->setAlternatingRowColors(true);
    strTable_->setSortingEnabled(true);
    strTable_->verticalHeader()->hide();
    strTable_->setContextMenuPolicy(Qt::ActionsContextMenu);

    // Copy address action
    auto* copyAddrAction = new QAction("Copy Address", strTable_);
    strTable_->addAction(copyAddrAction);
    connect(copyAddrAction, &QAction::triggered, this, [this] {
        auto idx = strTable_->currentIndex();
        if (!idx.isValid()) return;
        QModelIndex src = strProxy_->mapToSource(idx);
        if (!src.isValid()) return;
        QString addr = QString("0x%1")
            .arg(strModel_->entry(src.row()).address, 16, 16, QChar('0'));
        QGuiApplication::clipboard()->setText(addr);
    });

    // Copy value action
    auto* copyValAction = new QAction("Copy Value", strTable_);
    strTable_->addAction(copyValAction);
    connect(copyValAction, &QAction::triggered, this, [this] {
        auto idx = strTable_->currentIndex();
        if (!idx.isValid()) return;
        QModelIndex src = strProxy_->mapToSource(idx);
        if (!src.isValid()) return;
        QGuiApplication::clipboard()->setText(strModel_->entry(src.row()).value);
    });

    // ── Status ───────────────────────────────────────────────────────────────
    strStatusLabel_ = new QLabel("0 strings", tab);
    strStatusLabel_->setAlignment(Qt::AlignRight);

    // ── Layout ───────────────────────────────────────────────────────────────
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(4, 4, 4, 2);
    layout->setSpacing(4);
    layout->addWidget(toolbar);
    layout->addWidget(strTable_);
    layout->addWidget(strStatusLabel_);

    // ── Connections ──────────────────────────────────────────────────────────
    connect(strSearchBox_, &QLineEdit::textChanged,
            this, &StringsBrowserPanel::onStringFilterChanged);
    connect(typeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &StringsBrowserPanel::onTypeFilterChanged);
    connect(catCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &StringsBrowserPanel::onCatFilterChanged);
    connect(regexBtn_, &QToolButton::toggled,
            this, &StringsBrowserPanel::onRegexToggled);
    connect(strTable_, &QTableView::doubleClicked,
            this, &StringsBrowserPanel::onStringDoubleClicked);
    connect(exportBtn, &QToolButton::clicked,
            this, &StringsBrowserPanel::onExportStrings);
    connect(strProxy_, &QSortFilterProxyModel::rowsInserted,
            this, &StringsBrowserPanel::updateStatusBar);
    connect(strProxy_, &QSortFilterProxyModel::rowsRemoved,
            this, &StringsBrowserPanel::updateStatusBar);
    connect(strProxy_, &QSortFilterProxyModel::modelReset,
            this, &StringsBrowserPanel::updateStatusBar);
}

void StringsBrowserPanel::setupConstTab(QWidget* tab) {
    auto* toolbar = new QToolBar(tab);
    toolbar->setIconSize({16, 16});

    constSearchBox_ = new QLineEdit(tab);
    constSearchBox_->setPlaceholderText("Filter constants…");
    constSearchBox_->setClearButtonEnabled(true);
    constSearchBox_->setMinimumWidth(200);
    toolbar->addWidget(constSearchBox_);

    auto* exportBtn = new QToolButton(tab);
    exportBtn->setText("CSV");
    exportBtn->setToolTip("Export constants to CSV");
    toolbar->addWidget(exportBtn);

    constModel_ = new ConstantsModel(tab);
    constProxy_ = new QSortFilterProxyModel(tab);
    constProxy_->setSourceModel(constModel_);
    constProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    constProxy_->setFilterKeyColumn(-1);  // all columns

    constTable_ = new QTableView(tab);
    constTable_->setModel(constProxy_);
    constTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    constTable_->setColumnWidth(0, 130);
    constTable_->setColumnWidth(1, 50);
    constTable_->setColumnWidth(2, 100);
    constTable_->setColumnWidth(3, 100);
    constTable_->setColumnWidth(5, 50);
    constTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    constTable_->setAlternatingRowColors(true);
    constTable_->setSortingEnabled(true);
    constTable_->verticalHeader()->hide();

    constStatusLabel_ = new QLabel("0 constants", tab);
    constStatusLabel_->setAlignment(Qt::AlignRight);

    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(4, 4, 4, 2);
    layout->setSpacing(4);
    layout->addWidget(toolbar);
    layout->addWidget(constTable_);
    layout->addWidget(constStatusLabel_);

    connect(constSearchBox_, &QLineEdit::textChanged,
            this, &StringsBrowserPanel::onConstFilterChanged);
    connect(constTable_, &QTableView::doubleClicked,
            this, &StringsBrowserPanel::onConstDoubleClicked);
    connect(exportBtn, &QToolButton::clicked,
            this, &StringsBrowserPanel::onExportConstants);
    connect(constProxy_, &QSortFilterProxyModel::modelReset,
            this, &StringsBrowserPanel::updateStatusBar);
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void StringsBrowserPanel::onStringFilterChanged(const QString& text) {
    QString effective = text;
    bool useRegex = false;
    // Support /pattern/ syntax for regex
    if (text.startsWith('/') && text.endsWith('/') && text.size() > 1) {
        effective = text.mid(1, text.size() - 2);
        useRegex = true;
    }
    strProxy_->setTextFilter(effective, useRegex || (regexBtn_ && regexBtn_->isChecked()));
    updateStatusBar();
}

void StringsBrowserPanel::onConstFilterChanged(const QString& text) {
    constProxy_->setFilterFixedString(text);
    updateStatusBar();
}

void StringsBrowserPanel::onTypeFilterChanged(int index) {
    strProxy_->setTypeFilter(index == 0 ? "" : typeCombo_->itemText(index));
    updateStatusBar();
}

void StringsBrowserPanel::onCatFilterChanged(int index) {
    strProxy_->setCategoryFilter(index == 0 ? "" : catCombo_->itemText(index));
    updateStatusBar();
}

void StringsBrowserPanel::onRegexToggled(bool checked) {
    if (strSearchBox_)
        onStringFilterChanged(strSearchBox_->text());
    (void)checked;
}

void StringsBrowserPanel::onStringDoubleClicked(const QModelIndex& index) {
    QModelIndex src = strProxy_->mapToSource(index);
    if (!src.isValid()) return;
    emit addressNavigated(strModel_->entry(src.row()).address);
}

void StringsBrowserPanel::onConstDoubleClicked(const QModelIndex& index) {
    QModelIndex src = constProxy_->mapToSource(index);
    if (!src.isValid()) return;
    emit addressNavigated(constModel_->entry(src.row()).address);
}

void StringsBrowserPanel::onExportStrings() {
    QString path = QFileDialog::getSaveFileName(
        this, "Export Strings", "strings.csv", "CSV files (*.csv)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed",
                             "Could not open file for writing.");
        return;
    }
    f.write(strModel_->exportCsv().toUtf8());
}

void StringsBrowserPanel::onExportConstants() {
    QString path = QFileDialog::getSaveFileName(
        this, "Export Constants", "constants.csv", "CSV files (*.csv)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed",
                             "Could not open file for writing.");
        return;
    }
    f.write(constModel_->exportCsv().toUtf8());
}

void StringsBrowserPanel::updateStatusBar() {
    if (strStatusLabel_)
        strStatusLabel_->setText(
            QString("%1 / %2 strings")
            .arg(strProxy_->rowCount())
            .arg(strModel_->totalCount()));
    if (constStatusLabel_)
        constStatusLabel_->setText(
            QString("%1 / %2 constants")
            .arg(constProxy_->rowCount())
            .arg(constModel_->rowCount({})));
}

// ── Public API ────────────────────────────────────────────────────────────────

void StringsBrowserPanel::setStrings(std::vector<StringEntry> entries) {
    strModel_->setStrings(std::move(entries));
    updateStatusBar();
}

void StringsBrowserPanel::setConstants(std::vector<ConstantEntry> entries) {
    constModel_->setConstants(std::move(entries));
    updateStatusBar();
}

void StringsBrowserPanel::clear() {
    strModel_->clear();
    constModel_->clear();
    if (strSearchBox_)   strSearchBox_->clear();
    if (constSearchBox_) constSearchBox_->clear();
    if (typeCombo_)      typeCombo_->setCurrentIndex(0);
    if (catCombo_)       catCombo_->setCurrentIndex(0);
    updateStatusBar();
}

int StringsBrowserPanel::stringCount() const {
    return strModel_ ? strModel_->totalCount() : 0;
}

int StringsBrowserPanel::constantCount() const {
    return constModel_ ? constModel_->rowCount({}) : 0;
}

} // namespace retdec::gui::panels
