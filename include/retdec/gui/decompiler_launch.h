/**
 * @file include/retdec/gui/decompiler_launch.h
 * @brief Build retdec-decompiler CLI arguments and ingest subprocess logs.
 *
 * The GUI must launch the decompiler with the same flags as a manual CLI
 * invocation.  Child stdout/stderr is redirected to a temp file (not a pipe)
 * so Qt console rendering cannot block the decompiler on a full pipe buffer.
 */

#ifndef RETDEC_GUI_DECOMPILER_LAUNCH_H
#define RETDEC_GUI_DECOMPILER_LAUNCH_H

#include "retdec/gui/settings/settings.h"

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <memory>

class QTemporaryFile;

namespace retdec {
namespace gui {

struct DecompilerLaunchRequest
{
	QString binaryPath;
	QString outputPath;
	QString arch;
	bool fastDecompile = false;
	bool printAfterAll = false;
	/// Adds `--backend-emit-cfg` (Settings → Advanced → dump CFG).
	bool emitCfg = false;
	/// Adds `--disable-static-code-detection` when not already in fast mode.
	bool disableStaticCodeDetection = false;
	/// Default true keeps `-s` (silent). Verbose/Debug interactive runs set false.
	bool silent = true;
	/// Adds `--select-decode-only` (faster partial re-decompile).
	bool selectDecodeOnly = false;
	/// Adds `--print-before-all` (very verbose).
	bool printBeforeAll = false;
	/// Adds `-k` / `--keep-unreachable-funcs`.
	bool keepUnreachableFuncs = false;
	/// Adds `--backend-emit-cg` (call-graph DOT next to CFG dumps).
	bool emitCg = false;
	/// Non-empty → adds `--select-functions` (comma-separated LLVM/config names).
	QStringList selectedFunctions;
	/// Non-empty → adds `--select-ranges` (comma-separated `0xSTART-0xEND`).
	QStringList selectedRanges;
	/// Non-zero → `--raw-entry-point` (Target panel override).
	quint64 entryPoint = 0;
	/// Existing file → `-p` / `--pdb`.
	QString pdbPath;
	/// Non-empty and not `readable` → `--backend-var-renamer`.
	QString varRenamer;
	/// Existing file → `--static-code-sigfile`.
	QString staticCodeSigFile;
	/// Adds `--cleanup` (delete intermediates after the run).
	bool cleanup = false;
	/// Adds `--try-emulation` (experimental unpacking). Default off.
	bool tryEmulation = false;
	/// Non-zero → `--max-memory` (bytes). Zero omits the flag.
	quint64 maxMemoryBytes = 0;
	/// Adds `--backend-keep-library-funcs`.
	bool keepLibraryFuncs = false;
	DecompilerSettings decompiler;
};

/// Parsed decompile progress from retdec-decompiler log tail.
struct DecompileLogProgress
{
	QString stage;
	int percent = 0;
};

/// Build the argument vector passed to retdec-decompiler.
QStringList buildDecompilerArguments(
	const DecompilerLaunchRequest& req,
	QString* errOut = nullptr,
	std::unique_ptr<QTemporaryFile>* llvmPassesOut = nullptr);

/// Child-process environment for retdec-decompiler.
/// When @p applyInteractiveOverrides is false (headless / quit-when-done),
/// returns `QProcessEnvironment::systemEnvironment()` unchanged so CLI
/// parity is preserved. Interactive runs copy ML / CUDA settings into
/// `RETDEC_NEURAL_*` and `RETDEC_OCL_HOST` when a GGUF path exists.
QProcessEnvironment buildDecompilerProcessEnvironment(const AppSettings& settings, bool applyInteractiveOverrides);

/// Resolved `.gui-decompiled.c` path. Empty @p outputDir = beside the binary.
QString resolveGuiDecompiledCPath(const QString& binaryPath, const QString& outputDir = QString());

/// Prefer configured @p outputDir, then beside-binary; returns an existing path or the preferred default.
QString locateGuiDecompiledCPath(const QString& binaryPath, const QString& outputDir = QString());

namespace panels {
class DiagnosticsPanel;
class LiveConsolePanel;
} // namespace panels

/// Append decompiler log to the live console (tail only if huge).
void appendDecompilerLogToConsole(
	panels::LiveConsolePanel* panel, const QString& logPath, qint64 maxBytes = 512 * 1024);

/// Append new log bytes from @p *ioFileOffset (up to @p maxBytesPerTick). Returns true if any bytes were appended.
bool appendDecompilerLogIncrementalToConsole(
	panels::LiveConsolePanel* panel, const QString& logPath, qint64* ioFileOffset, qint64 maxBytesPerTick = 32 * 1024);

/// Scan a log file for warning/error lines (does not load the whole file).
void scanDecompilerLogDiagnostics(panels::DiagnosticsPanel* diagnostics, const QString& logPath, int maxEntries = 200);

/// Scan newly appended log bytes from @p *ioFileOffset for warning/error lines.
/// Advances @p *ioFileOffset past complete lines. Returns true if any were added.
bool scanDecompilerLogDiagnosticsIncremental(
	panels::DiagnosticsPanel* diagnostics, const QString& logPath, qint64* ioFileOffset, int maxEntries = 50);

/// Populate Problems dock from `semanticDetections` arrays in decompile config JSON.
void populateSemanticDetectionsFromConfig(
	panels::DiagnosticsPanel* diagnostics, const QJsonObject& configRoot, int maxEntries = 500);

/// Incrementally read @p logPath from @p *ioFileOffset and map retdec-decompiler
/// log lines to GUI stage names / approximate percent (0–100).
/// Returns true when @p out was updated.
bool pollDecompileLogProgress(const QString& logPath, qint64* ioFileOffset, DecompileLogProgress* out);

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_DECOMPILER_LAUNCH_H
