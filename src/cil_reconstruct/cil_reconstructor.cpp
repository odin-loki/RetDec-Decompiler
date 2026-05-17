/**
 * @file src/cil_reconstruct/cil_reconstructor.cpp
 * @brief CIL reconstruction orchestrator.
 */

#include "retdec/cil_reconstruct/cil_reconstructor.h"

#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <unordered_set>

namespace retdec {
namespace cil_reconstruct {

// ─── CilReconstructor ─────────────────────────────────────────────────────────

CilReconstructor::CilReconstructor(CilReconstructOptions opts)
    : opts_(std::move(opts)) {}

// ─── methodKey ───────────────────────────────────────────────────────────────

std::string CilReconstructor::methodKey(const BcClass& cls, const BcMethod& m) {
    return cls.fqName + "::" + m.name;
}

bool CilReconstructor::methodNeedsReconstruction(const BcMethod& m) {
    return m.cfg.blockCount() > 0;
}

// ─── detectLoop ──────────────────────────────────────────────────────────────

bool CilReconstructor::detectLoop(
        const std::vector<uint32_t>& blockIds,
        const BcCFG& cfg,
        uint32_t& headerBlock,
        std::vector<uint32_t>& loopBody,
        std::vector<uint32_t>& exitBlocks) const {
    // Simple back-edge detection: if any block has a successor that is
    // earlier in the ordering (dominator-based loop detection would be better,
    // but this works for typical CIL patterns)
    std::unordered_map<uint32_t, size_t> order;
    for (size_t i = 0; i < blockIds.size(); ++i) order[blockIds[i]] = i;

    for (size_t i = 0; i < blockIds.size(); ++i) {
        uint32_t bid = blockIds[i];
        if (bid >= cfg.blockCount()) continue;
        const auto& blk = cfg.block(bid);

        for (uint32_t succ : blk.succs) {
            auto it = order.find(succ);
            if (it != order.end() && it->second < i) {
                // Back edge: bid → succ
                headerBlock = succ;
                // Everything from header to bid is in the loop body
                loopBody.clear();
                for (size_t j = it->second; j <= i; ++j)
                    loopBody.push_back(blockIds[j]);
                // Exit blocks: successors of loop body blocks outside the loop
                std::unordered_set<uint32_t> loopSet(loopBody.begin(), loopBody.end());
                for (uint32_t lb : loopBody) {
                    if (lb >= cfg.blockCount()) continue;
                    for (uint32_t lsucc : cfg.block(lb).succs) {
                        if (!loopSet.count(lsucc))
                            exitBlocks.push_back(lsucc);
                    }
                }
                return true;
            }
        }
    }
    return false;
}

// ─── buildIfElse ─────────────────────────────────────────────────────────────

std::vector<CilStmt> CilReconstructor::buildIfElse(
        uint32_t condBlock,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg,
        const std::vector<uint32_t>& outerBlocks) const {
    std::vector<CilStmt> stmts;
    if (condBlock >= blocks.size()) return stmts;

    auto& rb = blocks[condBlock];
    if (rb.succs.size() < 2) {
        stmts = std::move(rb.stmts);
        return stmts;
    }

    // Find the branch statement
    CilExprPtr cond;
    for (auto& s : rb.stmts) {
        if (s.kind == StmtKind::If) {
            cond = s.expr;
            break;
        }
        stmts.push_back(std::move(s));
    }

    if (!cond) {
        stmts = std::move(rb.stmts);
        return stmts;
    }

    // Then branch = first successor, Else branch = second successor
    uint32_t thenBlock = rb.succs[0];
    uint32_t elseBlock = rb.succs[1];

    CilStmt ifStmt;
    ifStmt.kind = StmtKind::If;
    ifStmt.expr = cond;
    // We don't structurally nest here — the structuring pass handles that.
    // Emit as If + Goto pattern.
    ifStmt.blockRef = thenBlock;
    stmts.push_back(std::move(ifStmt));

    // Emit goto for else if it's not a fall-through
    if (elseBlock != condBlock + 1) {
        CilStmt gotoElse;
        gotoElse.kind      = StmtKind::Goto;
        gotoElse.blockRef  = elseBlock;
        gotoElse.labelName = "L" + std::to_string(elseBlock);
        stmts.push_back(std::move(gotoElse));
    }

    return stmts;
}

// ─── buildWhile ──────────────────────────────────────────────────────────────

std::vector<CilStmt> CilReconstructor::buildWhile(
        uint32_t headerBlock,
        const std::vector<uint32_t>& loopBlockIds,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg) const {
    std::vector<CilStmt> stmts;
    if (headerBlock >= blocks.size()) return stmts;

    // Find the condition (last If statement in header block)
    CilExprPtr cond;
    std::vector<CilStmt> headerPre;
    uint32_t exitBlock = 0;

    auto& header = blocks[headerBlock];
    for (auto& s : header.stmts) {
        if (s.kind == StmtKind::If) {
            cond = s.expr;
            exitBlock = s.blockRef;
        } else {
            headerPre.push_back(std::move(s));
        }
    }

    // Emit pre-condition statements
    stmts.insert(stmts.end(), headerPre.begin(), headerPre.end());

    // Build while loop
    CilStmt whileStmt;
    whileStmt.kind = StmtKind::If; // Placeholder — emitter handles while
    whileStmt.expr = cond;

    // Collect loop body statements (excluding header)
    std::unordered_set<uint32_t> headerSet = {headerBlock};
    for (uint32_t lb : loopBlockIds) {
        if (lb == headerBlock) continue;
        if (lb >= blocks.size()) continue;
        for (auto& s : blocks[lb].stmts) {
            if (s.kind != StmtKind::Goto ||
                s.blockRef != headerBlock) {
                whileStmt.loopBody.push_back(std::move(s));
            }
        }
        blocks[lb].stmts.clear();
    }
    header.stmts.clear();

    stmts.push_back(std::move(whileStmt));
    return stmts;
}

// ─── buildSwitch ─────────────────────────────────────────────────────────────

std::vector<CilStmt> CilReconstructor::buildSwitch(
        uint32_t switchBlock,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg) const {
    std::vector<CilStmt> stmts;
    if (switchBlock >= blocks.size()) return stmts;

    auto& rb = blocks[switchBlock];
    for (auto& s : rb.stmts) {
        stmts.push_back(std::move(s));
    }
    rb.stmts.clear();
    return stmts;
}

// ─── structureEH ─────────────────────────────────────────────────────────────

std::vector<CilStmt> CilReconstructor::structureEH(
        const std::vector<uint32_t>& blockIds,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg) const {
    std::vector<CilStmt> stmts;

    // For each EH region, build Try/Catch/Finally structures
    for (const auto& eh : cfg.handlers()) {
        // Find the try body blocks
        std::vector<CilStmt> tryBody;
        for (uint32_t bid : blockIds) {
            if (bid >= blocks.size()) continue;
            auto& rb = blocks[bid];
            if (!rb.stmts.empty() && !rb.isEHEntry) {
                for (auto& s : rb.stmts)
                    tryBody.push_back(std::move(s));
                rb.stmts.clear();
            }
        }

        // Build Try statement
        CilStmt tryStmt;
        tryStmt.kind    = StmtKind::Try;
        tryStmt.tryBody = std::move(tryBody);

        // Build catch/finally clause
        uint32_t hb = eh.handlerBlock;
        if (hb < blocks.size()) {
            std::vector<CilStmt> handlerBody;
            for (auto& s : blocks[hb].stmts)
                handlerBody.push_back(std::move(s));
            blocks[hb].stmts.clear();

            if (eh.isFinally) {
                tryStmt.finallyBody = std::move(handlerBody);
            } else if (eh.isFault) {
                tryStmt.faultBody = std::move(handlerBody);
            } else {
                CilStmt::CatchClause cc;
                if (eh.catchType.has_value())
                    cc.catchType = *eh.catchType;
                cc.body = std::move(handlerBody);
                tryStmt.catches.push_back(std::move(cc));
            }
        }

        if (!tryStmt.tryBody.empty() ||
            !tryStmt.catches.empty() ||
            !tryStmt.finallyBody.empty()) {
            stmts.push_back(std::move(tryStmt));
        }
    }

    return stmts;
}

// ─── structureRegion ─────────────────────────────────────────────────────────

std::vector<CilStmt> CilReconstructor::structureRegion(
        const std::vector<uint32_t>& blockIds,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg,
        int depth) const {
    std::vector<CilStmt> stmts;
    if (depth > opts_.maxStructureDepth) {
        // Fallback: emit gotos
        for (uint32_t bid : blockIds) {
            if (bid >= blocks.size()) continue;
            for (auto& s : blocks[bid].stmts)
                stmts.push_back(std::move(s));
            blocks[bid].stmts.clear();
        }
        return stmts;
    }

    // Try to detect EH structure first
    if (opts_.structureExcept && !cfg.handlers().empty()) {
        auto ehStmts = structureEH(blockIds, blocks, cfg);
        if (!ehStmts.empty()) {
            return ehStmts;
        }
    }

    // Try to detect a loop
    if (opts_.structureLoops) {
        uint32_t headerBlock = 0;
        std::vector<uint32_t> loopBody, exitBlocks;
        if (detectLoop(blockIds, cfg, headerBlock, loopBody, exitBlocks)) {
            auto loopStmts = buildWhile(headerBlock, loopBody, blocks, cfg);
            stmts.insert(stmts.end(), loopStmts.begin(), loopStmts.end());

            // Continue with blocks outside the loop
            std::unordered_set<uint32_t> loopSet(loopBody.begin(), loopBody.end());
            std::vector<uint32_t> remaining;
            for (uint32_t bid : blockIds)
                if (!loopSet.count(bid)) remaining.push_back(bid);

            auto restStmts = structureRegion(remaining, blocks, cfg, depth + 1);
            stmts.insert(stmts.end(), restStmts.begin(), restStmts.end());
            return stmts;
        }
    }

    // Fall-through: emit blocks in order with if-else structuring
    for (size_t i = 0; i < blockIds.size(); ++i) {
        uint32_t bid = blockIds[i];
        if (bid >= blocks.size()) continue;

        auto& rb = blocks[bid];
        if (rb.stmts.empty()) continue;

        // Check if this block ends with a 2-way conditional
        bool hasConditional = false;
        for (const auto& s : rb.stmts) {
            if (s.kind == StmtKind::If) { hasConditional = true; break; }
        }

        if (opts_.structureExcept == false && hasConditional && rb.succs.size() == 2) {
            auto ifStmts = buildIfElse(bid, blocks, cfg, blockIds);
            stmts.insert(stmts.end(), ifStmts.begin(), ifStmts.end());
        } else {
            // Check if this block ends with a switch
            bool hasSwitch = false;
            for (const auto& s : rb.stmts) {
                if (s.kind == StmtKind::Switch) { hasSwitch = true; break; }
            }
            if (opts_.structureSwitch && hasSwitch && rb.succs.size() > 2) {
                auto swStmts = buildSwitch(bid, blocks, cfg);
                stmts.insert(stmts.end(), swStmts.begin(), swStmts.end());
            } else {
                for (auto& s : rb.stmts)
                    stmts.push_back(std::move(s));
                rb.stmts.clear();
            }
        }
    }

    return stmts;
}

// ─── structureBlocks ─────────────────────────────────────────────────────────

std::vector<CilStmt> CilReconstructor::structureBlocks(
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg) const {
    // Collect all block IDs in topological order
    std::vector<uint32_t> allIds;
    allIds.reserve(blocks.size());
    for (const auto& rb : blocks)
        allIds.push_back(rb.id);

    return structureRegion(allIds, blocks, cfg, 0);
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

CilReconstructResult CilReconstructor::reconstruct(
        const BcMethod& method,
        const BcModule& module) const {
    CilReconstructResult result;
    result.success = false;

    if (method.cfg.blockCount() == 0) {
        // No body (abstract/native/extern)
        result.method.method = &method;
        result.success       = true;
        return result;
    }

    const BcCFG& cfg = method.cfg;

    // Phase 1 & 2: Stack simulation
    CilStackSimulator sim(opts_.stackOpts);
    if (!sim.simulate(cfg, method)) {
        result.error = sim.error();
        return result;
    }

    // Phase 3: Variable recovery
    CilVarRecovery recovery(opts_.varOpts);
    result.method = recovery.recover(method.cfg, method, sim);

    // Phase 4: Control-flow structuring
    result.method.body = structureBlocks(result.method.blocks, method.cfg);

    // Phase 5: Pattern detection
    CilPatternDetector detector(opts_.patternOpts);
    detector.detect(result.method, module);

    // Collect statistics
    result.blockCount  = static_cast<uint32_t>(result.method.blocks.size());
    result.stmtCount   = static_cast<uint32_t>(result.method.body.size());
    result.hasAsync    = result.method.isAsync;
    result.hasIterator = result.method.isIterator;
    result.hasLinq     = result.method.hasLinq;
    result.hasUnsafe   = result.method.hasUnsafe;
    result.hasPatternMatch = result.method.hasPatternMatch;

    // Count residual gotos
    for (const auto& s : result.method.body) {
        if (s.kind == StmtKind::Goto) ++result.gotoCount;
    }

    result.success = true;
    return result;
}

// ─── reconstructAll ──────────────────────────────────────────────────────────

std::unordered_map<std::string, CilReconstructResult>
CilReconstructor::reconstructAll(const BcModule& module) const {
    std::unordered_map<std::string, CilReconstructResult> results;

    for (const auto& cls : module.classes()) {
        for (const auto& m : cls.methods) {
            if (!methodNeedsReconstruction(m)) continue;
            auto key = methodKey(cls, m);
            results[key] = reconstruct(m, module);
        }
    }
    return results;
}

} // namespace cil_reconstruct
} // namespace retdec
