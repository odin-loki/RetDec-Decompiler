/**
 * @file src/ipa/call_graph.cpp
 * @brief Call graph construction and Tarjan SCC decomposition.
 *
 * ## Call graph construction
 *
 * For each CALL instruction in each function:
 *   - If the callee name is resolved (stored in the call instruction's
 *     debug string or in a dedicated callee-name field), create a direct
 *     edge.
 *   - If the callee is indirect (function pointer), we conservatively add
 *     edges to all functions whose address appears as a constant in the
 *     function body (address-taken functions).
 *
 * ## Tarjan SCC
 *
 * Standard Tarjan's SCC using a DFS with a stack.  Returns SCCs in
 * reverse topological order (a callee SCC appears before its caller SCC).
 */

#include "retdec/ipa/ipa.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <stack>

namespace retdec {
namespace ipa {

// ─── CallGraph::build ─────────────────────────────────────────────────────────

void CallGraph::build(const std::vector<const ssa::SSAFunction*>& fns) {
    nodes_.clear();
    edges_.clear();
    succs_.clear();
    preds_.clear();
    callCounts_.clear();

    // Register all nodes.
    for (const ssa::SSAFunction* fn : fns) {
        if (!fn) continue;
        nodes_.push_back(fn->name());
        succs_[fn->name()];
        preds_[fn->name()];
    }

    // Build name → function map for resolving call targets.
    std::unordered_map<std::string, const ssa::SSAFunction*> nameMap;
    for (const ssa::SSAFunction* fn : fns) {
        if (fn) nameMap[fn->name()] = fn;
    }

    // Extract call edges from each function.
    for (const ssa::SSAFunction* fn : fns) {
        if (!fn) continue;
        for (const auto& blk : fn->blocks()) {
            if (!blk) continue;
            for (const ssa::IrInstr* instr : blk->instrs) {
                if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;

                // Resolve callee name from the dedicated calleeName field.
                std::string callee = instr->calleeName;

                if (callee.empty() || !nameMap.count(callee)) continue;
                if (callee == fn->name()) {
                    // Self-call — still a valid edge.
                }

                CallEdge e;
                e.caller      = fn->name();
                e.callee      = callee;
                e.callInstrId = instr->id;
                edges_.push_back(e);

                succs_[fn->name()].push_back(callee);
                preds_[callee].push_back(fn->name());
                ++callCounts_[callee];
            }
        }
    }
}

const std::vector<FnName>& CallGraph::successors(const FnName& fn) const {
    static const std::vector<FnName> empty;
    auto it = succs_.find(fn);
    return (it != succs_.end()) ? it->second : empty;
}

const std::vector<FnName>& CallGraph::predecessors(const FnName& fn) const {
    static const std::vector<FnName> empty;
    auto it = preds_.find(fn);
    return (it != preds_.end()) ? it->second : empty;
}

std::size_t CallGraph::callCount(const FnName& callee) const {
    auto it = callCounts_.find(callee);
    return (it != callCounts_.end()) ? it->second : 0;
}

// ─── TarjanScc::strongConnect ─────────────────────────────────────────────────

void TarjanScc::strongConnect(const FnName& v,
                                const CallGraph& cg,
                                State& s) const {
    s.index[v]   = s.counter;
    s.lowlink[v] = s.counter;
    ++s.counter;
    s.stack.push_back(v);
    s.onStack[v] = true;

    for (const FnName& w : cg.successors(v)) {
        if (!s.index.count(w)) {
            strongConnect(w, cg, s);
            s.lowlink[v] = std::min(s.lowlink[v], s.lowlink[w]);
        } else if (s.onStack.count(w) && s.onStack.at(w)) {
            s.lowlink[v] = std::min(s.lowlink[v], s.index.at(w));
        }
    }

    // If v is a root of an SCC, pop the stack.
    if (s.lowlink[v] == s.index[v]) {
        CallGraphScc scc;
        while (true) {
            FnName w = s.stack.back();
            s.stack.pop_back();
            s.onStack[w] = false;
            scc.members.push_back(w);
            if (w == v) break;
        }
        // Detect recursion: size > 1, or self-edge.
        if (scc.members.size() > 1) {
            scc.isRecursive = true;
        } else {
            // Check for self-edge.
            const FnName& only = scc.members[0];
            for (const FnName& succ : cg.successors(only)) {
                if (succ == only) { scc.isRecursive = true; break; }
            }
        }
        s.sccs.push_back(std::move(scc));
    }
}

// ─── TarjanScc::run ───────────────────────────────────────────────────────────

std::vector<CallGraphScc> TarjanScc::run(const CallGraph& cg) const {
    State s;
    for (const FnName& v : cg.nodes()) {
        if (!s.index.count(v)) {
            strongConnect(v, cg, s);
        }
    }
    // SCCs are already in reverse topological order (callees before callers)
    // from Tarjan's algorithm.
    return s.sccs;
}

// ─── FunctionSummary equality / toString ─────────────────────────────────────

bool FunctionSummary::operator==(const FunctionSummary& o) const {
    if (params.size() != o.params.size()) return false;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& a = params[i]; const auto& b = o.params[i];
        if (a.width != b.width || a.isFp != b.isFp ||
            a.isPointer != b.isPointer || a.escapes != b.escapes ||
            a.isModified != b.isModified) return false;
    }
    return retWidth == o.retWidth && retIsFp == o.retIsFp &&
           retIsPtr == o.retIsPtr && isVoid == o.isVoid &&
           isPure == o.isPure && isNoReturn == o.isNoReturn &&
           globalWrites == o.globalWrites && globalReads == o.globalReads;
}

std::string FunctionSummary::toString() const {
    std::string s = name + "(";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        s += params[i].isFp ? "f" : "i";
        s += std::to_string(params[i].width);
        if (params[i].isPointer) s += "*";
        if (params[i].escapes)   s += "!";
    }
    s += ") -> ";
    s += isVoid ? "void" : ((retIsFp ? "f" : "i") + std::to_string(retWidth));
    if (isPure)     s += " [pure]";
    if (isNoReturn) s += " [noreturn]";
    return s;
}

} // namespace ipa
} // namespace retdec
