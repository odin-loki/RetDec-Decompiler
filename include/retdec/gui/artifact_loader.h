/**
 * @file include/retdec/gui/artifact_loader.h
 * @brief Load decompiler sidecars (.c, .config.json, .dsm, .ll) for GUI panels.
 */

#ifndef RETDEC_GUI_ARTIFACT_LOADER_H
#define RETDEC_GUI_ARTIFACT_LOADER_H

#include "retdec/gui/panels/cfg_panel.h"
#include "retdec/gui/panels/call_graph_panel.h"
#include "retdec/gui/panels/function_list_panel.h"
#include "retdec/gui/panels/strings_browser_panel.h"
#include "retdec/gui/panels/type_hierarchy_panel.h"

#include <QJsonObject>
#include <QString>

#include <memory>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace gui {

namespace panels {
class AssemblyPanel;
class IRPanel;
class CFGPanel;
class TriPaneCodeView;
} // namespace panels

struct DecompileArtifactPaths {
    QString cPath;
    QString configPath;
    QString dsmPath;
    QString llPath;
};

DecompileArtifactPaths pathsFromOutputC(const QString& cPath);

struct DecompileArtifacts {
    QString cPath;
    QJsonObject config;
    QString fullDsm;
    QString fullLl;
    std::vector<panels::FunctionEntry> functions;
    std::vector<panels::StringEntry> strings;
    std::vector<panels::CallGraphNode> callGraphNodes;
    std::vector<panels::CallEdge> callGraphEdges;
    /// Per-function CFG keyed by start address.
    std::unordered_map<uint64_t, std::vector<panels::BasicBlockData>> cfgBlocks;
    std::unordered_map<uint64_t, std::vector<panels::CFGEdgeData>> cfgEdges;
    /// RTTI class hierarchy for TypeHierarchyPanel.
    QList<panels::ClassInfo> typeHierarchyClasses;
    /// Flattened semantic recovery detections from config functions.
    struct SemanticDetectionEntry {
        QString function;
        QString kind;
        QString label;
        double confidence = 0.0;
        QString detail;
    };
    std::vector<SemanticDetectionEntry> semanticDetections;
};

bool loadDecompileArtifactsFromPaths(const DecompileArtifactPaths& paths,
                                     DecompileArtifacts& out,
                                     QString* errOut = nullptr);

QString extractDsmForFunction(const QString& fullDsm,
                              uint64_t startAddr,
                              uint64_t endAddr,
                              const QString& funcName = {});

QString extractLlvmForFunction(const QString& fullLl, const QString& funcName);

QString extractCForFunction(const QString& cPath,
                          int startLine,
                          int endLine,
                          const QString& funcName);

void populateFunctionViews(const DecompileArtifacts& art,
                           uint64_t funcAddr,
                           const QString& funcName,
                           panels::AssemblyPanel* assembly,
                           panels::IRPanel* ir,
                           panels::CFGPanel* cfg,
                           panels::TriPaneCodeView* triPane = nullptr);

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_ARTIFACT_LOADER_H
