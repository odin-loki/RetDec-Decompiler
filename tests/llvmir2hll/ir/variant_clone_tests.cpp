/**
 * @file tests/llvmir2hll/ir/variant_clone_tests.cpp
 * @brief Cloning an expression keeps its signed/unsigned variant.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * ExtCastExpr::clone() dropped the variant, so every copy-propagated sign
 * extension became a zero extension (x64 -O1 vmix: `s ^= va_arg(ap, int)`
 * printed as `(uint64_t)*(uint32_t *)p`).
 */

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/div_op_expr.h"
#include "retdec/llvmir2hll/ir/ext_cast_expr.h"
#include "retdec/llvmir2hll/ir/float_type.h"
#include "retdec/llvmir2hll/ir/int_to_fp_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/mod_op_expr.h"
#include "retdec/llvmir2hll/ir/variable.h"

namespace retdec {
namespace llvmir2hll {
namespace tests {

TEST(VariantCloneTests, ExtCastCloneKeepsSignExtension) {
	auto v = Variable::create("a", IntType::create(32, false));
	auto e = ExtCastExpr::create(v, IntType::create(64, false),
		ExtCastExpr::Variant::SExt);
	auto c = cast<ExtCastExpr>(e->clone());
	ASSERT_TRUE(c);
	EXPECT_EQ(ExtCastExpr::Variant::SExt, c->getVariant());
}

TEST(VariantCloneTests, IntToFPCloneKeepsSignedConversion) {
	auto v = Variable::create("a", IntType::create(32, false));
	auto e = IntToFPCastExpr::create(v, FloatType::create(32),
		IntToFPCastExpr::Variant::SIToFP);
	auto c = cast<IntToFPCastExpr>(e->clone());
	ASSERT_TRUE(c);
	EXPECT_EQ(IntToFPCastExpr::Variant::SIToFP, c->getVariant());
}

TEST(VariantCloneTests, DivAndModClonesKeepVariant) {
	auto a = Variable::create("a", IntType::create(32, false));
	auto b = Variable::create("b", IntType::create(32, false));
	auto d = cast<DivOpExpr>(DivOpExpr::create(a, b,
		DivOpExpr::Variant::SDiv)->clone());
	auto m = cast<ModOpExpr>(ModOpExpr::create(a, b,
		ModOpExpr::Variant::SMod)->clone());
	ASSERT_TRUE(d && m);
	EXPECT_EQ(DivOpExpr::Variant::SDiv, d->getVariant());
	EXPECT_EQ(ModOpExpr::Variant::SMod, m->getVariant());
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
