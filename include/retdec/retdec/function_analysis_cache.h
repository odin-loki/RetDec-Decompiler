/**
 * @file include/retdec/retdec/function_analysis_cache.h
 * @brief Per-function incremental cache for post-pipeline analysis detectors.
 * @copyright (c) 2024 RetDec contributors, MIT license
 */

#ifndef RETDEC_RETDEC_FUNCTION_ANALYSIS_CACHE_H
#define RETDEC_RETDEC_FUNCTION_ANALYSIS_CACHE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/container_detect/container_detect.h"
#include "retdec/sort_detect/sort_detect.h"

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace retdec {
namespace ssa { class SSAFunction; }

namespace analysis {

/// Sidecar filename appended to the output `.c` basename.
constexpr const char* kFunctionCacheSuffix = ".retdec-fn-cache.json";

/// Minimum function count before parallel analysis is considered (> 4).
constexpr std::size_t kParallelAnalysisMinFunctions = 5;

/**
 * @brief Per-function detection bundle (container, sort, algo, concurrency).
 */
struct FunctionDetections {
    std::optional<container_detect::ContainerResult> container;
    std::optional<sort_detect::SortResult> sort;
    std::optional<algo_recover::AlgorithmResult> algo;
};

/**
 * @brief True when RETDEC_PARALLEL_ANALYSIS is unset and hardware_concurrency > 2,
 *        or when the env var is set to a non-zero value.
 */
bool parallelAnalysisEnabled();

/**
 * @brief Derive the cache sidecar path from a decompiler output `.c` path.
 * Example: `/out/demo.c` → `/out/demo.retdec-fn-cache.json`
 */
std::string functionAnalysisCachePath(const std::string& outputCPath);

/**
 * @brief FNV-1a content hash of an LLVM function body, or SSA stats fallback.
 */
std::string computeFunctionBodyHash(
        const llvm::Module& module,
        const ssa::SSAFunction& fn);

/**
 * @brief Run container / sort / algo / concurrency detectors on one function.
 */
FunctionDetections analyseFunctionDetections(const ssa::SSAFunction& fn);

/**
 * @brief Incremental function-level analysis cache (JSON sidecar).
 *
 * Keys are `(functionName, bodyHash)`. Values hold serialized detector output
 * so unchanged functions can be skipped on `--select-functions` re-runs.
 */
class FunctionAnalysisCache {
public:
    static constexpr std::uint32_t kVersion = 1;

    struct Entry {
        std::string name;
        std::string bodyHash;
        FunctionDetections detections;
    };

    /// Load existing sidecar; missing or corrupt files are ignored.
    static FunctionAnalysisCache loadFromFile(const std::string& path);

    /// Write sidecar atomically (best-effort).
    bool saveToFile(const std::string& path) const;

    /// Lookup by name + body hash; returns nullptr when stale or absent.
    const Entry* lookup(const std::string& name, const std::string& bodyHash) const;

    /// Insert or replace an entry.
    void put(Entry entry);

    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};

} // namespace analysis
} // namespace retdec

#endif // RETDEC_RETDEC_FUNCTION_ANALYSIS_CACHE_H
