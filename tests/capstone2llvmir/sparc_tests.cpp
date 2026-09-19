/**
 * @file tests/capstone2llvmir/sparc_tests.cpp
 * @brief Capstone2LlvmIrTranslatorSparc unit tests.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/IR/InstIterator.h>

#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/sparc/sparc.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorSparcTests :
		public Capstone2LlvmIrTranslatorTests,
		public ::testing::WithParamInterface<cs_mode>
{
	protected:
		virtual void initKeystoneEngine() override
		{
			ks_mode mode = KS_MODE_SPARC32;
			switch (GetParam())
			{
				case CS_MODE_32: mode = KS_MODE_SPARC32; break;
				case CS_MODE_64: mode = static_cast<ks_mode>(KS_MODE_SPARC64 | KS_MODE_V9); break;
				default: throw std::runtime_error("ERROR: unknown mode.\n");
			}
			if (ks_open(KS_ARCH_SPARC, mode | KS_MODE_BIG_ENDIAN, &_assembler) != KS_ERR_OK)
			{
				throw std::runtime_error("ERROR: failed on ks_open().\n");
			}
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			switch (GetParam())
			{
				case CS_MODE_32:
					_translator = Capstone2LlvmIrTranslator::createSparc(
							&_module,
							CS_MODE_BIG_ENDIAN);
					break;
				case CS_MODE_64:
					_translator = Capstone2LlvmIrTranslator::createSparc(
							&_module,
							static_cast<cs_mode>(CS_MODE_BIG_ENDIAN | CS_MODE_V9));
					break;
				default:
					throw std::runtime_error("ERROR: unknown mode.\n");
			}
		}

	protected:
#define ALL_MODES
#define ONLY_MODE_32 if (GetParam() != CS_MODE_32) return;
#define ONLY_MODE_64 if (GetParam() != CS_MODE_64) return;
};

struct PrintCapstoneModeToString_Sparc
{
	template <class ParamType>
	std::string operator()(const TestParamInfo<ParamType>& info) const
	{
		switch (info.param)
		{
			case CS_MODE_32: return "CS_MODE_32";
			case CS_MODE_64: return "CS_MODE_64_V9";
			default: return "UNHANDLED CS_MODE";
		}
	}
};

INSTANTIATE_TEST_SUITE_P(
		InstantiateSparcWithAllModes,
		Capstone2LlvmIrTranslatorSparcTests,
		::testing::Values(CS_MODE_32, CS_MODE_64),
		PrintCapstoneModeToString_Sparc());

//
// Delay slots (id-level, same contract as MIPS).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, delay_slot_control_transfers)
{
	ALL_MODES;

	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_B));
	EXPECT_TRUE(_translator->hasDelaySlotTypical(SPARC_INS_B));
	EXPECT_FALSE(_translator->hasDelaySlotLikely(SPARC_INS_B));
	EXPECT_EQ(1u, _translator->getDelaySlot(SPARC_INS_B));

	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_CALL));
	EXPECT_EQ(1u, _translator->getDelaySlot(SPARC_INS_CALL));
	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_JMPL));
	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_JMP));
	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_RETT));
	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_RET));
	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_RETL));

	EXPECT_FALSE(_translator->hasDelaySlot(SPARC_INS_ADD));
	EXPECT_EQ(0u, _translator->getDelaySlot(SPARC_INS_LD));
}

//
// SPARC_INS_ADD  add %g1, %g2, %g3   encoding 86 00 40 02
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_ADD)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1234},
		{SPARC_REG_G2, 0x5678},
	});

	emulate_bin("86 00 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x68ac},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_SUB  sub %g1, %g2, %g3   encoding 86 20 40 02
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_SUB)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x5678},
		{SPARC_REG_G2, 0x1234},
	});

	emulate_bin("86 20 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x4444},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_AND  and %g1, %g2, %g3   encoding 86 08 40 02
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_AND)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0xff00},
		{SPARC_REG_G2, 0x0ff0},
	});

	emulate_bin("86 08 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x0f00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_OR  or %g1, %g2, %g3   encoding 86 10 40 02
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_OR)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0xff00},
		{SPARC_REG_G2, 0x00ff},
	});

	emulate_bin("86 10 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0xffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_XOR  xor %g1, %g2, %g3   encoding 86 18 40 02
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_XOR)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0xff00},
		{SPARC_REG_G2, 0x0ff0},
	});

	emulate_bin("86 18 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0xf0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_SETHI  sethi 0x48d00, %g1  -> 0x12340000   encoding 03 04 8d 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_SETHI)
{
	ALL_MODES;

	emulate_bin("03 04 8d 00");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G1, 0x12340000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_NOP  encoding 01 00 00 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_NOP)
{
	ALL_MODES;

	emulate_bin("01 00 00 00");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_LD  ld [%g1], %g2   encoding c4 00 40 00?  wait c4 00 60 00 with i=1 simm=0
// 11 00010 000000 00001 1 0000000000000 = C4 00 60 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_LD)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
	});
	setMemory({
		{0x1000, 0xAABBCCDDu},
	});

	emulate_bin("c4 00 60 00");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G2, 0xAABBCCDD},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_ST  st %g2, [%g1]   encoding c4 20 60 00
// 11 00010 000100 00001 1 0000000000000 = C4 20 60 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_ST)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G2, 0xAABBCCDD},
	});

	emulate_bin("c4 20 60 00");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xAABBCCDDu},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_LDX  ldx [%g1], %g2   encoding c4 58 60 00 (V9)
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_LDX)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
	});
	setMemory({
		{0x1000, 0x1122334455667788_qw},
	});

	emulate_bin("c4 58 60 00");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G2, 0x1122334455667788ull},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_STX  stx %g2, [%g1]   encoding c4 70 60 00 (V9)
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_STX)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G2, 0x1122334455667788ull},
	});

	emulate_bin("c4 70 60 00");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x1122334455667788_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_B  ba 0x2000 at pc=0x1000   encoding 10 80 04 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_B_always)
{
	ALL_MODES;

	emulate_bin("10 80 04 00", 0x1000);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x2000}},
	});
}

//
// Bicc be: ICC.Z set => taken. encoding 02 80 04 00 at 0x1000 -> 0x2000
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_B_equal_taken)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_ICC, 0x4}, // Z
	});

	emulate_bin("02 80 04 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_ICC});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x2000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_B_equal_not_taken)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_ICC, 0x0},
	});

	emulate_bin("02 80 04 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_ICC});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x2000}},
	});
}

//
// SPARC_INS_CMP then flags: cmp %g1, %g2  encoding 80 a0 40 02
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_CMP_equal_sets_Z)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1234},
		{SPARC_REG_G2, 0x1234},
	});

	emulate_bin("80 a0 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	if (GetParam() == CS_MODE_64)
	{
		EXPECT_JUST_REGISTERS_STORED({
			{SPARC_REG_ICC, 0x4},
			{SPARC_REG_XCC, 0x4},
		});
	}
	else
	{
		EXPECT_JUST_REGISTERS_STORED({
			{SPARC_REG_ICC, 0x4},
		});
	}
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_CALL  call 0x2000 at 0x1000   encoding 40 00 04 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_CALL)
{
	ALL_MODES;

	emulate_bin("40 00 04 00", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_O7, 0x1000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x2000}},
	});
}

//
// SPARC_INS_JMPL as jmp: jmpl %g1, %g0   encoding 81 c0 40 00 (i=0 rs2=g0)
// 10 00000 111000 00001 0 0000000000000 = 81 C0 40 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_JMPL_to_g0)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x2000},
	});

	emulate_bin("81 c0 40 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x2000}},
	});
}

//
// SPARC_INS_RETL  encoding 81 c3 e0 08  (jmpl %o7+8, %g0)
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_RETL)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_O7, 0x1000},
	});

	emulate_bin("81 c3 e0 08");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_O7});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x1008}},
	});
}

//
// SPARC_INS_SAVE  save %g0, %g0, %g0   encoding 81 e0 00 00
// Copies outs -> ins.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_SAVE)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_O0, 0x11},
		{SPARC_REG_O1, 0x22},
		{SPARC_REG_O7, 0x77},
		{SPARC_REG_SP, 0x1000},
	});

	emulate_bin("81 e0 00 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_I0, 0x11},
		{SPARC_REG_I1, 0x22},
		{SPARC_REG_I2, ANY},
		{SPARC_REG_I3, ANY},
		{SPARC_REG_I4, ANY},
		{SPARC_REG_I5, ANY},
		{SPARC_REG_FP, 0x1000},
		{SPARC_REG_I7, 0x77},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_RESTORE  restore %g0, %g0, %g0   encoding 81 e8 00 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_RESTORE)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_I0, 0x11},
		{SPARC_REG_I7, 0x77},
		{SPARC_REG_FP, 0x2000},
	});

	emulate_bin("81 e8 00 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_O0, 0x11},
		{SPARC_REG_O1, ANY},
		{SPARC_REG_O2, ANY},
		{SPARC_REG_O3, ANY},
		{SPARC_REG_O4, ANY},
		{SPARC_REG_O5, ANY},
		{SPARC_REG_SP, 0x2000},
		{SPARC_REG_O7, 0x77},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_RETT  rett %g1+8   encoding 81 c8 60 08
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_RETT)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x2000},
		{SPARC_REG_I0, 0x11},
		{SPARC_REG_FP, 0x3000},
		{SPARC_REG_I7, 0x77},
	});

	emulate_bin("81 c8 60 08");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_O0, 0x11},
		{SPARC_REG_O1, ANY},
		{SPARC_REG_O2, ANY},
		{SPARC_REG_O3, ANY},
		{SPARC_REG_O4, ANY},
		{SPARC_REG_O5, ANY},
		{SPARC_REG_SP, 0x3000},
		{SPARC_REG_O7, 0x77},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x2008}},
	});
}

//
// g0 is hardwired zero: add %g0, %g1, %g2
// encoding 84 00 00 01  (rd=g2=2, rs1=g0, rs2=g1)
// 10 00010 000000 00000 0 0000000000001 = 84 00 00 01
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_REG_G0_is_zero)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G0, 0xdead},
		{SPARC_REG_G1, 0x1234},
	});

	emulate_bin("84 00 00 01");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G2, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
