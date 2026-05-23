/**
 * @file src/gui/export_bundle.cpp
 */

#include "retdec/gui/export_bundle.h"

#include "retdec/gui/artifact_loader.h"
#include "retdec/gui/zip_writer.h"

#include <QFile>
#include <QFileInfo>

namespace retdec {
namespace gui {

namespace {

QString zipEntryBasename(const QString& path)
{
    return QFileInfo(path).fileName();
}

bool addFileIfExists(ZipWriter& zip, const QString& diskPath, QString* errOut)
{
    if (!QFileInfo::exists(diskPath))
        return true;
    if (!zip.addFile(diskPath, zipEntryBasename(diskPath))) {
        if (errOut)
            *errOut = zip.errorString();
        return false;
    }
    return true;
}

} // namespace

QString formatDecompilerCommandText(const DecompileBundleInput& in)
{
    QString line = QStringLiteral("executable: %1\n").arg(in.decompilerExe);
    if (!in.decompilerCwd.isEmpty())
        line += QStringLiteral("cwd: %1\n").arg(in.decompilerCwd);
    line += QStringLiteral("command:");
    for (const QString& a : in.decompilerArgs)
        line += QLatin1Char(' ') + a;
    line += QLatin1Char('\n');
    return line;
}

bool exportDecompileBundle(const DecompileBundleInput& in,
                           const QString& zipPath,
                           QString* errOut)
{
    if (in.cPath.isEmpty() || !QFileInfo::exists(in.cPath)) {
        if (errOut)
            *errOut = QStringLiteral("Decompiled C file not found: %1").arg(in.cPath);
        return false;
    }

    const DecompileArtifactPaths paths = pathsFromOutputC(in.cPath);

    ZipWriter zip(zipPath);
    if (!zip.isOpen()) {
        if (errOut)
            *errOut = zip.errorString();
        return false;
    }

    if (!addFileIfExists(zip, paths.cPath, errOut))
        return false;
    if (!addFileIfExists(zip, paths.configPath, errOut))
        return false;
    if (!addFileIfExists(zip, paths.dsmPath, errOut))
        return false;
    if (!addFileIfExists(zip, paths.llPath, errOut))
        return false;

    const QByteArray cmdText = formatDecompilerCommandText(in).toUtf8();
    if (!cmdText.isEmpty()) {
        if (!zip.addEntry(QStringLiteral("decompiler-command.txt"), cmdText)) {
            if (errOut)
                *errOut = zip.errorString();
            return false;
        }
    }

    QByteArray logBytes;
    if (!in.logFilePath.isEmpty()) {
        QFile logFile(in.logFilePath);
        if (logFile.open(QIODevice::ReadOnly))
            logBytes = logFile.readAll();
    }
    if (logBytes.isEmpty() && !in.logInline.isEmpty())
        logBytes = in.logInline;

    if (!logBytes.isEmpty()) {
        if (!zip.addEntry(QStringLiteral("decompiler.log"), logBytes)) {
            if (errOut)
                *errOut = zip.errorString();
            return false;
        }
    }

    if (!zip.close()) {
        if (errOut)
            *errOut = zip.errorString();
        return false;
    }
    return true;
}

} // namespace gui
} // namespace retdec
