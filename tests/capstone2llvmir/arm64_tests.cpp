/**
 * @file tests/capstone2llvmir/arm64_tests.cpp
 * @brief Capstone2LlvmIrTranslatorArm64 unit tests.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cstring>
#include <llvm/IR/InstIterator.h>

#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/arm64/arm64.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorArm64Tests :
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
				case CS_MODE_ARM: mode = KS_MODE_LITTLE_ENDIAN; break;
				// Extra modes.
				case CS_MODE_MCLASS: mode = KS_MODE_LITTLE_ENDIAN; break; // Missing in Keystone.
				case CS_MODE_V8: mode = KS_MODE_V8; break;
				// Unhandled modes.
				default: throw std::runtime_error("ERROR: unknown mode.\n");
			}
			if (ks_open(KS_ARCH_ARM64, mode, &_assembler) != KS_ERR_OK)
			{
				throw std::runtime_error("ERROR: failed on ks_open().\n");
			}
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			switch(GetParam())
			{
				case CS_MODE_ARM:
					_translator = Capstone2LlvmIrTranslator::createArm64(&_module);
					break;
				default:
					throw std::runtime_error("ERROR: unknown mode.\n");
			}
		}

	protected:
		Capstone2LlvmIrTranslatorArm64* getArm64Translator()
		{
			return dynamic_cast<Capstone2LlvmIrTranslatorArm64*>(_translator.get());
		}

		// V registers are i128 and StoredValue tops out at 64 bits -- its
		// `_ow` literal is an `assert(false)` -- so the two halves are read
		// and written directly here. Both matter: the whole point of the
		// 64-bit NEON arrangements is that they ZERO the upper half, and
		// getRegisterValueUnsigned() would assert on a register whose value
		// does not fit in 64 bits rather than report it.
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

		void setMemoryValue128(uint64_t addr, uint64_t hi, uint64_t lo)
		{
			llvm::GenericValue v;
			const uint64_t words[2] = {lo, hi};
			v.IntVal = llvm::APInt(128, llvm::ArrayRef<uint64_t>(words, 2));
			_emulator->setMemoryValue(addr, v);
		}

	// Some of these (or their parts) might be moved to abstract parent class.
	//
	protected:
		uint32_t getParentRegister(uint32_t reg)
		{
			return getArm64Translator()->getParentRegister(reg);
		}

		virtual llvm::GlobalVariable* getRegister(uint32_t reg) override
		{
			return _translator->getRegister(getParentRegister(reg));
		}

		// b/h/s/d/q are views of v, which is an i128 -- so a floating-point
		// register's bits live in the parent's IntVal, not in a GenericValue's
		// DoubleVal or FloatVal. These four reinterpret rather than reading a
		// field that was never written.
		// s0 holds a float and d0 a double, so which of the two a register
		// means depends on its width -- and the width is the VIEW's, not the
		// i128 parent's.
		bool viewIsSingle(uint32_t reg)
		{
			return _translator->getRegisterBitSize(reg) == 32;
		}

		virtual double getRegisterValueDouble(uint32_t reg) override
		{
			uint64_t bits = getRegisterValueUnsigned(reg);
			if (viewIsSingle(reg))
			{
				float f;
				uint32_t b32 = static_cast<uint32_t>(bits);
				std::memcpy(&f, &b32, sizeof f);
				return static_cast<double>(f);
			}
			double d;
			std::memcpy(&d, &bits, sizeof d);
			return d;
		}

		virtual float getRegisterValueFloat(uint32_t reg) override
		{
			return static_cast<float>(getRegisterValueDouble(reg));
		}

		virtual void setRegisterValueDouble(uint32_t reg, double val) override
		{
			if (viewIsSingle(reg))
			{
				float f = static_cast<float>(val);
				uint32_t b32;
				std::memcpy(&b32, &f, sizeof b32);
				setRegisterValueUnsigned(reg, b32);
				return;
			}
			uint64_t bits;
			std::memcpy(&bits, &val, sizeof bits);
			setRegisterValueUnsigned(reg, bits);
		}

		virtual void setRegisterValueFloat(uint32_t reg, float val) override
		{
			setRegisterValueDouble(reg, static_cast<double>(val));
		}

		// Since b/h/s/d/q are views of v, the parent here can be 128 bits
		// wide and the view can be 8, 16 or 32 -- so this truncates to the
		// view's own width rather than switching on a list of the two widths
		// general-purpose registers happen to come in.
		virtual uint64_t getRegisterValueUnsigned(uint32_t reg) override
		{
			auto preg = getParentRegister(reg);
			auto* gv = getRegister(preg);
			const llvm::APInt& whole = _emulator->getGlobalVariableValue(gv).IntVal;
			uint64_t val = whole.getBitWidth() > 64 ? whole.trunc(64).getZExtValue() : whole.getZExtValue();

			unsigned bits = reg == preg ? 64 : _translator->getRegisterBitSize(reg);
			if (bits == 0 || bits >= 64)
			{
				return val;
			}
			return val & ((1ULL << bits) - 1);
		}

		// Writes the view's own bits into the parent and leaves the rest of
		// the parent alone. This is test SETUP, so preserving the remainder is
		// what a caller wants; the translator's own storeRegister() zeroes it,
		// which is what the hardware does.
		//
		// Was a switch over {32, 64} that threw on anything else and read the
		// parent with getZExtValue(). Now that b/h/s/d/q are views of the
		// i128 v registers, both of those are wrong: the widths run 8 to 128
		// and the parent does not fit in a uint64_t.
		virtual void setRegisterValueUnsigned(uint32_t reg, uint64_t val) override
		{
			auto preg = getParentRegister(reg);
			auto* gv = getRegister(preg);
			auto* t = cast<llvm::IntegerType>(gv->getValueType());
			unsigned wholeBits = t->getBitWidth();

			GenericValue v = _emulator->getGlobalVariableValue(gv);

			unsigned bits = reg == preg ? wholeBits : _translator->getRegisterBitSize(reg);
			if (bits == 0 || bits > wholeBits)
			{
				throw std::runtime_error("Unknown reg bit size.");
			}

			APInt fresh(wholeBits, val, /*isSigned=*/false, /*implicitTrunc=*/true);
			APInt keep = APInt::getLowBitsSet(wholeBits, bits);
			v.IntVal = (v.IntVal & ~keep) | (fresh & keep);
			_emulator->setGlobalVariableValue(gv, v);
		}

};

struct PrintCapstoneModeToString_Arm64
{
	template <class ParamType>
	std::string operator()(const TestParamInfo<ParamType>& info) const
	{
		switch (info.param)
		{
			case CS_MODE_ARM: return "CS_MODE_ARM";
			case CS_MODE_MCLASS: return "CS_MODE_MCLASS";
			case CS_MODE_V8: return "CS_MODE_V8";
			default: return "UNHANDLED CS_MODE";
		}
	}
};

INSTANTIATE_TEST_SUITE_P(
		InstantiateArm64WithAllModes,
		Capstone2LlvmIrTranslatorArm64Tests,
		::testing::Values(CS_MODE_ARM),
		 PrintCapstoneModeToString_Arm64());

//
// ARM64_INS_ADC
//

//
// ARM64_INS_UBFX, UBFIZ, SBFX, SBFIZ, BFI, BFXIL -- the bitfield-move aliases
//
// These six were nullptr, so each became a pseudo-call. Capstone reports them
// rather than the UBFM/SBFM/BFM they alias, so these are the forms real code
// presents.
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UBFX)
{
	setRegisters({
		{ARM64_REG_X1, 0xabcdef12},
	});

	// bits [15:8] of 0xabcdef12 are 0xef, zero-extended.
	emulate("ubfx x0, x1, #8, #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UBFIZ)
{
	setRegisters({
		{ARM64_REG_X1, 0xabcdef12},
	});

	// low 8 bits (0x12), moved up to bit 8.
	emulate("ubfiz x0, x1, #8, #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1200},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SBFX_negative)
{
	setRegisters({
		{ARM64_REG_X1, 0xab80},
	});

	// bits [15:8] are 0xab: as a signed 8-bit field that is -85, so the
	// result is sign-extended across the whole 64-bit register. Taking the
	// same field with ubfx would give 0xab, which is what makes this the
	// case worth writing.
	emulate("sbfx x0, x1, #8, #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffab},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SBFIZ_negative)
{
	setRegisters({
		{ARM64_REG_X1, 0xf0},
	});

	// low 4 bits are 0x0; bits [7:4] are 0xf. Take the low 8 bits as signed
	// (-16) and place them at bit 4: 0xfffffffffffff00.
	emulate("sbfiz x0, x1, #4, #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffff00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BFI_preserves_the_rest)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x00},
	});

	// Insert the low 8 bits of X1 (zero) at bit 8 of X0, leaving every other
	// bit of X0 alone. A translation that forgot to read X0 would give 0.
	emulate("bfi x0, x1, #8, #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffff00ff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BFXIL_preserves_the_rest)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x0000},
	});

	// Take bits [15:8] of X1 (zero) into the low 8 bits of X0, leaving the
	// upper bits of X0 alone.
	emulate("bfxil x0, x1, #8, #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffff00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARMv8.3 pointer authentication and ARMv8.5 BTI
//
// Current toolchains emit these in almost every prologue and the table had no
// entry for any of them, so each became a pseudo-call. They model as nothing.
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BTI_is_nothing)
{
	// Keystone 0.9.2 predates ARMv8.5 and cannot assemble "bti c", so the
	// encoding goes in directly; capstone 5.0.9 disassembles these four bytes
	// as `bti c`.
	emulate_bin("5f 24 03 d5");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_PACIASP_AUTIASP_are_the_identity)
{
	// The pair is what a prologue and epilogue carry, and modelling each as
	// the identity is what keeps their composition right. The thing this
	// asserts is that neither touches a register -- in particular that
	// neither writes X30 with a value no source ever held.
	// paciasp; keystone cannot assemble it either.
	emulate_bin("3f 23 03 d5");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_AUTIASP_is_nothing)
{
	// autiasp.
	emulate_bin("bf 23 03 d5");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC_r_r_r_false)
{
	setRegisters({
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_X1, 0x1230},
		{ARM64_REG_X2, 0x4},
	});

	emulate("adc x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC_r_r_r_true)
{
	setRegisters({
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_X1, 0x1230},
		{ARM64_REG_X2, 0x4},
	});

	emulate("adc x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1235},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC_s_r_r_r_false)
{
	setRegisters({
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_X1, 0x1230},
		{ARM64_REG_X2, 0x4},
	});

	emulate("adcs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1234},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC32_r_r_r_true)
{
	setRegisters({
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_X1, 0x1230},
		{ARM64_REG_X2, 0x4},
	});

	emulate("adc w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1235},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC32_s_r_r_r_false)
{
	setRegisters({
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_X1, 0x1230},
		{ARM64_REG_X2, 0x4},
	});

	emulate("adcs w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1234},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC32_flags)
{
	setRegisters({
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_X1, 0xfffffffffffffffe},
		{ARM64_REG_X2, 0x1},
	});

	emulate("adcs w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC_flags)
{
	setRegisters({
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_X1, 0xfffffffffffffffe},
		{ARM64_REG_X2, 0x1},
	});

	emulate("adcs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC_flags1)
{
	setRegisters({
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_X1, 0xfffffffffffffffe},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("adcs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffffffffe},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADC_flags2)
{
	setRegisters({
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_X1, 0xfffffffffffffffe},
		{ARM64_REG_X2, 0x0},
	});

	emulate("adcs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_ADD
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x1230},
	});

	emulate("add x0, x1, #3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x1233},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_r_r_i_bin)
{
	setRegisters({
		{ARM64_REG_X1, 0x1230},
	});

	emulate_bin("20 0c 00 91");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x1233},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD32_r_r_i)
{
	setRegisters({
		{ARM64_REG_W1, 0x1230},
	});

	emulate("add w0, w1, #3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x1233},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD32_r_r_ishift)
{
	setRegisters({
		{ARM64_REG_X1, 0x1230},
	});

	// Valid shifts are: LSL #0 and LSL #12
	emulate("add x0, x1, #1, LSL #12");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x2230_qw},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD32_r_r_i_extend_test)
{
	// Value should be Zero extended into 64bit register
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
		{ARM64_REG_W1, 0xf0000000},
	});

	emulate("add w0, w1, #1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xf0000001},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Extended registers
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_r_r_w_UXTB)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
		{ARM64_REG_X2, 0x123456789abcdef0},
	});

	emulate("add x0, x1, w2, UXTB");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x10f0},});
	// 0x1000 + 0x00000000000000f0
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_r_r_w_UXTH)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
		{ARM64_REG_X2, 0x123456789abcdef0},
	});

	emulate("add x0, x1, w2, UXTH");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xeef0},});
	// 0x1000 + 0x000000000000def0
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_r_r_w_UXTW)
{
	// This means no extend just the optional shift, used in instruction aliases
	setRegisters({
		{ARM64_REG_X1, 0x1000000000000000},
		{ARM64_REG_X2, 0x123456789abcdef0},
	});

	emulate("add x0, x1, w2, UXTW");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x100000009abcdef0_qw},});
	// 0x1000000000000000 + 0x000000009abcdef0
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_r_r_w_SXTB)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff}, // -1
		{ARM64_REG_X2, 0x123456789abcdef0}, // -16
	});

	emulate("add x0, x1, w2, SXTB");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xffffffffffffffef},});
	// 0xffffffffffffffff + 0xfffffffffffffff0 = -17
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_r_r_w_SXTH)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff}, // -1
		{ARM64_REG_X2, 0x123456789abcfffb}, // -5
	});

	emulate("add x0, x1, w2, SXTH");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xfffffffffffffffa},});
	// 0xffffffffffffffff + 0xfffffffffffffffb = -6
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_r_r_w_SXTW)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff}, // -1
		{ARM64_REG_X2, 0x12345678fffffffb}, // -5
	});

	emulate("add x0, x1, w2, SXTW");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xfffffffffffffffa},});
	// 0xffffffffffffffff + 0xfffffffffffffffb = -6
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_w_w_w_UXTB)
{
	setRegisters({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0x1000000},
		{ARM64_REG_X2, 0x1234567800000123},
	});

	emulate("add w0, w1, w2, UXTB");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x1000023},});
	// 0x1000000 + 0x00000023
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_w_w_w_UXTH)
{
	setRegisters({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0x1000000},
		{ARM64_REG_X2, 0x1234567800000123},
	});

	emulate("add w0, w1, w2, UXTH");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x1000123},});
	// 0x1000000 + 0x00000123
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_w_w_w_UXTW)
{
	setRegisters({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0x1000000},
		{ARM64_REG_X2, 0x1234567812345678},
	});

	emulate("add w0, w1, w2, UXTW");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x13345678},});
	// 0x1000000 + 0x12345678
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_w_w_w_SXTB)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff}, // -1
		{ARM64_REG_X1, 0xffffffffffffffff}, // -1
		{ARM64_REG_X2, 0x123456789abcdef0}, // -16
	});

	emulate("add w0, w1, w2, SXTB");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x00000000ffffffef},});
	// 0x00000000ffffffff + 0x00000000fffffff0 = -17
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_w_w_w_SXTH)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff}, // -1
		{ARM64_REG_X1, 0xffffffffffffffff}, // -1
		{ARM64_REG_X2, 0x123456789abcfffb}, // -5
	});

	emulate("add w0, w1, w2, SXTH");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x00000000fffffffa},});
	// 0x00000000ffffffff + 0x00000000fffffffb = -6
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_w_w_w_SXTW)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff}, // -1
		{ARM64_REG_X1, 0xffffffffffffffff}, // -1
		{ARM64_REG_X2, 0x12345678fffffffb}, // -5
	});

	emulate("add w0, w1, w2, SXTW");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x00000000fffffffa},});
	// 0x00000000ffffffff + 0x00000000fffffffb = -6
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_s_zero_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0x0},
	});

	emulate("adds x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_s_negative_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffff000000000000},
		{ARM64_REG_X2, 0x1234},
	});

	emulate("adds x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffff000000001234},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_s_carry_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x1},
	});

	emulate("adds x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_s_overflow_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0fffffffffffffff},
		{ARM64_REG_X2, 0x7408089100000000},
	});

	emulate("adds x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x84080890ffffffff},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_ADR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADR)
{
	emulate("test:; adr x0, test", 0x40578);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x40578},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_ADRP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADRP)
{
	emulate("test:; adrp x0, test", 0x41578);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x82000},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_AND
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_AND_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567890abcdef},
	});

	emulate("and x0, x1, #0xf0");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000000000e0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_AND_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567890abcdef},
		{ARM64_REG_X2, 0xff00ff00ff00ff00},
	});

	emulate("and x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x120056009000cd00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_AND32_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567890abcdef},
	});

	emulate("and w0, w1, #0x0f");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x000000000000000f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_AND_s_zero_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x12345678},
		{ARM64_REG_X2, 0x0},
	});

	emulate("ands x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_AND32_s_negative_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567880abcdef},
		{ARM64_REG_X2, 0xf0000000},
	});

	emulate("ands w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x80000000},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_EOR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EOR_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x00000000ffffffff},
	});

	emulate("eor x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xffffffff00000000},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EOR_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("eor x0, x1, #3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xfffffffffffffffc},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EOR32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffff0000},
		{ARM64_REG_X2, 0xffffffff},
	});

	emulate("eor w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xffff},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_EON
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EON_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("eon x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x00000000ffffffff},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EON32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffff},
		{ARM64_REG_X2, 0x0000ffff},
	});

	emulate("eon w0, w1, w2, LSL #16");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xffff0000},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_ORR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ORR_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x00000000ffffffff},
	});

	emulate("orr x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xffffffffffffffff},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ORR_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("orr x0, x1, #3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xffffffffffffffff},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ORR32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffff0000},
		{ARM64_REG_X2, 0xffffffff},
	});

	emulate("orr w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xffffffff},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_ORN
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ORN_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("orn x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x00000000ffffffff},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ORN32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffff},
		{ARM64_REG_X2, 0x0000ffff},
	});

	emulate("orn w0, w1, w2, LSL #16");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xffffffff},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_EXTR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXTR_r_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x1111111111111111},
		{ARM64_REG_X2, 0x9999999999999999},
	});

	emulate("extr x0, x1, x2, #63");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2222222222222223},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXTR_r_r_r_i_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x1111111111111111},
		{ARM64_REG_X2, 0x9999999999999999},
	});

	emulate("extr x0, x1, x2, #48");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1111111111119999},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXTR_r_r_r_i_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x1111111111111111},
		{ARM64_REG_X2, 0x9999999999999999},
	});

	emulate("extr x0, x1, x2, #16");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1111999999999999},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXTR_r_r_r_i_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x1111111111111111},
		{ARM64_REG_X2, 0x9999999999999999},
	});

	emulate("extr x0, x1, x2, #10");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x4466666666666666},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZS_is_the_signed_convert)
{
	// Asserted on the IR, not on a value, and the reason is the point: the
	// emulator implements FPToUI and FPToSI with the SAME
	// APIntOps::RoundDoubleToAPInt (src/llvmir-emul/llvmir_emul.cpp:1167 and
	// :1215), so the two instructions are indistinguishable to it. No test
	// that runs through it can catch a translator emitting the wrong one of
	// the pair -- which is exactly how these two came to be transposed.
	auto* f = translate(assemble("fcvtzs x0, d1"));
	ASSERT_NE(nullptr, f);
	EXPECT_NE(std::string::npos, dumpFunction(f).find("fptosi")) << dumpFunction(f);
	EXPECT_EQ(std::string::npos, dumpFunction(f).find("fptoui")) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZU_is_the_unsigned_convert)
{
	auto* f = translate(assemble("fcvtzu x0, d1"));
	ASSERT_NE(nullptr, f);
	EXPECT_NE(std::string::npos, dumpFunction(f).find("fptoui")) << dumpFunction(f);
	EXPECT_EQ(std::string::npos, dumpFunction(f).find("fptosi")) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_EXT_UXTX_keeps_all_sixty_four_bits)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0x123456789abcdef0},
	});

	emulate("add x0, x1, x2, uxtx");

	// UXTX extends from the whole X register. The case was a copy of UXTW
	// and threw the top half away, answering 0x000000009abcdef0.
	EXPECT_EQ(0x123456789abcdef0ULL, getRegisterValueUnsigned(ARM64_REG_X0));
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_EXT_SXTX_keeps_all_sixty_four_bits)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0xfedcba9876543210},
	});

	emulate("add x0, x1, x2, sxtx");

	EXPECT_EQ(0xfedcba9876543210ULL, getRegisterValueUnsigned(ARM64_REG_X0));
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXTR_r_r_r_i_5)
{
	// 0x2222... rather than 0x1111...: with the old constant every set bit of
	// X1 was also set in X2, so the spurious `Xm | Xn` this instruction used
	// to compute was invisible and the test passed against a wrong answer.
	setRegisters({
		{ARM64_REG_X1, 0x2222222222222222},
		{ARM64_REG_X2, 0x9999999999999999},
	});

	emulate("extr x0, x1, x2, #0");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x9999999999999999},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXTR_r_r_r_i_6)
{
	setRegisters({
		{ARM64_REG_X2, 0x1234567890abcdef},
	});

	emulate("extr x0, x2, x2, #32");
	// alias ROR x0, x2, #32

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x90abcdef12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXTR32_r_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x1111111111111111},
		{ARM64_REG_X2, 0x9999999999999999},
	});

	emulate("extr w0, w1, w2, #31");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000022222223},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXTR32_r_r_r_i_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x1111111111111111},
		{ARM64_REG_X2, 0x9999999999999999},
	});

	emulate("extr w0, w1, w2, #16");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000011119999},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_ASR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ASR_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000000000000000},
		{ARM64_REG_X2, 0x20},
	});

	emulate("asr x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000010000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ASR_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
	});

	emulate("asr x0, x1, #63");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ASR32_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x0000000080000000},
	});

	emulate("asr w0, w1, #31");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CLZ
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CLZ_r_r)
{
	setRegisters({
		{ARM64_REG_X2, 0x0},
	});

	emulate("clz x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0x40},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CLZ_r_r_1)
{
	setRegisters({
		{ARM64_REG_X2, 0x1},
	});

	emulate("clz x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0x3f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CLZ_r_r_2)
{
	setRegisters({
		{ARM64_REG_X2, 0x100000000},
	});

	emulate("clz x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0x1f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CLZ32_r_r)
{
	setRegisters({
		{ARM64_REG_X2, 0x0},
	});

	emulate("clz w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0x20},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CLZ32_r_r_1)
{
	setRegisters({
		{ARM64_REG_X2, 0x10000000},
	});

	emulate("clz w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0x3},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CLZ32_r_r_2)
{
	setRegisters({
		{ARM64_REG_X2, 0x00000008},
	});

	emulate("clz w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0x1c},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CMN
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMN_zero_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0x0},
	});

	emulate("cmn x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMN_negative_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffff00000000},
		{ARM64_REG_X2, 0x12345678},
	});

	emulate("cmn x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMN_carry_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x1},
	});

	emulate("cmn x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMN_carry_overflow_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
		{ARM64_REG_X2, 0x8000000000000000},
	});

	emulate("cmn x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMN_overflow_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0fffffffffffffff},
		{ARM64_REG_X2, 0x7408089100000000},
	});

	emulate("cmn x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CCMP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_r_r_r_c)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmp x1, x2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_r_r_r_c_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmp x1, x2, #1, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_r_r_r_c_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmp x1, x2, #2, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_r_r_r_c_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmp x1, x2, #4, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_r_r_r_c_4)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmp x1, x2, #8, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_r_r_r_c_5)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmp x1, x2, #15, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_negative_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffff0000000fffff},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, true}
	});

	emulate("ccmp x1, x2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_carry_zero_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0x1},
		{ARM64_REG_CPSR_Z, true}
	});

	emulate("ccmp x1, x2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP_overflow_carry_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
		{ARM64_REG_X2, 0x7ffffffffffffffe},
		{ARM64_REG_CPSR_Z, true}
	});

	emulate("ccmp x1, x2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMP32_overflow_carry_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x80000000},
		{ARM64_REG_X2, 0x7ffffffe},
		{ARM64_REG_CPSR_Z, true}
	});

	emulate("ccmp w1, w2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CCMN
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_r_r_r_c)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmn x1, x2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_r_r_r_c_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmn x1, x2, #3, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_r_r_r_c_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmn x1, x2, #7, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_r_r_r_c_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmn x1, x2, #10, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_r_r_r_c_4)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmn x1, x2, #12, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_r_r_r_c_5)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_Z, false}
	});

	emulate("ccmn x1, x2, #14, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_negative_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xfffffffffffffffa},
		{ARM64_REG_X2, 0x2},
		{ARM64_REG_CPSR_Z, true}
	});

	emulate("ccmn x1, x2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_carry_negative_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_CPSR_Z, true}
	});

	emulate("ccmn x1, x2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN_overflow_carry_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
		{ARM64_REG_X2, 0x8000000000000000},
		{ARM64_REG_CPSR_Z, true}
	});

	emulate("ccmn x1, x2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CCMN32_overflow_carry_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x80000000},
		{ARM64_REG_X2, 0x80000000},
		{ARM64_REG_CPSR_Z, true}
	});

	emulate("ccmn w1, w2, #0, eq");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CMP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMP_zero_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1234},
	});

	emulate("cmp x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMP_negative_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffff0000000fffff},
		{ARM64_REG_X2, 0x1234},
	});

	emulate("cmp x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMP_carry_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0x1},
	});

	emulate("cmp x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMP_overflow_carry_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
		{ARM64_REG_X2, 0x7ffffffffffffffe},
	});

	emulate("cmp x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SUB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x1230},
	});

	emulate("sub x0, x1, #3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x122d},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB32_r_r_i)
{
	setRegisters({
		{ARM64_REG_W1, 0x1230},
	});

	emulate("sub w0, w1, #3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x122d},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB32_r_r_ishift)
{
	setRegisters({
		{ARM64_REG_X1, 0x1230},
	});

	// Valid shifts are: LSL #0 and LSL #12
	emulate("sub x0, x1, #1, LSL #12");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x0230_qw},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB32_r_r_i_extend_test)
{
	// Value should be Zero extended into 64bit register
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
		{ARM64_REG_W1, 0xf0000000},
	});

	emulate("sub w0, w1, #1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0xefffffff},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB_s_zero_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0x0},
	});

	emulate("subs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB_s_negative_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffff0000000fffff},
		{ARM64_REG_X2, 0x1234},
	});

	emulate("subs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffff0000000fedcb},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB_s_carry_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0x1},
	});

	emulate("subs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB_s_overflow_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0fffffffffffffff},
		{ARM64_REG_X2, 0x7408089100000000},
	});

	emulate("subs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x9bf7f76effffffff},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_NEG
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEG_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
	});

	emulate("neg x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffedcc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEG_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
	});

	emulate("neg x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEG_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("neg x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEG32_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
	});

	emulate("neg w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffedcc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEGS_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
	});

	emulate("negs x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEGS_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("negs x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEGS_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
	});

	emulate("negs x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffedcc},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEGS32_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0x1},
	});

	emulate("negs w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffffff},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEG_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.45678910_f64},
	});

	emulate("neg d0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-123.45678910)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_NGC
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NGC_r_r_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_CPSR_C, true},
	});

	emulate("ngc x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffedcc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NGC_r_r_false)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_CPSR_C, false},
	});

	emulate("ngc x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NGC32_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_CPSR_C, true},
	});

	emulate("ngc w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffedcc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NGCS_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_CPSR_C, false},
	});

	emulate("ngcs x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NGCS_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_CPSR_C, true},
	});

	emulate("ngcs x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NGCS_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_CPSR_C, true},
	});

	emulate("ngcs x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffedcc},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NGCS32_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_CPSR_C, true},
	});

	emulate("ngcs w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000fffffffe},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SBC
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SBC_r_r_r_false)
{
	setRegisters({
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x4},
	});

	emulate("sbc x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x122f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SBC_r_r_r_true)
{
	setRegisters({
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_X1, 0x1235},
		{ARM64_REG_X2, 0x4},
	});

	emulate("sbc x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1231},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SBC_s_r_r_r_false)
{
	setRegisters({
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x4},
	});

	emulate("sbcs x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x122f},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MOV
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOV_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xcafebabecafebabe},
	});

	emulate("mov x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOV32_r_r_extend_test)
{
	setRegisters({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_W1, 0xf0000000},
	});

	emulate("mov w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0xf0000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOV32_r_r)
{
	setRegisters({
		{ARM64_REG_W1, 0xcafebabe},
	});

	emulate("mov w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0xcafebabe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOV_d_v_0)
{
	setRegisters({
		{ARM64_REG_V1, 0x1234567890abcdef},
	});

	emulate("mov d0, v1.d[0]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_V1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 5.6263491089085159e-221_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOV_s_v_0)
{
	setRegisters({
		{ARM64_REG_V1, 0xffffffff12345678},
	});

	emulate("mov s0, v1.s[0]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_V1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 5.69045661e-28_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOV_s_v_1)
{
	setRegisters({
		{ARM64_REG_V1, 0x12345678ffffffff},
	});

	emulate("mov s0, v1.s[1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_V1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 5.69045661e-28_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MOVZ
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVZ_r_i)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("mov x0, #0xa");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xa},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MOVK
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVK_r_i)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("movk x0, #0x8f01");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xcafebabecafe8f01},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVK_r_i_16)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("movk x0, #0x8f01, LSL #16");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xcafebabe8f01babe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVK_r_i_32)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("movk x0, #0x8f01, LSL #32");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xcafe8f01cafebabe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVK_r_i_48)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("movk x0, #0x8f01, LSL #48");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8f01babecafebabe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVK_w_i)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
	});

	emulate("movk w0, #0x8f01");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xcafe8f01},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVK_w_i_16)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
	});

	emulate("movk w0, #0x8f01, LSL #16");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8f01babe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MOVN
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVN_r_i)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("movn x0, #0x8f01");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffff70fe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVN_r_i_16)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("movn x0, #0x8f01, LSL #16");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffff70feffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVN_r_i_32)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("movn x0, #0x8f01, LSL #32");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffff70feffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVN_r_i_48)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});

	emulate("movn x0, #0x8f01, LSL #48");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x70feffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVN_w_i)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
	});

	emulate("movn w0, #0x8f01");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffff70fe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVN_w_i_16)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
	});

	emulate("movn w0, #0x8f01, LSL #16");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000070feffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MVN
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MVN_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0123456789abcdef},
	});

	emulate("mvn x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfedcba9876543210},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MVN32_r_r)
{
	setRegisters({
		{ARM64_REG_W1, 0x89abcdef},
	});

	emulate("mvn w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0x76543210},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_NOP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NOP)
{
	emulate("nop");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_STR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STR_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("str x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0xcafebabecafebabe}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STR32_r_r)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("str w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0xcafebabe}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STR_d_r)
{
	setRegisters({
		{ARM64_REG_D0, 123.45678910_f64},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("str d0, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D0, ARM64_REG_SP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 123.45678910_f64}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STR_s_r)
{
	setRegisters({
		{ARM64_REG_S0, 24.122019_f32},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("str s0, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S0, ARM64_REG_SP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 24.122019_f32}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_STRB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STRB_r_r)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("strb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0xbe}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STRB_r_r_r)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x10},
	});

	emulate("strb w0, [x1, x2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1244, 0xbe}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_STRH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STRH_r_r)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("strh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0xbabe}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STRH_r_r_i)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("strh w0, [x1, #0x10]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1244, 0xbabe}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_STTR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STTR_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xcafebabecafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("sttr x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0xcafebabecafebabe}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_STTRB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STTRB_r_r)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("sttrb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0xbe}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_STTRH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STTRH_r_r)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("sttrh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0xbabe}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STTRH_r_r_i)
{
	setRegisters({
		{ARM64_REG_W0, 0xcafebabe},
		{ARM64_REG_X1, 0x1234},
	});

	emulate("sttrh w0, [x1, #0x10]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1244, 0xbabe}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_STP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STP_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0x0123456789abcdef},
		{ARM64_REG_X2, 0xfedcba9876543210},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("stp x0, x2, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X2, ARM64_REG_SP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0x0123456789abcdef_qw},
		{0x123c, 0xfedcba9876543210_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STP32_r_r_r)
{
	setRegisters({
		{ARM64_REG_W0, 0x01234567},
		{ARM64_REG_W2, 0xfedcba98},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("stp w0, w2, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_W2, ARM64_REG_SP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0x01234567_dw},
		{0x1238, 0xfedcba98_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STP_r_r_mw)
{
	setRegisters({
		{ARM64_REG_X0, 0x0123456789abcdef},
		{ARM64_REG_X2, 0xfedcba9876543210},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("stp x0, x2, [sp, #-0x20]!");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X2, ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_SP, 0x121c}
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1214, 0x0123456789abcdef_qw},
		{0x121c, 0xfedcba9876543210_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STP_r_r_m_i)
{
	setRegisters({
		{ARM64_REG_X0, 0x0123456789abcdef},
		{ARM64_REG_X2, 0xfedcba9876543210},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("stp x0, x2, [sp], #-0x20");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X2, ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_SP, 0x1214}
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0x0123456789abcdef_qw},
		{0x123c, 0xfedcba9876543210_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_STNP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STNP_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0x0123456789abcdef},
		{ARM64_REG_X2, 0xfedcba9876543210},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("stnp x0, x2, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X2, ARM64_REG_SP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0x0123456789abcdef_qw},
		{0x123c, 0xfedcba9876543210_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STNP32_r_r_r)
{
	setRegisters({
		{ARM64_REG_W0, 0x01234567},
		{ARM64_REG_W2, 0xfedcba98},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("stnp w0, w2, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W0, ARM64_REG_W2, ARM64_REG_SP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0x01234567_dw},
		{0x1238, 0xfedcba98_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_STNP_r_r_mw)
{
	setRegisters({
		{ARM64_REG_X0, 0x0123456789abcdef},
		{ARM64_REG_X2, 0xfedcba9876543210},
		{ARM64_REG_SP, 0x1234},
	});

	emulate("stnp x0, x2, [sp, #-0x20]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0, ARM64_REG_X2, ARM64_REG_SP});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1214, 0x0123456789abcdef_qw},
		{0x121c, 0xfedcba9876543210_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR32)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
		{ARM64_REG_X0, 0xcafebabecafebabe},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("ldr w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_plus_imm)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1008, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1, #8]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_minus_imm)
{
	setRegisters({
		{ARM64_REG_X1, 0x1010},
	});
	setMemory({
		{0x1008, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1, #-8]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_plus_reg)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
		{ARM64_REG_X2, 0x8},
	});
	setMemory({
		{0x1008, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1, x2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_minus_reg)
{
	setRegisters({
		{ARM64_REG_X1, 0x1010},
		{ARM64_REG_X2, -0x8},
	});
	setMemory({
		{0x1008, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1, x2]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_plus_imm_preindexed_writeback)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1008, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1, #8]!");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_minus_imm_preindexed_writeback)
{
	setRegisters({
		{ARM64_REG_X1, 0x1010},
	});
	setMemory({
		{0x1008, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1, #-8]!");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_plus_imm_postindexed_writeback)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1], #8");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0x1008},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_minus_imm_postindexed_writeback)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
	});

	emulate("ldr x0, [x1], #-8");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0xff8},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDR_label)
{
	// Load the memory at given label, or imm in this case
	setMemory({
		{0x15000, 0x123456789abcdef0_qw},
	});
	emulate("ldr x0, #0x15000");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0_qw},
	});
	EXPECT_JUST_MEMORY_LOADED({0x15000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDRB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDRB)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldrb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDRSB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDRSB)
{
	setRegisters({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x80_b},
	});

	emulate("ldrsb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffff80},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDRH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDRH)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x8182_w},
	});

	emulate("ldrh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8182},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDRSH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDRSH)
{
	setRegisters({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x8182_w},
	});

	emulate("ldrsh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffff8182},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDRSW
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDRSW)
{
	setRegisters({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x81828384_dw},
	});

	emulate("ldrsw x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffff81828384},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDTR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDTR)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
	});

	emulate("ldtr x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDTRB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDTRB)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldtrb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDTRSB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDTRSB)
{
	setRegisters({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x80_b},
	});

	emulate("ldtrsb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffff80},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDTRH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDTRH)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x8182_w},
	});

	emulate("ldtrh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8182},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDTRSH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDTRSH)
{
	setRegisters({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x8182_w},
	});

	emulate("ldtrsh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffff8182},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDTRSW
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDTRSW)
{
	setRegisters({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x81828384_dw},
	});

	emulate("ldtrsw x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffff81828384},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDXR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDXR)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
	});

	emulate("ldxr x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdxrLoadIsAtomic)
{
	auto* f = translate(assemble("ldxr x0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdxrLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldxr x0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdrLoadIsNotAtomic)
{
	auto* f = translate(assemble("ldr x0, [x1]"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			EXPECT_FALSE(l->isAtomic());
		}
	}
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, MemoryLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldr x0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, MemoryStoreAttachesPointeeMetadata)
{
	auto* f = translate(assemble("str w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StlxrStoreIsAtomic)
{
	auto* f = translate(assemble("stlxr w0, w1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StlxrAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stlxr w0, w1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StrStoreIsNotAtomic)
{
	auto* f = translate(assemble("str w0, [x1]"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* s = dyn_cast<StoreInst>(&*it))
		{
			EXPECT_FALSE(s->isAtomic());
		}
	}
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StxpStoreIsAtomic)
{
	auto* f = translate(assemble("stxp w0, w1, w2, [x3]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StxpAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stxp w0, w1, w2, [x3]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdxpLoadIsAtomic)
{
	auto* f = translate(assemble("ldxp x0, x1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdxpLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldxp x0, x1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdaxpLoadIsAtomic)
{
	auto* f = translate(assemble("ldaxp x0, x1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdaxpLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldaxp x0, x1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdaxrLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldaxr x0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdarLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldar x0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StxrAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stxr w0, w1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StlrAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stlr w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StlxpAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stlxp w0, w1, w2, [x3]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdaxrbLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldaxrb w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StxrbAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stxrb w0, w1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StlrbAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stlrb w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdxrbLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldxrb w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdxrhLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldxrh w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdaxrhLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldaxrh w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdarbLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldarb w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdarhLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldarh w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StxrhAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stxrh w0, w1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StlrhAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stlrh w0, [x1]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StlxrbAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stlxrb w0, w1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StlxrhAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stlxrh w0, w1, [x2]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, DmbEmitsFence)
{
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, DsbEmitsFence)
{
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, IsbEmitsFence)
{
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdpLoadIsNotAtomic)
{
	auto* f = translate(assemble("ldp x0, x1, [x2]"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			EXPECT_FALSE(l->isAtomic());
		}
	}
}

//
// ARM64_INS_LDXRB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDXRB)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldxrb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDXRH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDXRH)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x8182_w},
	});

	emulate("ldxrh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8182},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDAXR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDAXR)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
	});

	emulate("ldaxr x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDAXRB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDAXRB)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldaxrb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDAXRH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDAXRH)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x8182_w},
	});

	emulate("ldaxrh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8182},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDAR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDAR)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
	});

	emulate("ldar x0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDARB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDARB)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0xf1_b},
	});

	emulate("ldarb w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xf1},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDARH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDARH)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1000},
	});
	setMemory({
		{0x1000, 0x8182_w},
	});

	emulate("ldarh w0, [x1]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8182},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDP_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
		{0x1008, 0xfedcba9876543210_qw},
	});

	emulate("ldp x0, x1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0xfedcba9876543210},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDP32_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x9abcdef0_dw},
	});

	emulate("ldp w0, w1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0x12345678},
		{ARM64_REG_W1, 0x9abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDP_r_r_mw)
{
	setRegisters({
		{ARM64_REG_SP, 0x1020},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
		{0x1008, 0xfedcba9876543210_qw},
	});

	emulate("ldp x0, x1, [sp, #-32]!");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0xfedcba9876543210},
		{ARM64_REG_SP, 0x1000},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDP_r_r_r_i)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
		{0x1008, 0xfedcba9876543210_qw},
	});

	emulate("ldp x0, x1, [sp], #32");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0xfedcba9876543210},
		{ARM64_REG_SP, 0x1020},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDNP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDNP_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
		{0x1008, 0xfedcba9876543210_qw},
	});

	emulate("ldnp x0, x1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0xfedcba9876543210},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDNP32_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x9abcdef0_dw},
	});

	emulate("ldnp w0, w1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0x12345678},
		{ARM64_REG_W1, 0x9abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDNP_r_r_mw)
{
	setRegisters({
		{ARM64_REG_SP, 0x1020},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
		{0x1008, 0xfedcba9876543210_qw},
	});

	emulate("ldnp x0, x1, [sp, #-32]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0xfedcba9876543210},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDPSW
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDPSW_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x0},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0xfedcba98_dw},
	});

	emulate("ldpsw x0, x1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x12345678},
		{ARM64_REG_X1, 0xfffffffffedcba98},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDPSW1_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_X1, 0xffffffffffffffff},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0xfedcba98_dw},
	});

	emulate("ldpsw x1, x0, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffedcba98},
		{ARM64_REG_X1, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDPSW_r_r_r_i)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x0},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0xfedcba98_dw},
	});

	emulate("ldpsw x0, x1, [sp], #32");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x12345678},
		{ARM64_REG_X1, 0xfffffffffedcba98},
		{ARM64_REG_SP, 0x1020},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDXP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDXP_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
		{0x1008, 0xfedcba9876543210_qw},
	});

	emulate("ldxp x0, x1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0xfedcba9876543210},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDXP32_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x9abcdef0_dw},
	});

	emulate("ldxp w0, w1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0x12345678},
		{ARM64_REG_W1, 0x9abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LDAXP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDAXP_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
		{0x1008, 0xfedcba9876543210_qw},
	});

	emulate("ldaxp x0, x1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x123456789abcdef0},
		{ARM64_REG_X1, 0xfedcba9876543210},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1008});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDAXP32_r_r_r)
{
	setRegisters({
		{ARM64_REG_SP, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x9abcdef0_dw},
	});

	emulate("ldaxp w0, w1, [sp]");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0x12345678},
		{ARM64_REG_W1, 0x9abcdef0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LSL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LSL_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffff00000001},
		{ARM64_REG_X2, 0x20},
	});

	emulate("lsl x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000100000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LSL_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x0000000000000001},
	});

	emulate("lsl x0, x1, #63");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8000000000000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LSL32_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x0000000000000001},
		{ARM64_REG_X2, 31},
	});

	emulate("lsl w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000080000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LSR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LSR_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1000000000000000},
		{ARM64_REG_X2, 0x20},
	});

	emulate("lsr x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000010000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LSR_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
	});

	emulate("lsr x0, x1, #63");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000000000001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LSR32_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x0000000080000000},
	});

	emulate("lsr w0, w1, #31");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000000000001},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_B
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_B)
{
	emulate("b #0x110d8", 0x1107C);

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x110d8}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_B_cond_true)
{
	setRegisters({
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("b.ne #0x110d8", 0x1107C);

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x110d8}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_B_cond_false)
{
	setRegisters({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_V, false},
	});

	emulate("b.ge #0x110d8", 0x1107C);

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x110d8}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_B_cond_al)
{
	emulate("b.al #0x110d8", 0x1107C);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x110d8}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_B_cond_nv)
{
	emulate("b.nv #0x110d8", 0x1107C);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x110d8}},
	});
}

//
// ARM64_INS_BL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BL)
{
	emulate("bl #0x110d8", 0x1107C);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_LR, 0x11080},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x110d8}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BL_label)
{
	emulate("label_test:; bl label_test", 0x1000);

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_LR, 0x1004},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1000}},
	});
}

//
// ARM64_INS_BR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BR)
{
	setRegisters({
		{ARM64_REG_X1, 0xcafebabecafebabe},
	});

	emulate("br x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0xcafebabecafebabe}},
	});
}

//
// ARM64_INS_BLR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BLR)
{
	setRegisters({
		{ARM64_REG_X2, 0x123456789abcdef0},
	});

	emulate("blr x2", 0x2000);

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_LR, 0x2004},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x123456789abcdef0}},
	});
}

//
// ARM64_INS_BIC
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BIC_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567890abcdef},
		{ARM64_REG_X2, 0xff00ff00ff00ff00},
	});

	emulate("bic x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0034007800ab00ef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BIC_s_zero_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x12345678},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("bics x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BIC32_s_negative_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567880abcdef},
		{ARM64_REG_X2, 0x0fffffff},
	});

	emulate("bics w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x80000000},
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CBNZ
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CBNZ_true)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("cbnz x1, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CBNZ_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
	});

	emulate("cbnz x1, #0x1234");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CBNZ32_true)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("cbnz w1, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1000}},
	});
}

//
// ARM64_INS_CBZ
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CBZ_true)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("cbz x1, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CBZ_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
	});

	emulate("cbz x1, #0x1234");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CBZ32_true)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("cbz w1, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1000}},
	});
}

//
// ARM64_INS_CSEL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSEL_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x0000000000000001},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("csel x0, x1, x2, ne");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSEL_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x0000000000000001},
		{ARM64_REG_CPSR_V, false},
	});

	emulate("csel x0, x1, x2, vs");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSEL32_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x0000000000000001},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("csel w0, w1, w2, lt");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2,
				      ARM64_REG_CPSR_N, ARM64_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CSET
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSET_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("cset x0, hi");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSET_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("cset x0, ge");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSET32_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("cset w0, ge");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CSETM
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSETM_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("csetm x0, hi");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSETM_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("csetm x0, ge");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSETM32_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_CPSR_C, true},
	});

	emulate("csetm w0, hs");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CSINC
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSINC_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x1},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("csinc x0, x1, x2, hi");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z, ARM64_REG_CPSR_C, ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSINC_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1},
		{ARM64_REG_X2, 0x1234},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("csinc x0, x1, x2, ge");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V, ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1235},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CSINV
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSINV_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x1},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("csinv x0, x1, x2, hi");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z, ARM64_REG_CPSR_C, ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSINV_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x1},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("csinv x0, x1, x2, ge");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V, ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffffffffe},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CSNEG
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSNEG_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x1},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("csneg x0, x1, x2, hi");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z, ARM64_REG_CPSR_C, ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CSNEG_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0x5},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("csneg x0, x1, x2, ge");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V, ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffffffffb},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CINC
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CINC_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("cinc x0, x1, ls");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z, ARM64_REG_CPSR_C, ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CINC_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("cinc x0, x1, lt");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V, ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CINV
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CINV_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("cinv x0, x1, ls");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z, ARM64_REG_CPSR_C, ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CINV_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("cinv x0, x1, lt");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V, ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffedcb},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_CNEG
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CNEG_true)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("cneg x0, x1, ls");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_Z, ARM64_REG_CPSR_C, ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CNEG_false)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("cneg x0, x1, lt");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_CPSR_N, ARM64_REG_CPSR_V, ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MUL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MUL_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0x1},
	});

	emulate("mul x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MUL_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("mul x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MUL_r_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("mul x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MUL_r_r_r_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("mul x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffffffffe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MUL32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0x50},
	});

	emulate("mul w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xa0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MUL32_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("mul w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000fffffffe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MADD
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MADD_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0x1},
		{ARM64_REG_X3, 0x100},
	});

	emulate("madd x0, x1, x2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x104},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MADD_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_X3, 0x123},
	});

	emulate("madd x0, x1, x2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x124},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MADD_r_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_X3, 0xffffffffffffffff},
	});

	emulate("madd x0, x1, x2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MADD_r_r_r_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_X3, 0x2},
	});

	emulate("madd x0, x1, x2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MADD32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0x50},
		{ARM64_REG_X3, 0xffffffffffffffff},
	});

	emulate("madd w0, w1, w2, w3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x9f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MADD32_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_X3, 0x3},
	});

	emulate("madd w0, w1, w2, w3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_UMADDL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMADDL_r_w_w_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0x2},
		{ARM64_REG_X3, 0x100},
	});

	emulate("umaddl x0, w1, w2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2000000fe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMADDL_r_w_w_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0x2},
		{ARM64_REG_X3, 0x100000000},
	});

	emulate("umaddl x0, w1, w2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2fffffffe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SMADDL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMADDL_r_w_w_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0x2},
		{ARM64_REG_X3, 0xfffffffffffffffb},
	});

	emulate("smaddl x0, w1, w2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffffffff9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_UMSUBL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMSUBL_r_w_w_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0x2},
		{ARM64_REG_X3, 0x100},
	});

	emulate("umsubl x0, w1, w2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffe00000102},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMSUBL_r_w_w_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0x2},
		{ARM64_REG_X3, 0x11fffffffe},
	});

	emulate("umsubl x0, w1, w2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1000000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SMSUBL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMSUBL_r_w_w_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0x2},
		{ARM64_REG_X3, 0xfffffffffffffffb},
	});

	emulate("smsubl x0, w1, w2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffffffffd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_UMNEGL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMNEGL_r_w_w_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0x2},
	});

	emulate("umnegl x0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffe00000002},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SMNEGL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMNEGL_r_w_w_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x00000000ffffffff},
		{ARM64_REG_X2, 0x2},
	});

	emulate("smnegl x0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_UMULL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMULL_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0x1},
	});

	emulate("umull x0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMULL_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0xffffffff},
	});

	emulate("umull x0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x3fffffffc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SMULL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMULL_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0x1},
	});

	emulate("smull x0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMULL_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0xffffffff},
	});

	emulate("smull x0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffffffffc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_UMULH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMULH_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0x1},
		//{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("umulh x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SMULH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMULH_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0x1},
	});

	emulate("smulh x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMULH_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("smulh x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MNEG
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MNEG_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0x1},
	});

	emulate("mneg x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xfffffffffffffffc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MNEG_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("mneg x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MNEG_r_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("mneg x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MNEG_r_r_r_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("mneg x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MNEG32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0x50},
	});

	emulate("mneg w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffff60},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MNEG32_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0x1},
	});

	emulate("mneg w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000fffffffe},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MSUB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MSUB_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x4},
		{ARM64_REG_X2, 0x1},
		{ARM64_REG_X3, 0x3},
	});

	emulate("msub x0, x1, x2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MSUB_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_X3, 0x123},
	});

	emulate("msub x0, x1, x2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x122},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MSUB_r_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0x0},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_X3, 0xffffffffffffffff},
	});

	emulate("msub x0, x1, x2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MSUB_r_r_r_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_X3, 0xfffffffffffffffe},
	});

	emulate("msub x0, x1, x2, x3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MSUB32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0x50},
		{ARM64_REG_X3, 0xffffffffffffffff},
	});

	emulate("msub w0, w1, w2, w3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffff5f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MSUB32_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x2},
		{ARM64_REG_X2, 0xffffffffffffffff},
		{ARM64_REG_X3, 0x3},
	});

	emulate("msub w0, w1, w2, w3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x5},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SXTB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SXTB_r_r_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x80},
	});

	emulate("sxtb w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffff80},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SXTB_r_r_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x7f},
	});

	emulate("sxtb w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x000000000000007f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SXTH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SXTH_r_r_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000},
	});

	emulate("sxth w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffff8000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SXTH_r_r_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x7fff},
	});

	emulate("sxth w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000000007fff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SXTW
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SXTW_r_r_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x80000000},
	});

	emulate("sxtw x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffff80000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SXTW_r_r_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x7fffffff},
	});

	emulate("sxtw x0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x000000007fffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_UXTB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UXTB_r_r_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x80},
	});

	emulate("uxtb w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000000000080},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UXTB_r_r_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x7f},
	});

	emulate("uxtb w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x000000000000007f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_UXTH
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UXTH_r_r_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000},
	});

	emulate("uxth w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000000008000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UXTH_r_r_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x7fff},
	});

	emulate("uxth w0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000000007fff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_TBNZ
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TBNZ_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x000000000000000f},
	});

	emulate("tbnz x1, #0, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TBNZ_false)
{
	setRegisters({
		{ARM64_REG_X1, 0xfffffffffffffff0},
	});

	emulate("tbnz x1, #0, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TBNZ_63_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
	});

	emulate("tbnz x1, #63, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TBNZ_32_true)
{
	setRegisters({
		{ARM64_REG_X1, 0x100000000},
	});

	emulate("tbnz x1, #32, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1000}},
	});
}

//
// ARM64_INS_TBZ
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TBZ_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x000000000000000f},
	});

	emulate("tbz x1, #0, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TBZ_true)
{
	setRegisters({
		{ARM64_REG_X1, 0xfffffffffffffff0},
	});

	emulate("tbz x1, #0, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TBZ_63_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
	});

	emulate("tbz x1, #63, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1000}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TBZ_32_false)
{
	setRegisters({
		{ARM64_REG_X1, 0x100000000},
	});

	emulate("tbz x1, #32, #0x1000");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1000}},
	});
}

//
// ARM64_INS_RET
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_RET)
{
	setRegisters({
		{ARM64_REG_LR, 0xcafebabe},
	});

	emulate("ret");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_LR});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0xcafebabe}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_RET_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xcafebabe},
	});

	emulate("ret x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getReturnFunction(), {0xcafebabe}},
	});
}

//
// ARM64_INS_ROR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ROR_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x0000000000000001},
		{ARM64_REG_X2, 63},
	});

	emulate("ror x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000000000002},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ROR_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffff00000000},
	});

	emulate("ror x0, x1, #32");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000ffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ROR32_r_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffff00001234},
		{ARM64_REG_X2, 16},
	});

	emulate("ror w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000012340000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SDIV
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SDIV_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1230},
		{ARM64_REG_X2, 0x1230},
	});

	emulate("sdiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x1},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SDIV_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("sdiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffedcc},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SDIV_r_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffedcc},
		{ARM64_REG_X2, 0xffffffffffffedcc},
	});

	emulate("sdiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SDIV_r_r_r_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x5},
		{ARM64_REG_X2, 0x2},
	});

	emulate("sdiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SDIV_r_r_r_4)
{
	setRegisters({
		{ARM64_REG_X1, 0xfffffffffffffffc},
		{ARM64_REG_X2, 0xfffffffffffffffe},
	});

	emulate("sdiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SDIV32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0xa},
		{ARM64_REG_X2, 0x00000000fffffffe},
	});

	emulate("sdiv w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x00000000fffffffb},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UDIV_r_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1230},
		{ARM64_REG_X2, 0x1230},
	});

	emulate("udiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({{ARM64_REG_X0, 0x1},});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UDIV_r_r_r_1)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234},
		{ARM64_REG_X2, 0xffffffffffffffff},
	});

	emulate("udiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UDIV_r_r_r_2)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffedcc},
		{ARM64_REG_X2, 0xffffffffffffedcc},
	});

	emulate("udiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UDIV_r_r_r_3)
{
	setRegisters({
		{ARM64_REG_X1, 0x5},
		{ARM64_REG_X2, 0x2},
	});

	emulate("udiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UDIV_r_r_r_4)
{
	setRegisters({
		{ARM64_REG_X1, 0xfffffffffffffffe},
		{ARM64_REG_X2, 0xfffffffffffffffc},
	});

	emulate("udiv x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UDIV32_r_r_r)
{
	setRegisters({
		{ARM64_REG_X0, 0xffffffffffffffff},
		{ARM64_REG_X1, 0x00000000fffffffe},
		{ARM64_REG_X2, 0xa},
	});

	emulate("udiv w0, w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x0000000019999999},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_TST
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TST_zero_r_i)
{
	setRegisters({
		{ARM64_REG_X1, 0x12345678},
	});

	emulate("tst x1, #1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TST_zero_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x12345678},
		{ARM64_REG_X2, 0x0},
	});

	emulate("tst x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TST_minus_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
		{ARM64_REG_X2, 0x8000000000000000},
	});

	emulate("tst x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TST32_zero_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567880abcdef},
		{ARM64_REG_X2, 0x00000000},
	});

	emulate("tst w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TST32_negative_r_r)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567880abcdef},
		{ARM64_REG_X2, 0xf0000000},
	});

	emulate("tst w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_V, false},
		{ARM64_REG_CPSR_C, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_RBIT
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_RBIT_r_r)
{
	setRegisters({
		{ARM64_REG_X2, 0x1234567890abcdef},
	});

	emulate("rbit x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.bitreverse.i64"), {0x1234567890abcdef}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_RBIT32_r_r)
{
	setRegisters({
		{ARM64_REG_X2, 0x1234567890abcdef},
	});

	emulate("rbit w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.bitreverse.i32"), {0x90abcdef}},
	});
}

//
// ARM64_INS_REV
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_REV_r_r)
{
	setRegisters({
		{ARM64_REG_X2, 0x1234567890abcdef},
	});

	emulate("rev x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0xefcdab9078563412},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_REV32_r_r)
{
	setRegisters({
		{ARM64_REG_X2, 0x1234567890abcdef},
	});

	emulate("rev w1, w2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0x00000000efcdab90},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FABS
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FABS_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 40.42_f32},
	});

	emulate("fabs s0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f32"), {40.42_f32}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FABS_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 42.40_f64},
	});

	emulate("fabs d0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f64"), {42.40_f64}},
	});
}

//
// ARM64_INS_FADD
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FADD_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, 3.141592_f32},
	});

	emulate("fadd s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 103.641594_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FADD_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, 3.141592_f64},
	});

	emulate("fadd d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 3.2831841415921419_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FADD_s_s_s_neg)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, static_cast<float>(-3.141592)},
	});

	emulate("fadd s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 97.3584061_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FADD_d_d_d_neg)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, static_cast<double>(-3.141592)},
	});

	emulate("fadd d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-2.9999998584078584)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FCMP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCMP_s_s_eq)
{
	setRegisters({
		{ARM64_REG_S1, 321.321_f32},
		{ARM64_REG_S2, 321.321_f32},
	});

	emulate("fcmp s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCMP_s_s_gt)
{
	setRegisters({
		{ARM64_REG_S1, 321.321_f32},
		{ARM64_REG_S2, 123.456_f32},
	});

	emulate("fcmp s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCMP_s_s_lt)
{
	setRegisters({
		{ARM64_REG_S1, 123.456_f32},
		{ARM64_REG_S2, 321.321_f32},
	});

	emulate("fcmp s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCMP_d_d_eq)
{
	setRegisters({
		{ARM64_REG_D1, 321.3938216392863_f64},
		{ARM64_REG_D2, 321.3938216392863_f64},
	});

	emulate("fcmp d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCMP_d_d_gt)
{
	setRegisters({
		{ARM64_REG_D1, 321.3938216392863_f64},
		{ARM64_REG_D2, 123.45632918321_f64},
	});

	emulate("fcmp d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCMP_d_d_lt)
{
	setRegisters({
		{ARM64_REG_D1, 123.45632918321_f64},
		{ARM64_REG_D2, 321.3938216392863_f64},
	});

	emulate("fcmp d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FCCMP
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCCMP_s_s_f_false)
{
	setRegisters({
		{ARM64_REG_S1, 321.321_f32},
		{ARM64_REG_S2, 321.321_f32},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("fccmp s1, s2, #12, ne");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCCMP_s_s_f_true)
{
	setRegisters({
		{ARM64_REG_S1, 321.321_f32},
		{ARM64_REG_S2, 123.456_f32},
		{ARM64_REG_CPSR_C, true},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("fccmp s1, s2, #12, hi");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2, ARM64_REG_CPSR_C, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCCMP_d_d_f_true)
{
	setRegisters({
		{ARM64_REG_D1, 321.3938216392863_f64},
		{ARM64_REG_D2, 321.3938216392863_f64},
		{ARM64_REG_CPSR_C, true},
	});

	emulate("fccmp d1, d2, #5, cc");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_Z, true},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCCMP_d_d_f_false)
{
	setRegisters({
		{ARM64_REG_D1, 321.3938216392863_f64},
		{ARM64_REG_D2, 123.45632918321_f64},
		{ARM64_REG_CPSR_C, false},
	});

	emulate("fccmp d1, d2, #1, lo");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2, ARM64_REG_CPSR_C});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_CPSR_N, true},
		{ARM64_REG_CPSR_Z, false},
		{ARM64_REG_CPSR_C, false},
		{ARM64_REG_CPSR_V, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FCSEL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCSEL_true)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
		{ARM64_REG_D2, 12.34567_f64},
		{ARM64_REG_CPSR_Z, false},
	});

	emulate("fcsel d0, d1, d2, ne");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2, ARM64_REG_CPSR_Z});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 3.141592_f64},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCSEL_false)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
		{ARM64_REG_D2, 12.34567_f64},
		{ARM64_REG_CPSR_V, false},
	});

	emulate("fcsel d0, d1, d2, vs");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2, ARM64_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 12.34567_f64},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCSEL32_true)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f64},
		{ARM64_REG_S2, 12.34567_f64},
		{ARM64_REG_CPSR_N, false},
		{ARM64_REG_CPSR_V, true},
	});

	emulate("fcsel s0, s1, s2, lt");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2,
				      ARM64_REG_CPSR_N, ARM64_REG_CPSR_V});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 3.141592_f64},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FCVT
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVT_s_d)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
	});

	emulate("fcvt s0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 3.141592_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVT_d_s)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
	});

	emulate("fcvt d0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 3.141592_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_SCVTF
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SCVTF_s_w)
{
	setRegisters({
		{ARM64_REG_W1, 0xffffffff},
	});

	emulate("scvtf s0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-1)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SCVTF_d_w)
{
	setRegisters({
		{ARM64_REG_W1, 123},
	});

	emulate("scvtf d0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 123.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SCVTF_s_x)
{
	setRegisters({
		{ARM64_REG_X1, 0xffffffffffffffff},
	});

	emulate("scvtf s0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-1)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SCVTF_d_x)
{
	setRegisters({
		{ARM64_REG_X1, 123},
	});

	emulate("scvtf d0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 123.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_UCVTF
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UCVTF_s_w)
{
	setRegisters({
		{ARM64_REG_W1, 0xffffffff},
	});

	emulate("ucvtf s0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 4294967295.0_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UCVTF_d_w)
{
	setRegisters({
		{ARM64_REG_W1, 123},
	});

	emulate("ucvtf d0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 123.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UCVTF_s_x)
{
	setRegisters({
		{ARM64_REG_X1, 0x8000000000000000},
	});

	emulate("ucvtf s0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 9.22337204e+18_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UCVTF_d_x)
{
	setRegisters({
		{ARM64_REG_X1, 123},
	});

	emulate("ucvtf d0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 123.0_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FCVTZS
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZS_w_s)
{
	setRegisters({
		{ARM64_REG_S1, static_cast<float>(-1.0)},
	});

	emulate("fcvtzs w0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0xffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZS_w_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.9_f64},
	});

	emulate("fcvtzs w0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 123},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZS_x_s)
{
	setRegisters({
		{ARM64_REG_S1, static_cast<float>(-1)},
	});

	emulate("fcvtzs x0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZS_x_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.3_f64},
	});

	emulate("fcvtzs x0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 123},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FCVTZU
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZU_w_s)
{
	setRegisters({
		{ARM64_REG_S1, 31232321.0_f32},
	});

	emulate("fcvtzu w0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0x1dc9140},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZU_w_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.5_f64},
	});

	emulate("fcvtzu w0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 123},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZU_x_s)
{
	setRegisters({
		{ARM64_REG_S1, 9.22337204e+18_f32},
	});

	emulate("fcvtzu x0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x8000000000000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FCVTZU_x_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.0_f64},
	});

	emulate("fcvtzu x0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 123},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FDIV
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FDIV_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, 3.141592_f32},
	});

	emulate("fdiv s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 31.9901524_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FDIV_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, 3.141592_f64},
	});

	emulate("fdiv d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 0.045070187851300098_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FDIV_s_s_s_neg)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, static_cast<float>(-3.141592)},
	});

	emulate("fdiv s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-31.9901524)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FDIV_d_d_d_neg)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, static_cast<double>(-3.141592)},
	});

	emulate("fdiv d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-0.045070187851300098)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FMADD
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMADD_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 60.58365_f32},
		{ARM64_REG_S2, static_cast<float>(-0.320193)},
		{ARM64_REG_S3, 100.2383073_f32},
	});

	emulate("fmadd s0, s1, s2, s3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2, ARM64_REG_S3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 80.8398438_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMADD_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.62345_f64},
		{ARM64_REG_D2, static_cast<double>(-563.24683)},
		{ARM64_REG_D3, 863.246983963_f64},
	});

	emulate("fmadd d0, d1, d2, d3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2, ARM64_REG_D3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-68767.26934220051)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FNMADD
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNMADD_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 60.58365_f32},
		{ARM64_REG_S2, static_cast<float>(-0.320193)},
		{ARM64_REG_S3, 100.2383073_f32},
	});

	emulate("fnmadd s0, s1, s2, s3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2, ARM64_REG_S3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-80.8398438)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNMADD_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.62345_f64},
		{ARM64_REG_D2, static_cast<double>(-563.24683)},
		{ARM64_REG_D3, 863.246983963_f64},
	});

	emulate("fnmadd d0, d1, d2, d3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2, ARM64_REG_D3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 68767.26934220051_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FMAX
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMAX_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
		{ARM64_REG_D2, 12.34567_f64},
	});

	emulate("fmax d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 12.34567_f64},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMAX_d_d_d_1)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
		{ARM64_REG_D2, static_cast<double>(-12.34567)},
	});

	emulate("fmax d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 3.141592_f64},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMAX_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
		{ARM64_REG_S2, 12.34567_f32},
	});

	emulate("fmax s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 12.34567_f32},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMAX_s_s_s_1)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
		{ARM64_REG_S2, static_cast<float>(-12.34567)},
	});

	emulate("fmax s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 3.141592_f32},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FMAXNM
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMAXNM_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
		{ARM64_REG_D2, 12.34567_f64},
	});

	emulate("fmaxnm d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, ANY},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.maxnum.f64"), {3.141592_f64, 12.34567_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMAXNM_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
		{ARM64_REG_S2, 12.34567_f32},
	});

	emulate("fmaxnm s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, ANY},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.maxnum.f32"), {3.141592_f32, 12.34567_f32}},
	});
}

//
// ARM64_INS_FMIN
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMIN_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
		{ARM64_REG_D2, 12.34567_f64},
	});

	emulate("fmin d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 3.141592_f64},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMIN_d_d_d_1)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
		{ARM64_REG_D2, static_cast<double>(-12.34567)},
	});

	emulate("fmin d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-12.34567)},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMIN_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
		{ARM64_REG_S2, 12.34567_f32},
	});

	emulate("fmin s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 3.141592_f32},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMIN_s_s_s_1)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
		{ARM64_REG_S2, static_cast<float>(-12.34567)},
	});

	emulate("fmin s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-12.34567)},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FMINNM
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMINNM_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
		{ARM64_REG_D2, 12.34567_f64},
	});

	emulate("fminnm d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, ANY},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.minnum.f64"), {3.141592_f64, 12.34567_f64}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMINNM_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
		{ARM64_REG_S2, 12.34567_f32},
	});

	emulate("fminnm s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, ANY},
	    });
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.minnum.f32"), {3.141592_f32, 12.34567_f32}},
	});
}

//
// ARM64_INS_FMOV
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMOV_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
	});

	emulate("fmov s0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 3.141592_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMOV_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 3.141592_f64},
	});

	emulate("fmov d0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 3.141592_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMOV_s_w)
{
	setRegisters({
		{ARM64_REG_W1, 0x12345678},
	});

	emulate("fmov s0, w1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 5.69045661e-28_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMOV_w_s)
{
	setRegisters({
		{ARM64_REG_S1, 5.69045661e-28_f32},
	});

	emulate("fmov w0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W0, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMOV_x_d)
{
	setRegisters({
		{ARM64_REG_X1, 0x1234567890abcdef},
	});

	emulate("fmov d0, x1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 5.6263491089085159e-221_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMOV_d_x)
{
	setRegisters({
		{ARM64_REG_D1, 5.6263491089085159e-221_f64},
	});

	emulate("fmov x0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1234567890abcdef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMOV_d_i)
{
	emulate("fmov d0, #1.");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 1._f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMOV_s_i)
{
	emulate("fmov s0, #1.");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 1._f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_MOVI
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVI_d_i)
{
	emulate("movi d0, #0");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 0._f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MOVI_v_i)
{
	emulate("movi v15.4h, #0xcf");

	// 0xcf replicated into four halfword lanes. This used to be a
	// pseudo-assembly call, and the test asserted that -- which is why the
	// test had to change when the instruction started being translated.
	EXPECT_EQ(0x00cf00cf00cf00cfULL, vLow(ARM64_REG_V15));
	// A 64-bit arrangement clears the upper half of the register.
	EXPECT_EQ(0x0ULL, vHigh(ARM64_REG_V15));
	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FMUL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMUL_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, 3.141592_f32},
	});

	emulate("fmul s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 315.72998_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMUL_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, 3.141592_f64},
	});

	emulate("fmul d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 0.44482473928873928_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMUL_s_s_s_neg)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, static_cast<float>(-3.141592)},
	});

	emulate("fmul s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-315.72998)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMUL_d_d_d_neg)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, static_cast<double>(-3.141592)},
	});

	emulate("fmul d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-0.44482473928873928)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FNEG
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNEG_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 3.141592_f32},
	});

	emulate("fneg s0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-3.141592)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNEG_s_s_1)
{
	setRegisters({
		{ARM64_REG_S1, static_cast<float>(-3.141592)},
	});

	emulate("fneg s0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 3.141592_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FMSUB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMSUB_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 60.58365_f32},
		{ARM64_REG_S2, static_cast<float>(-0.320193)},
		{ARM64_REG_S3, 100.2383073_f32},
	});

	emulate("fmsub s0, s1, s2, s3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2, ARM64_REG_S3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 119.636765_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FMSUB_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.62345_f64},
		{ARM64_REG_D2, static_cast<double>(-563.24683)},
		{ARM64_REG_D3, 863.246983963_f64},
	});

	emulate("fmsub d0, d1, d2, d3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2, ARM64_REG_D3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 70493.763310126509_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FNMSUB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNMSUB_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 60.58365_f32},
		{ARM64_REG_S2, static_cast<float>(-0.320193)},
		{ARM64_REG_S3, 100.2383073_f32},
	});

	emulate("fnmsub s0, s1, s2, s3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2, ARM64_REG_S3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-119.636765)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNMSUB_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 123.62345_f64},
		{ARM64_REG_D2, static_cast<double>(-563.24683)},
		{ARM64_REG_D3, 863.246983963_f64},
	});

	emulate("fnmsub d0, d1, d2, d3");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2, ARM64_REG_D3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-70493.763310126509)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FNMUL
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNMUL_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, 3.141592_f32},
	});

	emulate("fnmul s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, static_cast<float>(-315.72998)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNMUL_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, 3.141592_f64},
	});

	emulate("fnmul d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-0.44482473928873928)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNMUL_s_s_s_neg)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, static_cast<float>(-3.141592)},
	});

	emulate("fnmul s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 315.72998_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FNMUL_d_d_d_neg)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, static_cast<double>(-3.141592)},
	});

	emulate("fnmul d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 0.44482473928873928_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_FSUB
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FSUB_s_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, 3.141592_f32},
	});

	emulate("fsub s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 97.3584061_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FSUB_d_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, 3.141592_f64},
	});

	emulate("fsub d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, static_cast<double>(-2.9999998584078584)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FSUB_s_s_s_neg)
{
	setRegisters({
		{ARM64_REG_S1, 100.5_f32},
		{ARM64_REG_S2, static_cast<float>(-3.141592)},
	});

	emulate("fsub s0, s1, s2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1, ARM64_REG_S2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, 103.641594_f32},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FSUB_d_d_d_neg)
{
	setRegisters({
		{ARM64_REG_D1, 0.141592141592141592141592_f64},
		{ARM64_REG_D2, static_cast<double>(-3.141592)},
	});

	emulate("fsub d0, d1, d2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1, ARM64_REG_D2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, 3.2831841415921419_f64},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

/*
//
// ARM64_INS_FSQRT
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FSQRT_s_s)
{
	setRegisters({
		{ARM64_REG_S1, 40.32_f32},
	});

	emulate("fsqrt s0, s1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_S1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_S0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	//EXPECT_NO_VALUE_CALLED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.fsqrt.f32"), {40.32}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_FSQRT_d_d)
{
	setRegisters({
		{ARM64_REG_D1, 32.40_f64},
	});

	emulate("fsqrt d0, d1");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_D1});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_D0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	//EXPECT_NO_VALUE_CALLED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.fsqrt.f64"), {32.40_f64}},
	});
}
*/

// Regression: ARM64 capstone2llvmir semantics for specific instruction forms.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, issue_998)
{
	setRegisters({
		{ARM64_REG_X0, 0x1234},
	});

	emulate("at s1e1r, x0");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X0});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_at"), {0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, LdpLoadAttachesPointeeMetadata)
{
	auto* f = translate(assemble("ldp x0, x1, [sp]"));
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

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, StpStoreAttachesPointeeMetadata)
{
	auto* f = translate(assemble("stp x0, x2, [sp]"));
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
// Vector-arrangement forms of the integer arithmetic instructions.
//
// `add v0.4s, v1.4s, v2.4s` is four 32-bit adds. The translator has no vector
// model, and it used to reach CreateAdd anyway: extractVectorValue truncates a
// .4s operand to i32 and bitcasts it to float, and isFPRegister() recognises
// Q, D, H and S registers but not V, so the bitcast back to an integer never
// fired. With assertions on, that is "Tried to create an integer operation on
// a non-integer type" and a core dump -- how all ten ARM64 binaries in ARCH-01
// died. With assertions off it is invalid IR that nothing notices.
//
// These read the IR rather than relying on an assertion, so they fail in
// either kind of build.
//

static bool hasIntegerOpcodeOnFpType(llvm::Function* f)
{
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		auto* bo = dyn_cast<BinaryOperator>(&*it);
		if (bo == nullptr || !bo->getType()->isFPOrFPVectorTy())
		{
			continue;
		}
		switch (bo->getOpcode())
		{
		case Instruction::Add:
		case Instruction::Sub:
		case Instruction::Mul:
		case Instruction::And:
		case Instruction::Or:
		case Instruction::Xor: return true;
		default: break;
		}
	}
	return false;
}

// These three asserted that a vector add or subtract came out as
// __asm_add/__asm_sub, which was true and was the defect: translateAdd() and
// translateSub() opened with `if (ifVectorGeneratePseudo(...)) return;`, so a
// vector operand meant "give up". 588 occurrences of __asm_add in the static
// parity corpus, every one of them scored covered by COV-01 because the
// dispatch table has a function pointer for ADD.
//
// The guard was right when it was written -- the alternative then was
// CreateAdd on a 128-bit register, which is not a lane-wise add of anything
// and dumped core on `.4s` operands -- and it is the wrong answer now that the
// lane model exists. What it protected against is still checked:
// hasIntegerOpcodeOnFpType() must stay false.
//
// The lane values are chosen so that a 128-bit add and a lane-wise one differ:
// every lane carries out of its top, so a register-wide add propagates each
// carry into the next lane and a lane-wise one does not.

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_vector_4s)
{
	setV(ARM64_REG_V1, 0xffffffffffffffffULL, 0xffffffffffffffffULL);
	setV(ARM64_REG_V2, 0x0000000100000001ULL, 0x0000000100000001ULL);

	emulate("add v0.4s, v1.4s, v2.4s");

	// Each word wraps to zero on its own. A 128-bit add answers
	// 0x0000000100000000 / 0x0000000100000001 instead.
	EXPECT_EQ(0x0000000000000000ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000000000000ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_vector_2d)
{
	setV(ARM64_REG_V4, /*hi=*/5, /*lo=*/0xffffffffffffffffULL);
	setV(ARM64_REG_V5, /*hi=*/7, /*lo=*/1);

	emulate("add v3.2d, v4.2d, v5.2d");

	// The low lane wraps to zero on its own. A 128-bit add would carry into
	// the high lane and answer 13 there.
	EXPECT_EQ(0x0000000000000000ULL, vLow(ARM64_REG_V3));
	EXPECT_EQ(0x000000000000000cULL, vHigh(ARM64_REG_V3));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SUB_vector_2d)
{
	setV(ARM64_REG_V1, /*hi=*/0xa, /*lo=*/0);
	setV(ARM64_REG_V2, /*hi=*/3, /*lo=*/1);

	emulate("sub v0.2d, v1.2d, v2.2d");

	// The low lane borrows. A 128-bit subtract would take that borrow out of
	// the high lane and answer 9 there.
	EXPECT_EQ(0xffffffffffffffffULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000000000007ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// The guard the three tests above used to assert is still needed for the
// forms the lane model does not cover, and this pins it: an integer opcode
// must never be created on a floating-point type.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_vector_ops_never_type_confuse)
{
	for (auto& a:
		 {std::string("add v0.4s, v1.4s, v2.4s"),
		  std::string("sub v0.2d, v1.2d, v2.2d"),
		  std::string("mul v0.8h, v1.8h, v2.8h")})
	{
		auto* f = translate(assemble(a));
		ASSERT_NE(nullptr, f) << a;
		EXPECT_FALSE(hasIntegerOpcodeOnFpType(f)) << a;
	}
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADD_scalar_is_still_a_real_add)
{
	// The guard must not swallow the scalar form: these are X registers, not
	// V registers, and this is an ordinary 64-bit add.
	setRegisters({
		{ARM64_REG_X1, 0x1200},
		{ARM64_REG_X2, 0x34},
	});

	emulate("add x0, x1, x2");

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_X1, ARM64_REG_X2});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, WholeVectorOperandIsNotALaneExtraction)
{
	// A whole-register operand carries an arrangement and no index, so
	// vector_index is -1. Every lane branch in extractVectorValue multiplies
	// it by a lane width and shifts by the result, which for -1 is a shift by
	// 2^128-32: poison, then truncated to a lane type. There must be no shift
	// by a constant that large.
	auto* f = translate(assemble("eor v0.16b, v1.16b, v2.16b"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		auto* bo = dyn_cast<BinaryOperator>(&*it);
		if (bo == nullptr || bo->getOpcode() != Instruction::LShr)
		{
			continue;
		}
		if (auto* c = dyn_cast<ConstantInt>(bo->getOperand(1)))
		{
			EXPECT_LT(c->getValue().getActiveBits(), 64u) << "shift amount came from a negative vector_index";
		}
	}
}

//
// ARMv8.1 LSE atomics.
//
// 120 instruction ids with no entry in the dispatch table at all -- not even
// nullptr -- so no translator could have reached them. A compiler targeting
// armv8.1-a or later emits these for every atomic operation. x86 has had the
// equivalent (LOCK XADD, CMPXCHG) as atomicrmw and cmpxchg all along.
//
// Keystone 0.9.2 predates LSE and refuses every one of them
// (KS_ERR_ASM_INVALIDOPERAND), so these go in as encodings produced by
// aarch64-linux-gnu-as and checked against capstone 5.0.9.
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDADD)
{
	setRegisters({
		{ARM64_REG_W1, 0x10},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0x22_dw},
	});

	emulate_bin("62 00 21 b8"); // ldadd w1, w2, [x3]

	EXPECT_JUST_REGISTERS_LOADED({ARM64_REG_W1, ARM64_REG_X3});
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W2, 0x22}, // the destination gets the OLD value
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x32_dw}, // and memory gets old + operand
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDADDAL_is_64_bit)
{
	setRegisters({
		{ARM64_REG_X1, 0x10},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0x123456789abcdef0_qw},
	});

	emulate_bin("62 00 e1 f8"); // ldaddal x1, x2, [x3]

	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X2, 0x123456789abcdef0},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x123456789abcdf00_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDADDB_touches_one_byte)
{
	// The width comes from the mnemonic suffix, not the register: this names
	// W registers and must touch exactly one byte.
	//
	// Checked in the IR, not by emulating. The interpreter's memory is a map
	// from address to value, not a byte array, so a one-byte and a four-byte
	// access at the same address are indistinguishable to it -- an emulated
	// version of this test passed with the width forced to 32 bits, and
	// finding that out is the only reason it is written this way.
	auto* f = translate(utils::hexStringToBytes("62 00 21 38")); // ldaddb
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
	EXPECT_EQ(8u, rmw->getValOperand()->getType()->getIntegerBitWidth());
	EXPECT_EQ(llvm::AtomicRMWInst::Add, rmw->getOperation());
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDCLR_clears_the_bits_that_are_set)
{
	// LDCLR clears the bits SET in the operand, so it is an AND with the
	// COMPLEMENT. Treating it as a plain AND clears exactly the wrong bits,
	// and with these values the two answers differ: 0xff & ~0x0f is 0xf0,
	// 0xff & 0x0f is 0x0f.
	setRegisters({
		{ARM64_REG_W1, 0x0f},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0xff_dw},
	});

	emulate_bin("62 10 21 b8"); // ldclr w1, w2, [x3]

	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W2, 0xff},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xf0_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDEOR)
{
	setRegisters({
		{ARM64_REG_W1, 0x0f},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0xff_dw},
	});

	emulate_bin("62 20 21 b8"); // ldeor w1, w2, [x3]

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xf0_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDSET)
{
	setRegisters({
		{ARM64_REG_W1, 0x0f},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0xf0_dw},
	});

	emulate_bin("62 30 21 b8"); // ldset w1, w2, [x3]

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xff_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SWP)
{
	setRegisters({
		{ARM64_REG_W1, 0xaa},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0xbb_dw},
	});

	emulate_bin("62 80 21 b8"); // swp w1, w2, [x3]

	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W2, 0xbb},
	});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xaa_dw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CAS_writes_the_old_value_to_Rs)
{
	// CAS is the one that does not follow the family: the old value goes back
	// to Rs, the FIRST operand, and Rt is the desired value. Writing it to Rt
	// like every other instruction here would be wrong, and these values make
	// that visible -- w1 must become 0xaa, not stay 0xaa by luck.
	setRegisters({
		{ARM64_REG_W1, 0xaa},
		{ARM64_REG_W2, 0xcc},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0xaa_dw},
	});

	emulate_bin("62 7c a1 88"); // cas w1, w2, [x3]

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0xcc_dw}, // matched, so the desired value is stored
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CAS_does_not_store_when_it_does_not_match)
{
	setRegisters({
		{ARM64_REG_W1, 0xaa},
		{ARM64_REG_W2, 0xcc},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0xbb_dw},
	});

	emulate_bin("62 7c a1 88"); // cas w1, w2, [x3]

	// The comparison fails, so memory keeps 0xbb and w1 takes it.
	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W1, 0xbb},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LDSMAX_is_not_a_pseudo_call)
{
	// The signed-max form. Its downstream converter had no case for
	// AtomicRMWInst::Max and dropped the write; that is fixed in llvmir2hll,
	// and this pins that the translator emits the atomic rather than an
	// opaque call.
	setRegisters({
		{ARM64_REG_W1, 0x05},
		{ARM64_REG_X3, 0x1000},
	});
	setMemory({
		{0x1000, 0x09_dw},
	});

	emulate_bin("62 40 21 b8"); // ldsmax w1, w2, [x3]

	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_W2, 0x09},
	});
	// 9 is already the larger, so memory is written back unchanged.
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_PRFM
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_PRFM_is_nothing)
{
	// A prefetch hint touches no register and no memory and cannot fault, so
	// there is nothing to translate -- the same answer ARM64_INS_NOP and
	// ARM64_INS_BTI already get. As a nullptr entry it came out as an
	// __asm_prfm call.
	setRegisters({
		{ARM64_REG_X0, 0x1000},
	});

	emulate("prfm pldl1keep, [x0]");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_LD1, ARM64_INS_ST1
//
// The straight-copy form of the NEON list load and store -- the last thing
// COV-01 found untranslated on ARM64 in the parity corpus. V registers are
// i128 globals, so the arrangement decides only the total width; these move
// bytes and need no lane model.
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LD1_whole_register)
{
	setRegisters({
		{ARM64_REG_X0, 0x1000},
	});
	setMemoryValue128(0x1000, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);

	emulate("ld1 {v0.16b}, [x0]");

	EXPECT_EQ(0x99aabbccddeeff00ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x1122334455667788ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_LD1_d_form_zeroes_the_upper_half)
{
	// `.8b` is a 64-bit access and every 64-bit write to a V register clears
	// bits 127:64. V0 starts with a non-zero upper half so that keeping it
	// would be visible; a translation that merged instead of zeroing would
	// leave 0xdeadbeefdeadbeef up there.
	setV(ARM64_REG_V0, 0xdeadbeefdeadbeefULL, 0);
	setRegisters({
		{ARM64_REG_X1, 0x2000},
	});
	setMemory({
		{0x2000, 0x0123456789abcdef_qw},
	});

	emulate("ld1 {v0.8b}, [x1]");

	EXPECT_EQ(0x0123456789abcdefULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ST1_d_form_writes_the_low_half)
{
	setV(ARM64_REG_V0, 0xdeadbeefdeadbeefULL, 0x0123456789abcdefULL);
	setRegisters({
		{ARM64_REG_X0, 0x3000},
	});

	emulate("st1 {v0.8b}, [x0]");

	EXPECT_JUST_MEMORY_STORED({
		{0x3000, 0x0123456789abcdef_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ST1_list_writes_consecutive_addresses)
{
	// The second register goes one access width further on, not to the same
	// address and not to base + 16.
	setV(ARM64_REG_V0, 0, 0x1111111111111111ULL);
	setV(ARM64_REG_V1, 0, 0x2222222222222222ULL);
	setRegisters({
		{ARM64_REG_X0, 0x4000},
	});

	emulate("st1 {v0.8b, v1.8b}, [x0]");

	EXPECT_JUST_MEMORY_STORED({
		{0x4000, 0x1111111111111111_qw},
		{0x4008, 0x2222222222222222_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}


//
// The NEON operations whose source and destination arrangements differ in
// width, and three that were not missing translations but wrong ones.

// `neg v0.4s, v1.4s` reached translateNeg(), which has no vector guard and
// emitted `sub i128 0, v1`. For v1 = 1,1,1,1 the instruction answers
// -1,-1,-1,-1 and that code answers 0xfffffffe fffffffe fffffffe ffffffff --
// every lane but the lowest is off by one, because the borrow crossed.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_NEG_vector_does_not_borrow_across_lanes)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000100000001ULL, /*lo=*/0x0000000100000001ULL);

	emulate("neg v0.4s, v1.4s");

	EXPECT_EQ(0xffffffffffffffffULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0xffffffffffffffffULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ABS_vector)
{
	// lanes -1, -2, 3, 4
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0xfffffffeffffffffULL);

	emulate("abs v0.4s, v1.4s");

	EXPECT_EQ(0x0000000200000001ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000400000003ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CNT)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0xaa557f800f0001ffULL);

	emulate("cnt v0.16b, v1.16b");

	// Per byte: ff->8, 01->1, 00->0, 0f->4, 80->1, 7f->7, 55->4, aa->4.
	EXPECT_EQ(0x0404070104000108ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// REV16, REV32 and REV64 reverse the BYTES within each group and leave the
// groups where they are. They are not a byte swap of the register: the three
// answers below differ from each other and all three differ from
// 0x000102...0f reversed.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_REV16_vector)
{
	setV(ARM64_REG_V1, /*hi=*/0x0f0e0d0c0b0a0908ULL, /*lo=*/0x0706050403020100ULL);

	emulate("rev16 v0.16b, v1.16b");

	EXPECT_EQ(0x0607040502030001ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0e0f0c0d0a0b0809ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_REV32_vector)
{
	setV(ARM64_REG_V1, /*hi=*/0x0f0e0d0c0b0a0908ULL, /*lo=*/0x0706050403020100ULL);

	emulate("rev32 v0.16b, v1.16b");

	EXPECT_EQ(0x0405060700010203ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0c0d0e0f08090a0bULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_REV64_vector)
{
	setV(ARM64_REG_V1, /*hi=*/0x0f0e0d0c0b0a0908ULL, /*lo=*/0x0706050403020100ULL);

	emulate("rev64 v0.16b, v1.16b");

	EXPECT_EQ(0x0001020304050607ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x08090a0b0c0d0e0fULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// DUP broadcasts. It reached translateMov(), which put w1 in lane 0 and zero
// in the other three.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_DUP_from_a_gpr)
{
	setRegisters({
		{ARM64_REG_W1, 0xdeadbeef},
	});

	emulate("dup v0.4s, w1");

	EXPECT_EQ(0xdeadbeefdeadbeefULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0xdeadbeefdeadbeefULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// The lane form. It reached translateMov() too, which copied the whole source
// register -- so the answer was the source unchanged rather than lane 2 in
// all four places.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_DUP_from_a_lane)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);

	emulate("dup v0.4s, v1.s[2]");

	EXPECT_EQ(0x0000000300000003ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000300000003ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// `mov s0, v1.s[0]` is the SAME Capstone id as `dup v0.4s, w1` -- ARM64_INS_DUP
// for both -- and the two are told apart only by whether the destination has a
// vector arrangement. This pins the scalar form so that diverting on the id
// alone fails.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_DUP_with_a_scalar_destination_is_a_move)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0x00000000deadbeefULL);

	auto* f = translate(assemble("mov s0, v1.s[0]"));
	ASSERT_NE(nullptr, f);
	EXPECT_EQ(nullptr, _module.getFunction("__asm_dup"));
	EXPECT_EQ(nullptr, _module.getFunction("__asm_mov"));
}

// The narrowing moves. The `2` suffix is not a different operation, it is a
// different DESTINATION HALF: `xtn` writes 64 bits and zeroes the top of the
// register, `xtn2` writes the top and leaves the bottom alone. A compiler
// emits the pair back to back, so translating `xtn2` as `xtn` destroys the
// half the previous instruction just produced.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_XTN)
{
	setV(ARM64_REG_V1, /*hi=*/0xff00ddeebbcc99aaULL, /*lo=*/0x7788556633441122ULL);
	setV(ARM64_REG_V0, /*hi=*/0xcafecafecafecafeULL, /*lo=*/0xcafecafecafecafeULL);

	emulate("xtn v0.8b, v1.8h");

	EXPECT_EQ(0x00eeccaa88664422ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_XTN2_keeps_the_lower_half)
{
	setV(ARM64_REG_V1, /*hi=*/0xff00ddeebbcc99aaULL, /*lo=*/0x7788556633441122ULL);
	setV(ARM64_REG_V0, /*hi=*/0xcafecafecafecafeULL, /*lo=*/0x0123456789abcdefULL);

	emulate("xtn2 v0.16b, v1.8h");

	EXPECT_EQ(0x0123456789abcdefULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x00eeccaa88664422ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SHRN)
{
	setV(ARM64_REG_V1, /*hi=*/0xddeeff0099aabbccULL, /*lo=*/0x5566778811223344ULL);

	emulate("shrn v0.4h, v1.4s, #4");

	// Each word shifted right 4 and truncated to a halfword.
	EXPECT_EQ(0xeff0abbc67782334ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// The widening adds exist so that the sum of two full-range lanes cannot
// overflow, which is exactly the bit an implementation at the narrow width
// throws away: 0xff + 1 is 0x0100 here and 0x00 at eight bits.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UADDL)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0xffffffffffffffffULL);
	setV(ARM64_REG_V2, /*hi=*/0ULL, /*lo=*/0x0101010101010101ULL);

	emulate("uaddl v0.8h, v1.8b, v2.8b");

	EXPECT_EQ(0x0100010001000100ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0100010001000100ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// Identical operands, one letter different: 0xff is -1 signed, so every lane
// is zero.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SADDL_is_signed)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0xffffffffffffffffULL);
	setV(ARM64_REG_V2, /*hi=*/0ULL, /*lo=*/0x0101010101010101ULL);

	emulate("saddl v0.8h, v1.8b, v2.8b");

	EXPECT_EQ(0ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// The W form takes one already-wide source and one narrow one.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UADDW)
{
	setV(ARM64_REG_V1, /*hi=*/0x00ff00ff00ff00ffULL, /*lo=*/0x00ff00ff00ff00ffULL);
	setV(ARM64_REG_V2, /*hi=*/0ULL, /*lo=*/0x0101010101010101ULL);

	emulate("uaddw v0.8h, v1.8h, v2.8b");

	EXPECT_EQ(0x0100010001000100ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0100010001000100ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

//
// The NEON lane operations.
//
// Batch C's note on ARM64 said of these: "UMAXP (882), SHRN (798), UMINP,
// ADDP, ADDV, UZP1, SADDL, UADDW, XTN, SHL, UMOV, CNT and MVNI are ordinary
// lane operations this register model can express. A next batch, not a
// limitation." This is that batch, restricted to the ones whose operands are
// all the same width.
//
// v1 holds the words 1, 2, 3, 4 and v2 holds 5, 6, 7, 8 throughout the
// permutes and the pairwise forms, because every lane is different and a mask
// that is backwards therefore answers with the lanes in the wrong order rather
// than by accident with the right ones.

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UZP1)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);
	setV(ARM64_REG_V2, /*hi=*/0x0000000800000007ULL, /*lo=*/0x0000000600000005ULL);

	emulate("uzp1 v0.4s, v1.4s, v2.4s");

	// The even lanes of [v1, v2]: 1, 3, 5, 7.
	EXPECT_EQ(0x0000000300000001ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000700000005ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UZP2)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);
	setV(ARM64_REG_V2, /*hi=*/0x0000000800000007ULL, /*lo=*/0x0000000600000005ULL);

	emulate("uzp2 v0.4s, v1.4s, v2.4s");

	// The odd lanes: 2, 4, 6, 8.
	EXPECT_EQ(0x0000000400000002ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000800000006ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ZIP1)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);
	setV(ARM64_REG_V2, /*hi=*/0x0000000800000007ULL, /*lo=*/0x0000000600000005ULL);

	emulate("zip1 v0.4s, v1.4s, v2.4s");

	// The lower halves interleaved: 1, 5, 2, 6.
	EXPECT_EQ(0x0000000500000001ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000600000002ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ZIP2)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);
	setV(ARM64_REG_V2, /*hi=*/0x0000000800000007ULL, /*lo=*/0x0000000600000005ULL);

	emulate("zip2 v0.4s, v1.4s, v2.4s");

	// The upper halves: 3, 7, 4, 8.
	EXPECT_EQ(0x0000000700000003ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000800000004ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// TRN1 and ZIP1 agree on the first two lanes and differ on the rest, which is
// why both are here: one test could not tell them apart.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TRN1)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);
	setV(ARM64_REG_V2, /*hi=*/0x0000000800000007ULL, /*lo=*/0x0000000600000005ULL);

	emulate("trn1 v0.4s, v1.4s, v2.4s");

	// The even lanes of each: 1, 5, 3, 7.
	EXPECT_EQ(0x0000000500000001ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000700000003ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_TRN2)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);
	setV(ARM64_REG_V2, /*hi=*/0x0000000800000007ULL, /*lo=*/0x0000000600000005ULL);

	emulate("trn2 v0.4s, v1.4s, v2.4s");

	// The odd lanes of each: 2, 6, 4, 8.
	EXPECT_EQ(0x0000000600000002ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000800000004ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// The pairwise forms operate on ADJACENT lanes of vn followed by vm, not on
// corresponding lanes of vn and vm. A lane-wise implementation of `addp` on
// these operands answers 6, 8, 10, 12 rather than 3, 7, 11, 15.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_ADDP)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);
	setV(ARM64_REG_V2, /*hi=*/0x0000000800000007ULL, /*lo=*/0x0000000600000005ULL);

	emulate("addp v0.4s, v1.4s, v2.4s");

	EXPECT_EQ(0x0000000700000003ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000f0000000bULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMAXP)
{
	setV(ARM64_REG_V1, /*hi=*/0x0000000400000003ULL, /*lo=*/0x0000000200000001ULL);
	setV(ARM64_REG_V2, /*hi=*/0x0000000800000007ULL, /*lo=*/0x0000000600000005ULL);

	emulate("umaxp v0.4s, v1.4s, v2.4s");

	// max of each adjacent pair: 2, 4, 6, 8.
	EXPECT_EQ(0x0000000400000002ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000800000006ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// UMAX and SMAX are one letter apart and disagree on every lane whose top bit
// is set -- which for the byte lanes a NEON string routine works on is all the
// interesting ones. The two tests use identical operands.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMAX_is_unsigned)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0x00000000000000ffULL);
	setV(ARM64_REG_V2, /*hi=*/0ULL, /*lo=*/0x0000000000000001ULL);

	emulate("umax v0.16b, v1.16b, v2.16b");

	// 0xff is 255 unsigned, so it wins.
	EXPECT_EQ(0x00000000000000ffULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMAX_is_signed)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0x00000000000000ffULL);
	setV(ARM64_REG_V2, /*hi=*/0ULL, /*lo=*/0x0000000000000001ULL);

	emulate("smax v0.16b, v1.16b, v2.16b");

	// 0xff is -1 signed, so 1 wins. Every other lane is 0 against 0.
	EXPECT_EQ(0x0000000000000001ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// The lane shifts are of the lane, not of the register: bits leaving a lane's
// top do not enter the next one.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SHL_vector)
{
	setV(ARM64_REG_V1, /*hi=*/0x8000000180000001ULL, /*lo=*/0x8000000180000001ULL);

	emulate("shl v0.4s, v1.4s, #1");

	// Each word's top bit is discarded rather than carried into the next.
	EXPECT_EQ(0x0000000200000002ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000000200000002ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_USHR_vector)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0x00000000000000ffULL);

	emulate("ushr v0.16b, v1.16b, #4");

	EXPECT_EQ(0x000000000000000fULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// SSHR is arithmetic and USHR is not, and they disagree on exactly the lanes
// whose top bit is set.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SSHR_vector)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0x00000000000000ffULL);

	emulate("sshr v0.16b, v1.16b, #4");

	// 0xff is -1, and -1 >> 4 is -1.
	EXPECT_EQ(0x00000000000000ffULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// A right shift by the lane width is a legal encoding -- it is how a register
// is zeroed lane by lane -- and a shift equal to the operand's width is poison
// in LLVM. Both right shifts are case-split for it.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_USHR_by_the_lane_width)
{
	setV(ARM64_REG_V1, /*hi=*/0xffffffffffffffffULL, /*lo=*/0xffffffffffffffffULL);

	emulate("ushr v0.16b, v1.16b, #8");

	EXPECT_EQ(0ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SSHR_by_the_lane_width)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0x000000000000ff01ULL);

	emulate("sshr v0.16b, v1.16b, #8");

	// Each lane becomes its own sign: 0x01 -> 0, 0xff -> 0xff.
	EXPECT_EQ(0x000000000000ff00ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

// One lane out of a NEON register into a general-purpose one, which is how the
// result of a lane compare gets back into scalar code.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_UMOV)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0x00000000ff112233ULL);

	emulate("umov w0, v1.b[3]");

	EXPECT_EQ(0xff, getRegisterValueUnsigned(ARM64_REG_W0));
	EXPECT_NO_VALUE_CALLED();
}

// SMOV sign-extends where UMOV zero-extends, and 0xff is where they differ.
TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_SMOV)
{
	setV(ARM64_REG_V1, /*hi=*/0ULL, /*lo=*/0x00000000ff112233ULL);

	emulate("smov x0, v1.b[3]");

	EXPECT_EQ(0xffffffffffffffffULL, getRegisterValueUnsigned(ARM64_REG_X0));
	EXPECT_NO_VALUE_CALLED();
}

//
// ARM64_INS_EXT, the bitwise selects, and the lane compares.
//
// What COV-01 finds on ARM64 once the static corpus is measured and the
// zero-filled holes in glibc's .text are taken out of the denominator: EXT
// 5,124, CMEQ 1,512, CMHS 294, BIT 210, CMGE 42. All of them expressible with
// V0..V31 as i128 globals.
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXT_takes_a_window_across_both)
{
	// Byte 0 of the result is byte 4 of vn, and byte 15 is byte 3 of vm.
	// The operand order is the opposite of x86's PALIGNR -- there the
	// destination is the high half of the concatenation, here the second
	// source is -- so a translation copied across from that one answers with
	// the halves swapped.
	setV(ARM64_REG_V1, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
	setV(ARM64_REG_V2, 0xaabbccddeeff0011ULL, 0x2233445566778899ULL);

	emulate("ext v0.16b, v1.16b, v2.16b, #4");

	EXPECT_EQ(0x5566778899aabbccULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x6677889911223344ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXT_of_zero_is_the_first_source)
{
	// The index for which the second shift would be by the full operand
	// width, which is poison.
	setV(ARM64_REG_V1, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
	setV(ARM64_REG_V2, 0xaabbccddeeff0011ULL, 0x2233445566778899ULL);

	emulate("ext v0.16b, v1.16b, v2.16b, #0");

	EXPECT_EQ(0x99aabbccddeeff00ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x1122334455667788ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_EXT_d_form_is_64_bit_and_zeroes_the_top)
{
	// `.8b` makes the whole operation 64-bit: the window runs from vn into
	// vm across eight bytes, not sixteen, and the write clears bits 127:64.
	// V0 starts dirty so that keeping them would be visible.
	setV(ARM64_REG_V0, 0xffffffffffffffffULL, 0xffffffffffffffffULL);
	setV(ARM64_REG_V1, 0xdeadbeefdeadbeefULL, 0x1122334455667788ULL);
	setV(ARM64_REG_V2, 0xdeadbeefdeadbeefULL, 0x99aabbccddeeff00ULL);

	emulate("ext v0.8b, v1.8b, v2.8b, #4");

	EXPECT_EQ(0xddeeff0011223344ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BSL_selects_with_the_destination)
{
	// The three bitwise selects share operands here on purpose: which
	// register is the selector is the entire difference between them, so the
	// three tests differ only in the mnemonic and answer differently.
	setV(ARM64_REG_V0, 0x1111111111111111ULL, 0x2222222222222222ULL);
	setV(ARM64_REG_V1, 0x3333333333333333ULL, 0x4444444444444444ULL);
	setV(ARM64_REG_V2, 0x5555555555555555ULL, 0x6666666666666666ULL);

	emulate("bsl v0.16b, v1.16b, v2.16b");

	EXPECT_EQ(0x4444444444444444ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x5555555555555555ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BIT_selects_with_the_second_source)
{
	setV(ARM64_REG_V0, 0x1111111111111111ULL, 0x2222222222222222ULL);
	setV(ARM64_REG_V1, 0x3333333333333333ULL, 0x4444444444444444ULL);
	setV(ARM64_REG_V2, 0x5555555555555555ULL, 0x6666666666666666ULL);

	emulate("bit v0.16b, v1.16b, v2.16b");

	EXPECT_EQ(0x4444444444444444ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x1111111111111111ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_BIF_is_BIT_with_the_mask_inverted)
{
	setV(ARM64_REG_V0, 0x1111111111111111ULL, 0x2222222222222222ULL);
	setV(ARM64_REG_V1, 0x3333333333333333ULL, 0x4444444444444444ULL);
	setV(ARM64_REG_V2, 0x5555555555555555ULL, 0x6666666666666666ULL);

	emulate("bif v0.16b, v1.16b, v2.16b");

	EXPECT_EQ(0x2222222222222222ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x3333333333333333ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMEQ_against_zero_is_the_string_idiom)
{
	// "Which of these sixteen bytes is the terminator." Byte 2 is the only
	// non-zero one, so it is the only lane that comes out zero.
	setV(ARM64_REG_V1, 0, 0x0000000000ff0000ULL);

	emulate("cmeq v0.16b, v1.16b, #0");

	EXPECT_EQ(0xffffffffff00ffffULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0xffffffffffffffffULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMHS_compares_unsigned)
{
	// Byte 0 is 0xff against 0x01: unsigned that is 255 >= 1 and the lane is
	// all ones.
	setV(ARM64_REG_V1, 0, 0x00000000000000ffULL);
	setV(ARM64_REG_V2, 0, 0x0000000000000001ULL);

	emulate("cmhs v0.16b, v1.16b, v2.16b");

	EXPECT_EQ(0xffffffffffffffffULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0xffffffffffffffffULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMGE_compares_signed)
{
	// Same operands as CMHS above, and one letter apart in the mnemonic:
	// signed, 0xff is -1 and -1 >= 1 is false, so byte 0 comes out zero.
	setV(ARM64_REG_V1, 0, 0x00000000000000ffULL);
	setV(ARM64_REG_V2, 0, 0x0000000000000001ULL);

	emulate("cmge v0.16b, v1.16b, v2.16b");

	EXPECT_EQ(0xffffffffffffff00ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0xffffffffffffffffULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMGT_reads_the_arrangement)
{
	// 4S rather than 16B, so the lane width comes from the arrangement and
	// not from a default. As byte lanes the answer would be
	// 0xffff00ff00000000 in the low half.
	setV(ARM64_REG_V1, 0, 0x00000005ffffffffULL);
	setV(ARM64_REG_V2, 0, 0x0000000200000001ULL);

	emulate("cmgt v0.4s, v1.4s, v2.4s");

	EXPECT_EQ(0xffffffff00000000ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMTST_is_an_AND_not_a_comparison)
{
	setV(ARM64_REG_V1, 0, 0x0000000000000f0fULL);
	setV(ARM64_REG_V2, 0, 0x0000000000000801ULL);

	emulate("cmtst v0.16b, v1.16b, v2.16b");

	EXPECT_EQ(0x000000000000ffffULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_CMEQ_d_form_zeroes_the_top)
{
	setV(ARM64_REG_V0, 0xffffffffffffffffULL, 0xffffffffffffffffULL);
	setV(ARM64_REG_V1, 0xdeadbeefdeadbeefULL, 0x0000000000ff0000ULL);

	emulate("cmeq v0.8b, v1.8b, #0");

	EXPECT_EQ(0xffffffffff00ffffULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}


//
// ARM64_INS_MRS, ARM64_INS_MSR
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MRS_reads_the_thread_pointer)
{
	// 275 of every 293 `mrs` in the static corpus are this one, 12,400 in
	// total. The register was already in arm64_init.cpp's tables; what was
	// missing was the instruction that reads it.
	setRegisters({
		{ARM64_SYSREG_TPIDR_EL0, 0xdeadbeefcafef00d},
	});

	emulate("mrs x0, tpidr_el0");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X0, 0xdeadbeefcafef00d},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MRS_reads_other_system_registers_too)
{
	// Not a special case for one register: every system register Capstone
	// knows already has a global here.
	setRegisters({
		{ARM64_SYSREG_MIDR_EL1, 0x1122334455667788},
	});

	emulate("mrs x1, midr_el1");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_REG_X1, 0x1122334455667788},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ARM64_INS_MSR_writes_the_thread_pointer)
{
	// The other direction, and the reason the operand order is read from the
	// instruction rather than assumed: MSR's system register is operand 0.
	setRegisters({
		{ARM64_REG_X0, 0x00c0ffee0badf00d},
	});

	emulate("msr tpidr_el0, x0");

	EXPECT_JUST_REGISTERS_STORED({
		{ARM64_SYSREG_TPIDR_EL0, 0x00c0ffee0badf00d},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// ============================================================================
// The ARM64 forms that had a dispatch entry and fell back anyway
// ============================================================================
//
// Every instruction below was already in the dispatch table with a function
// pointer, so COV-01 counted it as covered. PSEUDO-01 -- which asks the
// translator instead of the table -- found them emitting pseudo-assembly for
// 2,300 sites in the static corpus, because each translator declined the
// particular operand shape these forms use.
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, MOVI_replicates_across_a_4s_arrangement)
{
	setV(ARM64_REG_V0, 0xffffffffffffffffULL, 0xffffffffffffffffULL);

	emulate("movi v0.4s, #0x33");

	EXPECT_EQ(0x0000003300000033ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000003300000033ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, MOVI_zeroes_a_vector)
{
	setV(ARM64_REG_V0, 0xffffffffffffffffULL, 0xffffffffffffffffULL);

	emulate("movi v0.4s, #0x0");

	// 1,053 sites in the static corpus are exactly this: the idiom a
	// compiler uses to zero a vector register.
	EXPECT_EQ(0x0ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, MOVI_applies_the_lsl_shift_within_the_lane)
{
	setV(ARM64_REG_V0, 0, 0);

	emulate("movi v0.4s, #0x33, lsl #8");

	// Capstone reports the RAW imm8 and the shift separately, not the
	// element value: taking the immediate alone would answer 0x33.
	EXPECT_EQ(0x0000330000003300ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0000330000003300ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, MOVI_msl_shifts_ones_in_not_zeroes)
{
	setV(ARM64_REG_V0, 0, 0);

	emulate("movi v0.4s, #0x33, msl #8");

	// MSL fills the vacated bits with ONES. An ordinary left shift would
	// answer 0x00003300 per lane.
	EXPECT_EQ(0x000033ff000033ffULL, vLow(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, MOVI_2d_takes_the_immediate_whole)
{
	setV(ARM64_REG_V0, 0, 0);

	emulate("movi v0.2d, #0xff00ff00ff00ff00");

	// For the .2d arrangement capstone reports the already-expanded 64 bits
	// and there is no shift to apply.
	EXPECT_EQ(0xff00ff00ff00ff00ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0xff00ff00ff00ff00ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, MVNI_is_the_complement_within_the_lane)
{
	setV(ARM64_REG_V0, 0, 0);

	emulate("mvni v0.4s, #0x33");

	// Complemented at 32 bits, not at 8: 0xcc would be the wrong width.
	EXPECT_EQ(0xffffffccffffffccULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0xffffffccffffffccULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, REV16_reverses_bytes_within_each_halfword)
{
	setRegisters({
		{ARM64_REG_W1, 0x11223344},
	});

	emulate("rev16 w0, w1");

	// Within each halfword, not across the register: reversing the whole
	// word would answer 0x44332211.
	EXPECT_EQ(0x22114433ULL, getRegisterValueUnsigned(ARM64_REG_W0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, REV16_on_a_64_bit_register_has_four_halfwords)
{
	setRegisters({
		{ARM64_REG_X1, 0x1122334455667788ULL},
	});

	emulate("rev16 x0, x1");

	EXPECT_EQ(0x2211443366558877ULL, getRegisterValueUnsigned(ARM64_REG_X0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, REV32_reverses_bytes_within_each_word)
{
	setRegisters({
		{ARM64_REG_X1, 0x1122334455667788ULL},
	});

	emulate("rev32 x0, x1");

	EXPECT_EQ(0x4433221188776655ULL, getRegisterValueUnsigned(ARM64_REG_X0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, FMOV_reads_the_high_lane_of_a_vector)
{
	setV(ARM64_REG_V0, 0xaaaaaaaaaaaaaaaaULL, 0x5555555555555555ULL);

	emulate("fmov x3, v0.d[1]");

	// Lane 1 is the HIGH half. Reading the register as a scalar would
	// answer the low half.
	EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, getRegisterValueUnsigned(ARM64_REG_X3));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, FMOV_writes_the_high_lane_and_leaves_the_low_one)
{
	setV(ARM64_REG_V0, 0xaaaaaaaaaaaaaaaaULL, 0x5555555555555555ULL);
	setRegisters({
		{ARM64_REG_X3, 0x0123456789abcdefULL},
	});

	emulate("fmov v0.d[1], x3");

	EXPECT_EQ(0x5555555555555555ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0123456789abcdefULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, SADDL2_reads_the_upper_half_of_its_sources)
{
	// Low half 1, upper half 0x10 in both sources.
	setV(ARM64_REG_V4, 0x0000001000000010ULL, 0x0000000100000001ULL);
	setV(ARM64_REG_V5, 0x0000002000000020ULL, 0x0000000200000002ULL);
	setV(ARM64_REG_V7, 0, 0);

	emulate("saddl2 v7.2d, v4.4s, v5.4s");

	// 0x10 + 0x20 in both destination lanes. The form without the 2 would
	// read the LOW half and answer 3.
	EXPECT_EQ(0x30ULL, vLow(ARM64_REG_V7));
	EXPECT_EQ(0x30ULL, vHigh(ARM64_REG_V7));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, SADDL2_sign_extends_where_UADDL2_zero_extends)
{
	setV(ARM64_REG_V4, 0xffffffffffffffffULL, 0);
	setV(ARM64_REG_V5, 0x0000000100000001ULL, 0);
	setV(ARM64_REG_V7, 0, 0);

	emulate("saddl2 v7.2d, v4.4s, v5.4s");

	// -1 + 1 == 0 signed. Zero-extending would answer 0x100000000.
	EXPECT_EQ(0x0ULL, vLow(ARM64_REG_V7));
	EXPECT_EQ(0x0ULL, vHigh(ARM64_REG_V7));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, UADDL2_zero_extends)
{
	setV(ARM64_REG_V4, 0xffffffffffffffffULL, 0);
	setV(ARM64_REG_V5, 0x0000000100000001ULL, 0);
	setV(ARM64_REG_V7, 0, 0);

	emulate("uaddl2 v7.2d, v4.4s, v5.4s");

	EXPECT_EQ(0x100000000ULL, vLow(ARM64_REG_V7));
	EXPECT_EQ(0x100000000ULL, vHigh(ARM64_REG_V7));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ADDV_sums_every_lane_into_a_scalar)
{
	setV(ARM64_REG_V1, 0, 0x0807060504030201ULL);
	setV(ARM64_REG_V0, 0xffffffffffffffffULL, 0xffffffffffffffffULL);

	emulate("addv b0, v1.8b");

	// 1+2+...+8 == 36, and the byte destination clears everything above it.
	EXPECT_EQ(36ULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, ADDV_wraps_within_the_lane_width)
{
	setV(ARM64_REG_V1, 0, 0x8080808080808080ULL);
	setV(ARM64_REG_V0, 0, 0);

	emulate("addv b0, v1.8b");

	// Eight lanes of 0x80 sum to 0x400, which is 0 in eight bits. Summing
	// at a wider width would answer 0x400.
	EXPECT_EQ(0x0ULL, vLow(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, UMAXV_is_unsigned_and_SMAXV_is_not)
{
	setV(ARM64_REG_V1, 0, 0x01020304050607ffULL);
	setV(ARM64_REG_V0, 0, 0);

	emulate("umaxv b0, v1.8b");

	EXPECT_EQ(0xffULL, vLow(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, SMAXV_treats_the_lanes_as_signed)
{
	setV(ARM64_REG_V1, 0, 0x01020304050607ffULL);
	setV(ARM64_REG_V0, 0, 0);

	emulate("smaxv b0, v1.8b");

	// 0xff is -1 signed, so the maximum is 7 rather than 0xff.
	EXPECT_EQ(0x07ULL, vLow(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

//
// ============================================================================
// b0, h0, s0, d0, q0 and v0 are one register
// ============================================================================
//
// They were six independent globals. The ARM64 parent map covered only
// W -> X, WSP -> SP and WZR -> XZR, so a scalar floating-point write and a
// vector read of the same hardware register never saw each other. That is not
// an exotic-extension problem like SVE: it is ordinary compiled
// floating-point code.
//

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, A_scalar_FP_write_is_visible_as_the_vector_register)
{
	setV(ARM64_REG_V0, 0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL);
	setRegisters({{ARM64_REG_X1, 0x0123456789abcdefULL}});

	emulate("fmov d0, x1");

	EXPECT_EQ(0x0123456789abcdefULL, vLow(ARM64_REG_V0));
	// Writing a narrow view zeroes the rest of the register.
	EXPECT_EQ(0x0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, A_vector_write_is_visible_as_the_scalar_register)
{
	setV(ARM64_REG_V0, 0, 0);

	emulate("movi v0.2d, #0xff00ff00ff00ff00");

	EXPECT_EQ(0xff00ff00ff00ff00ULL, getRegisterValueUnsigned(ARM64_REG_D0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, An_s_register_write_zeroes_the_whole_vector_above_it)
{
	setV(ARM64_REG_V0, 0xffffffffffffffffULL, 0xffffffffffffffffULL);
	setRegisters({{ARM64_REG_W1, 0x40490fdb}});

	emulate("fmov s0, w1");

	EXPECT_EQ(0x40490fdbULL, vLow(ARM64_REG_V0));
	EXPECT_EQ(0x0ULL, vHigh(ARM64_REG_V0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorArm64Tests, A_d_register_read_takes_the_low_half_of_the_vector)
{
	setV(ARM64_REG_V2, 0xaaaaaaaaaaaaaaaaULL, 0x0123456789abcdefULL);

	emulate("fmov x4, d2");

	// The low half, not the high one and not some separate d2 storage.
	EXPECT_EQ(0x0123456789abcdefULL, getRegisterValueUnsigned(ARM64_REG_X4));
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
