/**
 * @file src/gui/decompiler_launch.cpp
 */

#include "retdec/gui/decompiler_launch.h"

#include "retdec/gui/panels/diagnostics_panel.h"
#include "retdec/gui/panels/live_console_panel.h"

#include <QTemporaryFile>

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace retdec {
namespace gui {

namespace {

std::unique_ptr<QTemporaryFile> makeLlvmPassesJsonTempFile(
        const DecompilerSettings& d,
        bool fastPreset,
        QString* errOut)
{
    const QString resource = fastPreset
            ? QStringLiteral(":/retdec/llvm_passes_fast.json")
            : QStringLiteral(":/retdec/llvm_passes_default.json");
    QFile f(resource);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut)
            *errOut = QStringLiteral("Missing resource %1").arg(resource);
        return nullptr;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isArray()) {
        if (errOut)
            *errOut = QStringLiteral("Invalid LLVM passes resource %1").arg(resource);
        return nullptr;
    }
    const QSet<QString> disabled(d.llvmPassesDisabled.begin(),
                                 d.llvmPassesDisabled.end());
    QJsonArray out;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isString())
            continue;
        const QString s = v.toString();
        if (!disabled.contains(s))
            out.append(s);
    }
    auto t = std::make_unique<QTemporaryFile>();
    t->setAutoRemove(true);
    if (!t->open()) {
        if (errOut)
            *errOut = QStringLiteral("Could not create temporary LLVM passes file.");
        return nullptr;
    }
    const QJsonDocument outDoc(out);
    if (t->write(outDoc.toJson(QJsonDocument::Compact)) < 0) {
        if (errOut)
            *errOut = QStringLiteral("Could not write LLVM passes JSON.");
        return nullptr;
    }
    t->flush();
    return t;
}

struct LogStageRule {
    const char* keyword;
    const char* stage;
    int         order;
};

/// Ordered pipeline stages aligned with ProgressPanel::kDefaultStages.
constexpr LogStageRule kLogStageRules[] = {
    {"initialization",           "Binary loading",              0},
    {"providers initialization", "Binary loading",              0},
    {"mach-o extraction",        "Binary loading",              0},
    {"archive extraction",       "Binary loading",              0},
    {"unpacking",                "Binary loading",              0},
    {"emulation-based unpacking","Binary loading",              0},
    {"input binary",             "Binary loading",              0},
    {"decoding",                 "Binary loading",              0},
    {"class hierarchy",          "RTTI reconstruction",         1},
    {"rtti",                     "RTTI reconstruction",         1},
    {"vtable",                   "RTTI reconstruction",         1},
    {"string",                   "String detection",            2},
    {"mem2reg",                  "SSA construction",            3},
    {"phi",                      "SSA construction",            3},
    {"stack",                    "SSA construction",            3},
    {"register",                 "SSA construction",            3},
    {"param",                    "Variable recovery",           4},
    {"return",                   "Variable recovery",           4},
    {"constants",                "Variable recovery",           4},
    {"alias",                    "Alias analysis",              5},
    {"tbaa",                     "Alias analysis",              5},
    {"basicaa",                  "Alias analysis",              5},
    {"type",                     "Type inference",              6},
    {"simple-types",             "Type inference",              6},
    {"simple types",             "Type inference",              6},
    {"cond-branch",              "CFG structuring",             7},
    {"jump",                     "CFG structuring",             7},
    {"simplifycfg",              "CFG structuring",             7},
    {"cfg",                      "CFG structuring",             7},
    {"syscall",                  "Calling convention",          8},
    {"main detection",           "Calling convention",          8},
    {"unreachable",              "Dead code elimination",       9},
    {"dead",                     "Dead code elimination",       9},
    {"dce",                      "Dead code elimination",       9},
    {"select-function",          "Inter-procedural analysis",  10},
    {"select function",          "Inter-procedural analysis",  10},
    {"inter-procedural",         "Inter-procedural analysis",  10},
    {"llvm",                     "Inter-procedural analysis",  10},
    {"llvmir2hll",               "Code generation",            11},
    {"write-ll",                 "Code generation",            11},
    {"write ll",                 "Code generation",            11},
    {"write bc",                 "Code generation",            11},
    {"conversion of llvm",       "Code generation",            11},
    {"emitting",                 "Code generation",            11},
    {"hll",                      "Code generation",            11},
    {"library func",             "STL recovery",               12},
    {"standard libr",            "STL recovery",               12},
    {"stl",                      "STL recovery",               12},
    {"pattern",                  "Pattern / crypto detection", 13},
    {"crypto",                   "Pattern / crypto detection", 13},
    {"static code",              "Pattern / crypto detection", 13},
    {"post-pipeline",            "Pattern / crypto detection", 13},
};

constexpr int kPipelineStageCount = 14;

QString stripAnsi(const QString& line) {
    static const QRegularExpression ansiRe(
            QStringLiteral("\\x1b\\[[0-9;]*m"));
    QString out = line;
    out.remove(ansiRe);
    return out;
}

QString stripElapsedSuffix(QString text) {
    static const QRegularExpression elapsedRe(
            QStringLiteral(R"(\(\s*[0-9.]+\s*s\s*\)$)"));
    text.remove(elapsedRe);
    return text.trimmed();
}

QString extractPhaseText(const QString& line) {
    const QString t = stripAnsi(line).trimmed();
    const int runIdx = t.indexOf(QStringLiteral("Running phase:"));
    if (runIdx >= 0)
        return stripElapsedSuffix(t.mid(runIdx + 14).trimmed());

    const int subIdx = t.lastIndexOf(QStringLiteral(" -> "));
    if (subIdx >= 0)
        return stripElapsedSuffix(t.mid(subIdx + 4).trimmed());

    return {};
}

bool matchLogStage(const QString& phaseText, QString* stageOut, int* orderOut) {
    if (phaseText.isEmpty())
        return false;

    const QString lower = phaseText.toLower();
    int bestOrder = -1;
    int bestKeywordLen = -1;
    QString bestStage;
    for (const LogStageRule& rule : kLogStageRules) {
        const QString kw = QString::fromLatin1(rule.keyword);
        if (!lower.contains(kw))
            continue;
        const int kwLen = kw.size();
        if (kwLen > bestKeywordLen ||
            (kwLen == bestKeywordLen && rule.order > bestOrder)) {
            bestKeywordLen = kwLen;
            bestOrder      = rule.order;
            bestStage      = QString::fromUtf8(rule.stage);
        }
    }
    if (bestOrder < 0)
        return false;

    if (stageOut)
        *stageOut = bestStage;
    if (orderOut)
        *orderOut = bestOrder;
    return true;
}

int percentForStageOrder(int order) {
    if (order < 0)
        return 0;
    return qBound(1, ((order + 1) * 100) / kPipelineStageCount, 99);
}

int percentFromBackendProgress(int backendPercent) {
    // llvmir2hll backend occupies the last ~35% of the bar.
    return qBound(1, 65 + (backendPercent * 34) / 100, 99);
}

bool parseProgressLine(const QString& line, DecompileLogProgress* out) {
    static const QRegularExpression progressRe(
            QStringLiteral(R"(\[progress\][^\n]*?(\d+)%)"));
    const QRegularExpressionMatch m = progressRe.match(stripAnsi(line));
    if (!m.hasMatch())
        return false;

    const int backendPct = m.captured(1).toInt();
    out->stage   = QStringLiteral("Code generation");
    out->percent = percentFromBackendProgress(backendPct);
    return true;
}

bool parsePhaseLine(const QString& line, DecompileLogProgress* out) {
    const QString phaseText = extractPhaseText(line);
    QString stage;
    int order = -1;
    if (!matchLogStage(phaseText, &stage, &order))
        return false;

    out->stage   = stage;
    out->percent = percentForStageOrder(order);
    return true;
}

} // namespace

QStringList buildDecompilerArguments(
        const DecompilerLaunchRequest& req,
        QString* errOut,
        std::unique_ptr<QTemporaryFile>* llvmPassesOut)
{
    QStringList args;

    if (!req.decompiler.extraConfigPath.isEmpty()) {
        if (QFileInfo::exists(req.decompiler.extraConfigPath)) {
            args << QStringLiteral("--config") << req.decompiler.extraConfigPath;
        } else if (errOut) {
            *errOut = QStringLiteral("Configured --config file missing: %1")
                              .arg(req.decompiler.extraConfigPath);
        }
    }

    args << req.binaryPath
         << QStringLiteral("-o") << req.outputPath
         << QStringLiteral("-f") << QStringLiteral("plain")
         << QStringLiteral("-s");

    const QString outLang = req.decompiler.outputLang.trimmed();
    if (!outLang.isEmpty())
        args << QStringLiteral("--output-lang") << outLang;

    const QString arch = req.arch.trimmed();
    if (!arch.isEmpty())
        args << QStringLiteral("-a") << arch;

    QString profileName = req.decompiler.decompileProfile.trimmed();
    if (profileName.isEmpty())
        profileName = QStringLiteral("balanced");
    if (req.fastDecompile)
        profileName = QStringLiteral("fast");
    if (!req.decompiler.useCustomLlvmPasses)
        args << QStringLiteral("--profile") << profileName;

    if (req.fastDecompile) {
        args << QStringLiteral("--backend-no-opts")
             << QStringLiteral("--disable-static-code-detection");
    }
    if (req.printAfterAll)
        args << QStringLiteral("--print-after-all");

    if (!req.selectedFunctions.isEmpty())
        args << QStringLiteral("--select-functions")
             << req.selectedFunctions.join(QStringLiteral(","));

    if (llvmPassesOut &&
        (req.decompiler.useCustomLlvmPasses || req.fastDecompile)) {
        QString passErr;
        auto passes = makeLlvmPassesJsonTempFile(
                req.decompiler, req.fastDecompile, &passErr);
        if (!passes) {
            if (errOut)
                *errOut = passErr;
            return {};
        }
        passes->close();
        args << QStringLiteral("--llvm-passes-json") << passes->fileName();
        *llvmPassesOut = std::move(passes);
    }

    return args;
}

void appendDecompilerLogToConsole(panels::LiveConsolePanel* panel,
                                  const QString& logPath,
                                  qint64 maxBytes)
{
    if (!panel || logPath.isEmpty())
        return;

    QFile f(logPath);
    if (!f.open(QIODevice::ReadOnly))
        return;

    const qint64 size = f.size();
    if (size <= 0)
        return;

    if (size <= maxBytes) {
        panel->appendChunk(panels::LiveConsolePanel::Stream::Stdout, f.readAll());
        return;
    }

    if (!f.seek(size - maxBytes)) {
        panel->appendChunk(panels::LiveConsolePanel::Stream::Stdout, f.readAll());
        return;
    }

    QByteArray tail = f.readAll();
    const int nl = tail.indexOf('\n');
    if (nl >= 0)
        tail.remove(0, nl + 1);

    panel->appendLine(
            panels::LiveConsolePanel::Stream::Stdout,
            QStringLiteral("… (%1 KiB log omitted, showing last %2 KiB) …")
                    .arg(size / 1024)
                    .arg(maxBytes / 1024));
    if (!tail.isEmpty())
        panel->appendChunk(panels::LiveConsolePanel::Stream::Stdout, tail);
}

bool appendDecompilerLogIncrementalToConsole(panels::LiveConsolePanel* panel,
                                             const QString& logPath,
                                             qint64* ioFileOffset,
                                             qint64 maxBytesPerTick)
{
    if (!panel || !ioFileOffset || logPath.isEmpty() || maxBytesPerTick <= 0)
        return false;

    QFile f(logPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    const qint64 size = f.size();
    if (*ioFileOffset > size)
        *ioFileOffset = size;

    const qint64 remaining = size - *ioFileOffset;
    if (remaining <= 0)
        return false;

    if (!f.seek(*ioFileOffset))
        return false;

    const qint64 toRead = qMin(remaining, maxBytesPerTick);
    const QByteArray chunk = f.read(toRead);
    if (chunk.isEmpty())
        return false;

    *ioFileOffset += chunk.size();
    panel->appendChunk(panels::LiveConsolePanel::Stream::Stdout, chunk);
    return true;
}

void scanDecompilerLogDiagnostics(panels::DiagnosticsPanel* diagnostics,
                                  const QString& logPath,
                                  int maxEntries)
{
    if (!diagnostics || logPath.isEmpty() || maxEntries <= 0)
        return;

    QFile f(logPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QTextStream ts(&f);
    int added = 0;
    while (!ts.atEnd() && added < maxEntries) {
        const QString trimmed = ts.readLine().trimmed();
        if (trimmed.isEmpty())
            continue;

        auto sev = panels::DiagnosticEntry::Severity::Info;
        if (trimmed.contains(QStringLiteral("error"), Qt::CaseInsensitive) ||
            trimmed.startsWith(QStringLiteral("[error]"), Qt::CaseInsensitive)) {
            sev = panels::DiagnosticEntry::Severity::Error;
        } else if (trimmed.contains(QStringLiteral("warning"), Qt::CaseInsensitive) ||
                   trimmed.startsWith(QStringLiteral("[warn]"), Qt::CaseInsensitive)) {
            sev = panels::DiagnosticEntry::Severity::Warning;
        } else {
            continue;
        }

        diagnostics->addMessage(sev, QStringLiteral("retdec-decompiler"), trimmed);
        ++added;
    }
}

void populateSemanticDetectionsFromConfig(panels::DiagnosticsPanel* diagnostics,
                                          const QJsonObject& configRoot,
                                          int maxEntries)
{
    if (!diagnostics || maxEntries <= 0)
        return;

    const QJsonArray fnArr = configRoot.value(QStringLiteral("functions")).toArray();
    int added = 0;
    for (const QJsonValue& fv : fnArr) {
        if (!fv.isObject())
            continue;
        const QJsonObject fnObj = fv.toObject();
        const QString fnName = fnObj.value(QStringLiteral("name")).toString();
        const QJsonArray dets =
                fnObj.value(QStringLiteral("semanticDetections")).toArray();
        if (dets.isEmpty())
            continue;

        uint64_t fnAddr = 0;
        {
            bool ok = false;
            const uint64_t a =
                    fnObj.value(QStringLiteral("startAddr")).toString().toULongLong(&ok, 0);
            if (ok)
                fnAddr = a;
        }

        for (const QJsonValue& dv : dets) {
            if (added >= maxEntries)
                return;
            if (!dv.isObject())
                continue;
            const QJsonObject det = dv.toObject();
            const QString kind  = det.value(QStringLiteral("kind")).toString();
            const QString label = det.value(QStringLiteral("label")).toString();
            const double conf   = det.value(QStringLiteral("confidence")).toDouble(0.0);
            const QString detail = det.value(QStringLiteral("detail")).toString();

            auto sev = panels::DiagnosticEntry::Severity::Muted;
            if (conf > 0.8)
                sev = panels::DiagnosticEntry::Severity::Info;
            else if (conf > 0.5)
                sev = panels::DiagnosticEntry::Severity::Warning;

            QString msg = label;
            if (msg.isEmpty())
                msg = kind;
            if (msg.isEmpty())
                msg = QStringLiteral("detection");
            msg += QStringLiteral(" (confidence %1%)")
                           .arg(QString::number(conf * 100.0, 'f', 0));
            if (!detail.isEmpty())
                msg += QStringLiteral(" — ") + detail;

            diagnostics->addMessage(
                    sev,
                    fnName.isEmpty() ? QStringLiteral("semantic") : fnName,
                    msg,
                    fnAddr);
            ++added;
        }
    }
}

bool pollDecompileLogProgress(const QString& logPath,
                              qint64* ioFileOffset,
                              DecompileLogProgress* out)
{
    if (!ioFileOffset || !out || logPath.isEmpty())
        return false;

    QFile f(logPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    const qint64 size = f.size();
    if (*ioFileOffset > size)
        *ioFileOffset = size;

    if (!f.seek(*ioFileOffset))
        return false;

    DecompileLogProgress latest;
    bool updated = false;

    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        DecompileLogProgress parsed;
        if (parseProgressLine(line, &parsed)) {
            if (!updated || parsed.percent >= latest.percent) {
                latest = parsed;
                updated = true;
            }
            continue;
        }
        if (parsePhaseLine(line, &parsed)) {
            if (!updated || parsed.percent >= latest.percent) {
                latest = parsed;
                updated = true;
            }
        }
    }

    *ioFileOffset = f.pos();
    if (!updated)
        return false;

    *out = latest;
    return true;
}

QString resolveGuiDecompiledCPath(const QString& binaryPath,
                                  const QString& outputDir)
{
    const QFileInfo bfi(QFileInfo(binaryPath).absoluteFilePath());
    const QString fileName =
            bfi.completeBaseName() + QStringLiteral(".gui-decompiled.c");
    const QString trimmed = outputDir.trimmed();
    if (trimmed.isEmpty())
        return QFileInfo(QDir(bfi.absolutePath()).filePath(fileName)).absoluteFilePath();
    return QFileInfo(QDir(trimmed).filePath(fileName)).absoluteFilePath();
}

QString locateGuiDecompiledCPath(const QString& binaryPath,
                                 const QString& outputDir)
{
    const QString trimmed = outputDir.trimmed();
    if (!trimmed.isEmpty()) {
        const QString configured = resolveGuiDecompiledCPath(binaryPath, trimmed);
        if (QFileInfo::exists(configured))
            return QFileInfo(configured).absoluteFilePath();
    }
    const QString beside = resolveGuiDecompiledCPath(binaryPath, QString());
    if (QFileInfo::exists(beside))
        return QFileInfo(beside).absoluteFilePath();
    return trimmed.isEmpty() ? beside : resolveGuiDecompiledCPath(binaryPath, trimmed);
}

} // namespace gui
} // namespace retdec
