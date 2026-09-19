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

#ifndef MIPS_REG_HI
#define MIPS_REG_HI MIPS_REG_HI0
#endif
#ifndef MIPS_REG_LO
#define MIPS_REG_LO MIPS_REG_LO0
#endif

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

		// lwl/lwr produce a 32-bit word, and MIPS64 sign-extends it into
		// the 64-bit register while MIPS32 does not. The same test body
		// therefore expects two different values, and writing them out by
		// hand in every case is how one of them ends up wrong.
		uint64_t sx32(uint32_t v)
		{
			return GetParam() == CS_MODE_MIPS64 ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)))
												: v;
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

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIV_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 105},
		{MIPS_REG_3, 10},
	});

	// Keystone expands "div $2, $3" into a multi-insn sequence. SPECIAL /
	// DIV rs=$2 rt=$3 → 0x0043001A, little-endian.
	emulate_bin("1a 00 43 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 5},
		{MIPS_REG_LO, 10},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIVU)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_2, 105},
		{MIPS_REG_3, 10},
	});

	// SPECIAL / DIVU rs=$2 rt=$3 → 0x0043001B.
	emulate_bin("1b 00 43 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 5},
		{MIPS_REG_LO, 10},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

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
	// SKIP_MODE_64 now: this test ran in MIPS64 too and asserted the 32-bit
	// answer there, which pinned the defect. 0xabcd has bit 15 set, so the
	// MIPS64 answer is 0xffffffffabcd0000 -- see MIPS_INS_LUI_64_sign_extends.
	SKIP_MODE_64;

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

// MIPS64 LUI sign-extends the 32-bit result into the register. The standard
// n64 constant idiom depends on it: `lui $2,0xffff; ori $2,$2,0x1234` is
// 0xffffffffffff1234 on the machine.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LUI_64_sign_extends)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
	});

	emulate("lui $1, 0xabcd");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0xffffffffabcd0000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// A positive immediate must NOT acquire high bits.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LUI_64_positive_stays_positive)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_1, 0x12345678},
	});

	emulate("lui $1, 0x7abc");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_1, 0x7abc0000},
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
		{MIPS_REG_F2, -3.14f},
	});

	emulate("abs.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f32"), {-3.14f}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ABS_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, -3.14},
	});

	emulate("abs.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f64"), {-3.14}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ABS_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, -3.14},
	});

	emulate("abs.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f64"), {-3.14}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ABS_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, -3.14},
	});

	emulate("abs.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 3.14_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f64"), {-3.14}},
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
		{MIPS_REG_F0, -3.14f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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
		{MIPS_REG_FD0, -3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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
		{MIPS_REG_F0, -3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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
		{MIPS_REG_F0, -3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_SQRT
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SQRT_s_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 16.0_f32},
	});

	emulate("sqrt.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 4.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.sqrt.f32"), {16.0_f32}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SQRT_d_32)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_FD2, 16.0_f64},
	});

	emulate("sqrt.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_FD0, 4.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.sqrt.f64"), {16.0_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SQRT_s_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 16.0_f64},
	});

	emulate("sqrt.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 4.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.sqrt.f64"), {16.0_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SQRT_d_64)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 16.0_f64},
	});

	emulate("sqrt.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, 4.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.sqrt.f64"), {16.0_f64}},
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

// CVT.W.fmt rounds with the current rounding mode, whose default is
// nearest-EVEN. The test below uses 3.1415, on which truncating and rounding
// agree -- which is why a bare fptosi survived here. 2.7, 2.5 and 3.5 separate
// them: truncation gives 2, 2 and 3.
/// The raw bits of a float register.
///
/// CVT.W.fmt leaves an INTEGER in a floating-point register, and the fixture's
/// comparison for a float register is EXPECT_NEAR(..., 0.001). The bit pattern
/// of a small integer is a denormal -- 3 is 4.2e-45 -- so every such
/// expectation compares equal to zero, to itself, and to every other small
/// integer. Those assertions cannot fail. This one can.
static uint32_t floatRegBits(float f)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &f, sizeof bits);
	return bits;
}

// MIPS does NOT saturate toward the nearer end. Its default result when the
// Invalid Operation exception is masked -- which is how ordinary code runs --
// is 2^(N-1) - 1 for every bad input alike: NaN, +infinity, AND a large
// negative. That is the opposite of Power for the low end, and the four
// architectures in this tree have four different rules here.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_W_out_of_range_high)
{
	SKIP_MODE_64;

	float v = 1.0e30f;
	setRegisters({
		{MIPS_REG_F2, v},
	});

	emulate("trunc.w.s $f0, $f2");

	EXPECT_EQ(0x7fffffffu, floatRegBits(getRegisterValueFloat(MIPS_REG_F0)));
}

// The one that separates MIPS from everyone else: a large NEGATIVE also gives
// the maximum, not the minimum.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_W_out_of_range_low_is_also_max)
{
	SKIP_MODE_64;

	float v = -1.0e30f;
	setRegisters({
		{MIPS_REG_F2, v},
	});

	emulate("trunc.w.s $f0, $f2");

	EXPECT_EQ(0x7fffffffu, floatRegBits(getRegisterValueFloat(MIPS_REG_F0)));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_TRUNC_W_in_range_still_converts)
{
	SKIP_MODE_64;

	float v = -3.7f;
	setRegisters({
		{MIPS_REG_F2, v},
	});

	emulate("trunc.w.s $f0, $f2");

	EXPECT_EQ(0xfffffffdu, floatRegBits(getRegisterValueFloat(MIPS_REG_F0)));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_W_s_rounds_to_nearest)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 2.7_f32},
	});

	emulate("cvt.w.s $f0, $f2");

	EXPECT_EQ(3u, floatRegBits(getRegisterValueFloat(MIPS_REG_F0)));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_W_s_ties_go_to_even)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 2.5_f32},
	});

	emulate("cvt.w.s $f0, $f2");

	// 2.5 -> 2, not 3: ties to even. Round-half-away would give 3 here and
	// agree with nearest-even on 3.5, so one of the two is not enough.
	EXPECT_EQ(2u, floatRegBits(getRegisterValueFloat(MIPS_REG_F0)));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_W_s_ties_go_to_even_upward)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.5_f32},
	});

	emulate("cvt.w.s $f0, $f2");

	EXPECT_EQ(4u, floatRegBits(getRegisterValueFloat(MIPS_REG_F0)));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_W_s)
{
	SKIP_MODE_64;

	setRegisters({
		{MIPS_REG_F2, 3.1415_f32}, // 3.1415 -> 3 -> 0x3 -> 4.2039e-45
	});

	emulate("cvt.w.s $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_EQ(3u, floatRegBits(getRegisterValueFloat(MIPS_REG_F0)));
	EXPECT_NO_MEMORY_LOADED_STORED();
	// CVT.W.fmt rounds; the conversion is no longer a bare fptosi. 3.1415
	// rounds and truncates to the same 3, which is why this test could not
	// tell the two apart -- see MIPS_INS_CVT_W_s_ties_go_to_even.
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.roundeven.f32"), {3.1415_f32}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CVT_W_d)
{
	SKIP_MODE_64;

	setRegisters({
		// _f64, not _f32: FD2 is a double register, and an _f32 literal put
		// the 32-bit pattern of 3.1415 there, which reads back as the double
		// 5.3e-315. The conversion then answered 0 -- and the expectation
		// below could not tell, because 0 and the bits of 3 are both denormals
		// well within the 0.001 the fixture compares floats to.
		{MIPS_REG_FD2, 3.1415_f64},
	});

	emulate("cvt.w.d $f0, $f2");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_FD2});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_F0, ANY},
	});
	EXPECT_EQ(3u, floatRegBits(getRegisterValueFloat(MIPS_REG_F0)));
	EXPECT_NO_MEMORY_LOADED_STORED();
	// CVT.W.fmt rounds; the conversion is no longer a bare fptosi. 3.1415
	// rounds and truncates to the same 3, which is why this test could not
	// tell the two apart -- see MIPS_INS_CVT_W_s_ties_go_to_even.
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.roundeven.f64"), {3.1415_f64}},
	});
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

	// Capstone 6 gives MSA its own ids: this is MIPS_INS_ORI_B, not scalar
	// ORI. Byte-immediate MSA is not modelled, so the honest answer is the
	// pseudo-assembly call every other unmodelled instruction gets.
	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_W31});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NE(nullptr, _module.getFunction("__asm_ori.b"));
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MSA_registers_are_a_hundred_and_twenty_eight_bits)
{
	ONLY_MODE_32;

	// initializeRegTypeMap() declared these with a local named `i128` that
	// was getInt64Ty, so every MSA vector register was half its width. The
	// same declaration block had `i1` as getInt32Ty, which made the nine DSP
	// condition and carry flags 32 bits each.
	ASSERT_NE(nullptr, getRegister(MIPS_REG_W0));
	EXPECT_EQ(128u, getRegister(MIPS_REG_W0)->getValueType()->getPrimitiveSizeInBits());
	EXPECT_EQ(128u, getRegister(MIPS_REG_W31)->getValueType()->getPrimitiveSizeInBits());
	ASSERT_NE(nullptr, getRegister(MIPS_REG_DSPCARRY));
	EXPECT_EQ(1u, getRegister(MIPS_REG_DSPCARRY)->getValueType()->getPrimitiveSizeInBits());
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


//
// MIPS_INS_RDHWR
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_RDHWR_29_is_the_thread_pointer_not_the_stack_pointer)
{
	// Capstone reports the hardware-register number as an ordinary GPR id, and
	// MIPS_REG_29 IS MIPS_REG_SP. A translation that loaded operand 1 would
	// read the stack pointer and look entirely plausible doing it -- so $sp is
	// set to something distinctive here, and the answer must not be it.
	//
	// There is no companion test for a different selector, because Capstone
	// 5.0.9 does not decode one: $0, $1, $2 and $3 all come back as
	// undecodable in MIPS32 mode, and only $29 produces an instruction. The
	// selector check in translateRdhwr() is therefore unexercised by this
	// suite, and saying so is better than implying otherwise with a test that
	// cannot reach it.
	//
	// 0x7c02e83b = rdhwr $2, $29, little-endian.
	ONLY_MODE_32;

	setRegisters({
		{MIPS_REG_SP, 0x7fff0000},
		{MIPS_REG_HWR_ULR, 0xdeadbeef},
	});

	emulate_bin("3b e8 02 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_HWR_ULR});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xdeadbeef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// MIPS_INS_LWL, MIPS_INS_LWR, MIPS_INS_SWL, MIPS_INS_SWR
//
// MIPS has no unaligned load, so a compiler that must read a word from an
// address it cannot prove aligned emits two instructions, each transferring
// the part of the word on one side of the containing aligned word's boundary.
// 12,442 occurrences in the static parity corpus across the four word forms:
// the largest specifiable group on any architecture outside x86's AVX.
//
// The memory word is 0x11223344 and the destination register 0xaabbccdd, so
// every byte of the answer says where it came from, and no two of the four
// alignments give the same result for any of the four instructions.
//
// These are the LITTLE-endian expectations; the big-endian fixture below
// carries its own, and the two sets are mirror images. That is the whole point
// of having both: the corpus is big-endian mips-linux-gnu and this fixture is
// little-endian, so an implementation that hard-codes either endianness passes
// one suite and fails the other.

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LWL)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("lwl $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, sx32(0x3344ccdd)},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// Offset 3 on a little-endian MIPS is the whole word: the left part reaches
// all the way down. Offset 0 is one byte. If the two were swapped this test
// and the one above would both still pass with either reading of `left`, which
// is why both alignments are here.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LWL_whole_word)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("lwl $2, 3($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, sx32(0x11223344)},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LWR)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("lwr $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, sx32(0xaa112233)},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// A partial store is a read-modify-write of the containing aligned word at
// this level of modelling -- the bytes it does not write have to survive. The
// old translation was translatePseudoAsmFncOp0Op1, which wrote no memory at
// all.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SWL)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("swl $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x1122aabb_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SWR)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("swr $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xbbccdd44_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

// The pair, which is the only form either instruction appears in. On a
// little-endian MIPS the idiom is `lwl rt, 3(A)` then `lwr rt, 0(A)`, and
// together they must reconstruct the unaligned word at A. Memory holds
// 44 33 22 11 88 77 66 55 from 0x1000, so the little-endian word at 0x1001 is
// 0x88112233. Neither halves' own test can catch a pair of errors that cancel;
// this one can.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, unaligned_word_load_pair)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_3, 0x1001},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
		{0x1004, 0x55667788_dw},
	});

	emulate("lwl $2, 3($3)\nlwr $2, 0($3)");

	EXPECT_EQ(sx32(0x88112233), getRegisterValueUnsigned(MIPS_REG_2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LDL)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd11223344},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x0102030405060708_qw},
	});

	emulate("ldl $2, 7($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x0102030405060708},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LDR)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd11223344},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x0102030405060708_qw},
	});

	emulate("ldr $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xaa01020304050607},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SDL)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd11223344},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x0102030405060708_qw},
	});

	emulate("sdl $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x010203040506aabb_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SDR)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd11223344},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x0102030405060708_qw},
	});

	emulate("sdr $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xbbccdd1122334408_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

// The doubleword forms are MIPS64 instructions. On a 32-bit MIPS they fall
// back rather than being answered at the wrong width, which is the same rule
// EXT, INS and WSBH follow.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_LDL_falls_back_on_mips32)
{
	ONLY_MODE_32;

	setRegisters({
		{MIPS_REG_3, 0x1000},
	});

	// 0x68620001 = ldl $2, 1($3). Keystone will not assemble a MIPS64
	// instruction in MIPS32 mode, which is the point.
	emulate_bin("01 00 62 68");

	EXPECT_NE(nullptr, _module.getFunction("__asm_ldl"));
}

//
// The MIPS64 doubleword instructions.
//
// Every one of these was `nullptr` in the dispatch table: MIPS_INS_DADD,
// DADDI, DADDU, DADDIU, DSUB, DSUBU, DMULT, DMULTU, DDIV, DDIVU, DSLL, DSRL,
// DSRA, DSLLV, DSRLV, DSRAV, DSLL32, DSRL32, DSRA32, DROTR, DROTRV, DROTR32,
// DCLZ, DCLO, DEXT, DEXTM, DEXTU, DINS, DINSM, DINSU, DSBH and DSHD -- the
// whole 64-bit arithmetic, shift and bitfield set. decoder_init.cpp selects
// CS_MODE_MIPS64 for a 64-bit MIPS binary, so a real one arrived here with
// almost none of its arithmetic translated, and neither COV-01 nor PSEUDO-01
// could see it because both corpora are 32-bit.
//
// ONLY_MODE_64 throughout, and it is not vacuous here the way ONLY_MODE_32R6
// was in Batch F: CS_MODE_MIPS64 is half of this suite's instantiation.
// Capstone decodes none of these in MIPS32 mode, which is why no mode guard is
// needed in the translator.

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DADD)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
		{MIPS_REG_4, 0x1000000000000001},
	});

	emulate("dadd $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x2122334455667789},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DADDIU)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
	});

	emulate("daddiu $2, $3, 8");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x1122334455667790},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DSUBU)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
		{MIPS_REG_4, 0x0000000000000088},
	});

	emulate("dsubu $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x1122334455667700},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// A shift of 40 bits, which no MIPS64 shift immediate can hold: the assembler
// spells it dsll32 with sa = 8 and the hardware adds 32. Reading the reported
// immediate at face value shifts by 8 and answers 0x2233445566778800.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DSLL32)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
	});

	emulate("dsll32 $2, $3, 8");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x6677880000000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DSRL32)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
	});

	emulate("dsrl32 $2, $3, 8");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x112233},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The operand's top bit is set, so an arithmetic shift and a logical one give
// different answers. dsrl32 of the same value is 0xaabbcc.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DSRA32)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0xaabbccdd11223344},
	});

	emulate("dsra32 $2, $3, 8");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xffffffffffaabbcc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DSLLV)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
		{MIPS_REG_4, 4},
	});

	emulate("dsllv $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x1223344556677880},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DCLZ)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000100000000},
	});

	emulate("dclz $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 31},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DMULT)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000100000002},
		{MIPS_REG_4, 3},
	});

	emulate("dmult $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_LO, 0x0000000300000006},
		{MIPS_REG_HI, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DDIVU)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000300000007},
		{MIPS_REG_4, 0x0000000100000000},
	});

	// 0x0064001f = ddivu $3, $4. Keystone macro-expands a written `ddivu`
	// into a zero check, the divide, a break and an mflo, which is four
	// instructions and not what this is about.
	emulate_bin("1f 00 64 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_LO, 3},
		{MIPS_REG_HI, 7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The bitfield six. Capstone reports the ENCODED fields and does not apply the
// +32 that DEXTM, DEXTU, DINSM and DINSU add, so `dextm $2, $3, 0, 33` arrives
// as "pos 0, size 1" and `dextu $2, $3, 32, 8" as "pos 0, size 8". Taking
// those at face value reads a one-bit field and reads bit 0 instead of bit 32.
// Keystone will not assemble any of the six in KS_MODE_MIPS64 -- they need the
// R2 feature bit -- so these use hand-assembled encodings, as Batch F's EXT,
// INS and WSBH tests do.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DEXT)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334555667788},
	});

	// 0x7c623a03 = dext $2, $3, 8, 8
	emulate_bin("03 3a 62 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x77},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// size 33, reported as 1. Bit 32 of the source is set, so the two readings
// differ in the top bit of the answer as well as in its width.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DEXTM)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334555667788},
	});

	// 0x7c620001 = dextm $2, $3, 0, 33
	emulate_bin("01 00 62 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x155667788},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// pos 32, reported as 0. Read at face value this answers 0x88, the byte at the
// bottom, instead of 0x45, the byte at bit 32.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DEXTU)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334555667788},
	});

	// 0x7c623802 = dextu $2, $3, 32, 8
	emulate_bin("02 38 62 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x45},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The insert forms read their destination and keep every bit outside the
// field.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DINS)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd11223344},
		{MIPS_REG_3, 0x1122334555667788},
	});

	// 0x7c627a07 = dins $2, $3, 8, 8
	emulate_bin("07 7a 62 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xaabbccdd11228844},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DINSM)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd11223344},
		{MIPS_REG_3, 0x1122334555667788},
	});

	// 0x7c620005 = dinsm $2, $3, 0, 33
	emulate_bin("05 00 62 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xaabbccdd55667788},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DINSU)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_2, 0xaabbccdd11223344},
		{MIPS_REG_3, 0x1122334555667788},
	});

	// 0x7c623806 = dinsu $2, $3, 32, 8
	emulate_bin("06 38 62 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xaabbcc8811223344},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// dsbh swaps the two bytes within each of the four halfwords; dshd reverses
// the four halfwords and leaves the bytes inside them alone. Neither is a
// 64-bit byte swap, and one llvm.bswap.i64 answers 0x8877665544332211 for
// both -- which is what dsbh FOLLOWED BY dshd produces, and is how the ISA
// spells the full swap.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DSBH)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
	});

	// 0x7c0310a4 = dsbh $2, $3
	emulate_bin("a4 10 03 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x2211443366558877},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DSHD)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
	});

	// 0x7c031164 = dshd $2, $3
	emulate_bin("64 11 03 7c");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x7788556633441122},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DROTR)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
	});

	// 0x0023123a = drotr $2, $3, 8
	emulate_bin("3a 12 23 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x8811223344556677},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// A rotation of zero is a legal encoding meaning "no rotation", and the
// obvious `x >> n | x << (64 - n)` shifts by the whole width for it, which is
// poison in LLVM. translateRotr() had exactly that shape at 32 bits and
// `rotr rd, rt, 0` produced poison; masking the complement with width - 1
// fixes both.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DROTR_by_zero)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
	});

	// 0x0023103a = drotr $2, $3, 0
	emulate_bin("3a 10 23 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x1122334455667788},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DROTR32)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x1122334455667788},
	});

	// 0x0023123e = drotr32 $2, $3, 8
	emulate_bin("3e 12 23 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x4455667788112233},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The 32-bit rotate had the same poison-by-zero shape. `rotr $2, $3, 0` is the
// one input that tells the two implementations apart.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ROTR_by_zero)
{
	ONLY_MODE_32;

	setRegisters({
		{MIPS_REG_3, 0x11223344},
	});

	// 0x00231002 = rotr $2, $3, 0
	emulate_bin("02 10 23 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x11223344},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// A rotation of zero is a legal encoding, and the obvious
// `x >> n | x << (width - n)` shifts by the WHOLE WIDTH for it, which is
// poison in LLVM. translateRotr() had exactly that shape.
//
// The emulator cannot see it: getShiftAmount() turns a shift of 64 on an i64
// into `63 & 64`, which is 0, so it computes `x | x` and answers correctly
// whichever implementation is underneath -- reverting the fix leaves the two
// emulation tests above passing. The defect is a constant in the IR, so that
// is where it is checked: no shift in a translated rotate may have a constant
// amount at or above the operand's width.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, rotates_by_zero_do_not_shift_by_the_width)
{
	ALL_MODES;

	// 0x00231002 = rotr $2, $3, 0   (32-bit)
	// 0x0023103a = drotr $2, $3, 0  (64-bit, MIPS64 only)
	std::vector<std::string> insns = {"02 10 23 00"};
	if (GetParam() == CS_MODE_MIPS64)
	{
		insns.push_back("3a 10 23 00");
	}

	for (auto& a: insns)
	{
		auto* f = translate(utils::hexStringToBytes(a));
		ASSERT_NE(nullptr, f) << a;
		for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
		{
			unsigned op = it->getOpcode();
			if (op != Instruction::Shl && op != Instruction::LShr && op != Instruction::AShr)
			{
				continue;
			}
			if (auto* c = dyn_cast<ConstantInt>(it->getOperand(1)))
			{
				EXPECT_LT(c->getZExtValue(), it->getType()->getIntegerBitWidth()) << a << ": " << llvmObjToString(&*it);
			}
			for (auto& o: it->operands())
			{
				EXPECT_FALSE(isa<PoisonValue>(o.get())) << a;
			}
		}
	}
}

// `sllv rd, rt, rs` shifts by the low FIVE bits of rs and `dsllv` by the low
// six. The hardware masks and nothing here did, so a computed shift amount
// above the register width -- which is every case where the amount is not a
// constant -- shifted past the operand's width, which is poison in LLVM.
//
// The emulator masks an over-wide shift with exactly the same
// `& (width - 1)`, so no emulation test can tell the two apart: reverting the
// mask leaves every other test in this file passing. The assertion is on the
// IR, where the defect is.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, variable_shifts_mask_their_amount)
{
	ALL_MODES;

	const std::vector<std::string> insns = {
		"sllv $2, $3, $4",
		"srlv $2, $3, $4",
		"srav $2, $3, $4",
	};

	for (auto& a: insns)
	{
		auto* f = translate(assemble(a));
		ASSERT_NE(nullptr, f) << a;
		bool checked = false;
		for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
		{
			unsigned op = it->getOpcode();
			if (op != Instruction::Shl && op != Instruction::LShr && op != Instruction::AShr)
			{
				continue;
			}
			checked = true;
			auto* amount = it->getOperand(1);
			auto* mask = dyn_cast<BinaryOperator>(amount);
			ASSERT_NE(nullptr, mask) << a << ": shift amount is not masked";
			EXPECT_EQ(Instruction::And, mask->getOpcode()) << a;
			auto* c = dyn_cast<ConstantInt>(mask->getOperand(1));
			ASSERT_NE(nullptr, c) << a;
			EXPECT_EQ(it->getType()->getIntegerBitWidth() - 1, c->getZExtValue()) << a;
		}
		EXPECT_TRUE(checked) << a << ": no shift found";
	}
}

//
// On MIPS64 the instructions WITHOUT the D are 32-bit word operations whose
// results are sign-extended into the 64-bit register.
//
// Every one of these was done at the register width, which makes it the
// doubleword instruction sitting next to it in the table -- `addu` performing
// `daddu`, `sll` performing `dsll`, `mult` performing `dmult`. The rule was
// already in this file: translateMadd() and translateMsub() carry
//
//     // We operate on 0..31 bits even if on MIPS64.
//
// and truncate their operands. It was applied in two translators out of a
// dozen.
//
// None of the 688 MIPS tests that existed before this could see the
// difference, because all of them use operands that fit in 32 bits and whose
// results do not set bit 31 -- under which the two readings agree exactly.
// These use values where they do not.

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ADDU_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000180000000},
		{MIPS_REG_4, 1},
	});

	emulate("addu $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		// The word sum is 0x80000001, sign-extended. At the register width
		// it would be 0x0000000180000001 -- the bits above 31 survive and
		// the sign extension does not happen.
		{MIPS_REG_2, 0xffffffff80000001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SUBU_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000100000000},
		{MIPS_REG_4, 1},
	});

	emulate("subu $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// `daddu` is the 64-bit one and must NOT be narrowed. Same operands as the
// `addu` test above, so the two answers sit side by side.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DADDU_is_a_doubleword_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000180000000},
		{MIPS_REG_4, 1},
	});

	emulate("daddu $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x0000000180000001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The logical operations genuinely are 64-bit on MIPS64 and are not on the
// word list. If they were added to it this fails.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_AND_is_a_doubleword_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0xaabbccdd11223344},
		{MIPS_REG_4, 0xffffffffffff0000},
	});

	emulate("and $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xaabbccdd11220000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLL_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000018000000},
	});

	emulate("sll $2, $3, 4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xffffffff80000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The shift is of the WORD, so the bits above 31 do not come down into it.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SRL_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0xffffffff11223344},
	});

	emulate("srl $2, $3, 4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x01122334},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// Arithmetic, so the sign comes from bit 31 of the word rather than bit 63 of
// the register.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SRA_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000080000000},
	});

	emulate("sra $2, $3, 4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xfffffffff8000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// `sllv` masks to the low FIVE bits on MIPS64 too, because it is a word
// instruction. 36 is a shift of 4, not of 36.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLLV_masks_to_five_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000011223344},
		{MIPS_REG_4, 36},
	});

	emulate("sllv $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x12233440},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_ROTR_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000011223344},
	});

	// 0x00231202 = rotr $2, $3, 8
	emulate_bin("02 12 23 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x44112233},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// `mult` is a word multiply even on MIPS64: 32 x 32 into a 64-bit product,
// whose halves go into LO and HI sign-extended. At the register width it is
// `dmult`, and LO gets the whole 64-bit product with HI zero.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_MULTU_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x00000000ffffffff},
		{MIPS_REG_4, 2},
	});

	emulate("multu $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_LO, 0xfffffffffffffffe},
		{MIPS_REG_HI, 0x0000000000000001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIVU_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000100000007},
		{MIPS_REG_4, 2},
	});

	// 0x0064001b = divu $3, $4. Keystone macro-expands a written `divu`.
	emulate_bin("1b 00 64 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_LO, 3},
		{MIPS_REG_HI, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// `clz` counts in the low 32 bits. At the register width it counts the 31
// leading zeros of the doubleword instead of the 16 of the word.
TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_CLZ_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000100008000},
	});

	emulate("clz $2, $3");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 16},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Production O32/N64 subset — 32-bit AND 64-bit cases.
// Keystone macro-expands a written `div`/`divu`, so those use encodings.
//

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIV)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_2, 105},
		{MIPS_REG_3, 10},
	});

	// 0x0043001a = div $2, $3
	emulate_bin("1a 00 43 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_HI, 5},
		{MIPS_REG_LO, 10},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_DIV_is_a_word_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0x0000000180000000},
		{MIPS_REG_4, 2},
	});

	// 0x0064001a = div $3, $4
	emulate_bin("1a 00 64 00");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		// Word 0x80000000 / 2 = 0xc0000000, sign-extended.
		{MIPS_REG_LO, 0xffffffffc0000000},
		{MIPS_REG_HI, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_JALR_rd_rs)
{
	ALL_MODES;

	setRegisters({
		{MIPS_REG_1, 0x4005dc},
	});

	emulate("jalr $2, $1", 0x4006f8);

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_1});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x4006f8 + 0x4 + 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x4005dc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_OR_is_a_doubleword_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0xaabbccdd00000000},
		{MIPS_REG_4, 0x0000000011223344},
	});

	emulate("or $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xaabbccdd11223344},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_XOR_is_a_doubleword_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0xaabbccdd11223344},
		{MIPS_REG_4, 0xffffffff00000000},
	});

	emulate("xor $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x5544332211223344},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_NOR_is_a_doubleword_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0xaabbccdd00000000},
		{MIPS_REG_4, 0x0000000011223344},
	});

	emulate("nor $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x55443322eeddccbb},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLT_is_a_doubleword_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0xffffffffffffffff},
		{MIPS_REG_4, 1},
	});

	emulate("slt $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsTests, MIPS_INS_SLTU_is_a_doubleword_operation)
{
	ONLY_MODE_64;

	setRegisters({
		{MIPS_REG_3, 0xffffffffffffffff},
		{MIPS_REG_4, 1},
	});

	emulate("sltu $2, $3, $4");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_3, MIPS_REG_4});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// The big-endian fixture.
//
// lwl/lwr/swl/swr are the only instructions in this translator whose meaning
// depends on the endianness of the machine, and the fixture above can only
// ever be one of the two: Capstone2LlvmIrTranslator::createMips32() defaults
// to CS_MODE_LITTLE_ENDIAN and Keystone's KS_MODE_MIPS32 does the same. The
// parity corpus is big-endian mips-linux-gnu, which is to say the endianness
// the little-endian suite cannot see is the one the measured binaries use.
//
// So a second fixture, differing from the first in exactly two lines. Its
// expectations are the mirror images of the ones above; an implementation that
// hard-codes either endianness passes one suite and fails the other, and one
// that reads the endianness from somewhere other than its own Capstone mode
// has nothing to read it from.

class Capstone2LlvmIrTranslatorMipsBigEndianTests : public Capstone2LlvmIrTranslatorTests,
													public ::testing::WithParamInterface<cs_mode> {
protected:
	virtual void initKeystoneEngine() override
	{
		if (ks_open(KS_ARCH_MIPS, static_cast<ks_mode>(KS_MODE_MIPS32 | KS_MODE_BIG_ENDIAN), &_assembler) != KS_ERR_OK)
		{
			throw std::runtime_error("ERROR: failed on ks_open().\n");
		}
	}

	virtual void initCapstone2LlvmIrTranslator() override
	{
		_translator = Capstone2LlvmIrTranslator::createMips32(&_module, CS_MODE_BIG_ENDIAN);
	}
};

INSTANTIATE_TEST_SUITE_P(
	InstantiateMipsBigEndian,
	Capstone2LlvmIrTranslatorMipsBigEndianTests,
	::testing::Values(CS_MODE_MIPS32),
	PrintCapstoneModeToString_Mips());

TEST_P(Capstone2LlvmIrTranslatorMipsBigEndianTests, MIPS_INS_LWL)
{
	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("lwl $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x223344dd},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// Offset 0 is the whole word on a big-endian MIPS and one byte on a
// little-endian one. Compare with MIPS_INS_LWL_whole_word above, which needs
// offset 3 for the same answer.
TEST_P(Capstone2LlvmIrTranslatorMipsBigEndianTests, MIPS_INS_LWL_whole_word)
{
	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("lwl $2, 0($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0x11223344},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsBigEndianTests, MIPS_INS_LWR)
{
	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("lwr $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_JUST_REGISTERS_STORED({
		{MIPS_REG_2, 0xaabb1122},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsBigEndianTests, MIPS_INS_SWL)
{
	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("swl $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x11aabbcc_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorMipsBigEndianTests, MIPS_INS_SWR)
{
	setRegisters({
		{MIPS_REG_2, 0xaabbccdd},
		{MIPS_REG_3, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate("swr $2, 1($3)");

	EXPECT_JUST_REGISTERS_LOADED({MIPS_REG_2, MIPS_REG_3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xccdd3344_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

// The pair, big-endian: `lwl rt, 0(A)` then `lwr rt, 3(A)`. Memory holds
// 11 22 33 44 55 66 77 88 from 0x1000, so the big-endian word at 0x1001 is
// 0x22334455 -- and this is what GCC emits for `*(int*)p` when it cannot prove
// p aligned, which is why these two instructions are 12,442 occurrences in the
// corpus and never appear apart.
TEST_P(Capstone2LlvmIrTranslatorMipsBigEndianTests, unaligned_word_load_pair)
{
	setRegisters({
		{MIPS_REG_3, 0x1001},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
		{0x1004, 0x55667788_dw},
	});

	emulate("lwl $2, 0($3)\nlwr $2, 3($3)");

	EXPECT_EQ(0x22334455, getRegisterValueUnsigned(MIPS_REG_2));
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
