/**
 * @file include/retdec/module_cluster/module_cluster.h
 * @brief Structural Reconstruction: Module Clustering and CMake Project
 *        Generation — Stage 40.
 *
 * Groups recovered functions into logical modules (compilation units) using a
 * combination of:
 *   1. **Call-graph community detection** (Louvain algorithm on weighted edges).
 *   2. **String-literal locality** — functions referencing the same string pool
 *      section are likely from the same translation unit.
 *   3. **Static-initialiser ordering** — CRT startup calls (.init_array /
 *      DT_INIT_ARRAY entries) hint at per-file initialisation order.
 *   4. **RTTI / typeinfo clustering** — vtable and typeinfo symbols whose
 *      mangled names share a common namespace prefix form one module.
 *   5. **Symbol prefix heuristic** — if debug symbols are absent, common
 *      name prefixes (e.g., `gl_`, `net_`, `audio_`) identify subsystems.
 *
 * ## Louvain community detection
 *
 *   Graph nodes = functions.
 *   Edge weight  = call frequency (static count of call instructions).
 *   Self-loops   = excluded.
 *   Algorithm:
 *     Phase 1 — Assign each node its own community.
 *               For each node n, compute the modularity gain ΔQ of moving n
 *               into each neighbouring community.  Move n to the community with
 *               the highest ΔQ > 0.  Repeat until no improvement.
 *     Phase 2 — Collapse each community into a super-node.
 *               Repeat Phase 1 on the compressed graph.
 *     Iterate until the modularity Q stabilises.
 *   Resolution parameter γ (default 1.0) controls module granularity.
 *
 * ## Output
 *
 *   ClusterResult — per-module:
 *     name          — inferred module name (e.g., "net", "crypto_aes")
 *     functions     — list of function names
 *     headers       — inferred #include directives (from library call patterns)
 *     dependencies  — other modules this one calls into
 *
 *   CMakeEmitter — writes a CMakeLists.txt that:
 *     - Declares a cmake_minimum_required / project block.
 *     - Adds one add_library() per module.
 *     - Adds target_include_directories, target_link_libraries.
 *     - Adds an add_executable for the binary's entry-point module.
 *     - Emits find_package() for detected third-party libraries.
 */

#ifndef RETDEC_MODULE_CLUSTER_H
#define RETDEC_MODULE_CLUSTER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec::module_cluster {

// ─── Input graph ──────────────────────────────────────────────────────────────

/**
 * @brief A directed, weighted call-graph edge.
 */
struct CallEdge {
    std::string caller;
    std::string callee;
    int         weight = 1;  ///< number of static call sites
};

/**
 * @brief Per-function metadata supplied to the clusterer.
 */
struct FunctionMeta {
    std::string              name;
    uint64_t                 address      = 0;
    std::string              sourceFile;  ///< from debug info (may be empty)
    std::string              namePrefix;  ///< common symbol prefix (e.g. "net_")
    std::vector<std::string> referencedStrings; ///< string-pool labels used
    std::vector<std::string> calledLibSyms;     ///< external library symbols called
    bool                     isEntryPoint = false;
    bool                     isInitArray  = false; ///< in .init_array
};

/**
 * @brief Input call graph for the clustering algorithm.
 */
struct CallGraph {
    std::vector<FunctionMeta> functions;
    std::vector<CallEdge>     edges;
};

// ─── Clustering result ────────────────────────────────────────────────────────

/**
 * @brief A single recovered module (compilation unit cluster).
 */
struct Module {
    std::string              name;
    std::vector<std::string> functions;
    std::vector<std::string> headers;       ///< inferred #include directives
    std::vector<std::string> dependencies;  ///< names of other modules this depends on
    bool                     isEntryModule  = false;
    int                      communityId    = -1;

    // Metrics
    int    callEdgesInternal = 0;  ///< edges within this module
    int    callEdgesExternal = 0;  ///< edges to other modules
    double cohesion          = 0.0; ///< intra/total edge ratio
};

/**
 * @brief Full clustering result.
 */
struct ClusterResult {
    std::vector<Module> modules;
    std::string         projectName;
    double              modularity = 0.0; ///< final Louvain Q value
    int                 iterations = 0;

    const Module* entryModule() const;
    const Module* findModule(const std::string& funcName) const;
};

// ─── Louvain clusterer ────────────────────────────────────────────────────────

/**
 * @brief Configuration for the Louvain algorithm.
 */
struct LouvainConfig {
    double resolution    = 1.0;  ///< γ — higher = more, smaller modules
    int    maxIterations = 100;
    bool   useSeedByName = true; ///< use symbol prefix as initial partition hint
};

/**
 * @brief Runs the Louvain community detection algorithm on a call graph.
 */
class LouvainClusterer {
public:
    explicit LouvainClusterer(LouvainConfig cfg = LouvainConfig{});

    /**
     * @brief Compute communities and return community assignment per node.
     * @param graph  The call graph.
     * @return Map from function name to community ID (0-based).
     */
    std::unordered_map<std::string, int>
    cluster(const CallGraph& graph);

    double lastModularity() const { return modularity_; }
    int    lastIterations() const { return iterations_; }

private:
    LouvainConfig cfg_;
    double        modularity_  = 0.0;
    int           iterations_  = 0;

    struct Node {
        std::string name;
        int         community  = -1;
        double      selfWeight = 0.0;
        double      degree     = 0.0;
    };

    double computeModularity(
        const std::vector<Node>& nodes,
        const std::unordered_map<std::string,
              std::unordered_map<std::string, double>>& adj,
        double totalWeight) const;

    double modularityGain(
        int nodeIdx,
        int targetCommunity,
        const std::vector<Node>& nodes,
        const std::unordered_map<std::string,
              std::unordered_map<std::string, double>>& adj,
        double totalWeight) const;
};

// ─── Module name inference ────────────────────────────────────────────────────

/**
 * @brief Infers a human-readable module name from a cluster's contents.
 *
 * Priority:
 *   1. Common symbol prefix (e.g., all functions start with "net_").
 *   2. Most prominent called library symbol (e.g., "ssl" → "ssl_module").
 *   3. Adjacent string literal content (e.g., "Error: socket" → "socket").
 *   4. Fallback: "module_N".
 */
class ModuleNamer {
public:
    std::string name(const std::vector<FunctionMeta>& funcs,
                     int fallbackId) const;
private:
    static std::string longestCommonPrefix(const std::vector<std::string>& names);
    static std::string extractLibName(const std::vector<std::string>& libSyms);
};

// ─── Header inference ─────────────────────────────────────────────────────────

/**
 * @brief Infers #include directives for a module from its library calls.
 *
 * Maps well-known library symbols to their standard headers.
 * Examples:
 *   malloc / free / realloc → <stdlib.h>
 *   printf / fprintf / scanf → <stdio.h>
 *   pthread_create           → <pthread.h>
 *   SSL_new                  → <openssl/ssl.h>
 *   cudaMalloc               → <cuda_runtime.h>
 */
class HeaderInference {
public:
    std::vector<std::string> infer(const std::vector<std::string>& libSyms) const;
private:
    static const std::unordered_map<std::string, std::string>& symbolHeaderMap();
};

// ─── CMake emitter ────────────────────────────────────────────────────────────

/**
 * @brief Configuration for the emitted CMakeLists.txt.
 */
struct CMakeConfig {
    std::string projectName     = "recovered_project";
    std::string cxxStandard     = "17";
    std::string minCmakeVersion = "3.18";
    bool        addInstall      = true;
    bool        addTesting      = false;
    std::string outputDir       = ".";
};

/**
 * @brief Emits a CMakeLists.txt for a ClusterResult.
 *
 * Example output (simplified):
 *
 *   cmake_minimum_required(VERSION 3.18)
 *   project(recovered_project CXX)
 *   set(CMAKE_CXX_STANDARD 17)
 *
 *   # Module: net (cohesion=0.82)
 *   add_library(net STATIC
 *       net_connect.cpp net_send.cpp net_recv.cpp
 *   )
 *   target_include_directories(net PRIVATE include)
 *
 *   # Module: crypto_aes
 *   add_library(crypto_aes STATIC ...)
 *   target_link_libraries(crypto_aes PRIVATE OpenSSL::Crypto)
 *
 *   # Entry point module
 *   add_executable(recovered_binary main.cpp)
 *   target_link_libraries(recovered_binary PRIVATE net crypto_aes)
 */
class CMakeEmitter {
public:
    /**
     * @brief Emit a CMakeLists.txt string for the given cluster result.
     */
    std::string emit(const ClusterResult& result,
                     const CMakeConfig&   cfg = CMakeConfig{}) const;

private:
    std::string emitFindPackages(const ClusterResult& result) const;
    std::string emitModuleTarget(const Module& mod) const;
    std::string emitEntryTarget (const Module& mod,
                                  const ClusterResult& result,
                                  const CMakeConfig& cfg) const;

    static std::string libToFindPackage(const std::string& lib);
};

// ─── ModuleClusterer (orchestrator) ───────────────────────────────────────────

/**
 * @brief Full pipeline: call-graph → module clustering → ClusterResult.
 */
class ModuleClusterer {
public:
    explicit ModuleClusterer(LouvainConfig louvainCfg = {});

    /**
     * @brief Run the full clustering pipeline.
     */
    ClusterResult cluster(const CallGraph& graph,
                          const std::string& projectName = "recovered");

private:
    LouvainConfig louvainCfg_;

    void applyStringLocalityRefinement(
        const CallGraph& graph,
        std::unordered_map<std::string, int>& communities) const;

    void applyDebugSymbolRefinement(
        const CallGraph& graph,
        std::unordered_map<std::string, int>& communities) const;

    Module buildModule(
        int communityId,
        const std::vector<std::string>& funcNames,
        const CallGraph& graph,
        const std::unordered_map<std::string, int>& communities) const;

    void inferModuleDependencies(ClusterResult& result,
                                  const CallGraph& graph,
                                  const std::unordered_map<std::string, int>& communities) const;
};

} // namespace retdec::module_cluster

#endif // RETDEC_MODULE_CLUSTER_H
