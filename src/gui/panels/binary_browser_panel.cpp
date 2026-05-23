#include "retdec/gui/panels/binary_browser_panel.h"

#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace retdec::gui::panels {

namespace {

quint64 parseHexU64(const QString& s) {
    QString t = s.trimmed();
    if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        t = t.mid(2);
    bool ok = false;
    const quint64 v = t.toULongLong(&ok, 16);
    return ok ? v : 0;
}

QString formatHexDump(const QByteArray& data, quint64 fileOffset) {
    if (data.isEmpty())
        return QString();

    QString out;
    out.reserve(data.size() * 4);
    constexpr int kBytesPerLine = 16;
    for (int i = 0; i < data.size(); i += kBytesPerLine) {
        const int lineLen = qMin(kBytesPerLine, data.size() - i);
        out += QStringLiteral("%1  ")
                       .arg(fileOffset + static_cast<quint64>(i), 8, 16, QChar('0'));
        for (int j = 0; j < kBytesPerLine; ++j) {
            if (j < lineLen)
                out += QStringLiteral("%1 ").arg(static_cast<uchar>(data[i + j]), 2, 16, QChar('0'));
            else
                out += QStringLiteral("   ");
            if (j == 7)
                out += QChar(' ');
        }
        out += QChar(' ');
        for (int j = 0; j < lineLen; ++j) {
            const uchar c = static_cast<uchar>(data[i + j]);
            out += (c >= 0x20 && c < 0x7f) ? QChar(c) : QChar('.');
        }
        out += QChar('\n');
    }
    return out;
}

} // namespace

BinaryBrowserPanel::BinaryBrowserPanel(QWidget* parent)
    : PanelBase("Binary Browser", parent) {
    setupUI();
}

void BinaryBrowserPanel::setupUI() {
    splitter_    = new QSplitter(Qt::Vertical, this);
    sectionTree_ = new QTreeWidget(splitter_);
    sectionTree_->setColumnCount(3);
    sectionTree_->setHeaderLabels({"Name", "Address", "Size"});
    sectionTree_->header()->setStretchLastSection(false);
    sectionTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    sectionTree_->setAlternatingRowColors(true);
    sectionTree_->setRootIsDecorated(true);

    auto* hexWidget = new QWidget(splitter_);
    auto* hexLayout = new QVBoxLayout(hexWidget);
    hexLayout->setContentsMargins(0, 0, 0, 0);
    hexLayout->setSpacing(4);
    hexHeader_ = new QLabel("Hex View", hexWidget);
    hexHeader_->setProperty("role", "muted");
    hexView_   = new QPlainTextEdit(hexWidget);
    hexView_->setReadOnly(true);
    hexView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    hexView_->setPlaceholderText("Select a section to view its hex dump…");
    hexLayout->addWidget(hexHeader_);
    hexLayout->addWidget(hexView_);

    splitter_->addWidget(sectionTree_);
    splitter_->addWidget(hexWidget);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter_);

    connect(sectionTree_, &QTreeWidget::itemDoubleClicked,
            this, &BinaryBrowserPanel::onItemDoubleClicked);
    connect(sectionTree_, &QTreeWidget::itemSelectionChanged,
            this, &BinaryBrowserPanel::onSelectionChanged);
}

void BinaryBrowserPanel::loadBinary(const QString& path) {
    binaryPath_ = QFileInfo(path).absoluteFilePath();
    sectionTree_->clear();
    hexView_->clear();
    hexHeader_->setText(QStringLiteral("Hex View"));

    auto* hint = new QTreeWidgetItem(sectionTree_,
                                     {QStringLiteral("Run Inspect or wait for fileinfo…"), "", ""});
    hint->setFlags(hint->flags() & ~Qt::ItemIsSelectable);
    sectionTree_->addTopLevelItem(hint);
}

void BinaryBrowserPanel::populateFromFileinfo(const QJsonObject& root) {
    if (binaryPath_.isEmpty())
        return;

    sectionTree_->clear();
    hexView_->clear();

    const QJsonObject sectionTable = root.value(QStringLiteral("sectionTable")).toObject();
    const QJsonArray sections = sectionTable.value(QStringLiteral("sections")).toArray();
    if (sections.isEmpty()) {
        auto* empty = new QTreeWidgetItem(sectionTree_,
                                          {QStringLiteral("(no sections in fileinfo)"), "", ""});
        empty->setFlags(empty->flags() & ~Qt::ItemIsSelectable);
        sectionTree_->addTopLevelItem(empty);
        return;
    }

    auto* rootItem = new QTreeWidgetItem(sectionTree_,
                                         {QFileInfo(binaryPath_).fileName(), "", ""});
    rootItem->setExpanded(true);

    for (const QJsonValue& v : sections) {
        const QJsonObject sec = v.toObject();
        const QString name = sec.value(QStringLiteral("name")).toString();
        const QString address = sec.value(QStringLiteral("address")).toString();
        const QString sizeInFile = sec.value(QStringLiteral("sizeInFile")).toString();
        const quint64 offset = parseHexU64(sec.value(QStringLiteral("offset")).toString());

        auto* item = new QTreeWidgetItem(rootItem, {name, address, sizeInFile});
        item->setData(0, Qt::UserRole, QVariant::fromValue(offset));
    }

    sectionTree_->addTopLevelItem(rootItem);
    sectionTree_->expandAll();
}

void BinaryBrowserPanel::clear() {
    binaryPath_.clear();
    sectionTree_->clear();
    hexView_->clear();
    hexHeader_->setText(QStringLiteral("Hex View"));
}

void BinaryBrowserPanel::onSelectionChanged() {
    showHexForItem(sectionTree_->currentItem());
}

void BinaryBrowserPanel::showHexForItem(QTreeWidgetItem* item) {
    hexView_->clear();
    if (!item || binaryPath_.isEmpty())
        return;

    const QVariant offVar = item->data(0, Qt::UserRole);
    if (!offVar.isValid())
        return;

    const quint64 offset = offVar.toULongLong();
    const quint64 size = parseHexU64(item->text(2));
    if (size == 0)
        return;

    constexpr quint64 kMaxHexBytes = 64 * 1024;
    const quint64 readSize = qMin(size, kMaxHexBytes);

    QFile f(binaryPath_);
    if (!f.open(QIODevice::ReadOnly)) {
        hexView_->setPlainText(QStringLiteral("Cannot open binary: %1").arg(f.errorString()));
        return;
    }
    if (!f.seek(static_cast<qint64>(offset))) {
        hexView_->setPlainText(QStringLiteral("Cannot seek to offset 0x%1")
                                       .arg(offset, 0, 16));
        return;
    }

    const QByteArray chunk = f.read(static_cast<qint64>(readSize));
    hexHeader_->setText(QStringLiteral("Hex View — %1 @ file 0x%2 (%3 bytes)")
                               .arg(item->text(0))
                               .arg(offset, 0, 16)
                               .arg(size));
    QString dump = formatHexDump(chunk, offset);
    if (size > kMaxHexBytes)
        dump += QStringLiteral("\n… [showing first %1 KiB of %2 KiB]")
                        .arg(kMaxHexBytes / 1024)
                        .arg((size + 1023) / 1024);
    hexView_->setPlainText(dump);
}

void BinaryBrowserPanel::onItemDoubleClicked() {
    auto* item = sectionTree_->currentItem();
    if (!item)
        return;
    const uint64_t addr = parseHexU64(item->text(1));
    if (addr != 0)
        emit addressNavigated(addr);
}

} // namespace retdec::gui::panels
