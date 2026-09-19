/**
 * @file tests/capstone2llvmir/sysz_tests.cpp
 * @brief Capstone2LlvmIrTranslatorSysz unit tests.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/IR/InstIterator.h>

#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/sysz/sysz.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorSyszTests : public Capstone2LlvmIrTranslatorTests
{
	protected:
		virtual void initKeystoneEngine() override
		{
			if (ks_open(KS_ARCH_SYSTEMZ, KS_MODE_BIG_ENDIAN, &_assembler) != KS_ERR_OK)
			{
				throw std::runtime_error("ERROR: failed on ks_open() for SystemZ.\n");
			}
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			_translator = Capstone2LlvmIrTranslator::createSysz(
					&_module,
					CS_MODE_BIG_ENDIAN);
		}
};

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LR)
{
	setRegisters({
		{SYSZ_REG_1, 0x111100000000ull},
		{SYSZ_REG_2, 0x2222},
	});

	emulate_bin("18 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x111100002222ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LGR)
{
	setRegisters({
		{SYSZ_REG_1, 0x1111},
		{SYSZ_REG_2, 0x0123456789abcdefull},
	});

	emulate_bin("b9 04 00 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x0123456789abcdefull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_AR)
{
	setRegisters({
		{SYSZ_REG_1, 0x1111},
		{SYSZ_REG_2, 0x2222},
	});

	emulate_bin("1a 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x3333},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_AGR)
{
	setRegisters({
		{SYSZ_REG_1, 0x100000000ull},
		{SYSZ_REG_2, 0x200000000ull},
	});

	emulate_bin("b9 08 00 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x300000000ull},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_SR)
{
	setRegisters({
		{SYSZ_REG_1, 0x3333},
		{SYSZ_REG_2, 0x1111},
	});

	emulate_bin("1b 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x2222},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_SGR)
{
	setRegisters({
		{SYSZ_REG_1, 0x300000000ull},
		{SYSZ_REG_2, 0x100000000ull},
	});

	emulate_bin("b9 09 00 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x200000000ull},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_NR)
{
	setRegisters({
		{SYSZ_REG_1, 0xff00},
		{SYSZ_REG_2, 0x0ff0},
	});

	emulate_bin("14 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x0f00},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_OR)
{
	setRegisters({
		{SYSZ_REG_1, 0xff00},
		{SYSZ_REG_2, 0x00ff},
	});

	emulate_bin("16 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xffff},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_XR)
{
	setRegisters({
		{SYSZ_REG_1, 0xff00},
		{SYSZ_REG_2, 0x0ff0},
	});

	emulate_bin("17 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xf0f0},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_L)
{
	setRegisters({
		{SYSZ_REG_1, 0xaaaabbbb00000000ull},
		{SYSZ_REG_2, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate_bin("58 10 20 00");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xaaaabbbb12345678ull},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_ST)
{
	setRegisters({
		{SYSZ_REG_1, 0x12345678},
		{SYSZ_REG_2, 0x1000},
	});

	emulate_bin("50 10 20 00");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LG)
{
	setRegisters({
		{SYSZ_REG_1, 0},
		{SYSZ_REG_2, 0x1000},
	});
	setMemory({
		{0x1000, 0x0123456789abcdefull},
	});

	emulate_bin("e3 10 20 00 00 04");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x0123456789abcdefull},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_STG)
{
	setRegisters({
		{SYSZ_REG_1, 0x0123456789abcdefull},
		{SYSZ_REG_2, 0x1000},
	});

	emulate_bin("e3 10 20 00 00 24");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x0123456789abcdef_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LA)
{
	setRegisters({
		{SYSZ_REG_1, 0xffff00000000ull},
		{SYSZ_REG_2, 0x1000},
	});

	emulate_bin("41 10 20 08");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1, SYSZ_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xffff00001008ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_BR)
{
	setRegisters({
		{SYSZ_REG_1, 0x2000},
	});

	emulate_bin("07 f1");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x2000}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_BRC)
{
	emulate_bin("a7 f4 00 10", 0x1000);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x1020}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_BRCL)
{
	emulate_bin("c0 f4 00 00 00 10", 0x1000);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x1020}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_BASR)
{
	setRegisters({
		{SYSZ_REG_1, 0x2000},
		{SYSZ_REG_14, 0},
	});

	emulate_bin("0d e1", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_14, 0x1002},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x2000}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_BRASL)
{
	emulate_bin("c0 e5 00 00 00 10", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_14, 0x1006},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1020}},
	});
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, translatorIs64Bit)
{
	EXPECT_EQ(8u, _translator->getArchByteSize());
	EXPECT_EQ(64u, _translator->getArchBitSize());
	EXPECT_EQ(CS_ARCH_SYSZ, _translator->getArchitecture());
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
