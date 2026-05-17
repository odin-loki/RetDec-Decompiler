/**
 * @file src/ssa/domtree.cpp
 * @brief Lengauer-Tarjan dominator tree and dominance frontier computation.
 *
 * ## Algorithm
 *
 * Thomas Lengauer and Robert Endre Tarjan, "A fast algorithm for finding
 * dominators in a flowgraph" (TOPLAS 1979).
 *
 * The algorithm runs in O(n α(n)) where n is the number of nodes and α is the
 * inverse Ackermann function (effectively constant for any practical input).
 *
 * ### Step 1: DFS numbering
 *
 * Assign DFS timestamps to every block.  vertex_[i] is the block with
 * DFS timestamp i.  parent_[v] is the DFS parent of v.
 *
 * ### Step 2: Semi-dominator computation
 *
 * For each block w in reverse DFS order (from largest timestamp to smallest),
 * compute semi_[w] = the minimum DFS number over all vertices u such that
 * there exists a path u → w whose intermediate nodes all have DFS number
 * greater than w (i.e. they were discovered after w in the DFS).
 *
 * ### Step 3: Implicit IDOM computation
 *
 * Use Lengauer-Tarjan's bucket+link/eval structure to compute immediate
 * dominators from semi-dominators.
 *
 * ### Step 4: Explicit IDOM
 *
 * A second pass converts implicit idom pointers to explicit idom[w].
 *
 * ### Step 5: Dominance frontiers (Cooper, Harvey, Kennedy 2001)
 *
 * For each join point j (block with ≥ 2 predecessors):
 *   For each predecessor p of j:
 *     runner = p
 *     while runner ≠ idom(j):
 *       DF[runner] ∪= {j}
 *       runner = idom(runner)
 *
 * This is Cooper's simple O(n²) algorithm which is fast enough for
 * function-level CFGs (typically < 1000 blocks).
 */

#include "retdec/ssa/ssa.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <numeric>
#include <vector>

namespace retdec {
namespace ssa {

// ─── DFS traversal ───────────────────────────────────────────────────────────

void DominatorTree::computeDFS(SSAFunction& fn) {
    std::size_t n = fn.blockCount();
    vertex_.assign(n, kInvalidBlock);
    parent_.assign(n, kInvalidBlock);
    semi_.assign(n, 0);
    ancestor_.assign(n, kInvalidBlock);
    labelArr_.assign(n, kInvalidBlock);
    bucket_.assign(n, {});

    uint32_t counter = 0;
    std::vector<bool> visited(n, false);

    std::function<void(BlockId)> dfs = [&](BlockId v) {
        visited[v] = true;
        semi_[v]  = counter;
        vertex_[counter] = v;
        fn.block(v)->rpo = counter;
        labelArr_[v] = v;
        ++counter;

        for (BlockId w : fn.block(v)->succs) {
            if (!visited[w]) {
                parent_[w] = v;
                dfs(w);
            }
        }
    };

    dfs(fn.entryId());
    // Handle disconnected blocks (unreachable from entry)
    for (std::size_t i = 0; i < n; ++i)
        if (!visited[i]) {
            parent_[i] = fn.entryId();
            fn.block(i)->rpo = counter;
            vertex_[counter] = (BlockId)i;
            labelArr_[i] = (BlockId)i;
            ++counter;
        }
}

// ─── Link-Eval with path compression ─────────────────────────────────────────

void DominatorTree::link(SSAFunction& fn, BlockId v, BlockId w) {
    ancestor_[w] = v;
    (void)fn;
}

void DominatorTree::compress(SSAFunction& fn, BlockId v) {
    if (ancestor_[ancestor_[v]] == kInvalidBlock) return;
    compress(fn, ancestor_[v]);
    if (semi_[labelArr_[ancestor_[v]]] < semi_[labelArr_[v]])
        labelArr_[v] = labelArr_[ancestor_[v]];
    ancestor_[v] = ancestor_[ancestor_[v]];
}

BlockId DominatorTree::eval(SSAFunction& fn, BlockId v) {
    if (ancestor_[v] == kInvalidBlock) return v;
    compress(fn, v);
    return labelArr_[v];
}

// ─── IDOM computation ─────────────────────────────────────────────────────────

void DominatorTree::computeIDom(SSAFunction& fn) {
    std::size_t n = fn.blockCount();
    std::vector<BlockId> idom(n, kInvalidBlock);

    // Process in reverse DFS order (skip root at index 0)
    for (int i = (int)n - 1; i >= 1; --i) {
        BlockId w = vertex_[i];
        if (w == kInvalidBlock) continue;

        // Compute semi-dominator
        for (BlockId v : fn.block(w)->preds) {
            if (!fn.block(v)) continue;
            BlockId u = eval(fn, v);
            if (semi_[u] < semi_[w])
                semi_[w] = semi_[u];
        }
        bucket_[vertex_[semi_[w]]].push_back(w);
        link(fn, parent_[w], w);

        // Step 3: implicitly set idom
        for (BlockId v : bucket_[parent_[w]]) {
            BlockId u = eval(fn, v);
            idom[v] = (semi_[u] < semi_[v]) ? u : parent_[w];
        }
        bucket_[parent_[w]].clear();
    }

    // Step 4: set explicit idom
    for (int i = 1; i < (int)n; ++i) {
        BlockId w = vertex_[i];
        if (w == kInvalidBlock) continue;
        if (idom[w] != vertex_[semi_[w]])
            idom[w] = idom[idom[w]];
        fn.block(w)->idom = idom[w];
    }
    fn.block(fn.entryId())->idom = kInvalidBlock;

    // Build domChildren
    for (std::size_t i = 0; i < n; ++i) {
        BlockId bid = (BlockId)i;
        BlockId parent = fn.block(bid)->idom;
        if (parent != kInvalidBlock && fn.block(parent))
            fn.block(parent)->domChildren.push_back(bid);
    }
}

// ─── Dominance frontier (Cooper 2001) ────────────────────────────────────────

void DominatorTree::computeDomFrontiers(SSAFunction& fn) {
    for (auto& blkPtr : fn.blocks())
        blkPtr->domFrontier.clear();

    for (auto& blkPtr : fn.blocks()) {
        BasicBlock& b = *blkPtr;
        if (b.preds.size() < 2) continue;  // only join points

        for (BlockId pred : b.preds) {
            BlockId runner = pred;
            while (runner != b.idom && runner != kInvalidBlock) {
                // Add b to DF[runner]
                auto& df = fn.block(runner)->domFrontier;
                if (std::find(df.begin(), df.end(), b.id) == df.end())
                    df.push_back(b.id);
                runner = fn.block(runner)->idom;
            }
        }
    }
}

// ─── Main entry point ─────────────────────────────────────────────────────────

void DominatorTree::run(SSAFunction& fn) {
    computeDFS(fn);
    computeIDom(fn);
    computeDomFrontiers(fn);
}

// ─── Dominance queries ────────────────────────────────────────────────────────

bool DominatorTree::dominates(const SSAFunction& fn, BlockId a, BlockId b) const {
    if (a == b) return true;
    return strictlyDominates(fn, a, b);
}

bool DominatorTree::strictlyDominates(const SSAFunction& fn,
                                        BlockId a, BlockId b) const {
    if (a == b) return false;
    // Walk up the idom chain from b toward root; if we reach a, a doms b.
    BlockId cur = fn.block(b) ? fn.block(b)->idom : kInvalidBlock;
    while (cur != kInvalidBlock) {
        if (cur == a) return true;
        cur = fn.block(cur) ? fn.block(cur)->idom : kInvalidBlock;
    }
    return false;
}

} // namespace ssa
} // namespace retdec
