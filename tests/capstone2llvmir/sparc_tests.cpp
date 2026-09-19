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
	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_FB));
	EXPECT_TRUE(_translator->hasDelaySlot(SPARC_INS_BR));
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
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_ICC, GetParam() == CS_MODE_64 ? 0x44 : 0x4},
	});
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

//
// SPARC_INS_SLL  sll %g1, 4, %g2   encoding 85 28 60 04
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_SLL)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x10},
	});

	emulate_bin("85 28 60 04");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G2, 0x100},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_ANDN  andn %g1, %g2, %g3   encoding 86 28 40 02
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_ANDN)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0xff},
		{SPARC_REG_G2, 0x0f},
	});

	emulate_bin("86 28 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0xf0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_MULX  mulx %g1, %g2, %g3   encoding 86 48 40 02 (V9)
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_MULX)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0x10},
		{SPARC_REG_G2, 0x20},
	});

	emulate_bin("86 48 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x200},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FADDS  fadds %f0, %f1, %f2   encoding 85 a0 08 21
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FADDS)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_F0, 1.5f},
		{SPARC_REG_F1, 2.25f},
	});

	emulate_bin("85 a0 08 21");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_F0, SPARC_REG_F1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_F2, 3.75f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FADDD  faddd %f0, %f2, %f4   encoding 89 a0 08 42
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FADDD)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 1.5},
		{SPARC_REG_D1, 2.25},
	});

	emulate_bin("89 a0 08 42");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_D0, SPARC_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 3.75},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FCMPS  fcmps %f0, %f1   encoding 81 a8 0a 21  → fcc=1 (less)
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FCMPS)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_F0, 1.0f},
		{SPARC_REG_F1, 2.0f},
	});

	emulate_bin("81 a8 0a 21");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_F0, SPARC_REG_F1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_FCC0, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ldf [%g1], %f0   encoding c1 00 60 00  (SPARC_INS_LD to an F register)
// 1.5f = 0x3FC00000
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_LD_f_ldf)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
	});
	setMemory({
		{0x1000, 0x3FC00000u},
	});

	emulate_bin("c1 00 60 00");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_F0, 1.5f},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_STD  std %g2, [%g1]   encoding c4 38 60 00
// even word at addr, odd at addr+4.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_STD)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G2, 0xAABBCCDD},
		{SPARC_REG_G3, 0x11223344},
	});

	emulate_bin("c4 38 60 00");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2, SPARC_REG_G3});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xAABBCCDD11223344_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// st %f0, [%g1]  encoding c1 20 60 00  (SPARC_INS_ST from an F register / stf)
// 1.5f = 0x3FC00000
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_ST_f_stf)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_F0, 1.5f},
	});

	emulate_bin("c1 20 60 00");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_F0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x3FC00000u},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ldd [%g1], %f0  encoding c1 18 60 00  (SPARC_INS_LDD to a D register / lddf)
// 1.5 = 0x3FF8000000000000
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_LDD_d_lddf)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
	});
	setMemory({
		{0x1000, 0x3FF8000000000000_qw},
	});

	emulate_bin("c1 18 60 00");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D0, 1.5},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FAND  fand %f0, %f2, %f4   encoding 89 b0 0e 02
// VIS logicals are IMPDEP1 (op3=0x36), opf=0x070 — not FPOP1.
// IEEE 1.0 AND 2.0 bit patterns are disjoint in the exponent, result is +0.
// Capstone 6 token is SPARC_INS_FAND (not FANDD). D0 overlays %f0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FAND)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 1.0},
		{SPARC_REG_D1, 2.0},
	});

	emulate_bin("89 b0 0e 02");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FPADD16  fpadd16 %f0, %f2, %f4   encoding 89 b0 0a 02
// Partitioned 16-bit add (VIS IMPDEP1 opf=0x050). 1.0 + 0.0 keeps IEEE 1.0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FPADD16)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 1.0},
		{SPARC_REG_D1, 0.0},
	});

	emulate_bin("89 b0 0a 02");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 1.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FPSUB16  fpsub16 %f0, %f2, %f4   encoding 89 b0 0a 82
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FPSUB16)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 1.0},
		{SPARC_REG_D1, 0.0},
	});

	emulate_bin("89 b0 0a 82");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 1.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FPADD16S  fpadd16s %f0, %f2, %f4   encoding 89 b0 0a 22
// Two 16-bit lanes in 32-bit F* registers (opf=0x051).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FPADD16S)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_F0, 1.0f},
		{SPARC_REG_F2, 0.0f},
	});

	emulate_bin("89 b0 0a 22");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_F4, 1.0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FPADD32  fpadd32 %f0, %f2, %f4   encoding 89 b0 0a 42
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FPADD32)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 1.0},
		{SPARC_REG_D1, 0.0},
	});

	emulate_bin("89 b0 0a 42");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 1.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FEXPAND  fexpand %f2, %f4   encoding 89 b0 09 a2
// VISInst2 (rs1=0, opf=0x04D). Zero pixels expand to +0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FEXPAND)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_F2, 0.0f},
	});

	emulate_bin("89 b0 09 a2");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FMUL8X16AL  fmul8x16al %f0, %f2, %f4   encoding 89 b0 06 a2
// 8x16 partitioned product vs rs2 low 16 bits (opf=0x035). Zeros stay +0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FMUL8X16AL)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 0.0},
		{SPARC_REG_D1, 1.0},
	});

	emulate_bin("89 b0 06 a2");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FMUL8X16AU  fmul8x16au %f0, %f2, %f4   encoding 89 b0 06 62
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FMUL8X16AU)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 0.0},
		{SPARC_REG_D1, 1.0},
	});

	emulate_bin("89 b0 06 62");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FPMERGE  fpmerge %f0, %f2, %f4   encoding 89 b0 09 62
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FPMERGE)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_F0, 0.0f},
		{SPARC_REG_F2, 0.0f},
	});

	emulate_bin("89 b0 09 62");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_PDIST  pdist %f0, %f2, %f4   encoding 89 b0 07 c2
// Eight 8-bit SAD accumulated into rd (opf=0x03E).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_PDIST)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 0.0},
		{SPARC_REG_D1, 0.0},
		{SPARC_REG_D2, 0.0},
	});

	emulate_bin("89 b0 07 c2");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_PDISTN  pdistn %f0, %f2, %g3   encoding 87 b0 07 e2
// VIS 3: SAD without accumulate, integer rd (opf=0x03F).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_PDISTN)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 0.0},
		{SPARC_REG_D1, 0.0},
	});

	emulate_bin("87 b0 07 e2");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_BSHUFFLE  bshuffle %f0, %f2, %f4   encoding 89 b0 09 82
// GSR.mask 0x89ABCDEF copies rs2 bytes (Capstone 6 opf=0x04C).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_BSHUFFLE)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 0.0},
		{SPARC_REG_D1, 1.0},
		{SPARC_REG_ASR19, 0x89ABCDEF00000000_qw},
	});

	emulate_bin("89 b0 09 82");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 1.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_ARRAY8  array8 %g1, %g2, %g3   encoding 87 b0 42 02
// Blocked 3D address; x_int=3, n=0 → rd=3 (OSA 2015 / QEMU helper_array8).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_ARRAY8)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1800},
		{SPARC_REG_G2, 0},
	});

	emulate_bin("87 b0 42 02");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 3},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_CASA  casa [%g1] 0x80, %g2, %g3   encoding c7 e0 50 02
// Compare-and-swap: if [%g1]==%g3 then [%g1]=%g2; %g3 <- old.
// Capstone 6 has SPARC_INS_CASA / SPARC_INS_CASXA (no SPARC_INS_CASX).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_CASA)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G2, 0xcc},
		{SPARC_REG_G3, 0xaa},
	});
	setMemory({
		{0x1000, 0xaau},
	});

	emulate_bin("c7 e0 50 02");

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xccu},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_CASXA  casxa [%g1] 0x80, %g2, %g3   encoding c7 f0 50 02 (V9)
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_CASXA)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G2, 0x1122334455667788ull},
		{SPARC_REG_G3, 0xaull},
	});
	setMemory({
		{0x1000, 0xa_qw},
	});

	emulate_bin("c7 f0 50 02");

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x1122334455667788_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FPADD64  fpadd64 %f0, %f2, %f4   encoding 89 b0 08 42
// 64-bit integer add on the D* bit pattern (VIS IMPDEP1 opf=0x042).
// 1.0 + 0.0 as i64 keeps IEEE 1.0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FPADD64)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 1.0},
		{SPARC_REG_D1, 0.0},
	});

	emulate_bin("89 b0 08 42");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 1.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FPACKFIX  fpackfix %f2, %f4   encoding 89 b0 07 a2
// Two i32 lanes << GSR.scale, bits [31:16] packed into 32-bit F* (opf=0x03D).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FPACKFIX)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D1, 0.0},
		{SPARC_REG_ASR19, 0},
	});

	emulate_bin("89 b0 07 a2");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_F4, 0.0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FMUL8ULX16  fmul8ulx16 %f0, %f2, %f4   encoding 89 b0 06 e2
// Lower 8 bits of each rs1 16-bit partition (opf=0x037). Zeros stay +0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FMUL8ULX16)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 0.0},
		{SPARC_REG_D1, 1.0},
	});

	emulate_bin("89 b0 06 e2");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FMULD8SUX16  fmuld8sux16 %f0, %f2, %f4   encoding 89 b0 07 02
// Two 8x16 products << 8 into a 64-bit D* dest (opf=0x038).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FMULD8SUX16)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_F0, 0.0f},
		{SPARC_REG_F2, 0.0f},
	});

	emulate_bin("89 b0 07 02");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FMULD8ULX16  fmuld8ulx16 %f0, %f2, %f4   encoding 89 b0 07 22
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FMULD8ULX16)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_F0, 0.0f},
		{SPARC_REG_F2, 0.0f},
	});

	emulate_bin("89 b0 07 22");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FCHKSM16  fchksm16 %f0, %f2, %f4   encoding 89 b0 08 82
// 16-bit lane add then add-fold (opf=0x044). Zeros checksum to +0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FCHKSM16)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 0.0},
		{SPARC_REG_D1, 0.0},
	});

	emulate_bin("89 b0 08 82");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 0.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_POPC  popc %g1, %g3   encoding 87 70 00 01  (V9, llvm.ctpop)
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_POPC)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0b1011},
	});

	emulate_bin("87 70 00 01");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 3},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_LDSTUB  ldstub [%g1], %g3   encoding c6 68 60 00
// Atomic byte: rd <- old, mem <- 0xFF (atomicrmw xchg).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_LDSTUB)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
	});
	setMemory({
		{0x1000, 0xaa_b},
	});

	emulate_bin("c6 68 60 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0xaa},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xff_b},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_LDSTUBA  ldstuba [%g1] #ASI_P, %g3   encoding c6 e8 50 00
// ASI 0x80 primary — same as LDSTUB.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_LDSTUBA)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
	});
	setMemory({
		{0x1000, 0x11_b},
	});

	emulate_bin("c6 e8 50 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x11},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xff_b},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_SWAP  swap [%g1], %g3   encoding c6 78 60 00
// Atomic 32-bit exchange (atomicrmw xchg).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_SWAP)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G3, 0x11223344},
	});
	setMemory({
		{0x1000, 0xaabbccddu},
	});

	emulate_bin("c6 78 60 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0xaabbccdd},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x11223344u},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_SWAPA  swapa [%g1] #ASI_P, %g3   encoding c6 f8 50 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_SWAPA)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G3, 0x55},
	});
	setMemory({
		{0x1000, 0xaau},
	});

	emulate_bin("c6 f8 50 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0xaa},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x55u},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_MOVR  movrz %g1, %g2, %g3   encoding 87 78 44 02
// Integer CMOV: if rs1==0, rd <- rs2. cc lives in cs_sparc.cc (SPARC_CC_REG_*),
// not as an extra operand. Capstone id is SPARC_INS_MOVR; alias SPARC_INS_ALIAS_MOVRZ.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_MOVR)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0},
		{SPARC_REG_G2, 0x55},
		{SPARC_REG_G3, 0x11},
	});

	emulate_bin("87 78 44 02");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x55},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FMOVRS  fmovrsz %g1, %f2, %f4   encoding 89 a8 44 a2
// FP single CMOV on integer rs1==0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FMOVRS)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0},
		{SPARC_REG_F2, 1.0f},
		{SPARC_REG_F4, 2.0f},
	});

	emulate_bin("89 a8 44 a2");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_F4, 1.0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FMOVRD  fmovrdz %g1, %f2, %f4   encoding 89 a8 44 c2
// Capstone names the overlapping D1/D2 containers.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FMOVRD)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0},
		{SPARC_REG_D1, 1.0},
		{SPARC_REG_D2, 2.0},
	});

	emulate_bin("89 a8 44 c2");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 1.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_LDA  lda [%g1] #ASI_P, %g3   encoding c6 80 50 00
// ASI 0x80 primary — same as LD.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_LDA)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
	});
	setMemory({
		{0x1000, 0xAABBCCDDu},
	});

	emulate_bin("c6 80 50 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0xAABBCCDD},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_STA  sta %g3, [%g1] #ASI_P   encoding c6 a0 50 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_STA)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G3, 0xAABBCCDD},
	});

	emulate_bin("c6 a0 50 00");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xAABBCCDDu},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_LDDA  ldda [%g1] #ASI_P, %g2   encoding c4 98 50 00
// Dest is the G2_G3 pair (even=high 32).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_LDDA)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
	});
	setMemory({
		{0x1000, 0x1122334455667788_qw},
	});

	emulate_bin("c4 98 50 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G2, 0x11223344},
		{SPARC_REG_G3, 0x55667788},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_STDA  stda %g2, [%g1] #ASI_P   encoding c4 b8 50 00
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_STDA)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1000},
		{SPARC_REG_G2, 0x11223344},
		{SPARC_REG_G3, 0x55667788},
	});

	emulate_bin("c4 b8 50 00");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x1122334455667788_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FNOR  fnor %f0, %f2, %f4   encoding 89 b0 0c 42
// VIS IMPDEP1 opf=0x062. ~(rs1 | rs2); zeros NOR to all-ones (NaN bit pattern).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FNOR)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 0.0},
		{SPARC_REG_D1, 0.0},
	});

	emulate_bin("89 b0 0c 42");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FNORS  fnors %f0, %f2, %f4   encoding 89 b0 0c 62  (opf=0x063)
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FNORS)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_F0, 0.0f},
		{SPARC_REG_F2, 0.0f},
	});

	emulate_bin("89 b0 0c 62");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_F4, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_FMEAN16  fmean16 %f0, %f2, %f4   encoding 89 b0 08 02
// VIS 3 IMPDEP1 opf=0x040. Per-lane (a+b+1)>>1. IEEE 1.0 mean 1.0 stays 1.0.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FMEAN16)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_D0, 1.0},
		{SPARC_REG_D1, 1.0},
	});

	emulate_bin("89 b0 08 02");

	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_D2, 1.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_TADDCC  taddcc %g1, %g2, %g3   encoding 87 00 40 02
// Tagged add is rs1+rs2 into rd and ICC like ADDCC (arithmetic NZVC).
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_TADDCC)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1234},
		{SPARC_REG_G2, 0x5678},
	});

	emulate_bin("87 00 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x68ac},
		{SPARC_REG_ICC, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_TADDCCTV  taddcctv %g1, %g2, %g3   encoding 87 10 40 02
// Same IR as TADDCC; no trap side effect.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_TADDCCTV)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x1234},
		{SPARC_REG_G2, 0x5678},
	});

	emulate_bin("87 10 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x68ac},
		{SPARC_REG_ICC, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_TSUBCC  tsubcc %g1, %g2, %g3   encoding 87 08 40 02
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_TSUBCC)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x5678},
		{SPARC_REG_G2, 0x1234},
	});

	emulate_bin("87 08 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x4444},
		{SPARC_REG_ICC, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_TSUBCCTV  tsubcctv %g1, %g2, %g3   encoding 87 18 40 02
// Same IR as TSUBCC; no trap side effect.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_TSUBCCTV)
{
	ALL_MODES;

	setRegisters({
		{SPARC_REG_G1, 0x5678},
		{SPARC_REG_G2, 0x1234},
	});

	emulate_bin("87 18 40 02");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0x4444},
		{SPARC_REG_ICC, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_UMULXHI  umulxhi %g1, %g2, %g3   encoding 87 b0 42 c2
// V9 VIS3: 64×64 unsigned multiply, keep high 64 of the 128-bit product.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_UMULXHI)
{
	ONLY_MODE_64;

	setRegisters({
		{SPARC_REG_G1, 0xffffffffffffffff},
		{SPARC_REG_G2, 0xffffffffffffffff},
	});

	emulate_bin("87 b0 42 c2");

	EXPECT_JUST_REGISTERS_LOADED({SPARC_REG_G1, SPARC_REG_G2});
	EXPECT_JUST_REGISTERS_STORED({
		{SPARC_REG_G3, 0xfffffffffffffffe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// SPARC_INS_TA  ta 8   encoding 91 d0 20 08
// Capstone 6: id=SPARC_INS_T, alias SPARC_INS_ALIAS_TA, mnemonic ta, imm trap #.
// Opaque __asm_ta like ARM64 SVC. No throw.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_TA)
{
	ALL_MODES;

	emulate_bin("91 d0 20 08");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_ta"), {8}},
	});
}

//
// SPARC_INS_T  te 8   encoding 83 d0 20 08
// Trap on ICC (equal). Same SVC-shaped pseudo, named __asm_t. No throw.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_T)
{
	ALL_MODES;

	emulate_bin("83 d0 20 08");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_t"), {8}},
	});
}

//
// SPARC_INS_FLUSH  flush %g1   encoding 81 d8 60 00
// I-cache flush is a nop (x86 PREFETCH-class). Do not model I-cache.
//

TEST_P(Capstone2LlvmIrTranslatorSparcTests, SPARC_INS_FLUSH)
{
	ALL_MODES;

	emulate_bin("81 d8 60 00");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
