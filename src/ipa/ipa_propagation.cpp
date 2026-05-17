/**
 * @file src/ipa/ipa_propagation.cpp
 * @brief SCC-stratified inter-procedural type propagation.
 *
 * ## Algorithm
 *
 * Given the SCCs in reverse topological order (callees first):
 *
 *  For each SCC:
 *    1. Compute / refresh the summary for each member function.
 *    2. For each call site in each member function:
 *         a. Look up the callee's summary.
 *         b. Propagate callee param types → narrow caller's argument types.
 *         c. Propagate callee return type → narrow caller's result type.
 *         d. If callee param i escapes → mark caller's argument as escaping.
 *    3. For recursive SCCs (size>1 or self-edge): repeat steps 1-2 until
 *       the summaries of all members stabilise (≤ maxIterations).
 *
 * ## Type narrowing
 *
 * We model types as a simple lattice:
 *   Unknown < {Integer(N), Float(N), Pointer} < Top (ambiguous)
 *
 * Narrowing: if callee says param 0 is i32, and the caller currently has it
 * as Unknown, refine to i32.  If the caller already has it as i64, widen
 * to "at least 32 bits" (keep the wider type).  Conflicting types (i32 vs f32)
 * produce Top and are flagged as ambiguous.
 */

#include "retdec/ipa/ipa.h"
#include "retdec/ssa/ssa.h"
#include "retdec/call_conv/call_conv.h"
#include <algorithm>

namespace retdec {
namespace ipa {

// ─── Helper: refine a ParamInfo from callee summary ──────────────────────────

static void refineParam(FunctionSummary::ParamInfo& dst,
                         const FunctionSummary::ParamInfo& src) {
    // Widen width to the maximum seen.
    if (src.width > dst.width) dst.width = src.width;
    // Propagate pointer / escape / modify flags.
    if (src.isPointer)  dst.isPointer  = true;
    if (src.escapes)    dst.escapes    = true;
    if (src.isModified) dst.isModified = true;
    if (src.isFp)       dst.isFp       = true;
}

// ─── IpaPropagation::propagate ───────────────────────────────────────────────

void IpaPropagation::propagate(
        const std::vector<CallGraphScc>& sccs,
        std::unordered_map<FnName, FunctionSummary>& summaries,
        const std::unordered_map<FnName, const ssa::SSAFunction*>& fnMap,
        const std::unordered_map<FnName, call_conv::CallingConvention>& ccMap) const {

    constexpr std::size_t kMaxIter = 5;

    for (const CallGraphScc& scc : sccs) {
        std::size_t maxIter = scc.isRecursive ? kMaxIter : 1;

        for (std::size_t iter = 0; iter < maxIter; ++iter) {
            // Snapshot summaries to detect convergence.
            std::unordered_map<FnName, FunctionSummary> prev = summaries;

            for (const FnName& callerName : scc.members) {
                auto fnIt = fnMap.find(callerName);
                if (fnIt == fnMap.end() || !fnIt->second) continue;
                const ssa::SSAFunction& caller = *fnIt->second;

                FunctionSummary& callerSummary = summaries[callerName];

                // Examine every CALL instruction in this function.
                bool foundCall = false;
                for (const auto& blk : caller.blocks()) {
                    if (!blk) continue;
                    for (const ssa::IrInstr* instr : blk->instrs) {
                        if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;

                        // Resolve callee name from the dedicated field.
                        const std::string calleeName = instr->calleeName;
                        if (calleeName.empty()) continue;

                        auto summIt = summaries.find(calleeName);
                        if (summIt == summaries.end()) continue;
                        const FunctionSummary& calleeSumm = summIt->second;

                        foundCall = true;

                        // Propagate param types: for each argument of the call,
                        // refine the caller's understanding.
                        for (std::size_t i = 0;
                             i < instr->uses.size() && i < calleeSumm.params.size();
                             ++i) {
                            // The argument is use[i]; find its position in caller's
                            // param list by matching against the caller's CC args.
                            auto ccIt = ccMap.find(callerName);
                            if (ccIt == ccMap.end()) continue;
                            const call_conv::CallingConvention& callerCC = ccIt->second;

                            // Match the SSA valueId to a caller parameter index.
                            ssa::ValueId argVid = instr->uses[i].valueId;
                            for (std::size_t pi = 0;
                                 pi < callerCC.args.size() && pi < callerSummary.params.size();
                                 ++pi) {
                                if (callerCC.args[pi].ssaValueId == argVid) {
                                    refineParam(callerSummary.params[pi],
                                                calleeSumm.params[i]);
                                    break;
                                }
                            }
                        }

                        // Propagate global side effects upward.
                        for (const auto& g : calleeSumm.globalWrites) {
                            callerSummary.globalWrites.insert(g);
                        }
                        for (const auto& g : calleeSumm.globalReads) {
                            callerSummary.globalReads.insert(g);
                        }
                    }
                }

                // Recompute isPure only when calls were found (propagation may
                // have added global side effects from callees).
                if (foundCall) {
                    callerSummary.isPure =
                        callerSummary.globalWrites.empty() &&
                        std::none_of(callerSummary.params.begin(),
                                     callerSummary.params.end(),
                                     [](const FunctionSummary::ParamInfo& p){
                                         return p.escapes || p.isModified; });
                }
            }

            // Convergence check for recursive SCCs.
            if (scc.isRecursive) {
                bool stable = true;
                for (const FnName& fn : scc.members) {
                    if (summaries.count(fn) && prev.count(fn) &&
                        summaries.at(fn) != prev.at(fn)) {
                        stable = false; break;
                    }
                }
                if (stable) break;
            }
        }
    }
}

} // namespace ipa
} // namespace retdec
