/**
 * @file src/dce/unreachable_elim.cpp
 * @brief Forward reachability analysis to find unreachable basic blocks.
 *
 * A block is unreachable if no path from the function entry can reach it.
 * We compute this with a simple BFS from the entry block over the CFG edges.
 * Any block not visited is unreachable.
 */

#include "retdec/dce/dce.h"
#include "retdec/ssa/ssa.h"

#include <queue>

namespace retdec {
namespace dce {

std::unordered_set<BlockId> UnreachableEliminator::run(
        const ssa::SSAFunction& fn) const {

    const std::size_t n = fn.blocks().size();
    if (n == 0) return {};

    std::unordered_set<BlockId> reachable;
    std::queue<BlockId> worklist;

    worklist.push(fn.entryId());
    reachable.insert(fn.entryId());

    while (!worklist.empty()) {
        BlockId cur = worklist.front(); worklist.pop();
        const ssa::BasicBlock* bb = fn.block(cur);
        if (!bb) continue;
        for (BlockId succ : bb->succs) {
            if (reachable.insert(succ).second) {
                worklist.push(succ);
            }
        }
    }

    std::unordered_set<BlockId> unreachable;
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        if (!reachable.count(blk->id)) {
            unreachable.insert(blk->id);
        }
    }

    return unreachable;
}

} // namespace dce
} // namespace retdec
