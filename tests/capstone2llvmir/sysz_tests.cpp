/**
 * @file tests/capstone2llvmir/sysz_tests.cpp
 * @brief Capstone2LlvmIrTranslatorSysz unit tests.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/ExecutionEngine/GenericValue.h>
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
			// extra=0: createSysz already opens CS_MODE_BIG_ENDIAN.
			// Passing BIG_ENDIAN again wraps 1U<<31 + 1U<<31 to 0.
			_translator = Capstone2LlvmIrTranslator::createSysz(&_module);
		}

		void setVr(uint32_t reg, uint64_t hi, uint64_t lo)
		{
			auto* gv = _translator->getRegister(reg);
			ASSERT_NE(gv, nullptr);
			llvm::GenericValue v = _emulator->getGlobalVariableValue(gv);
			const uint64_t words[2] = {lo, hi};
			v.IntVal = llvm::APInt(128, llvm::ArrayRef<uint64_t>(words, 2));
			_emulator->setGlobalVariableValue(gv, v);
		}

		uint64_t vrLow(uint32_t reg)
		{
			auto* gv = _translator->getRegister(reg);
			return _emulator->getGlobalVariableValue(gv).IntVal.trunc(64).getZExtValue();
		}

		uint64_t vrHigh(uint32_t reg)
		{
			auto* gv = _translator->getRegister(reg);
			return _emulator->getGlobalVariableValue(gv).IntVal.lshr(64).trunc(64).getZExtValue();
		}

		void setMemoryValue128(uint64_t addr, uint64_t hi, uint64_t lo)
		{
			llvm::GenericValue v;
			const uint64_t words[2] = {lo, hi};
			v.IntVal = llvm::APInt(128, llvm::ArrayRef<uint64_t>(words, 2));
			_emulator->setMemoryValue(addr, v);
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
		{0x1000, 0x0123456789abcdef_qw},
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

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_AEBR)
{
	setRegisters({
		{SYSZ_REG_F0S, 1.5_f32},
		{SYSZ_REG_F2S, 2.25_f32},
	});

	emulate_bin("b3 0a 00 02");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_F0S, SYSZ_REG_F2S});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_F0S, 3.75_f32},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_ADBR)
{
	setRegisters({
		{SYSZ_REG_F0D, 1.5_f64},
		{SYSZ_REG_F2D, 2.25_f64},
	});

	emulate_bin("b3 1a 00 02");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_F0D, SYSZ_REG_F2D});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_F0D, 3.75_f64},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_MEEBR)
{
	setRegisters({
		{SYSZ_REG_F0S, 1.5_f32},
		{SYSZ_REG_F2S, 4.0_f32},
	});

	emulate_bin("b3 17 00 02");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_F0S, SYSZ_REG_F2S});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_F0S, 6.0_f32},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LDEB)
{
	setRegisters({
		{SYSZ_REG_F0D, 0.0_f64},
		{SYSZ_REG_R2D, 0x1000},
	});
	setMemory({
		{0x1000, 1.5_f32},
	});

	emulate_bin("ed 00 20 00 00 04");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_R2D});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_F0D, 1.5_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_CEBR)
{
	setRegisters({
		{SYSZ_REG_F0S, 1.5_f32},
		{SYSZ_REG_F2S, 2.25_f32},
	});

	emulate_bin("b3 09 00 02");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_F0S, SYSZ_REG_F2S});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_CC, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LD)
{
	setRegisters({
		{SYSZ_REG_F0D, 0.0_f64},
		{SYSZ_REG_R2D, 0x1000},
	});
	setMemory({
		{0x1000, 3.5_f64},
	});

	emulate_bin("68 00 20 00");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_R2D});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_F0D, 3.5_f64},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_STD)
{
	setRegisters({
		{SYSZ_REG_F0D, 3.5_f64},
		{SYSZ_REG_R2D, 0x1000},
	});

	emulate_bin("60 00 20 00");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_F0D, SYSZ_REG_R2D});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 3.5_f64},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LGHI)
{
	emulate_bin("a7 19 00 05");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_R1D, 5},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_AGHI)
{
	setRegisters({
		{SYSZ_REG_R1D, 10},
	});

	emulate_bin("a7 1a 00 05");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_R1D});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_R1D, 15},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_NGR)
{
	setRegisters({
		{SYSZ_REG_R1D, 0xff00ff00ff00ull},
		{SYSZ_REG_R2D, 0x0ff00ff00ff0ull},
	});

	emulate_bin("b9 80 00 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_R1D, SYSZ_REG_R2D});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_R1D, 0x0f000f000f00ull},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_CGR)
{
	setRegisters({
		{SYSZ_REG_R1D, 1},
		{SYSZ_REG_R2D, 2},
	});

	emulate_bin("b9 20 00 12");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_R1D, SYSZ_REG_R2D});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_CC, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LAY)
{
	setRegisters({
		{SYSZ_REG_R1D, 0},
		{SYSZ_REG_R2D, 0x100000000ull},
	});

	emulate_bin("e3 10 20 08 00 71");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_R2D});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_R1D, 0x100000008ull},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLR)
{
	setVr(SYSZ_REG_V2, 0x0123456789abcdefull, 0xfedcba9876543210ull);

	emulate_bin("e7 02 00 00 00 56");

	EXPECT_EQ(0xfedcba9876543210ull, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x0123456789abcdefull, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VL)
{
	setRegisters({
		{SYSZ_REG_R2D, 0x1000},
	});
	setMemoryValue128(0x1000, 0x1111111111111111ull, 0x2222222222222222ull);

	emulate_bin("e7 00 20 00 00 06");

	EXPECT_EQ(0x2222222222222222ull, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x1111111111111111ull, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_MVC)
{
	setRegisters({
		{SYSZ_REG_R3D, 0x2000},
		{SYSZ_REG_R4D, 0x1000},
	});
	setMemory({
		{0x1000, 0x11223344_dw},
	});

	emulate_bin("d2 03 30 00 40 00");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_R3D, SYSZ_REG_R4D});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x2000, 0x11223344_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LMG)
{
	setRegisters({
		{SYSZ_REG_R2D, 0x1000},
	});
	setMemory({
		{0x1000, 0x1111111111111111_qw},
		{0x1008, 0x2222222222222222_qw},
		{0x1010, 0x3333333333333333_qw},
	});

	emulate_bin("eb 68 20 00 00 04");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_R2D});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_R6D, 0x1111111111111111ull},
		{SYSZ_REG_R7D, 0x2222222222222222ull},
		{SYSZ_REG_R8D, 0x3333333333333333ull},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008, 0x1010});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_CSG)
{
	setRegisters({
		{SYSZ_REG_R1D, 0xaa},
		{SYSZ_REG_R2D, 0xbb},
		{SYSZ_REG_R3D, 0x1000},
	});
	setMemory({
		{0x1000, 0xaa_qw},
	});

	emulate_bin("eb 12 30 00 00 30");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_R1D, 0xaa},
		{SYSZ_REG_CC, 0},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xbb_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VAB)
{
	setVr(SYSZ_REG_V1, 0, 0x00000000FF000001ull);
	setVr(SYSZ_REG_V2, 0, 0x0000000002000001ull);

	emulate_bin("e7 01 20 00 00 f3");

	EXPECT_EQ(0x0000000001000002ull, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_MAEBR)
{
	setRegisters({
		{SYSZ_REG_F0S, 1.0_f32},
		{SYSZ_REG_F2S, 2.0_f32},
		{SYSZ_REG_F4S, 3.0_f32},
	});

	emulate_bin("b3 0e 00 24");

	EXPECT_JUST_REGISTERS_LOADED({SYSZ_REG_F0S, SYSZ_REG_F2S, SYSZ_REG_F4S});
	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_F0S, 7.0_f32},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VPDI)
{
	setVr(SYSZ_REG_V1, 0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL);
	setVr(SYSZ_REG_V2, 0xCCCCCCCCCCCCCCCCULL, 0xDDDDDDDDDDDDDDDDULL);

	emulate_bin("e7 01 20 00 00 84");

	EXPECT_EQ(0xCCCCCCCCCCCCCCCCULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0xAAAAAAAAAAAAAAAAULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VMLF)
{
	setVr(SYSZ_REG_V1, 0, 0x0000000300000002ULL);
	setVr(SYSZ_REG_V2, 0, 0x0000000400000005ULL);

	emulate_bin("e7 01 20 00 20 a2");

	EXPECT_EQ(0x0000000C0000000AULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VREPB)
{
	setVr(SYSZ_REG_V1, 0xAB00000000000000ULL, 0);

	emulate_bin("e7 01 00 00 00 4d");

	EXPECT_EQ(0xABABABABABABABABULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0xABABABABABABABABULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VPERM)
{
	setVr(SYSZ_REG_V1, 0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL);
	setVr(SYSZ_REG_V2, 0, 0);
	setVr(SYSZ_REG_V3, 0x0001020304050607ULL, 0x08090A0B0C0D0E0FULL);

	emulate_bin("e7 01 20 00 30 8c");

	EXPECT_EQ(0x8899AABBCCDDEEFFULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x0011223344556677ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_CDS)
{
	setRegisters({
		{SYSZ_REG_R0D, 0xaa},
		{SYSZ_REG_R1D, 0xbb},
		{SYSZ_REG_R2D, 0xcc},
		{SYSZ_REG_R3D, 0x1000},
		{SYSZ_REG_R4D, 0xdd},
		{SYSZ_REG_R5D, 0xee},
	});
	setMemory({
		{0x1000, 0x000000aa000000bb_qw},
	});

	emulate_bin("bb 04 30 00");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_R0D, 0xaa},
		{SYSZ_REG_R1D, 0xbb},
		{SYSZ_REG_CC, 0},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x000000dd000000ee_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_CDSG)
{
	setRegisters({
		{SYSZ_REG_R0D, 0x1111},
		{SYSZ_REG_R1D, 0x2222},
		{SYSZ_REG_R3D, 0x1000},
		{SYSZ_REG_R4D, 0x3333},
		{SYSZ_REG_R5D, 0x4444},
	});
	setMemoryValue128(0x1000, 0x1111, 0x2222);

	emulate_bin("eb 04 30 00 00 3e");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_R0D, 0x1111},
		{SYSZ_REG_R1D, 0x2222},
		{SYSZ_REG_CC, 0},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VPKF)
{
	setVr(SYSZ_REG_V1, 0x0001000200030004ULL, 0x0005000600070008ULL);
	setVr(SYSZ_REG_V2, 0x0009000A000B000CULL, 0x000D000E000F0010ULL);

	emulate_bin("e7 01 20 00 20 94");

	EXPECT_EQ(0x000A000C000E0010ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x0002000400060008ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_FIDBR)
{
	setRegisters({
		{SYSZ_REG_F2D, 3.9_f64},
	});

	emulate_bin("b3 5f 50 02");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_F0D, 3.0_f64},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_FIEBR)
{
	setRegisters({
		{SYSZ_REG_F2S, 3.9_f32},
	});

	emulate_bin("b3 57 50 02");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_F0S, 3.0_f32},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LOCGRNE)
{
	setRegisters({
		{SYSZ_REG_1, 0xaa},
		{SYSZ_REG_2, 0xbb},
		{SYSZ_REG_CC, 1},
	});

	emulate_bin("b9 e2 70 12");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xbb},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LOCGRNE_untaken)
{
	setRegisters({
		{SYSZ_REG_1, 0xaa},
		{SYSZ_REG_2, 0xbb},
		{SYSZ_REG_CC, 0},
	});

	emulate_bin("b9 e2 70 12");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xaa},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LOCGRH)
{
	setRegisters({
		{SYSZ_REG_1, 0x11},
		{SYSZ_REG_2, 0x22},
		{SYSZ_REG_CC, 2},
	});

	emulate_bin("b9 e2 20 12");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x22},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LOCGNE)
{
	setRegisters({
		{SYSZ_REG_1, 0xaa},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_CC, 1},
	});
	setMemory({
		{0x1000, 0xcc_qw},
	});

	emulate_bin("eb 17 20 00 00 e2");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xcc},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LOCRNE)
{
	setRegisters({
		{SYSZ_REG_1, 0x11111111aaaaaaaallu},
		{SYSZ_REG_2, 0x22222222bbbbbbbbllu},
		{SYSZ_REG_CC, 1},
	});

	emulate_bin("b9 f2 70 12");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x11111111bbbbbbbbllu},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_RISBG)
{
	setRegisters({
		{SYSZ_REG_1, 0xAAAAAAAAAAAAAAAAULL},
		{SYSZ_REG_2, 0x1111222233334444ULL},
	});

	emulate_bin("ec 12 20 bf 00 55");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x33334444ULL},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_RISBGN)
{
	setRegisters({
		{SYSZ_REG_1, 0xAAAAAAAAAAAAAAAAULL},
		{SYSZ_REG_2, 0x1111222233334444ULL},
		{SYSZ_REG_CC, 3},
	});

	emulate_bin("ec 12 20 3f 00 59");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xAAAAAAAA33334444ULL},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LAA)
{
	setRegisters({
		{SYSZ_REG_1, 0xFFFF00000000000AULL},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_3, 5},
	});
	setMemory({
		{0x1000, 20_dw},
	});

	emulate_bin("eb 13 20 00 00 f8");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xFFFF000000000014ULL},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 25_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LAAG)
{
	setRegisters({
		{SYSZ_REG_1, 0xaa},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_3, 5},
	});
	setMemory({
		{0x1000, 0x20_qw},
	});

	emulate_bin("eb 13 20 00 00 e8");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x20},
		{SYSZ_REG_CC, 2},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x25_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VFMADB)
{
	setVr(SYSZ_REG_V1, 0, 0x4000000000000000ULL);
	setVr(SYSZ_REG_V2, 0, 0x4008000000000000ULL);
	setVr(SYSZ_REG_V3, 0, 0x4010000000000000ULL);

	emulate_bin("e7 01 23 00 30 8f");

	EXPECT_EQ(0x4024000000000000ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VSEL)
{
	setVr(SYSZ_REG_V1, 0xAAAAAAAAAAAAAAAAULL, 0xAAAAAAAAAAAAAAAAULL);
	setVr(SYSZ_REG_V2, 0xBBBBBBBBBBBBBBBBULL, 0xBBBBBBBBBBBBBBBBULL);
	setVr(SYSZ_REG_V3, 0x00FF00FF00FF00FFULL, 0x00FF00FF00FF00FFULL);

	emulate_bin("e7 01 20 00 30 8d");

	EXPECT_EQ(0xAABBAABBAABBAABBULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0xAABBAABBAABBAABBULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VAVGB)
{
	setVr(SYSZ_REG_V1, 0, 2);
	setVr(SYSZ_REG_V2, 0, 4);

	emulate_bin("e7 01 20 00 00 f2");

	EXPECT_EQ(3u, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VCHB)
{
	setVr(SYSZ_REG_V1, 0, 5);
	setVr(SYSZ_REG_V2, 0, 3);

	emulate_bin("e7 01 20 00 00 fb");

	EXPECT_EQ(0xFFu, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LAN)
{
	setRegisters({
		{SYSZ_REG_1, 0xFFFF00000000000AULL},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_3, 0x0F},
	});
	setMemory({
		{0x1000, 0x3C_dw},
	});

	emulate_bin("eb 13 20 00 00 f4");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xFFFF00000000003CULL},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x0C_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LANG)
{
	setRegisters({
		{SYSZ_REG_1, 0xaa},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_3, 0x0F},
	});
	setMemory({
		{0x1000, 0x3C_qw},
	});

	emulate_bin("eb 13 20 00 00 e4");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x3C},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x0C_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LAO)
{
	setRegisters({
		{SYSZ_REG_1, 0xFFFF00000000000AULL},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_3, 0x0F},
	});
	setMemory({
		{0x1000, 0x30_dw},
	});

	emulate_bin("eb 13 20 00 00 f6");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xFFFF000000000030ULL},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x3F_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LAOG)
{
	setRegisters({
		{SYSZ_REG_1, 0xaa},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_3, 0x0F},
	});
	setMemory({
		{0x1000, 0x30_qw},
	});

	emulate_bin("eb 13 20 00 00 e6");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x30},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x3F_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LOCHINE)
{
	setRegisters({
		{SYSZ_REG_1, 0x11111111aaaaaaaallu},
		{SYSZ_REG_CC, 1},
	});

	emulate_bin("ec 17 00 05 00 42");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x1111111100000005llu},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LOCHINE_untaken)
{
	setRegisters({
		{SYSZ_REG_1, 0x11111111aaaaaaaallu},
		{SYSZ_REG_CC, 0},
	});

	emulate_bin("ec 17 00 05 00 42");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x11111111aaaaaaaallu},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LOCHIH)
{
	setRegisters({
		{SYSZ_REG_1, 0x11111111aaaaaaaallu},
		{SYSZ_REG_CC, 2},
	});

	emulate_bin("ec 12 ff ff 00 42");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x11111111ffffffffllu},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VESLB)
{
	setVr(SYSZ_REG_V1, 0, 0x0101010101010101ULL);

	emulate_bin("e7 01 00 01 00 30");

	EXPECT_EQ(0x0202020202020202ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VESRAB)
{
	setVr(SYSZ_REG_V1, 0, 0x80);

	emulate_bin("e7 01 00 01 00 3a");

	EXPECT_EQ(0xC0u, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VESRLB)
{
	setVr(SYSZ_REG_V1, 0, 0x80);

	emulate_bin("e7 01 00 01 00 38");

	EXPECT_EQ(0x40u, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLCB)
{
	setVr(SYSZ_REG_V1, 0, 1);

	emulate_bin("e7 01 00 00 00 de");

	EXPECT_EQ(0xFFu, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLPB)
{
	setVr(SYSZ_REG_V1, 0, 0xFF);

	emulate_bin("e7 01 00 00 00 df");

	EXPECT_EQ(1u, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LAX)
{
	setRegisters({
		{SYSZ_REG_1, 0xFFFF00000000000AULL},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_3, 0x0F},
	});
	setMemory({
		{0x1000, 0x3C_dw},
	});

	emulate_bin("eb 13 20 00 00 f7");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0xFFFF00000000003CULL},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x33_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_LAXG)
{
	setRegisters({
		{SYSZ_REG_1, 0xaa},
		{SYSZ_REG_2, 0x1000},
		{SYSZ_REG_3, 0x0F},
	});
	setMemory({
		{0x1000, 0x3C_qw},
	});

	emulate_bin("eb 13 20 00 00 e7");

	EXPECT_JUST_REGISTERS_STORED({
		{SYSZ_REG_1, 0x3C},
		{SYSZ_REG_CC, 1},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x33_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLLEZB)
{
	setRegisters({
		{SYSZ_REG_R2D, 0x1000},
	});
	setMemory({
		{0x1000, 0xAB_b},
	});

	emulate_bin("e7 00 20 00 00 04");

	EXPECT_EQ(0, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0xAB00000000000000ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLLEZG)
{
	setRegisters({
		{SYSZ_REG_R2D, 0x1000},
	});
	setMemory({
		{0x1000, 0x1122334455667788_qw},
	});

	emulate_bin("e7 00 20 00 30 04");

	EXPECT_EQ(0, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x1122334455667788ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VUPHB)
{
	setVr(SYSZ_REG_V1, 0, 0x0102030405060780ULL);

	emulate_bin("e7 01 00 00 00 d7");

	EXPECT_EQ(0x000500060007FF80ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x0001000200030004ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VUPLB)
{
	setVr(SYSZ_REG_V1, 0x0102030405060780ULL, 0);

	emulate_bin("e7 01 00 00 00 d6");

	EXPECT_EQ(0x000500060007FF80ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x0001000200030004ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VSEGB)
{
	setVr(SYSZ_REG_V1, 0, 0x80);

	emulate_bin("e7 01 00 00 00 5f");

	EXPECT_EQ(0xFFFFFFFFFFFFFF80ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0xFFFFFFFFFFFFFF80ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VGEF)
{
	setVr(SYSZ_REG_V0, 0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL);
	setVr(SYSZ_REG_V1, 0, 0);
	setRegisters({
		{SYSZ_REG_R2D, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate_bin("e7 01 20 00 00 13");

	EXPECT_EQ(0xBBBBBBBBBBBBBBBBULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x12345678AAAAAAAAULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VGBM)
{
	emulate_bin("e7 00 80 00 00 44");

	EXPECT_EQ(0, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0xFF00000000000000ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VGBM_low_bit)
{
	emulate_bin("e7 00 00 01 00 44");

	EXPECT_EQ(0xFF, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VGMB)
{
	emulate_bin("e7 00 00 07 00 46");

	EXPECT_EQ(0xFFFFFFFFFFFFFFFFULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0xFFFFFFFFFFFFFFFFULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLEIB)
{
	setVr(SYSZ_REG_V0, 0x1111111111111111ULL, 0x1111111111111111ULL);

	emulate_bin("e7 00 00 ab 00 40");

	EXPECT_EQ(0x1111111111111111ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0xAB11111111111111ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLEIB_lane15)
{
	setVr(SYSZ_REG_V0, 0x1111111111111111ULL, 0x1111111111111111ULL);

	emulate_bin("e7 00 00 ab f0 40");

	EXPECT_EQ(0x11111111111111ABULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x1111111111111111ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLEIH)
{
	setVr(SYSZ_REG_V0, 0x1111111111111111ULL, 0x1111111111111111ULL);

	emulate_bin("e7 00 12 34 00 41");

	EXPECT_EQ(0x1111111111111111ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x1234111111111111ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLEIF)
{
	setVr(SYSZ_REG_V0, 0x1111111111111111ULL, 0x1111111111111111ULL);

	emulate_bin("e7 00 12 34 00 43");

	EXPECT_EQ(0x1111111111111111ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x0000123411111111ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VLEIG)
{
	setVr(SYSZ_REG_V0, 0x1111111111111111ULL, 0x1111111111111111ULL);

	emulate_bin("e7 00 12 34 00 42");

	EXPECT_EQ(0x1111111111111111ULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x0000000000001234ULL, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VSCEF)
{
	setVr(SYSZ_REG_V0, 0x12345678AAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL);
	setVr(SYSZ_REG_V1, 0, 0);
	setRegisters({
		{SYSZ_REG_R2D, 0x1000},
	});

	emulate_bin("e7 01 20 00 00 1b");

	EXPECT_EQ(0xBBBBBBBBBBBBBBBBULL, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x12345678AAAAAAAAULL, vrHigh(SYSZ_REG_V0));
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x12345678_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VFEEB)
{
	setVr(SYSZ_REG_V1, 0x000000AA00000000ULL, 0);
	setVr(SYSZ_REG_V2, 0x111111AA00000000ULL, 0);

	emulate_bin("e7 01 20 00 00 80");

	EXPECT_EQ(0, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(3, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VFEEB_none)
{
	setVr(SYSZ_REG_V1, 0x0101010101010101ULL, 0x0101010101010101ULL);
	setVr(SYSZ_REG_V2, 0x0202020202020202ULL, 0x0202020202020202ULL);

	emulate_bin("e7 01 20 00 00 80");

	EXPECT_EQ(0, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(0x10, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VFEEBS)
{
	setVr(SYSZ_REG_V1, 0x000000AA00000000ULL, 0);
	setVr(SYSZ_REG_V2, 0x111111AA00000000ULL, 0);

	emulate_bin("e7 01 20 10 00 80");

	EXPECT_EQ(0, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(3, vrHigh(SYSZ_REG_V0));
	EXPECT_EQ(1, getRegisterValueUnsigned(SYSZ_REG_CC));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_F(Capstone2LlvmIrTranslatorSyszTests, SYSZ_INS_VFENEB)
{
	setVr(SYSZ_REG_V1, 0x11111111111111AAULL, 0);
	setVr(SYSZ_REG_V2, 0x11111111111111BBULL, 0);

	emulate_bin("e7 01 20 00 00 81");

	EXPECT_EQ(0, vrLow(SYSZ_REG_V0));
	EXPECT_EQ(7, vrHigh(SYSZ_REG_V0));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
