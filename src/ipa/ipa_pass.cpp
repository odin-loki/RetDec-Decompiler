/**
 * @file src/ipa/ipa_pass.cpp
 * @brief IpaPass orchestrator + IpaResult summary.
 */

#include "retdec/ipa/ipa.h"
#include "retdec/ssa/ssa.h"
#include "retdec/call_conv/call_conv.h"

#include <sstream>

namespace retdec {
namespace ipa {

// ─── IpaResult::summary ──────────────────────────────────────────────────────

std::string IpaResult::summary() const {
    std::ostringstream os;
    os << "IPA: " << summaries.size() << " functions, "
       << sccs.size() << " SCCs, "
       << globals.size() << " globals, "
       << inlineCandidates.size() << " inline candidates";
    return os.str();
}

// ─── IpaPass::run ─────────────────────────────────────────────────────────────

IpaResult IpaPass::run(
        const std::vector<const ssa::SSAFunction*>& fns,
        const std::unordered_map<FnName, call_conv::CallingConvention>& ccMap,
        const Config& cfg) const {

    IpaResult result;

    // ── Step 1: Build call graph ─────────────────────────────────────────────
    result.callGraph.build(fns);

    // ── Step 2: Tarjan SCC decomposition ─────────────────────────────────────
    TarjanScc tarjan;
    result.sccs = tarjan.run(result.callGraph);

    // ── Step 3: Build function name → pointer map ─────────────────────────────
    std::unordered_map<FnName, const ssa::SSAFunction*> fnMap;
    for (const ssa::SSAFunction* fn : fns) {
        if (fn) fnMap[fn->name()] = fn;
    }

    // ── Step 4: Initial per-function summary computation ─────────────────────
    SummaryComputer computer;
    for (const ssa::SSAFunction* fn : fns) {
        if (!fn) continue;
        auto ccIt = ccMap.find(fn->name());
        const call_conv::CallingConvention& cc =
            (ccIt != ccMap.end()) ? ccIt->second : call_conv::CallingConvention{};
        FunctionSummary s = computer.compute(*fn, cc);
        // Record call count from call graph.
        s.callCount = result.callGraph.callCount(fn->name());
        // Mark recursive.
        for (const CallGraphScc& scc : result.sccs) {
            if (scc.isRecursive) {
                for (const FnName& m : scc.members) {
                    if (m == fn->name()) s.isRecursive = true;
                }
            }
        }
        result.summaries[fn->name()] = std::move(s);
    }

    // ── Step 5: SCC-stratified propagation ───────────────────────────────────
    IpaPropagation prop;
    prop.propagate(result.sccs, result.summaries, fnMap, ccMap);

    // ── Step 6: Global variable typing ───────────────────────────────────────
    if (cfg.enableGlobalTyping) {
        GlobalTyper typer;
        result.globals = typer.run(fns, result.summaries);
    }

    // ── Step 7: Inline candidate identification ───────────────────────────────
    if (cfg.enableInlineAnalysis) {
        InlineCandidateFinder finder;
        result.inlineCandidates = finder.run(
            result.callGraph, result.sccs, result.summaries);
    }

    return result;
}

} // namespace ipa
} // namespace retdec
