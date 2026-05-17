/**
 * @file cli_tool_paths.cpp
 * @brief Locate RetDec CLI tools next to retdec-gui or on PATH.
 */

#include "retdec/gui/cli_tool_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace retdec {
namespace gui {

QString resolveCliTool(const QString& toolBaseName) {
#ifdef Q_OS_WIN
    const QString name = toolBaseName + QStringLiteral(".exe");
#else
    const QString name = toolBaseName;
#endif
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString sideBySide = QDir(appDir).filePath(name);
    if (QFileInfo::exists(sideBySide))
        return QFileInfo(sideBySide).absoluteFilePath();
    const QString fromPath = QStandardPaths::findExecutable(name);
    if (!fromPath.isEmpty())
        return QFileInfo(fromPath).absoluteFilePath();
    return {};
}

QString resolveRetdecFileinfoExecutable() {
    QString p = resolveCliTool(QStringLiteral("retdec-fileinfo"));
    if (!p.isEmpty())
        return p;
    return resolveCliTool(QStringLiteral("fileinfo"));
}

QString resolveRetdecUnpackerExecutable() {
    QString p = resolveCliTool(QStringLiteral("retdec-unpacker"));
    if (!p.isEmpty())
        return p;
    return resolveCliTool(QStringLiteral("unpacker"));
}

QString resolveRetdecDecompilerExecutablePath() {
    return resolveCliTool(QStringLiteral("retdec-decompiler"));
}

QString resolveBundledDecompilerConfigPath() {
    const QString dec = resolveRetdecDecompilerExecutablePath();
    if (dec.isEmpty())
        return {};
    const QDir bin(QFileInfo(dec).absolutePath());
    const QStringList candidates = {
        QDir::cleanPath(bin.filePath(QStringLiteral("../share/retdec/decompiler-config.json"))),
        QDir::cleanPath(bin.filePath(QStringLiteral("../../share/retdec/decompiler-config.json"))),
    };
    for (const QString& c : candidates) {
        if (QFileInfo::exists(c))
            return QFileInfo(c).absoluteFilePath();
    }
    return {};
}

QString resolveRetdecArExtractorExecutable() {
    QString p = resolveCliTool(QStringLiteral("retdec-ar-extractor"));
    if (!p.isEmpty())
        return p;
    return resolveCliTool(QStringLiteral("ar_extractor"));
}

QString resolveRetdecMachoExtractorExecutable() {
    return resolveCliTool(QStringLiteral("retdec-macho-extractor"));
}

QString resolveRetdecBin2patExecutable() {
    return resolveCliTool(QStringLiteral("retdec-bin2pat"));
}

QString resolveRetdecPat2yaraExecutable() {
    return resolveCliTool(QStringLiteral("retdec-pat2yara"));
}

QString resolvePythonInterpreter() {
    QString p = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (!p.isEmpty())
        return QFileInfo(p).absoluteFilePath();
    p = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (!p.isEmpty())
        return QFileInfo(p).absoluteFilePath();
#ifdef Q_OS_WIN
    p = QStandardPaths::findExecutable(QStringLiteral("py.exe"));
    if (!p.isEmpty())
        return QFileInfo(p).absoluteFilePath();
#endif
    return {};
}

QString resolveSignatureFromLibraryCreatorScript() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(QStringLiteral("../share/retdec/scripts/retdec-signature-from-library-creator.py")),
        QDir(appDir).filePath(QStringLiteral("../../share/retdec/scripts/retdec-signature-from-library-creator.py")),
        QDir(appDir).filePath(QStringLiteral("../scripts/retdec-signature-from-library-creator.py")),
        QDir(appDir).filePath(QStringLiteral("../../scripts/retdec-signature-from-library-creator.py")),
        QDir(appDir).filePath(QStringLiteral("retdec-signature-from-library-creator.py")),
    };
    for (const QString& c : candidates) {
        const QString norm = QFileInfo(c).absoluteFilePath();
        if (QFileInfo::exists(norm))
            return norm;
    }
    return {};
}

} // namespace gui
} // namespace retdec
