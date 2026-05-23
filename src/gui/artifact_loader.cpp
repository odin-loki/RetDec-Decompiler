/**
 * @file src/gui/artifact_loader.cpp
 */

#include "retdec/gui/artifact_loader.h"

#include "retdec/gui/panels/assembly_panel.h"
#include "retdec/gui/panels/ir_panel.h"
#include "retdec/gui/panels/tri_pane_code_view.h"
#include "retdec/gui/panels/type_hierarchy_panel.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>

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
        {
            bool lineOk = false;
            const int sl = o.value(QStringLiteral("startLine")).toString().toInt(&lineOk);
            if (lineOk && sl > 0)
                e.startLine = sl;
            const int el = o.value(QStringLiteral("endLine")).toString().toInt(&lineOk);
            if (lineOk && el > 0)
                e.endLine = el;
        }
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

std::vector<panels::BlockInstr> extractDsmInstructionsForRange(const QString& fullDsm,
                                                                uint64_t startAddr,
                                                                uint64_t endAddr)
{
    std::vector<panels::BlockInstr> instrs;
    if (fullDsm.isEmpty() || endAddr <= startAddr)
        return instrs;

    static const QRegularExpression insRe(
            QStringLiteral("^(0x[0-9a-fA-F]+):\\s*(.*)$"));
    for (const QString& line : fullDsm.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = insRe.match(line.trimmed());
        if (!m.hasMatch())
            continue;
        const uint64_t a = parseAddr(m.captured(1));
        if (a < startAddr || a >= endAddr)
            continue;
        panels::BlockInstr bi;
        bi.address = a;
        bi.text    = m.captured(2).trimmed();
        instrs.push_back(std::move(bi));
    }
    return instrs;
}

void fillBlockInstructionsFromDsm(std::vector<panels::BasicBlockData>& blocks,
                                  const QString& fullDsm)
{
    if (fullDsm.isEmpty())
        return;
    for (auto& bb : blocks) {
        if (bb.endAddress <= bb.address)
            continue;
        bb.instrs = extractDsmInstructionsForRange(fullDsm, bb.address, bb.endAddress);
    }
}

struct VtableParseData {
    uint64_t                  address = 0;
    QList<panels::VtableSlot> vtableSlots;
};

QHash<QString, VtableParseData> parseVtables(const QJsonArray& vtArr)
{
    QHash<QString, VtableParseData> byName;
    for (const QJsonValue& vv : vtArr) {
        if (!vv.isObject())
            continue;
        const QJsonObject vo = vv.toObject();
        const QString name = vo.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;

        VtableParseData data;
        data.address = parseAddr(vo.value(QStringLiteral("address")).toString());

        int idx = 0;
        for (const QJsonValue& iv : vo.value(QStringLiteral("items")).toArray()) {
            if (!iv.isObject())
                continue;
            const QJsonObject io = iv.toObject();
            panels::VtableSlot vtSlot;
            vtSlot.index       = idx++;
            vtSlot.funcName    = io.value(QStringLiteral("targetName")).toString();
            vtSlot.funcAddress = parseAddr(io.value(QStringLiteral("targetAddress")).toString());
            vtSlot.isPure      = (vtSlot.funcAddress == 0);
            data.vtableSlots.append(vtSlot);
        }
        byName.insert(name, std::move(data));
    }
    return byName;
}

QList<panels::ClassInfo> parseTypeHierarchyClasses(const QJsonObject& config)
{
    const QHash<QString, VtableParseData> vtables =
            parseVtables(config.value(QStringLiteral("vtables")).toArray());

    QList<panels::ClassInfo> classes;
    const QJsonArray classArr = config.value(QStringLiteral("classes")).toArray();
    classes.reserve(classArr.size());

    for (const QJsonValue& cv : classArr) {
        if (!cv.isObject())
            continue;
        const QJsonObject co = cv.toObject();

        panels::ClassInfo info;
        const QString demangled = co.value(QStringLiteral("demangledName")).toString();
        info.name = !demangled.isEmpty()
                ? demangled
                : co.value(QStringLiteral("name")).toString();
        if (info.name.isEmpty())
            continue;

        for (const QJsonValue& sv : co.value(QStringLiteral("superClasses")).toArray()) {
            const QString base = sv.toString();
            if (base.isEmpty())
                continue;
            panels::InheritanceLink link;
            link.base = base;
            link.kind = panels::InheritanceLink::Kind::Public;
            info.bases.append(link);
        }

        int methodCount = 0;
        const auto countNames = [&](const char* key) {
            methodCount += co.value(QString::fromLatin1(key)).toArray().size();
        };
        countNames("constructors");
        countNames("destructors");
        countNames("methods");
        countNames("virtualMethods");
        info.methodCount = methodCount;

        for (const QJsonValue& vtNameVal : co.value(QStringLiteral("virtualTables")).toArray()) {
            const QString vtName = vtNameVal.toString();
            const auto vtIt = vtables.constFind(vtName);
            if (vtIt == vtables.constEnd())
                continue;
            if (info.vtableAddress == 0)
                info.vtableAddress = vtIt->address;
            if (info.vtable.isEmpty())
                info.vtable = vtIt->vtableSlots;
            for (const auto& vtSlot : vtIt->vtableSlots) {
                if (vtSlot.isPure) {
                    info.isAbstract = true;
                    break;
                }
            }
        }

        classes.append(std::move(info));
    }
    return classes;
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
            if (ok && bbEnd > bb.address)
                bb.endAddress = bbEnd;
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
    out.typeHierarchyClasses = parseTypeHierarchyClasses(out.config);

    out.fullDsm = readTextFileIfExists(paths.dsmPath);
    out.fullLl  = readTextFileIfExists(paths.llPath);

    for (auto& entry : out.cfgBlocks)
        fillBlockInstructionsFromDsm(entry.second, out.fullDsm);
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

QString readLineRangeFromFile(const QString& path, int startLine, int endLine)
{
    if (startLine < 1 || path.isEmpty())
        return {};

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};

    QStringList out;
    QTextStream ts(&f);
    int line = 0;
    while (!ts.atEnd()) {
        ++line;
        const QString l = ts.readLine();
        if (line < startLine)
            continue;
        if (endLine > 0 && line > endLine)
            break;
        out << l;
    }
    return out.join(QLatin1Char('\n'));
}

QString extractCForFunctionByName(const QString& fullC, const QString& funcName)
{
    if (fullC.isEmpty() || funcName.isEmpty())
        return {};

    const QString escaped = QRegularExpression::escape(funcName);
    const QRegularExpression sigRe(
            QStringLiteral(R"((?:^|\n)([\w\s\*]+?\b%1\s*\())").arg(escaped));

    const QRegularExpressionMatch m = sigRe.match(fullC);
    if (!m.hasMatch())
        return {};

    const int bodyStart = m.capturedStart(1);
    int depth = 0;
    bool seenBrace = false;
    int i = bodyStart;
    for (; i < fullC.size(); ++i) {
        const QChar c = fullC.at(i);
        if (c == QLatin1Char('{')) {
            ++depth;
            seenBrace = true;
        } else if (c == QLatin1Char('}') && seenBrace) {
            --depth;
            if (depth == 0) {
                ++i;
                break;
            }
        }
    }
    return fullC.mid(bodyStart, i - bodyStart).trimmed();
}

QString extractCForFunction(const QString& cPath,
                          int startLine,
                          int endLine,
                          const QString& funcName)
{
    if (startLine > 0)
        return readLineRangeFromFile(cPath, startLine, endLine);

    if (funcName.isEmpty() || cPath.isEmpty())
        return {};

    const QString fullC = readTextFileIfExists(cPath);
    return extractCForFunctionByName(fullC, funcName);
}

panels::LineMapping buildIdentityLineMapping(const QString& asmText,
                                             const QString& irText,
                                             const QString& cText)
{
    const auto lineCount = [](const QString& text) -> int {
        if (text.isEmpty())
            return 0;
        return static_cast<int>(text.count(QLatin1Char('\n')) + 1);
    };

    panels::LineMapping mapping;
    const int n = qMin(qMin(lineCount(asmText), lineCount(irText)), lineCount(cText));
    for (int i = 1; i <= n; ++i)
        mapping.addEntry(i, i, i);
    return mapping;
}

void populateFunctionViews(const DecompileArtifacts& art,
                           uint64_t funcAddr,
                           const QString& funcName,
                           panels::AssemblyPanel* assembly,
                           panels::IRPanel* ir,
                           panels::CFGPanel* cfg,
                           panels::TriPaneCodeView* triPane)
{
    uint64_t endAddr = 0;
    int startLine = -1;
    int endLine = -1;
    for (const auto& f : art.functions) {
        if (f.address == funcAddr) {
            endAddr = f.address + static_cast<uint64_t>(qMax(f.sizeBytes, 1));
            startLine = f.startLine;
            endLine = f.endLine;
            break;
        }
    }

    const QString asmText = extractDsmForFunction(art.fullDsm, funcAddr, endAddr, funcName);
    const QString irText  = extractLlvmForFunction(art.fullLl, funcName);

    if (assembly) {
        assembly->setAssemblyText(asmText);
        assembly->navigateTo(funcAddr);
    }
    if (ir)
        ir->setIRText(irText);

    if (triPane) {
        const QString cText = extractCForFunction(art.cPath, startLine, endLine, funcName);
        triPane->loadFunction(funcAddr, asmText, irText, cText);
        triPane->setLineMapping(buildIdentityLineMapping(asmText, irText, cText));
    }

    if (cfg) {
        const auto bIt = art.cfgBlocks.find(funcAddr);
        const auto eIt = art.cfgEdges.find(funcAddr);
        if (bIt != art.cfgBlocks.end() && !bIt->second.empty()) {
            auto blocks = bIt->second;
            fillBlockInstructionsFromDsm(blocks, art.fullDsm);
            const auto& edges = (eIt != art.cfgEdges.end()) ? eIt->second
                                                            : std::vector<panels::CFGEdgeData>{};
            cfg->loadCFG(blocks, edges);
        } else {
            cfg->clear();
        }
    }
}

} // namespace gui
} // namespace retdec
