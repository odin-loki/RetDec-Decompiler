/**
 * @file tests/capstone2llvmir/riscv_tests.cpp
 * @brief Capstone2LlvmIrTranslatorRiscv unit tests.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstrTypes.h>

#include <cstdint>
#include <cstring>
#include <limits>
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
									CS_MODE_RISCVC | CS_MODE_RISCV_FD | CS_MODE_RISCV_A
									| CS_MODE_RISCV_V
									| CS_MODE_RISCV_ZBA | CS_MODE_RISCV_ZBB
									| CS_MODE_RISCV_ZBKB | CS_MODE_RISCV_ZBKX
									| CS_MODE_RISCV_ZBC | CS_MODE_RISCV_ZBS
									| CS_MODE_RISCV_VENTANA));
					break;
				case CS_MODE_RISCV64:
					_translator = Capstone2LlvmIrTranslator::createRiscv64(
							&_module,
							static_cast<cs_mode>(
									CS_MODE_RISCVC | CS_MODE_RISCV_FD | CS_MODE_RISCV_A
									| CS_MODE_RISCV_V
									| CS_MODE_RISCV_ZBA | CS_MODE_RISCV_ZBB
									| CS_MODE_RISCV_ZBKB | CS_MODE_RISCV_ZBKX
									| CS_MODE_RISCV_ZBC | CS_MODE_RISCV_ZBS
									| CS_MODE_RISCV_VENTANA));
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

		static float f32bits(uint32_t u)
		{
			float f;
			std::memcpy(&f, &u, sizeof(f));
			return f;
		}

		static double f64bits(uint64_t u)
		{
			double d;
			std::memcpy(&d, &u, sizeof(d));
			return d;
		}

#define ALL_MODES
#define ONLY_MODE_RV32 if (GetParam() != CS_MODE_RISCV32) return;
#define ONLY_MODE_RV64 if (GetParam() != CS_MODE_RISCV64) return;
#define SKIP_MODE_RV32 if (GetParam() == CS_MODE_RISCV32) return;
#define SKIP_MODE_RV64 if (GetParam() == CS_MODE_RISCV64) return;

		static bool hasScalableBinop(Function* f, unsigned opc)
		{
			for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
			{
				if (auto* b = dyn_cast<BinaryOperator>(&*it))
				{
					if (b->getOpcode() == opc && b->getType()->isScalableTy())
					{
						return true;
					}
				}
			}
			return false;
		}

		static bool hasScalableMemLoad(Function* f)
		{
			for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
			{
				if (auto* l = dyn_cast<LoadInst>(&*it))
				{
					if (l->getType()->isScalableTy()
							&& !isa<GlobalVariable>(l->getPointerOperand()->stripPointerCasts()))
					{
						return true;
					}
				}
			}
			return false;
		}

		static bool hasScalableMemStore(Function* f)
		{
			for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
			{
				if (auto* s = dyn_cast<StoreInst>(&*it))
				{
					if (s->getValueOperand()->getType()->isScalableTy()
							&& !isa<GlobalVariable>(s->getPointerOperand()->stripPointerCasts()))
					{
						return true;
					}
				}
			}
			return false;
		}
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

//
// RISCV_INS_FMADD_S  fmadd.s fa0, fa1, fa2, fa3   bytes 43 85 C5 68
// rd = rs1*rs2 + rs3. 2*3+4 = 10; swapped rs2/rs3 would be 2*4+3 = 11.
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FMADD_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 2.0_f32},
		{RISCV_REG_F12_32, 3.0_f32},
		{RISCV_REG_F13_32, 4.0_f32},
	});

	emulate_bin("43 85 C5 68");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32, RISCV_REG_F12_32, RISCV_REG_F13_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 10.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FSQRT_S  fsqrt.s fa0, fa1   bytes 53 85 05 58
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FSQRT_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 16.0_f32},
	});

	emulate_bin("53 85 05 58");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 4.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FSGNJ_S  fsgnj.s fa0, fa1, fa2   bytes 53 85 C5 20
// rd = {sign(rs2), abs(rs1)}
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FSGNJ_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 1.5_f32},
		{RISCV_REG_F12_32, static_cast<float>(-2.25)},
	});

	emulate_bin("53 85 C5 20");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32, RISCV_REG_F12_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, static_cast<float>(-1.5)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_FMIN_S  fmin.s fa0, fa1, fa2   bytes 53 85 C5 28
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FMIN_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 1.5_f32},
		{RISCV_REG_F12_32, 2.25_f32},
	});

	emulate_bin("53 85 C5 28");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32, RISCV_REG_F12_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 1.5_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FMINM_S  fminm.s fa0, fa1, fa2   bytes 53 A5 C5 28
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FMINM_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 1.5_f32},
		{RISCV_REG_F12_32, 2.25_f32},
	});

	emulate_bin("53 A5 C5 28");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32, RISCV_REG_F12_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 1.5_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FMAXM_S  fmaxm.s fa0, fa1, fa2   bytes 53 B5 C5 28
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FMAXM_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 1.5_f32},
		{RISCV_REG_F12_32, 2.25_f32},
	});

	emulate_bin("53 B5 C5 28");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32, RISCV_REG_F12_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 2.25_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FROUND_S_rdn  fround.s fa0, fa1, rdn   bytes 53 A5 45 40
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FROUND_S_rdn)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 2.5_f32},
	});

	emulate_bin("53 A5 45 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 2.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FROUND_S_rmm  fround.s fa0, fa1, rmm   bytes 53 C5 45 40
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FROUND_S_rmm)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 2.5_f32},
	});

	emulate_bin("53 C5 45 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 3.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FROUND_S_rtz  fround.s fa0, fa1, rtz (2.75 -> 2.0)   bytes 53 95 45 40
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FROUND_S_rtz)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 2.75_f32},
	});

	emulate_bin("53 95 45 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 2.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FROUNDNX_S_rup  froundnx.s fa0, fa1, rup   bytes 53 B5 55 40
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FROUNDNX_S_rup)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 2.25_f32},
	});

	emulate_bin("53 B5 55 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 3.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FLEQ_S  fleq.s a0, fa1, fa2   bytes 53 C5 C5 A0
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FLEQ_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 2.25_f32},
		{RISCV_REG_F12_32, 2.25_f32},
	});

	emulate_bin("53 C5 C5 A0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32, RISCV_REG_F12_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FLTQ_S  fltq.s a0, fa1, fa2   bytes 53 D5 C5 A0
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FLTQ_S)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 2.25_f32},
		{RISCV_REG_F12_32, 2.25_f32},
	});

	emulate_bin("53 D5 C5 A0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32, RISCV_REG_F12_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// RISCV_INS_FMV_W_X  fmv.w.x fa0, a1   bytes 53 85 05 F0
// 0x3f800000 is 1.0f as IEEE bits.
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FMV_W_X)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x3f800000},
	});

	emulate_bin("53 85 05 F0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_F10_32, 1.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_AMOMIN_W  amomin.w a0, a1, (a2)   bytes 2f 25 b6 80
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_AMOMIN_W)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 5},
		{RISCV_REG_A2, 0x1000},
	});
	setMemory({
		{0x1000, 10_dw},
	});

	emulate_bin("2f 25 b6 80");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 10},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 5_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_AMOMIN_W_emits_atomicrmw)
{
	ALL_MODES;

	auto* f = translate(utils::hexStringToBytes("2f 25 b6 80"));
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
	EXPECT_EQ(llvm::AtomicRMWInst::Min, rmw->getOperation());
	EXPECT_EQ(32u, rmw->getValOperand()->getType()->getIntegerBitWidth());
}

//
// RISCV_INS_FCLASS_S  fclass.s a0, fa1   bytes 53 95 05 E0
// 10-bit mask: -inf, -norm, -sub, -0, +0, +sub, +norm, +inf, sNaN, qNaN.
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_S_pos_normal)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 1.5_f32},
	});

	emulate_bin("53 95 05 E0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 6},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_S_neg_normal)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, static_cast<float>(-1.5)},
	});

	emulate_bin("53 95 05 E0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_S_pos_zero)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, 0.0_f32},
	});

	emulate_bin("53 95 05 E0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_S_neg_zero)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, f32bits(0x80000000u)},
	});

	emulate_bin("53 95 05 E0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 3},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_S_pos_inf)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, std::numeric_limits<float>::infinity()},
	});

	emulate_bin("53 95 05 E0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_S_neg_inf)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, -std::numeric_limits<float>::infinity()},
	});

	emulate_bin("53 95 05 E0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_S_qnan)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, std::numeric_limits<float>::quiet_NaN()},
	});

	emulate_bin("53 95 05 E0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_S_pos_subnormal)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_32, f32bits(0x00000001u)},
	});

	emulate_bin("53 95 05 E0");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_32});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 5},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RISCV_INS_FCLASS_D  fclass.d a0, fa1   bytes 53 95 05 E2
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_D_pos_normal)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_64, 1.5},
	});

	emulate_bin("53 95 05 E2");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_64});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 6},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_FCLASS_D_neg_zero)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_F11_64, f64bits(0x8000000000000000ull)},
	});

	emulate_bin("53 95 05 E2");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_F11_64});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1u << 3},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Zbb / Zba (x86 BMI/ABM/POPCNT / LEA parity)
// Encodings: andn/orn/xnor/clz/cpop/min/sh1add/... a0, a1, a2
// RISC-V ANDN is rs1 & ~rs2, not x86 ~rs1 & rs2.
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ANDN)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0xC},
		{RISCV_REG_A2, 0xA},
	});

	emulate_bin("33 f5 c5 40");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CLZ)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 1},
	});

	emulate_bin("13 95 05 60");

	uint64_t expect = GetParam() == CS_MODE_RISCV64 ? 63ull : 31ull;
	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, expect},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CLZ_zero)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0},
	});

	emulate_bin("13 95 05 60");

	uint64_t expect = GetParam() == CS_MODE_RISCV64 ? 64ull : 32ull;
	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, expect},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CPOP)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0xF0},
	});

	emulate_bin("13 95 25 60");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_MIN)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, sx32(0xFFFFFFFDu)},
		{RISCV_REG_A2, 5},
	});

	emulate_bin("33 c5 c5 0a");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xFFFFFFFDu)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SH1ADD)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 3},
		{RISCV_REG_A2, 10},
	});

	emulate_bin("33 a5 c5 20");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 16},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SH1ADD_UW)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0xFFFFFFFF00000003ull},
		{RISCV_REG_A2, 10},
	});

	emulate_bin("3b a5 c5 20");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 16},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_REV8)
{
	ALL_MODES;

	if (GetParam() == CS_MODE_RISCV64)
	{
		setRegisters({
			{RISCV_REG_A1, 0x0123456789ABCDEFull},
		});
		emulate_bin("13 d5 85 6b");
		EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
		EXPECT_JUST_REGISTERS_STORED({
			{RISCV_REG_A0, 0xEFCDAB8967452301ull},
		});
	}
	else
	{
		setRegisters({
			{RISCV_REG_A1, 0x01234567},
		});
		emulate_bin("13 d5 85 69");
		EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
		EXPECT_JUST_REGISTERS_STORED({
			{RISCV_REG_A0, 0x67452301},
		});
	}
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Zknh / Zksh scalar hash functions
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SHA256SUM0)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x12345678},
	});

	emulate_bin("13 95 05 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0x66146474)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SHA256SUM1)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x12345678},
	});

	emulate_bin("13 95 15 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0x3561ABDA)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SHA256SIG0)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x12345678},
	});

	emulate_bin("13 95 25 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xE7FCE6EE)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SHA256SIG1)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x12345678},
	});

	emulate_bin("13 95 35 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xA1F78649)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SHA512SUM0)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x0123456789ABCDEFULL},
	});

	emulate_bin("13 95 45 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xB7C57A100C7EC1ABULL},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SHA512SUM1)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x0123456789ABCDEFULL},
	});

	emulate_bin("13 95 55 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x7703112333475567ULL},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SHA512SIG0)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x0123456789ABCDEFULL},
	});

	emulate_bin("13 95 65 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x6F92C77C6C4F1AA1ULL},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SHA512SIG1)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x0123456789ABCDEFULL},
	});

	emulate_bin("13 95 75 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x70A3460DBBD4317AULL},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SM3P0)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x12345678},
	});

	emulate_bin("13 95 85 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xD6688234)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SM3P1)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x12345678},
	});

	emulate_bin("13 95 95 10");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0x05014549)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_PREFETCH_R_is_nothing)
{
	ALL_MODES;

	emulate_bin("13 60 15 00");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_PREFETCH_W_is_nothing)
{
	ALL_MODES;

	emulate_bin("13 60 35 00");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_PREFETCH_I_is_nothing)
{
	ALL_MODES;

	emulate_bin("13 60 05 00");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ORC_B)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x00120000},
	});

	emulate_bin("13 d5 75 28");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x00FF0000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SEXT_B)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x80},
	});

	emulate_bin("13 95 45 60");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xFFFFFF80u)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SEXT_H)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x8000},
	});

	emulate_bin("13 95 55 60");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xFFFF8000u)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Zbs (x86 BTR/BTS/BTC / BMI BEXTR analogue) and Zicond CZERO.
// Encodings: bclr/bset/binv/bext a0, a1, a2; *i a0, a1, 4; slli.uw a0, a1, 4;
// czero.eqz/nez a0, a1, a2. Keystone has no RISC-V; hex from the ratified spec.
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BCLR)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0xF},
		{RISCV_REG_A2, 1},
	});

	emulate_bin("33 95 c5 48");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xD},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BCLRI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1F},
	});

	emulate_bin("13 95 45 48");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xF},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BSET)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1},
		{RISCV_REG_A2, 3},
	});

	emulate_bin("33 95 c5 28");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BSETI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1},
	});

	emulate_bin("13 95 45 28");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x11},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BINV)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0xF},
		{RISCV_REG_A2, 0},
	});

	emulate_bin("33 95 c5 68");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xE},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BINVI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x10},
	});

	emulate_bin("13 95 45 68");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BEXT)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x8},
		{RISCV_REG_A2, 3},
	});

	emulate_bin("33 d5 c5 48");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_BEXTI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x10},
	});

	emulate_bin("13 d5 45 48");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_SLLI_UW)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0xFFFFFFFF00000003ull},
	});

	emulate_bin("1b 95 45 08");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x30},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CZERO_EQZ)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 7},
		{RISCV_REG_A2, 0},
	});

	emulate_bin("33 d5 c5 0e");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CZERO_EQZ_keep)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 7},
		{RISCV_REG_A2, 1},
	});

	emulate_bin("33 d5 c5 0e");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CZERO_NEZ)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 7},
		{RISCV_REG_A2, 1},
	});

	emulate_bin("33 f5 c5 0e");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CZERO_NEZ_keep)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 7},
		{RISCV_REG_A2, 0},
	});

	emulate_bin("33 f5 c5 0e");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Zcb (x86 MOVZX/MOVSX/NOT parity). 16-bit compressed zext/sext/not
// and byte/half load/store. Live Capstone 6 DETAIL_REAL dump:
//   c.zext.b a0     61 9d  id=C_ZEXT_B  3-op andi a0,a0,0xff
//   c.sext.b a0     65 9d  id=C_SEXT_B  2-op rd, rd
//   c.zext.h a0     69 9d  id=C_ZEXT_H  2-op rd, rd
//   c.sext.h a0     6d 9d  id=C_SEXT_H  2-op rd, rd
//   c.zext.w a0     71 9d  id=C_ZEXT_W  unary rd (RV64; alias ZEXT.W)
//   c.not a0        75 9d  id=C_NOT     unary rd
//   c.lbu a0, 0(a1) 88 81  REG+MEM
//   c.lbu a0, 1(a1) c8 81
//   c.lhu a0, 0(a1) 88 85
//   c.lh  a0, 0(a1) c8 85
//   c.sb  a0, 0(a1) 88 89
//   c.sh  a0, 0(a1) 88 8d
// No CS_MODE_RISCV_ZCB token; FeatureStdExtZcb defaults true.
// C.SEXT.* / C.ZEXT.H need Zbb (already ORed). C.ZEXT.W needs Zba+RV64.
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_ZEXT_B)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x12345680},
	});

	emulate_bin("61 9d");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x80},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_ZEXT_H)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x12348000},
	});

	emulate_bin("69 9d");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x8000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_ZEXT_W)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A0, 0xFFFFFFFF80000000ull},
	});

	emulate_bin("71 9d");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x80000000ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_SEXT_B)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x80},
	});

	emulate_bin("65 9d");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xFFFFFF80u)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_SEXT_H)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x8000},
	});

	emulate_bin("6d 9d");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xFFFF8000u)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_NOT)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0x0F},
	});

	emulate_bin("75 9d");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xFFFFFFF0u)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_LBU)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1000, 0xfe_b},
	});

	emulate_bin("88 81");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xfe},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_LH)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1000, 0xfff0_w},
	});

	emulate_bin("c8 85");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, sx32(0xfffffff0)},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_LHU)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x1000},
	});
	setMemory({
		{0x1000, 0xfff0_w},
	});

	emulate_bin("88 85");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xfff0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_SB)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0xaabbccdd},
		{RISCV_REG_A1, 0x1000},
	});

	emulate_bin("88 89");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xdd_b},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_C_SH)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A0, 0xaabb},
		{RISCV_REG_A1, 0x1000},
	});

	emulate_bin("88 8d");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A0, RISCV_REG_A1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xaabb_w},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// Zbkb PACK / PACKH / PACKW (x86 PUNPCK / MOVZX parity).
// pack/packh/packw a0, a1, a2. Keystone has no RISC-V; hex from the ratified spec.
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_PACK)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x33334444},
		{RISCV_REG_A2, 0x77778888},
	});

	emulate_bin("33 c5 c5 08");

	uint64_t expect = GetParam() == CS_MODE_RISCV64
			? 0x7777888833334444ull
			: 0x88884444ull;
	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, expect},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_PACKH)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x11AA},
		{RISCV_REG_A2, 0x22BB},
	});

	emulate_bin("33 f5 c5 08");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xBBAA},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_PACKW)
{
	ONLY_MODE_RV64;

	setRegisters({
		{RISCV_REG_A1, 0x22221234ull},
		{RISCV_REG_A2, 0xFFFF8000ull},
	});

	emulate_bin("3b c5 c5 08");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0xffffffff80001234ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Zbc CLMUL / CLMULH / CLMULR (x86 PCLMULQDQ is already mapped).
// clmul/clmulh/clmulr a0, a1, a2. Keystone has no RISC-V; hex from the spec.
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CLMUL)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 3},
		{RISCV_REG_A2, 3},
	});

	emulate_bin("33 95 c5 0a");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 5},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CLMULR)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 3},
		{RISCV_REG_A2, 3},
	});

	emulate_bin("33 a5 c5 0a");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_CLMULH)
{
	ALL_MODES;

	uint64_t hi = GetParam() == CS_MODE_RISCV64
			? 0x8000000000000000ull
			: 0x80000000ull;
	setRegisters({
		{RISCV_REG_A1, hi},
		{RISCV_REG_A2, 2},
	});

	emulate_bin("33 b5 c5 0a");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// RVV compiler subset. Keystone 0.9.2 has no RISC-V vector assembler;
// encodings are hand-written and fed to emulate_bin()/translate().
// llvmir-emul cannot execute <vscale x 4 x i32>, so VADD/VMUL/VLE/VSE
// inspect IR from translate() instead of emulating.
//
// vsetvli a0, a1, e32,m1,ta,ma   zimm=0xd0  bytes 57 f5 05 0d
//

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSETVLI)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 7},
	});

	emulate_bin("57 f5 05 0d");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 7},
		{RISCV_REG_VL, 7},
		{RISCV_REG_VTYPE, 0xd0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

// vadd.vv v1, v2, v3  (unmasked)  bytes d7 80 21 02

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VADD_VV)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x80, 0x21, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Add)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VADD_VX)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x40, 0x25, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Add)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VADD_VI)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0xb0, 0x22, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Add)) << dumpFunction(f);
}

// vmul.vv v1, v2, v3  (unmasked)  bytes d7 a0 21 96

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMUL_VV)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0xa0, 0x21, 0x96});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Mul)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMUL_VX)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x60, 0x25, 0x96});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Mul)) << dumpFunction(f);
}

// vle32.v v1, (a0)  (unmasked)  bytes 87 60 05 02

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VLE32_V)
{
	ALL_MODES;

	auto* f = translate({0x87, 0x60, 0x05, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableMemLoad(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VLE8_V)
{
	ALL_MODES;

	auto* f = translate({0x87, 0x00, 0x05, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableMemLoad(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VLE16_V)
{
	ALL_MODES;

	auto* f = translate({0x87, 0x50, 0x05, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableMemLoad(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VLE64_V)
{
	ALL_MODES;

	auto* f = translate({0x87, 0x70, 0x05, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableMemLoad(f)) << dumpFunction(f);
}

// vse32.v v1, (a0)  (unmasked)  bytes a7 60 05 02

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSE32_V)
{
	ALL_MODES;

	auto* f = translate({0xa7, 0x60, 0x05, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableMemStore(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSE8_V)
{
	ALL_MODES;

	auto* f = translate({0xa7, 0x00, 0x05, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableMemStore(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSE16_V)
{
	ALL_MODES;

	auto* f = translate({0xa7, 0x50, 0x05, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableMemStore(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSE64_V)
{
	ALL_MODES;

	auto* f = translate({0xa7, 0x70, 0x05, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableMemStore(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSUB_VV)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x80, 0x21, 0x0a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Sub)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSUB_VX)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x40, 0x25, 0x0a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Sub)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VAND_VV)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x80, 0x21, 0x26});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::And)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VAND_VX)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x40, 0x25, 0x26});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::And)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VAND_VI)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0xb0, 0x22, 0x26});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::And)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VOR_VV)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x80, 0x21, 0x2a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Or)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VOR_VX)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x40, 0x25, 0x2a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Or)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VOR_VI)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0xb0, 0x22, 0x2a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Or)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VXOR_VV)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x80, 0x21, 0x2e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Xor)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VXOR_VX)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x40, 0x25, 0x2e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Xor)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VXOR_VI)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0xb0, 0x22, 0x2e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Xor)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMV_V_V)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x00, 0x01, 0x5e});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->getValueOperand()->getType()->isScalableTy()
					&& isa<GlobalVariable>(s->getPointerOperand()->stripPointerCasts()))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMV_V_X)
{
	ALL_MODES;

	auto* f = translate({0xd7, 0x40, 0x05, 0x5e});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<ShuffleVectorInst>(&*it) && it->getType()->isScalableTy())
		{
			found = true;
			break;
		}
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			if (s->getValueOperand()->getType()->isScalableTy())
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSETIVLI)
{
	ALL_MODES;

	emulate_bin("57 f5 03 cd");

	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 7},
		{RISCV_REG_VL, 7},
		{RISCV_REG_VTYPE, 0xd0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSETVL)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 7},
		{RISCV_REG_A2, 0xd0},
	});

	emulate_bin("57 f5 c5 80");

	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 7},
		{RISCV_REG_VL, 7},
		{RISCV_REG_VTYPE, 0xd0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

static bool hasScalableICmp(Function* f)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* c = dyn_cast<ICmpInst>(&*it))
		{
			if (c->getOperand(0)->getType()->isScalableTy())
			{
				return true;
			}
		}
	}
	return false;
}

static bool hasScalableFBinop(Function* f, unsigned opc)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* b = dyn_cast<BinaryOperator>(&*it))
		{
			if (b->getOpcode() == opc && b->getType()->isScalableTy())
			{
				return true;
			}
		}
	}
	return false;
}

static bool hasScalableFCmp(Function* f)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* c = dyn_cast<FCmpInst>(&*it))
		{
			if (c->getOperand(0)->getType()->isScalableTy())
			{
				return true;
			}
		}
	}
	return false;
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VDIV_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xa0, 0x21, 0x86});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::SDiv)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VDIV_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x60, 0x25, 0x86});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::SDiv)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VDIVU_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xa0, 0x21, 0x82});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::UDiv)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VDIVU_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x60, 0x25, 0x82});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::UDiv)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VREM_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xa0, 0x21, 0x8e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::SRem)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VREM_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x60, 0x25, 0x8e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::SRem)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VREMU_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xa0, 0x21, 0x8a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::URem)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VREMU_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x60, 0x25, 0x8a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::URem)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFADD_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x90, 0x21, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FAdd)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFADD_VF)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x50, 0x25, 0x02});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FAdd)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFSUB_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x90, 0x21, 0x0a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FSub)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFSUB_VF)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x50, 0x25, 0x0a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FSub)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFMUL_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x90, 0x21, 0x92});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FMul)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFMUL_VF)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x50, 0x25, 0x92});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FMul)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFDIV_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x90, 0x21, 0x82});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FDiv)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFDIV_VF)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x50, 0x25, 0x82});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FDiv)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFMACC_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x90, 0x21, 0xb0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FMul)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFMACC_VF)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x50, 0x25, 0xb0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FMul)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFMADD_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x90, 0x21, 0xa0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FMul)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VFMADD_VF)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x50, 0x25, 0xa0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableFBinop(f, Instruction::FMul)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSLL_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x96});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Shl)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSLL_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x96});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Shl)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSLL_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0x96});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Shl)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSRL_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0xa0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::LShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSRL_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0xa0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::LShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSRL_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0xa0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::LShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSRA_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0xa4});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::AShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSRA_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0xa4});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::AShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSRA_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0xa4});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::AShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMIN_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x16});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMIN_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x16});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMAX_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x1e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMAX_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x1e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMINU_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x12});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMINU_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x12});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMAXU_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x1a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMAXU_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x1a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSEQ_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x62});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSEQ_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x62});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSEQ_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0x62});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSNE_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x66});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSNE_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x66});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSNE_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0x66});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSLT_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x6e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSLT_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x6e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSLT_VI)
{
	ALL_MODES;
	// RVV has no vmslt.vi encoding (Capstone token is a TableGen pseudo).
	// Exercise the shared SLT path with the valid vmslt.vx encoding.
	auto* f = translate({0xd7, 0x40, 0x25, 0x6e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSLE_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x76});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSLE_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x76});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSLE_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0x76});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSGT_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x7e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VMSGT_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0x7e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSLIDEUP_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x3a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Shl)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSLIDEUP_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0x3a});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::Shl)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSLIDEDOWN_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x3e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::LShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VSLIDEDOWN_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0x3e});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::LShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VWADD_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xa0, 0x21, 0xc6});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<SExtInst>(&*it) || (isa<BinaryOperator>(&*it)
				&& cast<BinaryOperator>(&*it)->getOpcode() == Instruction::Add))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VWADD_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x60, 0x25, 0xc6});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<SExtInst>(&*it) || (isa<BinaryOperator>(&*it)
				&& cast<BinaryOperator>(&*it)->getOpcode() == Instruction::Add))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VWADDU_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xa0, 0x21, 0xc2});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<ZExtInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VWADDU_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x60, 0x25, 0xc2});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<ZExtInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VWSUB_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xa0, 0x21, 0xce});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* b = dyn_cast<BinaryOperator>(&*it))
		{
			if (b->getOpcode() == Instruction::Sub)
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VWSUB_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x60, 0x25, 0xce});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* b = dyn_cast<BinaryOperator>(&*it))
		{
			if (b->getOpcode() == Instruction::Sub)
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VRGATHER_VV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0x32});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VRGATHER_VX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0x32});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableICmp(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VRGATHER_VI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0x32});
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<ExtractElementInst>(&*it) || isa<SelectInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VNSRL_WV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0xb0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::LShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VNSRL_WX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0xb0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::LShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VNSRL_WI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0xb0});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::LShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VNSRA_WV)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x80, 0x21, 0xb4});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::AShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VNSRA_WX)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0x40, 0x25, 0xb4});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::AShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VNSRA_WI)
{
	ALL_MODES;
	auto* f = translate({0xd7, 0xb0, 0x22, 0xb4});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasScalableBinop(f, Instruction::AShr)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_ZIP)
{
	ONLY_MODE_RV32;

	setRegisters({
		{RISCV_REG_A1, 0x0000ffff},
	});
	emulate_bin("13 95 f5 08");
	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x55555555},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_UNZIP)
{
	ONLY_MODE_RV32;

	setRegisters({
		{RISCV_REG_A1, 0x55555555},
	});
	emulate_bin("13 d5 f5 08");
	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, 0x0000ffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_XPERM8)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x08070605},
		{RISCV_REG_A2, 0},
	});
	emulate_bin("33 c5 c5 28");
	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, GetParam() == CS_MODE_RISCV64 ? 0x0505050505050505ull : 0x05050505ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_XPERM4)
{
	ALL_MODES;

	setRegisters({
		{RISCV_REG_A1, 0x21},
		{RISCV_REG_A2, 0x10},
	});
	emulate_bin("33 a5 c5 28");
	EXPECT_JUST_REGISTERS_LOADED({RISCV_REG_A1, RISCV_REG_A2});
	// a2=0x10: nibble0→rs1[0]=1, nibble1→rs1[1]=2; remaining control
	// nibbles are 0 so they also pick rs1[0]=1 (not zero).
	EXPECT_JUST_REGISTERS_STORED({
		{RISCV_REG_A0, GetParam() == CS_MODE_RISCV64
				? 0x1111111111111121ull
				: 0x11111121ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

static bool hasExtractOrSelect(Function* f)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<ExtractElementInst>(&*it) || isa<SelectInst>(&*it)
				|| isa<InsertElementInst>(&*it))
		{
			return true;
		}
	}
	return false;
}

static bool hasAnyLoad(Function* f)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<LoadInst>(&*it))
		{
			return true;
		}
	}
	return false;
}

static bool hasAnyStore(Function* f)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<StoreInst>(&*it) && !cast<StoreInst>(&*it)->isVolatile())
		{
			return true;
		}
	}
	return false;
}

static bool hasCastOf(Function* f, unsigned opc)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* c = dyn_cast<CastInst>(&*it))
		{
			if (c->getOpcode() == opc)
			{
				return true;
			}
		}
	}
	return false;
}

static bool hasNamedCall(Function* f, const char* needle)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* c = dyn_cast<CallInst>(&*it))
		{
			if (auto* fn = c->getCalledFunction())
			{
				if (fn->getName().contains(needle))
				{
					return true;
				}
			}
		}
	}
	return false;
}

#define RVV_IR(name, bytes, pred) \
	TEST_P(Capstone2LlvmIrTranslatorRiscvTests, name) \
	{ \
		ALL_MODES; \
		auto* f = translate bytes; \
		ASSERT_NE(nullptr, f); \
		EXPECT_TRUE(pred) << dumpFunction(f); \
	}

RVV_IR(RISCV_INS_VMAND_MM, ({0xd7, 0xa0, 0x21, 0x66}), hasScalableBinop(f, Instruction::And))
RVV_IR(RISCV_INS_VMOR_MM, ({0xd7, 0xa0, 0x21, 0x6a}), hasScalableBinop(f, Instruction::Or))
RVV_IR(RISCV_INS_VMXOR_MM, ({0xd7, 0xa0, 0x21, 0x6e}), hasScalableBinop(f, Instruction::Xor))
RVV_IR(RISCV_INS_VMNAND_MM, ({0xd7, 0xa0, 0x21, 0x76}), hasScalableBinop(f, Instruction::And))
RVV_IR(RISCV_INS_VMANDN_MM, ({0xd7, 0xa0, 0x21, 0x62}), hasScalableBinop(f, Instruction::And))
RVV_IR(RISCV_INS_VMNOR_MM, ({0xd7, 0xa0, 0x21, 0x7a}), hasScalableBinop(f, Instruction::Or))
RVV_IR(RISCV_INS_VMORN_MM, ({0xd7, 0xa0, 0x21, 0x72}), hasScalableBinop(f, Instruction::Or))
RVV_IR(RISCV_INS_VMXNOR_MM, ({0xd7, 0xa0, 0x21, 0x7e}), hasScalableBinop(f, Instruction::Xor))

RVV_IR(RISCV_INS_VMSLTU_VV, ({0xd7, 0x80, 0x21, 0x6a}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSLTU_VX, ({0xd7, 0x40, 0x25, 0x6a}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSLEU_VV, ({0xd7, 0x80, 0x21, 0x72}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSLEU_VX, ({0xd7, 0x40, 0x25, 0x72}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSLEU_VI, ({0xd7, 0xb0, 0x22, 0x72}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSGTU_VX, ({0xd7, 0x40, 0x25, 0x7a}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSGTU_VI, ({0xd7, 0xb0, 0x22, 0x7a}), hasScalableICmp(f))

RVV_IR(RISCV_INS_VFMIN_VV, ({0xd7, 0x90, 0x21, 0x12}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFMIN_VF, ({0xd7, 0x50, 0x25, 0x12}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFMAX_VV, ({0xd7, 0x90, 0x21, 0x1a}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFMAX_VF, ({0xd7, 0x50, 0x25, 0x1a}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFSQRT_V, ({0xd7, 0x10, 0x20, 0x4e}), (hasExtractOrSelect(f) || hasCastOf(f, Instruction::BitCast)))
RVV_IR(RISCV_INS_VFRSUB_VF, ({0xd7, 0x50, 0x25, 0x9e}), hasScalableFBinop(f, Instruction::FSub))
RVV_IR(RISCV_INS_VFRDIV_VF, ({0xd7, 0x50, 0x25, 0x86}), hasScalableFBinop(f, Instruction::FDiv))
RVV_IR(RISCV_INS_VFSGNJ_VV, ({0xd7, 0x90, 0x21, 0x22}), hasScalableBinop(f, Instruction::And))
RVV_IR(RISCV_INS_VFSGNJ_VF, ({0xd7, 0x50, 0x25, 0x22}), hasScalableBinop(f, Instruction::And))
RVV_IR(RISCV_INS_VFSGNJN_VV, ({0xd7, 0x90, 0x21, 0x26}), hasScalableBinop(f, Instruction::Xor))
RVV_IR(RISCV_INS_VFSGNJN_VF, ({0xd7, 0x50, 0x25, 0x26}), hasScalableBinop(f, Instruction::Xor))
RVV_IR(RISCV_INS_VFSGNJX_VV, ({0xd7, 0x90, 0x21, 0x2a}), hasScalableBinop(f, Instruction::Xor))
RVV_IR(RISCV_INS_VFSGNJX_VF, ({0xd7, 0x50, 0x25, 0x2a}), hasScalableBinop(f, Instruction::Xor))
RVV_IR(RISCV_INS_VFCLASS_V, ({0xd7, 0x10, 0x28, 0x4e}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VFMSAC_VV, ({0xd7, 0x90, 0x21, 0xbc}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFMSAC_VF, ({0xd7, 0x50, 0x25, 0xbc}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFMSUB_VV, ({0xd7, 0x90, 0x21, 0xac}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFMSUB_VF, ({0xd7, 0x50, 0x25, 0xac}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFNMACC_VV, ({0xd7, 0x90, 0x21, 0xb6}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFNMACC_VF, ({0xd7, 0x50, 0x25, 0xb6}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFNMADD_VV, ({0xd7, 0x90, 0x21, 0xa6}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFNMADD_VF, ({0xd7, 0x50, 0x25, 0xa6}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFNMSAC_VV, ({0xd7, 0x90, 0x21, 0xbe}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFNMSAC_VF, ({0xd7, 0x50, 0x25, 0xbe}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFNMSUB_VV, ({0xd7, 0x90, 0x21, 0xae}), hasScalableFBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFNMSUB_VF, ({0xd7, 0x50, 0x25, 0xae}), hasScalableFBinop(f, Instruction::FMul))

RVV_IR(RISCV_INS_VSEXT_VF2, ({0xd7, 0xa0, 0x23, 0x4a}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VSEXT_VF4, ({0xd7, 0xa0, 0x22, 0x4a}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VSEXT_VF8, ({0xd7, 0xa0, 0x21, 0x4a}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VZEXT_VF2, ({0xd7, 0x20, 0x23, 0x4a}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VZEXT_VF4, ({0xd7, 0x20, 0x22, 0x4a}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VZEXT_VF8, ({0xd7, 0x20, 0x21, 0x4a}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VFCVT_XU_F_V, ({0xd7, 0x10, 0x20, 0x4a}), hasCastOf(f, Instruction::FPToUI))
RVV_IR(RISCV_INS_VFCVT_X_F_V, ({0xd7, 0x90, 0x20, 0x4a}), hasCastOf(f, Instruction::FPToSI))
RVV_IR(RISCV_INS_VFCVT_F_XU_V, ({0xd7, 0x10, 0x21, 0x4a}), hasCastOf(f, Instruction::UIToFP))
RVV_IR(RISCV_INS_VFCVT_F_X_V, ({0xd7, 0x90, 0x21, 0x4a}), hasCastOf(f, Instruction::SIToFP))
RVV_IR(RISCV_INS_VFCVT_RTZ_XU_F_V, ({0xd7, 0x10, 0x23, 0x4a}), hasCastOf(f, Instruction::FPToUI))
RVV_IR(RISCV_INS_VFCVT_RTZ_X_F_V, ({0xd7, 0x90, 0x23, 0x4a}), hasCastOf(f, Instruction::FPToSI))
RVV_IR(RISCV_INS_VFWCVT_F_F_V, ({0xd7, 0x10, 0x26, 0x4a}), hasCastOf(f, Instruction::FPExt))
RVV_IR(RISCV_INS_VFWCVT_XU_F_V, ({0xd7, 0x10, 0x24, 0x4a}), hasCastOf(f, Instruction::FPToUI))
RVV_IR(RISCV_INS_VFWCVT_X_F_V, ({0xd7, 0x90, 0x24, 0x4a}), hasCastOf(f, Instruction::FPToSI))
RVV_IR(RISCV_INS_VFWCVT_F_XU_V, ({0xd7, 0x10, 0x25, 0x4a}), hasCastOf(f, Instruction::UIToFP))
RVV_IR(RISCV_INS_VFWCVT_F_X_V, ({0xd7, 0x90, 0x25, 0x4a}), hasCastOf(f, Instruction::SIToFP))
RVV_IR(RISCV_INS_VFNCVT_F_F_W, ({0xd7, 0x10, 0x2a, 0x4a}), hasCastOf(f, Instruction::FPTrunc))
RVV_IR(RISCV_INS_VFNCVT_XU_F_W, ({0xd7, 0x10, 0x28, 0x4a}), hasCastOf(f, Instruction::FPToUI))
RVV_IR(RISCV_INS_VFNCVT_X_F_W, ({0xd7, 0x90, 0x28, 0x4a}), hasCastOf(f, Instruction::FPToSI))
RVV_IR(RISCV_INS_VFNCVT_F_XU_W, ({0xd7, 0x10, 0x29, 0x4a}), hasCastOf(f, Instruction::UIToFP))
RVV_IR(RISCV_INS_VFNCVT_F_X_W, ({0xd7, 0x90, 0x29, 0x4a}), hasCastOf(f, Instruction::SIToFP))

RVV_IR(RISCV_INS_VMERGE_VVM, ({0xd7, 0x80, 0x21, 0x5c}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VMERGE_VXM, ({0xd7, 0x40, 0x25, 0x5c}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VMERGE_VIM, ({0xd7, 0xb0, 0x22, 0x5c}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VCOMPRESS_VM, ({0xd7, 0xa0, 0x21, 0x5e}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VSLIDE1UP_VX, ({0xd7, 0x60, 0x25, 0x3a}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VSLIDE1DOWN_VX, ({0xd7, 0x60, 0x25, 0x3e}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFSLIDE1UP_VF, ({0xd7, 0x50, 0x25, 0x3a}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFSLIDE1DOWN_VF, ({0xd7, 0x50, 0x25, 0x3e}), hasExtractOrSelect(f))

RVV_IR(RISCV_INS_VWADD_WV, ({0xd7, 0xa0, 0x21, 0xd6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWADD_WX, ({0xd7, 0x60, 0x25, 0xd6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWADDU_WV, ({0xd7, 0xa0, 0x21, 0xd2}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWADDU_WX, ({0xd7, 0x60, 0x25, 0xd2}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWSUB_WV, ({0xd7, 0xa0, 0x21, 0xde}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWSUB_WX, ({0xd7, 0x60, 0x25, 0xde}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWSUBU_VV, ({0xd7, 0xa0, 0x21, 0xca}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWSUBU_VX, ({0xd7, 0x60, 0x25, 0xca}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWSUBU_WV, ({0xd7, 0xa0, 0x21, 0xda}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWSUBU_WX, ({0xd7, 0x60, 0x25, 0xda}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMUL_VV, ({0xd7, 0xa0, 0x21, 0xee}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMUL_VX, ({0xd7, 0x60, 0x25, 0xee}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMULU_VV, ({0xd7, 0xa0, 0x21, 0xe2}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMULU_VX, ({0xd7, 0x60, 0x25, 0xe2}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMULSU_VV, ({0xd7, 0xa0, 0x21, 0xea}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMULSU_VX, ({0xd7, 0x60, 0x25, 0xea}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMACC_VV, ({0xd7, 0xa0, 0x21, 0xf6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMACC_VX, ({0xd7, 0x60, 0x25, 0xf6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMACCU_VV, ({0xd7, 0xa0, 0x21, 0xf2}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMACCU_VX, ({0xd7, 0x60, 0x25, 0xf2}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMACCSU_VV, ({0xd7, 0xa0, 0x21, 0xfe}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMACCSU_VX, ({0xd7, 0x60, 0x25, 0xfe}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWMACCUS_VX, ({0xd7, 0x60, 0x25, 0xfa}), hasExtractOrSelect(f))

RVV_IR(RISCV_INS_VLSE8_V, ({0x87, 0x00, 0xb5, 0x0a}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLSE16_V, ({0x87, 0x50, 0xb5, 0x0a}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLSE32_V, ({0x87, 0x60, 0xb5, 0x0a}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLSE64_V, ({0x87, 0x70, 0xb5, 0x0a}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VSSE8_V, ({0xa7, 0x00, 0xb5, 0x0a}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSSE16_V, ({0xa7, 0x50, 0xb5, 0x0a}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSSE32_V, ({0xa7, 0x60, 0xb5, 0x0a}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSSE64_V, ({0xa7, 0x70, 0xb5, 0x0a}), hasAnyStore(f))

RVV_IR(RISCV_INS_VREDSUM_VS, ({0xd7, 0xa0, 0x21, 0x02}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VREDAND_VS, ({0xd7, 0xa0, 0x21, 0x06}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VREDOR_VS, ({0xd7, 0xa0, 0x21, 0x0a}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VREDXOR_VS, ({0xd7, 0xa0, 0x21, 0x0e}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFREDUSUM_VS, ({0xd7, 0x90, 0x21, 0x06}), hasExtractOrSelect(f))

RVV_IR(RISCV_INS_VAESDM_VV, ({0xf7, 0x20, 0x20, 0xa2}), hasNamedCall(f, "llvm.riscv.vaesdm"))
RVV_IR(RISCV_INS_VAESDM_VS, ({0xf7, 0x20, 0x20, 0xa6}), hasNamedCall(f, "llvm.riscv.vaesdm"))
RVV_IR(RISCV_INS_VAESDF_VV, ({0xf7, 0xa0, 0x20, 0xa2}), hasNamedCall(f, "llvm.riscv.vaesdf"))
RVV_IR(RISCV_INS_VAESDF_VS, ({0xf7, 0xa0, 0x20, 0xa6}), hasNamedCall(f, "llvm.riscv.vaesdf"))
RVV_IR(RISCV_INS_VAESEM_VV, ({0xf7, 0x20, 0x21, 0xa2}), hasNamedCall(f, "llvm.riscv.vaesem"))
RVV_IR(RISCV_INS_VAESEM_VS, ({0xf7, 0x20, 0x21, 0xa6}), hasNamedCall(f, "llvm.riscv.vaesem"))
RVV_IR(RISCV_INS_VAESEF_VV, ({0xf7, 0xa0, 0x21, 0xa2}), hasNamedCall(f, "llvm.riscv.vaesef"))
RVV_IR(RISCV_INS_VAESEF_VS, ({0xf7, 0xa0, 0x21, 0xa6}), hasNamedCall(f, "llvm.riscv.vaesef"))
RVV_IR(RISCV_INS_VAESZ_VS, ({0xf7, 0xa0, 0x23, 0xa6}), hasNamedCall(f, "llvm.riscv.vaesz"))
RVV_IR(RISCV_INS_VAESKF1_VI, ({0xf7, 0xa0, 0x20, 0x8a}), hasNamedCall(f, "llvm.riscv.vaeskf1"))
RVV_IR(RISCV_INS_VAESKF2_VI, ({0xf7, 0x20, 0x21, 0xaa}), hasNamedCall(f, "llvm.riscv.vaeskf2"))
RVV_IR(RISCV_INS_VSHA2CH_VV, ({0xf7, 0xa0, 0x21, 0xba}), hasNamedCall(f, "llvm.riscv.vsha2ch"))
RVV_IR(RISCV_INS_VSHA2CL_VV, ({0xf7, 0xa0, 0x21, 0xbe}), hasNamedCall(f, "llvm.riscv.vsha2cl"))
RVV_IR(RISCV_INS_VSHA2MS_VV, ({0xf7, 0xa0, 0x21, 0xb6}), hasNamedCall(f, "llvm.riscv.vsha2ms"))
RVV_IR(RISCV_INS_VSM3ME_VV, ({0xf7, 0xa0, 0x21, 0x82}), hasNamedCall(f, "llvm.riscv.vsm3me"))
RVV_IR(RISCV_INS_VSM3C_VI, ({0xf7, 0xa0, 0x23, 0xae}), hasNamedCall(f, "llvm.riscv.vsm3c"))
RVV_IR(RISCV_INS_VSM4K_VI, ({0xf7, 0xa0, 0x23, 0x86}), hasNamedCall(f, "llvm.riscv.vsm4k"))
RVV_IR(RISCV_INS_VSM4R_VV, ({0xf7, 0x20, 0x28, 0xa2}), hasNamedCall(f, "llvm.riscv.vsm4r"))
RVV_IR(RISCV_INS_VSM4R_VS, ({0xf7, 0x20, 0x28, 0xa6}), hasNamedCall(f, "llvm.riscv.vsm4r"))

RVV_IR(RISCV_INS_VLOXEI8_V, ({0x87, 0x00, 0x25, 0x0e}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLOXEI16_V, ({0x87, 0x50, 0x25, 0x0e}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLOXEI32_V, ({0x87, 0x60, 0x25, 0x0e}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLOXEI64_V, ({0x87, 0x70, 0x25, 0x0e}), (hasAnyLoad(f) || GetParam() == CS_MODE_RISCV32))
RVV_IR(RISCV_INS_VLUXEI8_V, ({0x87, 0x00, 0x25, 0x06}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLUXEI16_V, ({0x87, 0x50, 0x25, 0x06}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLUXEI32_V, ({0x87, 0x60, 0x25, 0x06}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLUXEI64_V, ({0x87, 0x70, 0x25, 0x06}), (hasAnyLoad(f) || GetParam() == CS_MODE_RISCV32))
RVV_IR(RISCV_INS_VSOXEI8_V, ({0xa7, 0x00, 0x25, 0x0e}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSOXEI16_V, ({0xa7, 0x50, 0x25, 0x0e}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSOXEI32_V, ({0xa7, 0x60, 0x25, 0x0e}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSOXEI64_V, ({0xa7, 0x70, 0x25, 0x0e}), (hasAnyStore(f) || GetParam() == CS_MODE_RISCV32))
RVV_IR(RISCV_INS_VSUXEI8_V, ({0xa7, 0x00, 0x25, 0x06}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSUXEI16_V, ({0xa7, 0x50, 0x25, 0x06}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSUXEI32_V, ({0xa7, 0x60, 0x25, 0x06}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSUXEI64_V, ({0xa7, 0x70, 0x25, 0x06}), (hasAnyStore(f) || GetParam() == CS_MODE_RISCV32))

RVV_IR(RISCV_INS_VMULH_VV, ({0xd7, 0xa0, 0x21, 0x9e}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VMULH_VX, ({0xd7, 0x60, 0x25, 0x9e}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VMULHU_VV, ({0xd7, 0xa0, 0x21, 0x92}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VMULHU_VX, ({0xd7, 0x60, 0x25, 0x92}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VMULHSU_VV, ({0xd7, 0xa0, 0x21, 0x9a}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VMULHSU_VX, ({0xd7, 0x60, 0x25, 0x9a}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VAADD_VV, ({0xd7, 0xa0, 0x21, 0x26}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VAADD_VX, ({0xd7, 0x60, 0x25, 0x26}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VAADDU_VV, ({0xd7, 0xa0, 0x21, 0x22}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VAADDU_VX, ({0xd7, 0x60, 0x25, 0x22}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VASUB_VV, ({0xd7, 0xa0, 0x21, 0x2e}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VASUB_VX, ({0xd7, 0x60, 0x25, 0x2e}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VASUBU_VV, ({0xd7, 0xa0, 0x21, 0x2a}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VASUBU_VX, ({0xd7, 0x60, 0x25, 0x2a}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VSADD_VV, ({0xd7, 0x80, 0x21, 0x86}), hasNamedCall(f, "llvm.sadd.sat"))
RVV_IR(RISCV_INS_VSADD_VX, ({0xd7, 0x40, 0x25, 0x86}), hasNamedCall(f, "llvm.sadd.sat"))
RVV_IR(RISCV_INS_VSADD_VI, ({0xd7, 0xb0, 0x22, 0x86}), hasNamedCall(f, "llvm.sadd.sat"))
RVV_IR(RISCV_INS_VSADDU_VV, ({0xd7, 0x80, 0x21, 0x82}), hasNamedCall(f, "llvm.uadd.sat"))
RVV_IR(RISCV_INS_VSADDU_VX, ({0xd7, 0x40, 0x25, 0x82}), hasNamedCall(f, "llvm.uadd.sat"))
RVV_IR(RISCV_INS_VSADDU_VI, ({0xd7, 0xb0, 0x22, 0x82}), hasNamedCall(f, "llvm.uadd.sat"))
RVV_IR(RISCV_INS_VSSUB_VV, ({0xd7, 0x80, 0x21, 0x8e}), hasNamedCall(f, "llvm.ssub.sat"))
RVV_IR(RISCV_INS_VSSUB_VX, ({0xd7, 0x40, 0x25, 0x8e}), hasNamedCall(f, "llvm.ssub.sat"))
RVV_IR(RISCV_INS_VSSUBU_VV, ({0xd7, 0x80, 0x21, 0x8a}), hasNamedCall(f, "llvm.usub.sat"))
RVV_IR(RISCV_INS_VSSUBU_VX, ({0xd7, 0x40, 0x25, 0x8a}), hasNamedCall(f, "llvm.usub.sat"))
RVV_IR(RISCV_INS_VNCLIP_WV, ({0xd7, 0x80, 0x21, 0xbe}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VNCLIP_WX, ({0xd7, 0x40, 0x25, 0xbe}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VNCLIP_WI, ({0xd7, 0xb0, 0x22, 0xbe}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VNCLIPU_WV, ({0xd7, 0x80, 0x21, 0xba}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VNCLIPU_WX, ({0xd7, 0x40, 0x25, 0xba}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VNCLIPU_WI, ({0xd7, 0xb0, 0x22, 0xba}), hasExtractOrSelect(f))

RVV_IR(RISCV_INS_VID_V, ({0xd7, 0xa0, 0x08, 0x52}), hasAnyStore(f))
RVV_IR(RISCV_INS_VIOTA_M, ({0xd7, 0x20, 0x28, 0x52}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VMV1R_V, ({0xd7, 0x30, 0x20, 0x9e}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VMV2R_V, ({0x57, 0xb1, 0x40, 0x9e}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VMV4R_V, ({0x57, 0xb2, 0x81, 0x9e}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VMV8R_V, ({0x57, 0xb4, 0x03, 0x9f}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VADC_VVM, ({0xd7, 0x80, 0x21, 0x40}), hasScalableBinop(f, Instruction::Add))
RVV_IR(RISCV_INS_VADC_VXM, ({0xd7, 0x40, 0x25, 0x40}), hasScalableBinop(f, Instruction::Add))
RVV_IR(RISCV_INS_VADC_VIM, ({0xd7, 0xb0, 0x22, 0x40}), hasScalableBinop(f, Instruction::Add))
RVV_IR(RISCV_INS_VMADC_VV, ({0xd7, 0x80, 0x21, 0x46}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMADC_VX, ({0xd7, 0x40, 0x25, 0x46}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMADC_VI, ({0xd7, 0xb0, 0x22, 0x46}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMADC_VVM, ({0xd7, 0x80, 0x21, 0x44}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMADC_VXM, ({0xd7, 0x40, 0x25, 0x44}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMADC_VIM, ({0xd7, 0xb0, 0x22, 0x44}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VSBC_VVM, ({0xd7, 0x80, 0x21, 0x48}), hasScalableBinop(f, Instruction::Sub))
RVV_IR(RISCV_INS_VSBC_VXM, ({0xd7, 0x40, 0x25, 0x48}), hasScalableBinop(f, Instruction::Sub))
RVV_IR(RISCV_INS_VMSBC_VV, ({0xd7, 0x80, 0x21, 0x4e}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSBC_VX, ({0xd7, 0x40, 0x25, 0x4e}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSBC_VVM, ({0xd7, 0x80, 0x21, 0x4c}), hasScalableICmp(f))
RVV_IR(RISCV_INS_VMSBC_VXM, ({0xd7, 0x40, 0x25, 0x4c}), hasScalableICmp(f))

RVV_IR(RISCV_INS_VREDMIN_VS, ({0xd7, 0xa0, 0x21, 0x16}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VREDMAX_VS, ({0xd7, 0xa0, 0x21, 0x1e}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VREDMINU_VS, ({0xd7, 0xa0, 0x21, 0x12}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VREDMAXU_VS, ({0xd7, 0xa0, 0x21, 0x1a}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFREDMIN_VS, ({0xd7, 0x90, 0x21, 0x16}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFREDMAX_VS, ({0xd7, 0x90, 0x21, 0x1e}), hasExtractOrSelect(f))

RVV_IR(RISCV_INS_VLSEG2E32_V, ({0x87, 0x60, 0x05, 0x22}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLSEG8E8_V, ({0x87, 0x00, 0x05, 0xe2}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VSSEG2E32_V, ({0xa7, 0x60, 0x05, 0x22}), hasAnyStore(f))
RVV_IR(RISCV_INS_VLSSEG2E32_V, ({0x87, 0x60, 0xb5, 0x2a}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VSSSEG2E32_V, ({0xa7, 0x60, 0xb5, 0x2a}), hasAnyStore(f))
RVV_IR(RISCV_INS_VLOXSEG2EI32_V, ({0x87, 0x60, 0x25, 0x2e}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLUXSEG2EI32_V, ({0x87, 0x60, 0x25, 0x26}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VSOXSEG2EI32_V, ({0xa7, 0x60, 0x25, 0x2e}), hasAnyStore(f))
RVV_IR(RISCV_INS_VSUXSEG2EI32_V, ({0xa7, 0x60, 0x25, 0x26}), hasAnyStore(f))
RVV_IR(RISCV_INS_VL1RE8_V, ({0x87, 0x00, 0x85, 0x02}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VL2RE32_V, ({0x07, 0x61, 0x85, 0x22}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VS1R_V, ({0xa7, 0x00, 0x85, 0x02}), hasAnyStore(f))
RVV_IR(RISCV_INS_VS2R_V, ({0x27, 0x01, 0x85, 0x22}), hasAnyStore(f))

RVV_IR(RISCV_INS_VMACC_VV, ({0xd7, 0xa0, 0x21, 0xb6}), hasScalableBinop(f, Instruction::Mul))
RVV_IR(RISCV_INS_VMACC_VX, ({0xd7, 0xe0, 0x25, 0xb6}), hasScalableBinop(f, Instruction::Mul))
RVV_IR(RISCV_INS_VNMSAC_VV, ({0xd7, 0xa0, 0x21, 0xbc}), hasScalableBinop(f, Instruction::Mul))
RVV_IR(RISCV_INS_VMADD_VV, ({0xd7, 0xa0, 0x21, 0xa4}), hasScalableBinop(f, Instruction::Mul))
RVV_IR(RISCV_INS_VNMSUB_VV, ({0xd7, 0xa0, 0x21, 0xac}), hasScalableBinop(f, Instruction::Mul))

RVV_IR(RISCV_INS_VMFEQ_VV, ({0xd7, 0x90, 0x21, 0x62}), hasScalableFCmp(f))
RVV_IR(RISCV_INS_VMFEQ_VF, ({0xd7, 0x50, 0x25, 0x62}), hasScalableFCmp(f))
RVV_IR(RISCV_INS_VMFNE_VV, ({0xd7, 0x90, 0x21, 0x72}), hasScalableFCmp(f))
RVV_IR(RISCV_INS_VMFLT_VV, ({0xd7, 0x90, 0x21, 0x6e}), hasScalableFCmp(f))
RVV_IR(RISCV_INS_VMFLE_VV, ({0xd7, 0x90, 0x21, 0x66}), hasScalableFCmp(f))
RVV_IR(RISCV_INS_VMFGT_VF, ({0xd7, 0x50, 0x25, 0x76}), hasScalableFCmp(f))
RVV_IR(RISCV_INS_VMFGE_VF, ({0xd7, 0x50, 0x25, 0x7e}), hasScalableFCmp(f))
RVV_IR(RISCV_INS_VFMERGE_VFM, ({0xd7, 0x50, 0x25, 0x5c}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFMV_V_F, ({0xd7, 0x50, 0x05, 0x5e}), hasAnyStore(f))
RVV_IR(RISCV_INS_VFMV_F_S, ({0x57, 0x15, 0x10, 0x42}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VFMV_S_F, ({0xd7, 0x50, 0x05, 0x42}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VMV_S_X, ({0xd7, 0x60, 0x05, 0x42}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VMV_X_S, ({0x57, 0x25, 0x10, 0x42}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VMV_V_I, ({0xd7, 0xb0, 0x02, 0x5e}), hasAnyStore(f))

RVV_IR(RISCV_INS_VCPOP_M, ({0x57, 0x25, 0x28, 0x42}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFIRST_M, ({0x57, 0xa5, 0x28, 0x42}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VMSBF_M, ({0xd7, 0xa0, 0x20, 0x52}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VMSIF_M, ({0xd7, 0xa0, 0x21, 0x52}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VMSOF_M, ({0xd7, 0x20, 0x21, 0x52}), hasExtractOrSelect(f))

RVV_IR(RISCV_INS_VSMUL_VV, ({0xd7, 0x80, 0x21, 0x9e}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VSMUL_VX, ({0xd7, 0x40, 0x25, 0x9e}), hasCastOf(f, Instruction::SExt))
RVV_IR(RISCV_INS_VSSRL_VV, ({0xd7, 0x80, 0x21, 0xaa}), hasScalableBinop(f, Instruction::LShr))
RVV_IR(RISCV_INS_VSSRL_VX, ({0xd7, 0xc0, 0x25, 0xaa}), hasScalableBinop(f, Instruction::LShr))
RVV_IR(RISCV_INS_VSSRL_VI, ({0xd7, 0xb0, 0x21, 0xaa}), hasScalableBinop(f, Instruction::LShr))
RVV_IR(RISCV_INS_VSSRA_VV, ({0xd7, 0x80, 0x21, 0xae}), hasScalableBinop(f, Instruction::AShr))
RVV_IR(RISCV_INS_VSSRA_VX, ({0xd7, 0xc0, 0x25, 0xae}), hasScalableBinop(f, Instruction::AShr))
RVV_IR(RISCV_INS_VSSRA_VI, ({0xd7, 0xb0, 0x21, 0xae}), hasScalableBinop(f, Instruction::AShr))

RVV_IR(RISCV_INS_VRGATHEREI16_VV, ({0xd7, 0x80, 0x21, 0x38}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFREDOSUM_VS, ({0xd7, 0x90, 0x21, 0x0e}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFWREDUSUM_VS, ({0xd7, 0x90, 0x21, 0xc6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFWREDOSUM_VS, ({0xd7, 0x90, 0x21, 0xcc}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VANDN_VV, ({0xd7, 0x80, 0x21, 0x04}), hasScalableBinop(f, Instruction::And))
RVV_IR(RISCV_INS_VANDN_VX, ({0xd7, 0x40, 0x25, 0x04}), hasScalableBinop(f, Instruction::And))
RVV_IR(RISCV_INS_VROR_VV, ({0xd7, 0x80, 0x21, 0x52}), hasScalableBinop(f, Instruction::Or))
RVV_IR(RISCV_INS_VROR_VX, ({0xd7, 0xc0, 0x25, 0x52}), hasScalableBinop(f, Instruction::Or))
RVV_IR(RISCV_INS_VROR_VI, ({0xd7, 0xb0, 0x21, 0x52}), hasScalableBinop(f, Instruction::Or))
RVV_IR(RISCV_INS_VROL_VV, ({0xd7, 0x80, 0x21, 0x56}), hasScalableBinop(f, Instruction::Or))
RVV_IR(RISCV_INS_VROL_VX, ({0xd7, 0xc0, 0x25, 0x56}), hasScalableBinop(f, Instruction::Or))
RVV_IR(RISCV_INS_VRSUB_VX, ({0xd7, 0xc0, 0x25, 0x0e}), hasScalableBinop(f, Instruction::Sub))
RVV_IR(RISCV_INS_VRSUB_VI, ({0xd7, 0xb0, 0x21, 0x0e}), hasScalableBinop(f, Instruction::Sub))
RVV_IR(RISCV_INS_VWSLL_VV, ({0xd7, 0xa0, 0x21, 0xd6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWSLL_VX, ({0xd7, 0xe0, 0x25, 0xd6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWSLL_VI, ({0xd7, 0xb0, 0x21, 0xd6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VCLMUL_VV, ({0xd7, 0xa0, 0x21, 0x32}), hasScalableBinop(f, Instruction::Xor))
RVV_IR(RISCV_INS_VCLMUL_VX, ({0xd7, 0xe0, 0x25, 0x32}), hasScalableBinop(f, Instruction::Xor))
RVV_IR(RISCV_INS_VCLMULH_VV, ({0xd7, 0xa0, 0x21, 0x36}), hasScalableBinop(f, Instruction::Xor))
RVV_IR(RISCV_INS_VBREV_V, ({0xd7, 0x20, 0x25, 0x4a}), hasNamedCall(f, "llvm.bitreverse"))
RVV_IR(RISCV_INS_VCLZ_V, ({0xd7, 0x20, 0x26, 0x4a}), hasNamedCall(f, "llvm.ctlz"))
RVV_IR(RISCV_INS_VCTZ_V, ({0xd7, 0xa0, 0x26, 0x4a}), hasNamedCall(f, "llvm.cttz"))
RVV_IR(RISCV_INS_VGHSH_VV, ({0xf7, 0xa0, 0x21, 0xb2}), hasNamedCall(f, "llvm.riscv.vghsh"))
RVV_IR(RISCV_INS_VGMUL_VV, ({0xf7, 0xa0, 0x28, 0xa2}), hasNamedCall(f, "llvm.riscv.vgmul"))

RVV_IR(RISCV_INS_VLE8FF_V, ({0x87, 0x00, 0x05, 0x03}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLE16FF_V, ({0x87, 0x50, 0x05, 0x03}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLE32FF_V, ({0x87, 0x60, 0x05, 0x03}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLE64FF_V, ({0x87, 0x70, 0x05, 0x03}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLSEG2E32FF_V, ({0x87, 0x60, 0x05, 0x23}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VLM_V, ({0x87, 0x00, 0xb5, 0x02}), hasAnyLoad(f))
RVV_IR(RISCV_INS_VSM_V, ({0xa7, 0x00, 0xb5, 0x02}), hasAnyStore(f))
RVV_IR(RISCV_INS_VCPOP_V, ({0xd7, 0x20, 0x27, 0x4a}), hasNamedCall(f, "llvm.ctpop"))
RVV_IR(RISCV_INS_VBREV8_V, ({0xd7, 0x20, 0x24, 0x4a}), hasNamedCall(f, "llvm.bitreverse"))
RVV_IR(RISCV_INS_VREV8_V, ({0xd7, 0xa0, 0x24, 0x4a}), hasNamedCall(f, "llvm.bswap"))
RVV_IR(RISCV_INS_VFREC7_V, ({0xd7, 0x90, 0x22, 0x4e}), hasScalableBinop(f, Instruction::FDiv))
RVV_IR(RISCV_INS_VFRSQRT7_V, ({0xd7, 0x10, 0x22, 0x4e}), hasNamedCall(f, "llvm.sqrt"))
RVV_IR(RISCV_INS_VFWADD_VV, ({0xd7, 0x90, 0x21, 0xc2}), hasCastOf(f, Instruction::FPExt))
RVV_IR(RISCV_INS_VFWADD_VF, ({0xd7, 0x50, 0x25, 0xc2}), hasCastOf(f, Instruction::FPExt))
RVV_IR(RISCV_INS_VFWADD_WV, ({0xd7, 0x90, 0x21, 0xd2}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWADD_WF, ({0xd7, 0x50, 0x25, 0xd2}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWSUB_VV, ({0xd7, 0x90, 0x21, 0xca}), hasScalableBinop(f, Instruction::FSub))
RVV_IR(RISCV_INS_VFWSUB_VF, ({0xd7, 0x50, 0x25, 0xca}), hasScalableBinop(f, Instruction::FSub))
RVV_IR(RISCV_INS_VFWSUB_WV, ({0xd7, 0x90, 0x21, 0xda}), hasScalableBinop(f, Instruction::FSub))
RVV_IR(RISCV_INS_VFWSUB_WF, ({0xd7, 0x50, 0x25, 0xda}), hasScalableBinop(f, Instruction::FSub))
RVV_IR(RISCV_INS_VFWMUL_VV, ({0xd7, 0x90, 0x21, 0xe2}), hasScalableBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFWMUL_VF, ({0xd7, 0x50, 0x25, 0xe2}), hasScalableBinop(f, Instruction::FMul))
RVV_IR(RISCV_INS_VFWMACC_VV, ({0xd7, 0x90, 0x21, 0xf2}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWMACC_VF, ({0xd7, 0x50, 0x25, 0xf2}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWNMACC_VV, ({0xd7, 0x90, 0x21, 0xf6}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWNMACC_VF, ({0xd7, 0x50, 0x25, 0xf6}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWMSAC_VV, ({0xd7, 0x90, 0x21, 0xfa}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWMSAC_VF, ({0xd7, 0x50, 0x25, 0xfa}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWNMSAC_VV, ({0xd7, 0x90, 0x21, 0xfe}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VFWNMSAC_VF, ({0xd7, 0x50, 0x25, 0xfe}), hasScalableBinop(f, Instruction::FAdd))
RVV_IR(RISCV_INS_VWREDSUM_VS, ({0xd7, 0xa0, 0x21, 0xc6}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VWREDSUMU_VS, ({0xd7, 0xa0, 0x21, 0xc2}), hasExtractOrSelect(f))
RVV_IR(RISCV_INS_VFWCVTBF16_F_F_V, ({0xd7, 0x90, 0x26, 0x4a}), hasCastOf(f, Instruction::ZExt))
RVV_IR(RISCV_INS_VFNCVTBF16_F_F_W, ({0xd7, 0x90, 0x2e, 0x4a}), hasCastOf(f, Instruction::Trunc))
RVV_IR(RISCV_INS_VFWMACCBF16_VV, ({0xd7, 0x90, 0x21, 0xee}), hasNamedCall(f, "llvm.fma"))
RVV_IR(RISCV_INS_VFWMACCBF16_VF, ({0xd7, 0x50, 0x25, 0xee}), hasNamedCall(f, "llvm.fma"))

#undef RVV_IR

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VT_MASKC)
{
	ONLY_MODE_RV64;
	auto* f = translate({0x7b, 0xf5, 0xc5, 0x00});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasExtractOrSelect(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorRiscvTests, RISCV_INS_VT_MASKCN)
{
	ONLY_MODE_RV64;
	auto* f = translate({0x7b, 0xfd, 0xc5, 0x00});
	ASSERT_NE(nullptr, f);
	EXPECT_TRUE(hasExtractOrSelect(f)) << dumpFunction(f);
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec


