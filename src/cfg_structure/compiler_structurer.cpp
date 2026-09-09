/**
 * @file src/cfg_structure/compiler_structurer.cpp
 * @brief Recursive SESE-based CFG structurer and CfgStructurePass orchestrator.
 *
 * ## Structuring Algorithm
 *
 * `structure(entry, exitNode, ...)` processes one SESE region [entry, exitNode]:
 *
 *  1. If entry == exitNode → stop (return null; the caller emits the exit block).
 *  2. If entry is already visited → emit a Goto (cycle / irreducible).
 *  3. If entry is a loop header → build a loop node:
 *       - Recurse into the loop body (SESE region [bodyEntry, header]).
 *       - Emit While / DoWhile / For / Infinite depending on LoopKind.
 *       - Then continue structuring from the loop's exit.
 *  4. If entry has a single successor → emit a Block node, continue.
 *  5. If entry ends in a CondBranch (2 successors):
 *       - Compute reconvergence = ipdom(entry).
 *       - If one branch == reconvergence → IfThen.
 *       - Else → IfThenElse; recurse into then-branch and else-branch.
 *       - Continue from reconvergence.
 *  6. If entry has ≥ 3 successors → Switch node.
 *  7. If irreducible → emit Goto with comment.
 *
 * The result is assembled into a Sequence at the top level.
 */

#include <memory>
#include "retdec/cfg_structure/cfg_structure.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <unordered_set>

namespace retdec {
namespace cfg_structure {

// ─── CompilerStructurer helpers ───────────────────────────────────────────────

const NaturalLoop*
CompilerStructurer::findLoop(BlockId header,
                              const std::vector<NaturalLoop>& loops) const {
    for (const NaturalLoop& lp : loops) {
        if (lp.header == header) return &lp;
    }
    return nullptr;
}

bool CompilerStructurer::isLoopHeader(BlockId b,
                                       const std::vector<NaturalLoop>& loops) const {
    for (const NaturalLoop& lp : loops) {
        if (lp.header == b) return true;
    }
    return false;
}

bool CompilerStructurer::isIrreducibleEntry(
        BlockId b,
        const IrreducibilityCheck::Result& irred) const {
    for (const CfgEdge& e : irred.irreducibleEdges) {
        if (e.dst == b) return true;
    }
    return false;
}

uint32_t CompilerStructurer::condValueOf(BlockId b,
                                          const ssa::SSAFunction& fn) const {
    const ssa::BasicBlock* bb = fn.block(b);
    if (!bb || bb->instrs.empty()) return ssa::kInvalidValue;
    const ssa::IrInstr* last = bb->instrs.back();
    if (!last) return ssa::kInvalidValue;
    if (last->op != ssa::IrInstr::Op::CondBranch) return ssa::kInvalidValue;
    // The first use operand of the CondBranch is the condition value.
    if (!last->uses.empty()) return last->uses[0].valueId;
    return last->flagBundleInput;
}

// ─── CompilerStructurer::structure ────────────────────────────────────────────

std::unique_ptr<StructNode>
CompilerStructurer::structure(
        BlockId entry,
        BlockId exitNode,
        const ssa::SSAFunction& fn,
        const PostDomTree& pdom,
        const SESEDecomposer& sese,
        const std::vector<NaturalLoop>& loops,
        const IrreducibilityCheck::Result& irred,
        std::unordered_set<BlockId>& visited,
        const Config& cfg) const {

    auto seq = StructNode::seq();

    BlockId cur = entry;
    while (cur != kInvalidBlock && cur != exitNode) {
        if (cur >= fn.blocks().size()) break;

        // Already visited → back edge or irreducible → emit goto.
        if (visited.count(cur)) {
            seq->children.push_back(StructNode::gotoNode(cur, isIrreducibleEntry(cur, irred)));
            break;
        }
        visited.insert(cur);

        const ssa::BasicBlock* bb = fn.block(cur);
        if (!bb) break;

        // ── Loop header ──────────────────────────────────────────────────────
        if (isLoopHeader(cur, loops)) {
            const NaturalLoop* lp = findLoop(cur, loops);
            std::unique_ptr<StructNode> loopNode;
            std::unique_ptr<StructNode> body;

            if (lp) {
				// The latch is NOT pre-marked visited. It used to be, "so body
				// structuring stops there" -- but for the ordinary two-block
				// while loop the latch IS the body entry, so the recursion
				// below saw it already visited and emitted a goto in place of
				// the body: `while (c) { x++; }` structured as a While whose
				// only child was `Goto b2`. Bounding the walk is the exitNode
				// argument's job, and which block ends the body depends on
				// where the loop keeps its condition:
				//
				//   while / for : the header holds the condition, so the body
				//                 starts at the header's in-body successor and
				//                 ends when the walk comes back to the header.
				//   do-while    : the latch holds the condition, so the body
				//                 starts at the header itself -- which the
				//                 caller has already marked visited, hence the
				//                 erase -- and ends at the latch.
				//
				// Bounding a do-while's body by the header instead let the walk
				// run past the latch's exit edge and pull the block after the
				// loop inside it, emitting that block twice.
				std::unordered_set<BlockId> bodyVisited = visited;
				std::unordered_set<BlockId> bodySet(lp->body.begin(), lp->body.end());
				const bool isDoWhile = (lp->kind == LoopKind::DoWhile);

				BlockId bodyEntry = kInvalidBlock;
				for (BlockId s: bb->succs)
				{
					if (bodySet.count(s) && s != cur)
					{
						bodyEntry = s;
						break;
					}
				}
				const BlockId bodyExit = isDoWhile ? lp->latch : cur;

				auto rest = (bodyEntry != kInvalidBlock && bodyEntry != bodyExit)
							  ? structure(bodyEntry, bodyExit, fn, pdom, sese, loops, irred, bodyVisited, cfg)
							  : nullptr;

				if (isDoWhile)
				{
					// The header is the first statement of the body, not a
					// condition. Emit it directly rather than recursing into
					// it -- the recursion would see a loop header again and
					// not come back.
					body = StructNode::seq();
					body->children.push_back(StructNode::block(cur));
					if (rest) body->children.push_back(std::move(rest));
				}
				else
				{
					body = rest ? std::move(rest) : StructNode::seq();
				}

				uint32_t condVal = condValueOf(cur, fn);

				switch (lp->kind)
				{
				case LoopKind::For:
					if (cfg.preferFor && lp->hasIncrement)
					{
						loopNode = StructNode::forLoop(condVal, lp->inductionVar);
					}
					else
					{
						loopNode = StructNode::whileLoop(condVal);
					}
					break;
                case LoopKind::DoWhile: {
                    // For do-while the condition is at the latch.
                    uint32_t latchCond = condValueOf(lp->latch, fn);
                    loopNode = StructNode::doWhileLoop(latchCond);
                    break;
                }
                case LoopKind::Infinite:
                    loopNode = StructNode::infinite();
                    break;
                default:
                    loopNode = StructNode::whileLoop(condVal);
                    break;
				}

				if (body) loopNode->children.push_back(std::move(body));
				seq->children.push_back(std::move(loopNode));

				// Continue structuring from the loop exit.
				//
				// NaturalLoop::exits holds the body blocks that HAVE an edge
				// leaving the loop -- not the blocks those edges lead to. Every
				// one of them is in the body by construction, so the filter
				// that used to be here, "skip any exits that are still inside
				// the body", could never fire, and cur was left pointing at a
				// body block the walk had already visited: everything after a
				// while or for loop came out as a single goto and the tail of
				// the function was dropped from the tree. Follow the edge.
				cur = kInvalidBlock;
				for (BlockId ex: lp->exits)
				{
					const ssa::BasicBlock* exBb = fn.block(ex);
					if (!exBb) continue;
					for (BlockId s: exBb->succs)
					{
						if (!bodySet.count(s))
						{
							cur = s;
							break;
						}
					}
					if (cur != kInvalidBlock) break;
				}
				continue;
			}
		}

		// ── Conditional branch (if-then / if-then-else) ──────────────────────
        if (bb->succs.size() == 2) {
            BlockId s0 = bb->succs[0];
            BlockId s1 = bb->succs[1];
            BlockId ipdom = pdom.ipostdom(cur);
            uint32_t condVal = condValueOf(cur, fn);

            // One branch goes directly to the post-dominator → IfThen.
            if (s0 == ipdom || s1 == ipdom) {
                auto ifNode = StructNode::ifThen(condVal);
                BlockId thenEntry = (s0 == ipdom) ? s1 : s0;
                std::unordered_set<BlockId> thenVisited = visited;
                auto thenBody = structure(thenEntry, ipdom, fn, pdom, sese,
                                          loops, irred, thenVisited, cfg);
                if (thenBody) ifNode->children.push_back(std::move(thenBody));
                seq->children.push_back(std::move(ifNode));
                cur = ipdom;
            } else {
                // Both branches lead to distinct nodes → IfThenElse.
                auto ifNode = StructNode::ifThenElse(condVal);

                std::unordered_set<BlockId> thenVisited = visited;
                auto thenBody = structure(s0, ipdom, fn, pdom, sese,
                                          loops, irred, thenVisited, cfg);

                std::unordered_set<BlockId> elseVisited = visited;
                // Merge thenVisited into visited (union).
                visited.insert(thenVisited.begin(), thenVisited.end());
                auto elseBody = structure(s1, ipdom, fn, pdom, sese,
                                          loops, irred, elseVisited, cfg);
                visited.insert(elseVisited.begin(), elseVisited.end());

                if (thenBody) ifNode->children.push_back(std::move(thenBody));
                if (elseBody) ifNode->children.push_back(std::move(elseBody));
                seq->children.push_back(std::move(ifNode));
                cur = ipdom;
            }
            continue;
        }

        // ── Switch (≥ 3 successors) ──────────────────────────────────────────
        if (bb->succs.size() >= 3) {
            auto sw = std::make_unique<StructNode>();
            sw->kind = StructNode::Kind::Switch;
            sw->condValueId = condValueOf(cur, fn);
            BlockId ipdom = pdom.ipostdom(cur);

            int64_t caseVal = 0;
            for (BlockId succ : bb->succs) {
                if (succ == ipdom) {
                    sw->defaultCase = structure(succ, ipdom, fn, pdom, sese,
                                                 loops, irred, visited, cfg);
                } else {
                    std::unordered_set<BlockId> caseVisited = visited;
                    auto caseBody = structure(succ, ipdom, fn, pdom, sese,
                                              loops, irred, caseVisited, cfg);
                    visited.insert(caseVisited.begin(), caseVisited.end());
                    sw->cases.emplace_back(caseVal++, std::move(caseBody));
                }
            }
            seq->children.push_back(std::move(sw));
            cur = ipdom;
            continue;
        }

        // ── Single successor (straight-line) ─────────────────────────────────
        seq->children.push_back(StructNode::block(cur));
        cur = bb->succs.empty() ? kInvalidBlock : bb->succs[0];
    }

    // Flatten a sequence with a single child.
    if (seq->children.size() == 1) return std::move(seq->children[0]);
    if (seq->children.empty())     return StructNode::seq();
    return seq;
}

// ─── CompilerStructurer::run ──────────────────────────────────────────────────

std::unique_ptr<StructNode>
CompilerStructurer::run(const ssa::SSAFunction& fn,
                         const Config& cfg) const {
    if (fn.blocks().empty()) return StructNode::seq();

    // Run the sub-passes.
    IrreducibilityCheck irredCheck;
    auto irred = irredCheck.run(fn);

    PostDomTree pdom;
    pdom.run(fn);

    SESEDecomposer sese;
    sese.run(fn, pdom);

    // Build idom array for LoopRecovery.
    const std::size_t n = fn.blocks().size();
    std::vector<BlockId> idom(n, ssa::kInvalidBlock);
    for (const auto& blk : fn.blocks()) {
        if (blk && blk->id < n) idom[blk->id] = blk->idom;
    }

    LoopRecovery loopRec;
    auto loops = loopRec.run(fn, irred, idom);

    std::unordered_set<BlockId> visited;
    return structure(fn.entryId(), kInvalidBlock,
                     fn, pdom, sese, loops, irred, visited, cfg);
}

// ─── CfgStructurePass::countNodes ────────────────────────────────────────────

void CfgStructurePass::countNodes(const StructNode* node, Stats& s) const {
    if (!node) return;
    switch (node->kind) {
    case StructNode::Kind::While:      ++s.whileLoops;   ++s.loopsFound; break;
    case StructNode::Kind::DoWhile:    ++s.doWhileLoops; ++s.loopsFound; break;
    case StructNode::Kind::For:        ++s.forLoops;     ++s.loopsFound; break;
    case StructNode::Kind::Infinite:   ++s.infiniteLoops;++s.loopsFound; break;
    case StructNode::Kind::IfThen:     ++s.ifThenNodes;     break;
    case StructNode::Kind::IfThenElse: ++s.ifThenElseNodes; break;
    case StructNode::Kind::Switch:     ++s.switchNodes;      break;
    case StructNode::Kind::Goto:       ++s.gotoNodes;        break;
    default: break;
    }
    for (const auto& child : node->children) countNodes(child.get(), s);
    for (const auto& [val, child] : node->cases) countNodes(child.get(), s);
    if (node->defaultCase) countNodes(node->defaultCase.get(), s);
}

// ─── CfgStructurePass::run ───────────────────────────────────────────────────

std::unique_ptr<StructNode>
CfgStructurePass::run(const ssa::SSAFunction& fn,
                       const CompilerStructurer::Config& cfg) {
    stats_ = Stats{};
    loops_.clear();

    if (fn.blocks().empty()) return StructNode::seq();

    // Irreducibility check (also stored for callers).
    IrreducibilityCheck irredCheck;
    irred_ = irredCheck.run(fn);
    stats_.isReducible = irred_.isReducible;

    // SESE region count.
    PostDomTree pdom;
    pdom.run(fn);
    SESEDecomposer sese;
    sese.run(fn, pdom);
    stats_.seseRegions = sese.regions().size();

    // Loop recovery (stored for callers).
    const std::size_t n = fn.blocks().size();
    std::vector<BlockId> idom(n, ssa::kInvalidBlock);
    for (const auto& blk : fn.blocks()) {
        if (blk && blk->id < n) idom[blk->id] = blk->idom;
    }
    LoopRecovery loopRec;
    loops_ = loopRec.run(fn, irred_, idom);

    // Full structuring.
    CompilerStructurer structurer;
    auto root = structurer.run(fn, cfg);

    countNodes(root.get(), stats_);
    return root;
}

} // namespace cfg_structure
} // namespace retdec
