/**
 * @file include/retdec/gui/export_bundle.h
 * @brief Export decompile artifacts as a ZIP bundle for sharing / debugging.
 */

#ifndef RETDEC_GUI_EXPORT_BUNDLE_H
#define RETDEC_GUI_EXPORT_BUNDLE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace retdec {
namespace gui {

struct DecompileBundleInput {
    QString     cPath;
    QString     decompilerExe;
    QStringList decompilerArgs;
    QString     decompilerCwd;
    /// Optional on-disk log (e.g. temp file from a running/finished decompile).
    QString     logFilePath;
    /// Optional inline log when no temp file remains (e.g. live console buffer).
    QByteArray  logInline;
};

/// Build @p zipPath from decompiler sidecars next to @p in.cPath.
bool exportDecompileBundle(const DecompileBundleInput& in,
                           const QString& zipPath,
                           QString* errOut = nullptr);

QString formatDecompilerCommandText(const DecompileBundleInput& in);

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_EXPORT_BUNDLE_H
