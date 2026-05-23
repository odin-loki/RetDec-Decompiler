/**
 * @file src/gui/artifact_loader.cpp
 */

#include "retdec/gui/artifact_loader.h"

#include "retdec/gui/panels/assembly_panel.h"
#include "retdec/gui/panels/ir_panel.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace retdec {
namespace gui {

namespace {

uint64_t parseAddr(const QString& s, bool* okOut = nullptr)
{
    const QString t = s.trimmed();
    bool ok = false;
    uint64_t v = 0;
    if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        v = t.mid(2).toULongLong(&ok, 16);
    else
        v = t.toULongLong(&ok, 10);
    if (okOut) *okOut = ok;
    return ok ? v : 0;
}

QString readTextFileIfExists(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

std::vector<panels::FunctionEntry> parseFunctions(const QJsonArray& fnArr)
{
    std::vector<panels::FunctionEntry> entries;
    entries.reserve(static_cast<size_t>(fnArr.size()));
    for (const QJsonValue& v : fnArr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        panels::FunctionEntry e;
        e.name    = o.value(QStringLiteral("name")).toString();
        e.rawName = o.value(QStringLiteral("demangledName")).toString();
        bool ok = false;
        e.address = parseAddr(o.value(QStringLiteral("startAddr")).toString(), &ok);
        if (!ok) e.address = 0;
        uint64_t end = parseAddr(o.value(QStringLiteral("endAddr")).toString(), &ok);
        if (ok && end > e.address)
            e.sizeBytes = static_cast<int>(end - e.address);
        e.cc = o.value(QStringLiteral("callingConvention")).toString();
        const QString fncType = o.value(QStringLiteral("fncType")).toString();
        e.isLibrary = (fncType == QStringLiteral("staticallyLinked")
                    || fncType == QStringLiteral("dynamicallyLinked"));
        entries.push_back(std::move(e));
    }
    return entries;
}

std::vector<panels::StringEntry> parseStringGlobals(const QJsonArray& globals)
{
    std::vector<panels::StringEntry> out;
    for (const QJsonValue& v : globals) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        const QJsonObject typeObj = o.value(QStringLiteral("type")).toObject();
        const QString llvmIr = typeObj.value(QStringLiteral("llvmIr")).toString();
        const bool wide = typeObj.value(QStringLiteral("isWideString")).toBool(false);
        if (!llvmIr.contains(QStringLiteral("i8")) && !wide)
            continue;

        panels::StringEntry e;
        const QJsonObject storage = o.value(QStringLiteral("storage")).toObject();
        e.address = parseAddr(storage.value(QStringLiteral("value")).toString());
        e.value = o.value(QStringLiteral("realName")).toString();
        if (e.value.isEmpty())
            e.value = o.value(QStringLiteral("name")).toString();
        e.type = wide ? panels::StringType::Wide : panels::StringType::ASCII;
        e.length = e.value.size();
        out.push_back(std::move(e));
    }
    return out;
}

void buildCallGraph(const QJsonArray& fnArr,
                    std::vector<panels::CallGraphNode>* nodes,
                    std::vector<panels::CallEdge>* edges)
{
    nodes->clear();
    edges->clear();
    QHash<uint64_t, int> nodeIndex;

    auto ensureNode = [&](uint64_t addr, const QString& name, bool isLib, int instrCount) {
        if (nodeIndex.contains(addr))
            return;
        panels::CallGraphNode n;
        n.address    = addr;
        n.name       = name.isEmpty() ? QStringLiteral("sub_%1").arg(addr, 0, 16) : name;
        n.isLibrary  = isLib;
        n.instrCount = instrCount;
        nodeIndex.insert(addr, static_cast<int>(nodes->size()));
        nodes->push_back(std::move(n));
    };

    for (const QJsonValue& fv : fnArr) {
        if (!fv.isObject())
            continue;
        const QJsonObject fo = fv.toObject();
        const QString fname = fo.value(QStringLiteral("name")).toString();
        bool ok = false;
        const uint64_t start = parseAddr(fo.value(QStringLiteral("startAddr")).toString(), &ok);
        uint64_t end = parseAddr(fo.value(QStringLiteral("endAddr")).toString(), &ok);
        const int instrEst = (ok && end > start)
                ? static_cast<int>((end - start) / 4)
                : 1;
        const QString fncType = fo.value(QStringLiteral("fncType")).toString();
        const bool isLib = (fncType == QStringLiteral("staticallyLinked")
                         || fncType == QStringLiteral("dynamicallyLinked"));
        ensureNode(start, fname, isLib, instrEst);

        const QJsonArray bbArr = fo.value(QStringLiteral("basicBlocks")).toArray();
        for (const QJsonValue& bv : bbArr) {
            if (!bv.isObject())
                continue;
            const QJsonArray calls = bv.toObject().value(QStringLiteral("calls")).toArray();
            for (const QJsonValue& cv : calls) {
                if (!cv.isObject())
                    continue;
                const uint64_t tgt = parseAddr(cv.toObject().value(QStringLiteral("targetAddr")).toString());
                if (tgt == 0)
                    continue;
                ensureNode(tgt, {}, false, 1);
                panels::CallEdge e;
                e.callerAddress = start;
                e.calleeAddress = tgt;
                e.isDirect      = true;
                e.callCount     = 1;
                edges->push_back(e);
            }
        }
    }
}

void buildCfgMaps(const QJsonArray& fnArr,
                  std::unordered_map<uint64_t, std::vector<panels::BasicBlockData>>* blocks,
                  std::unordered_map<uint64_t, std::vector<panels::CFGEdgeData>>* edges)
{
    blocks->clear();
    edges->clear();

    for (const QJsonValue& fv : fnArr) {
        if (!fv.isObject())
            continue;
        const QJsonObject fo = fv.toObject();
        bool ok = false;
        const uint64_t fnStart = parseAddr(fo.value(QStringLiteral("startAddr")).toString(), &ok);
        if (!ok || fnStart == 0)
            continue;

        std::vector<panels::BasicBlockData> fnBlocks;
        std::vector<panels::CFGEdgeData> fnEdges;
        const QJsonArray bbArr = fo.value(QStringLiteral("basicBlocks")).toArray();

        for (const QJsonValue& bv : bbArr) {
            if (!bv.isObject())
                continue;
            const QJsonObject bo = bv.toObject();
            panels::BasicBlockData bb;
            bb.address = parseAddr(bo.value(QStringLiteral("startAddr")).toString(), &ok);
            bb.id      = bb.address;
            if (!ok)
                continue;
            const uint64_t bbEnd = parseAddr(bo.value(QStringLiteral("endAddr")).toString(), &ok);
            if (bbEnd > bb.address) {
                panels::BlockInstr bi;
                bi.address = bb.address;
                bi.text = QStringLiteral("; block %1–%2").arg(bb.address, 0, 16).arg(bbEnd, 0, 16);
                bb.instrs.push_back(std::move(bi));
            }
            fnBlocks.push_back(std::move(bb));

            for (const QJsonValue& sv : bo.value(QStringLiteral("succs")).toArray()) {
                const uint64_t to = parseAddr(sv.toString());
                if (to == 0)
                    continue;
                panels::CFGEdgeData e;
                e.from = bb.address;
                e.to   = to;
                e.kind = panels::EdgeKind::FallThrough;
                fnEdges.push_back(e);
            }
        }

        if (!fnBlocks.empty()) {
            (*blocks)[fnStart] = std::move(fnBlocks);
            (*edges)[fnStart]  = std::move(fnEdges);
        }
    }
}

} // namespace

DecompileArtifactPaths pathsFromOutputC(const QString& cPath)
{
    DecompileArtifactPaths p;
    p.cPath = cPath;
    QString stem = cPath;
    if (stem.endsWith(QStringLiteral(".c"), Qt::CaseInsensitive))
        stem.chop(2);
    p.configPath = stem + QStringLiteral(".config.json");
    p.dsmPath    = stem + QStringLiteral(".dsm");
    p.llPath     = stem + QStringLiteral(".ll");
    return p;
}

bool loadDecompileArtifactsFromPaths(const DecompileArtifactPaths& paths,
                                     DecompileArtifacts& out,
                                     QString* errOut)
{
    out = DecompileArtifacts{};
    out.cPath = paths.cPath;

    QFile cfgFile(paths.configPath);
    if (!cfgFile.open(QIODevice::ReadOnly)) {
        if (errOut)
            *errOut = QStringLiteral("Missing config: %1").arg(paths.configPath);
        return false;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(cfgFile.readAll(), &pe);
    if (!doc.isObject()) {
        if (errOut)
            *errOut = QStringLiteral("Invalid config JSON: %1").arg(paths.configPath);
        return false;
    }
    out.config = doc.object();

    const QJsonArray fnArr = out.config.value(QStringLiteral("functions")).toArray();
    out.functions = parseFunctions(fnArr);
    out.strings   = parseStringGlobals(out.config.value(QStringLiteral("globals")).toArray());
    buildCallGraph(fnArr, &out.callGraphNodes, &out.callGraphEdges);
    buildCfgMaps(fnArr, &out.cfgBlocks, &out.cfgEdges);

    out.fullDsm = readTextFileIfExists(paths.dsmPath);
    out.fullLl  = readTextFileIfExists(paths.llPath);
    return true;
}

QString extractDsmForFunction(const QString& fullDsm,
                              uint64_t startAddr,
                              uint64_t endAddr,
                              const QString& funcName)
{
    if (fullDsm.isEmpty())
        return {};

    static const QRegularExpression fnRe(
            QStringLiteral("; function:\\s*(.+?)\\s+at\\s+(0x[0-9a-fA-F]+)\\s+--\\s+(0x[0-9a-fA-F]+)"));

    QStringList out;
    bool inFn = false;
    const QStringList lines = fullDsm.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QRegularExpressionMatch m = fnRe.match(line);
        if (m.hasMatch()) {
            const QString name = m.captured(1).trimmed();
            const uint64_t s = parseAddr(m.captured(2));
            inFn = (s == startAddr) || (!funcName.isEmpty() && name == funcName);
            if (inFn) {
                out.clear();
                out << line;
            }
            continue;
        }
        if (inFn) {
            if (line.startsWith(QStringLiteral("; function:")))
                break;
            out << line;
        }
    }

    if (out.isEmpty()) {
        static const QRegularExpression insRe(QStringLiteral("^(0x[0-9a-fA-F]+):"));
        for (const QString& line : lines) {
            const QRegularExpressionMatch m = insRe.match(line);
            if (!m.hasMatch())
                continue;
            const uint64_t a = parseAddr(m.captured(1));
            if (a >= startAddr && (endAddr == 0 || a < endAddr))
                out << line;
        }
    }
    return out.join(QLatin1Char('\n'));
}

QString extractLlvmForFunction(const QString& fullLl, const QString& funcName)
{
    if (fullLl.isEmpty() || funcName.isEmpty())
        return {};

    const QString needle = QStringLiteral("define ") + funcName;
    const int start = fullLl.indexOf(needle);
    if (start < 0)
        return {};

    int depth = 0;
    int i = start;
    for (; i < fullLl.size(); ++i) {
        const QChar c = fullLl.at(i);
        if (c == QLatin1Char('{'))
            ++depth;
        else if (c == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                ++i;
                break;
            }
        }
    }
    return fullLl.mid(start, i - start);
}

void populateFunctionViews(const DecompileArtifacts& art,
                           uint64_t funcAddr,
                           const QString& funcName,
                           panels::AssemblyPanel* assembly,
                           panels::IRPanel* ir,
                           panels::CFGPanel* cfg)
{
    uint64_t endAddr = 0;
    for (const auto& f : art.functions) {
        if (f.address == funcAddr) {
            endAddr = f.address + static_cast<uint64_t>(qMax(f.sizeBytes, 1));
            break;
        }
    }

    if (assembly)
        assembly->setAssemblyText(extractDsmForFunction(art.fullDsm, funcAddr, endAddr, funcName));
    if (ir)
        ir->setIRText(extractLlvmForFunction(art.fullLl, funcName));

    if (cfg) {
        const auto bIt = art.cfgBlocks.find(funcAddr);
        const auto eIt = art.cfgEdges.find(funcAddr);
        if (bIt != art.cfgBlocks.end() && !bIt->second.empty()) {
            const auto& edges = (eIt != art.cfgEdges.end()) ? eIt->second
                                                            : std::vector<panels::CFGEdgeData>{};
            cfg->loadCFG(bIt->second, edges);
        } else {
            cfg->clear();
        }
    }
}

} // namespace gui
} // namespace retdec
