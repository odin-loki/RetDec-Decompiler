/**
 * @file include/retdec/gui/model/artifact_loader.h
 * @brief Load the decompiler's sidecars (.c, .config.json, .dsm, .ll) into
 *        model data for the panels.
 *
 * For an output `x.gui-decompiled.c` the decompiler writes
 * `x.gui-decompiled.config.json` (functions, globals, classes, vtables,
 * basic blocks, semantic detections), `x.gui-decompiled.dsm` (disassembly)
 * and `x.gui-decompiled.ll` (LLVM IR). Only the config is required.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_ARTIFACT_LOADER_H
#define RETDEC_GUI_MODEL_ARTIFACT_LOADER_H

#include "retdec/gui/model/settings.h"
#include "retdec/gui/model/types.h"

#include <forge/json/json.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace retdec::gui::model {

struct DecompileArtifactPaths
{
	std::filesystem::path cPath;
	std::filesystem::path configPath;
	std::filesystem::path dsmPath;
	std::filesystem::path llPath;
};

/// Sidecar paths for @p cPath: a trailing ".c" (any case) is replaced by
/// ".config.json", ".dsm" and ".ll"; any other name gets them appended.
DecompileArtifactPaths pathsFromOutputC(const std::filesystem::path& cPath);

struct DecompileArtifacts
{
	std::filesystem::path cPath;
	/// The whole config.json document (an object).
	forge::json::value config;
	std::string fullDsm;
	std::string fullLl;
	std::vector<FunctionEntry> functions;
	std::vector<StringEntry> strings;
	std::vector<ConstantEntry> constants;
	std::vector<CallGraphNode> callGraphNodes;
	std::vector<CallEdge> callGraphEdges;
	/// Per-function CFG keyed by function start address. Blocks carry their
	/// instructions from the .dsm when it is present.
	std::unordered_map<uint64_t, std::vector<BasicBlockData>> cfgBlocks;
	std::unordered_map<uint64_t, std::vector<CFGEdgeData>> cfgEdges;
	/// RTTI class hierarchy for the type hierarchy view.
	std::vector<ClassInfo> typeHierarchyClasses;
	/// Flattened `semanticDetections` from config functions (entries without a label are dropped).
	std::vector<SemanticDetection> semanticDetections;
};

/**
 * @brief Read the sidecars. Fails when the config is missing or is not a JSON
 *        object; a missing .dsm / .ll just leaves the related data empty.
 *
 * @p recovery supplies the per-category confidence floors used when turning
 * semantic detections into FunctionEntry::patterns flags (a detection with a
 * known confidence below its floor does not set the flag).
 */
std::expected<DecompileArtifacts, std::string> loadDecompileArtifactsFromPaths(
	const DecompileArtifactPaths& paths, const RecoverySettings& recovery = AppSettings::instance().recovery);

/// Parse a config.json document already in memory (paths only name the .dsm /
/// .ll to read; cPath is recorded). Same rules as loadDecompileArtifactsFromPaths.
std::expected<DecompileArtifacts, std::string> loadDecompileArtifactsFromConfigJson(
	std::string_view configJson,
	const DecompileArtifactPaths& paths,
	const RecoverySettings& recovery = AppSettings::instance().recovery);

/// The .dsm section of one function: from its "; function: NAME at 0xS -- 0xE"
/// header (matched by start address or by @p funcName) to the next header.
/// Without a header, the instruction lines in [startAddr, endAddr) (endAddr 0 =
/// no upper bound). Lines joined with '\n'.
std::string
extractDsmForFunction(std::string_view fullDsm, uint64_t startAddr, uint64_t endAddr, std::string_view funcName = {});

/// Instructions "0xADDR: text" of @p fullDsm in [startAddr, endAddr).
std::vector<BlockInstr> extractDsmInstructionsForRange(std::string_view fullDsm, uint64_t startAddr, uint64_t endAddr);

/// The LLVM IR `define` of @p funcName up to its closing brace: first
/// "define <funcName>" as the Qt GUI searched, then a define line naming
/// "@<funcName>(" (how LLVM actually prints it).
std::string extractLlvmForFunction(std::string_view fullLl, std::string_view funcName);

/// C text of a function: lines [startLine, endLine] (1-based, endLine <= 0 =
/// to the end) when startLine > 0, else the definition of @p funcName found by
/// its signature and brace matching.
std::string
extractCForFunction(const std::filesystem::path& cPath, int startLine, int endLine, std::string_view funcName);

/// Same as the name-based part of extractCForFunction, on text in memory.
std::string extractCForFunctionByName(std::string_view fullC, std::string_view funcName);

/// One row of the tri-pane line mapping (1-based line numbers).
struct FunctionLineMapEntry
{
	int asmLine = 0;
	int irLine = 0;
	int cLine = 0;
};

/// Everything the per-function views (assembly, IR, C, tri-pane, CFG) show
/// for one function. Replaces the Qt populateFunctionViews(), minus the drawing.
struct FunctionViewData
{
	uint64_t address = 0;
	std::string name;
	/// address + max(size, 1) when the function is known, else 0.
	uint64_t endAddress = 0;
	int startLine = -1;
	int endLine = -1;
	std::string assemblyText;              ///< extractDsmForFunction()
	std::string irText;                    ///< extractLlvmForFunction()
	std::string cText;                     ///< extractCForFunction() (empty unless requested)
	std::vector<BasicBlockData> cfgBlocks; ///< empty = no CFG (clear the view)
	std::vector<CFGEdgeData> cfgEdges;
	/// Identity mapping over the shortest of the three texts, as the Qt GUI used.
	std::vector<FunctionLineMapEntry> lineMapping;
};

/// Build the per-function view data. @p includeC reads the .c file (only the
/// tri-pane view needs it). The whole-module IR is DecompileArtifacts::fullLl.
FunctionViewData buildFunctionViewData(
	const DecompileArtifacts& art, uint64_t funcAddr, std::string_view funcName, bool includeC = true);

/// 1..n identity rows, n = min(line counts) of the three texts (0 if any is empty).
std::vector<FunctionLineMapEntry>
buildIdentityLineMapping(std::string_view asmText, std::string_view irText, std::string_view cText);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_ARTIFACT_LOADER_H
