/**
 * @file src/ipa/inline_candidate.cpp
 * @brief Inline candidate identification.
 *
 * A function qualifies for inlining if ALL of:
 *   1. It is called from exactly one call site (callCount == 1).
 *   2. Its total instruction count < kMaxInlineInstrs (20).
 *   3. It is pure (no global writes, no escaping / modified pointer params).
 *   4. It is not recursive (not in an SCC of size > 1, no self-edge).
 */

#include "retdec/ipa/ipa.h"

namespace retdec {
namespace ipa {

std::vector<InlineCandidate>
InlineCandidateFinder::run(
        const CallGraph& cg,
        const std::vector<CallGraphScc>& sccs,
        const std::unordered_map<FnName, FunctionSummary>& summaries) const {

    // Build set of recursive functions.
    std::unordered_set<FnName> recursive;
    for (const CallGraphScc& scc : sccs) {
        if (scc.isRecursive) {
            for (const FnName& fn : scc.members) recursive.insert(fn);
        }
    }

    // Build single-call-site map: callee → caller.
    std::unordered_map<FnName, FnName> singleCaller;
    std::unordered_map<FnName, uint32_t> callInstrMap;
    for (const CallEdge& e : cg.edges()) {
        if (cg.callCount(e.callee) == 1) {
            singleCaller[e.callee] = e.caller;
            callInstrMap[e.callee] = e.callInstrId;
        }
    }

    std::vector<InlineCandidate> candidates;
    for (const auto& [name, summ] : summaries) {
        if (recursive.count(name)) continue;
        if (!summ.isPure) continue;
        if (summ.instrCount >= kMaxInlineInstrs) continue;
        if (cg.callCount(name) != 1) continue;

        auto callerIt = singleCaller.find(name);
        if (callerIt == singleCaller.end()) continue;

        InlineCandidate ic;
        ic.name        = name;
        ic.callerName  = callerIt->second;
        ic.callInstrId = callInstrMap.count(name) ? callInstrMap.at(name) : UINT32_MAX;
        ic.instrCount  = summ.instrCount;
        ic.isPure      = summ.isPure;
        candidates.push_back(ic);
    }

    return candidates;
}

} // namespace ipa
} // namespace retdec
