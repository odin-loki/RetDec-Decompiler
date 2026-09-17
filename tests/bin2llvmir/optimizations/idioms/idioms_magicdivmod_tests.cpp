/**
 * @file tests/bin2llvmir/optimizations/idioms/idioms_magicdivmod_tests.cpp
 * @brief Tests for the magic-number division and modulo idioms.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * These recover a division from the multiply-and-shift sequence a compiler
 * emits in its place. The divisor comes back out of arithmetic on the magic
 * number, and until this suite nothing checked what came back.
 *
 * It can come back as zero. Calling divisorByMagicNumberSigned2 over magic
 * < 4096 and shift <= 40 -- 167,936 pairs -- answers 0 on 36,869 of them,
 * because the `q == 0` check inside the helper does not bound its return
 * value: the ceil step `++result` on a uint32_t wraps 0xFFFFFFFF to 0, and the
 * signed helpers divide by an INT32_MIN quotient and truncate to 0. A zero
 * divisor in the emitted module is not a poison value the optimiser carries
 * around, it is immediate undefined behaviour, which licenses it to delete
 * whatever follows -- in a decompiler, the code path being read.
 */

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_analysis.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class IdiomsMagicDivModTests : public LlvmIrTests {
protected:
	/// Run every idiom exchanger over @a fnc. The Pass* is only forwarded
	/// to the multi-block gcc exchanger, which never dereferences it.
	bool runIdioms(CC_compiler cc = CC_ANY, CC_arch arch = ARCH_ANY)
	{
		IdiomsAnalysis a(module.get(), cc, arch);
		return a.doAnalysis(*module->getFunction("fnc"), nullptr);
	}

	/// Is there a division or remainder anywhere whose divisor is a
	/// constant smaller than two in magnitude?
	bool hasUnusableDivisor()
	{
		for (auto& f: *module)
		{
			for (auto& bb: f)
			{
				for (auto& i: bb)
				{
					unsigned op = i.getOpcode();
					if (op != Instruction::UDiv && op != Instruction::SDiv && op != Instruction::URem
						&& op != Instruction::SRem)
					{
						continue;
					}
					auto* c = dyn_cast<ConstantInt>(i.getOperand(1));
					if (c == nullptr)
					{
						continue;
					}
					if (c->getValue().abs().ule(1))
					{
						return true;
					}
				}
			}
		}
		return false;
	}
};

//
// magicSignedDiv1, via divisorByMagicNumberSigned2
//
// magic = 3, shift = 0 recovers a divisor of 0. Measured by calling the helper
// directly; the arithmetic is (3 * INT32_MAX) >> 32 = 1, INT32_MAX + 1 narrowed
// to int32_t is INT32_MIN, and INT32_MAX / INT32_MIN truncates to 0.
//

TEST_F(IdiomsMagicDivModTests, aRecoveredDivisorOfZeroIsNotEmitted)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%sext = sext i32 %x to i64
			%mul = mul i64 %sext, 3
			%lshr = lshr i64 %mul, 32
			%trunc = trunc i64 %lshr to i32
			%add = add i32 %trunc, %x
			%ashr1 = ashr i32 %add, 0
			%ashr2 = ashr i32 %x, 31
			%sub = sub i32 %ashr1, %ashr2
			ret i32 %sub
		}
	)");

	runIdioms();

	EXPECT_FALSE(hasUnusableDivisor()) << "a magic-number sequence was turned into a division by zero";
}

//
// The whole point of the pass still has to work
//
// 0x55555556 is the magic number gcc emits for a signed divide by 3.
//

TEST_F(IdiomsMagicDivModTests, aGenuineMagicSequenceStillRecoversItsDivisor)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%sext = sext i32 %x to i64
			%mul = mul i64 %sext, 1431655766
			%lshr = lshr i64 %mul, 32
			%trunc = trunc i64 %lshr to i32
			%ashr1 = ashr i32 %trunc, 0
			%ashr2 = ashr i32 %x, 31
			%sub = sub i32 %ashr1, %ashr2
			ret i32 %sub
		}
	)");

	runIdioms();

	bool found = false;
	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			if (i.getOpcode() != Instruction::SDiv)
			{
				continue;
			}
			auto* c = dyn_cast<ConstantInt>(i.getOperand(1));
			if (c && c->equalsInt(3))
			{
				found = true;
			}
		}
	}
	EXPECT_TRUE(found) << "the divide by 3 was not recovered";
	EXPECT_FALSE(hasUnusableDivisor());
}

//
// unsignedMod: x - (x/k)*k is x % k. x - x/k is not.
//
// For x = 10 and k = 2 the subtraction is 5 and the remainder is 0.
//

TEST_F(IdiomsMagicDivModTests, aBareSubtractionOfTheQuotientIsNotARemainder)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%d = udiv i32 %x, 7
			%r = sub i32 %x, %d
			ret i32 %r
		}
	)");

	runIdioms();

	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			EXPECT_NE(Instruction::URem, i.getOpcode()) << "x - x/k was rewritten as x % k";
		}
	}
}

TEST_F(IdiomsMagicDivModTests, theMultipleTakenBackOutIsARemainder)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%d = udiv i32 %x, 7
			%m = mul i32 %d, 7
			%r = sub i32 %x, %m
			ret i32 %r
		}
	)");

	runIdioms();

	bool found = false;
	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			auto* c = i.getNumOperands() > 1 ? dyn_cast<ConstantInt>(i.getOperand(1)) : nullptr;
			if (i.getOpcode() == Instruction::URem && c && c->equalsInt(7))
			{
				found = true;
			}
		}
	}
	EXPECT_TRUE(found) << "x - (x/7)*7 was not recognised as x % 7";
}

//
// exchangeUnsignedModulo2n: the mask plus one, at the mask's own width
//
// `and i8 x, -1` made the power-of-two test see 0x100 -- narrowed to unsigned
// -- while the modulus was built as APInt(8, 0xFF) + 1, which is zero.
//

TEST_F(IdiomsMagicDivModTests, anAllOnesMaskIsNotAModulo)
{
	parseInput(R"(
		define i8 @fnc(i8 %x) {
			%r = and i8 %x, -1
			ret i8 %r
		}
	)");

	runIdioms(CC_GCC);

	EXPECT_FALSE(hasUnusableDivisor()) << "and x, 0xFF became urem x, 0";
}

TEST_F(IdiomsMagicDivModTests, aGenuinePowerOfTwoMaskIsStillAModulo)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%r = and i32 %x, 15
			ret i32 %r
		}
	)");

	runIdioms(CC_GCC);

	bool found = false;
	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			auto* c = i.getNumOperands() > 1 ? dyn_cast<ConstantInt>(i.getOperand(1)) : nullptr;
			if (i.getOpcode() == Instruction::URem && c && c->equalsInt(16))
			{
				found = true;
			}
		}
	}
	EXPECT_TRUE(found) << "and x, 15 was not recognised as x % 16";
}

//
// exchangeCondBitShiftDiv1: the divisor is 2^(ashr amount), not 2^(addend)
//
// One `cnst` was reused for the ashr amount, the icmp constant and the add
// constant, so by the time the divisor was built it held the addend N-1 and
// pow(2, N-1) was emitted. For N = 8 that is 128, not 8.
//

TEST_F(IdiomsMagicDivModTests, condBitShiftDivRecoversTheShiftNotTheAddend)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%cmp = icmp slt i32 %x, 0
			%add = add i32 %x, 7
			%sel = select i1 %cmp, i32 %add, i32 %x
			%div = ashr i32 %sel, 3
			ret i32 %div
		}
	)");

	runIdioms(CC_GCC);

	bool found = false;
	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			if (i.getOpcode() != Instruction::SDiv)
			{
				continue;
			}
			auto* c = dyn_cast<ConstantInt>(i.getOperand(1));
			EXPECT_FALSE(c && c->equalsInt(128)) << "the divisor was built from the addend, 2^(N-1)";
			if (c && c->equalsInt(8))
			{
				found = true;
			}
		}
	}
	EXPECT_TRUE(found) << "the divide by 8 was not recovered";
}

// The three X bindings have to be one value.
TEST_F(IdiomsMagicDivModTests, condBitShiftDivNeedsOneVariableThroughout)
{
	parseInput(R"(
		define i32 @fnc(i32 %a, i32 %b, i32 %c) {
			%cmp = icmp slt i32 %a, 0
			%add = add i32 %b, 7
			%sel = select i1 %cmp, i32 %add, i32 %c
			%div = ashr i32 %sel, 3
			ret i32 %div
		}
	)");

	runIdioms(CC_GCC);

	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			EXPECT_NE(Instruction::SDiv, i.getOpcode()) << "three unrelated values were treated as one";
		}
	}
}

//
// exchangeIntegerAbs: the subtrahend must be the arithmetic shift
//

TEST_F(IdiomsMagicDivModTests, integerAbsNeedsTheShiftAsTheSubtrahend)
{
	parseInput(R"(
		define i32 @fnc(i32 %x, i32 %y) {
			%sx = ashr i32 %x, 31
			%xo = xor i32 %x, %sx
			%r = sub i32 %xo, %y
			ret i32 %r
		}
	)");

	runIdioms();

	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			EXPECT_FALSE(isa<SelectInst>(&i)) << "(x ^ (x s>> 31)) - y became abs(x) for an unrelated y";
		}
	}
}

TEST_F(IdiomsMagicDivModTests, integerAbsIsStillRecognised)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%sx = ashr i32 %x, 31
			%xo = xor i32 %x, %sx
			%r = sub i32 %xo, %sx
			ret i32 %r
		}
	)");

	runIdioms();

	bool found = false;
	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			if (isa<SelectInst>(&i))
			{
				found = true;
			}
		}
	}
	EXPECT_TRUE(found) << "abs(x) was not recovered";
}

//
// exchangeBitShiftSDiv1: the reassembly is an arithmetic shift, not a division
//
// `((x s>> 31) & mask) | (x u>> k)` equals `ashr x, k`. A signed division
// truncates toward zero where the shift floors, so for x = -5 and k = 1 they
// are -2 and -3.
//

TEST_F(IdiomsMagicDivModTests, theReassembledShiftIsAShift)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%sx = ashr i32 %x, 31
			%an = and i32 %sx, -1073741824
			%ls = lshr i32 %x, 2
			%r = or i32 %an, %ls
			ret i32 %r
		}
	)");

	runIdioms();

	bool sdiv = false;
	bool ashr = false;
	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			if (i.getOpcode() == Instruction::SDiv)
			{
				sdiv = true;
			}
			auto* c = i.getNumOperands() > 1 ? dyn_cast<ConstantInt>(i.getOperand(1)) : nullptr;
			if (i.getOpcode() == Instruction::AShr && c && c->equalsInt(2))
			{
				ashr = true;
			}
		}
	}
	EXPECT_FALSE(sdiv) << "an arithmetic shift was rendered as a division";
	EXPECT_TRUE(ashr) << "the reassembly was not simplified to a shift";
}

TEST_F(IdiomsMagicDivModTests, theReassembledShiftNeedsOneVariable)
{
	parseInput(R"(
		define i32 @fnc(i32 %a, i32 %b) {
			%sa = ashr i32 %a, 31
			%an = and i32 %sa, -1073741824
			%ls = lshr i32 %b, 2
			%r = or i32 %an, %ls
			ret i32 %r
		}
	)");

	runIdioms();

	// The input already contains `ashr %a, 31`; what must not appear is a
	// shift by the lshr's amount, or a division.
	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			EXPECT_NE(Instruction::SDiv, i.getOpcode());
			auto* c = i.getNumOperands() > 1 ? dyn_cast<ConstantInt>(i.getOperand(1)) : nullptr;
			EXPECT_FALSE(i.getOpcode() == Instruction::AShr && c && c->equalsInt(2))
				<< "two unrelated values were treated as one";
		}
	}
}

//
// exchangeGreaterEqualZero: the sign bit, at the value's own width
//

TEST_F(IdiomsMagicDivModTests, aSignMaskTestAtSixtyFourBits)
{
	parseInput(R"(
		define i1 @fnc(i64 %x) {
			%a = and i64 %x, 2147483648
			%c = icmp eq i64 %a, 0
			ret i1 %c
		}
	)");

	runIdioms(CC_LLVM);

	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			auto* cmp = dyn_cast<ICmpInst>(&i);
			EXPECT_FALSE(cmp && cmp->getPredicate() == ICmpInst::ICMP_SGT)
				<< "bit 31 of an i64 was treated as the sign bit";
		}
	}
}

TEST_F(IdiomsMagicDivModTests, aSignMaskTestAtThirtyTwoBitsStillFires)
{
	parseInput(R"(
		define i1 @fnc(i32 %x) {
			%a = and i32 %x, -2147483648
			%c = icmp eq i32 %a, 0
			ret i1 %c
		}
	)");

	runIdioms(CC_LLVM);

	bool found = false;
	for (auto& bb: *module->getFunction("fnc"))
	{
		for (auto& i: bb)
		{
			auto* cmp = dyn_cast<ICmpInst>(&i);
			if (cmp && cmp->getPredicate() == ICmpInst::ICMP_SGT)
			{
				found = true;
			}
		}
	}
	EXPECT_TRUE(found) << "(x & signbit) == 0 was not recognised as x > -1";
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
