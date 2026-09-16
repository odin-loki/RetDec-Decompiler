/**
 * @file tests/capstone2llvmir/arm_tests.cpp
 * @brief Capstone2LlvmIrTranslatorArm unit tests.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <llvm/IR/InstIterator.h>

#include <cstring>
#include <cstdint>
#include <limits>
#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/arm/arm.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorArmTests :
		public Capstone2LlvmIrTranslatorTests,
		public ::testing::WithParamInterface<cs_mode>
{
	protected:
		virtual void initKeystoneEngine() override
		{
			ks_mode mode = KS_MODE_ARM;
			switch(GetParam())
			{
				// Basic modes.
				case CS_MODE_ARM: mode = KS_MODE_ARM; break;
				case CS_MODE_THUMB: mode = KS_MODE_THUMB; break;
				// Extra modes.
				case CS_MODE_MCLASS: mode = KS_MODE_ARM; break; // Missing in Keystone.
				case CS_MODE_V8: mode = KS_MODE_V8; break;
				// Unhandled modes.
				default: throw std::runtime_error("ERROR: unknown mode.\n");
			}
			if (ks_open(KS_ARCH_ARM, mode, &_assembler) != KS_ERR_OK)
			{
				throw std::runtime_error("ERROR: failed on ks_open().\n");
			}
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			switch(GetParam())
			{
				case CS_MODE_ARM:
					_translator = Capstone2LlvmIrTranslator::createArm(&_module);
					break;
				case CS_MODE_THUMB:
					_translator = Capstone2LlvmIrTranslator::createThumb(&_module);
					break;
				default:
					throw std::runtime_error("ERROR: unknown mode.\n");
			}
		}

		// These can/should be used at the beginning of each test case to
		// determine which modes should the case be run for.
		// They are macros because we want them to cause return in the current
		// function (test case).
		//
		protected:
#define ALL_MODES
#define ONLY_MODE_ARM if (GetParam() != CS_MODE_ARM) return;
#define ONLY_MODE_THUMB if (GetParam() != CS_MODE_THUMB) return;
#define SKIP_MODE_ARM if (GetParam() == CS_MODE_ARM) return;
#define SKIP_MODE_THUMB if (GetParam() == CS_MODE_THUMB) return;
};

struct PrintCapstoneModeToString_Arm
{
	template <class ParamType>
	std::string operator()(const TestParamInfo<ParamType>& info) const
	{
		switch (info.param)
		{
			case CS_MODE_ARM: return "CS_MODE_ARM";
			case CS_MODE_THUMB: return "CS_MODE_THUMB";
			case CS_MODE_MCLASS: return "CS_MODE_MCLASS";
			case CS_MODE_V8: return "CS_MODE_V8";
			default: return "UNHANDLED CS_MODE";
		}
	}
};

// By default, all the test cases are run with all the modes.
// If some test case is not meant for all modes, use some of the ONLY_MODE_*,
// SKIP_MODE_* macros.
//
INSTANTIATE_TEST_SUITE_P(
		InstantiateArmWithAllModes,
		Capstone2LlvmIrTranslatorArmTests,
		::testing::Values(CS_MODE_ARM, CS_MODE_THUMB),
		 PrintCapstoneModeToString_Arm());

//
// ARM_INS_ADD
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1230},
	});

	emulate("add r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_bin)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R1, 0x1230},
	});

	emulate_bin("04 00 81 e2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_i)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1230},
	});

	emulate("add r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R1, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_R2, 0x4},
	});

	emulate("add r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1230},
		{ARM_REG_R1, 0x4},
	});

	emulate("add r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_s_zero_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0},
		{ARM_REG_R2, 0x0},
	});

	emulate("adds r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x0},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_s_negative_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xffff0000},
		{ARM_REG_R2, 0x1234},
	});

	emulate("adds r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xffff1234},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_s_carry_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xffffffff},
		{ARM_REG_R2, 0x1},
	});

	emulate("adds r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x0},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_C, true},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_s_overflow_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0fffffff},
		{ARM_REG_R2, 0x74080891},
	});

	emulate("adds r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x84080890},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_eq_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, true},
	});

	emulate("addeq r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_eq_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, false},
	});

	emulate("addeq r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_Z});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ne_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, false},
	});

	emulate("addne r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ne_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, true},
	});

	emulate("addne r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_Z});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_hs_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, true},
	});

	emulate("addhs r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_hs_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, false},
	});

	emulate("addhs r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_C});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_lo_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, false},
	});

	emulate("addlo r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_lo_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, true},
	});

	emulate("addlo r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_C});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_mi_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, true},
	});

	emulate("addmi r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_N});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_mi_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, false},
	});

	emulate("addmi r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_N});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_pl_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, false},
	});

	emulate("addpl r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_N});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_pl_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, true},
	});

	emulate("addpl r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_N});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_vs_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addvs r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_vs_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addvs r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_vc_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addvc r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_vc_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addvc r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_hi_true_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, true},
		{ARM_REG_CPSR_Z, false},
	});

	emulate("addhi r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C, ARM_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_hi_false_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, true},
		{ARM_REG_CPSR_Z, true},
	});

	emulate("addhi r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_C, ARM_REG_CPSR_Z});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_hi_false_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_Z, false},
	});

	emulate("addhi r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_C, ARM_REG_CPSR_Z});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ls_true_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_Z, false},
	});

	emulate("addls r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C, ARM_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ls_true_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_Z, true},
	});

	emulate("addls r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C, ARM_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ls_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_C, true},
		{ARM_REG_CPSR_Z, false},
	});

	emulate("addls r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_C, ARM_REG_CPSR_Z});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ge_true_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addge r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ge_true_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addge r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ge_false_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addge r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_ge_false_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addge r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_lt_true_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addlt r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_lt_true_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addlt r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_lt_false_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addlt r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_lt_false_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addlt r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_gt_true_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addgt r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_Z, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_gt_true_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addgt r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_Z, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_gt_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addgt r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_Z, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_le_true_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_V, true},
	});

	emulate("addle r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_Z, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_le_true_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addle r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_Z, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_r_r_i_le_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_V, false},
	});

	emulate("addle r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_Z, ARM_REG_CPSR_N, ARM_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_Seq_r_r_i_eq_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x0},
		{ARM_REG_R2, 0x0},
		{ARM_REG_CPSR_Z, true},
	});

	emulate("addseq r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x0},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_Seq_r_r_i_eq_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x0},
		{ARM_REG_R2, 0x0},
		{ARM_REG_CPSR_Z, false},
	});

	emulate("addseq r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_CPSR_Z});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_lsl)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x08000001}, // shifted out to CF | 1 << 5 = 0x20 = 32
	});

	emulate("add r0, r1, r2, LSL#5");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1020},
		{ARM_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_lsl_reg)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x1}, // 1 << 5 = 0x20 = 32
		{ARM_REG_R3, 0x5},
	});

	emulate("add r0, r1, r2, LSL r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1020},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_lsr)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x410}, // 0x410 >> 5 = 0x20 | shifted out to CF
	});

	emulate("add r0, r1, r2, LSR#5");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1020},
		{ARM_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_lsr_reg)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x400}, // 0x400 >> 5 = 0x20
		{ARM_REG_R3, 0x5},
	});

	emulate("add r0, r1, r2, LSR r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1020},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_asr)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x410}, // 0x410 >> 5 = 0x20 | shifted out to CF
	});

	emulate("add r0, r1, r2, ASR#5");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1020},
		{ARM_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_asr_reg)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x80000400}, // 0x80000400 >> 5 = 0xfc000020
		{ARM_REG_R3, 0x5},
	});

	emulate("add r0, r1, r2, ASR r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xfc001020},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_ror)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x410}, // 0x410 ror 5 = 0x80 00 00 20 | shifted out to CF
	});

	emulate("add r0, r1, r2, ROR#5");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x80001020},
		{ARM_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_ror_reg)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x400}, // 0x400 ror 5 = 0x20
		{ARM_REG_R3, 0x5},
	});

	emulate("add r0, r1, r2, ASR r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1020},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// TODO: Keystone/Capstone does not like this asm.
//
//TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_rrx)
//{
//	ALL_MODES;
//
//	setRegisters({
//		{ARM_REG_CPSR_C, true},
//		{ARM_REG_R1, 0x1000},
//		{ARM_REG_R2, 0x410}, // (0x410 | 0x1) ror 5 = 0x08 00 00 20 | shifted out to CF
//	});
//
//	emulate("add r0, r1, r2, RRX#5");
//
//	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_CPSR_C});
//	EXPECT_JUST_REGISTERS_STORED({
//		{ARM_REG_R0, 0x8001020},
//		{ARM_REG_CPSR_C, true},
//	});
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_NO_VALUE_CALLED();
//}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_arm_r_pc_i)
{
	SKIP_MODE_THUMB;

	emulate("add r0, pc, #4", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x100c}, // 0x4 + 0x1000 + 0x8
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_thumb_r_pc)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x4},
	});

	emulate("add r0, pc", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1008}, // 0x4 + 0x1000 + 0x4
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADD_arm_pc_result)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x4},
	});

	emulate("add pc, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x1004}},
	});
}

//
// ARM_INS_CMN
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CMN_zero_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0},
		{ARM_REG_R2, 0x0},
	});

	emulate("cmn r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CMN_negative_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xffff0000},
		{ARM_REG_R2, 0x1234},
	});

	emulate("cmn r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CMN_carry_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xffffffff},
		{ARM_REG_R2, 0x1},
	});

	emulate("cmn r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_C, true},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CMN_overflow_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0fffffff},
		{ARM_REG_R2, 0x74080891},
	});

	emulate("cmn r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_SUB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SUB_r_r_i)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
	});

	emulate("sub r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1230},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SUB_s_zero_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x1234},
	});

	emulate("subs r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x0},
		{ARM_REG_CPSR_N, false}, // TODO: check flags with some emulator
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_C, true},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_CMP
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CMP_zero_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x1234},
	});

	emulate("cmp r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
		{ARM_REG_CPSR_C, true},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_AND
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_AND_r_r_i)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("and r0, r1, #0x78");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x78},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_AND_s_zero_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
		{ARM_REG_R2, 0x0},
	});

	emulate("ands r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x0},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_BIC
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BIC_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
		{ARM_REG_R2, 0xff00ff00}, // -> 0x00ff00ff
	});

	emulate("bic r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x00340078},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BIC_s_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
		{ARM_REG_R2, 0xffffffff}, // -> 0x0
	});

	emulate("bics r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x0},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_ORR
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ORR_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12005678},
		{ARM_REG_R2, 0x00340078},
	});

	emulate("orr r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ORR_s_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xff000000},
		{ARM_REG_R2, 0x12345678},
	});

	emulate("orrs r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xff345678},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_EOR
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_EOR_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12005678},
		{ARM_REG_R2, 0x00340078},
	});

	emulate("eor r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345600},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_EOR_s_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xff00ff00},
		{ARM_REG_R2, 0x12345678},
	});

	emulate("eors r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xed34a978},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_MOV
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MOV_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("mov r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MOV_s_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xff000000},
	});

	emulate("movs r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xff000000},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_MOVT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MOVT)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
	});

	emulate("movt r0, #0xabcd");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xabcd5678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_MOVW
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MOVW)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
	});

	emulate("movw r0, #0xabcd");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xabcd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_MVN
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MVN_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xf0f0f0f0},
	});

	emulate("mvn r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x0f0f0f0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MVN_s_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0000ffff},
	});

	emulate("mvns r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xffff0000},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_HINT
//
// This test was called ARM_INS_NOP and asserted that `nop` came out as a call
// to __asm_nop. Both halves of that were wrong and agreed with each other:
// capstone decodes `nop` to ARM_INS_HINT, not ARM_INS_NOP, so the table's
// translateNop entry was on a key capstone never produces and every hint took
// the pseudo-asm path. The test recorded the behaviour it found rather than
// the behaviour the instruction has, and 238 hints in the ARM corpus -- its
// most frequent untranslated instruction, by a factor of three -- went out as
// opaque calls.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_HINT_nop_is_nothing)
{
	ALL_MODES;

	emulate("nop");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_HINT_wfi_keeps_its_call)
{
	ALL_MODES;

	// Not every hint is a no-op. WFI stops the core until an interrupt
	// arrives, and dropping it because it shares an instruction id with NOP
	// would delete an observable effect rather than an absence of one.
	emulate("wfi");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_wfi"), {}},
	});
}

//
// ARM_INS_IT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_IT_is_a_no_op_of_its_own)
{
	ONLY_MODE_THUMB;

	// The block header does nothing by itself: capstone puts the condition on
	// each instruction inside the block and the dispatcher wraps those in it.
	emulate("it eq");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_ADR
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADR)
{
	ONLY_MODE_THUMB;

	// At address 0 a Thumb instruction reads PC as 4, so `adr r0, #20` is 24.
	// Capstone hands over the offset, not the resolved address, so a
	// translation that stored the immediate would answer 20.
	emulate("adr r0, #20");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 24},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, thumb_reads_pc_as_address_plus_four_when_four_bytes_wide)
{
	ONLY_MODE_THUMB;

	// `addw r0, pc, #20` is a 32-bit Thumb instruction, and Thumb reads PC as
	// the instruction's address plus 4 whatever its width. getCurrentPc()
	// computed address + 2*size, which is +4 for a 16-bit instruction and +8
	// for this one, so this answered 28.
	emulate("addw r0, pc, #20");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 24},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_PLD, ARM_INS_PLI
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_PLD_is_nothing)
{
	ALL_MODES;

	// A prefetch hint touches no register and no memory and cannot fault, so
	// there is nothing to translate. As a nullptr entry it came out as an
	// __asm_pld call -- 2,142 of them across the static ARM corpus.
	setRegisters({
		{ARM_REG_R0, 0x1000},
	});

	emulate("pld [r0]");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_PLI_is_nothing)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1000},
	});

	emulate("pli [r0]");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_VPUSH, ARM_INS_VPOP
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VPUSH_uses_eight_byte_slots_for_d_registers)
{
	ALL_MODES;

	// The slot size is the register's, not the architecture's word. At four
	// bytes apart -- which is what the integer PUSH/POP translator writes --
	// d1 would land on top of the second half of d0 at 0x10f4 and the two
	// stores would overlap.
	setRegisters({
		{ARM_REG_SP, 0x1100},
		{ARM_REG_D0, 1.5_f64},
		{ARM_REG_D1, 2.5_f64},
	});

	emulate("vpush {d0, d1}");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_SP, 0x10f0},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x10f0, 1.5_f64},
		{0x10f8, 2.5_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VPOP_reads_upward_from_sp)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 1.5_f64},
		{0x1008, 2.5_f64},
	});

	emulate("vpop {d0, d1}");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 1.5_f64},
		{ARM_REG_D1, 2.5_f64},
		{ARM_REG_SP, 0x1010},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_ORN
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ORN)
{
	ONLY_MODE_THUMB;

	// 0x10000000 | ~0x0000000f is 0xfffffff0. `a | b` would be 0x1000000f
	// and `~(a | b)` 0xefffffe0, so the operands are chosen to tell the three
	// readings apart.
	setRegisters({
		{ARM_REG_R1, 0x10000000},
		{ARM_REG_R2, 0x0000000f},
	});

	emulate("orn r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xfffffff0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_ADC
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADC_r_r_i_false)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_CPSR_C, false},
		{ARM_REG_R1, 0x1230},
	});

	emulate("adc r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
		// TODO: These probably should not be set for "adc" without "s".
		// Probbaly a Capstone bug.
		{ARM_REG_CPSR_N, ANY},
		{ARM_REG_CPSR_Z, ANY},
		{ARM_REG_CPSR_C, ANY},
		{ARM_REG_CPSR_V, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADC_r_r_i_true)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_CPSR_C, true},
		{ARM_REG_R1, 0x1230},
	});

	emulate("adc r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1235},
		// TODO: These probably should not be set for "adc" without "s".
		// Probbaly a Capstone bug.
		{ARM_REG_CPSR_N, ANY},
		{ARM_REG_CPSR_Z, ANY},
		{ARM_REG_CPSR_C, ANY},
		{ARM_REG_CPSR_V, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADC_s_r_r_i_false)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_CPSR_C, false},
		{ARM_REG_R1, 0x1230},
	});

	emulate("adcs r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_SBC
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SBC_r_r_i_false)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_CPSR_C, false},
		{ARM_REG_R1, 0x1235},
	});

	emulate("sbc r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1230},
		// TODO: These probably should not be set for "sbc" without "s".
		// Probbaly a Capstone bug.
		{ARM_REG_CPSR_N, ANY},
		{ARM_REG_CPSR_Z, ANY},
		{ARM_REG_CPSR_C, ANY},
		{ARM_REG_CPSR_V, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SBC_r_r_i_true)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_CPSR_C, true},
		{ARM_REG_R1, 0x1235},
	});

	emulate("sbc r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1231},
		// TODO: These probably should not be set for "sbc" without "s".
		// Probbaly a Capstone bug.
		{ARM_REG_CPSR_N, ANY},
		{ARM_REG_CPSR_Z, ANY},
		{ARM_REG_CPSR_C, ANY},
		{ARM_REG_CPSR_V, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SBC_s_r_r_i_false)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_CPSR_C, false},
		{ARM_REG_R1, 0x1235},
	});

	emulate("sbcs r0, r1, #4");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1230},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false}, // TODO: check, somehow (emul) is it ok?
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_RSC
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_RSC_r_r_r_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_CPSR_C, false},
		{ARM_REG_R1, 0x1235},
		{ARM_REG_R2, 0x4},
	});

	emulate("rsc r0, r2, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1230},
		// TODO: These probably should not be set for "rsc" without "s".
		// Probably a Capstone bug.
		{ARM_REG_CPSR_N, ANY},
		{ARM_REG_CPSR_Z, ANY},
		{ARM_REG_CPSR_C, ANY},
		{ARM_REG_CPSR_V, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_RSC_r_r_r_true)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_CPSR_C, true},
		{ARM_REG_R1, 0x1235},
		{ARM_REG_R2, 0x4},
	});

	emulate("rsc r0, r2, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1231},
		// TODO: These probably should not be set for "rsc" without "s".
		// Probably a Capstone bug.
		{ARM_REG_CPSR_N, ANY},
		{ARM_REG_CPSR_Z, ANY},
		{ARM_REG_CPSR_C, ANY},
		{ARM_REG_CPSR_V, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_RSC_s_r_r_r_false)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_CPSR_C, false},
		{ARM_REG_R1, 0x1235},
		{ARM_REG_R2, 0x4},
	});

	emulate("rscs r0, r2, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1230},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false}, // TODO: check, somehow (emul) is it ok?
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_RSB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_RSB_r_r_i)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x4},
	});

	emulate("rsb r0, r1, #0x8");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_RSB_s_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x1234},
	});

	emulate("rsbs r0, r2, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false}, // TODO: Chek with some emulator.
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_MUL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MUL_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x1234},
	});

	emulate("mul r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1234000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MUL_s_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x0},
	});

	emulate("muls r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R1, ANY},
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDR
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldr r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_plus_imm)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, #8]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_imm)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1010},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, #-8]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_plus_reg)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x8},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, r2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_reg)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1010},
		{ARM_REG_R2, -0x8},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, r2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_plus_imm_preindexed_writeback)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, #8]!");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_imm_preindexed_writeback)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1010},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, #-8]!");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_plus_reg_preindexed_writeback)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x8},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, r2]!");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_reg_preindexed_writeback)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1010},
		{ARM_REG_R2, -0x8},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, r2]!");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_plus_imm_postindexed_writeback)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldr r0, [r1], #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_imm_postindexed_writeback)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldr r0, [r1], #-8");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0xff8},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_plus_reg_postindexed_writeback)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x8},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldr r0, [r1], r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_reg_postindexed_writeback_1)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, -0x8},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldr r0, [r1], r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0xff8},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_reg_postindexed_writeback_2)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x8},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldr r0, [r1], -r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0xff8},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRT)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldrt r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDREX
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDREX)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldrex r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdrexLoadIsAtomic)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldrex r0, [r1]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			if (l->isAtomic())
			{
				found = true;
				EXPECT_EQ(l->getOrdering(), AtomicOrdering::Monotonic);
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdrexLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldrex r0, [r1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdrexbLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldrexb r0, [r1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdrexhLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldrexh r0, [r1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdrLoadIsNotAtomic)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldr r0, [r1]"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			EXPECT_FALSE(l->isAtomic());
		}
	}
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, MemoryLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldr r0, [r1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, MemoryStoreAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("str r0, [r1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdaLoadIsAtomic)
{
	ONLY_MODE_ARM;
	// lda r0, [r1] — Keystone ARM mode lacks v8; encoding from llvm-mc.
	auto* f = translate({0x9f, 0x0c, 0x91, 0xe1});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			if (l->isAtomic())
			{
				found = true;
				EXPECT_EQ(l->getOrdering(), AtomicOrdering::Acquire);
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdaLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x9f, 0x0c, 0x91, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdabLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	// ldab r0, [r1] — Keystone ARM mode lacks v8; encoding from ARM ARM.
	auto* f = translate({0x9f, 0x0c, 0xd1, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdahLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x9f, 0x0c, 0xf1, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDA)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate_bin("9f 0c 91 e1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdaexLoadIsAtomic)
{
	ONLY_MODE_ARM;
	// ldaex r0, [r1]
	auto* f = translate({0x9f, 0x0e, 0x91, 0xe1});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			if (l->isAtomic())
			{
				found = true;
				EXPECT_EQ(l->getOrdering(), AtomicOrdering::Acquire);
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdaexLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x9f, 0x0e, 0x91, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdaexbLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x9f, 0x0e, 0xd1, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdaexhLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x9f, 0x0e, 0xf1, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdaexdLoadIsAtomic)
{
	ONLY_MODE_ARM;
	// ldaexd r0, r1, [r2] — Keystone ARM mode lacks v8; encoding from ARM ARM.
	auto* f = translate({0x9f, 0x0e, 0xb2, 0xe1});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			if (l->isAtomic())
			{
				found = true;
				EXPECT_EQ(l->getOrdering(), AtomicOrdering::Acquire);
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdaexdLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x9f, 0x0e, 0xb2, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDAEXD)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R2, 0x1000},
	});
	setMemory({
		{0x1000, 0x1234567890abcdef_qw},
	});

	emulate_bin("9f 0e b2 e1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x90abcdef},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRB)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldrb r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRBT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRBT)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldrbt r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDREXB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDREXB)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldrexb r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRSB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRSB)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldrsb r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xfffffff1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRSBT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRSBT)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldrsbt r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xfffffff1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRH
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRH)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf123_w},
	});

	emulate("ldrh r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf123},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRHT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRHT)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf123_w},
	});

	emulate("ldrht r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf123},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDREXH
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDREXH)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf123_w},
	});

	emulate("ldrexh r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf123},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRSH
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRSH)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf123_w},
	});

	emulate("ldrsh r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xfffff123},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRSHT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRSHT)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf123_w},
	});

	emulate("ldrsht r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xfffff123},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDRD
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDRD)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x1000},
	});
	setMemory({
		{0x1000, 0x1234567890abcdef_qw},
	});

	emulate("ldrd r0, r1, [r2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x90abcdef},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDREXD
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdrexdLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldrexd r0, r1, [r2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDREXD)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x1000},
	});
	setMemory({
		{0x1000, 0x1234567890abcdef_qw},
	});

	emulate("ldrexd r0, r1, [r2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x90abcdef},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STR
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STR)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1000},
	});

	emulate("str r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StrexStoreIsAtomic)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("strex r0, r1, [r2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StrexAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("strex r0, r1, [r2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StrexbAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("strexb r0, r1, [r2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StrexhAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("strexh r0, r1, [r2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STREX)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
		{ARM_REG_R2, 0x1000},
	});

	emulate("strex r0, r1, [r2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StrexdStoreIsAtomic)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("strexd r0, r2, r3, [r1]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->isAtomic())
			{
				found = true;
				EXPECT_EQ(s->getOrdering(), AtomicOrdering::Monotonic);
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StrexdAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("strexd r0, r2, r3, [r1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STREXD)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
		{ARM_REG_R1, 0x1000},
	});

	emulate("strexd r0, r2, r3, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x90abcdef12345678_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlStoreIsAtomic)
{
	ONLY_MODE_ARM;
	// stl r0, [r1] — Keystone ARM mode lacks v8; encoding from llvm-mc.
	auto* f = translate({0x90, 0xfc, 0x81, 0xe1});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->isAtomic())
			{
				found = true;
				EXPECT_EQ(s->getOrdering(), AtomicOrdering::Release);
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x90, 0xfc, 0x81, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlbAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x90, 0xfc, 0xc1, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlhAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x90, 0xfc, 0xe1, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STL)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1000},
	});

	emulate_bin("90 fc 81 e1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlexStoreIsAtomic)
{
	ONLY_MODE_ARM;
	// stlex r0, r1, [r2]
	auto* f = translate({0x91, 0x0e, 0x82, 0xe1});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->isAtomic())
			{
				found = true;
				EXPECT_EQ(s->getOrdering(), AtomicOrdering::Release);
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlexAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x91, 0x0e, 0x82, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlexbAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x91, 0x0e, 0xc2, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlexhAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x91, 0x0e, 0xe2, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STLEX)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
		{ARM_REG_R2, 0x1000},
	});

	emulate_bin("91 0e 82 e1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlexdStoreIsAtomic)
{
	ONLY_MODE_ARM;
	// stlexd r0, r2, r3, [r1] — Keystone ARM mode lacks v8; encoding from ARM ARM.
	auto* f = translate({0x92, 0x0e, 0xa1, 0xe1});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->isAtomic())
			{
				found = true;
				EXPECT_EQ(s->getOrdering(), AtomicOrdering::Release);
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, StlexdAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate({0x92, 0x0e, 0xa1, 0xe1});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STLEXD)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
		{ARM_REG_R1, 0x1000},
	});

	emulate_bin("92 0e a1 e1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x90abcdef12345678_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SwpEmitsAtomicRmw)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("swp r0, r1, [r2]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			found = true;
			EXPECT_EQ(rmw->getOperation(), AtomicRMWInst::Xchg);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SwpAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("swp r0, r1, [r2]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			if (rmw->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SWP)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
		{ARM_REG_R2, 0x1000},
	});
	setMemory({
		{0x1000, 0xaabbccdd_dw},
	});

	emulate("swp r0, r1, [r2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xaabbccdd},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SwpbEmitsAtomicRmw)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("swpb r0, r1, [r2]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			found = true;
			EXPECT_EQ(rmw->getOperation(), AtomicRMWInst::Xchg);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SwpbAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("swpb r0, r1, [r2]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			if (rmw->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SWPB)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
		{ARM_REG_R2, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("swpb r0, r1, [r2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x78_b}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STR_dst_shift)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R4, 0x12345678},
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R6, 0x2}, // 2 << 2 = 8
	});

	emulate("str r4, [r0, r6, lsl #2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R4, ARM_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x12345678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

// TODO: Same extensive testing as for LDR.

//
// ARM_INS_STRT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STRT)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1000},
	});

	emulate("strt r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STRB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STRB)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1000},
	});

	emulate("strb r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x78_b}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STRBT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STRBT)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1000},
	});

	emulate("strbt r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x78_b}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STRH
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STRH)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1000},
	});

	emulate("strh r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x5678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STRHT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STRHT)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x1000},
	});

	emulate("strht r0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x5678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STRD
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STRD)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x90abcdef},
		{ARM_REG_R2, 0x1000},
	});

	emulate("strd r0, r1, [r2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1, ARM_REG_R2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x90abcdef_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STRD_sp_imm)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x90abcdef},
		{ARM_REG_SP, 0x1000},
	});

	emulate("strd r0, r1, [sp, #0x18]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1, ARM_REG_SP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1018, 0x12345678_dw},
		{0x101c, 0x90abcdef_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STRD_sp_post_imm)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_R1, 0x90abcdef},
		{ARM_REG_SP, 0x1000},
	});

	emulate("strd r0, r1, [sp], #0x18");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1, ARM_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_SP, ANY}, // Not an exact value because some strange behaviour.
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x90abcdef_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_TEQ
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TQE_r_r_eq)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_R2, 0x1230},
	});

	emulate("teq r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, false}, // XOR of original sign bits
		{ARM_REG_CPSR_Z, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TQE_r_r_neg_1)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1230},
		{ARM_REG_R2, -0x1230},
	});

	emulate("teq r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, true}, // XOR of original sign bits
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TQE_r_r_neg_2)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, -0x5678},
		{ARM_REG_R2, -0x1234},
	});

	emulate("teq r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, false}, // XOR of original sign bits
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TQE_r_r_neg_3)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, -0x5678},
		{ARM_REG_R2, 0x1234},
	});

	emulate("teq r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, true}, // XOR of original sign bits
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_TST
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TST_eq)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xffff0000},
		{ARM_REG_R2, 0x0000ffff},
	});

	emulate("tst r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TST_neg)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xffff0000},
		{ARM_REG_R2, 0xf000ffff},
	});

	emulate("tst r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_REV
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_REV)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("rev r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x78563412},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_REV16
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_REV16)
{
	// Bytes swapped WITHIN each halfword, both halves. A 32-bit bswap -- the
	// plausible wrong answer, and the one REV next door actually is -- gives
	// 0x78563412.
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("rev16 r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x34127856},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_REVSH
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_REVSH)
{
	// The low halfword swapped and then SIGN-extended. 0x0080 swaps to 0x8000,
	// whose top bit is set, so zero-extending answers 0x00008000 instead.
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12340080},
	});

	emulate("revsh r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xffff8000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	// No call assertion: tests/llvmir-emul computes llvm.bswap without
	// recording it, unlike llvm.bitreverse, which it records because the RBIT
	// tests below ask for it. The stored value is what proves the semantics.
}

//
// ARM_INS_RBIT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_RBIT)
{
	// All 32 bits reversed, not the bytes: 0x12345678 is
	// 00010010 00110100 01010110 01111000, which read backwards is
	// 00011110 01101010 00101100 01001000.
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("rbit r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1e6a2c48},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("llvm.bitreverse.i32"), {0x12345678}},
	});
}

//
// ARM_INS_CLZ
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CLZ_zeroes)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0000ffff},
	});

	emulate("clz r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 16},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CLZ_ones)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xf0000000},
	});

	emulate("clz r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UQADD8
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQADD8)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("uqadd8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_uqadd8"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_UQADD16
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQADD16)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("uqadd16 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_uqadd16"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_UQSUB8
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQSUB8)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("uqsub8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_uqsub8"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_UQADD16
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQSUB16)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("uqsub16 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_uqsub16"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_UQASX
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQASX)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("uqasx r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_uqasx"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_UQSAX
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQSAX)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("uqsax r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_uqsax"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_SEL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SEL)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("sel r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_sel"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_USAD8
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_USAD8)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("usad8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_usad8"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_USADA8
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_USADA8)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
		{ARM_REG_R3, 0x9abc},
	});

	emulate("usada8 r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_usada8"), {0x1234, 0x5678, 0x9abc}},
	});
}

//
// ARM_INS_USAT
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_USAT)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x5678},
	});

	emulate("usat r0, #8, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_usat"), {0x8, 0x5678}},
	});
}

//
// ARM_INS_USAT16
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_USAT16)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x5678},
	});

	emulate("usat16 r0, #8, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_usat16"), {0x8, 0x5678}},
	});
}

//
// ARM_INS_UHADD8
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UHADD8)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
	});

	emulate("uhadd8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_uhadd8"), {0x1234, 0x5678}},
	});
}

//
// ARM_INS_B
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_B)
{
	ALL_MODES;

	emulate("b #0x110d8", 0x1107C);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x110d8}},
	});
}

//
// ARM_INS_BX
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BX)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x110d8},
	});

	emulate("bx r1", 0x1107C);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x110d8}},
	});
}

//
// ARM_INS_BL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BL)
{
	ALL_MODES;

	emulate("bl #0x110d8", 0x1107C);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_LR, 0x11080},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x110d8}},
	});
}

//
// ARM_INS_BLX
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BLX_arm)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x110d8},
	});

	emulate("blx r1", 0x1107C);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_LR, 0x11080},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x110d8}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BLX_thumb)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x110d8},
	});

	emulate("blx r1", 0x1107C);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_LR, 0x1107e},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x110d8}},
	});
}

//
// ARM_INS_CBNZ
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CBNZ_cond_true)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1234},
	});

	emulate("cbnz r1, #0x1008", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1008}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CBNZ_cond_false)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x0},
	});

	emulate("cbnz r1, #0x1008", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1008}},
	});
}

//
// ARM_INS_CBZ
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CBZ_cond_true)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x0},
	});

	emulate("cbz r1, #0x1008", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1008}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_CBZ_cond_false)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1234},
	});

	emulate("cbz r1, #0x1008", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1008}},
	});
}

//
// ARM_INS_LSL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSL_imm)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x00001234},
	});

	emulate("lsl r0, r1, #16");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12340000},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSL_reg)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x00001234},
		{ARM_REG_R2, 0x10},
	});

	emulate("lsl r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12340000},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LSR
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSR_imm)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x410}, // 0x410 >> 5 = 0x20 | shifted out to CF
	});

	emulate("lsr r0, r2, #5");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x20},
		{ARM_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSR_reg)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x400}, // 0x400 >> 5 = 0x20
		{ARM_REG_R3, 0x5},
	});

	emulate("lsr r0, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x20},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_ASR
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ASR_imm)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x410}, // 0x410 >> 5 = 0x20 | shifted out to CF
	});

	emulate("asr r0, r2, #5");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x20},
		{ARM_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ASR_reg)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R2, 0x80000400}, // 0x80000400 >> 5 = 0xfc000020
		{ARM_REG_R3, 0x5},
	});

	emulate("asr r0, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xfc000020},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_ROR
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ROR_imm)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x410}, // 0x410 ror 5 = 0x80 00 00 20 | shifted out to CF
	});

	emulate("ror r0, r2, #5");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x80000020},
		{ARM_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ROR_reg)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R2, 0x400}, // 0x400 ror 5 = 0x20
		{ARM_REG_R3, 0x5},
	});

	emulate("ror r0, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x20},
		{ARM_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDM
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDM)
{
	ALL_MODES;

	setMemory({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_R0, 0x1000},
	});

	emulate("ldm r0, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004, 0x1008});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDM_wb)
{
	ALL_MODES;

	setMemory({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_R0, 0x1000},
	});

	emulate("ldm r0!, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x100c},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004, 0x1008});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_POP
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_POP)
{
	ALL_MODES;

	setMemory({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_SP, 0x1000},
	});

	emulate("pop {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_SP, 0x100c},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004, 0x1008});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDMIB - ARM only
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDMIB)
{
	SKIP_MODE_THUMB;

	setMemory({
		{0x1004, 0x2_dw},
		{0x1008, 0x4_dw},
		{0x100c, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_R0, 0x1000},
	});

	emulate("ldmib r0, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1004, 0x1008, 0x100c});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDMIB_wb)
{
	SKIP_MODE_THUMB;

	setMemory({
		{0x1004, 0x2_dw},
		{0x1008, 0x4_dw},
		{0x100c, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_R0, 0x1000},
	});

	emulate("ldmib r0!, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x100c},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1004, 0x1008, 0x100c});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDMDA - ARM only
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDMDA)
{
	SKIP_MODE_THUMB;

	setMemory({
		{0x1004, 0x2_dw},
		{0x1008, 0x4_dw},
		{0x100c, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_R0, 0x100c},
	});

	emulate("ldmda r0, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R2, 0x6},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x2},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x100c, 0x1008, 0x1004});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDMDA_wb)
{
	SKIP_MODE_THUMB;

	setMemory({
		{0x1004, 0x2_dw},
		{0x1008, 0x4_dw},
		{0x100c, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_R0, 0x100c},
	});

	emulate("ldmda r0!, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R2, 0x6},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x2},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x100c, 0x1008, 0x1004});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_LDMDB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDMDB)
{
	ALL_MODES;

	setMemory({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_R0, 0x100c},
	});

	emulate("ldmdb r0, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R2, 0x6},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x2},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1008, 0x1004, 0x1000});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDMDB_wb)
{
	ALL_MODES;

	setMemory({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});

	setRegisters({
		{ARM_REG_R0, 0x100c},
	});

	emulate("ldmdb r0!, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R2, 0x6},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x2},
	});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1008, 0x1004, 0x1000});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STM
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STM)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("stm r0, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STM_wb)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("stm r0!, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x100c},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STMIB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STMIB)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0xffc},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("stmib r0, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STMIB_wb)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0xffc},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("stmib r0!, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1008},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1008, 0x6_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STMDA
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STMDA)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x1008},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("stmda r0, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1000, 0x6_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STMDA_wb)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x1008},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("stmda r0!, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xffc},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x2_dw},
		{0x1004, 0x4_dw},
		{0x1000, 0x6_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_STMDB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STMDB)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x100c},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("stmdb r0, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x6_dw},
		{0x1004, 0x4_dw},
		{0x1000, 0x2_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_STMDB_wb)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x100c},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("stmdb r0!, {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1000},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x6_dw},
		{0x1004, 0x4_dw},
		{0x1000, 0x2_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_PUSH
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_PUSH)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_SP, 0x100c},
		{ARM_REG_R2, 0x2},
		{ARM_REG_R4, 0x4},
		{ARM_REG_R6, 0x6},
	});

	emulate("push {r2, r4, r6}");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_SP, ARM_REG_R2, ARM_REG_R4, ARM_REG_R6});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_SP, 0x1000},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x6_dw},
		{0x1004, 0x4_dw},
		{0x1000, 0x2_dw},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UMULL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UMULL)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
	});

	emulate("umull r0, r1, r2, r3"); // -> 0a 49 a8 3e | 2a 42 d2 08

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x2a42d208}, // lo
		{ARM_REG_R1, 0x0a49a83e}, // hi
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UMULL_s)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
	});

	emulate("umulls r0, r1, r2, r3"); // -> 0a 49 a8 3e | 2a 42 d2 08

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x2a42d208}, // lo
		{ARM_REG_R1, 0x0a49a83e}, // hi
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_SMULL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SMULL)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
	});

	emulate("smull r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x2a42d208}, // lo
		{ARM_REG_R1, 0xF81551C6}, // hi
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SMULL_s)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
	});

	emulate("smulls r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x2a42d208}, // lo
		{ARM_REG_R1, 0xF81551C6}, // hi
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UMLAL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UMLAL)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
	});

	emulate("umlal r0, r1, r2, r3"); // -> 0a 49 b8 3e | 2a 42 e2 08

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x2a42e208}, // lo
		{ARM_REG_R1, 0x0a49b83e}, // hi
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UMLAL_s)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
	});

	emulate("umlals r0, r1, r2, r3"); // -> 0a 49 b8 3e | 2a 42 e2 08

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x2a42e208}, // lo
		{ARM_REG_R1, 0x0a49b83e}, // hi
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}
//
// ARM_INS_SMLAL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SMLAL)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
	});

	emulate("smlal r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x2a42e208}, // lo
		{ARM_REG_R1, 0xF81561C6}, // hi
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SMLAL_s)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x90abcdef},
	});

	emulate("smlals r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x2a42e208}, // lo
		{ARM_REG_R1, 0xF81561C6}, // hi
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UMAAL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UMAAL)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x4321},
		{ARM_REG_R1, 0x1234},
		{ARM_REG_R2, 0x5678},
		{ARM_REG_R3, 0x9abc},
	});

	emulate("umaal r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, ANY},
		{ARM_REG_R1, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_umaal"), {0x4321, 0x1234, 0x5678, 0x9abc}},
	});
}

//
// ARM_INS_MLS
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MLS)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x5},
		{ARM_REG_R3, 0x20000},
	});

	emulate("mls r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1b000}, // 0x20000 - 0x1000 * 0x5 = 0x1b000
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_MLA
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MLA)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x5},
		{ARM_REG_R3, 0x20000},
	});

	emulate("mla r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x25000}, // 0x20000 + 0x1000 * 0x5 = 0x25000
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MLA_s)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x5},
		{ARM_REG_R3, 0x20000},
	});

	emulate("mlas r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x25000}, // 0x20000 + 0x1000 * 0x5 = 0x25000
		{ARM_REG_CPSR_N, false},
		{ARM_REG_CPSR_Z, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_BFC
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BFC)
{
	// Clear bits 5..20. 0x1234 keeps only its bottom five bits, 0x14.
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1234},
	});

	emulate("bfc r0, #0x5, #0x10");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_BFI
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BFI)
{
	// Insert r1's low 16 bits at bit 5 and keep everything of r0 outside that
	// window: 0x14 from below the field, 0xacf00 from r1 shifted into it.
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1234},
		{ARM_REG_R1, 0x5678},
	});

	emulate("bfi r0, r1, #0x5, #0x10");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xacf14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UXTAH
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UXTAH)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R0, 0x1234},
		{ARM_REG_R1, 0x12345678},
	});

	emulate("uxtah r3, r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R3, 0x1234 + 0x5678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UXTB
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UXTB)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("uxtb r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x78},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UXTB16
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UXTB16)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("uxtb16 r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x00340078},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UXTH
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UXTH)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("uxth r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x00005678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_SVC
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SVC)
{
	ONLY_MODE_ARM;

	emulate("svc 0x123");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_svc"), {0x123}},
	});
}

//
// Tests for generic pseudo assembly generation.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, MVN_ternary)
{
	ONLY_MODE_ARM;

	emulate("mvn r1, #44, #4");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R1, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_mvn"), {44, 4}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, DmbEmitsFence)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("dmb sy"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<FenceInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, DsbEmitsFence)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("dsb sy"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<FenceInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, IsbEmitsFence)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("isb"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<FenceInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, AND_quaternary)
{
	ONLY_MODE_ARM;

	setRegisters({
		{ARM_REG_R8, 0x1234},
	});

	emulate("and r2, r8, #0, #2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R8});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R2, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_and"), {0x1234, 0, 2}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, PushStoreAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("push {r2, r4, r6}"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, PopLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("pop {r2, r4, r6}"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdmLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldm r0, {r2, r4, r6}"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StmStoreAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("stm r0, {r2, r4, r6}"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, LdrdLoadAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("ldrd r0, r1, [r2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, StrdStoreAttachesPointeeMetadata)
{
	ONLY_MODE_ARM;
	auto* f = translate(assemble("strd r0, r1, [r2]"));
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

//
// Scalar VFP.
//
// All 149 ARM_INS_V* entries were nullptr, so every floating-point instruction
// on 32-bit ARM was an opaque __asm_* call. These cover the set a C compiler
// emits for float and double, and the last one covers what must NOT happen:
// a NEON instruction must not be translated as if it were scalar.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VLDR_d)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});
	setMemory({
		{0x1000, 3.5_f64},
	});

	emulate("vldr d0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 3.5_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VSTR_d)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_D0, 3.5_f64},
	});

	emulate("vstr d0, [r1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_D0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 3.5_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VSTR_s_is_four_bytes)
{
	ALL_MODES;

	// An S register is 32 bits. Storing it as 8 would be the mistake worth
	// catching, so the expectation pins the width, not just the value.
	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_S0, 1.5_f32},
	});

	emulate("vstr s0, [r1]");

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 1.5_f32},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VADD_f64)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, 1.5_f64},
		{ARM_REG_D2, 2.25_f64},
	});

	emulate("vadd.f64 d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_D1, ARM_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 3.75_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VSUB_f64)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, 5.5_f64},
		{ARM_REG_D2, 2.25_f64},
	});

	emulate("vsub.f64 d0, d1, d2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 3.25_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VMUL_f32)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_S1, 1.5_f32},
		{ARM_REG_S2, 4.0_f32},
	});

	emulate("vmul.f32 s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_S1, ARM_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_S0, 6.0_f32},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VDIV_f64)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, 9.0_f64},
		{ARM_REG_D2, 4.0_f64},
	});

	emulate("vdiv.f64 d0, d1, d2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 2.25_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VNEG_f64)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, 3.5_f64},
	});

	emulate("vneg.f64 d0, d1");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, -3.5},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VABS_f64)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, -3.5},
	});

	emulate("vabs.f64 d0, d1");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 3.5_f64},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VSQRT_f64)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, 16.0_f64},
	});

	emulate("vsqrt.f64 d0, d1");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 4.0_f64},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCVT_f64_s32)
{
	ALL_MODES;

	// The source S register holds a signed integer's BIT PATTERN, not a
	// number: vector_data is F64S32 and that is the only thing that says so.
	float src;
	std::int32_t bits = -5;
	std::memcpy(&src, &bits, sizeof(src));

	setRegisters({
		{ARM_REG_S1, src},
	});

	emulate("vcvt.f64.s32 d0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, -5.0},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCVT_s32_f64)
{
	ALL_MODES;

	// And back: the destination S register receives a bit pattern.
	float expected;
	std::int32_t bits = 3;
	std::memcpy(&expected, &bits, sizeof(expected));

	setRegisters({
		{ARM_REG_D1, 3.7_f64},
	});

	emulate("vcvt.s32.f64 s0, d1");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_S0, expected},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCVT_f32_f64)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, 2.5_f64},
	});

	emulate("vcvt.f32.f64 s0, d1");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_S0, 2.5_f32},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VMOV_s_from_gpr_is_a_bit_move)
{
	ALL_MODES;

	// `vmov s0, r1` moves the bits. If it converted the number instead, s0
	// would hold 1078530011.0f rather than the float those bits spell.
	float expected;
	std::uint32_t bits = 0x40490fdb; // float pi
	std::memcpy(&expected, &bits, sizeof(expected));

	setRegisters({
		{ARM_REG_R1, 0x40490fdb},
	});

	emulate("vmov s0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_S0, expected},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VMOV_gpr_from_s_is_a_bit_move)
{
	ALL_MODES;

	float src;
	std::uint32_t bits = 0x40490fdb;
	std::memcpy(&src, &bits, sizeof(src));

	setRegisters({
		{ARM_REG_S1, src},
	});

	emulate("vmov r0, s1");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x40490fdb},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VMOV_d_from_gpr_pair_is_low_first)
{
	ALL_MODES;

	// `vmov d0, r0, r1` puts r0 in the low half. Measured from a real build:
	// gcc emits `ldrd r0, r1, [...]` and then this, so r0 is the low word.
	double expected;
	std::uint64_t bits = 0x3ff0000000000000ULL; // 1.0
	std::memcpy(&expected, &bits, sizeof(expected));

	setRegisters({
		{ARM_REG_R0, 0x00000000},
		{ARM_REG_R1, 0x3ff00000},
	});

	emulate("vmov d0, r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, expected},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_FCONSTD, ARM_INS_FCONSTS
//
// The VFP move-immediate. Capstone prints both as "vmov" but gives them ids of
// their own, and neither id was a key in the dispatch table -- not `nullptr`,
// absent -- which is why COV-01 reported `<id 52> NO ENTRY` instead of naming
// an unimplemented instruction.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_FCONSTD_loads_the_immediate)
{
	ALL_MODES;

	emulate("vmov.f64 d0, #2.5");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 2.5_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_FCONSTS_loads_the_immediate)
{
	ALL_MODES;

	// Negative, because the VFP modified immediate has a sign bit and dropping
	// it is the obvious way to get this wrong.
	emulate("vmov.f32 s0, #-1.5");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_S0, -1.5f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCMP_f64_packs_fpscr)
{
	ALL_MODES;

	// vcmp writes FPSCR[31:28]. For 1.0 < 2.0 the ARM flags are N=1 (less
	// than), Z=0, C=0 (not greater-or-equal, not unordered), V=0 (ordered),
	// which packs to 0x80000000. These are not the integer compare's flags:
	// C and V mean different things here.
	setRegisters({
		{ARM_REG_D0, 1.0_f64},
		{ARM_REG_D1, 2.0_f64},
	});

	emulate("vcmp.f64 d0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_D0, ARM_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_FPSCR_NZCV, 0x80000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCMP_f64_unordered)
{
	ALL_MODES;

	// A NaN on either side is unordered: V is set and C with it, and N and Z
	// are clear. Nothing else distinguishes this from an ordinary compare, so
	// without it the V bit could be wired to anything.
	setRegisters({
		{ARM_REG_D0, std::numeric_limits<double>::quiet_NaN()},
		{ARM_REG_D1, 2.0_f64},
	});

	emulate("vcmp.f64 d0, d1");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_FPSCR_NZCV, 0x30000000},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VMRS_unpacks_into_cpsr)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_FPSCR_NZCV, 0x80000000},
	});

	emulate("vmrs APSR_nzcv, fpscr");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_FPSCR_NZCV});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VADD_i32_is_neon_and_stays_a_pseudo_call)
{
	ALL_MODES;

	// `vadd.i32 d0, d1, d2` is two 32-bit lane adds, not one f64 add. The
	// registers are the same D registers the scalar form uses, so only
	// cs_arm::vector_data tells them apart. Translating this as a scalar
	// float add would be silently wrong, which is the whole reason the guard
	// exists.
	auto* f = translate(assemble("vadd.i32 d0, d1, d2"));
	ASSERT_NE(nullptr, f);
	// getPseudoAsmFunction() names the function after the mnemonic, and the
	// mnemonic carries the type suffix: "__asm_vadd.i32", not "__asm_vadd".
	EXPECT_NE(nullptr, _module.getFunction("__asm_vadd.i32"));
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(
			isa<llvm::BinaryOperator>(&*it) && cast<llvm::BinaryOperator>(&*it)->getOpcode() == llvm::Instruction::FAdd)
			<< "NEON vadd.i32 was translated as a scalar float add";
	}
}


//
// ARM_INS_UBFX, ARM_INS_SBFX, ARM_INS_SXTB, ARM_INS_SXTH
//
// None of these had a test. UBFX is 1,898 occurrences in the static corpus and
// SXTH 805, and all four were reaching a pseudo-assembly call.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UBFX_extracts_zero_extended)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345678},
	});

	emulate("ubfx r0, r1, #8, #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x56},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SBFX_extracts_sign_extended)
{
	// Same field position as UBFX above and a field whose top bit is set, so
	// the two answer 0xffffff80 and 0x80 -- the only thing that separates them.
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12348078},
	});

	emulate("sbfx r0, r1, #8, #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xffffff80},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SXTB_sign_extends_a_byte)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12345680},
	});

	emulate("sxtb r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xffffff80},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SXTH_sign_extends_a_halfword)
{
	// UXTH next door answers 0x8000 for this input; the S is the whole
	// instruction.
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x12348000},
	});

	emulate("sxth r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xffff8000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}


//
// ARM_INS_MRC
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MRC_p15_c13_c0_3_is_the_thread_pointer)
{
	// 11,697 occurrences in the static corpus and every one of them this exact
	// encoding: it is how glibc finds thread-local storage on ARM. They were
	// all becoming __asm_mrc(15, 0, 13, 0, 3).
	ALL_MODES;

	setRegisters({
		{ARM_REG_TPIDRURO, 0xdeadbeef},
	});

	emulate("mrc p15, #0, r0, c13, c0, #3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_TPIDRURO});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xdeadbeef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_MRC_any_other_coprocessor_read_stays_opaque)
{
	// c0, c0, 0 is MIDR, the main ID register. A coprocessor read really is
	// opaque; the point of the translation above is that one encoding is not,
	// so this checks that the six immediates are being looked at rather than
	// the mnemonic.
	ALL_MODES;

	emulate("mrc p15, #0, r0, c0, c0, #0");

	// translatePseudoAsmGeneric() passes every operand as an argument and
	// returns void, so r0 is read rather than written -- which is its own
	// reason for wanting the encoding above translated properly.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_mrc"), {15, 0, 0, 0, 0, 0}},
	});
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
