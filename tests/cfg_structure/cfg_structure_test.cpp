/**
 * @file tests/cfg_structure/cfg_structure_test.cpp
 * @brief Unit tests for cfg_structure module (Stage 20).
 *
 * Test groups:
 *   1.  IrreducibilityCheck — basic reducible / irreducible detection.
 *   2.  PostDomTree         — immediate post-dominator correctness.
 *   3.  SESEDecomposer      — SESE region containment.
 *   4.  LoopRecovery        — while / for / do-while / infinite classification.
 *   5.  CompilerStructurer  — if-then / if-then-else / switch / goto structuring.
 *   6.  CfgStructurePass    — full pipeline integration + stats.
 */

#include "retdec/cfg_structure/cfg_structure.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>
#include <memory>

using namespace retdec::cfg_structure;
using namespace retdec::ssa;

// ─── Helper: build an SSAFunction CFG without actual instructions ─────────────

namespace {

/// Build a minimal CFG.  Blocks are numbered 0..n-1.
/// `edges` is a list of (src, dst) pairs.
/// `condBlocks` are block IDs whose terminator is a CondBranch.
/// `addBlocks` are block IDs whose terminator is an Add instruction (for loop).
std::unique_ptr<SSAFunction> makeCfg(
        std::size_t numBlocks,
        std::vector<std::pair<uint32_t,uint32_t>> edges,
        std::vector<uint32_t> condBlocks = {},
        std::vector<uint32_t> addBlocks  = {}) {

    auto fn = std::make_unique<SSAFunction>("test_fn");

    // Create blocks
    for (std::size_t i = 0; i < numBlocks; ++i) {
        fn->addBlock("bb" + std::to_string(i));
    }

    // Add edges
    for (auto [s, d] : edges) {
        BasicBlock* bbs = fn->block(s);
        BasicBlock* bbd = fn->block(d);
        if (bbs && bbd) {
            bbs->addSucc(d);
            bbd->addPred(s);
        }
    }

    // Add CondBranch terminator to condBlocks
    for (uint32_t b : condBlocks) {
        auto* instr = fn->addInstr(b, IrInstr::Op::CondBranch);
        // Give it a dummy condition use
        auto* val = fn->allocValue(ValueKind::VirtualReg, 0);
        val->width = 1;
        instr->uses.push_back({val->id, 0});
        instr->defValue = kInvalidValue;
    }

    // Add a branch terminator to all non-cond, non-terminal blocks
    for (std::size_t i = 0; i < numBlocks; ++i) {
        BasicBlock* bb = fn->block(static_cast<BlockId>(i));
        if (!bb) continue;
        bool hasCond = std::find(condBlocks.begin(), condBlocks.end(),
                                  static_cast<uint32_t>(i)) != condBlocks.end();
        bool hasAdd  = std::find(addBlocks.begin(), addBlocks.end(),
                                  static_cast<uint32_t>(i)) != addBlocks.end();
        if (!hasCond) {
            if (hasAdd) {
                auto* add = fn->addInstr(static_cast<BlockId>(i), IrInstr::Op::Add);
                auto* val = fn->allocValue(ValueKind::VirtualReg, 1);
                add->defValue = val->id;
            }
            if (!bb->succs.empty()) {
                fn->addInstr(static_cast<BlockId>(i), IrInstr::Op::Branch);
            } else {
                fn->addInstr(static_cast<BlockId>(i), IrInstr::Op::Ret);
            }
        }
    }

    // Run SSA pass so idom fields are populated.
    SSAPass pass;
    pass.run(*fn);

    return fn;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// 1. IrreducibilityCheck
// ═══════════════════════════════════════════════════════════════════════════════

TEST(IrreducibilityCheck, SingleBlock_IsReducible) {
    auto fn = makeCfg(1, {});
    IrreducibilityCheck chk;
    auto res = chk.run(*fn);
    EXPECT_TRUE(res.isReducible);
    EXPECT_TRUE(res.irreducibleEdges.empty());
}

TEST(IrreducibilityCheck, LinearChain_IsReducible) {
    // 0 → 1 → 2 → 3
    auto fn = makeCfg(4, {{0,1},{1,2},{2,3}});
    IrreducibilityCheck chk;
    auto res = chk.run(*fn);
    EXPECT_TRUE(res.isReducible);
    EXPECT_TRUE(res.irreducibleEdges.empty());
}

TEST(IrreducibilityCheck, SimpleLoop_IsReducible) {
    // 0 → 1 → 2 → 1 (back-edge 2→1, 1 dominates 2)
    auto fn = makeCfg(3, {{0,1},{1,2},{2,1}}, {1});
    IrreducibilityCheck chk;
    auto res = chk.run(*fn);
    EXPECT_TRUE(res.isReducible);
    EXPECT_EQ(res.backEdges.size(), 1u);
    EXPECT_EQ(res.backEdges[0].src, 2u);
    EXPECT_EQ(res.backEdges[0].dst, 1u);
}

TEST(IrreducibilityCheck, DiamondIfElse_IsReducible) {
    // 0 → 1, 0 → 2, 1 → 3, 2 → 3
    auto fn = makeCfg(4, {{0,1},{0,2},{1,3},{2,3}}, {0});
    IrreducibilityCheck chk;
    auto res = chk.run(*fn);
    EXPECT_TRUE(res.isReducible);
    EXPECT_TRUE(res.backEdges.empty());
    EXPECT_TRUE(res.irreducibleEdges.empty());
}

TEST(IrreducibilityCheck, IrreducibleCfg_TwoEntryLoop) {
    // Classic two-entry irreducible loop:
    // 0 → 1, 0 → 2, 1 → 2, 2 → 1
    // Neither 1 nor 2 dominates the other; 2→1 is irreducible.
    auto fn = makeCfg(3, {{0,1},{0,2},{1,2},{2,1}}, {0,1,2});
    IrreducibilityCheck chk;
    auto res = chk.run(*fn);
    EXPECT_FALSE(res.isReducible);
    EXPECT_FALSE(res.irreducibleEdges.empty());
}

TEST(IrreducibilityCheck, NestedLoops_BothReducible) {
    // 0 → 1 → 2 → 3 → 2 (inner), 3 → 1 (outer)
    auto fn = makeCfg(4, {{0,1},{1,2},{2,3},{3,2},{3,1}}, {2,3});
    IrreducibilityCheck chk;
    auto res = chk.run(*fn);
    EXPECT_TRUE(res.isReducible);
    EXPECT_EQ(res.backEdges.size(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 2. PostDomTree
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PostDomTree, LinearChain_EachPostDominatesNext) {
    // 0 → 1 → 2 → 3 (ret)
    // ipdom(0)=1, ipdom(1)=2, ipdom(2)=3, ipdom(3)=invalid
    auto fn = makeCfg(4, {{0,1},{1,2},{2,3}});
    PostDomTree pdom;
    pdom.run(*fn);
    EXPECT_EQ(pdom.ipostdom(0), 1u);
    EXPECT_EQ(pdom.ipostdom(1), 2u);
    EXPECT_EQ(pdom.ipostdom(2), 3u);
    EXPECT_EQ(pdom.ipostdom(3), UINT32_MAX);
}

TEST(PostDomTree, Diamond_MergePostDominatesBranch) {
    // 0 → {1,2}, 1 → 3, 2 → 3, 3 is the merge (ret)
    // ipdom(0) = 3 (every path 0→1→3 and 0→2→3 goes through 3)
    auto fn = makeCfg(4, {{0,1},{0,2},{1,3},{2,3}}, {0});
    PostDomTree pdom;
    pdom.run(*fn);
    EXPECT_EQ(pdom.ipostdom(0), 3u);
    EXPECT_EQ(pdom.ipostdom(1), 3u);
    EXPECT_EQ(pdom.ipostdom(2), 3u);
    EXPECT_EQ(pdom.ipostdom(3), UINT32_MAX);
}

TEST(PostDomTree, PostDominates_TransitiveClosure) {
    // 0 → 1 → 2 → 3
    // 3 post-dominates everything
    auto fn = makeCfg(4, {{0,1},{1,2},{2,3}});
    PostDomTree pdom;
    pdom.run(*fn);
    EXPECT_TRUE(pdom.postDominates(3, 0));
    EXPECT_TRUE(pdom.postDominates(3, 1));
    EXPECT_TRUE(pdom.postDominates(3, 2));
    EXPECT_TRUE(pdom.postDominates(3, 3));
    EXPECT_FALSE(pdom.postDominates(0, 3));
    // In chain 0→1→2→3, 1 post-dominates 0 (every path from 0 to exit goes through 1).
    EXPECT_TRUE(pdom.postDominates(1, 0));
}

TEST(PostDomTree, IfThen_PostDominator) {
    // 0 → {1, 2}, 1 → 2, 2 is exit
    // ipdom(0) = 2
    auto fn = makeCfg(3, {{0,1},{0,2},{1,2}}, {0});
    PostDomTree pdom;
    pdom.run(*fn);
    EXPECT_EQ(pdom.ipostdom(0), 2u);
    EXPECT_EQ(pdom.ipostdom(1), 2u);
}

TEST(PostDomTree, SingleBlock_NoPostDom) {
    auto fn = makeCfg(1, {});
    PostDomTree pdom;
    pdom.run(*fn);
    EXPECT_EQ(pdom.ipostdom(0), UINT32_MAX);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3. SESEDecomposer
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SESEDecomposer, Diamond_OneSESERegion) {
    // 0 → {1,2}, 1 → 3, 2 → 3
    auto fn = makeCfg(4, {{0,1},{0,2},{1,3},{2,3}}, {0});
    PostDomTree pdom; pdom.run(*fn);
    SESEDecomposer sese; sese.run(*fn, pdom);
    // Should find one SESE region: entry=0, exit=3
    bool found = false;
    for (const auto& r : sese.regions()) {
        if (r.entry == 0 && r.exit_node == 3) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(SESEDecomposer, LinearChain_NoSESE) {
    // 0 → 1 → 2 → 3: all single-successor, no branches → no SESE regions
    auto fn = makeCfg(4, {{0,1},{1,2},{2,3}});
    PostDomTree pdom; pdom.run(*fn);
    SESEDecomposer sese; sese.run(*fn, pdom);
    EXPECT_TRUE(sese.regions().empty());
}

TEST(SESEDecomposer, ContainmentCheck_O1) {
    // 0 → {1,2}, 1 → 3, 2 → 3 → 4
    auto fn = makeCfg(5, {{0,1},{0,2},{1,3},{2,3},{3,4}}, {0});
    PostDomTree pdom; pdom.run(*fn);
    SESEDecomposer sese; sese.run(*fn, pdom);
    // Region entry=0, exit=3 should contain blocks 1 and 2.
    for (const auto& r : sese.regions()) {
        if (r.entry == 0 && r.exit_node == 3) {
            EXPECT_TRUE(sese.contains(r, 1));
            EXPECT_TRUE(sese.contains(r, 2));
            EXPECT_FALSE(sese.contains(r, 4));
            break;
        }
    }
}

TEST(SESEDecomposer, NestedBranches_MultipleSESE) {
    // 0→{1,4}, 1→{2,3}, 2→4, 3→4, 4 is exit
    auto fn = makeCfg(5, {{0,1},{0,4},{1,2},{1,3},{2,4},{3,4}}, {0,1});
    PostDomTree pdom; pdom.run(*fn);
    SESEDecomposer sese; sese.run(*fn, pdom);
    EXPECT_GE(sese.regions().size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 4. LoopRecovery
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoopRecovery, WhileLoop) {
    // 0 → 1 (header, cond) → {2 (body) → 1, 3 (exit)}
    auto fn = makeCfg(4, {{0,1},{1,2},{1,3},{2,1}}, {1});
    SSAPass pass; pass.run(*fn);

    std::vector<BlockId> idom(fn->blocks().size(), UINT32_MAX);
    for (const auto& blk : fn->blocks()) {
        if (blk && blk->id < idom.size()) idom[blk->id] = blk->idom;
    }

    IrreducibilityCheck chk;
    auto edges = chk.run(*fn);
    EXPECT_TRUE(edges.isReducible);

    LoopRecovery rec;
    auto loops = rec.run(*fn, edges, idom);
    ASSERT_EQ(loops.size(), 1u);
    EXPECT_EQ(loops[0].header, 1u);
    EXPECT_EQ(loops[0].latch, 2u);
    EXPECT_EQ(loops[0].kind, LoopKind::While);
}

TEST(LoopRecovery, ForLoop_HasIncrement) {
    // Same structure as while, but latch (block 2) has an Add instruction.
    auto fn = makeCfg(4, {{0,1},{1,2},{1,3},{2,1}}, {1}, {2});
    SSAPass pass; pass.run(*fn);

    std::vector<BlockId> idom(fn->blocks().size(), UINT32_MAX);
    for (const auto& blk : fn->blocks()) {
        if (blk && blk->id < idom.size()) idom[blk->id] = blk->idom;
    }

    IrreducibilityCheck chk;
    auto edges = chk.run(*fn);

    LoopRecovery rec;
    auto loops = rec.run(*fn, edges, idom);
    ASSERT_EQ(loops.size(), 1u);
    EXPECT_EQ(loops[0].kind, LoopKind::For);
    EXPECT_TRUE(loops[0].hasIncrement);
}

TEST(LoopRecovery, DoWhileLoop) {
    // 0 → 1 (body, unconditional) → 2 (latch, cond) → {1 (back), 3 (exit)}
    auto fn = makeCfg(4, {{0,1},{1,2},{2,1},{2,3}}, {2});
    SSAPass pass; pass.run(*fn);

    std::vector<BlockId> idom(fn->blocks().size(), UINT32_MAX);
    for (const auto& blk : fn->blocks()) {
        if (blk && blk->id < idom.size()) idom[blk->id] = blk->idom;
    }

    IrreducibilityCheck chk;
    auto edges = chk.run(*fn);

    LoopRecovery rec;
    auto loops = rec.run(*fn, edges, idom);
    ASSERT_EQ(loops.size(), 1u);
    EXPECT_EQ(loops[0].kind, LoopKind::DoWhile);
}

TEST(LoopRecovery, InfiniteLoop) {
    // 0 → 1 → 1  (no exit)
    auto fn = makeCfg(2, {{0,1},{1,1}});
    SSAPass pass; pass.run(*fn);

    std::vector<BlockId> idom(fn->blocks().size(), UINT32_MAX);
    for (const auto& blk : fn->blocks()) {
        if (blk && blk->id < idom.size()) idom[blk->id] = blk->idom;
    }

    IrreducibilityCheck chk;
    auto edges = chk.run(*fn);

    LoopRecovery rec;
    auto loops = rec.run(*fn, edges, idom);
    ASSERT_EQ(loops.size(), 1u);
    EXPECT_EQ(loops[0].kind, LoopKind::Infinite);
    EXPECT_TRUE(loops[0].exits.empty());
}

TEST(LoopRecovery, NestedLoops_TwoLoopsFound) {
    // 0 → 1 → 2 → 3 → 2 (inner), 3 → 1 (outer), 1 → 4
    auto fn = makeCfg(5, {{0,1},{1,2},{2,3},{3,2},{3,1},{1,4}}, {1,2,3});
    SSAPass pass; pass.run(*fn);

    std::vector<BlockId> idom(fn->blocks().size(), UINT32_MAX);
    for (const auto& blk : fn->blocks()) {
        if (blk && blk->id < idom.size()) idom[blk->id] = blk->idom;
    }

    IrreducibilityCheck chk;
    auto edges = chk.run(*fn);

    LoopRecovery rec;
    auto loops = rec.run(*fn, edges, idom);
    EXPECT_EQ(loops.size(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 5. CompilerStructurer
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CompilerStructurer, SingleBlock_EmitsBlock) {
    auto fn = makeCfg(1, {});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);
    // Either a Block or an empty Sequence
    bool ok = (root->kind == StructNode::Kind::Block)
           || (root->kind == StructNode::Kind::Sequence && root->children.empty());
    EXPECT_TRUE(ok);
}

TEST(CompilerStructurer, LinearSequence) {
    // 0 → 1 → 2 → 3
    auto fn = makeCfg(4, {{0,1},{1,2},{2,3}});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);
    // A sequence of blocks
    EXPECT_EQ(root->kind, StructNode::Kind::Sequence);
}

TEST(CompilerStructurer, IfThen) {
    // 0 → {1, 2}, 1 → 2 (then-branch goes to merge)
    auto fn = makeCfg(3, {{0,1},{0,2},{1,2}}, {0});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);

    bool foundIfThen = false;
    std::function<void(const StructNode*)> scan = [&](const StructNode* n) {
        if (!n) return;
        if (n->kind == StructNode::Kind::IfThen) foundIfThen = true;
        for (const auto& c : n->children) scan(c.get());
    };
    scan(root.get());
    EXPECT_TRUE(foundIfThen);
}

TEST(CompilerStructurer, IfThenElse) {
    // 0 → {1, 2}, 1 → 3, 2 → 3
    auto fn = makeCfg(4, {{0,1},{0,2},{1,3},{2,3}}, {0});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);

    bool foundIfThenElse = false;
    std::function<void(const StructNode*)> scan = [&](const StructNode* n) {
        if (!n) return;
        if (n->kind == StructNode::Kind::IfThenElse) foundIfThenElse = true;
        for (const auto& c : n->children) scan(c.get());
    };
    scan(root.get());
    EXPECT_TRUE(foundIfThenElse);
}

TEST(CompilerStructurer, WhileLoop_Detected) {
    // 0 → 1 (cond) → {2 → 1, 3}
    auto fn = makeCfg(4, {{0,1},{1,2},{1,3},{2,1}}, {1});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);

    bool foundLoop = false;
    std::function<void(const StructNode*)> scan = [&](const StructNode* n) {
        if (!n) return;
        if (n->kind == StructNode::Kind::While ||
            n->kind == StructNode::Kind::For)   foundLoop = true;
        for (const auto& c : n->children) scan(c.get());
    };
    scan(root.get());
    EXPECT_TRUE(foundLoop);
}

TEST(CompilerStructurer, ForLoop_Detected) {
    // Same as while but with increment in latch (block 2)
    auto fn = makeCfg(4, {{0,1},{1,2},{1,3},{2,1}}, {1}, {2});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);

    bool foundFor = false;
    std::function<void(const StructNode*)> scan = [&](const StructNode* n) {
        if (!n) return;
        if (n->kind == StructNode::Kind::For) foundFor = true;
        for (const auto& c : n->children) scan(c.get());
    };
    scan(root.get());
    EXPECT_TRUE(foundFor);
}

TEST(CompilerStructurer, DoWhileLoop_Detected) {
    // 0 → 1 → 2 (cond) → {1, 3}
    auto fn = makeCfg(4, {{0,1},{1,2},{2,1},{2,3}}, {2});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);

    bool foundDoWhile = false;
    std::function<void(const StructNode*)> scan = [&](const StructNode* n) {
        if (!n) return;
        if (n->kind == StructNode::Kind::DoWhile) foundDoWhile = true;
        for (const auto& c : n->children) scan(c.get());
    };
    scan(root.get());
    EXPECT_TRUE(foundDoWhile);
}

TEST(CompilerStructurer, InfiniteLoop_Detected) {
    // 0 → 1 → 1 (no exit)
    auto fn = makeCfg(2, {{0,1},{1,1}});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);

    bool foundInfinite = false;
    std::function<void(const StructNode*)> scan = [&](const StructNode* n) {
        if (!n) return;
        if (n->kind == StructNode::Kind::Infinite) foundInfinite = true;
        for (const auto& c : n->children) scan(c.get());
    };
    scan(root.get());
    EXPECT_TRUE(foundInfinite);
}

TEST(CompilerStructurer, IrreducibleCfg_EmitsGoto) {
    // Classic irreducible loop: 0→1, 0→2, 1→2, 2→1
    auto fn = makeCfg(3, {{0,1},{0,2},{1,2},{2,1}}, {0,1,2});
    CompilerStructurer s;
    auto root = s.run(*fn);
    ASSERT_NE(root, nullptr);
    // There should be at least one Goto node (irreducible handling).
    bool foundGoto = false;
    std::function<void(const StructNode*)> scan = [&](const StructNode* n) {
        if (!n) return;
        if (n->kind == StructNode::Kind::Goto) foundGoto = true;
        for (const auto& c : n->children) scan(c.get());
    };
    scan(root.get());
    EXPECT_TRUE(foundGoto);
}

TEST(CompilerStructurer, StructNodeFactory_Kinds) {
    auto block = StructNode::block(5);
    EXPECT_EQ(block->kind, StructNode::Kind::Block);
    EXPECT_EQ(block->blockId, 5u);

    auto seq = StructNode::seq();
    EXPECT_EQ(seq->kind, StructNode::Kind::Sequence);

    auto ifThen = StructNode::ifThen(42);
    EXPECT_EQ(ifThen->kind, StructNode::Kind::IfThen);
    EXPECT_EQ(ifThen->condValueId, 42u);

    auto ite = StructNode::ifThenElse(7);
    EXPECT_EQ(ite->kind, StructNode::Kind::IfThenElse);

    auto wh = StructNode::whileLoop(3);
    EXPECT_EQ(wh->kind, StructNode::Kind::While);
    EXPECT_EQ(wh->loopKind, LoopKind::While);

    auto dw = StructNode::doWhileLoop(3);
    EXPECT_EQ(dw->kind, StructNode::Kind::DoWhile);
    EXPECT_EQ(dw->loopKind, LoopKind::DoWhile);

    auto fl = StructNode::forLoop(3, 10);
    EXPECT_EQ(fl->kind, StructNode::Kind::For);
    EXPECT_EQ(fl->loopKind, LoopKind::For);
    EXPECT_EQ(fl->inductionVar, 10u);

    auto inf = StructNode::infinite();
    EXPECT_EQ(inf->kind, StructNode::Kind::Infinite);
    EXPECT_EQ(inf->loopKind, LoopKind::Infinite);

    auto gt = StructNode::gotoNode(99, true);
    EXPECT_EQ(gt->kind, StructNode::Kind::Goto);
    EXPECT_EQ(gt->gotoTarget, 99u);
    EXPECT_TRUE(gt->isIrreducible);
}

TEST(CompilerStructurer, LoopKindName) {
    EXPECT_STREQ(loopKindName(LoopKind::While),    "while");
    EXPECT_STREQ(loopKindName(LoopKind::DoWhile),  "do-while");
    EXPECT_STREQ(loopKindName(LoopKind::For),      "for");
    EXPECT_STREQ(loopKindName(LoopKind::Infinite), "infinite");
}

TEST(CompilerStructurer, StructNodeKindName) {
    EXPECT_STREQ(StructNode::block(0)->kindName(), "Block");
    EXPECT_STREQ(StructNode::seq()->kindName(),    "Sequence");
    EXPECT_STREQ(StructNode::ifThen(0)->kindName(),"IfThen");
    EXPECT_STREQ(StructNode::infinite()->kindName(),"Infinite");
    EXPECT_STREQ(StructNode::gotoNode(0)->kindName(),"Goto");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 6. CfgStructurePass — integration + stats
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CfgStructurePass, EmptyFunction_Succeeds) {
    SSAFunction fn("empty");
    CfgStructurePass pass;
    auto root = pass.run(fn);
    EXPECT_NE(root, nullptr);
    EXPECT_TRUE(pass.stats().isReducible);
    EXPECT_EQ(pass.stats().loopsFound, 0u);
}

TEST(CfgStructurePass, SingleBlock_Reducible) {
    auto fn = makeCfg(1, {});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_TRUE(pass.stats().isReducible);
}

TEST(CfgStructurePass, WhileLoop_Stats) {
    // 0 → 1 (cond) → {2 → 1, 3}
    auto fn = makeCfg(4, {{0,1},{1,2},{1,3},{2,1}}, {1});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_TRUE(pass.stats().isReducible);
    EXPECT_GE(pass.stats().loopsFound, 1u);
}

TEST(CfgStructurePass, ForLoop_Stats) {
    auto fn = makeCfg(4, {{0,1},{1,2},{1,3},{2,1}}, {1}, {2});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_GE(pass.stats().forLoops, 1u);
}

TEST(CfgStructurePass, DoWhile_Stats) {
    auto fn = makeCfg(4, {{0,1},{1,2},{2,1},{2,3}}, {2});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_GE(pass.stats().doWhileLoops, 1u);
}

TEST(CfgStructurePass, IfThenElse_Stats) {
    auto fn = makeCfg(4, {{0,1},{0,2},{1,3},{2,3}}, {0});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_GE(pass.stats().ifThenElseNodes, 1u);
}

TEST(CfgStructurePass, IfThen_Stats) {
    // 0 → {1, 2}, 1 → 2
    auto fn = makeCfg(3, {{0,1},{0,2},{1,2}}, {0});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_GE(pass.stats().ifThenNodes, 1u);
}

TEST(CfgStructurePass, Irreducible_NotReducible) {
    auto fn = makeCfg(3, {{0,1},{0,2},{1,2},{2,1}}, {0,1,2});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_FALSE(pass.stats().isReducible);
    EXPECT_GE(pass.stats().gotoNodes, 1u);
}

TEST(CfgStructurePass, NestedLoops_CorrectCount) {
    // Two natural loops
    auto fn = makeCfg(5, {{0,1},{1,2},{2,3},{3,2},{3,1},{1,4}}, {1,2,3});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_TRUE(pass.stats().isReducible);
    EXPECT_EQ(pass.stats().loopsFound, 2u);
}

TEST(CfgStructurePass, LoopsAccessor_Matches_Stats) {
    auto fn = makeCfg(4, {{0,1},{1,2},{1,3},{2,1}}, {1});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_EQ(pass.loops().size(), pass.stats().loopsFound);
}

TEST(CfgStructurePass, SESERegions_Counted) {
    // Diamond: has 1 SESE region
    auto fn = makeCfg(4, {{0,1},{0,2},{1,3},{2,3}}, {0});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_GE(pass.stats().seseRegions, 1u);
}

TEST(CfgStructurePass, OutputRoot_NonNull) {
    auto fn = makeCfg(4, {{0,1},{1,2},{2,3}});
    CfgStructurePass pass;
    auto root = pass.run(*fn);
    EXPECT_NE(root, nullptr);
}

TEST(CfgStructurePass, Config_PreferFor) {
    // Disable for-loop preference
    auto fn = makeCfg(4, {{0,1},{1,2},{1,3},{2,1}}, {1}, {2});
    CompilerStructurer::Config cfg;
    cfg.preferFor = false;
    CfgStructurePass pass;
    auto root = pass.run(*fn, cfg);
    EXPECT_NE(root, nullptr);
    // Without preferFor, we get While instead of For
    bool foundFor = false;
    std::function<void(const StructNode*)> scan = [&](const StructNode* n) {
        if (!n) return;
        if (n->kind == StructNode::Kind::For) foundFor = true;
        for (const auto& c : n->children) scan(c.get());
    };
    scan(root.get());
    EXPECT_FALSE(foundFor);
}

TEST(CfgStructurePass, IrreducibilityAccessor) {
    auto fn = makeCfg(3, {{0,1},{0,2},{1,2},{2,1}}, {0,1,2});
    CfgStructurePass pass;
    pass.run(*fn);
    EXPECT_FALSE(pass.irreducibility().isReducible);
    EXPECT_FALSE(pass.irreducibility().irreducibleEdges.empty());
}
