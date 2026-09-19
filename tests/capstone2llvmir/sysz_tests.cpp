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

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
