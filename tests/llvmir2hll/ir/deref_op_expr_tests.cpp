/**
* @file tests/llvmir2hll/ir/deref_op_expr_tests.cpp
* @brief Tests for the @c deref_op_expr module.
* @copyright (c) 2025-2026 Odin Loch trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/array_type.h"
#include "retdec/llvmir2hll/ir/deref_op_expr.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/unknown_type.h"
#include "retdec/llvmir2hll/ir/variable.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class DerefOpExprTests: public Test {};

TEST_F(DerefOpExprTests,
GetTypeOfDerefOfPointerIsThePointee) {
	ShPtr<IntType> i32(IntType::create(32));
	ShPtr<Variable> p(Variable::create("p", PointerType::create(i32)));

	EXPECT_EQ(i32, DerefOpExpr::create(p)->getType());
}

TEST_F(DerefOpExprTests,
GetTypeOfDerefOfPointerToPointerIsOneLevelDown) {
	ShPtr<IntType> i8(IntType::create(8));
	ShPtr<PointerType> pi8(PointerType::create(i8));
	ShPtr<Variable> pp(Variable::create("pp", PointerType::create(pi8)));

	EXPECT_EQ(pi8, DerefOpExpr::create(pp)->getType());
	EXPECT_EQ(i8, DerefOpExpr::create(DerefOpExpr::create(pp))->getType());
}

TEST_F(DerefOpExprTests,
GetTypeOfDerefOfArrayIsTheElement) {
	ShPtr<IntType> i16(IntType::create(16));
	ShPtr<Variable> a(Variable::create("a",
		ArrayType::create(i16, ArrayType::Dimensions{4})));

	EXPECT_EQ(i16, DerefOpExpr::create(a)->getType());
}

TEST_F(DerefOpExprTests,
GetTypeOfDerefOfUntypedOperandIsTheOperandsType) {
	ShPtr<IntType> i64(IntType::create(64));
	ShPtr<Variable> x(Variable::create("x", i64));

	EXPECT_EQ(i64, DerefOpExpr::create(x)->getType());
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
