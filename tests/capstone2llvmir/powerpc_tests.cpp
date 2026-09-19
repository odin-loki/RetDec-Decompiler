/**
 * @file tests/capstone2llvmir/powerpc_tests.cpp
 * @brief Capstone2LlvmIrTranslatorPowerpc unit tests.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <llvm/IR/InstIterator.h>

#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/powerpc/powerpc.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorPowerpcTests :
		public Capstone2LlvmIrTranslatorTests,
		public ::testing::WithParamInterface<cs_mode>
{
	protected:
		virtual void initKeystoneEngine() override
		{
			ks_mode mode = KS_MODE_PPC32;
			switch(GetParam())
			{
				case CS_MODE_32: mode = KS_MODE_PPC32; break;
				case CS_MODE_64: mode = KS_MODE_PPC64; break;
				case CS_MODE_QPX: mode = KS_MODE_QPX; break;
				default: throw std::runtime_error("ERROR: unknown mode.\n");
			}
			if (ks_open(KS_ARCH_PPC, mode | KS_MODE_BIG_ENDIAN, &_assembler) != KS_ERR_OK)
			{
				throw std::runtime_error("ERROR: failed on ks_open().\n");
			}
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			switch(GetParam())
			{
				case CS_MODE_32:
					_translator = Capstone2LlvmIrTranslator::createPpc32(
							&_module,
							CS_MODE_BIG_ENDIAN);
					break;
				case CS_MODE_64:
					_translator = Capstone2LlvmIrTranslator::createPpc64(
							&_module,
							CS_MODE_BIG_ENDIAN);
					break;
				case CS_MODE_QPX:
					_translator = Capstone2LlvmIrTranslator::createPpcQpx(
							&_module,
							CS_MODE_BIG_ENDIAN);
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
#define ONLY_MODE_32 if (GetParam() != CS_MODE_32) return;
#define ONLY_MODE_64 if (GetParam() != CS_MODE_64) return;
#define ONLY_MODE_QPX if (GetParam() != CS_MODE_QPX) return;
#define SKIP_MODE_32 if (GetParam() == CS_MODE_32) return;
#define SKIP_MODE_64 if (GetParam() == CS_MODE_64) return;
#define SKIP_MODE_QPX if (GetParam() == CS_MODE_QPX) return;
		void setV(uint32_t reg, uint64_t hi, uint64_t lo)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			llvm::GenericValue v = _emulator->getGlobalVariableValue(gv);
			const uint64_t words[2] = {lo, hi};
			v.IntVal = llvm::APInt(128, llvm::ArrayRef<uint64_t>(words, 2));
			_emulator->setGlobalVariableValue(gv, v);
		}

		uint64_t vLow(uint32_t reg)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			return _emulator->getGlobalVariableValue(gv).IntVal.trunc(64).getZExtValue();
		}

		uint64_t vHigh(uint32_t reg)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			return _emulator->getGlobalVariableValue(gv).IntVal.lshr(64).trunc(64).getZExtValue();
		}
};

struct PrintCapstoneModeToString_Powerpc
{
	template <class ParamType>
	std::string operator()(const TestParamInfo<ParamType>& info) const
	{
		switch (info.param)
		{
			case CS_MODE_16: return "CS_MODE_16";
			case CS_MODE_32: return "CS_MODE_32";
			case CS_MODE_64: return "CS_MODE_64";
			case CS_MODE_QPX: return "CS_MODE_QPX";
			default: return "UNHANDLED CS_MODE";
		}
	}
};

// By default, all the test cases are run with all the modes.
// If some test case is not meant for all modes, use some of the ONLY_MODE_*,
// SKIP_MODE_* macros.
//
INSTANTIATE_TEST_SUITE_P(
		InstantiatePowerpcWithAllModes,
		Capstone2LlvmIrTranslatorPowerpcTests,
		::testing::Values(CS_MODE_32, CS_MODE_64),
		 PrintCapstoneModeToString_Powerpc());

//
// PPC_INS_ADD
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADD)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x1111},
		{PPC_REG_R1, 0x2222},
	});

	emulate("add 0, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x3333},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADD_bin)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R0, 0x1111},
		{PPC_REG_R1, 0x2222},
	});

	emulate_bin("7c 00 0a 14");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x3333},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADD_dot_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x0},
		{PPC_REG_R1, 0x0},
	});

	emulate("add. 0, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADD_dot_negative)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R0, 0xffff0000},
		{PPC_REG_R1, 0x00001234},
	});

	emulate("add. 0, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, ANY},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADD_dot_postitive)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x1111},
		{PPC_REG_R1, 0x2222},
	});

	emulate("add. 0, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x3333},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ADDI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDI)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1111},
	});

	emulate("addi 0, 1, 0x2222");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x3333},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LA
// 1. and 2. operands are reversed, but it probbaly does not matter.
// la 0, 0x4, 1 (reg, imm, reg) == addi 0, 1, 0x4 (reg, reg, imm)
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LA)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1111},
	});

	emulate("la 0, 0x2222, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x3333},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ADDIS
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDIS)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1111},
	});

	emulate("addis 0, 1, 0x2222");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x22221111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ADDIC
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDIC_32_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
	});

	emulate("addic 0, 1, 0xff");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfe}, // 0xffffffff + 0xff = 0x01 | 00 00 00 fe
		{PPC_REG_CARRY, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDIC_64_true)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xffffffffffffffff},
	});

	emulate("addic 0, 1, 0xff");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfe}, // 0xffffffffffffffff + 0xff = 0x01 | 00 00 00 00 00 00 00 fe
		{PPC_REG_CARRY, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDIC_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1200},
	});

	emulate("addic 0, 1, 0x34");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDIC_dot_32_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
	});

	emulate("addic. 0, 1, 0xff");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfe}, // 0xffffffff + 0xff = 0x01 | 00 00 00 fe
		{PPC_REG_CARRY, true},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ADDC
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDC_32_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0xff},
	});

	emulate("addc 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfe}, // 0xffffffff + 0xff = 0x01 | 00 00 00 fe
		{PPC_REG_CARRY, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDC_64_true)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xffffffffffffffff},
		{PPC_REG_R2, 0xff},
	});

	emulate("addc 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfe}, // 0xffffffffffffffff + 0xff = 0x01 | 00 00 00 00 00 00 00 fe
		{PPC_REG_CARRY, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDC_dot_32_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0xff},
	});

	emulate("addc. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfe}, // 0xffffffff + 0xff = 0x01 | 00 00 00 fe
		{PPC_REG_CARRY, true},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ADDE
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDE_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_R2, 0x5670},
		{PPC_REG_CARRY, true},
	});

	emulate("adde 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345671},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDE_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_R2, 0x5670},
		{PPC_REG_CARRY, false},
	});

	emulate("adde 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345670},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDE_dot_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_R2, 0x5670},
		{PPC_REG_CARRY, true},
	});

	emulate("adde. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345671},
		{PPC_REG_CARRY, false},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ADDZE
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDZE_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_CARRY, true},
	});

	emulate("addze 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12340001},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDZE_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_CARRY, false},
	});

	emulate("addze 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12340000},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDZE_dot_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_CARRY, true},
	});

	emulate("addze. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12340001},
		{PPC_REG_CARRY, false},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ADDME
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDME_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_CARRY, true},
	});

	emulate("addme 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12340000},
		{PPC_REG_CARRY, true}, // TODO: I'm not sure about this, check it somehow.
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDME_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340001},
		{PPC_REG_CARRY, false},
	});

	emulate("addme 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12340000},
		{PPC_REG_CARRY, true}, // TODO: I'm not sure about this, check it somehow.
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ADDME_dot_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_CARRY, true},
	});

	emulate("addme. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12340000},
		{PPC_REG_CARRY, true}, // TODO: I'm not sure about this, check it somehow.
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_AND
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_AND)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x10203040},
	});

	emulate("and 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x10203040},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_AND_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x10203040},
	});

	emulate("and. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x10203040},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ANDI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ANDI_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
	});

	emulate("andi. 0, 1, 0xf0f0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xf0f0},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ANDC
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ANDC)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("andc 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xf0f0f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ANDC_dot)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("andc. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xf0f0f0f0},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ANDIS
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ANDIS_dot)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
	});

	emulate("andis. 0, 1, 0xffff");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffff0000},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_OR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_OR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("or 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1f3f0f0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_OR_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("or. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1f3f0f0f},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ORI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ORI)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
	});

	emulate("ori 0, 1, 0xf0f0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ORC
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ORC)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xf0f0f0f0},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("andc 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xf0f0f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ORC_dot)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xf0f0f0f0},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("orc. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xf0f0f0f0},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ORIS
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ORIS)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x10203456},
	});

	emulate("oris 0, 1, 0xffff");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffff3456},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_XOR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XOR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("xor 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xf0f0f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XOR_dot)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("xor. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xf0f0f0f0},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_XORI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XORI)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1234ffff},
	});

	emulate("xori 0, 1, 0xf0f0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12340f0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_XORIS
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XORIS)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x10203456},
	});

	emulate("xoris 0, 1, 0xffff");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xefdf3456},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_NOR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NOR_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("nor 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xe0c0f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NOR_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("nor 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffffffe0c0f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NOR_dot_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12340000},
		{PPC_REG_R2, 0x0f0f0f0f},
	});

	emulate("nor. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xe0c0f0f0},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_NOT
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NOT)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("not 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xEDCBA987},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_NOP
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NOP)
{
	ALL_MODES;

	emulate("nop");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_NEG
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NEG_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("neg 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xedcba988},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NEG_dot_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("neg. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xedcba988},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NEG_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("neg 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfedcba9876543211},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NEG_dot_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("neg. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfedcba9876543211},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_NAND
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NAND_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x10203040},
	});

	emulate("nand 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xefdfcfbf},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NAND_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x10203040},
	});

	emulate("nand 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffffffefdfcfbf},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_NAND_dot)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
		{PPC_REG_R2, 0x10203040},
	});

	emulate("nand. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xefdfcfbf},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SUBF
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBF)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x1111},
	});

	emulate("subf 0, 2, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBF_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x2222},
	});

	emulate("subf. 0, 2, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SUBFC
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFC_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x1111},
	});

	emulate("subfc 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffeeef},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFC_dot_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x1111},
	});

	emulate("subfc. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffeeef},
		{PPC_REG_CARRY, false},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SUBFIC
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFIC_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
	});

	emulate("subfic 0, 1, 0x1111");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffeeef},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SUBFE
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFE_32_carry_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x1111},
		{PPC_REG_CARRY, true},
	});

	emulate("subfe 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffeeef},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFE_32_carry_false)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x1111},
		{PPC_REG_CARRY, false},
	});

	emulate("subfe 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffeeee},
		{PPC_REG_CARRY, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFE_dot_32_carry_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x1111},
		{PPC_REG_CARRY, true},
	});

	emulate("subfe. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffeeef},
		{PPC_REG_CARRY, false},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SUBFME
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFME_32_carry_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_CARRY, true},
	});

	emulate("subfme 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffdddd},
		{PPC_REG_CARRY, true}, // ???
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFME_32_carry_false)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_CARRY, false},
	});

	emulate("subfme 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffdddc},
		{PPC_REG_CARRY, true}, // ???
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFME_32_dot_carry_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_CARRY, true},
	});

	emulate("subfme. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffdddd},
		{PPC_REG_CARRY, true}, // ???
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SUBFZE
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFZE_32_carry_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_CARRY, true},
	});

	emulate("subfze 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffddde},
		{PPC_REG_CARRY, false}, // ???
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFZE_32_carry_false)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_CARRY, false},
	});

	emulate("subfze 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffdddd},
		{PPC_REG_CARRY, false}, // ???
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFZE_dot_32_carry_true)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_CARRY, true},
	});

	emulate("subfze. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_CARRY});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffddde},
		{PPC_REG_CARRY, false}, // ???
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MULLI
//

// mulli multiplies the WHOLE register, not its low word -- there is no
// "mulliw". The test below uses 0x2222, which fits in a word, so the narrowed
// and un-narrowed readings agree on it and it passed either way.
// The record forms of the zero-extending word family set CR0 from the 64-bit
// REGISTER, not from the 32-bit word. Power ISA: for Rc=1 in 64-bit mode the
// whole register is compared, and rlwinm leaves RA[0:31] zero -- so LT can
// never be set, however the word looks. Comparing the word said LT=1, GT=0.
//
// There was no record-form test for any of the seven instructions in this
// family, which is why it survived. EQ is right at either width, and that is
// the bit compilers actually branch on after rlwinm., which is the rest of why.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLWINM_dot_64_compares_the_register)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x80000000},
	});

	emulate("rlwinm. 0, 1, 0, 0, 15");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x80000000},
		{PPC_REG_CR0LT, false}, // 0x0000000080000000 is positive
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
}

// And the sign-extending members of the same family must keep comparing as
// signed: srawi. leaves RA sign-extended, so a negative word IS a negative
// register and LT must be set. This is the case the fix must not break.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRAWI_dot_64_stays_signed)
{
	ONLY_MODE_64;

	// 0x8000000f, not 0x80000000: CA is set only when a NEGATIVE value loses
	// 1-bits off the bottom, and 0x80000000 >> 4 loses four zeros, so it would
	// assert CA = true against a correct CA = false. The low nibble here makes
	// the two conditions independent.
	setRegisters({
		{PPC_REG_R1, 0x8000000f},
	});

	emulate("srawi. 0, 1, 4");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfffffffff8000000},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
		{PPC_REG_CARRY, true},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULLI_64_is_not_a_word_multiply)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000000100000002},
	});

	emulate("mulli 0, 1, 3");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0000000300000006},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULLI)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x2222},
	});

	emulate("mulli 0, 1, 0x123");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x26cca6},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MULLW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULLW)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x123},
	});

	emulate("mullw 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x26cca6},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULLW_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x2222},
		{PPC_REG_R2, 0x123},
	});

	emulate("mullw. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x26cca6},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MULHW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULHW)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x11111111},
		{PPC_REG_R2, 0x22222222},
	});

	emulate("mulhw 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x02468acf}, // 0x02 46 8a cf | 0e ca 86 42
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULHW_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x11111111},
		{PPC_REG_R2, 0x22222222},
	});

	emulate("mulhw. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x02468acf}, // 0x02 46 8a cf | 0e ca 86 42
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MULHWU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULHWU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x11111111},
		{PPC_REG_R2, 0x22222222},
	});

	emulate("mulhwu 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x02468acf}, // 0x02 46 8a cf | 0e ca 86 42
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULHWU_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x11111111},
		{PPC_REG_R2, 0x22222222},
	});

	emulate("mulhwu. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x02468acf}, // 0x02 46 8a cf | 0e ca 86 42
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_DIVW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_DIVW)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0xabcd},
	});

	emulate("divw 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1b20},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_DIVW_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0xabcd},
	});

	emulate("divw. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1b20},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_DIVWU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_DIVWU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0xabcd},
	});

	emulate("divwu 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1b20},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_DIVWU_dot)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0xabcd},
	});

	emulate("divwu. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1b20},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_EQV
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EQV)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0x87654321},
	});

	emulate("eqv 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x6aaeeaa6},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EQV_dot)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0x87654321},
	});

	emulate("eqv. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x6aaeeaa6},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CNTLZW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CNTLZW_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xffffffffffffffff},
	});

	emulate("cntlzw 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CNTLZW_dot_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xffffffffffffffff},
	});

	emulate("cntlzw. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CNTLZW_non_zero_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x0000ffff},
	});

	emulate("cntlzw 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 16},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_EXTSB
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSB_zero_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678}, // last byte = 01111000
	});

	emulate("extsb 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x78},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSB_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x123456f8}, // last byte = 11111000
	});

	emulate("extsb 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfffffff8},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSB_zero_dot_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678}, // last byte = 01111000
	});

	emulate("extsb. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x78},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_EXTSH
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSH_zero_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12347856}, // last word = 01111000 ...
	});

	emulate("extsh 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x7856},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSH_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x1234f856}, // last word = 11111000 ...
	});

	emulate("extsh 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfffff856},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSH_zero_dot_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12347856}, // last word = 01111000 ...
	});

	emulate("extsh. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x7856},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_EXTSW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSW_zero_32)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1234567878123456}, // last dword = 01111000 ...
	});

	emulate("extsw 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x78123456},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSW_32)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x12345678f8123456}, // last dword = 11111000 ...
	});

	emulate("extsw 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfffffffff8123456},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_EXTSW_zero_dot_32)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1234567878123456}, // last dword = 01111000 ...
	});

	emulate("extsw. 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x78123456},
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_RLWINM
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLWINM)
{
	ALL_MODES;

	// A plain rotate: the full mask keeps every bit.
	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("rlwinm 0, 1, 8, 0, 31");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x34567812},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLWINM_extracts_a_field)
{
	// `rlwinm rA, rS, 0, 16, 31` is how GCC spells `(uint16_t) rS`, and it is
	// why this instruction is the most frequent unmodelled pseudo-asm call on
	// any of the five architectures.
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("rlwinm 0, 1, 0, 16, 31");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x5678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLWINM_bit_zero_is_the_top_bit)
{
	// PowerPC numbers bits from the most significant end, so 0..15 is the
	// HIGH half. Read the other way round this answers 0x5678.
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("rlwinm 0, 1, 0, 0, 15");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12340000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLWINM_mask_wraps_when_MB_exceeds_ME)
{
	// MB > ME is not an error: the mask is bits 24..31 and 0..7, which is
	// 0xff0000ff. An implementation that treated MB > ME as empty answers 0.
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("rlwinm 0, 1, 0, 24, 7");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12000078},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_RLWIMI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLWIMI)
{
	// The insert form is the only one that reads its destination: the bits
	// outside the mask are kept. Dropping that reads as 0x5678.
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0xaabbccdd},
		{PPC_REG_R1, 0x12345678},
	});

	emulate("rlwimi 0, 1, 0, 16, 31");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xaabb5678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_RLWNM
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLWNM)
{
	// Same as RLWINM but the rotate amount comes from a register.
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 8},
	});

	emulate("rlwnm 0, 1, 2, 0, 31");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x34567812},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLWNM_by_zero_is_the_source)
{
	// The one rotate amount for which the complementary shift would be by the
	// full operand width, which is poison, and which cannot be decided in C++
	// here because the amount is in a register.
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0},
	});

	emulate("rlwnm 0, 1, 2, 0, 31");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_RLDICL / RLDICR / RLDIMI and compiler aliases (ppc64 only).
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLDICL_rotates_the_doubleword)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("rldicl 0, 1, 8, 0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x23456789abcdef01},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLDICL_clears_the_left)
{
	// `clrldi rA, rS, 32` is `rldicl rA, rS, 0, 32`: the 64-bit (uint32_t) cast.
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("rldicl 0, 1, 0, 32");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0000000089abcdef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CLRLDI)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("clrldi 0, 1, 32");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0000000089abcdef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLDICL_is_srdi)
{
	// `srdi rA, rS, 8` is `rldicl rA, rS, 56, 8`.
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("rldicl 0, 1, 56, 8");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x000123456789abcd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ROTLDI)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("rotldi 0, 1, 8");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x23456789abcdef01},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLDICR_is_sldi)
{
	// `sldi rA, rS, 8` is `rldicr rA, rS, 8, 55`.
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("rldicr 0, 1, 8, 55");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x23456789abcdef00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SLDI)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
	});

	emulate("sldi 0, 1, 8");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x23456789abcdef00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_DIVD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x123456789abcdef0},
		{PPC_REG_R2, 0x10},
	});

	emulate("divd 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0123456789abcdef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_DIVDU)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xfffffffffffffffe},
		{PPC_REG_R2, 0x2},
	});

	emulate("divdu 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x7fffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULLD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000000100000002},
		{PPC_REG_R2, 0x3},
	});

	emulate("mulld 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0000000300000006},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULHD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000000100000000},
		{PPC_REG_R2, 0x0000000100000000},
	});

	emulate("mulhd 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULHDU)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xffffffffffffffff},
		{PPC_REG_R2, 0xffffffffffffffff},
	});

	emulate("mulhdu 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xfffffffffffffffe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SLD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
		{PPC_REG_R2, 8},
	});

	emulate("sld 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x23456789abcdef00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
		{PPC_REG_R2, 4},
	});

	emulate("srd 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x00123456789abcde},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRADI)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xf00000000000000f},
	});

	emulate("sradi 0, 1, 4");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xff00000000000000},
		{PPC_REG_CARRY, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRAD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xf00000000000000f},
		{PPC_REG_R2, 4},
	});

	emulate("srad 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xff00000000000000},
		{PPC_REG_CARRY, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CNTLZD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000ffffffffffff},
	});

	emulate("cntlzd 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 16},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_POPCNTD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0f0f0f0f0f0f0f0f},
	});

	emulate("popcntd 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLDIMI_inserts_the_low_word)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R0, 0xaaaaaaaaaaaaaaaa},
		{PPC_REG_R1, 0x1111111122222222},
	});

	emulate("rldimi 0, 1, 0, 32");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xaaaaaaaa22222222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ROTLD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0123456789abcdef},
		{PPC_REG_R2, 8},
	});

	emulate("rotld 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x23456789abcdef01},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_RLDICL_dot_compares_the_register)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x8000000000000000},
	});

	emulate("rldicl. 0, 1, 0, 0");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x8000000000000000},
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// The count at or past the width, the carry out of SUBFE, and the carry out
// of SRAW. All three were confirmed from the code before being fixed; see
// docs/internal/UNFIXED_AUDIT_FINDINGS.md, Batch AH.
//

//
// The `w` instructions work on the low WORD whatever the register width.
// These are ONLY_MODE_64 because that is the mode they were wrong in; the
// 32-bit answers were always right. See Batch AL.
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CNTLZW_counts_within_the_word)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000000000000001},
	});

	emulate("cntlzw 0, 1");

	// 31 leading zeros within the word. Instantiating llvm.ctlz on the i64
	// register counted the 32 zero bits above the word as well and said 63.
	EXPECT_EQ(31, getRegisterValueUnsigned(PPC_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULLW_multiplies_the_low_words)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000000100000002},
		{PPC_REG_R2, 0x0000000000000003},
	});

	emulate("mullw 0, 1, 2");

	// The 64-bit product of the low words: 2 * 3. The register-width multiply
	// answered 0x0000000300000006.
	EXPECT_EQ(6, getRegisterValueUnsigned(PPC_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MULHW_takes_the_high_word_of_the_word_product)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000000100000002},
		{PPC_REG_R2, 0x0000000000000002},
	});

	emulate("mulhw 0, 1, 2");

	// 2 * 2 is 4, whose high word is 0. SExtOrTrunc to i64 is a no-op when
	// the operand is already i64, so this was a 64x64 multiply and answered 2.
	EXPECT_EQ(0, getRegisterValueUnsigned(PPC_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_DIVW_divides_the_low_words)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000000100000000},
		{PPC_REG_R2, 0x0000000000000001},
	});

	emulate("divw 0, 1, 2");

	// The low word of r1 is zero, so the quotient is zero. Dividing at
	// register width answered 0x0000000100000000.
	EXPECT_EQ(0, getRegisterValueUnsigned(PPC_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRAWI_shifts_within_the_word)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x00000000ffffffff},
	});

	emulate("srawi 0, 1, 4");

	// The word is -1, so an arithmetic shift leaves it -1 and the register
	// takes the sign extension. The body is a hand-unrolled 32-bit rotate --
	// its constants are 31 and 32 -- and it was being applied to an i64.
	EXPECT_EQ(0xffffffffffffffffULL, getRegisterValueUnsigned(PPC_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SLW_count_past_the_width_is_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x00000001}, {PPC_REG_R2, 0x00000020}, // 32: six bits read, but bit 5 means zero
	});

	emulate("slw 0, 1, 2");

	// `slw` reads six bits of the count, and the sixth selects a result of
	// ZERO rather than participating in the shift. The old code shifted an
	// i32 by up to 63, which is poison.
	EXPECT_EQ(0x0, getRegisterValueUnsigned(PPC_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRW_count_past_the_width_is_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0xffffffff}, {PPC_REG_R2, 0x0000003f}, // 63, the largest the six bits can hold
	});

	emulate("srw 0, 1, 2");

	EXPECT_EQ(0x0, getRegisterValueUnsigned(PPC_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SUBFE_carry_is_out_of_the_complemented_sum)
{
	SKIP_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x00000001},
		{PPC_REG_R2, 0x00000005},
		{PPC_REG_CARRY, true},
	});

	emulate("subfe 0, 1, 2");

	// subfe RT,RA,RB is ~RA + RB + CA = 0xfffffffe + 5 + 1, which carries
	// out. The value was already right; the carry was computed from RA
	// rather than ~RA, so 1 + 5 + 1 does not overflow and CA came out 0.
	EXPECT_EQ(0x4, getRegisterValueUnsigned(PPC_REG_R0));
	EXPECT_EQ(1, getRegisterValueUnsigned(PPC_REG_CARRY));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRAWI_sets_the_carry)
{
	SKIP_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xffffffff},
	});

	emulate("srawi 0, 1, 4");

	// CA is set when RS was negative and a 1-bit was shifted out. This used
	// to place the answer at XER bit 29 and store that into PPC_REG_CARRY,
	// which is an i1 -- so it truncated to bit 0 and CA was always false.
	EXPECT_EQ(0xffffffff, getRegisterValueUnsigned(PPC_REG_R0));
	EXPECT_EQ(1, getRegisterValueUnsigned(PPC_REG_CARRY));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRAWI_positive_never_sets_the_carry)
{
	SKIP_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x0000000f},
	});

	emulate("srawi 0, 1, 4");

	// Bits ARE shifted out here, and CA must still be zero: the rule is that
	// RS was negative AND a 1-bit was lost. Testing only the losing half
	// passes both readings, which is what the first version of these tests
	// did -- a mutation dropping the sign condition came back green.
	EXPECT_EQ(0x0, getRegisterValueUnsigned(PPC_REG_R0));
	EXPECT_EQ(0, getRegisterValueUnsigned(PPC_REG_CARRY));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRAWI_clears_the_carry_when_nothing_is_lost)
{
	SKIP_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0xfffffff0},
	});

	emulate("srawi 0, 1, 4");

	// Negative, but the four bits shifted out are all zero.
	EXPECT_EQ(0xffffffff, getRegisterValueUnsigned(PPC_REG_R0));
	EXPECT_EQ(0, getRegisterValueUnsigned(PPC_REG_CARRY));
}

//
// PPC_INS_SLW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SLW)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0x12345690}, // last byte = 10|010000 = 144 -> (6 bits) 16
	});

	emulate("slw 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x56780000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SRW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRW)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
		{PPC_REG_R2, 0x12345690}, // last byte = 10|010000 = 144 -> (6 bits) 16
	});

	emulate("srw 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x00001234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SRAW
//

// TODO
//TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRAW)
//{
//	ALL_MODES;
//
//	setRegisters({
//		{PPC_REG_R1, 0x1234},
//		{PPC_REG_R2, 0x5678},
//	});
//
//	emulate("sraw 0, 1, 2");
//
//	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
//	EXPECT_JUST_REGISTERS_STORED({
//		{PPC_REG_R0, ANY},
//		{PPC_REG_CARRY, ANY},
//	});
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_module.getFunction("__asm_sraw"), {0x1234, 0x5678}},
//	});
//}

//
// PPC_INS_SRAWI
//

// TODO
//TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRAWI)
//{
//	ALL_MODES;
//
//	setRegisters({
//		{PPC_REG_R1, 0x1234},
//	});
//
//	emulate("srawi 0, 1, 0xf");
//
//	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
//	EXPECT_JUST_REGISTERS_STORED({
//		{PPC_REG_R0, ANY},
//		{PPC_REG_CARRY, ANY},
//	});
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_module.getFunction("__asm_srawi"), {0x1234, 0xf}},
//	});
//}

//
// PPC_INS_MR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R11, 0x1234},
	});

	emulate("mr 0, 11");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R11});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MR_does_not_inherit_a_record_form_from_the_heap)
{
	// capstone's PPC post-printer reads insn->mnemonic BEFORE fill_insn()
	// writes it:
	//
	//     if (strrchr(insn->mnemonic, '.') != NULL)
	//         insn->detail->ppc.update_cr0 = true;
	//
	// and capstone2llvmir hands it a freshly cs_malloc'd buffer for every
	// instruction, so an uninitialised '.' in that buffer turns any PowerPC
	// instruction into a record form -- CR0 written, and conditional branches
	// reading it, on an instruction that sets no flags. The same four bytes
	// decode to `mr r0, r11` with update_cr0 = 0 from a cleared buffer and
	// update_cr0 = 1 from one holding "addc.".
	//
	// This is what made PPC_INS_MR above fail about one run in fourteen under
	// load, and pass every time in isolation: the failing runs were the ones
	// where the allocator returned a chunk with a dot in it.
	//
	// The priming below frees a chunk of exactly sizeof(cs_insn) carrying
	// "addc." at the mnemonic offset. glibc's tcache is LIFO per size class,
	// so cs_malloc's next allocation of that size gets it back. The assemble
	// step is done first so that nothing else allocates in between.
	auto bytes = assemble("mr 0, 11");

	void* poison = std::malloc(sizeof(cs_insn));
	ASSERT_NE(nullptr, poison);
	std::memset(poison, 0, sizeof(cs_insn));
	std::strcpy(static_cast<char*>(poison) + offsetof(cs_insn, mnemonic), "addc.");
	std::free(poison);

	setRegisters({
		{PPC_REG_R11, 0x1234},
	});

	_emulate(bytes);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R11});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MTCRF
//

// The source word is 0x12345678, whose eight nibbles are 1,2,3,4,5,6,7,8 --
// every condition register field gets a different value, so a field landing in
// the wrong place is visible rather than coincidentally right. Counted from the
// most significant end, field f is nibble f: CR0 is 0x1 = LT,GT,EQ,SO 0,0,0,1
// and CR3 is 0x4 = 0,1,0,0.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MTCRF)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("mtcrf 0xf0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, true},
		{PPC_REG_CR1LT, false},
		{PPC_REG_CR1GT, false},
		{PPC_REG_CR1EQ, true},
		{PPC_REG_CR1UN, false},
		{PPC_REG_CR2LT, false},
		{PPC_REG_CR2GT, false},
		{PPC_REG_CR2EQ, true},
		{PPC_REG_CR2UN, true},
		{PPC_REG_CR3LT, false},
		{PPC_REG_CR3GT, true},
		{PPC_REG_CR3EQ, false},
		{PPC_REG_CR3UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// CRM is numbered from the most significant end: 0x08 is 0x80 >> 4 and selects
// CR4 alone. Read as `1 << f` it would select CR3, so this distinguishes the
// two directions -- and EXPECT_JUST_REGISTERS_STORED insists nothing else is
// written, which a mask ignored altogether would fail.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MTCRF_one_field)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("mtcrf 0x8, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CR4GT, true},
		{PPC_REG_CR4EQ, false},
		{PPC_REG_CR4UN, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// `mtcr rS` is `mtcrf 0xff, rS`, and capstone reports it as one: PPC_INS_MTCR
// is in the instruction enum but no encoding decodes to it, which is why the
// translator it was dispatched to could never run. The low nibble of the word
// is CR7, so this is also the check that the word is not read backwards.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MTCR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("mtcr 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, false}, {PPC_REG_CR0GT, false}, {PPC_REG_CR0EQ, false}, {PPC_REG_CR0UN, true},
		{PPC_REG_CR1LT, false}, {PPC_REG_CR1GT, false}, {PPC_REG_CR1EQ, true},  {PPC_REG_CR1UN, false},
		{PPC_REG_CR2LT, false}, {PPC_REG_CR2GT, false}, {PPC_REG_CR2EQ, true},  {PPC_REG_CR2UN, true},
		{PPC_REG_CR3LT, false}, {PPC_REG_CR3GT, true},  {PPC_REG_CR3EQ, false}, {PPC_REG_CR3UN, false},
		{PPC_REG_CR4LT, false}, {PPC_REG_CR4GT, true},  {PPC_REG_CR4EQ, false}, {PPC_REG_CR4UN, true},
		{PPC_REG_CR5LT, false}, {PPC_REG_CR5GT, true},  {PPC_REG_CR5EQ, true},  {PPC_REG_CR5UN, false},
		{PPC_REG_CR6LT, false}, {PPC_REG_CR6GT, true},  {PPC_REG_CR6EQ, true},  {PPC_REG_CR6UN, true},
		{PPC_REG_CR7LT, true},  {PPC_REG_CR7GT, false}, {PPC_REG_CR7EQ, false}, {PPC_REG_CR7UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MTCTR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MTCTR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R11, 0x1234},
	});

	emulate("mtctr 11");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R11});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MTLR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MTLR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R11, 0x1234},
	});

	emulate("mtlr 11");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R11});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CRAND
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CRAND)
{
	// `crand 1, 2, 3` is CR0GT = CR0EQ & CR0UN. Capstone reports the three
	// operands as the individual bit registers, which this translator has.
	//
	// The version this replaces asserted the old behaviour as if it were the
	// specification: all four CR0 bits written from a pseudo-assembly call,
	// plus the seven write-only CR1..CR7 `i4` registers. It was pinning a
	// clobber.
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, true},
	});

	emulate("crand 1, 2, 3");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0EQ, PPC_REG_CR0UN});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0GT, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CRAND_is_an_AND)
{
	// The same instruction with one input clear. Without this, a translation
	// that always wrote true would pass the test above.
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, false},
	});

	emulate("crand 1, 2, 3");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0GT, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CROR_reaches_CR7UN)
{
	// isCrBitRegister() read `PPC_REG_CR0EQ <= r && r <= PPC_REG_CR5UN`, and
	// Capstone groups its CR bit registers by bit NAME rather than by field --
	// EQ is 312..319, GT 320..327, LT 328..335, UN 336..343 -- so that range
	// covered thirty of the thirty-two and excluded CR6UN and CR7UN. This is
	// the bit it excluded.
	//
	// 0x4ffefb82 = cror cr7un, cr7eq, cr7un.
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR7EQ, true},
		{PPC_REG_CR7UN, false},
	});

	emulate_bin("4f fe fb 82");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR7UN, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CROR_on_CR7_leaves_CR0_alone)
{
	// Bits 28..31 are CR7. `cror 28, 29, 28` does not touch CR0, and the
	// translation it replaces overwrote all four of CR0's bits with the
	// results of an undefined function -- a wrong branch, not a missing
	// translation, for any code that compared into CR0 and then did CR-bit
	// arithmetic on another field.
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, true},
		{PPC_REG_CR7LT, false},
		{PPC_REG_CR7GT, true},
	});

	// Hand-assembled: Keystone does not produce this encoding from that
	// syntax, and the bytes are what the corpus contains.
	// 0x4f9de382 = cror cr7lt, cr7gt, cr7lt.
	emulate_bin("4f 9d e3 82");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR7LT, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LBZ
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LBZ)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x12_b},
	});

	emulate("lbz 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LBZ_zext)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0xff_b},
	});

	emulate("lbz 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHZ
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHZ)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x1234_w},
	});

	emulate("lhz 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LWZ
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWZ)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwz 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, MemoryLoadAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("lwz 0, 0x120, 1"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, MemoryStoreAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("stw 0, 0x120, 1"));
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
// PPC_INS_LBZU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LBZU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x12_b},
	});

	emulate("lbzu 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LBZU_zext)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0xff_b},
	});

	emulate("lbzu 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xff},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHZU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHZU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x1234_w},
	});

	emulate("lhzu 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LWZU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWZU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwzu 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LBZX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LBZX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x12_b},
	});

	emulate("lbzx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LBZX_zext)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0xff_b},
	});

	emulate("lbzx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHZX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHZX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x1234_w},
	});

	emulate("lhzx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LWZX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWZX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwzx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// `lwzx rD, 0, rB` is not `lwzx rD, r0, rB`: in every X-form memory access rA
// = 0 is the literal zero. GCC emits it whenever the address is already whole
// in one register, so it is not a corner case -- and the address it produced
// was `add undef, rB`. r0 is given a value here so that reading it as a
// register moves the load somewhere this test does not set.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWZX_ra_is_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x2000},
		{PPC_REG_R2, 0x1120},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwzx 3, 0, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R3, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STWX_ra_is_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x2000},
		{PPC_REG_R3, 0x12345678},
		{PPC_REG_R2, 0x1120},
	});

	emulate("stwx 3, 0, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R2, PPC_REG_R3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x12345678_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

// The emulator cannot see this one. It evaluates UndefValue as zero, so
// `add undef, rB` and `rB` reach the same address and the tests above pass
// either way -- which is why the bug survived: every existing test of an
// indexed access used a real rA. The defect is in the IR, so the assertion
// belongs there: after translating an rA = 0 form, nothing in the function may
// be undef or poison.
//
// All six shapes are listed because the rA slot is read by five different
// translators and they were fixed one at a time.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, IndexedFormsWithRaZeroProduceNoUndefAddress)
{
	ALL_MODES;

	const std::vector<std::string> insns = {
		"lwzx 3, 0, 2",
		"stwx 3, 0, 2",
		"lwbrx 3, 0, 2",
		"stwbrx 3, 0, 2",
		"lhbrx 3, 0, 2",
		"lfsx 3, 0, 2",
		"stfsx 3, 0, 2",
	};

	for (auto& a: insns)
	{
		auto* f = translate(assemble(a));
		ASSERT_NE(nullptr, f) << a;
		for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
		{
			for (auto& op: it->operands())
			{
				EXPECT_FALSE(isa<UndefValue>(op.get()))
					<< a << " produced an undef operand in: " << llvmObjToString(&*it);
			}
		}
	}
}

// The same instructions with a real rA must still add it, or the fix above
// would be "ignore rA".
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, IndexedFormsWithRealRaStillAdd)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwzx 3, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R3, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// lmw and stmw transfer 32-bit words on 64-bit PowerPC too. The emulator's
// memory is a map from address to value and does not record the width of an
// access, so it cannot tell a 32-bit store from a 64-bit one at the same
// address -- reverting this to the register width leaves every emulation test
// above passing. The width is in the IR, so it is checked there.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, LoadStoreMultipleTransfersWords)
{
	ALL_MODES;

	for (auto& a: {std::string("stmw 28, 8(1)"), std::string("lmw 28, 8(1)")})
	{
		auto* f = translate(assemble(a));
		ASSERT_NE(nullptr, f) << a;
		unsigned n = 0;
		for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
		{
			// Memory, as opposed to a register, is reached through an
			// inttoptr of the computed address.
			if (auto* l = dyn_cast<LoadInst>(&*it))
			{
				if (isa<IntToPtrInst>(l->getPointerOperand()))
				{
					EXPECT_EQ(32u, l->getType()->getIntegerBitWidth()) << a;
					++n;
				}
			}
			else if (auto* st = dyn_cast<StoreInst>(&*it))
			{
				if (isa<IntToPtrInst>(st->getPointerOperand()))
				{
					EXPECT_EQ(32u, st->getValueOperand()->getType()->getIntegerBitWidth()) << a;
					++n;
				}
			}
		}
		EXPECT_EQ(4u, n) << a << " should transfer r28..r31";
	}
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, LwarxLoadIsAtomic)
{
	ALL_MODES;
	auto* f = translate(assemble("lwarx 0, 1, 2"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, LwarxLoadAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("lwarx 0, 1, 2"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, LdarxLoadAttachesPointeeMetadata)
{
	ONLY_MODE_64;
	auto* f = translate(assemble("ldarx 0, 1, 2"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWARX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwarx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LBZUX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LBZUX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x12_b},
	});

	emulate("lbzux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LBZUX_zext)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0xff_b},
	});

	emulate("lbzux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xff},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHZUX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHZUX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x1234_w},
	});

	emulate("lhzux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LWZUX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWZUX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwzux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHA
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHA)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x1234_w},
	});

	emulate("lha 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHA_sext_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0xff34_w},
	});

	emulate("lha 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffff34},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHA_sext_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0xff34_w},
	});

	emulate("lha 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffffffffffff34},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHAU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x1234_w},
	});

	emulate("lhau 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAU_sext_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0xff34_w},
	});

	emulate("lhau 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffff34},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAU_sext_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0xff34_w},
	});

	emulate("lhau 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffffffffffff34},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHAX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x1234_w},
	});

	emulate("lhax 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAX_sext_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0xff34_w},
	});

	emulate("lhax 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffff34},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAX_sext_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0xff34_w},
	});

	emulate("lhax 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffffffffffff34},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHAUX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAUX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x1234_w},
	});

	emulate("lhaux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x1234},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAUX_sext_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0xff34_w},
	});

	emulate("lhaux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffff34},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHAUX_sext_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0xff34_w},
	});

	emulate("lhaux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xffffffffffffff34},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LHBRX, PPC_INS_LWBRX, PPC_INS_LDBRX
//

// This test was commented out with "TODO: Not working, maybe because of little
// vs big endian?" on it. It was right about the halfword and wrong about the
// cause: lhbrx was the only one of the six byte-reversed accesses that was
// modelled at all, by hand, as two byte loads OR'd together -- and the
// emulator stores a 16-bit value at one address rather than as two bytes, so
// the second byte load read nothing. One llvm.bswap reads the halfword whole.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LHBRX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x12ff_w},
	});

	emulate("lhbrx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0xff12},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LDBRX)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x1122334455667788_qw},
	});

	emulate("ldbrx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x8877665544332211},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LI)
{
	ALL_MODES;

	emulate("li 11, 0x1234");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R11, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LIS
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LIS)
{
	ALL_MODES;

	emulate("lis 11, 0x1234");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R11, 0x12340000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LWBRX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWBRX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwbrx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x78563412},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The rA = 0 form. In every X-form memory access rA = 0 means the literal zero
// rather than r0, and capstone reports that slot as a register with the
// invalid id -- which loadOp() turns into UndefValue, so the address was
// `add undef, rB`. r0 is deliberately given a value here: if the translator
// reads it as a register, the address is 0x1120 + 0x2000 and no load lands on
// the memory this sets.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWBRX_ra_is_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x2000},
		{PPC_REG_R2, 0x1120},
	});
	setMemory({
		{0x1120, 0x12345678_dw},
	});

	emulate("lwbrx 3, 0, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R3, 0x78563412},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MTSPR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MTSPR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1234},
	});

	emulate("mtspr 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_mtspr"), {0, 0x1234}},
	});
}

//
// PPC_INS_MFSPR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MFSPR)
{
	ALL_MODES;

	emulate("mfspr 0, 0");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_mfspr"), {0}},
	});
}

//
// PPC_INS_MFCR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MFCR)
{
	// 9,008 occurrences in the static corpus, the largest unmodelled
	// pseudo-assembly call on PowerPC.
	//
	// PowerPC numbers the condition register's bits from the MOST significant
	// end: CR0's LT bit is bit 31 of the word and CR7's SO bit is bit 0. The
	// three bits set here are the two ends and one in the middle, so a
	// translation that assembled the fields the other way round answers
	// 0x80004001 instead.
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true}, // bit 31
		{PPC_REG_CR3EQ, true}, // bit 31 - (4*3 + 2) = 17
		{PPC_REG_CR7UN, true}, // bit 0
	});

	emulate("mfcr 0");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x80020001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MFCTR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MFCTR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x1234},
	});

	emulate("mfctr 11");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R11, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MFLR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MFLR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x1234},
	});

	emulate("mflr 11");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R11, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_MCRF
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MCRF_same)
{
	ALL_MODES;

	emulate("mcrf 0, 0");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The bit pattern is LT,GT,EQ,SO = 1,0,1,0: no transposition of two of them
// and no reversal of all four produces the same four values, so a field copied
// into the wrong slot shows up.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MCRF_read_cr0)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, false},
	});

	emulate("mcrf 4, 0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT, PPC_REG_CR0GT, PPC_REG_CR0EQ, PPC_REG_CR0UN});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CR4GT, false},
		{PPC_REG_CR4EQ, true},
		{PPC_REG_CR4UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MCRF_write_cr0)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CR4GT, false},
		{PPC_REG_CR4EQ, true},
		{PPC_REG_CR4UN, false},
	});

	emulate("mcrf 0, 4");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT, PPC_REG_CR4GT, PPC_REG_CR4EQ, PPC_REG_CR4UN});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// Neither side is CR0, which is most of the 466 occurrences in the parity
// corpus and was the case where both halves of the old model were dead: it
// read crS as a four-bit register nothing writes and wrote crD as a four-bit
// register nothing reads.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_MCRF_other)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CR4GT, false},
		{PPC_REG_CR4EQ, true},
		{PPC_REG_CR4UN, false},
	});

	emulate("mcrf 2, 4");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT, PPC_REG_CR4GT, PPC_REG_CR4EQ, PPC_REG_CR4UN});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR2LT, true},
		{PPC_REG_CR2GT, false},
		{PPC_REG_CR2EQ, true},
		{PPC_REG_CR2UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STHBRX, PPC_INS_STWBRX, PPC_INS_STDBRX
//

// These were `translatePseudoAsmFncOp0Op1Op2`: the value and the two address
// registers went into an opaque function and nothing was written. A store that
// stores nothing is not an approximation of a store.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STHBRX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12ff},
		{PPC_REG_R4, 0x1000},
		{PPC_REG_R5, 0x120},
	});

	emulate("sthbrx 0, 4, 5");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R4, PPC_REG_R5});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0xff12_w},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STWBRX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R4, 0x1000},
		{PPC_REG_R5, 0x120},
	});

	emulate("stwbrx 0, 4, 5");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R4, PPC_REG_R5});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x78563412_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STDBRX)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R0, 0x1122334455667788},
		{PPC_REG_R4, 0x1000},
		{PPC_REG_R5, 0x120},
	});

	emulate("stdbrx 0, 4, 5");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R4, PPC_REG_R5});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x8877665544332211_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

// The store side of the rA = 0 form.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STWBRX_ra_is_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x2000},
		{PPC_REG_R3, 0x12345678},
		{PPC_REG_R5, 0x1120},
	});

	emulate("stwbrx 3, 0, 5");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R3, PPC_REG_R5});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x78563412_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_LMW, PPC_INS_STMW
//

// The count is 32 - rS and is not encoded anywhere: `stmw 28, 8(1)` is four
// stores, of r28, r29, r30 and r31, at 8, 12, 16 and 20 past r1. Each register
// gets a distinct value, so a run written in the wrong order or starting from
// the wrong register does not pass; EXPECT_JUST_MEMORY_STORED holds the ends
// of the range, so a fifth store or a missing first one does not either.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STMW)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R28, 0xaaaa1111},
		{PPC_REG_R29, 0xbbbb2222},
		{PPC_REG_R30, 0xcccc3333},
		{PPC_REG_R31, 0xdddd4444},
	});

	emulate("stmw 28, 8(1)");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R28, PPC_REG_R29, PPC_REG_R30, PPC_REG_R31});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0xaaaa1111_dw},
		{0x100c, 0xbbbb2222_dw},
		{0x1010, 0xcccc3333_dw},
		{0x1014, 0xdddd4444_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

// rS = r31 is one transfer. A fixed count, or one counting up from r0, is
// thirty-two.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STMW_one_register)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R31, 0xdddd4444},
	});

	emulate("stmw 31, 8(1)");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R31});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0xdddd4444_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LMW)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1008, 0xaaaa1111_dw},
		{0x100c, 0xbbbb2222_dw},
		{0x1010, 0xcccc3333_dw},
		{0x1014, 0xdddd4444_dw},
	});

	emulate("lmw 28, 8(1)");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R28, 0xaaaa1111},
		{PPC_REG_R29, 0xbbbb2222},
		{PPC_REG_R30, 0xcccc3333},
		{PPC_REG_R31, 0xdddd4444},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008, 0x100c, 0x1010, 0x1014});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STB
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STB)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
	});

	emulate("stb 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({{0x1120, 0x78_b}});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STH
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STH)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
	});

	emulate("sth 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({{0x1120, 0x5678_w}});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STW)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
	});

	emulate("stw 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({{0x1120, 0x12345678_w}});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STBU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STBU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
	});

	emulate("stbu 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({{PPC_REG_R1, 0x1120}});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x78_b}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STHU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STHU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
	});

	emulate("sthu 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R1, 0x1120}
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x5678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STWU
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STWU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
	});

	emulate("stwu 0, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R1, 0x1120}
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x12345678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STBX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STBX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});

	emulate("stbx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1, PPC_REG_R2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x78_b}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STHX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STHX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});

	emulate("sthx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1, PPC_REG_R2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x5678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STWX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STWX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});

	emulate("stwx 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1, PPC_REG_R2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x12345678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, StwcxStoreIsAtomic)
{
	ALL_MODES;
	auto* f = translate(assemble("stwcx. 0, 1, 2"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, StwcxAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("stwcx. 0, 1, 2"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, StdcxAttachesPointeeMetadata)
{
	ONLY_MODE_64;
	auto* f = translate(assemble("stdcx. 0, 1, 2"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STWCX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});

	emulate("stwcx. 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0EQ, true},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x12345678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STBUX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STBUX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});

	emulate("stbux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R1, 0x1120}
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x78_b}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STHUX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STHUX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});

	emulate("sthux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R1, 0x1120}
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x5678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_STWUX
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STWUX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x12345678},
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});

	emulate("stwux 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R1, 0x1120}
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x12345678_w}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CMPD
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPD_lt)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x1111},
		{PPC_REG_R1, 0x2222},
	});

	emulate("cmpd 0, 1"); // cr0 is default

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPD_gt)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x3333},
		{PPC_REG_R1, 0x2222},
	});

	emulate("cmpd cr0, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPD_eq)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x2222},
		{PPC_REG_R1, 0x2222},
	});

	emulate("cmpd cr0, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, true},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPD_lt_sign_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R0, 0xffff0000},
		{PPC_REG_R1, 0x0},
	});

	emulate("cmpd cr0, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPD_lt_sign_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R0, 0xffff000000000000},
		{PPC_REG_R1, 0x0},
	});

	emulate("cmpd cr0, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPD_lt_cr7)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x1111},
		{PPC_REG_R1, 0x2222},
	});

	emulate("cmpd cr7, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR7LT, true},
		{PPC_REG_CR7GT, false},
		{PPC_REG_CR7EQ, false},
		{PPC_REG_CR7UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CMPDI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPDI_lt_cr7)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x1111},
	});

	emulate("cmpdi cr7, 0, 0x2222");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR7LT, true},
		{PPC_REG_CR7GT, false},
		{PPC_REG_CR7EQ, false},
		{PPC_REG_CR7UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CMPW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPW_lt_cr7)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x1111},
		{PPC_REG_R1, 0x2222},
	});

	emulate("cmpw cr7, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR7LT, true},
		{PPC_REG_CR7GT, false},
		{PPC_REG_CR7EQ, false},
		{PPC_REG_CR7UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPW_lt_cr7_64_trunc)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R0, 0xffffffff11111111},
		{PPC_REG_R1, 0x22222222},
	});

	emulate("cmpw cr7, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR7LT, true},
		{PPC_REG_CR7GT, false},
		{PPC_REG_CR7EQ, false},
		{PPC_REG_CR7UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CMPWI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPWI_lt_cr7)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x1111},
	});

	emulate("cmpwi cr7, 0, 0x2222");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR7LT, true},
		{PPC_REG_CR7GT, false},
		{PPC_REG_CR7EQ, false},
		{PPC_REG_CR7UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPI_word)
{
	// Primary form cmpi BF, L, rA, SI. L=0 is a word compare, same as cmpwi.
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0x1111},
	});

	emulate("cmpi cr7, 0, 0, 0x2222");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR7LT, true},
		{PPC_REG_CR7GT, false},
		{PPC_REG_CR7EQ, false},
		{PPC_REG_CR7UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPLI_word)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R0, 0xffff0000},
	});

	emulate("cmpli cr5, 0, 0, 0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR5LT, false},
		{PPC_REG_CR5GT, true},
		{PPC_REG_CR5EQ, false},
		{PPC_REG_CR5UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CMPLD
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPLD_gt_unsign_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R0, 0xffff0000},
		{PPC_REG_R1, 0x0},
	});

	emulate("cmpld cr5, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR5LT, false},
		{PPC_REG_CR5GT, true},
		{PPC_REG_CR5EQ, false},
		{PPC_REG_CR5UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CMPLDI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPLDI_gt_unsign_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R0, 0xffff0000},
	});

	emulate("cmpldi cr5, 0, 0x0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR5LT, false},
		{PPC_REG_CR5GT, true},
		{PPC_REG_CR5EQ, false},
		{PPC_REG_CR5UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CMPLW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPLW_gt_unsign_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R0, 0xffff0000},
		{PPC_REG_R1, 0x0},
	});

	emulate("cmplw cr5, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR5LT, false},
		{PPC_REG_CR5GT, true},
		{PPC_REG_CR5EQ, false},
		{PPC_REG_CR5UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPLW_eq_unsign_64)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R0, 0xffff000000000000},
		{PPC_REG_R1, 0x0},
	});

	emulate("cmplw cr5, 0, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0, PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR5LT, false},
		{PPC_REG_CR5GT, false},
		{PPC_REG_CR5EQ, true},
		{PPC_REG_CR5UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CMPLWI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CMPLWI_gt_unsign_32)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R0, 0xffff0000},
	});

	emulate("cmplwi cr5, 0, 0x0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R0});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR5LT, false},
		{PPC_REG_CR5GT, true},
		{PPC_REG_CR5EQ, false},
		{PPC_REG_CR5UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CRSET
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CRSET_cr0)
{
	ALL_MODES;

	emulate("crset eq");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0EQ, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CRSET_cr5)
{
	ALL_MODES;

	emulate("crset 4*cr5+eq");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR5EQ, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CRCLR
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CRCLR_cr0)
{
	ALL_MODES;

	emulate("crclr eq");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0EQ, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CRCLR_cr5)
{
	ALL_MODES;

	emulate("crclr 4*cr5+eq");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR5EQ, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CRNOT
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CRNOT)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR5UN, true},
	});

	emulate("crnot 4*cr2+eq, 4*cr5+so");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR5UN});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR2EQ, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CRNOT
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CRMOVE)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR5UN, true},
	});

	emulate("crmove 4*cr2+eq, 4*cr5+so");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR5UN});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR2EQ, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SLWI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SLWI)
{
	SKIP_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("slwi 0, 1, 0x8");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x34567800},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_SRWI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_SRWI)
{
	SKIP_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("srwi 0, 1, 0x8");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x00123456},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_CLRLWI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CLRLWI)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("clrlwi 0, 1, 16");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x00005678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CLRLWI_zero)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("clrlwi 0, 1, 0");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_CLRLWI_31)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x12345678},
	});

	emulate("clrlwi 0, 1, 31");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ROTLW
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ROTLW)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x00240000}, // 00100100 0...
		{PPC_REG_R2, 0x8},
	});

	emulate("rotlw 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x24000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// PPC_INS_ROTLWI
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_ROTLWI)
{
	ONLY_MODE_32;

	setRegisters({
		{PPC_REG_R1, 0x00240000}, // 00100100 0...
	});

	emulate("rotlwi 0, 1, 0x8");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R0, 0x24000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
//==============================================================================
// PPC_INS_B
//==============================================================================
//

// PPC_BC_INVALID, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_uncond)
{
	ALL_MODES;

	emulate("b 0x4bc", 0x10000510);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x100004bc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BC_true)
{
	// Primary form: BO=12 (branch if CR bit true), BI=0 (CR0 LT).
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
	});

	emulate("bc 12, 0, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BC_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
	});

	emulate("bc 12, 0, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_blt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
	});

	emulate("blt 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_blt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
	});

	emulate("blt 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_LE, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_ble_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0EQ, false},
	});

	emulate("ble 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT, PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LE, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_ble_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0EQ, false},
	});

	emulate("ble 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT, PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_EQ, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_beq_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0EQ, true},
	});

	emulate("beq 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_EQ, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_beq_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0EQ, false},
	});

	emulate("beq 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_NE, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bne_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0EQ, false},
	});

	emulate("bne 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_NE, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bne_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0EQ, true},
	});

	emulate("bne 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_GT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bgt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0GT, true},
	});

	emulate("bgt 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0GT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_GT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bgt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0GT, false},
	});

	emulate("bgt 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0GT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_GE, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bge_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0GT, true},
		{PPC_REG_CR0EQ, false},
	});

	emulate("bge 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0GT, PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_GE, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bge_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
	});

	emulate("bge 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0GT, PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_SO (PPC_BC_UN is not used here), op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bun_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0UN, true},
	});

	emulate("bun 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0UN});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_SO (PPC_BC_UN is not used here), op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bun_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0UN, false},
	});

	emulate("bun 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0UN});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_SO, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bso_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0UN, true},
	});

	emulate("bso 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0UN});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_SO, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bso_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0UN, false},
	});

	emulate("bso 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0UN});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_NS, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bns_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0UN, false},
	});

	emulate("bns 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0UN});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_NS, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bns_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0UN, true},
	});

	emulate("bns 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0UN});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_NS (PPC_BC_NU is not used here), op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bnu_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0UN, false},
	});

	emulate("bnu 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0UN});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_NS (PPC_BC_NU is not used here), op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_bnu_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0UN, true},
	});

	emulate("bnu 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0UN});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_blt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
	});

	emulate("blt cr4, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_B_blt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
	});

	emulate("blt cr4, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BA
//==============================================================================
//

// PPC_BC_INVALID, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BA_uncond)
{
	ALL_MODES;

	emulate("ba 0x4bc", 0x10000510);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BA_blt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
	});

	emulate("blta 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BA_blt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
	});

	emulate("blta 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BA_blt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
	});

	emulate("blta cr4, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BA_blt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
	});

	emulate("blta cr4, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BL
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BL_uncond)
{
	ALL_MODES;

	emulate("bl 0x4bc", 0x10000510);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BL_blt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
	});

	emulate("bltl 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BL_blt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
	});

	emulate("bltl 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BL_blt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
	});

	emulate("bltl cr4, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BL_blt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
	});

	emulate("bltl cr4, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BLA
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLA_uncond)
{
	ALL_MODES;

	emulate("bla 0x4bc", 0x10000510);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLA_blt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
	});

	emulate("bltla 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLA_blt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
	});

	emulate("bltla 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLA_blt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
	});

	emulate("bltla cr4, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLA_blt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
	});

	emulate("bltla cr4, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BLR
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLR_uncond)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("blr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLR_lt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR0LT, true},
	});

	emulate("bltlr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLR_lt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR0LT, false},
	});

	emulate("bltlr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
// TODO: We cannot check this, because it is in always false branch.
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//		{_translator->getReturnFunction(), {0x100004bc}},
//	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLR_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR4LT, true},
	});

	emulate("bltlr cr4", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLR_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR4LT, false},
	});

	emulate("bltlr cr4", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BCTR
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTR_uncond)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
	});

	emulate("bctr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTR_lt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR0LT, true},
	});

	emulate("bltctr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTR_lt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR0LT, false},
	});

	emulate("bltctr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTR_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR4LT, true},
	});

	emulate("bltctr cr4", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTR_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR4LT, false},
	});

	emulate("bltctr cr4", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BLRL
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLRL_uncond)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("blrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLRL_lt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR0LT, true},
	});

	emulate("bltlrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLRL_lt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR0LT, false},
	});

	emulate("bltlrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLRL_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR4LT, true},
	});

	emulate("bltlrl cr4", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BLRL_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR4LT, false},
	});

	emulate("bltlrl cr4", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BCTRL
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTRL_uncond)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
	});

	emulate("bctrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTRL_lt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR0LT, true},
	});

	emulate("bltctrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTRL_lt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR0LT, false},
	});

	emulate("bltctrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTRL_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR4LT, true},
	});

	emulate("bltctrl cr4", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BCTRL_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR4LT, false},
	});

	emulate("bltctrl cr4", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BT
//==============================================================================
//

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BT_lt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
	});

	emulate("bt lt, 0x4bc", 0x10000510); // gets translated to b

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BT_lt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
	});

	emulate("bt lt, 0x4bc", 0x10000510); // gets translated to b

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BT_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
	});

	emulate("bt 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to b

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BT_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
	});

	emulate("bt 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to b

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BTA
//==============================================================================
//

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTA_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
	});

	emulate("bta 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to ba

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTA_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
	});

	emulate("bta 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to ba

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BTLR
//==============================================================================
//

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTLR_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR4LT, true},
	});

	emulate("btlr 4*cr4+lt", 0x10000510); // gets translated to blr

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTLR_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR4LT, false},
	});

	emulate("btlr 4*cr4+lt", 0x10000510); // gets translated to blr

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BTCTR
//==============================================================================
//

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTCTR_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR4LT, true},
	});

	emulate("btctr 4*cr4+lt", 0x10000510); // gets translated to bctr

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTCTR_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR4LT, false},
	});

	emulate("btctr 4*cr4+lt", 0x10000510); // gets translated to bctr

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BTL
//==============================================================================
//

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTL_blt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
	});

	emulate("btl 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to bl

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTL_blt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
	});

	emulate("btl 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to bl

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BTLA
//==============================================================================
//

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTLA_blt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
	});

	emulate("btla 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to bla

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTLA_blt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
	});

	emulate("btla 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to bla

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BTLA
//==============================================================================
//

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTLRL_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR4LT, true},
	});

	emulate("btlrl 4*cr4+lt", 0x10000510); // gets translated to blrl

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTLRL_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CR4LT, false},
	});

	emulate("btlrl 4*cr4+lt", 0x10000510); // gets translated to blrl

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_LR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BTCTRL
//==============================================================================
//

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTCTRL_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR4LT, true},
	});

	emulate("btctrl 4*cr4+lt", 0x10000510); // gets translated to bctrl

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_LT, op0 = PPC_OP_REG = cr4
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BTCTRL_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 0x100004bc},
		{PPC_REG_CR4LT, false},
	});

	emulate("btctrl 4*cr4+lt", 0x10000510); // gets translated to bctrl

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BF
//==============================================================================
//

// PPC_BC_GE, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BF_lt_cr0_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0GT, false},
	});

	emulate("bf lt, 0x4bc", 0x10000510); // gets translated to b ge

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0GT, PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_GE, op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BF_lt_cr0_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0GT, true},
	});

	emulate("bf lt, 0x4bc", 0x10000510); // gets translated to b ge

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR0GT, PPC_REG_CR0EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// PPC_BC_GE, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BF_lt_cr4_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4EQ, false},
		{PPC_REG_CR4GT, false},
	});

	emulate("bf 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to b ge

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4GT, PPC_REG_CR4EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// PPC_BC_GE, op0 = PPC_OP_REG = cr4, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_Bf_lt_cr4_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4EQ, true},
		{PPC_REG_CR4GT, false},
	});

	emulate("bf 4*cr4+lt, 0x4bc", 0x10000510); // gets translated to b ge

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CR4GT, PPC_REG_CR4EQ});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZ
//==============================================================================
//

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZ_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 10},
	});

	emulate("bdnz 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZ_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 1},
	});

	emulate("bdnz 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZA
//==============================================================================
//

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZA_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 10},
	});

	emulate("bdnza 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZA_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 1},
	});

	emulate("bdnza 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZLR
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZLR_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzlr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZLR_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzlr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BDNZL
//==============================================================================
//

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZL_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzl 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZL_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzl 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZLA
//==============================================================================
//

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZLA_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzla 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZLA_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzla 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZLRL
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZLRL_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzlrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZLRL_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzlrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BDNZ
//==============================================================================
//

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZ_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 10},
	});

	emulate("bdz 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZ_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 1},
	});

	emulate("bdz 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZA
//==============================================================================
//

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZA_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 10},
	});

	emulate("bdza 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZA_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 1},
	});

	emulate("bdza 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZLR
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZLR_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzlr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZLR_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzlr", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZL
//==============================================================================
//

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZL_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 10},
	});

	emulate("bdzl 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZL_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 1},
	});

	emulate("bdzl 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZLA
//==============================================================================
//

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZLA_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 10},
	});

	emulate("bdzla 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZLA_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CTR, 1},
	});

	emulate("bdzla 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZLRL
//==============================================================================
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZLRL_nonzero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzlrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZLRL_zero)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_LR, 0x100004bc},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzlrl", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZT
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZT_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzt 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZT_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzt 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZT_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzt 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZT_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzt 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZT_nonzero_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzt lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZT_nonzero_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzt lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZT_zero_true)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzt lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZT_zero_false)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR0LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzt lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR0LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZTA
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTA_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzta 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTA_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzta 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTA_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzta 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTA_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzta 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZTLR
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLR_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnztlr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLR_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnztlr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLR_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnztlr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLR_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnztlr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BDNZTL
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTL_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnztl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTL_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnztl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTL_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnztl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTL_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnztl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZTLA
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLA_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnztla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLA_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnztla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLA_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnztla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLA_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnztla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZTLRL
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLRL_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnztlrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLRL_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnztlrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLRL_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnztlrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZTLRL_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnztlrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BDNZF
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZF_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzf 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZF_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzf 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZF_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzf 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZF_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzf 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZFA
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFA_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzfa 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFA_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzfa 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFA_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzfa 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFA_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzfa 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZFLR
//==============================================================================
//

// TODO: PPC_INS_BDNZFLR - missing, it gets translated to PPC_INS_BCLR,
// but then we dont know that CRT should be decremented -- only hint is that
// insn is reading and writing it, but we are not using this info at the moment.

//// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
//TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLR_nonzero_true_cr4)
//{
//	ALL_MODES;
//
//	setRegisters({
//		{PPC_REG_CR4LT, true},
//		{PPC_REG_CTR, 10},
//		{PPC_REG_LR, 0x100004bc},
//	});
//
//	emulate("bdnzflr 4*cr4+lt", 0x10000510);
//
//	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
//	EXPECT_JUST_REGISTERS_STORED({
//		{PPC_REG_CTR, 9},
//	});
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
//}
//
//// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
//TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLR_nonzero_false_cr4)
//{
//	ALL_MODES;
//
//	setRegisters({
//		{PPC_REG_CR4LT, false},
//		{PPC_REG_CTR, 10},
//		{PPC_REG_LR, 0x100004bc},
//	});
//
//	emulate("bdnzflr 4*cr4+lt", 0x10000510);
//
//	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
//	EXPECT_JUST_REGISTERS_STORED({
//		{PPC_REG_CTR, 9},
//	});
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
//	});
//}
//
//// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
//TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLR_zero_true_cr4)
//{
//	ALL_MODES;
//
//	setRegisters({
//		{PPC_REG_CR4LT, true},
//		{PPC_REG_CTR, 1},
//		{PPC_REG_LR, 0x100004bc},
//	});
//
//	emulate("bdnzflr 4*cr4+lt", 0x10000510);
//
//	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
//	EXPECT_JUST_REGISTERS_STORED({
//		{PPC_REG_CTR, 0},
//	});
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
//}
//
//// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
//TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLR_zero_false_cr4)
//{
//	ALL_MODES;
//
//	setRegisters({
//		{PPC_REG_CR4LT, false},
//		{PPC_REG_CTR, 1},
//		{PPC_REG_LR, 0x100004bc},
//	});
//
//	emulate("bdnzflr 4*cr4+lt", 0x10000510);
//
//	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
//	EXPECT_JUST_REGISTERS_STORED({
//		{PPC_REG_CTR, 0},
//	});
//	EXPECT_NO_MEMORY_LOADED_STORED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
//}

//
//==============================================================================
// PPC_INS_BDNZFL
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFL_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzfl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFL_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzfl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFL_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzfl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFL_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzfl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZFLA
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLA_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzfla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLA_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdnzfla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLA_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzfla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLA_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdnzfla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDNZFLRL
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLRL_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnzflrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLRL_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnzflrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLRL_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnzflrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDNZFLRL_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdnzflrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BDZT
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZT_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzt 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZT_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzt 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZT_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzt 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZT_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzt 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZTA
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTA_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzta 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTA_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzta 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTA_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzta 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTA_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzta 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZTLR
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLR_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdztlr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLR_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdztlr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLR_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdztlr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLR_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdztlr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BDZTL
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTL_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdztl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTL_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdztl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTL_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdztl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTL_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdztl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZTLA
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLA_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdztla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLA_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdztla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLA_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdztla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLA_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdztla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZTLRL
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLRL_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdztlrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLRL_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdztlrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLRL_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdztlrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZTLRL_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdztlrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

//
//==============================================================================
// PPC_INS_BDZF
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZF_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzf 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZF_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzf 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZF_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzf 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZF_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzf 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZFA
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFA_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzfa 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFA_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzfa 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFA_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzfa 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFA_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzfa 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZFLR
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLR_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdzflr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLR_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdzflr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLR_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdzflr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLR_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdzflr 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZFL
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFL_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzfl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFL_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzfl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFL_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzfl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFL_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzfl 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x100004bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZFLA
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLA_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzfla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLA_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
	});

	emulate("bdzfla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLA_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzfla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x4bc}},
	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLA_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
	});

	emulate("bdzfla 4*cr4+lt, 0x4bc", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x4bc}},
	});
}

//
//==============================================================================
// PPC_INS_BDZFLRL
//==============================================================================
//

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLRL_nonzero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdzflrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLRL_nonzero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 10},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdzflrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 9},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLRL_zero_true_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, true},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdzflrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
//	EXPECT_JUST_VALUES_CALLED({
//		{_translator->getCondBranchFunction(), {false, 0x100004bc}},
//	});
}

// op0 = ppc_op_crx, op1 = PPC_OP_IMM = target
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_BDZFLRL_zero_false_cr4)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_CR4LT, false},
		{PPC_REG_CTR, 1},
		{PPC_REG_LR, 0x100004bc},
	});

	emulate("bdzflrl 4*cr4+lt", 0x10000510);

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_CTR, PPC_REG_CR4LT, PPC_REG_LR});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CTR, 0},
		{PPC_REG_LR, 0x10000514},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x100004bc}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, SyncEmitsFence)
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, IsyncEmitsFence)
{
	ALL_MODES;
	auto* f = translate(assemble("isync"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, LwsyncEmitsFence)
{
	ALL_MODES;
	auto* f = translate(assemble("lwsync"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, EieioEmitsFence)
{
	ALL_MODES;
	auto* f = translate(assemble("eieio"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, MbarEmitsFence)
{
	ALL_MODES;
	auto* f = translate(assemble("mbar"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, MsyncEmitsFence)
{
	ALL_MODES;
	auto* f = translate(assemble("msync"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, TlbsyncEmitsFence)
{
	ALL_MODES;
	auto* f = translate(assemble("tlbsync"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PtesyncEmitsFence)
{
	ALL_MODES;
	auto* f = translate(assemble("ptesync"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, LwzxLoadAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("lwzx 0, 1, 2"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, StwxStoreAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("stwx 0, 1, 2"));
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

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, LhbrxLoadAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("lhbrx 0, 1, 2"));
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

//
// Floating-point loads and stores.
//
// The FPRs are modelled as doubles, so a single-precision access converts:
// widening on the way in, narrowing on the way out. These pin that, and pin
// that the datum written to memory is 4 bytes for the S forms and 8 for the D
// forms -- which is the part a bitcast would get wrong.
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LFD)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 3.14_f64},
	});

	emulate("lfd 2, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F2, 3.14_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LFS)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 1.5_f32},
	});

	emulate("lfs 2, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F2, 1.5_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LFDU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 3.14_f64},
	});

	emulate("lfdu 2, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F2, 3.14_f64},
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LFDX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 3.14_f64},
	});

	emulate("lfdx 3, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F3, 3.14_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STFD)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_F2, 3.14_f64},
	});

	emulate("stfd 2, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_F2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 3.14_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STFS)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_F2, 1.5_f64},
	});

	emulate("stfs 2, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_F2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 1.5_f32},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STFDU)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_F2, 3.14_f64},
	});

	emulate("stfdu 2, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R1, 0x1120},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 3.14_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STFDX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
		{PPC_REG_F3, 3.14_f64},
	});

	emulate("stfdx 3, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2, PPC_REG_F3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 3.14_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, FloatLoadAttachesPointeeMetadata)
{
	ALL_MODES;
	auto* f = translate(assemble("lfd 2, 0x120, 1"));
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

//
// 64-bit integer loads and stores. ppc64 only: on ppc32 these encodings do
// not exist, and the register file is 32 bits wide.
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0x123456789abcdef0_qw},
	});

	emulate("ld 3, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R3, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STD)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R3, 0x123456789abcdef0},
	});

	emulate("std 3, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1120, 0x123456789abcdef0_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LDX)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
		{PPC_REG_R2, 0x120},
	});
	setMemory({
		{0x1120, 0x123456789abcdef0_qw},
	});

	emulate("ldx 3, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1, PPC_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R3, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LWA)
{
	ONLY_MODE_64;

	setRegisters({
		{PPC_REG_R1, 0x1000},
	});
	setMemory({
		{0x1120, 0xfffffffe_dw},
	});

	emulate("lwa 3, 0x120, 1");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_R3, 0xfffffffffffffffe},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1120});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Floating-point arithmetic.
//
// Every one of the 44 PPC_INS_F* entries in the dispatch table was nullptr,
// while powerpc_init.cpp maps PPC_REG_F0..F31 to double: the register file was
// modelled and nothing ever wrote to it.
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FADD)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 1.5_f64},
		{PPC_REG_F2, 2.25_f64},
	});

	emulate("fadd 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_F1, PPC_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 3.75_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FADDS_rounds_to_single)
{
	ALL_MODES;

	// 2^24+1 is the smallest integer a float cannot represent; it rounds to
	// 2^24. The S form rounds its result to single before writing it back, so
	// the register must hold 16777216, not 16777217.
	//
	// The obvious choice, 0.1, does not work: the double nearest 0.1 and the
	// double nearest the float 0.1 differ by about 1.5e-9, and this harness
	// compares doubles to a tolerance of 0.001, so the test would pass whether
	// the rounding happened or not. Checked by deleting the rounding -- with
	// 0.1 the test still passed, with this value it fails.
	setRegisters({
		{PPC_REG_F1, 16777217.0},
		{PPC_REG_F2, 0.0_f64},
	});

	emulate("fadds 0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_F1, PPC_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 16777216.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FSUB)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 5.5_f64},
		{PPC_REG_F2, 2.25_f64},
	});

	emulate("fsub 0, 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 3.25_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FMUL)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 1.5_f64},
		{PPC_REG_F2, 4.0_f64},
	});

	emulate("fmul 0, 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 6.0_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FDIV)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 9.0_f64},
		{PPC_REG_F2, 4.0_f64},
	});

	emulate("fdiv 0, 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 2.25_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FMR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F2, 3.5_f64},
	});

	emulate("fmr 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F1, 3.5_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FNEG)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F2, 3.5_f64},
	});

	emulate("fneg 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F1, -3.5},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FABS)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F2, -3.5},
	});

	emulate("fabs 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F1, 3.5_f64},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FNABS)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F2, 3.5_f64},
	});

	emulate("fnabs 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F1, -3.5},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FSQRT)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F2, 16.0_f64},
	});

	emulate("fsqrt 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F1, 4.0_f64},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FRSP)
{
	ALL_MODES;

	// See PPC_INS_FADDS_rounds_to_single for why the value is 2^24+1 and not
	// something like 0.1.
	setRegisters({
		{PPC_REG_F2, 16777217.0},
	});

	emulate("frsp 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F1, 16777216.0},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FMADD)
{
	ALL_MODES;

	// fmadd FRT, FRA, FRC, FRB is (FRA * FRC) + FRB. 2*3+4 is 10; if the last
	// two operands were the other way round it would be 2*4+3 = 11, so this
	// pins capstone's operand order as well as the arithmetic.
	setRegisters({
		{PPC_REG_F1, 2.0_f64},
		{PPC_REG_F2, 3.0_f64},
		{PPC_REG_F3, 4.0_f64},
	});

	emulate("fmadd 0, 1, 2, 3");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_F1, PPC_REG_F2, PPC_REG_F3});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 10.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FMSUB)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 2.0_f64},
		{PPC_REG_F2, 3.0_f64},
		{PPC_REG_F3, 4.0_f64},
	});

	emulate("fmsub 0, 1, 2, 3");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 2.0_f64},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FNMADD)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 2.0_f64},
		{PPC_REG_F2, 3.0_f64},
		{PPC_REG_F3, 4.0_f64},
	});

	emulate("fnmadd 0, 1, 2, 3");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, -10.0},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FNMSUB)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 2.0_f64},
		{PPC_REG_F2, 3.0_f64},
		{PPC_REG_F3, 4.0_f64},
	});

	emulate("fnmsub 0, 1, 2, 3");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, -2.0},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FSEL_takes_FRC_when_FRA_is_not_negative)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 0.0_f64},
		{PPC_REG_F2, 10.0_f64},
		{PPC_REG_F3, 20.0_f64},
	});

	emulate("fsel 0, 1, 2, 3");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 10.0_f64},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FSEL_takes_FRB_when_FRA_is_negative)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, -1.0},
		{PPC_REG_F2, 10.0_f64},
		{PPC_REG_F3, 20.0_f64},
	});

	emulate("fsel 0, 1, 2, 3");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, 20.0_f64},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCMPU_less_than)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 1.0_f64},
		{PPC_REG_F2, 2.0_f64},
	});

	emulate("fcmpu cr0, 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_F1, PPC_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCMPU_equal)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 2.0_f64},
		{PPC_REG_F2, 2.0_f64},
	});

	emulate("fcmpu cr1, 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR1LT, false},
		{PPC_REG_CR1GT, false},
		{PPC_REG_CR1EQ, true},
		{PPC_REG_CR1UN, false},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCMPU_unordered)
{
	ALL_MODES;

	// The fourth bit of the field is FU, not a copy of XER, and it is the only
	// thing that tells a NaN comparison from an equal one. storeCrX writes a
	// constant zero there, which is why this does not go through it.
	setRegisters({
		{PPC_REG_F1, std::numeric_limits<double>::quiet_NaN()},
		{PPC_REG_F2, 2.0_f64},
	});

	emulate("fcmpu cr0, 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, false},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, true},
	});
}

/// The raw bits of a double register.
///
/// fctiwz leaves an INTEGER in an FPR, and the fixture compares a double
/// register with EXPECT_NEAR(..., 0.001). The bit pattern of a small integer
/// is a denormal -- 3 is 1.5e-323 -- so the FCTIWZ test below cannot fail on
/// its value. These can.
static uint64_t ppcDoubleRegBits(double d)
{
	uint64_t bits = 0;
	std::memcpy(&bits, &d, sizeof bits);
	return bits;
}

// Power defines the out-of-range and NaN answers, and defines them DIFFERENTLY
// from ARM: it saturates, and sends a NaN to the destination's MINIMUM rather
// than to zero. The result is sign-extended into the 64-bit FPR.
TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCTIWZ_saturates_high)
{
	ALL_MODES;

	double v = 1.0e30;
	setRegisters({
		{PPC_REG_F2, v},
	});

	emulate("fctiwz 1, 2");

	EXPECT_EQ(0x000000007fffffffull, ppcDoubleRegBits(getRegisterValueDouble(PPC_REG_F1)));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCTIWZ_saturates_low)
{
	ALL_MODES;

	double v = -1.0e30;
	setRegisters({
		{PPC_REG_F2, v},
	});

	emulate("fctiwz 1, 2");

	// 0x80000000 sign-extended into the 64-bit FPR.
	EXPECT_EQ(0xffffffff80000000ull, ppcDoubleRegBits(getRegisterValueDouble(PPC_REG_F1)));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCTIWZ_in_range_still_converts)
{
	ALL_MODES;

	double v = -3.7;
	setRegisters({
		{PPC_REG_F2, v},
	});

	emulate("fctiwz 1, 2");

	EXPECT_EQ(0xfffffffffffffffdull, ppcDoubleRegBits(getRegisterValueDouble(PPC_REG_F1)));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCTIWZ)
{
	ALL_MODES;

	// fctiwz leaves an INTEGER in the FPR, not a number: 3.7 truncated to 3,
	// sign-extended to 64 bits, and those bits reinterpreted as a double. The
	// expected value is therefore the double whose bit pattern is 3 -- a
	// denormal, not 3.0. Getting this wrong in the obvious direction (storing
	// 3.0) is exactly what the expectation is here to catch.
	double expected;
	std::uint64_t bits = 3;
	std::memcpy(&expected, &bits, sizeof(expected));

	setRegisters({
		{PPC_REG_F2, 3.7_f64},
	});

	emulate("fctiwz 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F1, expected},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCFID)
{
	ALL_MODES;

	// fcfid is the other direction: the source FPR holds the integer 5 as a
	// bit pattern, and the result is the number 5.0.
	double src;
	std::uint64_t bits = 5;
	std::memcpy(&src, &bits, sizeof(src));

	setRegisters({
		{PPC_REG_F2, src},
	});

	emulate("fcfid 1, 2");

	EXPECT_JUST_REGISTERS_LOADED({PPC_REG_F2});
	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F1, 5.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCPSGN)
{
	ALL_MODES;

	// fcpsgn FRT, FRA, FRB: the sign of FRA, the magnitude of FRB.
	setRegisters({
		{PPC_REG_F1, -1.0},
		{PPC_REG_F2, 3.5_f64},
	});

	emulate("fcpsgn 0, 1, 2");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_F0, -3.5},
	});
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_FCMPO_less_than)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F1, 1.0_f64},
		{PPC_REG_F2, 2.0_f64},
	});

	emulate_bin("fc 01 10 40");

	EXPECT_JUST_REGISTERS_STORED({
		{PPC_REG_CR0LT, true},
		{PPC_REG_CR0GT, false},
		{PPC_REG_CR0EQ, false},
		{PPC_REG_CR0UN, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// Altivec / VSX — gcc -O1. Binary encodings so Keystone does not have to
// know VMX/VSX; Capstone 6 plain PPC32/64 already decodes these (do not
// OR CS_MODE_PWR7 — that drops classic fadd).
//
// vand v2,v3,v4  VX-form opcode 4 VX=1028
// vor  v2,v3,v4  VX=1156
// vxor v2,v2,v2  VX=1220  (the compiler's vector zero)
// lvx  v2,0,r3   X-form opcode 31 XO=103
// stvx v2,0,r3   XO=231
// xsadddp f1,f2,f3  XX3 opcode 60 XO=32 T=1 A=2 B=3  → f0221900
// xsmuldp f1,f2,f3  XX3 XO=48                         → f0221980
// xxlor vs1,vs2,vs2 XX3 XO=146                         → f0221490

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VAND)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0f0f0f0f0f0f0f0fULL, 0xf0f0f0f0f0f0f0f0ULL);
	setV(PPC_REG_V4, 0x00ff00ff00ff00ffULL, 0xff00ff00ff00ff00ULL);

	emulate_bin("10 43 24 04");

	EXPECT_EQ(0x000f000f000f000fULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xf000f000f000f000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VOR)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000000000001ULL, 0x0000000000000002ULL);
	setV(PPC_REG_V4, 0x0000000000000004ULL, 0x0000000000000008ULL);

	emulate_bin("10 43 24 84");

	EXPECT_EQ(0x0000000000000005ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x000000000000000aULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VXOR_zero)
{
	ALL_MODES;

	setV(PPC_REG_V2, 0xdeadbeefdeadbeefULL, 0xcafecafecafecafeULL);

	emulate_bin("10 42 14 c4");

	EXPECT_EQ(0ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSPLTISW_minus_one)
{
	ALL_MODES;

	emulate_bin("10 5f 03 8c");

	EXPECT_EQ(0xffffffffffffffffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xffffffffffffffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LVX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R3, 0x2000},
	});
	setV(PPC_REG_V2, 0, 0);

	setMemoryValueUnsigned(0x2000, 0xfedcba9876543210ULL, 64);
	setMemoryValueUnsigned(0x2008, 0x0123456789abcdefULL, 64);

	emulate_bin("7c 40 18 ce");

	EXPECT_EQ(0xfedcba9876543210ULL, vHigh(PPC_REG_V2)) << dumpFunction(_function);
	EXPECT_EQ(0x0123456789abcdefULL, vLow(PPC_REG_V2)) << dumpFunction(_function);
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_STVX)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R3, 0x2000},
	});
	setV(PPC_REG_V2, 0x1111111111111111ULL, 0x2222222222222222ULL);

	emulate_bin("7c 40 19 ce");

	EXPECT_EQ(0x1111111111111111ULL, getMemoryValueUnsigned(0x2000, 64)) << dumpFunction(_function);
	EXPECT_EQ(0x2222222222222222ULL, getMemoryValueUnsigned(0x2008, 64)) << dumpFunction(_function);
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XSADDDP)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F2, 1.5_f64},
		{PPC_REG_F3, 2.25_f64},
	});

	emulate_bin("f0 22 19 00");

	EXPECT_EQ(3.75, getRegisterValueDouble(PPC_REG_F1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XSMULDP)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_F2, 1.5_f64},
		{PPC_REG_F3, 4.0_f64},
	});

	emulate_bin("f0 22 19 80");

	EXPECT_EQ(6.0, getRegisterValueDouble(PPC_REG_F1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XXLOR_move)
{
	ALL_MODES;

	setV(PPC_REG_VSL2, 0x4022000000000000ULL, 0); // 9.0 in the high doubleword

	emulate_bin("f0 22 14 90");

	EXPECT_EQ(0x4022000000000000ULL, vHigh(PPC_REG_VSL1));
	EXPECT_EQ(0ULL, vLow(PPC_REG_VSL1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VADDFP)
{
	ALL_MODES;

	// 1.0f and 2.0f in each of four lanes: 0x3f800000 and 0x40000000.
	setV(PPC_REG_V3, 0x3f8000003f800000ULL, 0x3f8000003f800000ULL);
	setV(PPC_REG_V4, 0x4000000040000000ULL, 0x4000000040000000ULL);

	emulate_bin("10 43 20 0a");

	EXPECT_EQ(0x4040000040400000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4040000040400000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPERM)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
	setV(PPC_REG_V4, 0x1011121314151617ULL, 0x18191a1b1c1d1e1fULL);
	setV(PPC_REG_V5, 0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);

	emulate_bin("10 43 21 6b");

	EXPECT_EQ(0x0001020304050607ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x08090a0b0c0d0e0fULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSEL)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0xaaaaaaaaaaaaaaaaULL, 0xaaaaaaaaaaaaaaaaULL);
	setV(PPC_REG_V4, 0x5555555555555555ULL, 0x5555555555555555ULL);
	setV(PPC_REG_V5, 0xffffffffffffffffULL, 0x0000000000000000ULL);

	emulate_bin("10 43 21 6a");

	EXPECT_EQ(0x5555555555555555ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVADDDP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V4, 0x4000000000000000ULL, 0x4000000000000000ULL);

	emulate_bin("f0 43 23 07");

	EXPECT_EQ(0x4008000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4008000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVADDSP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3f8000003f800000ULL, 0x3f8000003f800000ULL);
	setV(PPC_REG_V4, 0x4000000040000000ULL, 0x4000000040000000ULL);

	emulate_bin("f0 43 22 07");

	EXPECT_EQ(0x4040000040400000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4040000040400000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMULDP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4010000000000000ULL, 0x4010000000000000ULL);

	emulate_bin("f0 43 23 87");

	EXPECT_EQ(0x4020000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4020000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSLDOI)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
	setV(PPC_REG_V4, 0x1011121314151617ULL, 0x18191a1b1c1d1e1fULL);

	emulate_bin("10 43 22 2c");

	EXPECT_EQ(0x08090a0b0c0d0e0fULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x1011121314151617ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VMRGHW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000011111111ULL, 0x2222222233333333ULL);
	setV(PPC_REG_V4, 0x4444444455555555ULL, 0x6666666677777777ULL);

	emulate_bin("10 43 20 8c");

	EXPECT_EQ(0x0000000044444444ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x1111111155555555ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VMRGLW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000011111111ULL, 0x2222222233333333ULL);
	setV(PPC_REG_V4, 0x4444444455555555ULL, 0x6666666677777777ULL);

	emulate_bin("10 43 21 8c");

	EXPECT_EQ(0x2222222266666666ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x3333333377777777ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VCMPEQUW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000011111111ULL, 0x2222222233333333ULL);
	setV(PPC_REG_V4, 0x00000000ffffffffULL, 0x2222222200000000ULL);

	emulate_bin("10 43 20 86");

	EXPECT_EQ(0xffffffff00000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xffffffff00000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VCMPGTUW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000200000001ULL, 0x0000000000000003ULL);
	setV(PPC_REG_V4, 0x0000000100000002ULL, 0x0000000000000003ULL);

	emulate_bin("10 43 22 86");

	EXPECT_EQ(0xffffffff00000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSPLTW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x1111111122222222ULL, 0x3333333344444444ULL);

	emulate_bin("10 41 1a 8c");

	EXPECT_EQ(0x2222222222222222ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x2222222222222222ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSPLTH)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0xaabbccdd11223344ULL, 0x5566778899aabbccULL);

	emulate_bin("10 40 1a 4c");

	EXPECT_EQ(0xaabbaabbaabbaabbULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xaabbaabbaabbaabbULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSPLTB)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0xaa01020304050607ULL, 0x08090a0b0c0d0e0fULL);

	emulate_bin("10 40 1a 0c");

	EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBUBM)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0505050505050505ULL, 0x0505050505050505ULL);
	setV(PPC_REG_V4, 0x0101010101010101ULL, 0x0101010101010101ULL);

	emulate_bin("10 43 24 00");

	EXPECT_EQ(0x0404040404040404ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0404040404040404ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBUHM)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0005000500050005ULL, 0x0005000500050005ULL);
	setV(PPC_REG_V4, 0x0001000100010001ULL, 0x0001000100010001ULL);

	emulate_bin("10 43 24 40");

	EXPECT_EQ(0x0004000400040004ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0004000400040004ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBUWM)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000500000005ULL, 0x0000000500000005ULL);
	setV(PPC_REG_V4, 0x0000000100000001ULL, 0x0000000100000001ULL);

	emulate_bin("10 43 24 80");

	EXPECT_EQ(0x0000000400000004ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000400000004ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LVSL)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R3, 0x2005},
	});

	emulate_bin("7c 40 18 0c");

	EXPECT_EQ(0x05060708090a0b0cULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0d0e0f1011121314ULL, vLow(PPC_REG_V2));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_LVSR)
{
	ALL_MODES;

	setRegisters({
		{PPC_REG_R3, 0x2005},
	});

	emulate_bin("7c 40 18 4c");

	EXPECT_EQ(0x15161718191a1b1cULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x1d1e1f2021222324ULL, vLow(PPC_REG_V2));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPKUHUM)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0102030405060708ULL, 0x090a0b0c0d0e0f10ULL);
	setV(PPC_REG_V4, 0x1112131415161718ULL, 0x191a1b1c1d1e1f20ULL);

	emulate_bin("10 43 20 0e");

	EXPECT_EQ(0x020406080a0c0e10ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x121416181a1c1e20ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPKUWUM)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
	setV(PPC_REG_V4, 0x1011121314151617ULL, 0x18191a1b1c1d1e1fULL);

	emulate_bin("10 43 20 4e");

	EXPECT_EQ(0x020306070a0b0e0fULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x121316171a1b1e1fULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSL)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
	setV(PPC_REG_V4, 0x0000000000000000ULL, 0x0000000000000004ULL);

	emulate_bin("10 43 21 c4");

	EXPECT_EQ(0x0010203040506070ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x8090a0b0c0d0e0f0ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSR)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
	setV(PPC_REG_V4, 0x0000000000000000ULL, 0x0000000000000004ULL);

	emulate_bin("10 43 22 c4");

	EXPECT_EQ(0x0000102030405060ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x708090a0b0c0d0e0ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

//
// Altivec saturating arith / per-element shifts / VSX FMA — gcc -O2.
// Encodings dumped with Capstone 6 CS_MODE_32|BIG_ENDIAN (v2,v3,v4 / vs34–36).
// vaddsbs VX=768 → 10 43 23 00; xvmaddadp XX3 XO=97 → f0 43 23 0f.
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VADDSBS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x7070808010107f7fULL, 0x80807070f0f01010ULL);
	setV(PPC_REG_V4, 0x2020808020200101ULL, 0x8080101010102020ULL);

	emulate_bin("10 43 23 00");

	EXPECT_EQ(0x7f7f808030307f7fULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x80807f7f00003030ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VADDSHS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x7ff0800010007fffULL, 0x80007ff0f0001000ULL);
	setV(PPC_REG_V4, 0x0020800020000001ULL, 0x8000001010002000ULL);

	emulate_bin("10 43 23 40");

	EXPECT_EQ(0x7fff800030007fffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x80007fff00003000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VADDSWS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x7ffffff080000000ULL, 0x0000001080000000ULL);
	setV(PPC_REG_V4, 0x0000002080000000ULL, 0x0000002080000000ULL);

	emulate_bin("10 43 23 80");

	EXPECT_EQ(0x7fffffff80000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000003080000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VADDUBS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0xf0f01010ffff0000ULL, 0x8080f0f01010ffffULL);
	setV(PPC_REG_V4, 0x2020202000010101ULL, 0x8080202001010001ULL);

	emulate_bin("10 43 22 00");

	EXPECT_EQ(0xffff3030ffff0101ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xffffffff1111ffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VADDUHS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0xfff01000ffff0000ULL, 0x8000fff01000ffffULL);
	setV(PPC_REG_V4, 0x0020200000010001ULL, 0x8000002000010001ULL);

	emulate_bin("10 43 22 40");

	EXPECT_EQ(0xffff3000ffff0001ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xffffffff1001ffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VADDUWS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0xfffffff000000010ULL, 0x80000000ffffffffULL);
	setV(PPC_REG_V4, 0x0000002000000020ULL, 0x8000000000000001ULL);

	emulate_bin("10 43 22 80");

	EXPECT_EQ(0xffffffff00000030ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xffffffffffffffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBSBS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8080707010107f7fULL, 0x7f7f8080f0f01010ULL);
	setV(PPC_REG_V4, 0x0101202010100101ULL, 0x0101808010102020ULL);

	emulate_bin("10 43 27 00");

	EXPECT_EQ(0x8080505000007e7eULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x7e7e0000e0e0f0f0ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBSHS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x80007ff010007fffULL, 0x7fff8000f0001000ULL);
	setV(PPC_REG_V4, 0x0001002010000001ULL, 0x0001800010002000ULL);

	emulate_bin("10 43 27 40");

	EXPECT_EQ(0x80007fd000007ffeULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x7ffe0000e000f000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBSWS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x800000007fffffffULL, 0x0000001080000000ULL);
	setV(PPC_REG_V4, 0x0000000100000001ULL, 0x0000000180000000ULL);

	emulate_bin("10 43 27 80");

	EXPECT_EQ(0x800000007ffffffeULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000f00000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBUBS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x1010ffff20200000ULL, 0x8080f0f01010ffffULL);
	setV(PPC_REG_V4, 0x2020000120200001ULL, 0x0101202010100001ULL);

	emulate_bin("10 43 26 00");

	EXPECT_EQ(0x0000fffe00000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x7f7fd0d00000fffeULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBUHS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x1000ffff20000000ULL, 0x8000fff01000ffffULL);
	setV(PPC_REG_V4, 0x2000000120000001ULL, 0x0001002010000001ULL);

	emulate_bin("10 43 26 40");

	EXPECT_EQ(0x0000fffe00000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x7fffffd00000fffeULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSUBUWS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x00000010ffffffffULL, 0x8000000000000010ULL);
	setV(PPC_REG_V4, 0x0000002000000001ULL, 0x0000000100000020ULL);

	emulate_bin("10 43 26 80");

	EXPECT_EQ(0x00000000fffffffeULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x7fffffff00000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSLB)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x1111111111111111ULL, 0x8080808080808080ULL);
	setV(PPC_REG_V4, 0x0202020202020202ULL, 0x0101010101010101ULL);

	emulate_bin("10 43 21 04");

	EXPECT_EQ(0x4444444444444444ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSLH)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0001000100010001ULL, 0x8000800080008000ULL);
	setV(PPC_REG_V4, 0x0004000400040004ULL, 0x0001000100010001ULL);

	emulate_bin("10 43 21 44");

	EXPECT_EQ(0x0010001000100010ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSLW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000100000001ULL, 0x8000000080000000ULL);
	setV(PPC_REG_V4, 0x0000000400000004ULL, 0x0000000100000001ULL);

	emulate_bin("10 43 21 84");

	EXPECT_EQ(0x0000001000000010ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSRB)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8080808080808080ULL, 0x1111111111111111ULL);
	setV(PPC_REG_V4, 0x0101010101010101ULL, 0x0202020202020202ULL);

	emulate_bin("10 43 22 04");

	EXPECT_EQ(0x4040404040404040ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0404040404040404ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSRH)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8000800080008000ULL, 0x0010001000100010ULL);
	setV(PPC_REG_V4, 0x0001000100010001ULL, 0x0004000400040004ULL);

	emulate_bin("10 43 22 44");

	EXPECT_EQ(0x4000400040004000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0001000100010001ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSRW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8000000080000000ULL, 0x0000001000000010ULL);
	setV(PPC_REG_V4, 0x0000000100000001ULL, 0x0000000400000004ULL);

	emulate_bin("10 43 22 84");

	EXPECT_EQ(0x4000000040000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000100000001ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSRAB)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8080808080808080ULL, 0x1111111111111111ULL);
	setV(PPC_REG_V4, 0x0101010101010101ULL, 0x0202020202020202ULL);

	emulate_bin("10 43 23 04");

	EXPECT_EQ(0xc0c0c0c0c0c0c0c0ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0404040404040404ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSRAH)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8000800080008000ULL, 0x0010001000100010ULL);
	setV(PPC_REG_V4, 0x0001000100010001ULL, 0x0004000400040004ULL);

	emulate_bin("10 43 23 44");

	EXPECT_EQ(0xc000c000c000c000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0001000100010001ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSRAW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8000000080000000ULL, 0x0000001000000010ULL);
	setV(PPC_REG_V4, 0x0000000100000001ULL, 0x0000000400000004ULL);

	emulate_bin("10 43 23 84");

	EXPECT_EQ(0xc0000000c0000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000100000001ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSRAD)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8000000000000000ULL, 0x0000000000000010ULL);
	setV(PPC_REG_V4, 0x0000000000000001ULL, 0x0000000000000004ULL);

	emulate_bin("10 43 23 c4");

	EXPECT_EQ(0xc000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000001ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMADDADP)
{
	ALL_MODES;

	// type-A: XT = XA * XB + XT  →  2*3 + 1 = 7
	setV(PPC_REG_V2, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4008000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 23 0f");

	EXPECT_EQ(0x401c000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x401c000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMADDMDP)
{
	ALL_MODES;

	// type-M: XT = XT * XB + XA  →  1*3 + 2 = 5
	setV(PPC_REG_V2, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4008000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 23 4f");

	EXPECT_EQ(0x4014000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4014000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMADDASP)
{
	ALL_MODES;

	setV(PPC_REG_V2, 0x3f8000003f800000ULL, 0x3f8000003f800000ULL);
	setV(PPC_REG_V3, 0x4000000040000000ULL, 0x4000000040000000ULL);
	setV(PPC_REG_V4, 0x4040000040400000ULL, 0x4040000040400000ULL);

	emulate_bin("f0 43 22 0f");

	EXPECT_EQ(0x40e0000040e00000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x40e0000040e00000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMADDMSP)
{
	ALL_MODES;

	setV(PPC_REG_V2, 0x3f8000003f800000ULL, 0x3f8000003f800000ULL);
	setV(PPC_REG_V3, 0x4000000040000000ULL, 0x4000000040000000ULL);
	setV(PPC_REG_V4, 0x4040000040400000ULL, 0x4040000040400000ULL);

	emulate_bin("f0 43 22 4f");

	EXPECT_EQ(0x40a0000040a00000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x40a0000040a00000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMSUBADP)
{
	ALL_MODES;

	// type-A: XT = XA * XB - XT  →  6 - 1 = 5
	setV(PPC_REG_V2, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4008000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 23 8f");

	EXPECT_EQ(0x4014000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4014000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMSUBMDP)
{
	ALL_MODES;

	// type-M: XT = XT * XB - XA  →  3 - 2 = 1
	setV(PPC_REG_V2, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4008000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 23 cf");

	EXPECT_EQ(0x3ff0000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x3ff0000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNMSUBADP)
{
	ALL_MODES;

	// type-A nmsub: XT = XT - XA * XB  →  1 - 6 = -5
	setV(PPC_REG_V2, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4008000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 27 8f");

	EXPECT_EQ(0xc014000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc014000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNMSUBMDP)
{
	ALL_MODES;

	// type-M nmsub: XT = XA - XT * XB  →  2 - 3 = -1
	setV(PPC_REG_V2, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4008000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 27 cf");

	EXPECT_EQ(0xbff0000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xbff0000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

//
// Leftover Altivec/VSX — vsld/vsrd, xvnmadd*, vcmpeqfp/vcmpgtfp/xvcmpeqdp,
// saturating pack. Encodings dumped with Capstone 6 CS_MODE_32|BIG_ENDIAN
// (v2,v3,v4 / vs34–36). vsld VX=1476 → 10 43 25 c4; xvnmaddadp XX3 XO=225
// → f0 43 27 0f; vpkshss VX=398 → 10 43 21 8e.
//

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSLD)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000000000001ULL, 0x8000000000000000ULL);
	setV(PPC_REG_V4, 0x0000000000000004ULL, 0x0000000000000001ULL);

	emulate_bin("10 43 25 c4");

	EXPECT_EQ(0x0000000000000010ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VSRD)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x8000000000000000ULL, 0x0000000000000010ULL);
	setV(PPC_REG_V4, 0x0000000000000001ULL, 0x0000000000000004ULL);

	emulate_bin("10 43 26 c4");

	EXPECT_EQ(0x4000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000001ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNMADDADP)
{
	ALL_MODES;

	// type-A nmadd: XT = -(XA * XB + XT)  →  -(6 + 1) = -7
	setV(PPC_REG_V2, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4008000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 27 0f");

	EXPECT_EQ(0xc01c000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc01c000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNMADDMDP)
{
	ALL_MODES;

	// type-M nmadd: XT = -(XT * XB + XA)  →  -(3 + 2) = -5
	setV(PPC_REG_V2, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x4008000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 27 4f");

	EXPECT_EQ(0xc014000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc014000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNMADDASP)
{
	ALL_MODES;

	setV(PPC_REG_V2, 0x3f8000003f800000ULL, 0x3f8000003f800000ULL);
	setV(PPC_REG_V3, 0x4000000040000000ULL, 0x4000000040000000ULL);
	setV(PPC_REG_V4, 0x4040000040400000ULL, 0x4040000040400000ULL);

	emulate_bin("f0 43 26 0f");

	EXPECT_EQ(0xc0e00000c0e00000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc0e00000c0e00000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNMADDMSP)
{
	ALL_MODES;

	setV(PPC_REG_V2, 0x3f8000003f800000ULL, 0x3f8000003f800000ULL);
	setV(PPC_REG_V3, 0x4000000040000000ULL, 0x4000000040000000ULL);
	setV(PPC_REG_V4, 0x4040000040400000ULL, 0x4040000040400000ULL);

	emulate_bin("f0 43 26 4f");

	EXPECT_EQ(0xc0a00000c0a00000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc0a00000c0a00000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VCMPEQFP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3f80000040000000ULL, 0x3f80000040400000ULL);
	setV(PPC_REG_V4, 0x3f80000040000000ULL, 0x0000000040400000ULL);

	emulate_bin("10 43 20 c6");

	EXPECT_EQ(0xffffffffffffffffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x00000000ffffffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VCMPGTFP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x400000003f800000ULL, 0x4040000040400000ULL);
	setV(PPC_REG_V4, 0x3f80000040000000ULL, 0x4040000040000000ULL);

	emulate_bin("10 43 22 c6");

	EXPECT_EQ(0xffffffff00000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x00000000ffffffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVCMPEQDP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3ff0000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x3ff0000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 23 1f");

	EXPECT_EQ(0xffffffffffffffffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPKSHSS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x007F0080FF80FF7FULL, 0x000000FF007F8000ULL);
	setV(PPC_REG_V4, 0x0001007FFF00FF80ULL, 0x0080007F0100FFFFULL);

	emulate_bin("10 43 21 8e");

	EXPECT_EQ(0x7F7F8080007F7F80ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x017F80807F7F7FFFULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPKUHUS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x00FF0100007FFFFFULL, 0x0000000100FF0100ULL);
	setV(PPC_REG_V4, 0x0002008000FF0101ULL, 0x00FE00FF0102FFFFULL);

	emulate_bin("10 43 20 8e");

	EXPECT_EQ(0xFFFF7FFF0001FFFFULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0280FFFFFEFFFFFFULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPKUWUS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x000100000000FFFFULL, 0x0000000100008000ULL);
	setV(PPC_REG_V4, 0x0000FFFE00010001ULL, 0x0000000000010000ULL);

	emulate_bin("10 43 20 ce");

	EXPECT_EQ(0xFFFFFFFF00018000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xFFFEFFFF0000FFFFULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPKSHUS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x000100FFFF000100ULL, 0x007F0080FFFF0000ULL);
	setV(PPC_REG_V4, 0x00000002FFFF0200ULL, 0x00FE00FF01008000ULL);

	emulate_bin("10 43 21 0e");

	EXPECT_EQ(0x01FF00FF7F800000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x000200FFFEFFFF00ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPKSWSS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000100007FFFULL, 0x00008000FFFF8000ULL);
	setV(PPC_REG_V4, 0xFFFF7FFF00010000ULL, 0x8000000000000000ULL);

	emulate_bin("10 43 21 ce");

	EXPECT_EQ(0x00017FFF7FFF8000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x80007FFF80000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VPKSWUS)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x000000010000FFFFULL, 0x00010000FFFFFFFFULL);
	setV(PPC_REG_V4, 0x0000000000000002ULL, 0xFFFFFFFE00011170ULL);

	emulate_bin("10 43 21 4e");

	EXPECT_EQ(0x0001FFFFFFFF0000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x000000020000FFFFULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVCMPEQSP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3f80000040000000ULL, 0x3f80000040400000ULL);
	setV(PPC_REG_V4, 0x3f80000040000000ULL, 0x0000000040400000ULL);

	emulate_bin("f0 43 22 1f");

	EXPECT_EQ(0xffffffffffffffffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x00000000ffffffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVCMPGTSP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x400000003f800000ULL, 0x4040000040400000ULL);
	setV(PPC_REG_V4, 0x3f80000040000000ULL, 0x4040000040000000ULL);

	emulate_bin("f0 43 22 5f");

	EXPECT_EQ(0xffffffff00000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x00000000ffffffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVCMPGESP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x400000003f800000ULL, 0x4040000040400000ULL);
	setV(PPC_REG_V4, 0x3f80000040000000ULL, 0x4040000040000000ULL);

	emulate_bin("f0 43 22 9f");

	EXPECT_EQ(0xffffffff00000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xffffffffffffffffULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVCMPGTDP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x4000000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V4, 0x3ff0000000000000ULL, 0x4000000000000000ULL);

	emulate_bin("f0 43 23 5f");

	EXPECT_EQ(0xffffffffffffffffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVCMPGEDP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3ff0000000000000ULL, 0x4000000000000000ULL);
	setV(PPC_REG_V4, 0x3ff0000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 43 23 9f");

	EXPECT_EQ(0xffffffffffffffffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVABSDP)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0xc000000000000000ULL, 0x4008000000000000ULL);

	emulate_bin("f0 40 27 67");

	EXPECT_EQ(0x4000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4008000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNEGDP)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x4000000000000000ULL, 0xc008000000000000ULL);

	emulate_bin("f0 40 27 e7");

	EXPECT_EQ(0xc000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4008000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVABSSP)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0xbf80000040000000ULL, 0xc04000003f800000ULL);

	emulate_bin("f0 40 26 67");

	EXPECT_EQ(0x3f80000040000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x404000003f800000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNEGSP)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3f800000c0000000ULL, 0x40400000bf800000ULL);

	emulate_bin("f0 40 26 e7");

	EXPECT_EQ(0xbf80000040000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc04000003f800000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VCFSX)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x0000000100000002ULL, 0xFFFFFFFF00000000ULL);

	emulate_bin("10 40 23 4a");

	EXPECT_EQ(0x3f80000040000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xbf80000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VCTUXS)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3f8000004f800000ULL, 0xc080000040600000ULL);

	emulate_bin("10 40 23 8a");

	EXPECT_EQ(0x00000001ffffffffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000000000003ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VRFIN)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3fe00000bfe00000ULL, 0x3e80000040500000ULL);

	emulate_bin("10 40 22 0a");

	EXPECT_EQ(0x40000000c0000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000040400000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VRFIZ)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3fe00000bfe00000ULL, 0x3e80000040500000ULL);

	emulate_bin("10 40 22 4a");

	EXPECT_EQ(0x3f800000bf800000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000040400000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VRFIP)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3fe00000bfe00000ULL, 0x3e80000040500000ULL);

	emulate_bin("10 40 22 8a");

	EXPECT_EQ(0x40000000bf800000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x3f80000040800000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VRFIM)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3fe00000bfe00000ULL, 0x3e80000040500000ULL);

	emulate_bin("10 40 22 ca");

	EXPECT_EQ(0x3f800000c0000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000040400000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMAXDP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3ff0000000000000ULL, 0x4008000000000000ULL);
	setV(PPC_REG_V4, 0x4000000000000000ULL, 0x3ff8000000000000ULL);

	emulate_bin("f0 43 27 07");

	EXPECT_EQ(0x4000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4008000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMINDP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3ff0000000000000ULL, 0x4008000000000000ULL);
	setV(PPC_REG_V4, 0x4000000000000000ULL, 0x3ff8000000000000ULL);

	emulate_bin("f0 43 27 47");

	EXPECT_EQ(0x3ff0000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x3ff8000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMAXSP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3f80000040800000ULL, 0x3f00000040400000ULL);
	setV(PPC_REG_V4, 0x400000003f800000ULL, 0x3f80000040000000ULL);

	emulate_bin("f0 43 26 07");

	EXPECT_EQ(0x4000000040800000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x3f80000040400000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVMINSP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x3f80000040800000ULL, 0x3f00000040400000ULL);
	setV(PPC_REG_V4, 0x400000003f800000ULL, 0x3f80000040000000ULL);

	emulate_bin("f0 43 26 47");

	EXPECT_EQ(0x3f8000003f800000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x3f00000040000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVCPSGNDP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0xbff0000000000000ULL, 0x3ff0000000000000ULL);
	setV(PPC_REG_V4, 0x4000000000000000ULL, 0xc010000000000000ULL);

	emulate_bin("f0 43 27 87");

	EXPECT_EQ(0xc000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4010000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVCPSGNSP)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0xbf8000003f800000ULL, 0xbf8000003f800000ULL);
	setV(PPC_REG_V4, 0x40000000c0800000ULL, 0x3f000000c0400000ULL);

	emulate_bin("f0 43 26 87");

	EXPECT_EQ(0xc000000040800000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xbf00000040400000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNABSDP)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x4000000000000000ULL, 0xc008000000000000ULL);

	emulate_bin("f0 40 27 a7");

	EXPECT_EQ(0xc000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc008000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVNABSSP)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3f800000c0000000ULL, 0x40400000c0800000ULL);

	emulate_bin("f0 40 26 a7");

	EXPECT_EQ(0xbf800000c0000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc0400000c0800000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVRDPI)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3ffc000000000000ULL, 0xbffc000000000000ULL);

	emulate_bin("f0 40 23 27");

	EXPECT_EQ(0x4000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc000000000000000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVRDPIC)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3ffc000000000000ULL, 0xbffc000000000000ULL);

	emulate_bin("f0 40 23 af");

	EXPECT_EQ(0x4000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc000000000000000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVRDPIM)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3ffc000000000000ULL, 0xbffc000000000000ULL);

	emulate_bin("f0 40 23 e7");

	EXPECT_EQ(0x3ff0000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xc000000000000000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVRDPIP)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3ffc000000000000ULL, 0xbffc000000000000ULL);

	emulate_bin("f0 40 23 a7");

	EXPECT_EQ(0x4000000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xbff0000000000000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_XVRDPIZ)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3ffc000000000000ULL, 0xbffc000000000000ULL);

	emulate_bin("f0 40 23 67");

	EXPECT_EQ(0x3ff0000000000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xbff0000000000000ULL, vLow(PPC_REG_V2));
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VCFUX)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x0000000100000002ULL, 0xFFFFFFFF00000000ULL);

	emulate_bin("10 40 23 0a");

	EXPECT_EQ(0x3f80000040000000ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x4f80000000000000ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VCTSXS)
{
	ALL_MODES;

	setV(PPC_REG_V4, 0x3f8000004f800000ULL, 0xc080000040600000ULL);

	emulate_bin("10 40 23 ca");

	EXPECT_EQ(0x000000017fffffffULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0xfffffffc00000003ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VRLB)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0101010101010101ULL, 0x8080808080808080ULL);
	setV(PPC_REG_V4, 0x0404040404040404ULL, 0x0101010101010101ULL);

	emulate_bin("10 43 20 04");

	EXPECT_EQ(0x1010101010101010ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0101010101010101ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VRLH)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0001000100010001ULL, 0x8000800080008000ULL);
	setV(PPC_REG_V4, 0x0004000400040004ULL, 0x0001000100010001ULL);

	emulate_bin("10 43 20 44");

	EXPECT_EQ(0x0010001000100010ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0001000100010001ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorPowerpcTests, PPC_INS_VRLW)
{
	ALL_MODES;

	setV(PPC_REG_V3, 0x0000000100000001ULL, 0x8000000080000000ULL);
	setV(PPC_REG_V4, 0x0000000400000004ULL, 0x0000000100000001ULL);

	emulate_bin("10 43 20 84");

	EXPECT_EQ(0x0000001000000010ULL, vHigh(PPC_REG_V2));
	EXPECT_EQ(0x0000000100000001ULL, vLow(PPC_REG_V2));
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
