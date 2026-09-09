/**
* @file tests/llvmir2hll/support/global_vars_sorter_tests.cpp
* @brief Tests for the @c global_vars_sorter module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <set>

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/address_op_expr.h"
#include "retdec/llvmir2hll/ir/global_var_def.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/support/global_vars_sorter.h"
#include "retdec/llvmir2hll/support/types.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c global_vars_sorter module.
*/
class GlobalVarsSorterTests: public TestsWithModule {};

TEST_F(GlobalVarsSorterTests,
NoGlobalVarsReturnsEmptyVector) {
	GlobalVarDefVector globalVars;
	GlobalVarDefVector refSortedGlobalVars(globalVars);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

TEST_F(GlobalVarsSorterTests,
SingleGlobalVarReturnsSingletonVector) {
	GlobalVarDefVector globalVars;

	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Expression> varAInit;
	globalVars.push_back(GlobalVarDef::create(varA, varAInit));

	GlobalVarDefVector refSortedGlobalVars(globalVars);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

TEST_F(GlobalVarsSorterTests,
WhenThereAreNoInterdependenciesTheVariablesAreSortedByOriginalName) {
	//
	// int a;
	// int b;
	// int c;
	//

	GlobalVarDefVector globalVars;

	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	// Change the name so that we can test that the variables are sorted by
	// their original name.
	varA->setName("z");
	ShPtr<Expression> varAInit;
	ShPtr<GlobalVarDef> varADef(GlobalVarDef::create(varA, varAInit));
	globalVars.push_back(varADef);

	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	// Change the name so that we can test that the variables are sorted by
	// their original name.
	varB->setName("y");
	ShPtr<Expression> varBInit;
	ShPtr<GlobalVarDef> varBDef(GlobalVarDef::create(varB, varBInit));
	globalVars.push_back(varBDef);

	ShPtr<Variable> varC(Variable::create("c", IntType::create(32)));
	// Change the name so that we can test that the variables are sorted by
	// their original name.
	varC->setName("x");
	ShPtr<Expression> varCInit;
	ShPtr<GlobalVarDef> varCDef(GlobalVarDef::create(varC, varCInit));
	globalVars.push_back(varCDef);

	GlobalVarDefVector refSortedGlobalVars;
	refSortedGlobalVars.push_back(varADef);
	refSortedGlobalVars.push_back(varBDef);
	refSortedGlobalVars.push_back(varCDef);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

TEST_F(GlobalVarsSorterTests,
TwoGlobalVarsWithInterdependenciesTharAreAlreadyOrderedUntouched) {
	//
	// int a;
	// int b = a;
	//

	GlobalVarDefVector globalVars;

	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Expression> varAInit;
	ShPtr<GlobalVarDef> varADef(GlobalVarDef::create(varA, varAInit));
	globalVars.push_back(varADef);

	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<Expression> varBInit(varA);
	ShPtr<GlobalVarDef> varBDef(GlobalVarDef::create(varB, varBInit));
	globalVars.push_back(varBDef);

	GlobalVarDefVector refSortedGlobalVars(globalVars);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

TEST_F(GlobalVarsSorterTests,
TwoGlobalVarsWithInterdependenciesInReverseOrderGetsCorrectlyOrdered) {
	//
	// int b = a;
	// int a;
	//

	GlobalVarDefVector globalVars;

	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<Expression> varBInit(varA);
	ShPtr<GlobalVarDef> varBDef(GlobalVarDef::create(varB, varBInit));
	globalVars.push_back(varBDef);

	ShPtr<Expression> varAInit;
	ShPtr<GlobalVarDef> varADef(GlobalVarDef::create(varA, varAInit));
	globalVars.push_back(varADef);

	GlobalVarDefVector refSortedGlobalVars;
	refSortedGlobalVars.push_back(varADef);
	refSortedGlobalVars.push_back(varBDef);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

TEST_F(GlobalVarsSorterTests,
ThreeGlobalVarsWithInterdependenciesGetsCorrectlyOrdered) {
	//
	// int b = a;
	// int a;
	// int c = b;
	//

	GlobalVarDefVector globalVars;

	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<Expression> varBInit(varA);
	ShPtr<GlobalVarDef> varBDef(GlobalVarDef::create(varB, varBInit));
	globalVars.push_back(varBDef);

	ShPtr<Expression> varAInit;
	ShPtr<GlobalVarDef> varADef(GlobalVarDef::create(varA, varAInit));
	globalVars.push_back(varADef);

	ShPtr<Variable> varC(Variable::create("c", IntType::create(32)));
	ShPtr<Expression> varCInit(varB);
	ShPtr<GlobalVarDef> varCDef(GlobalVarDef::create(varC, varCInit));
	globalVars.push_back(varCDef);

	GlobalVarDefVector refSortedGlobalVars;
	refSortedGlobalVars.push_back(varADef);
	refSortedGlobalVars.push_back(varBDef);
	refSortedGlobalVars.push_back(varCDef);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

TEST_F(GlobalVarsSorterTests,
SortingWorksCorrectlyEvenIfVariableIsNested) {
	//
	// int *b = &a;
	// int a;
	//

	GlobalVarDefVector globalVars;

	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", PointerType::create(
		IntType::create(32))));
	ShPtr<Expression> varBInit(AddressOpExpr::create(varA));
	ShPtr<GlobalVarDef> varBDef(GlobalVarDef::create(varB, varBInit));
	globalVars.push_back(varBDef);

	ShPtr<Expression> varAInit;
	ShPtr<GlobalVarDef> varADef(GlobalVarDef::create(varA, varAInit));
	globalVars.push_back(varADef);

	GlobalVarDefVector refSortedGlobalVars;
	refSortedGlobalVars.push_back(varADef);
	refSortedGlobalVars.push_back(varBDef);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

TEST_F(GlobalVarsSorterTests,
MutuallyDependentVariablesAreSortedByOriginalName) {
	//
	// int *a = &b;
	// int *b = &a;
	//
	// Neither variable can be defined before the other, so the two rules that
	// order a variable before the one whose initializer names it both apply,
	// in both directions.  Read as a comparator that says "a < b" and "b < a"
	// at the same time, which is not an ordering: std::sort is entitled to
	// walk off the end of the range looking for a place to stop.  Such a pair
	// is ordered on the name, the way independent variables are.
	//
	// The variables are pushed in name order, so a comparator that answers
	// "less than" for both directions reverses them and this test sees it.
	//

	GlobalVarDefVector globalVars;

	ShPtr<Variable> varA(Variable::create("a", PointerType::create(
		IntType::create(32))));
	ShPtr<Variable> varB(Variable::create("b", PointerType::create(
		IntType::create(32))));

	ShPtr<GlobalVarDef> varADef(GlobalVarDef::create(varA,
		AddressOpExpr::create(varB)));
	globalVars.push_back(varADef);

	ShPtr<GlobalVarDef> varBDef(GlobalVarDef::create(varB,
		AddressOpExpr::create(varA)));
	globalVars.push_back(varBDef);

	GlobalVarDefVector refSortedGlobalVars;
	refSortedGlobalVars.push_back(varADef);
	refSortedGlobalVars.push_back(varBDef);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

TEST_F(GlobalVarsSorterTests,
SelfReferentialVariableDoesNotDisturbTheOrder) {
	//
	// int *a = &a;
	// int b;
	//
	// A variable whose initializer names itself is the degenerate case of the
	// pair above -- the comparator used to answer "a < a" -- and nothing else
	// covers the shape.  std::sort never compares an element with itself, so
	// this pins the resulting order rather than the comparator's answer: it
	// passes either way, and is here to say what the sorter does with a
	// self-referential global.
	//

	GlobalVarDefVector globalVars;

	ShPtr<Variable> varA(Variable::create("a", PointerType::create(
		IntType::create(32))));
	ShPtr<GlobalVarDef> varADef(GlobalVarDef::create(varA,
		AddressOpExpr::create(varA)));
	globalVars.push_back(varADef);

	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<Expression> varBInit;
	ShPtr<GlobalVarDef> varBDef(GlobalVarDef::create(varB, varBInit));
	globalVars.push_back(varBDef);

	// b has no initializer, so it is emitted before the variable that has one.
	GlobalVarDefVector refSortedGlobalVars;
	refSortedGlobalVars.push_back(varBDef);
	refSortedGlobalVars.push_back(varADef);

	EXPECT_EQ(refSortedGlobalVars,
		GlobalVarsSorter::sortByInterdependencies(globalVars));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
