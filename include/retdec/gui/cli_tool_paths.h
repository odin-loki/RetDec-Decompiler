#ifndef RETDEC_GUI_CLI_TOOL_PATHS_H
#define RETDEC_GUI_CLI_TOOL_PATHS_H

#include <QString>

namespace retdec {
namespace gui {

/// Resolve `name` / `name.exe` next to the GUI binary, then PATH.
QString resolveCliTool(const QString& toolBaseName);

QString resolveRetdecFileinfoExecutable();
QString resolveRetdecUnpackerExecutable();
QString resolveRetdecDecompilerExecutablePath();
/// Install-relative `share/retdec/decompiler-config.json` next to `retdec-decompiler`, if present.
QString resolveBundledDecompilerConfigPath();
QString resolveRetdecArExtractorExecutable();
QString resolveRetdecMachoExtractorExecutable();
QString resolveRetdecBin2patExecutable();
QString resolveRetdecPat2yaraExecutable();

/// `python3` or `python` on PATH (for bundled scripts).
QString resolvePythonInterpreter();

/// Installed `retdec-signature-from-library-creator.py` (empty if not found).
QString resolveSignatureFromLibraryCreatorScript();

} // namespace gui
} // namespace retdec

#endif
