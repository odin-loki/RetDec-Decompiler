/**
 * @file tests/codegen/codegen_test.cpp
 * @brief Unit tests for the readability-optimised C code generation module (Stage 24).
 *
 * Coverage:
 *   - CType::toString
 *   - CExpr::toString (literal, var, binop, unop, cast, call, index, member, ternary)
 *   - ExprCoalescer (coalescing detection, immediate materialisation)
 *   - CondNormaliser (NOT elimination, boolean context, double negation)
 *   - LoopFormSelector (For/While/DoWhile/Infinite)
 *   - PointerSyntax (subscript, struct member, cast minimisation)
 *   - GotoEliminator (flag variable introduction, irreducible goto kept)
 *   - Emitter (statement, function, unit emission)
 *   - CodeGenPass (full pipeline, stats)
 */

#include "retdec/codegen/codegen.h"
#include "retdec/ssa/ssa.h"
#include "retdec/cfg_structure/cfg_structure.h"
#include "retdec/call_conv/call_conv.h"
#include "retdec/dce/dce.h"

#include <gtest/gtest.h>
#include <string>
#include <memory>

using namespace retdec::codegen;
using namespace retdec;

// ─── CType tests ──────────────────────────────────────────────────────────────

TEST(CTypeTest, VoidToString)
{
	auto t = CType::make(CType::Kind::Void);
	EXPECT_EQ(t->toString(), "void");
}

TEST(CTypeTest, Int32ToString)
{
	auto t = CType::make(CType::Kind::Int32);
	EXPECT_EQ(t->toString(), "int32_t");
}

TEST(CTypeTest, PointerToInt32)
{
	auto t = CType::ptr(CType::make(CType::Kind::Int32));
	EXPECT_EQ(t->toString(), "int32_t *");
}

TEST(CTypeTest, ArrayOfInt8)
{
	auto t = CType::arr(CType::make(CType::Kind::Int8), 16);
	EXPECT_EQ(t->toString(), "int8_t[16]");
}

TEST(CTypeTest, StructToString)
{
	auto t = CType::make(CType::Kind::Struct);
	t->name = "MyStruct";
	EXPECT_EQ(t->toString(), "struct MyStruct");
}

TEST(CTypeTest, ConstInt32)
{
	auto t = CType::make(CType::Kind::Int32);
	t->isConst = true;
	EXPECT_EQ(t->toString(), "const int32_t");
}

TEST(CTypeTest, BitWidth)
{
	EXPECT_EQ(CType::make(CType::Kind::Int8)->bitWidth(), 8);
	EXPECT_EQ(CType::make(CType::Kind::Int16)->bitWidth(), 16);
	EXPECT_EQ(CType::make(CType::Kind::Int32)->bitWidth(), 32);
	EXPECT_EQ(CType::make(CType::Kind::Int64)->bitWidth(), 64);
	EXPECT_EQ(CType::make(CType::Kind::Float)->bitWidth(), 32);
	EXPECT_EQ(CType::make(CType::Kind::Double)->bitWidth(), 64);
}

TEST(CTypeTest, IsIntegral)
{
	EXPECT_TRUE(CType::make(CType::Kind::Int32)->isIntegral());
	EXPECT_FALSE(CType::make(CType::Kind::Float)->isIntegral());
	EXPECT_FALSE(CType::make(CType::Kind::Pointer)->isIntegral());
}

// ─── CExpr::toString tests ────────────────────────────────────────────────────

TEST(CExprTest, Literal)
{
	auto e = CExpr::lit("42");
	EXPECT_EQ(e->toString(), "42");
}

TEST(CExprTest, Var)
{
	auto e = CExpr::var("my_var");
	EXPECT_EQ(e->toString(), "my_var");
}

TEST(CExprTest, BinOpAdd)
{
	auto e = CExpr::binop(CExpr::BinOpKind::Add, CExpr::var("a"), CExpr::var("b"));
	EXPECT_EQ(e->toString(), "a + b");
}

TEST(CExprTest, BinOpPrecedence)
{
	// (a + b) * c  → no parens needed on outer
	auto add = CExpr::binop(CExpr::BinOpKind::Add, CExpr::var("a"), CExpr::var("b"));
	auto mul = CExpr::binop(CExpr::BinOpKind::Mul, add, CExpr::var("c"));
	// add has lower prec than mul, so it should be parenthesised.
	EXPECT_NE(mul->toString().find("("), std::string::npos);
}

TEST(CExprTest, UnOpNeg)
{
	auto e = CExpr::unop(CExpr::UnOpKind::Neg, CExpr::var("x"));
	EXPECT_EQ(e->toString(), "-x");
}

TEST(CExprTest, UnOpNot)
{
	auto e = CExpr::unop(CExpr::UnOpKind::Not, CExpr::var("cond"));
	EXPECT_EQ(e->toString(), "!cond");
}

TEST(CExprTest, UnOpDeref)
{
	auto e = CExpr::unop(CExpr::UnOpKind::Deref, CExpr::var("p"));
	EXPECT_EQ(e->toString(), "*p");
}

// The unary operator and its operand were pasted together with nothing
// between them, and the parenthesisation test `14 < outerPrec` is false when
// the operand is itself unary -- also precedence 14 -- so the C tokeniser
// re-read the pair as one operator: Neg(Neg(x)) printed `--x`, a pre-decrement.
TEST(CExprTest, NestedNegIsNotADecrement)
{
	auto e = CExpr::unop(CExpr::UnOpKind::Neg, CExpr::unop(CExpr::UnOpKind::Neg, CExpr::var("x")));
	EXPECT_NE(e->toString(), "--x");
	EXPECT_EQ(e->toString(), "- -x");
}

TEST(CExprTest, NestedAddrOfIsNotALogicalAnd)
{
	auto e = CExpr::unop(CExpr::UnOpKind::AddrOf, CExpr::unop(CExpr::UnOpKind::AddrOf, CExpr::var("p")));
	EXPECT_NE(e->toString(), "&&p");
	EXPECT_EQ(e->toString(), "& &p");
}

// Two unaries that cannot form another token still print tightly.
TEST(CExprTest, NestedDerefNeedsNoSpace)
{
	auto e = CExpr::unop(CExpr::UnOpKind::Deref, CExpr::unop(CExpr::UnOpKind::Deref, CExpr::var("p")));
	EXPECT_EQ(e->toString(), "**p");
}

TEST(CExprTest, Cast)
{
	auto e = CExpr::cast(CType::make(CType::Kind::Int32), CExpr::var("x"));
	EXPECT_EQ(e->toString(), "(int32_t)x");
}

TEST(CExprTest, CallNoArgs)
{
	auto e = CExpr::call("foo", {});
	EXPECT_EQ(e->toString(), "foo()");
}

TEST(CExprTest, CallWithArgs)
{
	auto e = CExpr::call("bar", {CExpr::var("a"), CExpr::lit("1")});
	EXPECT_EQ(e->toString(), "bar(a, 1)");
}

TEST(CExprTest, Index)
{
	auto e = CExpr::index(CExpr::var("arr"), CExpr::var("i"));
	EXPECT_EQ(e->toString(), "arr[i]");
}

TEST(CExprTest, MemberArrow)
{
	auto e = CExpr::member(CExpr::var("p"), "field", true);
	EXPECT_EQ(e->toString(), "p->field");
}

TEST(CExprTest, MemberDot)
{
	auto e = CExpr::member(CExpr::var("s"), "x", false);
	EXPECT_EQ(e->toString(), "s.x");
}

TEST(CExprTest, Ternary)
{
	auto e = CExpr::ternary(CExpr::var("c"), CExpr::var("a"), CExpr::var("b"));
	EXPECT_EQ(e->toString(), "c ? a : b");
}

TEST(CExprTest, BinOpStr)
{
	EXPECT_STREQ(binOpStr(CExpr::BinOpKind::Add), "+");
	EXPECT_STREQ(binOpStr(CExpr::BinOpKind::And), "&");
	EXPECT_STREQ(binOpStr(CExpr::BinOpKind::LAnd), "&&");
	EXPECT_STREQ(binOpStr(CExpr::BinOpKind::Eq), "==");
	EXPECT_STREQ(binOpStr(CExpr::BinOpKind::Le), "<=");
}

// ─── CondNormaliser tests ─────────────────────────────────────────────────────

TEST(CondNormaliserTest, NotGeBecomesLt)
{
	// NOT(a >= b) → a < b
	CondNormaliser n;
	auto ge = CExpr::binop(CExpr::BinOpKind::Ge, CExpr::var("a"), CExpr::var("b"));
	auto notE = CExpr::unop(CExpr::UnOpKind::Not, ge);
	auto result = n.normalise(notE);
	ASSERT_EQ(result->kind, CExpr::Kind::BinOp);
	EXPECT_EQ(result->binOp, CExpr::BinOpKind::Lt);
}

TEST(CondNormaliserTest, NotGtBecomesLe)
{
	CondNormaliser n;
	auto gt = CExpr::binop(CExpr::BinOpKind::Gt, CExpr::var("a"), CExpr::var("b"));
	auto notE = CExpr::unop(CExpr::UnOpKind::Not, gt);
	auto result = n.normalise(notE);
	ASSERT_EQ(result->kind, CExpr::Kind::BinOp);
	EXPECT_EQ(result->binOp, CExpr::BinOpKind::Le);
}

TEST(CondNormaliserTest, NotEqBecomesNe)
{
	CondNormaliser n;
	auto eq = CExpr::binop(CExpr::BinOpKind::Eq, CExpr::var("a"), CExpr::var("b"));
	auto notE = CExpr::unop(CExpr::UnOpKind::Not, eq);
	auto result = n.normalise(notE);
	ASSERT_EQ(result->kind, CExpr::Kind::BinOp);
	EXPECT_EQ(result->binOp, CExpr::BinOpKind::Ne);
}

TEST(CondNormaliserTest, DoubleNegation)
{
	CondNormaliser n;
	auto inner = CExpr::var("x");
	auto not1 = CExpr::unop(CExpr::UnOpKind::Not, inner);
	auto not2 = CExpr::unop(CExpr::UnOpKind::Not, not1);
	auto result = n.normalise(not2);
	ASSERT_EQ(result->kind, CExpr::Kind::Var);
	EXPECT_EQ(result->varName, "x");
}

TEST(CondNormaliserTest, BoolContextNeZeroCollapses)
{
	// (x != 0) in bool context → x
	CondNormaliser n;
	auto ne = CExpr::binop(CExpr::BinOpKind::Ne, CExpr::var("x"), CExpr::lit("0"));
	auto result = n.normalise(ne, /*boolContext=*/true);
	ASSERT_EQ(result->kind, CExpr::Kind::Var);
	EXPECT_EQ(result->varName, "x");
}

TEST(CondNormaliserTest, BoolContextEqZeroBecomesNot)
{
	// (x == 0) in bool context → !x
	CondNormaliser n;
	auto eq = CExpr::binop(CExpr::BinOpKind::Eq, CExpr::var("x"), CExpr::lit("0"));
	auto result = n.normalise(eq, /*boolContext=*/true);
	ASSERT_EQ(result->kind, CExpr::Kind::UnOp);
	EXPECT_EQ(result->unOp, CExpr::UnOpKind::Not);
}

TEST(CondNormaliserTest, NonBoolContextPreservedNeZero)
{
	// (x != 0) NOT in bool context stays as-is.
	CondNormaliser n;
	auto ne = CExpr::binop(CExpr::BinOpKind::Ne, CExpr::var("x"), CExpr::lit("0"));
	auto result = n.normalise(ne, false);
	EXPECT_EQ(result->kind, CExpr::Kind::BinOp);
	EXPECT_EQ(result->binOp, CExpr::BinOpKind::Ne);
}

TEST(CondNormaliserTest, RewritesAreCounted)
{
	// CodeGenPass::Stats::condRewrites is documented as a count of these and
	// was never assigned, so it read 0 on every input -- including the ones
	// where the pass had just rewritten something.
	CondNormaliser n;
	auto ne = CExpr::binop(CExpr::BinOpKind::Ne, CExpr::var("x"), CExpr::lit("0"));
	std::size_t rewrites = 0;
	n.normalise(ne, /*boolContext=*/true, &rewrites);
	EXPECT_EQ(1u, rewrites);
}

TEST(CondNormaliserTest, NestedRewritesAreCountedOnce_Each)
{
	// !(a >= b) && !(c == d) -- two rewrites, one per operand, and the
	// enclosing && is not one.
	CondNormaliser n;
	auto lhs = CExpr::unop(CExpr::UnOpKind::Not, CExpr::binop(CExpr::BinOpKind::Ge, CExpr::var("a"), CExpr::var("b")));
	auto rhs = CExpr::unop(CExpr::UnOpKind::Not, CExpr::binop(CExpr::BinOpKind::Eq, CExpr::var("c"), CExpr::var("d")));
	auto both = CExpr::binop(CExpr::BinOpKind::LAnd, lhs, rhs);
	std::size_t rewrites = 0;
	n.normalise(both, /*boolContext=*/true, &rewrites);
	EXPECT_EQ(2u, rewrites);
}

TEST(CondNormaliserTest, NothingToRewriteCountsNothing)
{
	CondNormaliser n;
	auto lt = CExpr::binop(CExpr::BinOpKind::Lt, CExpr::var("a"), CExpr::var("b"));
	std::size_t rewrites = 0;
	n.normalise(lt, /*boolContext=*/true, &rewrites);
	EXPECT_EQ(0u, rewrites);
}

// ─── LoopFormSelector tests ───────────────────────────────────────────────────

TEST(LoopFormSelectorTest, WhileLoop)
{
	LoopFormSelector sel;
	cfg_structure::StructNode node;
	node.kind = cfg_structure::StructNode::Kind::While;

	auto body = CStmt::block();
	body->children.push_back(CStmt::retStmt(CExpr::lit("0")));

	auto cond = CExpr::var("running");
	auto result = sel.select(node, cond, nullptr, nullptr, body);
	ASSERT_EQ(result->kind, CStmt::Kind::While);
	EXPECT_EQ(result->expr->varName, "running");
}

TEST(LoopFormSelectorTest, WhileLoopNoCond)
{
	LoopFormSelector sel;
	cfg_structure::StructNode node;
	node.kind = cfg_structure::StructNode::Kind::While;
	auto body = CStmt::block();
	// No condition → while (1)
	auto result = sel.select(node, nullptr, nullptr, nullptr, body);
	ASSERT_EQ(result->kind, CStmt::Kind::While);
	EXPECT_EQ(result->expr->literal, "1");
}

TEST(LoopFormSelectorTest, DoWhileLoop)
{
	LoopFormSelector sel;
	cfg_structure::StructNode node;
	node.kind = cfg_structure::StructNode::Kind::DoWhile;
	auto body = CStmt::block();
	auto cond = CExpr::var("again");
	auto result = sel.select(node, cond, nullptr, nullptr, body);
	ASSERT_EQ(result->kind, CStmt::Kind::DoWhile);
	EXPECT_EQ(result->expr->varName, "again");
}

TEST(LoopFormSelectorTest, ForLoopAllParts)
{
	LoopFormSelector sel;
	cfg_structure::StructNode node;
	node.kind = cfg_structure::StructNode::Kind::For;
	auto body = CStmt::block();
	auto cond = CExpr::binop(CExpr::BinOpKind::Lt, CExpr::var("i"), CExpr::lit("10"));
	auto init = CExpr::binop(CExpr::BinOpKind::Assign, CExpr::var("i"), CExpr::lit("0"));
	auto incr = CExpr::unop(CExpr::UnOpKind::PostInc, CExpr::var("i"));
	auto result = sel.select(node, cond, init, incr, body);
	ASSERT_EQ(result->kind, CStmt::Kind::For);
}

TEST(LoopFormSelectorTest, ForLoopDegradesWhenNoInit)
{
	LoopFormSelector sel;
	cfg_structure::StructNode node;
	node.kind = cfg_structure::StructNode::Kind::For;
	auto body = CStmt::block();
	auto cond = CExpr::var("x");
	// No init → degrades to While.
	auto result = sel.select(node, cond, nullptr, nullptr, body);
	ASSERT_EQ(result->kind, CStmt::Kind::While);
}

TEST(LoopFormSelectorTest, InfiniteLoop)
{
	LoopFormSelector sel;
	cfg_structure::StructNode node;
	node.kind = cfg_structure::StructNode::Kind::Infinite;
	auto body = CStmt::block();
	auto result = sel.select(node, nullptr, nullptr, nullptr, body);
	ASSERT_EQ(result->kind, CStmt::Kind::While);
	EXPECT_EQ(result->expr->literal, "1");
}

// ─── PointerSyntax tests ──────────────────────────────────────────────────────

TEST(PointerSyntaxTest, DerefAddBecomesIndex)
{
	PointerSyntax ps;
	// *(p + i) → p[i]
	auto add = CExpr::binop(CExpr::BinOpKind::Add, CExpr::var("p"), CExpr::var("i"));
	auto deref = CExpr::unop(CExpr::UnOpKind::Deref, add);
	auto result = ps.recover(deref);
	ASSERT_EQ(result->kind, CExpr::Kind::Index);
	EXPECT_EQ(result->children[0]->varName, "p");
	EXPECT_EQ(result->children[1]->varName, "i");
}

TEST(PointerSyntaxTest, DerefAddZeroBecomesDeref)
{
	PointerSyntax ps;
	// *(p + 0) → *p
	auto add = CExpr::binop(CExpr::BinOpKind::Add, CExpr::var("p"), CExpr::lit("0"));
	auto deref = CExpr::unop(CExpr::UnOpKind::Deref, add);
	auto result = ps.recover(deref);
	ASSERT_EQ(result->kind, CExpr::Kind::UnOp);
	EXPECT_EQ(result->unOp, CExpr::UnOpKind::Deref);
}

TEST(PointerSyntaxTest, DerefAddWithStrideBecomesIndex)
{
	PointerSyntax ps;
	// *(p + i * 4) → p[i]  (stride 4 stripped)
	auto mul = CExpr::binop(CExpr::BinOpKind::Mul, CExpr::var("i"), CExpr::lit("4"));
	auto add = CExpr::binop(CExpr::BinOpKind::Add, CExpr::var("p"), mul);
	auto deref = CExpr::unop(CExpr::UnOpKind::Deref, add);
	auto result = ps.recover(deref);
	ASSERT_EQ(result->kind, CExpr::Kind::Index);
	EXPECT_EQ(result->children[0]->varName, "p");
	EXPECT_EQ(result->children[1]->varName, "i");
}

TEST(PointerSyntaxTest, StructMemberRecovery)
{
	PointerSyntax ps;
	PointerSyntax::StructInfo si;
	si.typeName = "MyStruct";
	si.fields[8] = "y";
	std::unordered_map<std::string, PointerSyntax::StructInfo> structs;
	structs["MyStruct"] = si;

	// *(p + 8) → p->y
	auto add = CExpr::binop(CExpr::BinOpKind::Add, CExpr::var("p"), CExpr::lit("8"));
	auto deref = CExpr::unop(CExpr::UnOpKind::Deref, add);
	// Set p as pointer type.
	deref->children[0]->children[0]->exprType = CType::make(CType::Kind::Pointer);

	auto result = ps.recover(deref, structs);
	ASSERT_EQ(result->kind, CExpr::Kind::Member);
	EXPECT_EQ(result->fieldName, "y");
}

TEST(PointerSyntaxTest, DuplicateCastRemoved)
{
	PointerSyntax ps;
	// (int32_t)(int32_t)x → (int32_t)x
	auto inner = CExpr::cast(CType::make(CType::Kind::Int32), CExpr::var("x"));
	auto outer = CExpr::cast(CType::make(CType::Kind::Int32), inner);
	// The inner cast has exprType = Int32, outer cast is redundant.
	inner->exprType = CType::make(CType::Kind::Int32);
	auto result = ps.recover(outer);
	// After minimisation: (int32_t)x since inner is now the expr.
	ASSERT_EQ(result->kind, CExpr::Kind::Cast);
	// The cast chain should be collapsed.
	EXPECT_EQ(result->children[0]->kind, CExpr::Kind::Cast);
}

TEST(PointerSyntaxTest, RemovedCastsAreCounted)
{
	// (int32_t)x where x is already int32_t -- the cast goes, and
	// CodeGenPass::Stats::castsRemoved is what is supposed to say so.
	PointerSyntax ps;
	auto x = CExpr::var("x");
	x->exprType = CType::make(CType::Kind::Int32);
	auto redundant = CExpr::cast(CType::make(CType::Kind::Int32), x);

	std::size_t removed = 0;
	auto result = ps.recover(redundant, {}, &removed);

	EXPECT_EQ(CExpr::Kind::Var, result->kind);
	EXPECT_EQ(1u, removed);
}

TEST(PointerSyntaxTest, ACastThatIsNotRedundantIsNotCounted)
{
	PointerSyntax ps;
	auto x = CExpr::var("x");
	x->exprType = CType::make(CType::Kind::Int8);
	auto widening = CExpr::cast(CType::make(CType::Kind::Int32), x);

	std::size_t removed = 0;
	auto result = ps.recover(widening, {}, &removed);

	EXPECT_EQ(CExpr::Kind::Cast, result->kind);
	EXPECT_EQ(0u, removed);
}

// ─── GotoEliminator tests ─────────────────────────────────────────────────────

TEST(GotoEliminatorTest, NoGotosUnchanged)
{
	GotoEliminator ge;
	auto body = CStmt::block();
	body->children.push_back(CStmt::retStmt(CExpr::lit("0")));
	auto result = ge.eliminate(body);
	ASSERT_EQ(result->children.size(), 1u);
	EXPECT_EQ(result->children[0]->kind, CStmt::Kind::Return);
}

TEST(GotoEliminatorTest, SimpleForwardGotoEliminated)
{
	GotoEliminator ge;
	auto body = CStmt::block();
	// goto done; ... done: return 0;
	body->children.push_back(CStmt::gotoStmt("done"));
	body->children.push_back(CStmt::exprStmt(CExpr::call("side_effect", {})));
	body->children.push_back(CStmt::labelStmt("done"));
	body->children.push_back(CStmt::retStmt(CExpr::lit("0")));

	auto result = ge.eliminate(body);
	// Result should not contain a Goto statement anymore.
	std::function<bool(const CStmt*)> hasGoto = [&](const CStmt* s) {
		if (!s) return false;
		if (s->kind == CStmt::Kind::Goto && s->label == "done") return true;
		for (auto& c: s->children)
			if (hasGoto(c.get())) return true;
		return false;
	};
	EXPECT_FALSE(hasGoto(result.get()));
}

// The Label node was replaced by an empty block on the way down, before any
// guard was built, so the guard was built from every remaining sibling --
// including the statements *at and after* the label. Those are the goto's
// destination and must always run; they were skipped exactly when the goto was
// taken.
TEST(GotoEliminatorTest, TheLabelsOwnTargetIsNotGuarded)
{
	GotoEliminator ge;
	auto body = CStmt::block();
	body->children.push_back(CStmt::gotoStmt("done"));
	body->children.push_back(CStmt::exprStmt(CExpr::call("skipped", {})));
	body->children.push_back(CStmt::labelStmt("done"));
	body->children.push_back(CStmt::exprStmt(CExpr::call("always", {})));

	auto result = ge.eliminate(body);

	// `always()` runs whatever the goto did, so it must not be inside the
	// `if (!_flag_done)` guard.
	std::function<bool(const CStmt*, bool)> insideGuard = [&](const CStmt* s, bool guarded) {
		if (!s) return false;
		if (s->kind == CStmt::Kind::ExprStmt && s->expr && s->expr->toString().find("always") != std::string::npos)
			return guarded;
		const bool now = guarded || s->kind == CStmt::Kind::If;
		for (auto& c: s->children)
			if (insideGuard(c.get(), now)) return true;
		return false;
	};
	EXPECT_FALSE(insideGuard(result.get(), false));

	// ...and `skipped()` must be.
	std::function<bool(const CStmt*, bool)> skippedGuarded = [&](const CStmt* s, bool guarded) {
		if (!s) return false;
		if (s->kind == CStmt::Kind::ExprStmt && s->expr && s->expr->toString().find("skipped") != std::string::npos)
			return guarded;
		const bool now = guarded || s->kind == CStmt::Kind::If;
		for (auto& c: s->children)
			if (skippedGuarded(c.get(), now)) return true;
		return false;
	};
	EXPECT_TRUE(skippedGuarded(result.get(), false));
}

// The flag-set detector only matched a child that was literally an Assign at
// this block level, so a goto nested inside an if -- `if (c) goto L;` -- left
// activeFlags empty and produced no guard at all: the statements the goto was
// meant to skip ran unconditionally and the flag was dead.
TEST(GotoEliminatorTest, ANestedGotoStillGuardsWhatFollows)
{
	GotoEliminator ge;
	auto body = CStmt::block();
	auto cond = CStmt::ifStmt(CExpr::var("c"));
	cond->children.push_back(CStmt::gotoStmt("done"));
	body->children.push_back(cond);
	body->children.push_back(CStmt::exprStmt(CExpr::call("skipped", {})));
	body->children.push_back(CStmt::labelStmt("done"));
	body->children.push_back(CStmt::retStmt());

	auto result = ge.eliminate(body);

	// A guard mentioning the flag must exist somewhere above `skipped()`.
	std::function<bool(const CStmt*, bool)> guarded = [&](const CStmt* s, bool inGuard) {
		if (!s) return false;
		if (s->kind == CStmt::Kind::ExprStmt && s->expr && s->expr->toString().find("skipped") != std::string::npos)
			return inGuard;
		bool now = inGuard;
		if (s->kind == CStmt::Kind::If && s->expr && s->expr->toString().find("_flag_done") != std::string::npos)
			now = true;
		for (auto& c: s->children)
			if (guarded(c.get(), now)) return true;
		return false;
	};
	EXPECT_TRUE(guarded(result.get(), false));
}

TEST(GotoEliminatorTest, MultipleGotosKept)
{
	GotoEliminator ge;
	auto body = CStmt::block();
	// Three gotos → more than kMaxFlagUses → keep gotos.
	body->children.push_back(CStmt::gotoStmt("target"));
	body->children.push_back(CStmt::gotoStmt("target"));
	body->children.push_back(CStmt::gotoStmt("target"));
	body->children.push_back(CStmt::labelStmt("target"));
	body->children.push_back(CStmt::retStmt());

	auto result = ge.eliminate(body);
	int gotoCount = 0;
	std::function<void(const CStmt*)> count = [&](const CStmt* s) {
		if (!s) return;
		if (s->kind == CStmt::Kind::Goto) ++gotoCount;
		for (auto& c: s->children)
			count(c.get());
	};
	count(result.get());
	// 3 gotos > kMaxFlagUses (2), so they should remain.
	EXPECT_GT(gotoCount, 0);
}

// ─── Emitter tests ────────────────────────────────────────────────────────────

TEST(EmitterTest, EmitTypeInt32)
{
	Emitter e;
	auto t = CType::make(CType::Kind::Int32);
	EXPECT_EQ(e.emitType(*t, "x"), "int32_t x");
}

TEST(EmitterTest, EmitTypePointer)
{
	Emitter e;
	auto t = CType::ptr(CType::make(CType::Kind::Int8));
	EXPECT_EQ(e.emitType(*t, "buf"), "int8_t * buf");
}

TEST(EmitterTest, EmitReturnStmt)
{
	Emitter e;
	auto s = CStmt::retStmt(CExpr::lit("42"));
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("return 42"), std::string::npos);
}

TEST(EmitterTest, EmitAssignStmt)
{
	Emitter e;
	auto s = CStmt::assign(CExpr::var("x"), CExpr::lit("10"));
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("x = 10"), std::string::npos);
}

TEST(EmitterTest, EmitIfStmt)
{
	Emitter e;
	auto cond = CExpr::var("flag");
	auto s = CStmt::ifStmt(cond);
	auto thenBlk = CStmt::block();
	thenBlk->children.push_back(CStmt::retStmt(CExpr::lit("1")));
	s->children.push_back(thenBlk);
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("if (flag)"), std::string::npos);
	EXPECT_NE(out.find("return 1"), std::string::npos);
}

TEST(EmitterTest, EmitWhileStmt)
{
	Emitter e;
	auto s = CStmt::whileStmt(CExpr::var("cond"));
	auto body = CStmt::block();
	body->children.push_back(CStmt::breakStmt());
	s->children.push_back(body);
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("while (cond)"), std::string::npos);
	EXPECT_NE(out.find("break"), std::string::npos);
}

TEST(EmitterTest, EmitDoWhileStmt)
{
	Emitter e;
	auto s = CStmt::doWhileStmt(CExpr::var("cond"));
	s->children.push_back(CStmt::block());
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("do {"), std::string::npos);
	EXPECT_NE(out.find("} while (cond)"), std::string::npos);
}

TEST(EmitterTest, EmitForStmt)
{
	Emitter e;
	auto init = CExpr::binop(CExpr::BinOpKind::Assign, CExpr::var("i"), CExpr::lit("0"));
	auto cond = CExpr::binop(CExpr::BinOpKind::Lt, CExpr::var("i"), CExpr::lit("10"));
	auto incr = CExpr::unop(CExpr::UnOpKind::PostInc, CExpr::var("i"));
	auto s = CStmt::forStmt(init, cond, incr);
	s->children.push_back(CStmt::block());
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("for ("), std::string::npos);
	EXPECT_NE(out.find("i < 10"), std::string::npos);
}

TEST(EmitterTest, EmitGotoAndLabel)
{
	Emitter e;
	Emitter::Config cfg;
	auto g = CStmt::gotoStmt("end");
	auto l = CStmt::labelStmt("end");
	EXPECT_NE(e.emitStmt(*g, 1, cfg).find("goto end"), std::string::npos);
	EXPECT_NE(e.emitStmt(*l, 1, cfg).find("end:"), std::string::npos);
}

TEST(EmitterTest, EmitSimpleFunction)
{
	Emitter e;
	CFunction fn;
	fn.name = "add";
	fn.returnType = CType::make(CType::Kind::Int32);
	fn.params.push_back({"a", CType::make(CType::Kind::Int32)});
	fn.params.push_back({"b", CType::make(CType::Kind::Int32)});
	fn.body = CStmt::block();
	auto ret = CStmt::retStmt(CExpr::binop(CExpr::BinOpKind::Add, CExpr::var("a"), CExpr::var("b")));
	fn.body->children.push_back(ret);

	Emitter::Config cfg;
	std::string out = e.emitFunction(fn, cfg);
	EXPECT_NE(out.find("int32_t add("), std::string::npos);
	EXPECT_NE(out.find("int32_t a"), std::string::npos);
	EXPECT_NE(out.find("int32_t b"), std::string::npos);
	EXPECT_NE(out.find("return a + b"), std::string::npos);
}

TEST(EmitterTest, EmitVariadicFunction)
{
	Emitter e;
	CFunction fn;
	fn.name = "my_printf";
	fn.returnType = CType::make(CType::Kind::Int32);
	fn.params.push_back({"fmt", CType::ptr(CType::make(CType::Kind::Int8))});
	fn.isVariadic = true;
	fn.body = CStmt::block();
	fn.body->children.push_back(CStmt::retStmt(CExpr::lit("0")));
	Emitter::Config cfg;
	std::string out = e.emitFunction(fn, cfg);
	EXPECT_NE(out.find("..."), std::string::npos);
}

TEST(EmitterTest, EmitUnitWithIncludes)
{
	Emitter e;
	CUnit unit;
	CFunction fn;
	fn.name = "run";
	fn.returnType = CType::make(CType::Kind::Void);
	fn.body = CStmt::block();
	fn.body->children.push_back(CStmt::exprStmt(CExpr::call("printf", {CExpr::lit("\"hello\"")})));
	unit.functions.push_back(fn);
	Emitter::Config cfg;
	std::string out = e.emitUnit(unit, cfg);
	EXPECT_NE(out.find("#include <stdio.h>"), std::string::npos);
	EXPECT_NE(out.find("printf("), std::string::npos);
}

TEST(EmitterTest, EmitSwitchStmt)
{
	Emitter e;
	Emitter::Config cfg;
	auto sw = std::make_shared<CStmt>();
	sw->kind = CStmt::Kind::Switch;
	sw->expr = CExpr::var("val");

	auto cs = std::make_shared<CStmt>();
	cs->kind = CStmt::Kind::Case;
	cs->caseValue = 1;
	sw->children.push_back(cs);
	sw->children.push_back(CStmt::breakStmt());

	auto def = std::make_shared<CStmt>();
	def->kind = CStmt::Kind::Default;
	sw->children.push_back(def);
	sw->children.push_back(CStmt::breakStmt());

	std::string out = e.emitStmt(*sw, 0, cfg);
	EXPECT_NE(out.find("switch (val)"), std::string::npos);
	EXPECT_NE(out.find("case 1:"), std::string::npos);
	EXPECT_NE(out.find("default:"), std::string::npos);
}

// ─── CodeGenPass stat tests ───────────────────────────────────────────────────

TEST(CodeGenPassTest, EmitSimpleFunctionFromFn)
{
	// Build a trivial SSA function with no instructions, no call convention.
	// The codegen should produce a valid (if empty) function.
	ssa::SSAFunction fn("test_func");
	auto* entry = fn.addBlock("entry");
	// One instruction: ret (no uses/defs).
	auto* retI = fn.addInstr(entry->id, ssa::IrInstr::Op::Ret);
	(void)retI;

	cfg_structure::StructNode tree;
	tree.kind = cfg_structure::StructNode::Kind::Block;
	tree.blockId = entry->id;

	call_conv::CallingConvention cc;
	cc.ret.kind = call_conv::RetKind::Void;

	dce::DeadCodeResult dce;
	// All instructions live.
	dce.liveInstrs.insert(retI->id);

	CodeGenPass pass;
	auto cfn = pass.generateFunction(fn, tree, cc, dce, {});
	EXPECT_EQ(cfn.name, "test_func");
	EXPECT_EQ(pass.stats().totalFunctions, 1u);

	// Emit the function.
	Emitter e;
	std::string out = e.emitFunction(cfn, {});
	EXPECT_NE(out.find("test_func"), std::string::npos);
	EXPECT_NE(out.find("return"), std::string::npos);
}

TEST(CodeGenPassTest, StatsCoalescingCounts)
{
	// Multiple independent functions → total count increments.
	ssa::SSAFunction fn1("alpha");
	auto* b1 = fn1.addBlock("entry");
	auto* r1 = fn1.addInstr(b1->id, ssa::IrInstr::Op::Ret);

	ssa::SSAFunction fn2("beta");
	auto* b2 = fn2.addBlock("entry");
	auto* r2 = fn2.addInstr(b2->id, ssa::IrInstr::Op::Ret);

	cfg_structure::StructNode t1, t2;
	t1.kind = t2.kind = cfg_structure::StructNode::Kind::Block;
	t1.blockId = b1->id;
	t2.blockId = b2->id;

	call_conv::CallingConvention cc;
	cc.ret.kind = call_conv::RetKind::Void;
	dce::DeadCodeResult dce;
	dce.liveInstrs = {r1->id, r2->id};

	CodeGenPass pass;
	pass.generateFunction(fn1, t1, cc, dce, {});
	pass.generateFunction(fn2, t2, cc, dce, {});
	EXPECT_EQ(pass.stats().totalFunctions, 2u);
}

TEST(CodeGenPassTest, StatsCountTheConditionRewritesThePassMade)
{
	// Stats::condRewrites is documented as a count of CondNormaliser's
	// rewrites and was never assigned, so it read 0 on every unit -- and
	// there was nothing for it to count anyway, because the coalescer built
	// no comparison for CondNormaliser to match. Both halves are here: the
	// predicate reaches the expression, and the count reaches the stats.
	//
	//   t = (a == 0);  if (t) { ... }      →      if (!a) { ... }
	ssa::SSAFunction fn("cond_test");
	auto* entry = fn.addBlock("entry");
	auto* a = fn.allocValue(ssa::ValueKind::VirtualReg);
	auto* zero = fn.allocValue(ssa::ValueKind::Immediate);
	zero->imm = 0;

	auto* cmpI = fn.addInstr(entry->id, ssa::IrInstr::Op::Compare);
	cmpI->cmpPred = ssa::CmpPred::Eq;
	auto* cond = fn.allocValue(ssa::ValueKind::VirtualReg);
	cmpI->defValue = cond->id;
	cond->defInstr = cmpI;
	cmpI->uses.push_back(ssa::Use{a->id, 0});
	cmpI->uses.push_back(ssa::Use{zero->id, 1});

	auto* retI = fn.addInstr(entry->id, ssa::IrInstr::Op::Ret);

	cfg_structure::StructNode body;
	body.kind = cfg_structure::StructNode::Kind::Block;
	body.blockId = entry->id;

	cfg_structure::StructNode tree;
	tree.kind = cfg_structure::StructNode::Kind::IfThen;
	tree.condValueId = cond->id;
	tree.children.push_back(std::make_unique<cfg_structure::StructNode>(std::move(body)));

	call_conv::CallingConvention cc;
	cc.ret.kind = call_conv::RetKind::Void;
	dce::DeadCodeResult dce;
	dce.liveInstrs = {cmpI->id, retI->id};

	CodeGenPass pass;
	pass.generateFunction(fn, tree, cc, dce, {});

	EXPECT_EQ(1u, pass.stats().condRewrites);
}

TEST(CodeGenPassTest, EnableCondNormOffActuallyTurnsTheNormaliserOff)
{
	// Config::enableCondNorm sat beside enableGotoElim and enableCoalescing,
	// both of which are consulted, and was read by nothing: the normaliser
	// ran whatever it was set to. The same shape as enablePtrSyntax below it.
	ssa::SSAFunction fn("cond_off");
	auto* entry = fn.addBlock("entry");
	auto* a = fn.allocValue(ssa::ValueKind::VirtualReg);
	auto* zero = fn.allocValue(ssa::ValueKind::Immediate);
	zero->imm = 0;

	auto* cmpI = fn.addInstr(entry->id, ssa::IrInstr::Op::Compare);
	cmpI->cmpPred = ssa::CmpPred::Eq;
	auto* cond = fn.allocValue(ssa::ValueKind::VirtualReg);
	cmpI->defValue = cond->id;
	cond->defInstr = cmpI;
	cmpI->uses.push_back(ssa::Use{a->id, 0});
	cmpI->uses.push_back(ssa::Use{zero->id, 1});
	auto* retI = fn.addInstr(entry->id, ssa::IrInstr::Op::Ret);

	cfg_structure::StructNode body;
	body.kind = cfg_structure::StructNode::Kind::Block;
	body.blockId = entry->id;
	cfg_structure::StructNode tree;
	tree.kind = cfg_structure::StructNode::Kind::IfThen;
	tree.condValueId = cond->id;
	tree.children.push_back(std::make_unique<cfg_structure::StructNode>(std::move(body)));

	call_conv::CallingConvention cc;
	cc.ret.kind = call_conv::RetKind::Void;
	dce::DeadCodeResult dce;
	dce.liveInstrs = {cmpI->id, retI->id};

	CodeGenPass::Config off;
	off.enableCondNorm = false;

	CodeGenPass pass;
	auto cfn = pass.generateFunction(fn, tree, cc, dce, off);

	EXPECT_EQ(0u, pass.stats().condRewrites);
	ASSERT_NE(cfn.body, nullptr);
	ASSERT_FALSE(cfn.body->children.empty());
	const auto& ifStmt = cfn.body->children[0];
	ASSERT_EQ(CStmt::Kind::If, ifStmt->kind);
	ASSERT_NE(ifStmt->expr, nullptr);
	EXPECT_EQ(CExpr::Kind::BinOp, ifStmt->expr->kind) << "the == 0 was collapsed to a negation with the normaliser off";
}

TEST(CodeGenPassTest, EnablePtrSyntaxOffLeavesTheAddressArithmeticAlone)
{
	//   *(p + 4) = v
	// With pointer syntax on that is `p[4] = v`; with it off the knob has to
	// leave the dereference as written.
	ssa::SSAFunction fn("ptr_off");
	auto* entry = fn.addBlock("entry");
	auto* p = fn.allocValue(ssa::ValueKind::VirtualReg);
	auto* four = fn.allocValue(ssa::ValueKind::Immediate);
	four->imm = 4;
	auto* v = fn.allocValue(ssa::ValueKind::VirtualReg);

	auto* addI = fn.addInstr(entry->id, ssa::IrInstr::Op::Add);
	auto* addr = fn.allocValue(ssa::ValueKind::VirtualReg);
	addI->defValue = addr->id;
	addr->defInstr = addI;
	addI->uses.push_back(ssa::Use{p->id, 0});
	addI->uses.push_back(ssa::Use{four->id, 1});

	auto* stI = fn.addInstr(entry->id, ssa::IrInstr::Op::Store);
	stI->uses.push_back(ssa::Use{v->id, 0});
	stI->uses.push_back(ssa::Use{addr->id, 1});

	cfg_structure::StructNode tree;
	tree.kind = cfg_structure::StructNode::Kind::Block;
	tree.blockId = entry->id;

	call_conv::CallingConvention cc;
	cc.ret.kind = call_conv::RetKind::Void;
	dce::DeadCodeResult dce;
	dce.liveInstrs = {addI->id, stI->id};

	// Store emits an Assign, whose destination is the lhs.
	auto bodyText = [](const CFunction& f) {
		std::string all;
		for (const auto& c: f.body->children)
		{
			if (!c) continue;
			if (c->lhs) all += c->lhs->toString() + " = ";
			if (c->expr) all += c->expr->toString();
			all += ";";
		}
		return all;
	};

	CodeGenPass on;
	auto withSyntax = on.generateFunction(fn, tree, cc, dce, {});
	const std::string subscripted = bodyText(withSyntax);
	EXPECT_NE(std::string::npos, subscripted.find("[")) << subscripted;

	CodeGenPass::Config off;
	off.enablePtrSyntax = false;
	CodeGenPass pass;
	auto plain = pass.generateFunction(fn, tree, cc, dce, off);
	const std::string emitted = bodyText(plain);
	EXPECT_EQ(std::string::npos, emitted.find("[")) << emitted;
}

TEST(CodeGenPassTest, GenerateUnitMultipleFunctions)
{
	ssa::SSAFunction fn1("func_a");
	auto* b1 = fn1.addBlock("entry");
	fn1.addInstr(b1->id, ssa::IrInstr::Op::Ret);
	cfg_structure::StructNode t1;
	t1.kind = cfg_structure::StructNode::Kind::Block;
	t1.blockId = b1->id;

	ssa::SSAFunction fn2("func_b");
	auto* b2 = fn2.addBlock("entry");
	fn2.addInstr(b2->id, ssa::IrInstr::Op::Ret);
	cfg_structure::StructNode t2;
	t2.kind = cfg_structure::StructNode::Kind::Block;
	t2.blockId = b2->id;

	call_conv::CallingConvention cc;
	cc.ret.kind = call_conv::RetKind::Void;

	CodeGenPass pass;
	std::vector<const ssa::SSAFunction*> fns = {&fn1, &fn2};
	std::vector<const cfg_structure::StructNode*> trees = {&t1, &t2};
	std::unordered_map<std::string, call_conv::CallingConvention> ccMap;
	ccMap["func_a"] = cc;
	ccMap["func_b"] = cc;
	std::unordered_map<std::string, dce::DeadCodeResult> dceMap;

	auto unit = pass.generateUnit(fns, trees, ccMap, dceMap, {});
	EXPECT_EQ(unit.functions.size(), 2u);

	Emitter e;
	std::string out = e.emitUnit(unit, {});
	EXPECT_NE(out.find("func_a"), std::string::npos);
	EXPECT_NE(out.find("func_b"), std::string::npos);
}

// ─── Expression tree integration ─────────────────────────────────────────────

TEST(ExprCoalescerTest, ImmediateValueMaterialised)
{
	// A function with one Immediate IrValue should produce a literal.
	ssa::SSAFunction fn("imm_test");
	auto* blk = fn.addBlock("entry");
	(void)blk;

	// Manually add an Immediate value.
	auto* immVal = fn.allocValue(ssa::ValueKind::Immediate);
	immVal->imm = 42;

	dce::DeadCodeResult dce;
	ExprCoalescer ec;
	auto result = ec.run(fn, dce);

	auto it = result.valueExprs.find(immVal->id);
	ASSERT_NE(it, result.valueExprs.end());
	EXPECT_EQ(it->second->literal, "42");
}

TEST(ExprCoalescerTest, AKnownComparisonBecomesAComparison)
{
	// icmp slt a, b -- the predicate reaches the SSA IR now, so the C is
	// `a < b`. It used to be `a - b`: Op::Compare carried no predicate and
	// the coalescer emitted the value a machine CMP computes.
	ssa::SSAFunction fn("cmp_test");
	auto* blk = fn.addBlock("entry");
	auto* a = fn.allocValue(ssa::ValueKind::Immediate);
	a->imm = 3;
	auto* b = fn.allocValue(ssa::ValueKind::Immediate);
	b->imm = 4;

	auto* cmpI = fn.addInstr(blk->id, ssa::IrInstr::Op::Compare);
	cmpI->cmpPred = ssa::CmpPred::Slt;
	auto* res = fn.allocValue(ssa::ValueKind::VirtualReg);
	cmpI->defValue = res->id;
	res->defInstr = cmpI;
	cmpI->uses.push_back(ssa::Use{a->id, 0});
	cmpI->uses.push_back(ssa::Use{b->id, 1});

	dce::DeadCodeResult dce;
	dce.liveInstrs.insert(cmpI->id);
	ExprCoalescer ec;
	auto result = ec.run(fn, dce);

	auto it = result.valueExprs.find(res->id);
	ASSERT_NE(it, result.valueExprs.end());
	ASSERT_EQ(CExpr::Kind::BinOp, it->second->kind);
	EXPECT_EQ(CExpr::BinOpKind::Lt, it->second->binOp);
	EXPECT_EQ("3 < 4", it->second->toString());
}

TEST(ExprCoalescerTest, AnUnsignedComparisonReadsItsOperandsAsUnsigned)
{
	// C puts the signedness in the operands. `icmp ult` on 32-bit values is
	// `(uint32_t)a < (uint32_t)b`, not `a < b`, which would be a signed
	// comparison of the same bits.
	ssa::SSAFunction fn("ucmp_test");
	auto* blk = fn.addBlock("entry");
	auto* a = fn.allocValue(ssa::ValueKind::Immediate);
	a->imm = 3;
	a->width = 32;
	auto* b = fn.allocValue(ssa::ValueKind::Immediate);
	b->imm = 4;
	b->width = 32;

	auto* cmpI = fn.addInstr(blk->id, ssa::IrInstr::Op::Compare);
	cmpI->cmpPred = ssa::CmpPred::Ult;
	auto* res = fn.allocValue(ssa::ValueKind::VirtualReg);
	cmpI->defValue = res->id;
	res->defInstr = cmpI;
	cmpI->uses.push_back(ssa::Use{a->id, 0});
	cmpI->uses.push_back(ssa::Use{b->id, 1});

	dce::DeadCodeResult dce;
	dce.liveInstrs.insert(cmpI->id);
	ExprCoalescer ec;
	auto result = ec.run(fn, dce);

	auto it = result.valueExprs.find(res->id);
	ASSERT_NE(it, result.valueExprs.end());
	EXPECT_EQ("(uint32_t)3 < (uint32_t)4", it->second->toString());
}

TEST(ExprCoalescerTest, AMachineCompareStillEmitsTheSubtraction)
{
	// A CMP/TEST asks nothing on its own -- the condition is in the branch
	// that reads the flags -- so CmpPred::None keeps the old shape. Without
	// this the new branch could widen to cases it has no answer for.
	ssa::SSAFunction fn("machine_cmp");
	auto* blk = fn.addBlock("entry");
	auto* a = fn.allocValue(ssa::ValueKind::Immediate);
	a->imm = 3;
	auto* b = fn.allocValue(ssa::ValueKind::Immediate);
	b->imm = 4;

	auto* cmpI = fn.addInstr(blk->id, ssa::IrInstr::Op::Compare);
	auto* res = fn.allocValue(ssa::ValueKind::VirtualReg);
	cmpI->defValue = res->id;
	res->defInstr = cmpI;
	cmpI->uses.push_back(ssa::Use{a->id, 0});
	cmpI->uses.push_back(ssa::Use{b->id, 1});

	dce::DeadCodeResult dce;
	dce.liveInstrs.insert(cmpI->id);
	ExprCoalescer ec;
	auto result = ec.run(fn, dce);

	auto it = result.valueExprs.find(res->id);
	ASSERT_NE(it, result.valueExprs.end());
	ASSERT_EQ(CExpr::Kind::BinOp, it->second->kind);
	EXPECT_EQ(CExpr::BinOpKind::Sub, it->second->binOp);
}

TEST(ExprCoalescerTest, UndefValueEmitsZero)
{
	// The comment this replaces was right that an Undef materialises only when
	// referenced -- which is why the old body, which looked it up directly and
	// then discarded the iterator with (void), contained no assertion at all
	// and never once observed a zero. Referencing it is what the test's name
	// is about, so the reference is now built.
	ssa::SSAFunction fn("undef_test");
	auto* blk = fn.addBlock("entry");
	auto* uval = fn.allocValue(ssa::ValueKind::Undef);

	// t = undef + undef
	auto* addI = fn.addInstr(blk->id, ssa::IrInstr::Op::Add);
	auto* sum = fn.allocValue(ssa::ValueKind::VirtualReg);
	addI->defValue = sum->id;
	sum->defInstr = addI;
	addI->uses.push_back(ssa::Use{uval->id, 0});
	addI->uses.push_back(ssa::Use{uval->id, 1});

	dce::DeadCodeResult dce;
	dce.liveInstrs.insert(addI->id);
	ExprCoalescer ec;
	auto result = ec.run(fn, dce);

	// Nothing materialises the Undef on its own; it exists in the operand.
	EXPECT_EQ(0u, result.valueExprs.count(uval->id));

	auto it = result.valueExprs.find(sum->id);
	ASSERT_NE(it, result.valueExprs.end());
	ASSERT_NE(it->second, nullptr);
	ASSERT_EQ(2u, it->second->children.size());
	for (const auto& child: it->second->children)
	{
		ASSERT_NE(child, nullptr);
		EXPECT_EQ(CExpr::Kind::Literal, child->kind);
		EXPECT_EQ("0", child->literal);
	}
}

// ─── Loop bodies that are a single statement ─────────────────────────────────
//
// The While, DoWhile and For arms of Emitter::emitStmt wrote
//
//     if (body->kind == CStmt::Kind::Block)
//         for (auto& c : body->children) if (c) s += emitStmt(*c, ...);
//     else
//         s += emitStmt(*body, ...);
//
// with no braces, so the `else` bound to `if (c)`, not to the `if` on the
// kind. A body that is not a Block therefore matched neither branch and was
// dropped, and a Block body with a null child emitted the whole body again.
// -Wdangling-else said so on all three, three times per build.
//
// The If arm two cases above is the same shape written correctly, with braces;
// these check that all four now agree.

TEST(EmitterTest, WhileBodyThatIsASingleStatementIsNotDropped)
{
	Emitter e;
	auto s = CStmt::whileStmt(CExpr::var("cond"));
	s->children.push_back(CStmt::breakStmt()); // not a Block
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("while (cond)"), std::string::npos) << out;
	EXPECT_NE(out.find("break"), std::string::npos) << out;
}

TEST(EmitterTest, DoWhileBodyThatIsASingleStatementIsNotDropped)
{
	Emitter e;
	auto s = CStmt::doWhileStmt(CExpr::var("cond"));
	s->children.push_back(CStmt::retStmt(CExpr::lit("7")));
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("do {"), std::string::npos) << out;
	EXPECT_NE(out.find("return 7"), std::string::npos) << out;
}

TEST(EmitterTest, ForBodyThatIsASingleStatementIsNotDropped)
{
	Emitter e;
	auto s = CStmt::forStmt(nullptr, CExpr::var("cond"), nullptr);
	s->children.push_back(CStmt::retStmt(CExpr::lit("9")));
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);
	EXPECT_NE(out.find("for ("), std::string::npos) << out;
	EXPECT_NE(out.find("return 9"), std::string::npos) << out;
}

TEST(EmitterTest, ANullChildInALoopBodyDoesNotDuplicateTheBody)
{
	// The `else` that bound to `if (c)` re-emitted the whole body for each null
	// child, so a body of {return 3; nullptr} came out with two returns.
	Emitter e;
	auto s = CStmt::whileStmt(CExpr::var("cond"));
	auto body = CStmt::block();
	body->children.push_back(CStmt::retStmt(CExpr::lit("3")));
	body->children.push_back(nullptr);
	s->children.push_back(body);
	Emitter::Config cfg;
	std::string out = e.emitStmt(*s, 0, cfg);

	unsigned returns = 0;
	for (size_t p = out.find("return 3"); p != std::string::npos; p = out.find("return 3", p + 1))
		++returns;
	EXPECT_EQ(1u, returns) << out;
}