/**
 * @file tests/capstone2llvmir/xcore_tests.cpp
 * @brief Capstone2LlvmIrTranslatorXcore unit tests.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/IR/InstIterator.h>

#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/xcore/xcore.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorXcoreTests : public Capstone2LlvmIrTranslatorTests
{
	protected:
		virtual void initKeystoneEngine() override
		{
			// Keystone has no XCore backend. Tests use emulate_bin() only.
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			_translator = Capstone2LlvmIrTranslator::createXcore(
					&_module,
					CS_MODE_BIG_ENDIAN);
		}
};

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_ADD)
{
	setRegisters({
		{XCORE_REG_R2, 0x1111},
		{XCORE_REG_R3, 0x2222},
	});

	emulate_bin("1b 10");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R2, XCORE_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R1, 0x3333},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_ADD_imm)
{
	setRegisters({
		{XCORE_REG_R2, 0x10},
	});

	emulate_bin("e9 92");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R10, 0x15},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_SUB)
{
	setRegisters({
		{XCORE_REG_R2, 0x3333},
		{XCORE_REG_R5, 0x1111},
	});

	emulate_bin("89 1a");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R2, XCORE_REG_R5});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R4, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_AND)
{
	setRegisters({
		{XCORE_REG_R10, 0xff00},
		{XCORE_REG_R9, 0x0ff0},
	});

	emulate_bin("b9 3e");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R10, XCORE_REG_R9});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R11, 0x0f00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_OR)
{
	setRegisters({
		{XCORE_REG_R3, 0xff00},
		{XCORE_REG_R2, 0x00ff},
	});

	emulate_bin("1e 40");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R3, XCORE_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R1, 0xffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_XOR)
{
	setRegisters({
		{XCORE_REG_R3, 0xff00},
		{XCORE_REG_R9, 0x0ff0},
	});

	emulate_bin("cd fc ec 0f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R3, XCORE_REG_R9});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R4, 0xf0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_NOT)
{
	setRegisters({
		{XCORE_REG_R8, 0x0000ffff},
	});

	emulate_bin("24 8f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R8});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R1, 0xffff0000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_NEG)
{
	setRegisters({
		{XCORE_REG_R6, 1},
	});

	emulate_bin("ce 97");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R7, 0xffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_ANDNOT)
{
	setRegisters({
		{XCORE_REG_R10, 0xffff},
		{XCORE_REG_R11, 0x00ff},
	});

	emulate_bin("ab 2f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R10, XCORE_REG_R11});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R10, 0xff00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_EQ)
{
	setRegisters({
		{XCORE_REG_R1, 5},
		{XCORE_REG_R2, 5},
	});

	emulate_bin("66 30");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R1, XCORE_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R6, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_LSS)
{
	setRegisters({
		{XCORE_REG_R3, 1},
		{XCORE_REG_R0, 2},
	});

	emulate_bin("7c c0");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R3, XCORE_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R7, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_LSU)
{
	setRegisters({
		{XCORE_REG_R8, 1},
		{XCORE_REG_R6, 2},
	});

	emulate_bin("12 cc");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R8, XCORE_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R5, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_SHL)
{
	setRegisters({
		{XCORE_REG_R2, 1},
		{XCORE_REG_R4, 4},
	});

	emulate_bin("c8 22");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R2, XCORE_REG_R4});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R8, 0x10},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_SHR)
{
	setRegisters({
		{XCORE_REG_R7, 0x20},
		{XCORE_REG_R1, 1},
	});

	emulate_bin("5d 29");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R7, XCORE_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R9, 0x10},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_MUL)
{
	setRegisters({
		{XCORE_REG_R4, 6},
		{XCORE_REG_R2, 7},
	});

	emulate_bin("c2 f8 ec 3f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R4, XCORE_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R0, 42},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_LDC)
{
	emulate_bin("de 68");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R3, 30},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_LDW)
{
	setRegisters({
		{XCORE_REG_R6, 0x1000},
	});
	setMemory({
		{0x1004, 0xaabbccdd_dw},
	});

	emulate_bin("19 09");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R5, 0xaabbccdd},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_STW)
{
	setRegisters({
		{XCORE_REG_R3, 0x12345678},
		{XCORE_REG_R2, 0x1000},
	});

	emulate_bin("38 00");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R3, XCORE_REG_R2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_BAU)
{
	setRegisters({
		{XCORE_REG_R2, 0x2000},
	});

	emulate_bin("f2 27");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x2000}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_BLA)
{
	setRegisters({
		{XCORE_REG_R6, 0x2000},
	});

	emulate_bin("e6 27", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_LR, 0x1002},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x2000}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_BL)
{
	emulate_bin("08 d0", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_LR, 0x1002},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {ANY}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_BU)
{
	emulate_bin("18 73", 0x1000);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {ANY}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_BF_taken)
{
	setRegisters({
		{XCORE_REG_R5, 0},
	});

	emulate_bin("48 79", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R5});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, ANY}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_RETSP)
{
	setRegisters({
		{XCORE_REG_SP, 0x1000},
		{XCORE_REG_LR, 0x2000},
	});

	emulate_bin("e8 77");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_SP, XCORE_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_SP, 0x1000 + 40 * 4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x2000}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_BITREV)
{
	setRegisters({
		{XCORE_REG_R10, 0x1},
	});

	emulate_bin("26 ff ec 07");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R10});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R1, 0x80000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.bitreverse.i32"), {0x1}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_BYTEREV)
{
	setRegisters({
		{XCORE_REG_R1, 0x12345678},
	});

	emulate_bin("11 ff ec 07");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R4, 0x78563412},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_MKMSK)
{
	emulate_bin("72 a7");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R4, 0x00ffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_SEXT)
{
	setRegisters({
		{XCORE_REG_R8, 0x0000ffff},
	});

	emulate_bin("b1 37");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R8});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R8, 0xffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_ZEXT)
{
	setRegisters({
		{XCORE_REG_R3, 0x12345678},
		{XCORE_REG_R8, 8},
	});

	emulate_bin("2c 47");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R3, XCORE_REG_R8});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R3, 0x78},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_DIVU)
{
	setRegisters({
		{XCORE_REG_R1, 20},
		{XCORE_REG_R3, 4},
	});

	emulate_bin("97 f8 ec 4f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R1, XCORE_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R9, 5},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_DIVS)
{
	setRegisters({
		{XCORE_REG_R7, 0xffffffec},
		{XCORE_REG_R2, 4},
	});

	emulate_bin("2e f9 ec 47");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R7, XCORE_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R6, 0xfffffffb},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_REMU)
{
	setRegisters({
		{XCORE_REG_R2, 20},
		{XCORE_REG_R3, 6},
	});

	emulate_bin("1b f8 ec cf");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R2, XCORE_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R1, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_EXTSP)
{
	setRegisters({
		{XCORE_REG_SP, 0x2000},
	});

	emulate_bin("89 77");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_SP, 0x2000 - 9 * 4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_EXTDP)
{
	setRegisters({
		{XCORE_REG_DP, 0x2000},
	});

	emulate_bin("84 73");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_DP});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_DP, 0x2000 - 4 * 4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_LDA16)
{
	setRegisters({
		{XCORE_REG_R2, 0x1000},
		{XCORE_REG_R1, 3},
	});

	emulate_bin("b9 f8 ec 2f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R2, XCORE_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R11, 0x1000 + 3 * 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_LDAP)
{
	emulate_bin("28 d8", 0x1000);

	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R11, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_LMUL)
{
	setRegisters({
		{XCORE_REG_R2, 2},
		{XCORE_REG_R5, 3},
		{XCORE_REG_R8, 4},
		{XCORE_REG_R10, 5},
	});

	emulate_bin("f9 fa 02 06");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R2, XCORE_REG_R5, XCORE_REG_R8, XCORE_REG_R10});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R11, 0},
		{XCORE_REG_R0, 15},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_LADD)
{
	setRegisters({
		{XCORE_REG_R5, 0xffffffff},
		{XCORE_REG_R1, 1},
		{XCORE_REG_R7, 0},
	});

	emulate_bin("e5 f8 fb 06");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R5, XCORE_REG_R1, XCORE_REG_R7});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R10, 0},
		{XCORE_REG_R2, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_MACCU)
{
	setRegisters({
		{XCORE_REG_R0, 0},
		{XCORE_REG_R2, 1},
		{XCORE_REG_R5, 2},
		{XCORE_REG_R8, 3},
	});

	emulate_bin("44 fd f2 07");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R0, XCORE_REG_R2, XCORE_REG_R5, XCORE_REG_R8});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R0, 0},
		{XCORE_REG_R2, 7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_CRC32_zero)
{
	setRegisters({
		{XCORE_REG_R5, 0},
		{XCORE_REG_R6, 0},
		{XCORE_REG_R1, 0x04c11db7},
	});

	emulate_bin("19 f9 ec af");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R5, XCORE_REG_R6, XCORE_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R5, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_SSYNC)
{
	emulate_bin("ee 07");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_GET)
{
	setRegisters({
		{XCORE_REG_ID, 3},
	});

	emulate_bin("ee 17");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_ID});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R11, 3},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_SET)
{
	setRegisters({
		{XCORE_REG_R3, 0x3000},
	});

	emulate_bin("f3 2f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_SP, 0x3000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_BLAT)
{
	setRegisters({
		{XCORE_REG_CP, 0x1000},
	});
	setMemory({
		{0x1000 + 9 * 4, 0x2000_dw},
	});

	emulate_bin("49 73", 0x3000);

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_CP});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_LR, 0x3002},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000 + 9 * 4});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x2000}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_KENTSP)
{
	setRegisters({
		{XCORE_REG_SP, 0x2000},
	});

	emulate_bin("96 7b");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_KSP, 0x2000},
		{XCORE_REG_SP, 0x2000 - 22 * 4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_ECALLF_not_taken)
{
	setRegisters({
		{XCORE_REG_R5, 1},
		{XCORE_REG_KEP, 0x4000},
	});

	emulate_bin("e5 4f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R5, XCORE_REG_KEP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_ASHR)
{
	setRegisters({
		{XCORE_REG_R1, 0xfffffff0},
	});

	emulate_bin("57 f8 ec 97");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R5, 0xfffffffe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_CLZ)
{
	setRegisters({
		{XCORE_REG_R10, 1},
	});

	emulate_bin("ae ff ec 0f");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R10});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R11, 31},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_IN)
{
	setRegisters({
		{XCORE_REG_R0, 0x1234},
	});

	emulate_bin("48 b7");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R10, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("xcore.chan.in"), {0x1234}},
	});
	EXPECT_EQ(nullptr, _module.getFunction("__asm_in"));
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_OUT)
{
	setRegisters({
		{XCORE_REG_R9, 0x20},
		{XCORE_REG_R10, 0xab},
	});

	emulate_bin("a9 af");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R9, XCORE_REG_R10});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("xcore.chan.out"), {0x20, 0xab}},
	});
	EXPECT_EQ(nullptr, _module.getFunction("__asm_out"));
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, XCORE_INS_PEEK)
{
	setRegisters({
		{XCORE_REG_R5, 0x40},
	});

	emulate_bin("81 bf");

	EXPECT_JUST_REGISTERS_LOADED({XCORE_REG_R5});
	EXPECT_JUST_REGISTERS_STORED({
		{XCORE_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("xcore.chan.peek"), {0x40}},
	});
	EXPECT_EQ(nullptr, _module.getFunction("__asm_peek"));
}

TEST_F(Capstone2LlvmIrTranslatorXcoreTests, translatorIs32Bit)
{
	EXPECT_EQ(4u, _translator->getArchByteSize());
	EXPECT_EQ(32u, _translator->getArchBitSize());
	EXPECT_EQ(CS_ARCH_XCORE, _translator->getArchitecture());
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
