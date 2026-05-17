/**
 * @file include/retdec/ipa/ipa.h
 * @brief Summary-Based Inter-Procedural Analysis (Stage 23).
 *
 * ## Overview
 *
 * Intra-procedural analyses (type inference, alias analysis, DCE) operate on
 * one function at a time.  They produce conservative approximations at call
 * boundaries because they don't know what the callee does to its arguments.
 *
 * This module refines those approximations by:
 *   1. Building a call graph.
 *   2. Finding strongly connected components (SCCs) — mutually recursive groups.
 *   3. Processing SCCs in reverse topological order (callees before callers).
 *   4. For each function, computing a `FunctionSummary` — a compact description
 *      of its observable effects at call boundaries.
 *   5. Propagating summaries across call sites to refine caller context.
 *   6. Unifying types of global variables across all writers.
 *   7. Identifying inline candidates.
 *
 * ## Call graph
 *
 * Each node is one `SSAFunction`.  An edge `A → B` exists if A contains a
 * CALL instruction that targets B (by function name in the symbol table or
 * by direct VMA match).  Indirect calls (through function pointers) generate
 * edges to all functions whose address is taken.
 *
 * ## Function summary
 *
 * ```
 * FunctionSummary {
 *   paramTypes[i]     — refined type of the i-th parameter
 *   returnType        — refined return type
 *   aliasEffects      — bitmask: which param pairs may alias each other
 *   escapeSet         — set of param indices that escape (their address taken)
 *   globalReads       — set of global variable names read
 *   globalWrites      — set of global variable names written
 *   isPure            — true if no global writes and no escaping params
 *   isNoReturn        — true if function has no reachable RET
 *   instrCount        — total instruction count (for inline decision)
 *   callCount         — number of call sites in the whole module
 * }
 * ```
 *
 * ## SCC-stratified propagation
 *
 * Processing order: reverse topological order of SCCs.
 *   - Non-recursive SCC (single node, no self-edge): compute summary once.
 *   - Recursive SCC (≥2 nodes, or self-edge): iterate summary computation
 *     to fixpoint.  Convergence criterion: summaries unchanged between
 *     iterations.  Real-world code converges in ≤3 iterations.
 *
 * ## Type propagation at call sites
 *
 * For each call site `ret = call f(a0, a1, ...)`:
 *   - Narrow the type of each argument `ai` to `summary(f).paramTypes[i]`.
 *   - Narrow the type of `ret` to `summary(f).returnType`.
 *   - If param `i` escapes: mark the pointed-to memory as potentially aliased.
 *
 * ## Global variable typing
 *
 * All stores to each global address across all functions are collected.
 * Their types are unified (widest compatible type).  Conflicting types
 * (e.g. one function writes int, another writes float to the same address)
 * produce a `GlobalVarInfo::isAmbiguous = true` flag; the code generator
 * emits a union type in that case.
 *
 * ## Inline candidates
 *
 * A function is an inline candidate if:
 *   - It is called from exactly one call site.
 *   - It has fewer than 20 IR instructions.
 *   - It has no global side effects (isPure).
 *   - It is not recursive (not in an SCC with a self-edge or size > 1).
 *
 * Inlining is performed by the code generator; this pass only marks candidates.
 */

#ifndef RETDEC_IPA_H
#define RETDEC_IPA_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa    { class SSAFunction; }
namespace call_conv { struct CallingConvention; }
namespace type_inference {
    struct IrType;
    enum class TypeKind : uint8_t;
}
} // namespace retdec

namespace retdec {
namespace ipa {

using FnName = std::string;

// ─── Function summary ─────────────────────────────────────────────────────────

/**
 * A compact cross-boundary descriptor for one function, computed from its
 * intra-procedural analyses and refined iteratively by IPA.
 */
struct FunctionSummary {
    FnName   name;

    // Parameter types (index = ABI argument position).
    struct ParamInfo {
        uint8_t  width        = 64;
        bool     isFp         = false;
        bool     isPointer    = false;
        bool     escapes      = false;  ///< address of this param is taken
        bool     isModified   = false;  ///< callee writes through this pointer
        uint32_t aliasGroup   = UINT32_MAX; ///< params sharing the same group may alias
    };
    std::vector<ParamInfo> params;

    // Return type
    uint8_t  retWidth   = 64;
    bool     retIsFp    = false;
    bool     retIsPtr   = false;
    bool     isVoid     = true;

    // Side effects
    std::unordered_set<std::string> globalReads;
    std::unordered_set<std::string> globalWrites;

    // Derived flags
    bool isPure       = false;  ///< no global writes, no escaping params
    bool isNoReturn   = false;  ///< no reachable RET
    bool isRecursive  = false;  ///< self-call or in multi-node SCC

    // Sizing
    std::size_t instrCount = 0;
    std::size_t callCount  = 0;  ///< call sites targeting this function

    // Convergence helper (for recursive SCCs)
    bool operator==(const FunctionSummary& o) const;
    bool operator!=(const FunctionSummary& o) const { return !(*this == o); }

    std::string toString() const;
};

// ─── Call graph ───────────────────────────────────────────────────────────────

struct CallEdge {
    FnName    caller;
    FnName    callee;
    uint32_t  callInstrId = UINT32_MAX;
};

/**
 * Directed call graph over a module (set of SSAFunctions).
 * Nodes = function names; edges = direct calls.
 */
class CallGraph {
public:
    void build(const std::vector<const ssa::SSAFunction*>& fns);

    const std::vector<FnName>& successors(const FnName& fn) const;
    const std::vector<FnName>& predecessors(const FnName& fn) const;
    const std::vector<CallEdge>& edges() const { return edges_; }
    const std::vector<FnName>& nodes() const { return nodes_; }

    std::size_t callCount(const FnName& callee) const;

private:
    std::vector<FnName>    nodes_;
    std::vector<CallEdge>  edges_;
    std::unordered_map<FnName, std::vector<FnName>> succs_;
    std::unordered_map<FnName, std::vector<FnName>> preds_;
    std::unordered_map<FnName, std::size_t>         callCounts_;
};

// ─── SCC decomposition ────────────────────────────────────────────────────────

struct CallGraphScc {
    std::vector<FnName> members;   ///< functions in this SCC
    bool isRecursive = false;      ///< true if size>1 or self-edge exists
};

/**
 * Tarjan's SCC algorithm on the call graph.
 * Returns SCCs in reverse topological order (callees before callers).
 */
class TarjanScc {
public:
    std::vector<CallGraphScc> run(const CallGraph& cg) const;

private:
    struct State {
        std::unordered_map<FnName, uint32_t> index;
        std::unordered_map<FnName, uint32_t> lowlink;
        std::unordered_map<FnName, bool>     onStack;
        std::vector<FnName>                  stack;
        std::vector<CallGraphScc>            sccs;
        uint32_t                             counter = 0;
    };

    void strongConnect(const FnName& v, const CallGraph& cg, State& s) const;
};

// ─── Global variable information ──────────────────────────────────────────────

struct GlobalVarInfo {
    std::string name;
    uint64_t    address    = 0;
    uint8_t     width      = 0;
    bool        isFp       = false;
    bool        isPointer  = false;
    bool        isAmbiguous= false;  ///< conflicting types from different writers
    std::unordered_set<FnName> writers;
    std::unordered_set<FnName> readers;

    std::string toString() const;
};

// ─── Inline candidate ────────────────────────────────────────────────────────

struct InlineCandidate {
    FnName      name;
    std::string callerName;       ///< the single caller
    uint32_t    callInstrId = UINT32_MAX;
    std::size_t instrCount  = 0;
    bool        isPure      = true;
};

// ─── Summary computer ─────────────────────────────────────────────────────────

/**
 * Computes the `FunctionSummary` for a single function, using its
 * intra-procedural analysis results (calling convention, type inference,
 * alias analysis, DCE result).
 */
class SummaryComputer {
public:
    FunctionSummary compute(
        const ssa::SSAFunction& fn,
        const call_conv::CallingConvention& cc) const;
};

// ─── IPA propagation ─────────────────────────────────────────────────────────

/**
 * Propagates summaries across call sites in SCC topological order.
 *
 * For each call site `call f(a0..an)` in function `caller`:
 *   - Refine caller's type assignments for a0..an using f's param types.
 *   - Refine the return type of the call result using f's return type.
 *   - If param i escapes in f's summary, mark the underlying SSA value
 *     in caller as potentially escaping.
 */
class IpaPropagation {
public:
    void propagate(
        const std::vector<CallGraphScc>& sccs,
        std::unordered_map<FnName, FunctionSummary>& summaries,
        const std::unordered_map<FnName, const ssa::SSAFunction*>& fnMap,
        const std::unordered_map<FnName, call_conv::CallingConvention>& ccMap) const;
};

// ─── Global typer ────────────────────────────────────────────────────────────

/**
 * Collects all stores to each global variable address across all functions
 * and unifies their types.  Conflicting types set `isAmbiguous`.
 */
class GlobalTyper {
public:
    std::unordered_map<std::string, GlobalVarInfo>
    run(const std::vector<const ssa::SSAFunction*>& fns,
        const std::unordered_map<FnName, FunctionSummary>& summaries) const;
};

// ─── Inline candidate finder ─────────────────────────────────────────────────

class InlineCandidateFinder {
public:
    static constexpr std::size_t kMaxInlineInstrs = 20;

    std::vector<InlineCandidate>
    run(const CallGraph& cg,
        const std::vector<CallGraphScc>& sccs,
        const std::unordered_map<FnName, FunctionSummary>& summaries) const;
};

// ─── IPA result ──────────────────────────────────────────────────────────────

struct IpaResult {
    std::unordered_map<FnName, FunctionSummary>    summaries;
    std::unordered_map<std::string, GlobalVarInfo> globals;
    std::vector<InlineCandidate>                   inlineCandidates;
    std::vector<CallGraphScc>                      sccs;
    CallGraph                                      callGraph;

    std::string summary() const;
};

// ─── Main IPA pass ────────────────────────────────────────────────────────────

/**
 * Orchestrates the full IPA pipeline:
 *   1. Build call graph.
 *   2. Compute SCCs (Tarjan).
 *   3. Compute per-function summaries (SummaryComputer).
 *   4. SCC-stratified propagation to fixpoint (IpaPropagation).
 *   5. Global variable type unification (GlobalTyper).
 *   6. Inline candidate identification (InlineCandidateFinder).
 */
class IpaPass {
public:
    struct Config {
        std::size_t maxFixpointIterations = 5;
        bool        enableInlineAnalysis  = true;
        bool        enableGlobalTyping    = true;
    };
    static Config defaultConfig() noexcept { return {}; }

    IpaResult run(
        const std::vector<const ssa::SSAFunction*>& fns,
        const std::unordered_map<FnName, call_conv::CallingConvention>& ccMap,
        const Config& cfg = defaultConfig()) const;
};

} // namespace ipa
} // namespace retdec

#endif // RETDEC_IPA_H
