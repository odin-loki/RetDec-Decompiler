/**
 * @file tests/capstone2llvmir/riscv_tests.cpp
 * @brief Capstone2LlvmIrTranslatorRiscv unit tests.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/IR/InstIterator.h>

#include <cstdint>
#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/riscv/riscv.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorRiscvTests :
		public Capstone2LlvmIrTranslatorTests,
		public ::testing::WithParamInterface<cs_mode>
{
	protected:
		virtual void initKeystoneEngine() override
		{
			// Keystone 0.9.2 has no RISC-V backend. Tests use emulate_bin().
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			switch (GetParam())
			{
				case CS_MODE_RISCV32:
					_translator = Capstone2LlvmIrTranslator::createRiscv32(
							&_module,
							static_cast<cs_mode>(
									CS_MODE_RISCVC | CS_MODE_RISCV_FD | CS_MODE_RISCV_A));
					break;
				case CS_MODE_RISCV64:
					_translator = Capstone2LlvmIrTranslator::createRiscv64(
							&_module,
							static_cast<cs_mode>(
									CS_MODE_RISCVC | CS_MODE_RISCV_FD | CS_MODE_RISCV_A));
					break;
				default:
					throw std::runtime_error("ERROR: unknown mode.\n");
			}
		}

		uint64_t sx32(uint32_t v)
		{
			return GetParam() == CS_MODE_RISCV64
					? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)))
					: v;
		}

#define ALL_MODES
#define ONLY_MODE_RV32 if (GetParam() != CS_MODE_RISCV32) return;
#define ONLY_MODE_RV64 if (GetParam() != CS_MODE_RISCV64) return;
#define SKIP_MODE_RV32 if (GetParam() == CS_MODE_RISCV32) return;
#define SKIP_MODE_RV64 if (GetParam() == CS_MODE_RISCV64) return;
};

struct PrintCapstoneModeToString_Riscv
{
	template <class ParamType>
	std::string operator()(const TestParamInfo<ParamType>& info) const
	{
		switch (info.param)
		{
			case CS_MODE_RISCV32: return "CS_MODE_RISCV32";
			case CS_MODE_RISCV64: return "CS_MODE_RISCV64";
			default: return "UNHANDLED CS_MODE";
		}
	}
};

INSTANTIATE_TEST_SUITE_P(
		InstantiateRiscvWithAllModes,
		Capstone2LlvmIrTranslatorRiscvTests,
		::testing::Values(CS_MODE_RISCV32, CS_MODE_RISCV64),
		PrintCapstoneModeToString_Riscv());

//
// RISCV_INS_LUI  lui a0, 0x12345   bytes 37 55 34 12
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LUI)
{
	ALL_MODES;

	emulate_bin("37 55 34 12");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0x12345000)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_AUIPC  auipc a0, 1  at 0x1000 → a0 = 0x2000
// bytes 17 15 00 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_AUIPC)
{
	ALL_MODES;

	emulate_bin("17 15 00 00", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x2000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_ADDI  addi a0, a1, 5   bytes 13 85 55 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ADDI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1230},
	});

	emulate_bin("13 85 55 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x1235},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// addi x0, x0, 0 is the canonical NOP — x0 writes are discarded.
// bytes 13 00 00 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ADDI_x0_nop)
{
	ALL_MODES;

	emulate_bin("13 00 00 00");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_ADD  add a0, a1, a2   bytes 33 85 c5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ADD)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x10},
		{RISCV_REG_A2, 0x20},
	});

	emulate_bin("33 85 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x30},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SUB  sub a0, a1, a2   bytes 33 85 c5 40
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SUB)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x50},
		{RISCV_REG_A2, 0x20},
	});

	emulate_bin("33 85 c5 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x30},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_AND  and a0, a1, a2   bytes 33 f5 c5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_AND)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0xff0},
		{RISCV_REG_A2, 0x0ff},
	});

	emulate_bin("33 f5 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x0f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_OR  or a0, a1, a2   bytes 33 e5 c5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_OR)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0xf0},
		{RISCV_REG_A2, 0x0f},
	});

	emulate_bin("33 e5 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_XOR  xor a0, a1, a2   bytes 33 c5 c5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_XOR)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0xff},
		{RISCV_REG_A2, 0x0f},
	});

	emulate_bin("33 c5 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xf0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_ANDI  andi a0, a1, 0x0ff   bytes 13 f5 f5 0f
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ANDI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1ff},
	});

	emulate_bin("13 f5 f5 0f");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x0ff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_ORI  ori a0, a1, 0x0f0   bytes 13 e5 05 0f
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ORI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x00f},
	});

	emulate_bin("13 e5 05 0f");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x0ff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_XORI  xori a0, a1, 0x0aa   bytes 13 c5 a5 0a
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_XORI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x0ff},
	});

	emulate_bin("13 c5 a5 0a");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x055},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SLT  slt a0, a1, a2   bytes 33 a5 c5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLT_true)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, sx32(0xffffffff)},
		{RISCV_REG_A2, 0x1},
	});

	emulate_bin("33 a5 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLT_false)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x5},
		{RISCV_REG_A2, 0x1},
	});

	emulate_bin("33 a5 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SLTU  sltu a0, a1, a2   bytes 33 b5 c5 00
// 0xffffffff < 1 unsigned is false
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLTU_false)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0xffffffff},
		{RISCV_REG_A2, 0x1},
	});

	emulate_bin("33 b5 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SLTI  slti a0, a1, 10   bytes 13 a5 a5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLTI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x5},
	});

	emulate_bin("13 a5 a5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SLTIU  sltiu a0, a1, 10   bytes 13 b5 a5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLTIU)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x14},
	});

	emulate_bin("13 b5 a5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SLLI  slli a0, a1, 4   bytes 13 95 45 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLLI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1},
	});

	emulate_bin("13 95 45 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x10},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SRLI  srli a0, a1, 4   bytes 13 d5 45 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SRLI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x100},
	});

	emulate_bin("13 d5 45 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x10},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SRAI  srai a0, a1, 4   bytes 13 d5 45 40
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SRAI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, sx32(0xfffffff0)},
	});

	emulate_bin("13 d5 45 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xffffffff)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SLL  sll a0, a1, a2   bytes 33 95 c5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLL)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x3},
		{RISCV_REG_A2, 0x2},
	});

	emulate_bin("33 95 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SRL  srl a0, a1, a2   bytes 33 d5 c5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SRL)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x20},
		{RISCV_REG_A2, 0x2},
	});

	emulate_bin("33 d5 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x8},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SRA  sra a0, a1, a2   bytes 33 d5 c5 40
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SRA)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, sx32(0xfffffff0)},
		{RISCV_REG_A2, 0x4},
	});

	emulate_bin("33 d5 c5 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xffffffff)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_JAL  jal ra, 8  at 0x1000 → ra=0x1004, call 0x1008
// bytes ef 00 80 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_JAL)
{
	ALL_MODES;

	emulate_bin("ef 00 80 00", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_RA, 0x1004},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1008}},
	});
}

//
// RISCV_INS_JALR  jalr ra, a0, 4   bytes e7 00 45 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_JALR)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x2000},
	});

	emulate_bin("e7 00 45 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_RA, 0x1004},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x2004}},
	});
}

//
// jalr x0, ra, 0  (ret)   bytes 67 80 00 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_JALR_ret)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_RA, 0x4000},
	});

	emulate_bin("67 80 00 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_RA});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0x4000}},
	});
}

//
// RISCV_INS_BEQ  beq a0, a1, 8  at 0x1000 → target 0x1008
// bytes 63 04 b5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BEQ_taken)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x11},
		{RISCV_REG_A1, 0x11},
	});

	emulate_bin("63 04 b5 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1008}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BEQ_not_taken)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x11},
		{RISCV_REG_A1, 0x22},
	});

	emulate_bin("63 04 b5 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1008}},
	});
}

//
// RISCV_INS_BNE  bne a0, a1, 8   bytes 63 14 b5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BNE_taken)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x11},
		{RISCV_REG_A1, 0x22},
	});

	emulate_bin("63 14 b5 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1008}},
	});
}

//
// RISCV_INS_BLT  blt a0, a1, 8   bytes 63 44 b5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BLT_taken)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, sx32(0xffffffff)},
		{RISCV_REG_A1, 0x1},
	});

	emulate_bin("63 44 b5 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1008}},
	});
}

//
// RISCV_INS_BGE  bge a0, a1, 8   bytes 63 54 b5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BGE_taken)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x5},
		{RISCV_REG_A1, 0x5},
	});

	emulate_bin("63 54 b5 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1008}},
	});
}

//
// RISCV_INS_BLTU  bltu a0, a1, 8   bytes 63 64 b5 00
// 0xffffffff < 1 unsigned is false
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BLTU_not_taken)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0xffffffff},
		{RISCV_REG_A1, 0x1},
	});

	emulate_bin("63 64 b5 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1008}},
	});
}

//
// RISCV_INS_BGEU  bgeu a0, a1, 8   bytes 63 74 b5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BGEU_taken)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0xffffffff},
		{RISCV_REG_A1, 0x1},
	});

	emulate_bin("63 74 b5 00", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1008}},
	});
}

//
// RISCV_INS_LW  lw a0, 4(a1)   bytes 03 a5 45 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LW)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1004, 0x12345678_dw},
	});

	emulate_bin("03 a5 45 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0x12345678)},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LW_sext_64)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1004, 0xffff0000_dw},
	});

	emulate_bin("03 a5 45 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xffffffffffff0000ull},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_LH  lh a0, 2(a1)   bytes 03 95 25 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LH)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1002, 0xfff0_w},
	});

	emulate_bin("03 95 25 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xfffffff0)},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1002});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_LHU  lhu a0, 2(a1)   bytes 03 d5 25 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LHU)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1002, 0xfff0_w},
	});

	emulate_bin("03 d5 25 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xfff0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1002});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_LB  lb a0, 1(a1)   bytes 03 85 15 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LB)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1001, 0xfe_b},
	});

	emulate_bin("03 85 15 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xfffffffe)},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1001});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_LBU  lbu a0, 1(a1)   bytes 03 c5 15 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LBU)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1001, 0xfe_b},
	});

	emulate_bin("03 c5 15 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xfe},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1001});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SW  sw a0, 8(a1)   bytes 23 a4 a5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SW)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0xaabbccdd},
		{RISCV_REG_A1, 0x1000},
	});

	emulate_bin("23 a4 a5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0xaabbccdd_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SH  sh a0, 4(a1)   bytes 23 92 a5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SH)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0xaabb},
		{RISCV_REG_A1, 0x1000},
	});

	emulate_bin("23 92 a5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1004, 0xaabb_w},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SB  sb a0, 2(a1)   bytes 23 81 a5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SB)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x5a},
		{RISCV_REG_A1, 0x1000},
	});

	emulate_bin("23 81 a5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1002, 0x5a_b},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_ECALL  bytes 73 00 00 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ECALL)
{
	ALL_MODES;

	emulate_bin("73 00 00 00", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1004}},
	});
}

//
// RV64I OP-32
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ADDW)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x7fffffff},
		{RISCV_REG_A2, 0x1},
	});

	emulate_bin("3b 85 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xffffffff80000000ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ADDIW)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0xffffffff},
	});

	emulate_bin("1b 85 55 00"); // addiw a0, a1, 5

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SUBW)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x10},
		{RISCV_REG_A2, 0x20},
	});

	emulate_bin("3b 85 c5 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xfffffffffffffff0ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLLIW)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x1},
	});

	emulate_bin("1b 95 45 00"); // slliw a0, a1, 4

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x10},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLLW)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x3},
		{RISCV_REG_A2, 0x2},
	});

	emulate_bin("3b 95 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RV64I LD / SD / LWU
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LD)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1008, 0x1122334455667788_qw},
	});

	emulate_bin("03 b5 85 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x1122334455667788ull},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SD)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A0, 0x1122334455667788ull},
		{RISCV_REG_A1, 0x1000},
	});

	emulate_bin("23 b4 a5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1008, 0x1122334455667788_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LWU)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1004, 0xffff0000_dw},
	});

	emulate_bin("03 e5 45 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xffff0000ull},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_MUL  mul a0, a1, a2   bytes 33 85 c5 02
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_MUL)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 6},
		{RISCV_REG_A2, 7},
	});

	emulate_bin("33 85 c5 02");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 42},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_DIV  div a0, a1, a2   bytes 33 c5 c5 02
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_DIV)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 20},
		{RISCV_REG_A2, 5},
	});

	emulate_bin("33 c5 c5 02");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_FADD_S  fadd.s fa0, fa1, fa2   bytes 53 85 c5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FADD_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 1.5_f32},
		{RISCV_REG_F12_32, 2.25_f32},
	});

	emulate_bin("53 85 c5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32, RISCV_REG_F12_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 3.75_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_FLW  flw fa0, 0(a1)   bytes 07 a5 05 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FLW)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1000, 1.0_f32},
	});

	emulate_bin("07 a5 05 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 1.0_f32},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_FSW  fsw fa0, 0(a1)   bytes 27 a0 a5 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FSW)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
		{RISCV_REG_F10_32, 2.5_f32},
	});

	emulate_bin("27 a0 a5 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_F10_32});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 2.5_f32},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_C_FLW  c.flw fa0, 0(a1)   bytes 88 61  (RV32C)
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_FLW)
{
	ONLY_MODE_RV32;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1000, 4.0_f32},
	});

	emulate_bin("88 61");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 4.0_f32},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_AMOADD_W  amoadd.w a0, a1, (a2)   bytes 2f 25 b6 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_AMOADD_W)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 5},
		{RISCV_REG_A2, 0x1000},
	});
	setMemory({
		{0x1000, 10_dw},
	});

	emulate_bin("2f 25 b6 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 10},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 15_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_AMOADD_W_emits_atomicrmw)
{
	ALL_MODES;

	auto* f = translate(utils::hexStringToBytes("2f 25 b6 00"));
	ASSERT_NE(nullptr, f);
	llvm::AtomicRMWInst* rmw = nullptr;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* a = dyn_cast<llvm::AtomicRMWInst>(&*it))
		{
			rmw = a;
			break;
		}
	}
	ASSERT_NE(nullptr, rmw);
	EXPECT_EQ(llvm::AtomicRMWInst::Add, rmw->getOperation());
	EXPECT_EQ(32u, rmw->getValOperand()->getType()->getIntegerBitWidth());
}

//
// RISCV_INS_LR_W  lr.w a0, (a1)   bytes 2f a5 05 10
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_LR_W)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate_bin("2f a5 05 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0x12345678)},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_SC_W  sc.w a0, a1, (a2)   bytes 2f 25 b6 18
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SC_W)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x99},
		{RISCV_REG_A2, 0x1000},
	});

	emulate_bin("2f 25 b6 18");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x99_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_CSRRS  csrrs a0, fflags, x0   bytes 73 25 10 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CSRRS_fflags)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_FFLAGS, 0x15},
	});

	emulate_bin("73 25 10 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_FFLAGS});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x15},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_CSRRW  csrrw a0, fflags, a1   bytes 73 95 15 00
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CSRRW_fflags)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_FFLAGS, 0x3},
		{RISCV_REG_A1, 0x11},
	});

	emulate_bin("73 95 15 00");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_FFLAGS, RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x3},
		{RISCV_REG_FFLAGS, 0x11},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec

