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

		// s0 and s1 ARE d0: since the translator stopped holding them
		// separately, a test that names an S register is naming half of a D
		// register, and the fixture has to address it the same way the
		// translator does. getRegister() maps to the parent so that the
		// EXPECT_*_REGISTERS_* comparisons -- which compare globals -- line
		// up, and the float accessors carry the offset.
		static bool isSingleView(uint32_t reg)
		{
			return reg >= ARM_REG_S0 && reg <= ARM_REG_S31;
		}

		static uint32_t singleParent(uint32_t reg, unsigned& offset)
		{
			unsigned n = reg - ARM_REG_S0;
			offset = (n % 2) * 32;
			return ARM_REG_D0 + n / 2;
		}

		virtual llvm::GlobalVariable* getRegister(uint32_t reg) override
		{
			unsigned offset = 0;
			return _translator->getRegister(isSingleView(reg) ? singleParent(reg, offset) : reg);
		}

		/// The raw bits of a D register. Its global is typed f64, so the
		/// value lives in the GenericValue's DoubleVal and reading IntVal
		/// answers zero.
		uint64_t dBits(uint32_t reg)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			double d = _emulator->getGlobalVariableValue(gv).DoubleVal;
			uint64_t bits;
			std::memcpy(&bits, &d, sizeof bits);
			return bits;
		}

		virtual float getRegisterValueFloat(uint32_t reg) override
		{
			if (!isSingleView(reg))
			{
				return Capstone2LlvmIrTranslatorTests::getRegisterValueFloat(reg);
			}
			unsigned offset = 0;
			auto* gv = getRegister(singleParent(reg, offset));
			double d = _emulator->getGlobalVariableValue(gv).DoubleVal;
			uint64_t bits;
			std::memcpy(&bits, &d, sizeof bits);
			uint32_t half = static_cast<uint32_t>(bits >> offset);
			float f;
			std::memcpy(&f, &half, sizeof f);
			return f;
		}

		virtual double getRegisterValueDouble(uint32_t reg) override
		{
			return isSingleView(reg) ? static_cast<double>(getRegisterValueFloat(reg))
									 : Capstone2LlvmIrTranslatorTests::getRegisterValueDouble(reg);
		}

		virtual void setRegisterValueFloat(uint32_t reg, float val) override
		{
			if (!isSingleView(reg))
			{
				Capstone2LlvmIrTranslatorTests::setRegisterValueFloat(reg, val);
				return;
			}
			unsigned offset = 0;
			auto* gv = getRegister(singleParent(reg, offset));
			llvm::GenericValue v = _emulator->getGlobalVariableValue(gv);
			uint64_t bits;
			std::memcpy(&bits, &v.DoubleVal, sizeof bits);
			uint32_t half;
			std::memcpy(&half, &val, sizeof half);
			bits = (bits & ~(0xffffffffULL << offset)) | (static_cast<uint64_t>(half) << offset);
			std::memcpy(&v.DoubleVal, &bits, sizeof v.DoubleVal);
			_emulator->setGlobalVariableValue(gv, v);
		}

		virtual void setRegisterValueDouble(uint32_t reg, double val) override
		{
			if (isSingleView(reg))
			{
				setRegisterValueFloat(reg, static_cast<float>(val));
				return;
			}
			Capstone2LlvmIrTranslatorTests::setRegisterValueDouble(reg, val);
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

	// CPSR_C is now LOADED as well as stored. A register-controlled shift
	// by zero must leave the carry exactly as it was, and a flag cannot be
	// left alone without being read. The immediate forms still do not read
	// it: their count is known at translation time, so the zero case is
	// decided there rather than selected.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3, ARM_REG_CPSR_C});
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

	// CPSR_C is now LOADED as well as stored. A register-controlled shift
	// by zero must leave the carry exactly as it was, and a flag cannot be
	// left alone without being read. The immediate forms still do not read
	// it: their count is known at translation time, so the zero case is
	// decided there rather than selected.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3, ARM_REG_CPSR_C});
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

	// CPSR_C is now LOADED as well as stored. A register-controlled shift
	// by zero must leave the carry exactly as it was, and a flag cannot be
	// left alone without being read. The immediate forms still do not read
	// it: their count is known at translation time, so the zero case is
	// decided there rather than selected.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3, ARM_REG_CPSR_C});
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

	// CPSR_C is now LOADED as well as stored. A register-controlled shift
	// by zero must leave the carry exactly as it was, and a flag cannot be
	// left alone without being read. The immediate forms still do not read
	// it: their count is known at translation time, so the zero case is
	// decided there rather than selected.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3, ARM_REG_CPSR_C});
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
		// Capstone 6.x reports the architectural S bit: ADC without S does
		// not write NZCV. Capstone 5.x wrongly set update_flags here.
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
		// Capstone 6.x reports the architectural S bit: ADC without S does
		// not write NZCV. Capstone 5.x wrongly set update_flags here.
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
		// Capstone 6.x reports the architectural S bit: SBC without S does
		// not write NZCV. Capstone 5.x wrongly set update_flags here.
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
		// Capstone 6.x reports the architectural S bit: SBC without S does
		// not write NZCV. Capstone 5.x wrongly set update_flags here.
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
		// 0x1235 - 4 - 1 does not borrow, and ARM's carry out is NOT borrow.
		// This was `false` with a comment asking whether it was right; it was
		// not, and it matched a bug in generateBorrowSubC().
		{ARM_REG_CPSR_C, true},
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
		// Capstone 6.x reports the architectural S bit: RSC without S does
		// not write NZCV. Capstone 5.x wrongly set update_flags here.
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
		// Capstone 6.x reports the architectural S bit: RSC without S does
		// not write NZCV. Capstone 5.x wrongly set update_flags here.
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
		// 0x1235 - 4 - 1 does not borrow, and ARM's carry out is NOT borrow.
		// This was `false` with a comment asking whether it was right; it was
		// not, and it matched a bug in generateBorrowSubC().
		{ARM_REG_CPSR_C, true},
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

// `[Rn, -Rm]` -- the minus is part of the ENCODING, not a negative value in
// Rm. capstone reports it as operand.subtracted, and mem.scale stays 1, so the
// mem.scale == -1 branch this translator used never fired and the offset was
// ADDED. The test below is named "minus_reg" but assembles `[r1, r2]!` with a
// negative value in r2, which exercises nothing of that path.
// The writeback must move the base by the SAME amount the address used. It
// reloaded the bare index register, so the shift and the sign were dropped and
// the two disagreed about their own addressing mode. There is no test anywhere
// for `[Rn, Rm, lsl #N]!`.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_preindexed_writeback_keeps_the_shift)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
		{ARM_REG_R2, 0x4},
	});
	setMemory({
		{0x1010, 0xfeedface_dw},
	});

	emulate("ldr r0, [r1, r2, lsl #2]!");

	// 0x1000 + (4 << 2) = 0x1010, and r1 must land there too -- not at 0x1004.
	EXPECT_EQ(0xfeedfaceu, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(0x1010u, getRegisterValueUnsigned(ARM_REG_R1));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_preindexed_writeback_keeps_the_sign)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1010},
		{ARM_REG_R2, 0x8},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, -r2]!");

	EXPECT_EQ(0x12345678u, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(0x1008u, getRegisterValueUnsigned(ARM_REG_R1));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_reg_is_subtracted)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1010},
		{ARM_REG_R2, 0x8},
	});
	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [r1, -r2]");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
}

// A negative IMMEDIATE displacement must not be negated twice: capstone puts
// the sign in mem.disp and leaves subtracted clear.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_minus_imm_is_not_negated_twice)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1010},
	});
	setMemory({
		{0x100c, 0xcafebabe_dw},
	});

	emulate("ldr r0, [r1, #-4]");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xcafebabe},
	});
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
	// Rt takes the word at [Rn], which little-endian makes the LOW half
	// of the loaded quadword. Shares translateLdrd with LDRD, and the
	// same correction applies.
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x90abcdef},
		{ARM_REG_R1, 0x12345678},
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

	// Rt takes the word at [Rn] and Rt2 the word at [Rn+4]. Memory here is
	// little-endian, so the low half of the stored quadword is the word at
	// 0x1000 and belongs in r0. This test asserted the reverse, which is the
	// same reading STRD and UMULL in the same file contradict.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x90abcdef},
		{ARM_REG_R1, 0x12345678},
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
	// Rt takes the word at [Rn], which little-endian makes the LOW half
	// of the loaded quadword. Shares translateLdrd with LDRD, and the
	// same correction applies.
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x90abcdef},
		{ARM_REG_R1, 0x12345678},
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

//
// The ARMv6 parallel add and subtract instructions, and the GE flags.
//
// Four byte lanes or two halfword lanes in a general-purpose register. ARM
// needs no NEON register for these -- the lanes are the bytes of r0..r14 --
// which is why hand-written ARMv6 string routines are built out of them, and
// why uqsub8 (1,554), uadd8 (840) and sel (840) are what is left on ARM after
// the bitfield batch.
//
// The plain forms are the ones with the GE flags, and the flags are the whole
// point: without them these are an ordinary wrapping add done four times, and
// SEL -- which reads nothing else -- cannot be translated at all.

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UADD8_sets_the_GE_flags)
{
	ALL_MODES;

	// Lanes 1 and 3 carry out of the byte; lanes 0 and 2 do not.
	setRegisters({
		{ARM_REG_R1, 0xff01ff01},
		{ARM_REG_R2, 0x01010101},
	});

	emulate("uadd8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x00020002},
		{ARM_REG_CPSR_GE0, false},
		{ARM_REG_CPSR_GE1, true},
		{ARM_REG_CPSR_GE2, false},
		{ARM_REG_CPSR_GE3, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// For the unsigned subtract, GE means "did not borrow", which is the unsigned
// a >= b the mnemonic is named after.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_USUB8_sets_the_GE_flags)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0510ff00},
		{ARM_REG_R2, 0x10050010},
	});

	emulate("usub8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf50bfff0},
		{ARM_REG_CPSR_GE0, false},
		{ARM_REG_CPSR_GE1, true},
		{ARM_REG_CPSR_GE2, true},
		{ARM_REG_CPSR_GE3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The signed forms set GE when the lane's SIGNED result is non-negative, which
// is a different question from the unsigned carry: lane 1 here is 0x80 + 0x80,
// which carries out unsigned and is -256 signed.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SADD8_sets_the_GE_flags)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x7f800100},
		{ARM_REG_R2, 0x01800100},
	});

	emulate("sadd8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x80000200},
		{ARM_REG_CPSR_GE0, true},
		{ARM_REG_CPSR_GE1, true},
		{ARM_REG_CPSR_GE2, false},
		{ARM_REG_CPSR_GE3, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// Two lanes, four flags: each halfword sets a pair.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UADD16_sets_the_GE_flags_in_pairs)
{
	ALL_MODES;

	// The low halfword carries, the high one does not.
	setRegisters({
		{ARM_REG_R1, 0x0001ffff},
		{ARM_REG_R2, 0x00010001},
	});

	emulate("uadd16 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x00020000},
		{ARM_REG_CPSR_GE0, true},
		{ARM_REG_CPSR_GE1, true},
		{ARM_REG_CPSR_GE2, false},
		{ARM_REG_CPSR_GE3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The saturating and halving forms set NO flags. If they did, the GE state a
// preceding plain form left would be destroyed and the SEL after it would pick
// the wrong bytes -- which is a whole-idiom failure, not a wrong lane.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQADD8_leaves_the_GE_flags_alone)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x8090a0b0},
		{ARM_REG_R2, 0x70605040},
	});

	emulate("uqadd8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf0f0f0f0},
	});
	EXPECT_NO_VALUE_CALLED();
}

// The exchange pair, on operands whose halves differ, because for equal halves
// ASX and SAX answer the same thing.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UASX_subtracts_the_low_lane)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x00500100},
		{ARM_REG_R2, 0x00200030},
	});

	// low  = 0x0100 - 0x0020 = 0x00e0, no borrow, so its pair of GE flags set
	// high = 0x0050 + 0x0030 = 0x0080, no carry, so its pair does not
	emulate("uasx r0, r1, r2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x008000e0},
		{ARM_REG_CPSR_GE0, true},
		{ARM_REG_CPSR_GE1, true},
		{ARM_REG_CPSR_GE2, false},
		{ARM_REG_CPSR_GE3, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_USAX_adds_the_low_lane)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x00500100},
		{ARM_REG_R2, 0x00200030},
	});

	// low  = 0x0100 + 0x0020 = 0x0120 -- the OTHER half of r2, which is what
	// makes this the exchange form; the straight usub16 would use 0x0030
	// high = 0x0050 - 0x0030 = 0x0020
	emulate("usax r0, r1, r2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x00200120},
		{ARM_REG_CPSR_GE0, false},
		{ARM_REG_CPSR_GE1, false},
		{ARM_REG_CPSR_GE2, true},
		{ARM_REG_CPSR_GE3, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

// The pair, which is the only shape either half appears in: a parallel compare
// that leaves the GE flags, then a SEL that reads them. Without the flags
// neither instruction means anything on its own.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_parallel_compare_then_select)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xff01ff01},
		{ARM_REG_R2, 0x01010101},
		{ARM_REG_R4, 0xaabbccdd},
		{ARM_REG_R5, 0x11223344},
	});

	emulate("uadd8 r0, r1, r2");
	emulate("sel r3, r4, r5");

	EXPECT_EQ(0xaa22cc44, getRegisterValueUnsigned(ARM_REG_R3));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQADD8)
{
	ALL_MODES;

	// Saturating: every byte sum exceeds 0xff and clamps there rather than
	// wrapping. A wrapping add answers 0xf0f0f0f0 here too for these lanes --
	// so the operands are chosen with sums of 0xf0 exactly, and UQSUB8 below
	// on the same pair is what separates saturate from wrap.
	setRegisters({
		{ARM_REG_R1, 0x8090a0b0},
		{ARM_REG_R2, 0x70605040},
	});

	emulate("uqadd8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf0f0f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UQADD16
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQADD16)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x8090a0b0},
		{ARM_REG_R2, 0x70605040},
	});

	emulate("uqadd16 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xf0f0f0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UQSUB8
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQSUB8)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x8090a0b0},
		{ARM_REG_R2, 0x70605040},
	});

	emulate("uqsub8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x10305070},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UQADD16
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQSUB16)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x8090a0b0},
		{ARM_REG_R2, 0x70605040},
	});

	emulate("uqsub16 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x10305070},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UQASX
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQASX)
{
	ALL_MODES;

	// ASX subtracts the LOW lane and adds the high one, against the OTHER
	// half of the second operand. USAX below is the same operands the other
	// way round and answers differently.
	setRegisters({
		{ARM_REG_R1, 0x8090a0b0},
		{ARM_REG_R2, 0x70605040},
	});

	emulate("uqasx r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xd0d03050},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UQSAX
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UQSAX)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x8090a0b0},
		{ARM_REG_R2, 0x70605040},
	});

	emulate("uqsax r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x3050ffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_SEL
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SEL)
{
	ALL_MODES;

	// SEL reads the GE flags and nothing else, so it could not be translated
	// until something produced them. GE = 0,1,0,1 picks bytes from r4, r5,
	// r4, r5 counting from the bottom.
	setRegisters({
		{ARM_REG_CPSR_GE0, false},
		{ARM_REG_CPSR_GE1, true},
		{ARM_REG_CPSR_GE2, false},
		{ARM_REG_CPSR_GE3, true},
		{ARM_REG_R1, 0xaabbccdd},
		{ARM_REG_R2, 0x11223344},
	});

	emulate("sel r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED(
		{ARM_REG_R1, ARM_REG_R2, ARM_REG_CPSR_GE0, ARM_REG_CPSR_GE1, ARM_REG_CPSR_GE2, ARM_REG_CPSR_GE3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0xaa22cc44},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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
		{ARM_REG_R0, 0x88},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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
		{ARM_REG_R0, 0x9b44},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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
		{ARM_REG_R0, 255},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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
		{ARM_REG_R0, 0xff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM_INS_UHADD8
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UHADD8)
{
	ALL_MODES;

	// Halving keeps the bit an eight-bit add would have lost: 0x80 + 0x70 is
	// 0xf0, and half of it is 0x78. Computing the sum at eight bits first and
	// then halving gives the same answer only when the sum did not carry.
	setRegisters({
		{ARM_REG_R1, 0x8090a0b0},
		{ARM_REG_R2, 0x70605040},
	});

	emulate("uhadd8 r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x78787878},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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

//
// ARM's register-controlled shifts are not modulo the operand width. The
// count is the low EIGHT bits of the register, 0..255, and every one of those
// is defined. See docs/internal/UNFIXED_AUDIT_FINDINGS.md, Batch AI.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSL_reg_by_the_width_is_zero)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x00000001},
		{ARM_REG_R2, 32},
		{ARM_REG_CPSR_C, false},
	});

	emulate("lsl r0, r1, r2");

	// A shift by exactly the width gives zero, with the carry taking the last
	// bit shifted out -- bit 0. `shl i32 %v, 32` is poison, and the emulator
	// reduces it modulo 32, so this used to answer r1 unchanged.
	EXPECT_EQ(0x0, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSL_reg_past_the_width_clears_the_carry)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0xffffffff},
		{ARM_REG_R2, 33},
		{ARM_REG_CPSR_C, true},
	});

	emulate("lsl r0, r1, r2");

	EXPECT_EQ(0x0, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(0, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSL_reg_by_zero_changes_nothing)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0000abcc}, // bit 0 CLEAR, so a computed carry is 0
		{ARM_REG_R2, 0},
		{ARM_REG_CPSR_C, true},
	});

	emulate("lsl r0, r1, r2");

	// The value passes through AND the carry is left exactly as it was. The
	// old carry path computed `n - 1`, so a count of zero -- the commonest
	// runtime value there is -- gave `shl i32 %val, 0xffffffff`.
	EXPECT_EQ(0x0000abcc, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSL_reg_reads_only_eight_bits_of_the_count)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x0000abcc}, // bit 0 clear, as above
		{ARM_REG_R2, 0x100},      // low eight bits are zero
		{ARM_REG_CPSR_C, true},
	});

	emulate("lsl r0, r1, r2");

	// shift_n is R[s]<7:0>, so 0x100 is a count of zero, not of 256.
	EXPECT_EQ(0x0000abcc, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSR_reg_by_the_width_takes_the_top_bit)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x80000000},
		{ARM_REG_R2, 32},
		{ARM_REG_CPSR_C, false},
	});

	emulate("lsr r0, r1, r2");

	EXPECT_EQ(0x0, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ASR_reg_past_the_width_broadcasts_the_sign)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x80000000},
		{ARM_REG_R2, 40},
		{ARM_REG_CPSR_C, false},
	});

	emulate("asr r0, r1, r2");

	// ASR is the one direction where past the width is not zero: every bit
	// becomes the sign, and the carry is that sign bit.
	EXPECT_EQ(0xffffffff, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ASR_reg_past_the_width_on_a_positive_value)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x7fffffff},
		{ARM_REG_R2, 40},
		{ARM_REG_CPSR_C, true},
	});

	emulate("asr r0, r1, r2");

	EXPECT_EQ(0x0, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(0, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LSL_reg)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x00001234},
		{ARM_REG_R2, 0x10},
	});

	emulate("lsl r0, r1, r2");

	// CPSR_C is now LOADED as well as stored. A register-controlled shift
	// by zero must leave the carry exactly as it was, and a flag cannot be
	// left alone without being read. The immediate forms still do not read
	// it: their count is known at translation time, so the zero case is
	// decided there rather than selected.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_CPSR_C});
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

	// CPSR_C is now LOADED as well as stored. A register-controlled shift
	// by zero must leave the carry exactly as it was, and a flag cannot be
	// left alone without being read. The immediate forms still do not read
	// it: their count is known at translation time, so the zero case is
	// decided there rather than selected.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3, ARM_REG_CPSR_C});
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

	// CPSR_C is now LOADED as well as stored. A register-controlled shift
	// by zero must leave the carry exactly as it was, and a flag cannot be
	// left alone without being read. The immediate forms still do not read
	// it: their count is known at translation time, so the zero case is
	// decided there rather than selected.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3, ARM_REG_CPSR_C});
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

// A register count of zero leaves BOTH the value and the carry alone. The
// only register-form test used a count of 5 -- in range and non-zero -- so
// neither the missing eight-bit mask nor the unconditional carry write could
// show. r2's bit 31 is 0, so a wrongly-written carry is observably different
// from the preserved 1.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ROR_reg_zero_count_preserves_carry)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 0x0},
		{ARM_REG_CPSR_C, true},
	});

	emulate("ror r0, r2, r3");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
		{ARM_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// A count of 32 is NOT a count of zero: it rotates by nothing and still writes
// the carry, from bit 31 of the result. Unmasked, this emitted `lshr i32 %v,
// 32` -- poison.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ROR_reg_count_32_rotates_by_nothing)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R2, 0x12345678},
		{ARM_REG_R3, 32},
		{ARM_REG_CPSR_C, true},
	});

	emulate("ror r0, r2, r3");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678}, {ARM_REG_CPSR_C, false}, // bit 31 of 0x12345678
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// The count is the low EIGHT bits of the register, so 0x105 rotates by 5.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ROR_reg_count_is_eight_bits)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R2, 0x400},
		{ARM_REG_R3, 0x105},
	});

	emulate("ror r0, r2, r3");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x20},
		{ARM_REG_CPSR_C, false},
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

	// CPSR_C is LOADED as well as stored, for the same reason the LSL, LSR and
	// ASR register forms load it: a rotate by zero must leave the carry
	// exactly as it was, and a flag cannot be left alone without being read.
	// ROR was left out of that change and kept writing the carry
	// unconditionally. The immediate forms still do not read it.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R2, ARM_REG_R3, ARM_REG_CPSR_C});
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
		{ARM_REG_R0, 0x34440575},
		{ARM_REG_R1, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
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

/// The raw bits of a float register.
///
/// VCVT-to-integer leaves an INTEGER in an S register, and the fixture's
/// comparison for a float register is EXPECT_NEAR(..., 0.001). The bit pattern
/// of a small integer is a denormal -- 3 is 4.2e-45 -- so every such
/// expectation compares equal to zero and to every other small integer. The
/// VCVT_s32_f64 test below is one of those; it cannot fail on its value. These
/// can.
static uint32_t armFloatRegBits(float f)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &f, sizeof bits);
	return bits;
}

// ARM's FPToFixed(): a NaN converts to ZERO and an out-of-range magnitude
// SATURATES to the destination's minimum or maximum. LLVM's fptosi calls all
// three poison.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCVT_s32_saturates_high)
{
	ALL_MODES;

	double v = 1.0e30;
	setRegisters({
		{ARM_REG_D1, v},
	});

	emulate("vcvt.s32.f64 s0, d1");

	EXPECT_EQ(0x7fffffffu, armFloatRegBits(getRegisterValueFloat(ARM_REG_S0)));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCVT_s32_saturates_low)
{
	ALL_MODES;

	double v = -1.0e30;
	setRegisters({
		{ARM_REG_D1, v},
	});

	emulate("vcvt.s32.f64 s0, d1");

	EXPECT_EQ(0x80000000u, armFloatRegBits(getRegisterValueFloat(ARM_REG_S0)));
}

// The in-range case must still convert -- the guard against over-correcting.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCVT_s32_in_range_still_converts)
{
	ALL_MODES;

	double v = -3.7;
	setRegisters({
		{ARM_REG_D1, v},
	});

	emulate("vcvt.s32.f64 s0, d1");

	// VCVT truncates toward zero: -3.7 -> -3.
	EXPECT_EQ(0xfffffffdu, armFloatRegBits(getRegisterValueFloat(ARM_REG_S0)));
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

	// d0 is loaded as well as stored: s0 is its low half and an ARM
	// sub-register write MERGES, so the other half has to be read to be
	// preserved. That is the whole difference from ARM64, where the same
	// write would zero it.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_D0});
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

	// Even a constant into s0 reads d0 first, because s1 -- the other half --
	// must survive.
	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_D0});
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

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VADD_i32_is_lane_wise)
{
	ALL_MODES;

	// Each word wraps on its own. A register-wide 64-bit add would carry into
	// the high lane and answer 0x0000000100000000.
	double d1;
	double d2;
	uint64_t b1 = 0xffffffffffffffffULL;
	uint64_t b2 = 0x0000000100000001ULL;
	std::memcpy(&d1, &b1, sizeof d1);
	std::memcpy(&d2, &b2, sizeof d2);
	setRegisters({
		{ARM_REG_D1, d1},
		{ARM_REG_D2, d2},
	});

	emulate("vadd.i32 d0, d1, d2");

	EXPECT_EQ(0x0000000000000000ULL, dBits(ARM_REG_D0));
	EXPECT_NO_VALUE_CALLED();
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

//
// ============================================================================
// s0 and s1 are d0
// ============================================================================
//
// ARM's VFP bank overlaps: Dn[31:0] is S2n and Dn[63:32] is S2n+1. They were
// separate globals, so `vldr d0, [r0]` followed by `vmov r1, s0` read storage
// nothing had written -- and the static corpus does exactly that, 9,323
// `vldr d` against 684 `vmov gpr, s`.
//
// The mapping is the ARM ARM's, and gcc's own register allocation agrees: a
// function returning the low float half of a double argument compiles to a
// bare `bx lr`, and the high half to `vmov.f32 s0, s1`.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, A_write_to_s0_is_visible_as_the_low_half_of_d0)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x40490fdb},
	});

	emulate("vmov s0, r1");

	EXPECT_EQ(0x40490fdbULL, dBits(ARM_REG_D0) & 0xffffffffULL);
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, A_write_to_s1_is_visible_as_the_HIGH_half_of_d0)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x40490fdb},
	});

	emulate("vmov s1, r1");

	// s1 is the upper half, not the lower one. This is what makes ARM
	// different from ARM64, where every narrow view is the low end.
	EXPECT_EQ(0x40490fdbULL, dBits(ARM_REG_D0) >> 32);
	EXPECT_EQ(0x0ULL, dBits(ARM_REG_D0) & 0xffffffffULL);
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, Writing_s0_leaves_s1_alone)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_S1, 2.5_f32},
		{ARM_REG_R1, 0x40490fdb},
	});

	emulate("vmov s0, r1");

	// An ARM sub-register write MERGES. Zeroing the rest -- which is the
	// ARM64 rule -- would lose s1.
	EXPECT_EQ(2.5f, getRegisterValueFloat(ARM_REG_S1));
	EXPECT_EQ(0x40490fdbULL, dBits(ARM_REG_D0) & 0xffffffffULL);
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, Writing_s1_leaves_s0_alone)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_S0, 2.5_f32},
		{ARM_REG_R1, 0x40490fdb},
	});

	emulate("vmov s1, r1");

	EXPECT_EQ(2.5f, getRegisterValueFloat(ARM_REG_S0));
	EXPECT_EQ(0x40490fdbULL, dBits(ARM_REG_D0) >> 32);
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, A_d_register_write_is_visible_through_both_s_halves)
{
	ALL_MODES;

	// The double whose bits are 0x400000003f800000: 1.0f in the low half and
	// 2.0f in the high one.
	setRegisters({
		{ARM_REG_D1, 2.000000473111868},
	});

	emulate("vmov.f64 d0, d1");

	// Before s and d shared storage, both of these read zero.
	EXPECT_EQ(1.0f, getRegisterValueFloat(ARM_REG_S0));
	EXPECT_EQ(2.0f, getRegisterValueFloat(ARM_REG_S1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, The_pairing_continues_past_the_first_register)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x40490fdb},
	});

	emulate("vmov s5, r1");

	// s5 is the high half of d2, not of d5 and not the low half of anything.
	EXPECT_EQ(0x40490fdbULL, dBits(ARM_REG_D2) >> 32);
	EXPECT_EQ(0x0ULL, dBits(ARM_REG_D5));
	EXPECT_NO_VALUE_CALLED();
}

//
// SBC's carry out
// ---------------
// ARM's SBC computes `Rn - Rm - NOT(C)` and reports NOT(borrow) in C, which
// is the same ALU operation as x86's SBB with the flag named the other way
// round. So these expectations come from executing the equivalent SBB on this
// machine's CPU and inverting the carry -- the arithmetic is identical, only
// the convention differs.
//
// generateBorrowSubC() answered `(op0 < sub - cf) || (op1 != all ones)`,
// whose second term is true for almost every operand, so with a borrow in the
// carry out was wrong nearly always. x86's SBB had its own copy of the same
// mistake.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, SBC_carry_out_when_it_does_not_borrow)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 5}, {ARM_REG_R2, 3}, {ARM_REG_CPSR_C, false}, // borrow in
	});

	emulate("sbcs r0, r1, r2");

	// 5 - 3 - 1 = 1, no borrow, so C is set.
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_C));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_CPSR_N));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_CPSR_Z));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_CPSR_V));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SBC_carry_out_when_it_does_borrow)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 3}, {ARM_REG_R2, 5}, {ARM_REG_CPSR_C, true}, // no borrow in
	});

	emulate("sbcs r0, r1, r2");

	EXPECT_EQ(0xfffffffeULL, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_CPSR_C));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_N));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SBC_equal_operands_are_decided_by_the_carry_in)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000}, {ARM_REG_R2, 0x1000}, {ARM_REG_CPSR_C, false}, // borrow in
	});

	emulate("sbcs r0, r1, r2");

	// Equal operands with a borrow in: the result is -1 and it borrows, so C
	// is clear. This is the boundary case the old formula could not express
	// without computing `op1 + 1`, which wraps.
	EXPECT_EQ(0xffffffffULL, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_CPSR_C));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_N));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_CPSR_Z));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SBC_equal_operands_without_a_borrow_in_are_zero)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x1000}, {ARM_REG_R2, 0x1000}, {ARM_REG_CPSR_C, true}, // no borrow in
	});

	emulate("sbcs r0, r1, r2");

	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_C));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_Z));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SBC_overflow_is_read_from_the_result)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x80000000}, {ARM_REG_R2, 1}, {ARM_REG_CPSR_C, true}, // no borrow in
	});

	emulate("sbcs r0, r1, r2");

	// The minimum signed value minus one: signed overflow. generateOverflowSubC()
	// asked the question about `result - carry`, a value the instruction never
	// produced.
	EXPECT_EQ(0x7fffffffULL, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_V));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SBC_overflow_with_a_borrow_in_does_not_overflow)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x80000001}, {ARM_REG_R2, 0}, {ARM_REG_CPSR_C, false}, // borrow in
	});

	emulate("sbcs r0, r1, r2");

	// The result is exactly the minimum signed value, which is representable,
	// so V is clear. Asking the overflow question about `result - borrow`
	// instead -- 0x7fffffff, whose sign differs -- answers V set. That is
	// what generateOverflowSubC() used to do, and it needs a borrow in AND a
	// result on the sign boundary to show, which is why the first four SBC
	// tests did not catch it.
	EXPECT_EQ(0x80000000ULL, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_CPSR_V));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_N));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, SBC_overflow_with_a_borrow_in_that_does_overflow)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x80000000}, {ARM_REG_R2, 0}, {ARM_REG_CPSR_C, false}, // borrow in
	});

	emulate("sbcs r0, r1, r2");

	// One less, and it does overflow. The pair pins the boundary from both
	// sides.
	EXPECT_EQ(0x7fffffffULL, getRegisterValueUnsigned(ARM_REG_R0));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_V));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(ARM_REG_CPSR_N));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(ARM_REG_CPSR_C));
}


// The barrier family had no test on any architecture and could not have had
// one -- FenceInst had no visitor in the emulator, so it aborted the process
// rather than failing a test. That this runs at all is the point.
TEST_P(Capstone2LlvmIrTranslatorArmTests, DMB_translates_to_a_fence)
{
	SKIP_MODE_THUMB;

	emulate("dmb sy");

	EXPECT_NO_MEMORY_LOADED_STORED();
}

// The barrier family had no test on any architecture and could not have had
// one -- FenceInst had no visitor in the emulator, so it aborted the process
// rather than failing a test. That this runs at all is the point.
TEST_P(Capstone2LlvmIrTranslatorArmTests, DSB_translates_to_a_fence)
{
	SKIP_MODE_THUMB;

	emulate("dsb sy");

	EXPECT_NO_MEMORY_LOADED_STORED();
}

// The barrier family had no test on any architecture and could not have had
// one -- FenceInst had no visitor in the emulator, so it aborted the process
// rather than failing a test. That this runs at all is the point.
TEST_P(Capstone2LlvmIrTranslatorArmTests, ISB_translates_to_a_fence)
{
	SKIP_MODE_THUMB;

	emulate("isb");

	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// Production-bar compiler subset: ADDW/SUBW, SDIV/UDIV, TBB/TBH, PC-relative
// LDR, BX LR, extend-and-add, PKH, SSAT, SMULBB, NEG, IT, real machine bytes.
//

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_ADDW_r_r_i)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1000},
	});

	emulate("addw r0, r1, #0x123");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1123},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SUBW_r_r_i)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x1123},
	});

	emulate("subw r0, r1, #0x123");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x1000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SDIV_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 20},
		{ARM_REG_R2, 4},
	});

	emulate("sdiv r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 5},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SDIV_by_zero_is_zero)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 20},
		{ARM_REG_R2, 0},
	});

	emulate("sdiv r0, r1, r2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SDIV_int_min_over_minus_one)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x80000000},
		{ARM_REG_R2, 0xffffffff},
	});

	emulate("sdiv r0, r1, r2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x80000000},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UDIV_r_r_r)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 20},
		{ARM_REG_R2, 4},
	});

	emulate("udiv r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 5},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UDIV_arm_bin)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 20},
		{ARM_REG_R2, 4},
	});

	// A32 `udiv r0, r1, r2` = 0xE7310F12
	emulate_bin("12 0f 31 e7");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 5},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SDIV_arm_bin)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 20},
		{ARM_REG_R2, 4},
	});

	// A32 `sdiv r0, r1, r2` = 0xE7110F12
	emulate_bin("12 0f 11 e7");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 5},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TBB)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x2000},
		{ARM_REG_R1, 1},
	});
	setMemory({
		{0x2001, 0x05_b},
	});

	emulate("tbb [r0, r1]", 0x1000);

	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x100e}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TBB_bin)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x2000},
		{ARM_REG_R1, 1},
	});
	setMemory({
		{0x2001, 0x05_b},
	});

	// Thumb-2 `tbb [r0, r1]` = 0xE8D0F001
	emulate_bin("d0 e8 01 f0", 0x1000);

	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x100e}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TBH)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x2000},
		{ARM_REG_R1, 1},
	});
	setMemory({
		{0x2002, 0x0007_w},
	});

	emulate("tbh [r0, r1, lsl #1]", 0x1000);

	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_TBH_bin)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x2000},
		{ARM_REG_R1, 1},
	});
	setMemory({
		{0x2002, 0x0007_w},
	});

	// Thumb-2 `tbh [r0, r1, lsl #1]` = 0xE8D0F011
	emulate_bin("d0 e8 11 f0", 0x1000);

	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_arm_pc_relative)
{
	SKIP_MODE_THUMB;

	setMemory({
		{0x100c, 0x12345678_dw},
	});

	emulate("ldr r0, [pc, #4]", 0x1000);

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x100c});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_arm_pc_relative_bin)
{
	SKIP_MODE_THUMB;

	setMemory({
		{0x100c, 0x12345678_dw},
	});

	// A32 `ldr r0, [pc, #4]` = 0xE59F0004
	emulate_bin("04 00 9f e5", 0x1000);

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_LDR_thumb_pc_relative)
{
	ONLY_MODE_THUMB;

	setMemory({
		{0x1008, 0x12345678_dw},
	});

	emulate("ldr r0, [pc, #4]", 0x1000);

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BX_lr_is_a_return)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_LR, 0x110d8},
	});

	emulate("bx lr", 0x1107C);

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_LR});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x110d8}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SXTAB)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 10},
		{ARM_REG_R1, 0xffffff80},
	});

	emulate("sxtab r2, r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R2, 0xffffff8a},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SXTAH)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 1},
		{ARM_REG_R1, 0xffff8000},
	});

	emulate("sxtah r2, r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R2, 0xffff8001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_UXTAB)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 10},
		{ARM_REG_R1, 0xffffff80},
	});

	emulate("uxtab r2, r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R0, ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R2, 10 + 0x80},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_PKHBT)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x11112222},
		{ARM_REG_R2, 0x33334444},
	});

	emulate("pkhbt r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x33332222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_PKHTB)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x11112222},
		{ARM_REG_R2, 0x33334444},
	});

	emulate("pkhtb r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x11114444},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SSAT)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 200},
	});

	emulate("ssat r0, #8, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 127},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_USAT_negative_saturates_to_zero)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0xffffffff},
	});

	emulate("usat r0, #8, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SMULBB)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x00020003},
		{ARM_REG_R2, 0x00040005},
	});

	emulate("smulbb r0, r1, r2");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 15},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SMLABB)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x00020003},
		{ARM_REG_R2, 0x00040005},
		{ARM_REG_R3, 10},
	});

	emulate("smlabb r0, r1, r2, r3");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1, ARM_REG_R2, ARM_REG_R3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 25},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_NEG)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 5},
	});

	emulate("negs r0, r1");

	EXPECT_JUST_REGISTERS_LOADED({ARM_REG_R1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, static_cast<uint32_t>(-5)},
		{ARM_REG_CPSR_N, true},
		{ARM_REG_CPSR_Z, false},
		{ARM_REG_CPSR_C, false},
		{ARM_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_IT_then_addeq_when_z_is_set)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x10},
		{ARM_REG_CPSR_Z, true},
	});

	emulate("it eq\naddeq r0, r1, #1");

	EXPECT_EQ(0x11u, getRegisterValueUnsigned(ARM_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_IT_then_addeq_when_z_is_clear)
{
	ONLY_MODE_THUMB;

	setRegisters({
		{ARM_REG_R0, 0x20},
		{ARM_REG_R1, 0x10},
		{ARM_REG_CPSR_Z, false},
	});

	emulate("it eq\naddeq r0, r1, #1");

	EXPECT_EQ(0x20u, getRegisterValueUnsigned(ARM_REG_R0));
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_BLX_register_is_annotated)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R1, 0x110d8},
	});

	auto* f = emulate("blx r1", 0x1107C);

	bool annotated = false;
	for (llvm::inst_iterator I = llvm::inst_begin(f), E = llvm::inst_end(f); I != E; ++I)
	{
		if (auto* c = llvm::dyn_cast<llvm::CallInst>(&*I))
		{
			if (c->getMetadata("arm.thumb_call"))
			{
				annotated = true;
			}
		}
	}
	EXPECT_TRUE(annotated);
}

static double armDBits(uint64_t bits)
{
	double d;
	std::memcpy(&d, &bits, sizeof d);
	return d;
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VAND_d)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, armDBits(0xf0f0f0f0f0f0f0f0ULL)},
		{ARM_REG_D2, armDBits(0x0ff00ff00ff00ff0ULL)},
	});

	emulate("vand d0, d1, d2");

	EXPECT_EQ(0x00f000f000f000f0ULL, dBits(ARM_REG_D0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VEOR_d)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, armDBits(0x00000000ffffffffULL)},
		{ARM_REG_D2, armDBits(0x0f0f0f0f0f0f0f0fULL)},
	});

	emulate("veor d0, d1, d2");

	EXPECT_EQ(0x0f0f0f0ff0f0f0f0ULL, dBits(ARM_REG_D0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VADD_q_is_two_d_registers)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D2, armDBits(0xffffffffffffffffULL)},
		{ARM_REG_D3, armDBits(0xffffffffffffffffULL)},
		{ARM_REG_D4, armDBits(0x0000000100000001ULL)},
		{ARM_REG_D5, armDBits(0x0000000100000001ULL)},
	});

	emulate("vadd.i32 q0, q1, q2");

	EXPECT_EQ(0x0000000000000000ULL, dBits(ARM_REG_D0));
	EXPECT_EQ(0x0000000000000000ULL, dBits(ARM_REG_D1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VLD1_d)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1000},
	});
	setMemory({
		{0x1000, 3.5_f64},
	});

	emulate("vld1.64 {d0}, [r0]");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 3.5_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VST1_d)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1000},
		{ARM_REG_D0, 2.25_f64},
	});

	emulate("vst1.64 {d0}, [r0]");

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 2.25_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VLDMIA_d)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0x1000},
	});
	setMemory({
		{0x1000, 1.5_f64},
		{0x1008, 2.5_f64},
	});

	emulate("vldmia r0, {d0, d1}");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_D0, 1.5_f64},
		{ARM_REG_D1, 2.5_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VCEQ_i32)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, armDBits(0x0000000200000001ULL)},
		{ARM_REG_D2, armDBits(0x0000000200000000ULL)},
	});

	emulate("vceq.i32 d0, d1, d2");

	EXPECT_EQ(0xffffffff00000000ULL, dBits(ARM_REG_D0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VDUP_32_from_gpr)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_R0, 0xa5a5a5a5},
	});

	emulate("vdup.32 d0, r0");

	EXPECT_EQ(0xa5a5a5a5a5a5a5a5ULL, dBits(ARM_REG_D0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_QADD)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x7fffffff},
		{ARM_REG_R2, 2},
	});

	emulate("qadd r0, r1, r2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 0x7fffffff},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_SMUAD)
{
	SKIP_MODE_THUMB;

	setRegisters({
		{ARM_REG_R1, 0x00020003},
		{ARM_REG_R2, 0x00040005},
	});

	emulate("smuad r0, r1, r2");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM_REG_R0, 23},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArmTests, ARM_INS_VSHR_u32)
{
	ALL_MODES;

	setRegisters({
		{ARM_REG_D1, armDBits(0x0000000800000004ULL)},
	});

	emulate("vshr.u32 d0, d1, #1");

	EXPECT_EQ(0x0000000400000002ULL, dBits(ARM_REG_D0));
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
