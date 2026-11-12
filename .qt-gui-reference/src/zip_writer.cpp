/**
 * @file src/gui/zip_writer.cpp
 */

#include "retdec/gui/zip_writer.h"

#include <QFile>
#include <QFileInfo>

namespace retdec {
namespace gui {

namespace {

constexpr quint32 kLocalFileHeaderSig  = 0x04034b50;
constexpr quint32 kCentralDirHeaderSig = 0x02014b50;
constexpr quint32 kEndOfCentralDirSig  = 0x06054b50;

void appendLe16(QByteArray& out, quint16 v)
{
    out.append(static_cast<char>(v & 0xff));
    out.append(static_cast<char>((v >> 8) & 0xff));
}

void appendLe32(QByteArray& out, quint32 v)
{
    out.append(static_cast<char>(v & 0xff));
    out.append(static_cast<char>((v >> 8) & 0xff));
    out.append(static_cast<char>((v >> 16) & 0xff));
    out.append(static_cast<char>((v >> 24) & 0xff));
}

QByteArray entryNameBytes(const QString& entryName)
{
    return entryName.toUtf8();
}

} // namespace

ZipWriter::ZipWriter(const QString& zipPath)
    : path_(zipPath)
{
    file_ = new QFile(path_);
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error_ = file_->errorString();
        delete file_;
        file_ = nullptr;
        return;
    }
    open_ = true;
}

ZipWriter::~ZipWriter()
{
    if (open_ && !closed_)
        close();
    delete file_;
}

quint32 ZipWriter::crc32(const QByteArray& data)
{
    static quint32 table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        tableReady = true;
    }

    quint32 crc = 0xffffffffu;
    for (unsigned char ch : data) {
        crc = table[(crc ^ ch) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

bool ZipWriter::writeLocalEntry(const QString& entryName, const QByteArray& data,
                                quint32 crc, quint32 offset)
{
    const QByteArray name = entryNameBytes(entryName);
    if (name.isEmpty()) {
        error_ = QStringLiteral("Empty ZIP entry name");
        return false;
    }

    QByteArray hdr;
    hdr.reserve(30 + name.size());
    appendLe32(hdr, kLocalFileHeaderSig);
    appendLe16(hdr, 20); // version needed
    appendLe16(hdr, 0);  // flags
    appendLe16(hdr, 0);  // compression: store
    appendLe16(hdr, 0);  // mod time
    appendLe16(hdr, 0);  // mod date
    appendLe32(hdr, crc);
    appendLe32(hdr, static_cast<quint32>(data.size()));
    appendLe32(hdr, static_cast<quint32>(data.size()));
    appendLe16(hdr, static_cast<quint16>(name.size()));
    appendLe16(hdr, 0); // extra length
    hdr.append(name);

    if (file_->write(hdr) != hdr.size()) {
        error_ = file_->errorString();
        return false;
    }
    if (!data.isEmpty() && file_->write(data) != data.size()) {
        error_ = file_->errorString();
        return false;
    }

    Entry e;
    e.name              = entryName;
    e.crc32             = crc;
    e.size              = static_cast<quint32>(data.size());
    e.localHeaderOffset = offset;
    entries_.push_back(std::move(e));
    return true;
}

bool ZipWriter::addEntry(const QString& entryName, const QByteArray& data)
{
    if (!open_ || closed_) {
        error_ = QStringLiteral("ZIP archive is not open");
        return false;
    }
    QString name = entryName;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (name.startsWith(QLatin1Char('/'))) {
        error_ = QStringLiteral("Invalid ZIP entry name: %1").arg(entryName);
        return false;
    }
    const quint32 offset = static_cast<quint32>(file_->pos());
    return writeLocalEntry(name, data, crc32(data), offset);
}

bool ZipWriter::addFile(const QString& diskPath, const QString& entryName)
{
    QFile in(diskPath);
    if (!in.open(QIODevice::ReadOnly)) {
        error_ = QStringLiteral("Cannot read %1: %2").arg(diskPath, in.errorString());
        return false;
    }
    return addEntry(entryName, in.readAll());
}

bool ZipWriter::close()
{
    if (!open_ || closed_)
        return open_;
    closed_ = true;

    const quint32 centralDirOffset = static_cast<quint32>(file_->pos());
    quint32 centralDirSize = 0;

    for (const Entry& e : entries_) {
        const QByteArray name = entryNameBytes(e.name);
        QByteArray hdr;
        hdr.reserve(46 + name.size());
        appendLe32(hdr, kCentralDirHeaderSig);
        appendLe16(hdr, 20); // version made by
        appendLe16(hdr, 20); // version needed
        appendLe16(hdr, 0);  // flags
        appendLe16(hdr, 0);  // compression
        appendLe16(hdr, 0);  // mod time
        appendLe16(hdr, 0);  // mod date
        appendLe32(hdr, e.crc32);
        appendLe32(hdr, e.size);
        appendLe32(hdr, e.size);
        appendLe16(hdr, static_cast<quint16>(name.size()));
        appendLe16(hdr, 0); // extra
        appendLe16(hdr, 0); // comment
        appendLe16(hdr, 0); // disk start
        appendLe16(hdr, 0); // internal attrs
        appendLe32(hdr, 0); // external attrs
        appendLe32(hdr, e.localHeaderOffset);
        hdr.append(name);

        if (file_->write(hdr) != hdr.size()) {
            error_ = file_->errorString();
            return false;
        }
        centralDirSize += static_cast<quint32>(hdr.size());
    }

    QByteArray eocd;
    eocd.reserve(22);
    appendLe32(eocd, kEndOfCentralDirSig);
    appendLe16(eocd, 0); // disk
    appendLe16(eocd, 0); // central dir disk
    appendLe16(eocd, static_cast<quint16>(entries_.size()));
    appendLe16(eocd, static_cast<quint16>(entries_.size()));
    appendLe32(eocd, centralDirSize);
    appendLe32(eocd, centralDirOffset);
    appendLe16(eocd, 0); // comment length

    if (file_->write(eocd) != eocd.size()) {
        error_ = file_->errorString();
        return false;
    }

    file_->close();
    open_ = false;
    return true;
}

} // namespace gui
} // namespace retdec
