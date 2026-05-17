/**
* @file tests/llvmir2hll/analysis/used_vars_visitor_tests.cpp
* @brief Tests for the @c used_vars_visitor module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/analysis/used_vars_visitor.h"
#include "retdec/llvmir2hll/ir/array_index_op_expr.h"
#include "retdec/llvmir2hll/ir/array_type.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/deref_op_expr.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/struct_index_op_expr.h"
#include "retdec/llvmir2hll/ir/struct_type.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "llvmir2hll/ir/tests_with_module.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c used_vars_visitor module.
*/
class UsedVarsVisitorTests: public TestsWithModule {};

// ---------------------------------------------------------------------------
// Basic read/write tracking
// ---------------------------------------------------------------------------

TEST_F(UsedVarsVisitorTests,
NoVarsUsedInEmptyAssignment) {
	// a = 1 (no variable on RHS)
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assign));

	ASSERT_EQ(1u, used->getWrittenVars().size()) << "a should be written";
	ASSERT_EQ(0u, used->getReadVars().size()) << "no vars should be read";
	ASSERT_TRUE(used->isUsed(varA, false, true)) << "a is written";
	ASSERT_FALSE(used->isUsed(varA, true, false)) << "a is not read";
}

TEST_F(UsedVarsVisitorTests,
BothReadAndWrittenVarsInAssignment) {
	// a = b
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, varB));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assign));

	ASSERT_TRUE(used->isUsed(varA, false, true)) << "a is written";
	ASSERT_TRUE(used->isUsed(varB, true, false)) << "b is read";
	ASSERT_EQ(2u, used->getAllVars().size());
}

TEST_F(UsedVarsVisitorTests,
VarDefStmtWithInitializerTracksBothReadAndWrite) {
	// int a = b;
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<VarDefStmt> varDef(VarDefStmt::create(varA, varB));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(varDef));

	ASSERT_TRUE(used->isUsed(varA, false, true)) << "a is written";
	ASSERT_TRUE(used->isUsed(varB, true, false)) << "b is read";
}

TEST_F(UsedVarsVisitorTests,
VarDefStmtWithoutInitializerTracksOnlyWrite) {
	// int a;
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<VarDefStmt> varDef(VarDefStmt::create(varA));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(varDef));

	ASSERT_TRUE(used->isUsed(varA, false, true)) << "a is written";
	ASSERT_FALSE(used->isUsed(varA, true, false)) << "a is not read";
	ASSERT_EQ(0u, used->getReadVars().size());
}

TEST_F(UsedVarsVisitorTests,
NumOfUsesCountedCorrectly) {
	// a = a + a   (a read twice, written once)
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	// We model "a + a" as two reads of a on the RHS
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, varA));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assign));

	// a is written once (LHS) and read once (RHS)
	ASSERT_GE(used->getNumOfUses(varA), 1u);
}

// ---------------------------------------------------------------------------
// ArrayIndexOpExpr: array[index] = ...
// ---------------------------------------------------------------------------

TEST_F(UsedVarsVisitorTests,
ArrayIndexLhsIsRead) {
	// arr[0] = 1    -> arr is READ (not written) even though it's on LHS
	ShPtr<Variable> varArr(Variable::create("arr",
		ArrayType::create(IntType::create(32), {10})));
	ShPtr<ArrayIndexOpExpr> arrIdx(ArrayIndexOpExpr::create(
		varArr, ConstInt::create(0, 32)));
	ShPtr<AssignStmt> assign(AssignStmt::create(arrIdx, ConstInt::create(1, 32)));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assign));

	// arr is read (array base pointer is read, not written)
	ASSERT_TRUE(used->isUsed(varArr, true, false)) << "arr should be read";
}

// ---------------------------------------------------------------------------
// DerefOpExpr: *p = ...
// ---------------------------------------------------------------------------

TEST_F(UsedVarsVisitorTests,
DerefLhsPointerIsRead) {
	// *p = 1  -> p is READ (not written)
	ShPtr<Variable> varP(Variable::create("p", PointerType::create(IntType::create(32))));
	ShPtr<DerefOpExpr> deref(DerefOpExpr::create(varP));
	ShPtr<AssignStmt> assign(AssignStmt::create(deref, ConstInt::create(1, 32)));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assign));

	ASSERT_TRUE(used->isUsed(varP, true, false)) << "p is read (dereferenced)";
	ASSERT_FALSE(used->isUsed(varP, false, true)) << "p is not written";
}

// ---------------------------------------------------------------------------
// StructIndexOpExpr: s.field = ...
// ---------------------------------------------------------------------------

TEST_F(UsedVarsVisitorTests,
StructIndexLhsIsRead) {
	// s.field = 1  -> s is READ (not written)
	StructType::ElementTypes elems = {IntType::create(32)};
	ShPtr<StructType> sType(StructType::create(elems, "MyStruct"));
	ShPtr<Variable> varS(Variable::create("s", sType));
	// field 0 (index 0)
	ShPtr<StructIndexOpExpr> structIdx(StructIndexOpExpr::create(varS,
		ConstInt::create(0, 32)));
	ShPtr<AssignStmt> assign(AssignStmt::create(structIdx, ConstInt::create(1, 32)));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assign));

	ASSERT_TRUE(used->isUsed(varS, true, false)) << "s is read";
}

// ---------------------------------------------------------------------------
// ForLoopStmt
// ---------------------------------------------------------------------------

TEST_F(UsedVarsVisitorTests,
ForLoopStmtTracksIndVarAsWritten) {
	// for (i = 0; i < 10; i++) { int a; }
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));

	ShPtr<VarDefStmt> body(VarDefStmt::create(varA));
	ShPtr<Expression> step(ConstInt::create(1, 32));
	ShPtr<Expression> cond(LtOpExpr::create(varI, ConstInt::create(10, 32)));
	ShPtr<ForLoopStmt> forLoop(ForLoopStmt::create(
		varI, ConstInt::create(0, 32), cond, step, body));

	// Without visiting nested stmts
	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(forLoop, false, false));

	ASSERT_TRUE(used->isUsed(varI, false, true)) << "i is the loop variable (written)";
	ASSERT_TRUE(used->isUsed(varI, true, false)) << "i is read in the condition";
}

TEST_F(UsedVarsVisitorTests,
ForLoopWithNestedStmtsVisitsBody) {
	// for (i = 0; i < 10; i++) { a = b; }
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));

	ShPtr<AssignStmt> body(AssignStmt::create(varA, varB));
	ShPtr<Expression> step(ConstInt::create(1, 32));
	ShPtr<Expression> cond(LtOpExpr::create(varI, ConstInt::create(10, 32)));
	ShPtr<ForLoopStmt> forLoop(ForLoopStmt::create(
		varI, ConstInt::create(0, 32), cond, step, body));

	// With visiting nested stmts
	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(forLoop, false, true));

	ASSERT_TRUE(used->isUsed(varI)) << "i should be found";
	ASSERT_TRUE(used->isUsed(varA)) << "a should be found (in body)";
	ASSERT_TRUE(used->isUsed(varB)) << "b should be found (in body)";
}

// ---------------------------------------------------------------------------
// getCount API
// ---------------------------------------------------------------------------

TEST_F(UsedVarsVisitorTests,
GetCountReturnsTotalVarCount) {
	// a = b  -> 2 vars total
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, varB));

	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assign));

	ASSERT_EQ(1u, used->getCount(false, true)) << "1 written var";
	ASSERT_EQ(1u, used->getCount(true, false)) << "1 read var";
	ASSERT_EQ(2u, used->getCount(true, true)) << "2 total vars";
}

// ---------------------------------------------------------------------------
// visitSuccessors option
// ---------------------------------------------------------------------------

TEST_F(UsedVarsVisitorTests,
WithVisitSuccessorsVisitsNextStatement) {
	// a = 1; b = 2;
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> assignB(AssignStmt::create(varB, ConstInt::create(2, 32)));
	ShPtr<AssignStmt> assignA(AssignStmt::create(varA, ConstInt::create(1, 32)));
	assignA->setSuccessor(assignB);

	// With visitSuccessors=true, should see both a and b
	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assignA, true, false));
	ASSERT_TRUE(used->isUsed(varA)) << "a should be used";
	ASSERT_TRUE(used->isUsed(varB)) << "b should be used (from successor)";
}

TEST_F(UsedVarsVisitorTests,
WithoutVisitSuccessorsOnlyVisitsFirstStatement) {
	// a = 1; b = 2;
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> assignB(AssignStmt::create(varB, ConstInt::create(2, 32)));
	ShPtr<AssignStmt> assignA(AssignStmt::create(varA, ConstInt::create(1, 32)));
	assignA->setSuccessor(assignB);

	// Without visitSuccessors, should only see a
	ShPtr<UsedVars> used(UsedVarsVisitor::getUsedVars(assignA, false, false));
	ASSERT_TRUE(used->isUsed(varA)) << "a should be used";
	ASSERT_FALSE(used->isUsed(varB)) << "b should not be used (successor not visited)";
}

// ---------------------------------------------------------------------------
// Equality operators
// ---------------------------------------------------------------------------

TEST_F(UsedVarsVisitorTests,
EqualUsedVarsCompareEqual) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<AssignStmt> assign1(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> assign2(AssignStmt::create(varA, ConstInt::create(1, 32)));

	ShPtr<UsedVars> used1(UsedVarsVisitor::getUsedVars(assign1));
	ShPtr<UsedVars> used2(UsedVarsVisitor::getUsedVars(assign2));

	ASSERT_EQ(*used1, *used2);
	ASSERT_FALSE(*used1 != *used2);
}

TEST_F(UsedVarsVisitorTests,
DifferentUsedVarsCompareUnequal) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> assign1(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> assign2(AssignStmt::create(varB, ConstInt::create(1, 32)));

	ShPtr<UsedVars> used1(UsedVarsVisitor::getUsedVars(assign1));
	ShPtr<UsedVars> used2(UsedVarsVisitor::getUsedVars(assign2));

	ASSERT_NE(*used1, *used2);
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
