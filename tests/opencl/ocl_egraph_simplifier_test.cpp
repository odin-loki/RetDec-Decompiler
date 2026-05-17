/**
 * @file tests/opencl/ocl_egraph_simplifier_test.cpp
 * @brief Unit tests for OCLEGraphSimplifier.
 *
 * Tests verify that equality saturation fires the expected rewrite rules
 * and that the C-distance extraction produces idiomatic C forms.
 */

#include "retdec/opencl/ocl_egraph_simplifier.h"

#include <gtest/gtest.h>

using namespace retdec::opencl;

// ─── Helper: build a trivial two-node tree ───────────────────────────────────

/**
 * Builds a graph for: op(LIT(lv), LIT(rv))
 * Returns {opNodeClass, litLClass, litRClass}
 */
static std::array<uint32_t, 3> makeBinaryLit(EGraph &g, EOpcode op,
                                              uint64_t lv, uint64_t rv,
                                              uint32_t &opNodeIdx)
{
    // Three e-classes: 0=result, 1=lhs literal, 2=rhs literal
    g.setClassCount(3);
    uint32_t li = g.addLiteral(1, lv);
    uint32_t ri = g.addLiteral(2, rv);
    opNodeIdx   = g.addNode(op, 0, 1, 2);
    return {0, 1, 2};
}

// ─── Tests ───────────────────────────────────────────────────────────────────

class EGraphTest : public ::testing::Test {
protected:
    OCLEGraphSimplifier simplifier{nullptr}; // CPU path
};

// ── Empty graph ──────────────────────────────────────────────────────────────

TEST_F(EGraphTest, EmptyGraphReturnsEmpty)
{
    EGraph g;
    auto r = simplifier.simplify(g);
    EXPECT_TRUE(r.empty());
}

// ── Rule 1: x + 0 → x ───────────────────────────────────────────────────────

TEST_F(EGraphTest, AddZeroIdentity)
{
    EGraph g;
    // Classes: 0=result, 1=x(var), 2=zero
    g.setClassCount(3);
    g.addVar(1, 42);
    g.addLiteral(2, 0);
    g.addNode(EOpcode::ADD, 0, 1, 2);

    auto r = simplifier.simplify(g);
    ASSERT_EQ(r.size(), 3u);
    // Class 0 (ADD result) should now merge with class 1 (x)
    uint32_t root0 = g.find(0);
    uint32_t root1 = g.find(1);
    EXPECT_EQ(root0, root1) << "ADD(x,0) class should merge with x class";
}

// ── Rule 1: x - 0 → x ───────────────────────────────────────────────────────

TEST_F(EGraphTest, SubZeroIdentity)
{
    EGraph g;
    g.setClassCount(3);
    g.addVar(1, 7);
    g.addLiteral(2, 0);
    g.addNode(EOpcode::SUB, 0, 1, 2);

    simplifier.simplify(g);
    EXPECT_EQ(g.find(0), g.find(1));
}

// ── Rule 1: x ^ 0 → x ───────────────────────────────────────────────────────

TEST_F(EGraphTest, XorZeroIdentity)
{
    EGraph g;
    g.setClassCount(3);
    g.addVar(1, 3);
    g.addLiteral(2, 0);
    g.addNode(EOpcode::XOR, 0, 1, 2);

    simplifier.simplify(g);
    EXPECT_EQ(g.find(0), g.find(1));
}

// ── Rule 2: x * 1 → x ───────────────────────────────────────────────────────

TEST_F(EGraphTest, MulOneIdentity)
{
    EGraph g;
    g.setClassCount(3);
    g.addVar(1, 5);
    g.addLiteral(2, 1);
    g.addNode(EOpcode::MUL, 0, 1, 2);

    simplifier.simplify(g);
    EXPECT_EQ(g.find(0), g.find(1));
}

// ── Rule 2: 1 * x → x (commutative) ────────────────────────────────────────

TEST_F(EGraphTest, MulOneIdentityCommutative)
{
    EGraph g;
    g.setClassCount(3);
    g.addLiteral(1, 1);
    g.addVar(2, 9);
    g.addNode(EOpcode::MUL, 0, 1, 2);

    simplifier.simplify(g);
    EXPECT_EQ(g.find(0), g.find(2));
}

// ── Rule 3: x * 0 → 0 ───────────────────────────────────────────────────────

TEST_F(EGraphTest, MulZeroIsZero)
{
    EGraph g;
    // 4 classes: 0=result, 1=var, 2=zero_lit, 3=another_zero (for verification)
    g.setClassCount(4);
    g.addVar(1, 1);
    uint32_t zeroNodeIdx = g.addLiteral(2, 0);  // zero literal in class 2
    g.addLiteral(3, 0);                           // another zero in class 3
    g.addNode(EOpcode::MUL, 0, 1, 2);

    simplifier.simplify(g);
    // Class 0 should now be in the same equivalence class as some LIT(0) node
    // (class 2 or 3 both hold LIT(0))
    uint32_t root0 = g.find(0);
    uint32_t root2 = g.find(2);
    EXPECT_EQ(root0, root2) << "MUL(x,0) should be equivalent to LIT(0)";
    (void)zeroNodeIdx;
}

// ── Rule 4: x >> 0 → x ──────────────────────────────────────────────────────

TEST_F(EGraphTest, ShrZeroIdentity)
{
    EGraph g;
    g.setClassCount(3);
    g.addVar(1, 10);
    g.addLiteral(2, 0);
    g.addNode(EOpcode::SHR, 0, 1, 2);

    simplifier.simplify(g);
    EXPECT_EQ(g.find(0), g.find(1));
}

// ── Rule 4: x << 0 → x ──────────────────────────────────────────────────────

TEST_F(EGraphTest, ShlZeroIdentity)
{
    EGraph g;
    g.setClassCount(3);
    g.addVar(1, 11);
    g.addLiteral(2, 0);
    g.addNode(EOpcode::SHL, 0, 1, 2);

    simplifier.simplify(g);
    EXPECT_EQ(g.find(0), g.find(1));
}

// ── Rule 5: (x >> k) & mask → BITFIELD ──────────────────────────────────────

TEST_F(EGraphTest, BitfieldExtraction)
{
    // Build: AND( SHR(x, LIT(4)), LIT(0x0F) )
    // Expect: BITFIELD node with opcode BITFIELD
    EGraph g;
    // Classes: 0=and_result, 1=shr_result, 2=x, 3=shift_k(=4), 4=mask(=0x0F)
    g.setClassCount(5);
    g.addVar(2, 0);               // x in class 2
    g.addLiteral(3, 4);           // shift amount 4 in class 3
    g.addLiteral(4, 0x0F);        // mask 0x0F in class 4
    g.addNode(EOpcode::SHR, 1, 2, 3); // SHR(x,4) → class 1
    uint32_t andIdx = g.addNode(EOpcode::AND, 0, 1, 4); // AND(shr,mask) → class 0

    simplifier.simplify(g);

    // The AND node should have been rewritten to BITFIELD
    EXPECT_EQ(static_cast<EOpcode>(g.op()[andIdx]), EOpcode::BITFIELD)
        << "AND(SHR(x,k),mask) should be rewritten to BITFIELD";
    EXPECT_EQ(g.aux()[andIdx], 4u) << "BITFIELD shift amount should be 4";
}

// ── Rule 6: CAST(CAST(x, 8), 4) → CAST(x, 4) ────────────────────────────────

TEST_F(EGraphTest, CastCollapsing)
{
    EGraph g;
    // Classes: 0=outer_cast, 1=inner_cast, 2=x
    g.setClassCount(3);
    g.addVar(2, 0);
    uint32_t innerIdx = g.addNode(EOpcode::CAST, 1, 2, kNoClass, 8); // CAST(x,8)
    uint32_t outerIdx = g.addNode(EOpcode::CAST, 0, 1, kNoClass, 4); // CAST(inner,4)

    simplifier.simplify(g);

    // Outer cast should now point directly to class 2 (x), bypassing inner
    EXPECT_EQ(g.lhs()[outerIdx], g.find(2))
        << "CAST(CAST(x,8),4) outer should bypass inner cast";
    (void)innerIdx;
}

// ── Rule 7a: DEREF(ADD(base, idx)) → ARRAY ───────────────────────────────────

TEST_F(EGraphTest, DerefAddBecomesArray)
{
    EGraph g;
    // Classes: 0=deref_result, 1=add_result, 2=base, 3=idx
    g.setClassCount(4);
    g.addVar(2, 0);  // base
    g.addVar(3, 1);  // index (variable, not literal)
    g.addNode(EOpcode::ADD, 1, 2, 3);
    uint32_t derefIdx = g.addNode(EOpcode::DEREF, 0, 1, kNoClass);

    simplifier.simplify(g);

    EXPECT_EQ(static_cast<EOpcode>(g.op()[derefIdx]), EOpcode::ARRAY)
        << "DEREF(ADD(base,idx)) should become ARRAY";
}

// ── Rule 7b: DEREF(ADD(base, LIT(offset))) → FIELD ───────────────────────────

TEST_F(EGraphTest, DerefAddLiteralBecomesField)
{
    EGraph g;
    // Classes: 0=deref_result, 1=add_result, 2=base, 3=offset(=8)
    g.setClassCount(4);
    g.addVar(2, 0);    // base pointer
    g.addLiteral(3, 8); // constant offset
    g.addNode(EOpcode::ADD, 1, 2, 3);
    uint32_t derefIdx = g.addNode(EOpcode::DEREF, 0, 1, kNoClass);

    simplifier.simplify(g);

    EXPECT_EQ(static_cast<EOpcode>(g.op()[derefIdx]), EOpcode::FIELD)
        << "DEREF(ADD(base,LIT(off))) should become FIELD";
    EXPECT_EQ(g.aux()[derefIdx], 8u) << "Field offset should be 8";
}

// ── Extraction: C-distance selects best representative ───────────────────────

TEST_F(EGraphTest, ExtractionSelectsLowestCDist)
{
    // Build: ADD(x, LIT(0)) in class 0 — after saturation, class 0 merges
    // with class 1 (x).  Both an ADD node (score=3) and a VAR node (score=0)
    // belong to the same e-class root.  Extraction should pick the VAR.
    EGraph g;
    g.setClassCount(3);
    g.addVar(1, 0);       // x → class 1, score 0
    g.addLiteral(2, 0);   // LIT(0) → class 2, score 0
    g.addNode(EOpcode::ADD, 0, 1, 2); // ADD → class 0, score 3

    auto results = simplifier.simplify(g);
    // Find the result for class 0's root
    uint32_t root0 = g.find(0);
    EClassResult *res = nullptr;
    for (auto &r : results) {
        if (g.find(r.classId) == root0) { res = &r; break; }
    }
    ASSERT_NE(res, nullptr);
    EXPECT_LE(res->score, 3u) << "Should pick VAR (score=0) over ADD (score=3)";
    EXPECT_EQ(res->opcode, EOpcode::VAR) << "Best node should be the VAR";
}

// ── No rules fire on already-idiomatic expressions ───────────────────────────

TEST_F(EGraphTest, ArrayAccessUnchanged)
{
    // ARRAY(base, idx) — no rule applies, opcode should stay ARRAY
    EGraph g;
    g.setClassCount(3);
    g.addVar(1, 0);
    g.addVar(2, 1);
    uint32_t arrIdx = g.addNode(EOpcode::ARRAY, 0, 1, 2);

    simplifier.simplify(g);
    EXPECT_EQ(static_cast<EOpcode>(g.op()[arrIdx]), EOpcode::ARRAY);
}

TEST_F(EGraphTest, FieldAccessUnchanged)
{
    EGraph g;
    g.setClassCount(2);
    g.addVar(1, 0);
    uint32_t fldIdx = g.addNode(EOpcode::FIELD, 0, 1, kNoClass, 16);

    simplifier.simplify(g);
    EXPECT_EQ(static_cast<EOpcode>(g.op()[fldIdx]), EOpcode::FIELD);
}

// ── Multiple independent sub-expressions simplify in same pass ───────────────

TEST_F(EGraphTest, MultipleSubExpressions)
{
    EGraph g;
    // (x + 0) and (y * 1) in independent parts of the graph
    // Classes 0,1,2 for first; 3,4,5 for second
    g.setClassCount(6);
    g.addVar(1, 0);
    g.addLiteral(2, 0);
    g.addNode(EOpcode::ADD, 0, 1, 2);  // x + 0

    g.addVar(4, 1);
    g.addLiteral(5, 1);
    g.addNode(EOpcode::MUL, 3, 4, 5);  // y * 1

    simplifier.simplify(g);

    EXPECT_EQ(g.find(0), g.find(1)) << "x+0 should alias x";
    EXPECT_EQ(g.find(3), g.find(4)) << "y*1 should alias y";
}

// ── Chained rewrites converge in multiple iterations ─────────────────────────

TEST_F(EGraphTest, ChainedRewritesConverge)
{
    // CAST(CAST(x, 8), 4) + LIT(0) → eventually collapses to just x
    EGraph g;
    // Classes: 0=add_result, 1=outer_cast, 2=inner_cast, 3=x, 4=zero
    g.setClassCount(5);
    g.addVar(3, 0);
    g.addLiteral(4, 0);
    g.addNode(EOpcode::CAST, 2, 3, kNoClass, 8);  // CAST(x,8)
    g.addNode(EOpcode::CAST, 1, 2, kNoClass, 4);  // CAST(inner,4)
    g.addNode(EOpcode::ADD,  0, 1, 4);             // outer_cast + 0

    simplifier.simplify(g);

    // After all rules: ADD(CAST_outer, 0) merges with CAST_outer (rule 1),
    // CAST_outer (lhs→inner) collapses to CAST(x,4) (rule 6).
    // So class 0 root == class 1 root.
    EXPECT_EQ(g.find(0), g.find(1)) << "ADD(CAST,0) should alias CAST";
}
