/**
 * @file tests/capstone2llvmir/mips_tests.cpp
 * @brief Capstone2LlvmIrTranslatorMips unit tests.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <llvm/IR/InstIterator.h>

#include <cstring>
#include <cstdint>
#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/mips/mips.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorMipsTests :
		public Capstone2LlvmIrTranslatorTests,
		public ::testing::WithParamInterface<cs_mode>
{
	protected:
		virtual void initKeystoneEngine() override
		{
			ks_mode mode = KS_MODE_MIPS32;
			switch(GetParam())
			{
				case CS_MODE_MIPS32: mode = KS_MODE_MIPS32; break;
				case CS_MODE_MIPS64: mode = KS_MODE_MIPS64; break;
				case CS_MODE_MIPS3: mode = KS_MODE_MIPS3; break;
				case CS_MODE_MIPS32R6: mode = KS_MODE_MIPS32R6; break;
				default: throw std::runtime_error("ERROR: unknown mode.\n");
			}
			if (ks_open(KS_ARCH_MIPS, mode, &_assembler) != KS_ERR_OK)
			{
				throw std::runtime_error("ERROR: failed on ks_open().\n");
			}
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			switch(GetParam())
			{
				case CS_MODE_MIPS32:
					_translator = Capstone2LlvmIrTranslator::createMips32(&_module);
					break;
				case CS_MODE_MIPS64:
					_translator = Capstone2LlvmIrTranslator::createMips64(&_module);
					break;
				case CS_MODE_MIPS3:
					_translator = Capstone2LlvmIrTranslator::createMips3(&_module);
					break;
				case CS_MODE_MIPS32R6:
					_translator = Capstone2LlvmIrTranslator::createMips32R6(&_module);
					break;
				default:
					throw std::runtime_error("ERROR: unknown mode.\n");
			}
		}

		// An integer result of trunc/round/ceil/floor lives in an FP
		// register as a bit pattern, and comparing it as a float is useless:
		// `bitcast i32 3 to float` is 4.2e-45, which EXPECT_NEAR(0.001) cannot
		// tell from zero or from 4. These read the bits back.
		uint32_t fpBits32(uint32_t reg)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			float f = _emulator->getGlobalVariableValue(gv).FloatVal;
			uint32_t b = 0;
			std::memcpy(&b, &f, sizeof(b));
			return b;
		}

		uint64_t fpBits64(uint32_t reg)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			double d = _emulator->getGlobalVariableValue(gv).DoubleVal;
			uint64_t b = 0;
			std::memcpy(&b, &d, sizeof(b));
			return b;
		}

		// These can/should be used at the beginning of each test case to
		// determine which modes should the case be run for.
		// They are macros because we want them to cause return in the current
		// function (test case).
		//
		protected:
#define ALL_MODES
#define ONLY_MODE_32 if (GetParam() != CS_MODE_MIPS32) return;
#define ONLY_MODE_64 if (GetParam() != CS_MODE_MIPS64) return;
#define ONLY_MODE_3 if (GetParam() != CS_MODE_MIPS3) return;
#define ONLY_MODE_32R6 if (GetParam() != CS_MODE_MIPS32R6) return;
#define SKIP_MODE_32 if (GetParam() == CS_MODE_MIPS32) return;
#define SKIP_MODE_64 if (GetParam() == CS_MODE_MIPS64) return;
#define SKIP_MODE_3 if (GetParam() == CS_MODE_MIPS3) return;
#define SKIP_MODE_32R6 if (GetParam() == CS_MODE_MIPS32R6) return;
};

struct PrintCapstoneModeToString_Mips
{
	template <class ParamType>
	std::string operator()(const TestParamInfo<ParamType>& info) const
	{
		switch (info.param)
		{
			case CS_MODE_16: return "CS_MODE_16";
			case CS_MODE_MIPS32: return "CS_MODE_MIPS32";
			case CS_MODE_MIPS64: return "CS_MODE_MIPS64";
			case CS_MODE_MICRO: return "CS_MODE_MICRO";
			case CS_MODE_MIPS3: return "CS_MODE_MIPS3";
			case CS_MODE_MIPS32R6: return "CS_MODE_MIPS32R6";
			default: return "UNHANDLED CS_MODE";
		}
	}
};

// By default, all the test cases are run with all the modes.
// If some test case is not meant for all modes, use some of the ONLY_MODE_*,
// SKIP_MODE_* macros.
//
INSTANTIATE_TEST_SUITE_P(
		InstantiateMipsWithAllModes,
		Capstone2LlvmIrTranslatorMipsTests,
// TODO: Try to add CS_MODE_MIPS3 and CS_MODE_MIPS32R6. But Keystone is failing
// with these as basic mode. Maybe Capstone does also? Capstone tutorial says
// CS_MODE_MIPS32R6 is MIPS basic mode, and does not say anything about
// CS_MODE_MIPS3. Maybe these two are not basic modes and we should not use
// them. Explore, but this is not critical at the moment.
		::testing::Values(CS_MODE_MIPS32, CS_MODE_MIPS64),
		PrintCapstoneModeToString_Mips());

//
// MIPS_INS_ADDIU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDIU_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x5678},
	});

	emulate("addiu $1, $2, 0x1000");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x6678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDIU_3_op_bin)
{
	ONLY_MODE_32;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x5678},
	});

	emulate_bin("00 10 41 24");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x6678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDIU_2_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("addiu $1, 0x1000");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x2234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDIU_2_op_sub)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("addiu $1, -0x1000");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDIU_3_op_zero_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x5678},
	});

	emulate("addiu $1, $0, 0x1234");

	EXPECT_JUST_REGISTERS_LOADED({});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDIU_3_op_zero_dst)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x5678},
	});

	emulate("addiu $0, $1, 0x1234");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_ADDI
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDI_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x5678},
	});

	emulate("addi $1, $2, 0x1000");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x6678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_ADD
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADD_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1000},
		{MIPS_REG_2, 0x5678},
	});

	emulate("add $1, $2, $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x6678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADD_3_zero_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1000},
	});

	emulate("add $1, $0, $0");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADD_3_op_all_zero)
{
	ALL_MODES;

	emulate("add $0, $0, $0");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_ADDU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDU_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1000},
		{MIPS_REG_2, 0x5678},
	});

	emulate("addu $1, $2, $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x6678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SUB
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUB_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x22222222},
		{MIPS_REG_2, 0x11111111},
	});

	emulate("sub $1, $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x11111111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUB_2_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x22222222},
		{MIPS_REG_2, 0x11111111},
	});

	emulate("sub $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x11111111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUB_3_op_op2_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x22222222},
		{MIPS_REG_2, 0x11111111},
	});

	emulate("sub $1, $2, $0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x11111111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SUBU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUBU_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x22222222},
		{MIPS_REG_2, 0x11111111},
	});

	emulate("subu $1, $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x11111111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_AND
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_AND_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0xffffffff},
		{MIPS_REG_2, 0xf0f0f0f0},
	});

	emulate("and $1, $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xf0f0f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_ANDI
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ANDI_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
	});

	emulate("andi $1, $2, 0xff00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x00005600},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_OR
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_OR_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0xffff0000},
		{MIPS_REG_2, 0x00001234},
	});

	emulate("or $1, $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffff1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_ORI
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ORI_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
	});

	emulate("ori $1, $2, 0xffff");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x1234ffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_XOR
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_XOR_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0xffff0000},
		{MIPS_REG_2, 0x00001234},
	});

	emulate("xor $1, $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffff1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_XORI
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_XORI_3_op)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1234ffff},
	});

	emulate("xori $1, $2, 0xffff");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12340000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_NOR
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NOR_3_op)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0xfff00000},
		{MIPS_REG_2, 0x00000fff},
	});

	emulate("nor $1, $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x000ff000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_NOT
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NOT)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0xff00ff00},
	});

	emulate("not $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x00ff00ff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MUL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MUL)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x01234567},
		{MIPS_REG_3, 0x89abcdef},
	});

	emulate("mul $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xc94e4629},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MULT
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MULT)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x01234567},
		{MIPS_REG_3, 0x89abcdef},
	});

	emulate("mult $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 0xff795e36},
		{MIPS_REG_LO, 0xc94e4629},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MULTU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MULTU)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x01234567},
		{MIPS_REG_3, 0x89abcdef},
	});

	emulate("multu $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 0x009ca39d},
		{MIPS_REG_LO, 0xc94e4629},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_DIV
//

// TODO: For some reason, Keystone translates "div $1, $2" into 10 instructions.
//
//TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIV)
//{
//	SKIP_MODE_64;
//
//	setRegisters({
//		{MIPS_REG_2, 105},
//		{MIPS_REG_3, 10},
//	});
//
//	emulate("div $2, $3");
//
//	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
//	EXPECT_JUST_REGISTERS_STORED({
//		{MIPS_REG_HI, 5},
//		{MIPS_REG_LO, 10},
//	});
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_NO_VALUE_CALLED();
//}

//
// MIPS_INS_SLL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLL)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
	});

	emulate("sll $1, $2, 0x8");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x34567800},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SLLV
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLLV)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
		{MIPS_REG_3, 0x8},
	});

	emulate("sllv $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x34567800},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SRL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SRL)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
	});

	emulate("srl $1, $2, 0x8");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x00123456},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SRLV
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SRLV)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
		{MIPS_REG_3, 0x8},
	});

	emulate("srlv $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x00123456},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SRA
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SRA_no_sign)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x00ff0000},
	});

	emulate("sra $1, $2, 20");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x0000000f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SRA_sign)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xf0ff0000},
	});

	emulate("sra $1, $2, 20");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffffff0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SRAV
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SRAV_no_sign)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x00ff0000},
		{MIPS_REG_3, 20},
	});

	emulate("srav $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x0000000f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SYSCALL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SYSCALL_no_op)
{
	ALL_MODES;

	emulate("syscall");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_syscall"), {0x0}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SYSCALL_imm_op)
{
	ALL_MODES;

	emulate("syscall 0x20");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_syscall"), {0x20}},
	});
}

//
// MIPS_INS_BREAK
//

//TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BREAK_no_op)
//{
//	ALL_MODES;
//
//	emulate("break");
//
//	EXPECT_NO_REGISTERS_LOADED();
//	EXPECT_NO_REGISTERS_STORED();
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_module.getFunction("__asm_break"), {0x0}},
//	});
//}
//
//TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BREAK_imm_op)
//{
//	ALL_MODES;
//
//	emulate("break 0x20");
//
//	EXPECT_NO_REGISTERS_LOADED();
//	EXPECT_NO_REGISTERS_STORED();
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_module.getFunction("__asm_break"), {0x20}},
//	});
//}
//
//TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BREAK_bin_imm_op)
//{
//	ALL_MODES;
//
//	emulate("break 0, 7");
//
//	EXPECT_NO_REGISTERS_LOADED();
//	EXPECT_NO_REGISTERS_STORED();
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_module.getFunction("__asm_break_bin"), {0, 7}},
//	});
//}

//
// MIPS_INS_SLT
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLT_true_postitive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x10},
		{MIPS_REG_3, 0x20},
	});

	emulate("slt $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLT_true_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, -0x1234},
		{MIPS_REG_3, 0x0},
	});

	emulate("slt $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLT_false_positive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1234},
		{MIPS_REG_3, 0x100},
	});

	emulate("slt $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLT_false_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, -0x100},
		{MIPS_REG_3, -0x200},
	});

	emulate("slt $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLT_false_eq)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x100},
		{MIPS_REG_3, 0x100},
	});

	emulate("slt $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SLTI
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLTI_true_postitive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x10},
	});

	emulate("slti $1, $2, 0x20");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLTI_true_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, -0x1234},
	});

	emulate("slti $1, $2, 0x0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLTI_false_positive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1234},
	});

	emulate("slti $1, $2, 0x100");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLTI_false_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, -0x100},
	});

	emulate("slti $1, $2, -0x200");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLTI_false_eq)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x100},
	});

	emulate("slti $1, $2, 0x100");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LUI
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LUI)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
	});

	emulate("lui $1, 0xabcd");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xabcd0000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MOVZ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVZ_false)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0xabcd},
		{MIPS_REG_3, 0x1234},
	});

	emulate("movz $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVZ_true)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0xabcd},
	});

	emulate("movz $1, $2, $0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xabcd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVZ_true_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 2.71_f32},
		{MIPS_REG_3, 0x1234},
	});

	emulate("movz.s $f0, $f2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVZ_false_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f64},
		{MIPS_REG_F2, 2.71_f64},
	});

	emulate("movz.d $f0, $f2, $0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 2.71_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MOVN
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVN_true)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0xabcd},
		{MIPS_REG_3, 0x1234},
	});

	emulate("movn $1, $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xabcd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVN_false)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0xabcd},
	});

	emulate("movn $1, $2, $0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVN_true_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 2.71_f32},
		{MIPS_REG_3, 0x1234},
	});

	emulate("movn.s $f0, $f2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 2.71_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVN_false_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f64},
		{MIPS_REG_F2, 2.71_f64},
	});

	emulate("movn.d $f0, $f2, $0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MOVF
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVF_true)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
		{MIPS_REG_4, 0xabcd},
		{MIPS_REG_FCC0, false},
	});

	emulate("movf $2, $4, $fcc0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_4, MIPS_REG_FCC0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xabcd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVF_false)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
		{MIPS_REG_4, 0xabcd},
		{MIPS_REG_FCC0, true},
	});

	emulate("movf $2, $4, $fcc0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_4, MIPS_REG_FCC0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVF_true_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32},
		{MIPS_REG_F4, 2.71_f32},
		{MIPS_REG_FCC0, false},
	});

	emulate("movf.s $f2, $f4, $fcc0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_FCC0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F2, 2.71_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVF_false_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
		{MIPS_REG_F4, 2.71_f64},
		{MIPS_REG_FCC0, true},
	});

	emulate("movf.d $f2, $f4, $fcc0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_FCC0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F2, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MOVT
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVT_true)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
		{MIPS_REG_4, 0xabcd},
		{MIPS_REG_FCC0, true},
	});

	emulate("movt $2, $4, $fcc0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_4, MIPS_REG_FCC0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xabcd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVT_false)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x12345678},
		{MIPS_REG_4, 0xabcd},
		{MIPS_REG_FCC0, false},
	});

	emulate("movt $2, $4, $fcc0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_4, MIPS_REG_FCC0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVT_true_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32},
		{MIPS_REG_F4, 2.71_f32},
		{MIPS_REG_FCC0, true},
	});

	emulate("movt.s $f2, $f4, $fcc0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_FCC0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F2, 2.71_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVT_false_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
		{MIPS_REG_F4, 2.71_f64},
		{MIPS_REG_FCC0, false},
	});

	emulate("movt.d $f2, $f4, $fcc0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_FCC0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F2, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_CLO
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CLO_zeroes)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x0fffffff},
	});

	emulate("clo $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CLO_ones)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xffff0000},
	});

	emulate("clo $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 16},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_CLZ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CLZ_zeroes)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x0000ffff},
	});

	emulate("clz $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 16},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CLZ_ones)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xf0000000},
	});

	emulate("clz $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_WSBH
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_SEB
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_SEH
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_SNE, MIPS_INS_SNEI
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_SNE, MIPS_INS_SNEI
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_SEQ, MIPS_INS_SEQI
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_EXT
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_INS
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_MFLO
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MFLO)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_LO, 0x12345678},
	});

	emulate("mflo $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_LO});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MFHI)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_HI, 0x12345678},
	});

	emulate("mfhi $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_HI});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MTLO
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MTLO)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
	});

	emulate("mtlo $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_LO, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MTHI)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
	});

	emulate("mthi $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MAX
// TODO: Keystone -- Invalid mnemonic (KS_ERR_ASM_MNEMONICFAIL).
//

//
// MIPS_INS_MIN
// TODO: Keystone -- Invalid mnemonic (KS_ERR_ASM_MNEMONICFAIL).
//

//
// MIPS_INS_MADD
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MADD)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x123456},
		{MIPS_REG_2, 0x56789a},
		{MIPS_REG_HI, 0x1234},
		{MIPS_REG_LO, 0x56789abc},
	});

	emulate("madd $1, $2"); // 12 34 56 78 9a bc + 06 26 28 5f cb bc = 00 00 18 5a 7e d8 66 78

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2, MIPS_REG_HI, MIPS_REG_LO});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 0x0000185a},
		{MIPS_REG_LO, 0x7ed86678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MADDU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MADDU)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x123456},
		{MIPS_REG_2, 0x56789a},
		{MIPS_REG_HI, 0x1234},
		{MIPS_REG_LO, 0x56789abc},
	});

	emulate("maddu $1, $2"); // 12 34 56 78 9a bc + 06 26 28 5f cb bc = 00 00 18 5a 7e d8 66 78

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2, MIPS_REG_HI, MIPS_REG_LO});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 0x0000185a},
		{MIPS_REG_LO, 0x7ed86678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MSUB
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MSUB)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x123456},
		{MIPS_REG_2, 0x56789a},
		{MIPS_REG_HI, 0x1234},
		{MIPS_REG_LO, 0x56789abc},
	});

	emulate("msub $1, $2"); // 12 34 56 78 9a bc - 06 26 28 5f cb bc = 00 00 0c 0e 2e 18 cf 00

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2, MIPS_REG_HI, MIPS_REG_LO});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 0x00000c0e},
		{MIPS_REG_LO, 0x2e18cf00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MSUBU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MSUBU)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x123456},
		{MIPS_REG_2, 0x56789a},
		{MIPS_REG_HI, 0x1234},
		{MIPS_REG_LO, 0x56789abc},
	});

	emulate("msubu $1, $2"); // 12 34 56 78 9a bc - 06 26 28 5f cb bc = 00 00 0c 0e 2e 18 cf 00

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2, MIPS_REG_HI, MIPS_REG_LO});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 0x00000c0e},
		{MIPS_REG_LO, 0x2e18cf00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_ROTR
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_ROTRV
// TODO: Keystone -- instruction requires a CPU feature not currently enabled.
//

//
// MIPS_INS_J
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_J)
{
	ALL_MODES;

	emulate("j 0x4005dc", 0x4006f8);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x4005dc}},
	});
}

//
// MIPS_INS_JR
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_JR)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x4005dc},
	});

	emulate("jr $1", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x4005dc}},
	});
}

//
// MIPS_INS_B
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_B)
{
	ALL_MODES;

	emulate("j 0x1000", 0x1000);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x1000}},
	});
}

//
// MIPS_INS_JAL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_JAL)
{
	ALL_MODES;

	emulate("jal 0x1008", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x1000 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1008}},
	});
}

//
// MIPS_INS_BAL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BAL)
{
	ALL_MODES;

	emulate("bal 0x1008", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x1000 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1008}},
	});
}

//
// MIPS_INS_JALR
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_JALR)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x4005dc},
	});

	emulate("jalr $1", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x4006f8 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x4005dc}},
	});
}

//
// MIPS_INS_LB
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LB)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x12_b},
	});

	emulate("lb $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LB_sext)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xff_b},
	});

	emulate("lb $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffffffff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LB_sext_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xff_b},
	});

	emulate("lb $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffffffffffffffff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LBU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LBU)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x12_b},
	});

	emulate("lbu $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LBU_zext)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xff_b},
	});

	emulate("lbu $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LH
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LH)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x1234_w},
	});

	emulate("lh $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x1234},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LH_sext)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xffff_w},
	});

	emulate("lh $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffffffff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LH_sext_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xffff_w},
	});

	emulate("lh $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffffffffffffffff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LBU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LHU)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x1234_w},
	});

	emulate("lhu $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x1234},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LHU_zext)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xffff_w},
	});

	emulate("lhu $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LW
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LW)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("lw $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, LlLoadIsAtomic)
{
	ALL_MODES;
	auto* f = translate(assemble("ll $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			if (l->isAtomic())
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, LlLoadAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("ll $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			if (l->isAtomic() && l->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, LldLoadAttachesPointeeMetadata)
{
	ONLY_MODE_64;
	auto* f = translate(assemble("lld $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			if (l->isAtomic() && l->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LL)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ll $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, LwLoadIsNotAtomic)
{
	ALL_MODES;
	auto* f = translate(assemble("lw $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			EXPECT_FALSE(l->isAtomic());
		}
	}
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MemoryLoadAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("lw $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			if (l->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MemoryStoreAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("sw $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LW_sext)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xffff0000_dw},
	});

	emulate("lw $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffff0000},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LW_sext_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xffff0000_dw},
	});

	emulate("lw $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffffffffffff0000},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LWC1
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LWC1_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 3.14_f32},
	});

	emulate("lwc1 $f0, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f32},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LWC1_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 3.0_f32},
	});

	emulate("lwc1 $f0, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.0_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LDC1
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LDC1_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 3.14_f64},
	});

	emulate("ldc1 $f0, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 3.14_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LDC1_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 3.14_f64},
	});

	emulate("ldc1 $f0, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LWU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LWU)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x1234_dw},
	});

	emulate("lwu $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x1234},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LWU_zext)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0xffff0000_dw},
	});

	emulate("lwu $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffff0000},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LD
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LD)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x0123456789abcdef_qw},
	});

	// This gets translated to ldc3 somewhere along the way (Keystone/Capstone?).
	emulate("ld $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x0123456789abcdef},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LDC3
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LDC3)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x1000},
	});
	setMemory({
		{0x1008, 0x0123456789abcdef_qw},
	});

	emulate("ldc3 $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x0123456789abcdef},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SB
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SB)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0x1000},
	});

	emulate("sb $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x78_b}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SH
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SH)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0x1000},
	});

	emulate("sh $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x5678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SW
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SW)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0x1000},
	});

	emulate("sw $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x12345678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, ScStoreIsAtomic)
{
	ALL_MODES;
	auto* f = translate(assemble("sc $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->isAtomic())
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, ScAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("sc $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->isAtomic() && s->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, ScdAttachesPointeeMetadata)
{
	ONLY_MODE_64;
	auto* f = translate(assemble("scd $1, 0x8($2)"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->isAtomic() && s->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SC)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
		{MIPS_REG_2, 0x1000},
	});

	emulate("sc $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 1},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x12345678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SD
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SD)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x0123456789abcdef},
		{MIPS_REG_2, 0x1000},
	});

	// This gets translated to sdc3 somewhere along the way (Keystone/Capstone?).
	emulate("sd $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x0123456789abcdef_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SDC3
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SDC3)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x0123456789abcdef},
		{MIPS_REG_2, 0x1000},
	});

	emulate("sdc3 $1, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x0123456789abcdef_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SWC1
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SWC1_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_2, 0x1000},
	});

	emulate("swc1 $f0, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 3.14_f32}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SWC1_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f64},
		{MIPS_REG_2, 0x1000},
	});

	emulate("swc1 $f0, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 3.14_f32}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SDC1
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SDC1_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_2, 0x1000},
	});

	emulate("sdc1 $f0, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 3.14_f64}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SDC1_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f64},
		{MIPS_REG_2, 0x1000},
	});

	emulate("sdc1 $f0, 0x8($2)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 3.14_f64}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_BGEZALL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZALL_call_on_positive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bgezall $1, 0x4005dc", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x4006f8 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x4005dc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZALL_call_on_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bgezall $1, 0x4005dc", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x4006f8 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x4005dc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZALL_no_call_on_negative_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0xff000000},
	});

	emulate("bgezall $1, 0x4005dc", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZALL_no_call_on_negative_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0xff00000000000000},
	});

	emulate("bgezall $1, 0x4005dc", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_BGEZAL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZAL_call_on_positive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bgezal $1, 0x700", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x4006f8 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x400700}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZAL_call_on_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bgezal $1, 0x700", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x4006f8 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x400700}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZAL_no_call_on_negative_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0xff000000},
	});

	emulate("bgezal $1, 0x700", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZAL_no_call_on_negative_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0xff00000000000000},
	});

	emulate("bgezal $1, 0x700", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_BLTZALL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZALL_call_on_negative_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0xff000000},
	});

	emulate("bltzall $1, 0x4005dc", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x4006f8 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x4005dc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZALL_call_on_negative_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0xff00000000000000},
	});

	emulate("bltzall $1, 0x4005dc", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_RA, 0x4006f8 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x4005dc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZALL_no_call_on_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bltzall $1, 0x4005dc", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZALL_no_call_on_positive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bltzall $1, 0x4005dc", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_BEQ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BEQ_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x1234},
	});

	emulate("beq $1, $2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BEQ_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x5678},
	});

	emulate("beq $1, $2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BEQL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BEQL_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x1234},
	});

	emulate("beql $1, $2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BEQL_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x5678},
	});

	emulate("beql $1, $2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BNE
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BNE_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x5678},
	});

	emulate("bne $1, $2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BNE_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x1234},
	});

	emulate("bne $1, $2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BNEL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BNEL_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x5678},
	});

	emulate("bnel $1, $2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BNEL_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x1234},
	});

	emulate("bnel $1, $2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BLEZ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLEZ_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("blez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLEZ_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("blez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLEZ_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("blez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BLEZL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLEZL_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("blezl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLEZL_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("blezl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLEZL_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("blezl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BGTZ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGTZ_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bgtz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGTZ_no_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bgtz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGTZ_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("bgtz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BGTZL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGTZL_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bgtzl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGTZL_no_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bgtzl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGTZL_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("bgtzl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BLTZ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZ_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("bltz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZ_no_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bltz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZ_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bltz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BLTZL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZL_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("bltzl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZL_no_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bltzl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BLTZL_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bltzl $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BGEZ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZ_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bgez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZ_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bgez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZ_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("bgez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BGEZL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZL_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bgez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZL_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bgez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BGEZL_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("bgez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BEQZ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BEQZ_no_branch_positive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("beqz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BEQZ_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("beqz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BEQZ_no_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("beqz $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// MIPS_INS_BNEZ
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BNEZ_branch_positive)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("bnez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BNEZ_no_branch_zero)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x0},
	});

	emulate("bnez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BNEZ_branch_negative)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, -10},
	});

	emulate("bnez $1, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

//
// MIPS_INS_MOV
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOV_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("mov.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOV_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("mov.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOV_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("mov.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOV_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("mov.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MOVE
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MOVE)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_4, 0x1234},
	});

	emulate("move $2, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// fp MIPS_INS_ADD
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADD_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32},
		{MIPS_REG_F4, 3.14_f32},
	});

	emulate("add.s $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 6.28_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADD_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 3.14_f64},
		{MIPS_REG_FD4, 3.14_f64},
	});

	emulate("add.d $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2, MIPS_REG_FD4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 6.28_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADD_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
		{MIPS_REG_F4, 3.14_f64},
	});

	emulate("add.s $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 6.28_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADD_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
		{MIPS_REG_F4, 3.14_f64},
	});

	emulate("add.d $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 6.28_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// fp MIPS_INS_SUB
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUB_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 6.28_f32},
		{MIPS_REG_F4, 3.14_f32},
	});

	emulate("sub.s $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUB_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 6.28_f64},
		{MIPS_REG_FD4, 3.14_f64},
	});

	emulate("sub.d $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2, MIPS_REG_FD4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUB_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 6.28_f64},
		{MIPS_REG_F4, 3.14_f64},
	});

	emulate("sub.s $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUB_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 6.28_f64},
		{MIPS_REG_F4, 3.14_f64},
	});

	emulate("sub.d $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// fp MIPS_INS_MUL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MUL_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 5.2_f32},
		{MIPS_REG_F4, 3.0_f32},
	});

	emulate("mul.s $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 15.6_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MUL_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 5.2_f64},
		{MIPS_REG_FD4, 3.0_f64},
	});

	emulate("mul.d $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2, MIPS_REG_FD4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 15.6_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MUL_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 5.2_f64},
		{MIPS_REG_F4, 3.0_f64},
	});

	emulate("mul.s $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 15.6_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MUL_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 5.2_f64},
		{MIPS_REG_F4, 3.0_f64},
	});

	emulate("mul.d $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 15.6_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// fp MIPS_INS_DIV
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIV_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 15.6_f32},
		{MIPS_REG_F4, 3.0_f32},
	});

	emulate("div.s $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 5.2_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIV_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 15.6_f64},
		{MIPS_REG_FD4, 3.0_f64},
	});

	emulate("div.d $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2, MIPS_REG_FD4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 5.2_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIV_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 15.6_f64},
		{MIPS_REG_F4, 3.0_f64},
	});

	emulate("div.s $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 5.2_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIV_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 15.6_f64},
		{MIPS_REG_F4, 3.0_f64},
	});

	emulate("div.d $f0, $f2, $f4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 5.2_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// fp MIPS_INS_MADD
// TODO: 32-bit variants: error: instruction requires a CPU feature not currently enabled
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MADD_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 1.1_f64},
		{MIPS_REG_F4, 2.2_f64},
		{MIPS_REG_F6, 3.3_f64},
	});

	emulate("madd.s $f0, $f2, $f4, $f6");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_F6});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 8.36_f64}, // 2.2 * 3.3 + 1.1
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MADD_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 1.1_f64},
		{MIPS_REG_F4, 2.2_f64},
		{MIPS_REG_F6, 3.3_f64},
	});

	emulate("madd.d $f0, $f2, $f4, $f6");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_F6});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 8.36_f64}, // 2.2 * 3.3 + 1.1
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// fp MIPS_INS_NMADD
// TODO: 32-bit variants: error: instruction requires a CPU feature not currently enabled
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NMADD_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 1.1_f64},
		{MIPS_REG_F4, 2.2_f64},
		{MIPS_REG_F6, 3.3_f64},
	});

	emulate("nmadd.s $f0, $f2, $f4, $f6");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_F6});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, double(-8.36)}, // -(2.2 * 3.3 + 1.1)
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NMADD_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 1.1_f64},
		{MIPS_REG_F4, 2.2_f64},
		{MIPS_REG_F6, 3.3_f64},
	});

	emulate("nmadd.d $f0, $f2, $f4, $f6");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_F6});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, double(-8.36)}, // -(2.2 * 3.3 + 1.1)
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// fp MIPS_INS_MSUB
// TODO: 32-bit variants: error: instruction requires a CPU feature not currently enabled
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MSUB_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 1.1_f64},
		{MIPS_REG_F4, 2.2_f64},
		{MIPS_REG_F6, 3.3_f64},
	});

	emulate("msub.s $f0, $f2, $f4, $f6");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_F6});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 6.16_f64}, // 2.2 * 3.3 - 1.1
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MSUB_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 1.1_f64},
		{MIPS_REG_F4, 2.2_f64},
		{MIPS_REG_F6, 3.3_f64},
	});

	emulate("msub.d $f0, $f2, $f4, $f6");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_F6});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 6.16_f64}, // 2.2 * 3.3 - 1.1
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// fp MIPS_INS_NMSUB
// TODO: 32-bit variants: error: instruction requires a CPU feature not currently enabled
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NMSUB_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 1.1_f64},
		{MIPS_REG_F4, 2.2_f64},
		{MIPS_REG_F6, 3.3_f64},
	});

	emulate("nmsub.s $f0, $f2, $f4, $f6");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_F6});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, double(-6.16)}, // -(2.2 * 3.3 - 1.1)
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NMSUB_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 1.1_f64},
		{MIPS_REG_F4, 2.2_f64},
		{MIPS_REG_F6, 3.3_f64},
	});

	emulate("nmsub.d $f0, $f2, $f4, $f6");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2, MIPS_REG_F4, MIPS_REG_F6});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, double(-6.16)}, // -(2.2 * 3.3 - 1.1)
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_ROUND
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ROUND_w_s_32)
{
	SKIP_MODE_64;

	// To nearest EVEN, which is what MIPS specifies and what
	// llvm.roundeven does. llvm.round -- half away from zero -- answers 3.

	setRegisters({
		{MIPS_REG_F2, 2.5_f32},
	});

	emulate("round.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.roundeven.f32"), {2.5_f32}},
	});
	EXPECT_EQ(2u, fpBits32(MIPS_REG_F0));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ROUND_w_d_32)
{
	SKIP_MODE_64;

	// The negative half of the same question.

	setRegisters({
		{MIPS_REG_FD2, -2.5},
	});

	emulate("round.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.roundeven.f64"), {-2.5}},
	});
	// -2, not -3.
	EXPECT_EQ(0xfffffffeu, fpBits32(MIPS_REG_F0));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ROUND_w_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("round.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_round.w.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ROUND_w_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("round.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_round.w.d"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ROUND_l_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("round.l.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_round.l.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ROUND_l_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 2.5_f64},
	});

	emulate("round.l.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.roundeven.f64"), {2.5_f64}},
	});
	EXPECT_EQ(2ull, fpBits64(MIPS_REG_F0));
}

//
// MIPS_INS_ABS
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ABS_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("abs.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_abs.s"), {3.14_f32}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ABS_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("abs.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_abs.d"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ABS_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("abs.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_abs.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ABS_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("abs.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_abs.d"), {3.14_f64}},
	});
}

//
// MIPS_INS_NEGU
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NEGU)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x00ffffff},
	});

	emulate("negu $1, $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xff000001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_NEG
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NEG_int)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x00ffffff},
	});

	emulate("neg $1, $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xff000001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NEG_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("neg.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_neg.s"), {3.14_f32}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NEG_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("neg.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_neg.d"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NEG_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("neg.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_neg.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NEG_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("neg.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_neg.d"), {3.14_f64}},
	});
}

//
// MIPS_INS_SQRT
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SQRT_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("sqrt.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_sqrt.s"), {3.14_f32}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SQRT_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("sqrt.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_sqrt.d"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SQRT_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("sqrt.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_sqrt.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SQRT_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("sqrt.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_sqrt.d"), {3.14_f64}},
	});
}

//
// MIPS_INS_FLOOR
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_FLOOR_w_s_32)
{
	SKIP_MODE_64;

	// Down. Ceil answers 4.

	setRegisters({
		{MIPS_REG_F2, 3.9_f32},
	});

	emulate("floor.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.floor.f32"), {3.9_f32}},
	});
	EXPECT_EQ(3u, fpBits32(MIPS_REG_F0));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_FLOOR_w_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, -3.1},
	});

	emulate("floor.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.floor.f64"), {-3.1}},
	});
	// -4, not -3: down, away from zero.
	EXPECT_EQ(0xfffffffcu, fpBits32(MIPS_REG_F0));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_FLOOR_w_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("floor.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_floor.w.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_FLOOR_w_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("floor.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_floor.w.d"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_FLOOR_l_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("floor.l.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_floor.l.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_FLOOR_l_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.9_f64},
	});

	emulate("floor.l.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.floor.f64"), {3.9_f64}},
	});
	EXPECT_EQ(3ull, fpBits64(MIPS_REG_F0));
}

//
// MIPS_INS_CEIL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CEIL_w_s_32)
{
	SKIP_MODE_64;

	// Up. Truncation answers 3.

	setRegisters({
		{MIPS_REG_F2, 3.1_f32},
	});

	emulate("ceil.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.ceil.f32"), {3.1_f32}},
	});
	EXPECT_EQ(4u, fpBits32(MIPS_REG_F0));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CEIL_w_d_32)
{
	SKIP_MODE_64;

	// Up is toward zero for a negative, which is where ceil and floor
	// swap places.

	setRegisters({
		{MIPS_REG_FD2, -3.1},
	});

	emulate("ceil.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.ceil.f64"), {-3.1}},
	});
	// -3, not -4.
	EXPECT_EQ(0xfffffffdu, fpBits32(MIPS_REG_F0));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CEIL_w_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("ceil.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_ceil.w.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CEIL_w_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("ceil.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_ceil.w.d"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CEIL_l_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("ceil.l.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_ceil.l.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CEIL_l_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.1_f64},
	});

	emulate("ceil.l.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// An LLVM intrinsic, not a pseudo-asm call. tests/llvmir-emul
	// computes it and still records the call, so the assertion is
	// which function was called rather than that none was.
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.ceil.f64"), {3.1_f64}},
	});
	EXPECT_EQ(4ull, fpBits64(MIPS_REG_F0));
}

//
// MIPS_INS_TRUNC
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_w_s_32)
{
	SKIP_MODE_64;

	// Toward zero. A round-to-nearest reading answers 4.

	setRegisters({
		{MIPS_REG_F2, 3.9_f32},
	});

	emulate("trunc.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
	EXPECT_EQ(3u, fpBits32(MIPS_REG_F0));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_w_d_32)
{
	SKIP_MODE_64;

	// The destination of a `.w` form is the 32-bit register f0, NOT the
	// fd0 that loadRegister()/storeRegister() map every FP operand of a
	// double-format instruction to. Reading fd0 here would find nothing.
	// Toward zero on a negative: floor would answer -4.

	setRegisters({
		{MIPS_REG_FD2, -3.9},
	});

	emulate("trunc.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
	// -3 as a 32-bit two's complement pattern.
	EXPECT_EQ(0xfffffffdu, fpBits32(MIPS_REG_F0));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_w_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("trunc.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_trunc.w.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_w_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("trunc.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_trunc.w.d"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_l_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64},
	});

	emulate("trunc.l.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_trunc.l.s"), {3.14_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_l_d_64)
{
	ONLY_MODE_64;

	// The one 64-bit form whose source and destination widths both match
	// what MIPS64's register file provides.

	setRegisters({
		{MIPS_REG_F2, 3.9_f64},
	});

	emulate("trunc.l.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
	EXPECT_EQ(3ull, fpBits64(MIPS_REG_F0));
}

//
// MIPS_INS_MFC1
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MFC1_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
	});

	emulate("mfc1 $2, $f0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x4048f5c3}, // float to hex
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MFC1_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f64},
	});

	emulate("mfc1 $2, $f0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x51eb851f}, // double to hex = 0x40091eb8 | 51eb851f
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_MTC1
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MTC1_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0x4048f5c3},
	});

	emulate("mtc1 $2, $f0");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f32}, // hex to float
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_CFC1
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CFC1_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("cfc1 $2, $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_cfc1"), {0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CFC1_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x1234},
	});

	emulate("cfc1 $2, $1");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_cfc1"), {0x1234}},
	});
}

//
// MIPS_INS_CTC1
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CTC1)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x1234},
		{MIPS_REG_2, 0x5678},
	});

	emulate("ctc1 $1, $2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1, MIPS_REG_2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_ctc1"), {0x1234, 0x5678}},
	});
}

//
// MIPS_INS_BC1F
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1F_fcc0_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC0, false},
	});

	emulate("bc1f $fcc0, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1F_fcc0_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC0, true},
	});

	emulate("bc1f 0x2000", 0x1000); // $fcc0 implied

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1F_fcc2_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC2, false},
	});

	emulate("bc1f $fcc2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

//
// MIPS_INS_BC1FL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1FL_fcc0_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC0, false},
	});

	emulate("bc1fl $fcc0, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1FL_fcc0_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC0, true},
	});

	emulate("bc1fl 0x2000", 0x1000); // $fcc0 implied

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1FL_fcc2_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC2, false},
	});

	emulate("bc1fl $fcc2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

//
// MIPS_INS_BC1T
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1T_fcc0_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC0, true},
	});

	emulate("bc1t $fcc0, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1T_fcc0_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC0, false},
	});

	emulate("bc1t 0x2000", 0x1000); // $fcc0 implied

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1T_fcc2_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC2, true},
	});

	emulate("bc1t $fcc2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

//
// MIPS_INS_BC1TL
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1TL_fcc0_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC0, true},
	});

	emulate("bc1tl $fcc0, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1TL_fcc0_no_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC0, false},
	});

	emulate("bc1tl 0x2000", 0x1000); // $fcc0 implied

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_BC1TL_fcc2_branch)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_FCC2, true},
	});

	emulate("bc1tl $fcc2, 0x2000", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FCC2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

//
// MIPS_INS_CVT.S.fmt
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_s_d)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("cvt.s.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_s_w)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32}, // 3.14 -> 0x4048f5c3 -> 1078523331.0
	});

	emulate("cvt.s.w $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 1078523331.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_s_l)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64}, // 3.14 -> 0x40091eb851eb851f -> 4614253070214989087.0
	});

	emulate("cvt.s.l $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 4614253070214989087.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_CVT.D.fmt
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_d_s)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.0_f32},
	});

	emulate("cvt.d.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 3.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_d_w)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f32}, // 3.14 -> 0x4048f5c3 -> 1078523331.0
	});

	emulate("cvt.d.w $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 1078523331.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_d_l)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.14_f64}, // 3.14 -> 0x40091eb851eb851f -> 4614253070214989087.0
	});

	emulate("cvt.d.l $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 4614253070214989087.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_CVT.W.fmt
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_W_s)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.1415_f32}, // 3.1415 -> 3 -> 0x3 -> 4.2039e-45
	});

	emulate("cvt.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 4.2039e-45_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_W_d)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 3.1415_f32}, // 3.1415 -> 3 -> 0x3 -> 4.2039e-45
	});

	emulate("cvt.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 4.2039e-45_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_C
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_f_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.f.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_f_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.f.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_sf_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.sf.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_sf_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.sf.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_un_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.un.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_un_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.un.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ngle_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.ngle.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ngle_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.ngle.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_eq_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.eq.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_eq_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_FD2, 2.71_f64},
	});

	emulate("c.eq.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_seq_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.seq.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_seq_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_FD2, 2.71_f64},
	});

	emulate("c.seq.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ngl_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.ngl.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ngl_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_FD2, 2.71_f64},
	});

	emulate("c.ngl.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ueq_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.ueq.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ueq_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 3.14_f64},
		{MIPS_REG_FD2, 2.71_f64},
	});

	emulate("c.ueq.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_olt_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 2.71_f32},
	});

	emulate("c.olt.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_olt_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 2.71_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.olt.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_lt_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 2.71_f32},
	});

	emulate("c.lt.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_lt_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 2.71_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.lt.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_nge_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 2.71_f32},
	});

	emulate("c.nge.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_nge_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 2.71_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.nge.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ult_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 2.71_f32},
	});

	emulate("c.ult.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ult_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 2.71_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.ult.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ole_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.ole.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ole_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 2.71_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.ole.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_le_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.le.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_le_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 2.71_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.le.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ngt_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.ngt.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ngt_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 2.71_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.ngt.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ule_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F0, 3.14_f32},
		{MIPS_REG_F2, 3.14_f32},
	});

	emulate("c.ule.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F0, MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_C_ule_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD0, 2.71_f64},
		{MIPS_REG_FD2, 3.14_f64},
	});

	emulate("c.ule.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0, MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FCC0, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
//==============================================================================
// Issue unit tests.
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, issue_633)
{
	ONLY_MODE_32;

	setRegisters({
		{MIPS_REG_W31, 3.14_f64},
	});

	emulate_bin("c0 ff b7 79"); // ori.b $w31, $w31, 0xb7

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_W31});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_W31, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, SyncEmitsFence)
{
	ALL_MODES;
	auto* f = translate(assemble("sync"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* fence = dyn_cast<FenceInst>(&*it))
		{
			found = true;
			EXPECT_EQ(fence->getOrdering(), AtomicOrdering::SequentiallyConsistent);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, SynciEmitsFence)
{
	ALL_MODES;
	// synci 0($a0) — Keystone MIPS32 lacks R2 synci; encoding from binutils
	// (REGIMM / SYNCI, base=$a0, offset=0) → 0x049f0000.
	auto* f = translate({0x00, 0x00, 0x9f, 0x04});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* fence = dyn_cast<FenceInst>(&*it))
		{
			found = true;
			EXPECT_EQ(fence->getOrdering(), AtomicOrdering::SequentiallyConsistent);
			break;
		}
	}
	EXPECT_TRUE(found);
}

//
// MIPS_INS_MTHC1, MIPS_INS_MFHC1
//
// mtc1 and mfc1 move the low half of a 64-bit FPU register; these move the
// high half, and a compiler emits them in pairs to get a double in and out of
// the FPU without going through memory. MTHC1 was the only instruction COV-01
// found untranslated in MIPS floating-point programs.
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MTHC1)
{
	SKIP_MODE_64;

	// The low half must survive: this writes only bits 63..32. Starting from
	// 1.0 (0x3ff0000000000000) and writing 0x40090000 over the high half gives
	// 0x4009000000000000, which is 3.125. Getting it wrong by clobbering the
	// low half would still look plausible, so the low half is non-zero.
	double start;
	std::uint64_t startBits = 0x3ff0000012345678ULL;
	std::memcpy(&start, &startBits, sizeof(start));
	double expected;
	std::uint64_t expectedBits = 0x4009000012345678ULL;
	std::memcpy(&expected, &expectedBits, sizeof(expected));

	setRegisters({
		{MIPS_REG_4, 0x40090000},
		{MIPS_REG_FD0, start},
	});

	// Keystone 0.9.2 will not assemble mthc1 in its MIPS32 mode ("instruction
	// requires a CPU feature not currently enabled" -- it is MIPS32r2), so this
	// goes in as the encoding, checked against capstone 5.0.9 first: 0x44e40000
	// decodes as MIPS_INS_MTHC1 with operands $a0, $f0.
	emulate_bin("00 00 e4 44");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_4, MIPS_REG_FD0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, expected},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MFHC1)
{
	SKIP_MODE_64;

	double start;
	std::uint64_t startBits = 0x4009000012345678ULL;
	std::memcpy(&start, &startBits, sizeof(start));

	setRegisters({
		{MIPS_REG_FD0, start},
	});

	// 0x44640000; see MIPS_INS_MTHC1 above for why this is an encoding.
	emulate_bin("00 00 64 44");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD0});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_4, 0x40090000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_PREF
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_PREF_is_nothing)
{
	SKIP_MODE_64;

	// A prefetch hint touches no register and no memory and raises no
	// addressing exception, so there is nothing to translate -- the answer
	// MIPS_INS_NOP already gets. As a nullptr entry it came out as an
	// __asm_pref call.
	setRegisters({
		{MIPS_REG_4, 0x1000},
	});

	emulate("pref 0, 0($4)");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}


//
// MIPS_INS_EXT, MIPS_INS_INS, MIPS_INS_WSBH
//
// EXT had a translator that handled one idiom -- `ext rt, rs, 0, 31`, read as
// a floating-point absolute value -- and sent every other form to
// __asm_ext, which is 4,389 occurrences in the static corpus. INS (2,580) and
// WSBH (1,890) had no translation at all. PSEUDO-01 is what named them; COV-01
// scores all three covered, because the table has a function pointer for each.
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_EXT_general_field)
{
	// Assembled by hand rather than through emulate(): Keystone refuses EXT,
	// INS and WSBH in plain MIPS32 mode ("instruction requires a CPU feature
	// not currently enabled") because they are MIPS32R2, and this suite is
	// instantiated over CS_MODE_MIPS32 and CS_MODE_MIPS64 only -- so an
	// ONLY_MODE_32R6 guard would make the test body unreachable. Capstone
	// decodes them in MIPS32 mode without complaint.
	//
	// 0x7c623a00 = ext $2, $3, 8, 8, little-endian.
	ONLY_MODE_32;

	setRegisters({
		{MIPS_REG_3, 0x12345678},
	});

	emulate_bin("00 3a 62 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x56},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_INS_keeps_the_bits_outside_the_field)
{
	// The only one of the pair that reads its destination. Dropping that
	// answers 0xee00.
	//
	// 0x7c627a04 = ins $2, $3, 8, 8.
	ONLY_MODE_32;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x000000ee},
	});

	emulate_bin("04 7a 62 7c");

	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xaabbeedd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_WSBH_swaps_within_halfwords)
{
	// Not a 32-bit byte swap: that would answer 0x78563412. `wsbh` followed by
	// `rotr $2, $2, 16` is how MIPS spells one, which is why this instruction
	// turns up in every endian conversion in the corpus.
	//
	// 0x7c0310a0 = wsbh $2, $3.
	ONLY_MODE_32;

	setRegisters({
		{MIPS_REG_3, 0x12345678},
	});

	emulate_bin("a0 10 03 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x34127856},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
