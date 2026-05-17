/**
 * @file src/cfg_structure/sese_decomp.cpp
 * @brief SESE region decomposition using DFS timestamps and post-dominator tree.
 *
 * A Single-Entry Single-Exit (SESE) region is characterised by:
 *   - Exactly one entry edge into the region.
 *   - Exactly one exit edge out of the region.
 *
 * We identify SESE regions as pairs (h, e) where:
 *   - h is a block with outdegree ≥ 2 (branch / loop header).
 *   - e = ipdom(h), the immediate post-dominator of h.
 *   - All paths from h to e stay within the DFS subtree rooted at h.
 *
 * DFS timestamps:
 *   - disc_[v]  = discovery time of v   (set when DFS first visits v).
 *   - fin_[v]   = finish time of v      (set when DFS finishes all successors).
 *
 * Containment: node v is inside the region [h, e] iff
 *   disc_[h] ≤ disc_[v]  and  fin_[v] ≤ fin_[h].
 *
 * This gives O(1) containment checks.
 */

#include "retdec/cfg_structure/cfg_structure.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <unordered_set>

namespace retdec {
namespace cfg_structure {

// ─── SESEDecomposer::dfsTimestamp ─────────────────────────────────────────────

void SESEDecomposer::dfsTimestamp(BlockId v,
                                   const ssa::SSAFunction& fn,
                                   std::unordered_set<BlockId>& visited,
                                   uint32_t& timer) {
    if (visited.count(v)) return;
    visited.insert(v);
    disc_[v] = timer++;

    const ssa::BasicBlock* bb = fn.block(v);
    if (bb) {
        for (BlockId w : bb->succs) {
            dfsTimestamp(w, fn, visited, timer);
        }
    }
    fin_[v] = timer++;
}

// ─── SESEDecomposer::run ──────────────────────────────────────────────────────

void SESEDecomposer::run(const ssa::SSAFunction& fn, const PostDomTree& pdom) {
    regions_.clear();
    disc_.clear();
    fin_.clear();

    if (fn.blocks().empty()) return;

    // Step 1: DFS timestamp pass
    std::unordered_set<BlockId> visited;
    uint32_t timer = 0;
    dfsTimestamp(fn.entryId(), fn, visited, timer);

    // Step 2: For each block with ≥ 2 successors, check if ipdom gives a SESE.
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        BlockId h = blk->id;

        if (blk->succs.size() < 2) continue;  // single successor → no branch

        BlockId e = pdom.ipostdom(h);
        if (e == kInvalidBlock) continue;      // no post-dom (exit block)
        if (!disc_.count(e)) continue;         // e not visited (unreachable)

        // Verify that e is reachable from h and that disc/fin are valid.
        if (disc_[h] > disc_[e]) continue;     // e discovered before h → not inside
        if (fin_[e] > fin_[h]) continue;       // e finishes after h → not inside

        // Collect all nodes inside the region [h, e].
        SESERegion region;
        region.entry     = h;
        region.exit_node = e;
        region.entryTime = disc_[h];
        region.exitTime  = fin_[h];

        for (const auto& member : fn.blocks()) {
            if (!member) continue;
            BlockId v = member->id;
            if (!disc_.count(v) || !fin_.count(v)) continue;
            if (region.contains(disc_[v], fin_[v]) && v != e) {
                region.nodes.push_back(v);
            }
        }

        regions_.push_back(std::move(region));
    }
}

// ─── SESEDecomposer::contains ─────────────────────────────────────────────────

bool SESEDecomposer::contains(const SESERegion& r, BlockId v) const {
    auto itD = disc_.find(v);
    auto itF = fin_.find(v);
    if (itD == disc_.end() || itF == fin_.end()) return false;
    uint32_t dv = itD->second, fv = itF->second;

    // v must be within the DFS subtree of the entry node.
    if (!r.contains(dv, fv)) return false;

    // Nodes in the exit node's DFS subtree (including the exit itself) are
    // considered outside the region [entry, exit).
    BlockId e = r.exit_node;
    auto itDe = disc_.find(e);
    auto itFe = fin_.find(e);
    if (itDe != disc_.end() && itFe != fin_.end()) {
        if (itDe->second <= dv && fv <= itFe->second) return false;
    }

    return true;
}

uint32_t SESEDecomposer::discTime(BlockId b) const {
    auto it = disc_.find(b);
    return (it != disc_.end()) ? it->second : UINT32_MAX;
}

uint32_t SESEDecomposer::finTime(BlockId b) const {
    auto it = fin_.find(b);
    return (it != fin_.end()) ? it->second : UINT32_MAX;
}

} // namespace cfg_structure
} // namespace retdec
