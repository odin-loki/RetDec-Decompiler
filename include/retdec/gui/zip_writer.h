/**
 * @file include/retdec/gui/zip_writer.h
 * @brief Minimal PKZIP writer (store-only, no external zip library).
 */

#ifndef RETDEC_GUI_ZIP_WRITER_H
#define RETDEC_GUI_ZIP_WRITER_H

#include <QByteArray>
#include <QFile>
#include <QString>
#include <vector>

namespace retdec {
namespace gui {

/**
 * Writes a ZIP archive using compression method 0 (store).
 * Suitable for bundling decompiler artifacts without QuaZip / zlib linkage.
 */
class ZipWriter {
public:
    explicit ZipWriter(const QString& zipPath);
    ~ZipWriter();

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    bool isOpen() const { return open_; }

    /// Add an on-disk file under @p entryName (forward slashes, no leading slash).
    bool addFile(const QString& diskPath, const QString& entryName);

    /// Add in-memory bytes under @p entryName.
    bool addEntry(const QString& entryName, const QByteArray& data);

    /// Finalize central directory and close the archive.
    bool close();

    QString errorString() const { return error_; }

private:
    struct Entry {
        QString   name;
        quint32   crc32 = 0;
        quint32   size  = 0;
        quint32   localHeaderOffset = 0;
    };

    bool writeLocalEntry(const QString& entryName, const QByteArray& data,
                         quint32 crc32, quint32 offset);
    static quint32 crc32(const QByteArray& data);

    QString              path_;
    QFile*               file_ = nullptr;
    bool                 open_ = false;
    bool                 closed_ = false;
    QString              error_;
    std::vector<Entry>   entries_;
};

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_ZIP_WRITER_H
