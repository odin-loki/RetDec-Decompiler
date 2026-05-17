/**
 * @file src/cfg_structure/post_domtree.cpp
 * @brief Post-dominator tree via Lengauer-Tarjan on the reversed CFG.
 *
 * The post-dominator tree is the dominator tree of the reversed CFG.  We add
 * a virtual exit node (id = blockCount) with edges FROM all terminal blocks
 * (blocks with no successors or explicit `ret` instructions).
 *
 * Lengauer-Tarjan on the reversed CFG:
 *   - Predecessors in the reversed CFG = successors in the original CFG.
 *   - Successors  in the reversed CFG = predecessors in the original CFG.
 *   - The virtual exit node has predecessors = all blocks with
 *     succs.empty() in the original CFG.
 *
 * After `run()`, `ipostdom(b)` returns the immediate post-dominator of b,
 * and `postDominates(a, b)` checks whether every path from b to any exit
 * passes through a.
 */

#include "retdec/cfg_structure/cfg_structure.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace retdec {
namespace cfg_structure {

// ─── Internal Lengauer-Tarjan implementation ──────────────────────────────────

namespace {

struct LTNode {
    uint32_t              semi     = 0;
    uint32_t              label    = 0;
    uint32_t              ancestor = UINT32_MAX;
    uint32_t              parent   = UINT32_MAX;
    std::vector<uint32_t> bucket;
    uint32_t              dom      = UINT32_MAX;
    uint32_t              vertex   = UINT32_MAX; // DFS visit order → node index
};

struct LTGraph {
    uint32_t              n       = 0;     // number of nodes incl. virtual exit
    uint32_t              exitId  = 0;     // virtual exit node index
    uint32_t              entryId = 0;     // virtual exit (DFS root for post-dom)
    std::vector<std::vector<uint32_t>> pred;  // predecessors in REVERSED cfg
    std::vector<std::vector<uint32_t>> succ;  // successors in REVERSED cfg
    std::vector<LTNode>   nodes;
    std::vector<uint32_t> order;          // DFS post-order
    uint32_t              timer = 0;

    void compress(uint32_t v) {
        if (nodes[v].ancestor == UINT32_MAX) return;
        if (nodes[nodes[v].ancestor].ancestor != UINT32_MAX) {
            compress(nodes[v].ancestor);
            if (nodes[nodes[nodes[v].ancestor].label].semi
                    < nodes[nodes[v].label].semi) {
                nodes[v].label = nodes[nodes[v].ancestor].label;
            }
            nodes[v].ancestor = nodes[nodes[v].ancestor].ancestor;
        }
    }

    uint32_t eval(uint32_t v) {
        if (nodes[v].ancestor == UINT32_MAX) return v;
        compress(v);
        return nodes[v].label;
    }

    void link(uint32_t v, uint32_t w) {
        nodes[w].ancestor = v;
    }

    void dfs(uint32_t v) {
        nodes[v].semi   = timer;
        nodes[v].label  = v;
        order.push_back(v);
        nodes[order[nodes[v].semi]].vertex = v;
        timer++;
        for (uint32_t w : succ[v]) {
            if (nodes[w].semi == UINT32_MAX) {
                nodes[w].parent = v;
                dfs(w);
            }
        }
    }

    // Lengauer-Tarjan dominator algorithm.
    // Returns idom[] indexed by node id.
    std::vector<uint32_t> compute() {
        // Initialise
        for (uint32_t i = 0; i < n; ++i) {
            nodes[i].semi     = UINT32_MAX;
            nodes[i].label    = i;
            nodes[i].ancestor = UINT32_MAX;
            nodes[i].dom      = UINT32_MAX;
        }
        order.clear();
        order.reserve(n);
        timer = 0;

        // Step 1: DFS from virtual entry (= virtual exit of original CFG)
        dfs(entryId);

        uint32_t N = static_cast<uint32_t>(order.size());
        // Steps 2 & 3: compute semi-dominators, link, compute idom
        for (uint32_t i = N - 1; i >= 1; --i) {
            uint32_t w = order[i];
            // Step 2: compute semidominator
            for (uint32_t v : pred[w]) {
                if (nodes[v].semi == UINT32_MAX) continue;  // unreachable
                uint32_t u = eval(v);
                if (nodes[u].semi < nodes[w].semi) {
                    nodes[w].semi = nodes[u].semi;
                }
            }
            nodes[order[nodes[w].semi]].bucket.push_back(w);
            link(nodes[w].parent, w);

            // Step 3: implicitly compute idom
            for (uint32_t v : nodes[nodes[w].parent].bucket) {
                uint32_t u = eval(v);
                nodes[v].dom = (nodes[u].semi < nodes[v].semi)
                               ? u : nodes[w].parent;
            }
            nodes[nodes[w].parent].bucket.clear();
        }
        // Step 4: adjust idom
        for (uint32_t i = 1; i < N; ++i) {
            uint32_t w = order[i];
            if (nodes[w].dom != order[nodes[w].semi]) {
                nodes[w].dom = nodes[nodes[w].dom].dom;
            }
        }

        std::vector<uint32_t> idom(n, UINT32_MAX);
        for (uint32_t i = 0; i < n; ++i) idom[i] = nodes[i].dom;
        idom[entryId] = entryId;
        return idom;
    }
};

} // anonymous namespace

// ─── PostDomTree::run ─────────────────────────────────────────────────────────

void PostDomTree::run(const ssa::SSAFunction& fn) {
    const uint32_t blockCount = static_cast<uint32_t>(fn.blocks().size());
    if (blockCount == 0) return;

    // Virtual exit node has index = blockCount.
    const uint32_t vExit = blockCount;
    const uint32_t total = blockCount + 1;

    LTGraph g;
    g.n       = total;
    g.entryId = vExit;  // DFS root for post-dom = virtual exit of original
    g.exitId  = fn.entryId();
    g.pred.resize(total);
    g.succ.resize(total);
    g.nodes.resize(total);

    // Build reversed CFG edges.
    // Original: u → v  becomes reversed: v → u.
    // In reversed CFG:  pred[v] = {u : u→v in original}
    //                   succ[v] = {u : v→u in original} (= preds of v in original)
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        uint32_t u = blk->id;
        for (uint32_t v : blk->succs) {
            if (v >= blockCount) continue;
            // Original edge u→v, reversed edge v→u
            // reversed succ[v] += u;  reversed pred[u] += v
            g.succ[v].push_back(u);
            g.pred[u].push_back(v);
        }
        // If this block has no successors → it's a terminal → add edge to vExit
        if (blk->succs.empty()) {
            // In reversed CFG: vExit → u (succ of vExit = u)
            g.succ[vExit].push_back(u);
            g.pred[u].push_back(vExit);
        }
    }

    // Also connect vExit to the original entry so the virtual exit can
    // post-dominate it (original entry is always reachable from vExit in
    // the reversed CFG since everything is reachable from entry).
    // (Not needed for correctness; vExit is the unique root.)

    auto idomRaw = g.compute();

    // idomRaw[i] gives the index of i's immediate post-dominator in the
    // Lengauer-Tarjan numbering (which is the same as block IDs for 0..blockCount-1,
    // and blockCount for the virtual exit).
    ipdom_.resize(total, kInvalidBlock);
    for (uint32_t i = 0; i < total; ++i) {
        if (idomRaw[i] == UINT32_MAX) {
            ipdom_[i] = kInvalidBlock;
        } else if (idomRaw[i] == vExit) {
            ipdom_[i] = kInvalidBlock;  // virtual exit → no real post-dom
        } else {
            ipdom_[i] = static_cast<BlockId>(idomRaw[i]);
        }
    }
    // The virtual exit post-dominates itself.
    ipdom_[vExit] = kInvalidBlock;
}

// ─── PostDomTree::ipostdom ────────────────────────────────────────────────────

BlockId PostDomTree::ipostdom(BlockId b) const {
    if (b >= ipdom_.size()) return kInvalidBlock;
    return ipdom_[b];
}

// ─── PostDomTree::postDominates ───────────────────────────────────────────────

// a post-dominates b iff walking ipostdom chain from b eventually reaches a.
bool PostDomTree::postDominates(BlockId a, BlockId b) const {
    if (a == b) return true;
    if (a >= ipdom_.size() || b >= ipdom_.size()) return false;
    BlockId cur = ipdom_[b];
    std::size_t budget = ipdom_.size() + 1;
    while (budget-- && cur != kInvalidBlock) {
        if (cur == a) return true;
        if (cur >= ipdom_.size()) break;
        cur = ipdom_[cur];
    }
    return false;
}

} // namespace cfg_structure
} // namespace retdec
