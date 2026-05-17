/**
 * @file src/cfg_structure/irreducibility.cpp
 * @brief CFG reducibility check via DFS edge classification.
 *
 * A CFG is reducible iff every back-edge (u→v where v is an ancestor of u
 * in the DFS tree) has a target v that dominates u.  We detect irreducibility
 * by running DFS and checking each back-edge's target against the
 * already-computed idom tree from `SSAFunction`.
 *
 * Algorithm:
 *   1. Run DFS, tracking discovery time (disc) and colour (white/grey/black).
 *   2. A grey target → back-edge.  Check if target dominates source.
 *      - If yes: natural loop back-edge (reducible).
 *      - If no:  irreducible back-edge.
 *   3. Record all edges with their classification.
 */

#include "retdec/cfg_structure/cfg_structure.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <stack>

namespace retdec {
namespace cfg_structure {

// ─── IrreducibilityCheck::dominates ──────────────────────────────────────────

// Check if block 'a' dominates block 'b' using the idom tree stored in
// BasicBlock::idom fields.  Walk up from b until we reach a or the entry.
bool IrreducibilityCheck::dominates(const std::vector<BlockId>& idom,
                                     const std::vector<uint32_t>& disc,
                                     BlockId a, BlockId b) const {
    if (a == b) return true;
    if (a >= idom.size() || b >= idom.size()) return false;

    // Use disc times: a dominates b iff disc[a] <= disc[b] (in the idom tree,
    // walking up idom from b must hit a before the root/itself).
    // Since we have the idom array, walk the idom chain from b upward.
    BlockId cur = b;
    std::size_t steps = idom.size() + 1;
    while (steps-- && cur != ssa::kInvalidBlock) {
        if (cur == a) return true;
        if (cur == idom[cur]) break;  // reached root / cycle guard
        cur = idom[cur];
    }
    return false;
}

// ─── IrreducibilityCheck::dfs ─────────────────────────────────────────────────

void IrreducibilityCheck::dfs(BlockId v,
                               const ssa::SSAFunction& fn,
                               std::vector<uint8_t>& color,
                               std::vector<BlockId>& parent,
                               std::vector<uint32_t>& disc,
                               const std::vector<BlockId>& idom,
                               Result& res,
                               uint32_t& timer) const {
    color[v] = 1;  // grey
    disc[v]  = timer++;

    const ssa::BasicBlock* bb = fn.block(v);
    if (!bb) return;

    for (BlockId w : bb->succs) {
        if (w >= color.size()) continue;

        if (color[w] == 0) {
            // Tree edge
            parent[w] = v;
            res.allEdges.push_back({v, w, EdgeKind::Tree});
            dfs(w, fn, color, parent, disc, idom, res, timer);
        } else if (color[w] == 1) {
            // Back edge — w is grey (ancestor)
            if (dominates(idom, disc, w, v)) {
                res.backEdges.push_back({v, w, EdgeKind::Back});
                res.allEdges.push_back({v, w, EdgeKind::Back});
            } else {
                res.irreducibleEdges.push_back({v, w, EdgeKind::IrreducibleBack});
                res.allEdges.push_back({v, w, EdgeKind::IrreducibleBack});
                res.isReducible = false;
            }
        } else {
            // color[w] == 2 (black)
            // Could be a forward or cross edge.
            if (disc[w] > disc[v]) {
                res.allEdges.push_back({v, w, EdgeKind::Forward});
            } else {
                res.allEdges.push_back({v, w, EdgeKind::Cross});
            }
        }
    }

    color[v] = 2;  // black
}

// ─── IrreducibilityCheck::run ─────────────────────────────────────────────────

IrreducibilityCheck::Result
IrreducibilityCheck::run(const ssa::SSAFunction& fn) const {
    Result res;
    const std::size_t n = fn.blocks().size();
    if (n == 0) return res;

    // Build idom array from BasicBlock::idom fields.
    std::vector<BlockId> idom(n, ssa::kInvalidBlock);
    for (const auto& blk : fn.blocks()) {
        if (blk && blk->id < n) {
            idom[blk->id] = blk->idom;
        }
    }

    std::vector<uint8_t>  color(n, 0);
    std::vector<BlockId>  parent(n, ssa::kInvalidBlock);
    std::vector<uint32_t> disc(n, 0);
    uint32_t timer = 0;

    // DFS from entry (always block 0).
    dfs(fn.entryId(), fn, color, parent, disc, idom, res, timer);

    // Visit any unreachable blocks (disconnected components) for completeness.
    for (BlockId b = 0; b < static_cast<BlockId>(n); ++b) {
        if (color[b] == 0) {
            dfs(b, fn, color, parent, disc, idom, res, timer);
        }
    }

    return res;
}

} // namespace cfg_structure
} // namespace retdec
