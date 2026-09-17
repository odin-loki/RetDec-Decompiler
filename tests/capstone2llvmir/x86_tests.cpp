/**
 * @file tests/capstone2llvmir/x86_tests.cpp
 * @brief Capstone2LlvmIrTranslatorX86 unit tests.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cmath>
#include <cstring>

#include <llvm/IR/InstIterator.h>

#include "capstone2llvmir/capstone2llvmir_tests.h"
#include "retdec/capstone2llvmir/x86/x86.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace capstone2llvmir {
namespace tests {

class Capstone2LlvmIrTranslatorX86Tests :
		public Capstone2LlvmIrTranslatorTests,
		public ::testing::WithParamInterface<cs_mode>
{
	protected:
		virtual void initKeystoneEngine() override
		{
			ks_mode mode = KS_MODE_32;
			switch(GetParam())
			{
				case CS_MODE_16: mode = KS_MODE_16; break;
				case CS_MODE_32: mode = KS_MODE_32; break;
				case CS_MODE_64: mode = KS_MODE_64; break;
				default: throw std::runtime_error("ERROR: unknown mode.\n");
			}
			if (ks_open(KS_ARCH_X86, mode, &_assembler) != KS_ERR_OK)
			{
				throw std::runtime_error("ERROR: failed on ks_open().\n");
			}
		}

		virtual void initCapstone2LlvmIrTranslator() override
		{
			switch(GetParam())
			{
				case CS_MODE_16:
					_translator = Capstone2LlvmIrTranslator::createX86_16(&_module);
					break;
				case CS_MODE_32:
					_translator = Capstone2LlvmIrTranslator::createX86_32(&_module);
					break;
				case CS_MODE_64:
					_translator = Capstone2LlvmIrTranslator::createX86_64(&_module);
					break;
				default:
					throw std::runtime_error("ERROR: unknown mode.\n");
			}
		}

	protected:
		virtual llvm::Function* modifyTranslationForEmulation(llvm::Function* f) override
		{
			Capstone2LlvmIrTranslatorX86* x86Trans = getX86Translator();

			auto* top = getRegister(X87_REG_TOP);
			assert(top);
			int topVal = _emulator->getGlobalVariableValue(top).IntVal.getZExtValue();

			std::map<Value*, int> vals;

			for (llvm::inst_iterator I = llvm::inst_begin(f),
					E = llvm::inst_end(f); I != E; ++I)
			{
				llvm::Instruction* i = &*I;

				auto* l = dyn_cast<LoadInst>(i);
				auto* sub = dyn_cast<SubOperator>(i);
				auto* add = dyn_cast<AddOperator>(i);
				auto* call = dyn_cast<CallInst>(i);

				if (l && l->getPointerOperand() == top)
				{
					vals[l] = topVal;
				}
				else if (sub
						&& vals.find(sub->getOperand(0)) != vals.end()
						&& isa<ConstantInt>(sub->getOperand(1)))
				{
					uint64_t v = cast<ConstantInt>(sub->getOperand(1))->getZExtValue();
					auto fIt = vals.find(sub->getOperand(0));
					assert(fIt != vals.end());
					vals[sub] = fIt->second - v;
				}
				else if (add
						&& vals.find(add->getOperand(0)) != vals.end()
						&& isa<ConstantInt>(add->getOperand(1)))
				{
					uint64_t v = cast<ConstantInt>(add->getOperand(1))->getZExtValue();
					auto fIt = vals.find(add->getOperand(0));
					assert(fIt != vals.end());
					vals[add] = fIt->second + v;
				}
				else if (call
						&& x86Trans->getX87DataStoreFunction() == call->getCalledFunction())
				{
					int idx = 0;
					if (auto* ci = dyn_cast<ConstantInt>(call->getArgOperand(0)))
					{
						idx = ci->getZExtValue();
					}
					else
					{
						auto fIt = vals.find(call->getArgOperand(0));
						assert(fIt != vals.end());
						idx = fIt->second;
					}
					assert(0 <= idx && idx <= 7);
					auto* val = call->getArgOperand(1);
					GlobalVariable* reg = nullptr;
					if (x86Trans->getX87DataStoreFunction() == call->getCalledFunction())
					{
						reg = x86Trans->getRegister(X86_REG_ST0 + idx);
					}
					assert(reg);

					new StoreInst(val, reg, i);
					E = llvm::inst_end(f);
				}
				else if (call
						&& x86Trans->getX87DataLoadFunction() == call->getCalledFunction())
				{
					int idx = 0;
					if (auto* ci = dyn_cast<ConstantInt>(call->getArgOperand(0)))
					{
						idx = ci->getZExtValue();
					}
					else
					{
						auto fIt = vals.find(call->getArgOperand(0));
						assert(fIt != vals.end());
						idx = fIt->second;
					}
					assert(0 <= idx && idx <= 7);
					GlobalVariable* reg = nullptr;
					if (x86Trans->getX87DataLoadFunction() == call->getCalledFunction())
					{
						reg = x86Trans->getRegister(X86_REG_ST0 + idx);
					}
					assert(reg);

					auto* l = new LoadInst(reg->getValueType(), reg, "", i);
					call->replaceAllUsesWith(l);
					E = llvm::inst_end(f);
				}
			}

			return f;
		}

	// These can/should be used at the beginning of each test case to
	// determine which modes should the case be run for.
	// They are macros because we want them to cause return in the current
	// function (test case).
	//
	protected:
#define ALL_MODES
#define ONLY_MODE_16 if (GetParam() != CS_MODE_16) return;
#define ONLY_MODE_32 if (GetParam() != CS_MODE_32) return;
#define ONLY_MODE_64 if (GetParam() != CS_MODE_64) return;
#define SKIP_MODE_16 if (GetParam() == CS_MODE_16) return;
#define SKIP_MODE_32 if (GetParam() == CS_MODE_32) return;
#define SKIP_MODE_64 if (GetParam() == CS_MODE_64) return;

	protected:
		Capstone2LlvmIrTranslatorX86* getX86Translator()
		{
			return dynamic_cast<Capstone2LlvmIrTranslatorX86*>(_translator.get());
		}

	// Some of these (or their parts) might be moved to abstract parent class.
	//
	protected:
		uint32_t getParentRegister(uint32_t reg)
		{
			return getX86Translator()->getParentRegister(reg);
		}

		virtual llvm::GlobalVariable* getRegister(uint32_t reg) override
		{
			return _translator->getRegister(getParentRegister(reg));
		}

		// XMM registers are i128 and StoredValue tops out at 64 bits -- its
		// `_ow` literal is an `assert(false)` -- so the two halves are read
		// and written directly here. Both halves matter: the difference
		// between ADDSD and ADDPD is only visible in the upper one, and
		// getRegisterValueUnsigned() below would assert on a register whose
		// value does not fit in 64 bits.
		void setXmm(uint32_t reg, uint64_t hi, uint64_t lo)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			llvm::GenericValue v = _emulator->getGlobalVariableValue(gv);
			const uint64_t words[2] = {lo, hi};
			v.IntVal = llvm::APInt(128, llvm::ArrayRef<uint64_t>(words, 2));
			_emulator->setGlobalVariableValue(gv, v);
		}

		uint64_t xmmLow(uint32_t reg)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			return _emulator->getGlobalVariableValue(gv).IntVal.trunc(64).getZExtValue();
		}

		uint64_t xmmHigh(uint32_t reg)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			return _emulator->getGlobalVariableValue(gv).IntVal.lshr(64).trunc(64).getZExtValue();
		}

		// A vector register is THREE globals, not one:
		//
		//   ZMMn = ZMMn_HI(511:256) : YMMn_HI(255:128) : XMMn(127:0)
		//
		// The i256 X86_REG_YMMn and i512 X86_REG_ZMMn globals still exist in
		// the register file but nothing reads or writes them -- using them
		// would put xmm3 and zmm3 in different storage, which is the bug the
		// decomposition exists to avoid. These helpers therefore address the
		// slices, so a test cannot accidentally set a global nothing reads.
		void setRegisterWide(uint32_t reg, const uint64_t* words, unsigned n)
		{
			auto* gv = getRegister(reg);
			assert(gv);
			llvm::GenericValue v = _emulator->getGlobalVariableValue(gv);
			v.IntVal = llvm::APInt(n * 64, llvm::ArrayRef<uint64_t>(words, n));
			_emulator->setGlobalVariableValue(gv, v);
		}

		/// @param q Eight quadwords, least significant first.
		void setZmm(unsigned n, const uint64_t (&q)[8])
		{
			setRegisterWide(X86_REG_XMM0 + n, &q[0], 2);
			setRegisterWide(X86_REG_YMM0_HI + n, &q[2], 2);
			setRegisterWide(X86_REG_ZMM0_HI + n, &q[4], 4);
		}

		/// True when @p f contains a call to a pseudo-assembly function --
		/// i.e. the translator declined to model the instruction, which for
		/// a form it cannot express is the correct outcome.
		bool callsPseudoAsm(llvm::Function* f)
		{
			for (auto it = llvm::inst_begin(f), e = llvm::inst_end(f); it != e; ++it)
			{
				auto* c = dyn_cast<CallInst>(&*it);
				if (c && _translator->isPseudoAsmFunctionCall(c))
				{
					return true;
				}
			}
			return false;
		}

		/// Quadword @p w (0..7) of vector register @p n.
		uint64_t zmmWord(unsigned n, unsigned w)
		{
			uint32_t reg = w < 2 ? X86_REG_XMM0 + n : (w < 4 ? X86_REG_YMM0_HI + n : X86_REG_ZMM0_HI + n);
			unsigned shift = (w < 2 ? w : (w < 4 ? w - 2 : w - 4)) * 64;
			auto* gv = getRegister(reg);
			assert(gv);
			return _emulator->getGlobalVariableValue(gv).IntVal.lshr(shift).trunc(64).getZExtValue();
		}

		// Same reason as setXmm: StoredValue cannot carry 128 bits, so a
		// 128-bit memory cell is written and read in halves.
		void setMemoryValue128(uint64_t addr, uint64_t hi, uint64_t lo)
		{
			llvm::GenericValue v;
			const uint64_t words[2] = {lo, hi};
			v.IntVal = llvm::APInt(128, llvm::ArrayRef<uint64_t>(words, 2));
			_emulator->setMemoryValue(addr, v);
		}

		// getMemoryValue() hands back whatever width was last written to that
		// address, and a cell nothing wrote is narrower than 128 bits -- on
		// which APInt::lshr() asserts and takes the process down, hiding every
		// test after this one. That is not hypothetical: it is what the
		// falsification run for these translations did before this widened
		// first. An unwritten cell should read as zero and fail a comparison,
		// which is what a failing test is for.
		static llvm::APInt as128(llvm::APInt v)
		{
			return v.getBitWidth() == 128 ? v : v.zextOrTrunc(128);
		}

		uint64_t memLow128(uint64_t addr)
		{
			return as128(_emulator->getMemoryValue(addr).IntVal).trunc(64).getZExtValue();
		}

		uint64_t memHigh128(uint64_t addr)
		{
			return as128(_emulator->getMemoryValue(addr).IntVal).lshr(64).trunc(64).getZExtValue();
		}

		virtual uint64_t getRegisterValueUnsigned(uint32_t reg) override
		{
			auto preg = getParentRegister(reg);
			auto* gv = getRegister(preg);
			auto val = _emulator->getGlobalVariableValue(gv).IntVal.getZExtValue();

			if (reg == preg)
			{
				return val;
			}

			if (reg == X86_REG_AH
					|| reg == X86_REG_CH
					|| reg == X86_REG_DH
					|| reg == X86_REG_BH)
			{
				val = val >> 8;
			}

			switch (_translator->getRegisterBitSize(reg))
			{
				case 1: return static_cast<bool>(val);
				case 8: return static_cast<uint8_t>(val);
				case 16: return static_cast<uint16_t>(val);
				case 32: return static_cast<uint32_t>(val);
				case 64: return static_cast<uint64_t>(val);
				default: throw std::runtime_error("Unknown reg bit size.");
			}
		}

		virtual void setRegisterValueUnsigned(uint32_t reg, uint64_t val) override
		{
			auto preg = getParentRegister(reg);
			auto* gv = getRegister(preg);
			auto* t = cast<llvm::IntegerType>(gv->getValueType());

			GenericValue v = _emulator->getGlobalVariableValue(gv);

			if (reg == preg)
			{
				bool isSigned = false;
				v.IntVal = APInt(t->getBitWidth(), val, isSigned,
						/*implicitTrunc=*/true);
				_emulator->setGlobalVariableValue(gv, v);
				return;
			}

			uint64_t old = v.IntVal.getZExtValue();

			if (reg == X86_REG_AH
					|| reg == X86_REG_CH
					|| reg == X86_REG_DH
					|| reg == X86_REG_BH)
			{
				val = val << 8;
				val = val & 0x000000000000ff00;
				old = old & 0xffffffffffff00ff;
			}
			else
			{
				switch (_translator->getRegisterBitSize(reg))
				{
					case 8:
						val = val & 0x00000000000000ff;
						old = old & 0xffffffffffffff00;
						break;
					case 16:
						val = val & 0x000000000000ffff;
						old = old & 0xffffffffffff0000;
						break;
					case 32:
						val = val & 0x00000000ffffffff;
						old = old & 0xffffffff00000000;
						break;
					case 64:
						val = val & 0xffffffffffffffff;
						old = old & 0x0000000000000000;
						break;
					default:
						throw std::runtime_error("Unknown reg bit size.");
				}
			}

			val = old | val;
			bool isSigned = false;
			v.IntVal = APInt(t->getBitWidth(), val, isSigned,
					/*implicitTrunc=*/true);
			_emulator->setGlobalVariableValue(gv, v);
			return;
		}
};

struct PrintCapstoneModeToString_x86
{
	template <class ParamType>
	std::string operator()(const TestParamInfo<ParamType>& info) const
	{
		switch (info.param)
		{
			case CS_MODE_16: return "CS_MODE_16";
			case CS_MODE_32: return "CS_MODE_32";
			case CS_MODE_64: return "CS_MODE_64";
			default: return "UNHANDLED CS_MODE";
		}
	}
};

// By default, all the test cases are run with all the modes.
// If some test case is not meant for all modes, use some of the ONLY_MODE_*,
// SKIP_MODE_* macros.
//
INSTANTIATE_TEST_SUITE_P(
		InstantiateX86WithAllModes,
		Capstone2LlvmIrTranslatorX86Tests,
		::testing::Values(CS_MODE_16, CS_MODE_32, CS_MODE_64),
		PrintCapstoneModeToString_x86());

//
// X86_INS_AAA
//

// The sub-register write mask used to be a hard-coded three-by-four table --
// i8, i16 and i32 children against i16, i32 and i64 parents, plus the row for
// the four high-byte registers -- and any pair outside it threw "Mask not
// initialized in storeRegister()".
//
// That covered every general-purpose register and nothing else, which is the
// reason XMM, YMM and ZMM are three INDEPENDENT globals in this register file
// rather than one register seen at three widths: mapping XMM's parent to YMM
// would have made every SSE store throw before it wrote anything.
//
// The mask is `~((2^childBits - 1) << offset)` at the parent's width, which is
// exactly what the table spelled out. These pin the four shapes the table had
// and the high-byte offset, so that a generalisation that gets the shift or
// the width wrong is not silently equivalent on the cases that are tested.

//
// AVX: the VEX-encoded integer instructions.
//
// YMM = YMMH:XMM. The low half is the XMM register every SSE test already
// uses; the upper half is X86_REG_YMMn_HI. setYmm/ymmWord below write and read
// the four 64-bit words of a 256-bit register through those two globals, which
// is also the check that the decomposition holds: an SSE write to xmm0 and an
// AVX read of ymm0 now see each other, and before this they did not.

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VZEROUPPER_clears_only_the_upper_halves)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x1111111111111111ULL, 0x2222222222222222ULL);
	setXmm(X86_REG_YMM0_HI, 0x3333333333333333ULL, 0x4444444444444444ULL);
	setXmm(X86_REG_XMM5, 0x5555555555555555ULL, 0x6666666666666666ULL);
	setXmm(X86_REG_YMM5_HI, 0x7777777777777777ULL, 0x8888888888888888ULL);

	emulate("vzeroupper");

	// Every upper half is zero and every lower half is untouched. Until the
	// upper halves were registers there was nothing for this to do, and it was
	// 9,336 occurrences of a call to an undefined function.
	EXPECT_EQ(0ULL, xmmLow(X86_REG_YMM0_HI));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_YMM0_HI));
	EXPECT_EQ(0ULL, xmmLow(X86_REG_YMM5_HI));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_YMM5_HI));
	EXPECT_EQ(0x2222222222222222ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x1111111111111111ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_EQ(0x6666666666666666ULL, xmmLow(X86_REG_XMM5));
	EXPECT_EQ(0x5555555555555555ULL, xmmHigh(X86_REG_XMM5));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VZEROALL_clears_the_whole_register)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x1111111111111111ULL, 0x2222222222222222ULL);
	setXmm(X86_REG_YMM0_HI, 0x3333333333333333ULL, 0x4444444444444444ULL);

	emulate("vzeroall");

	EXPECT_EQ(0ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmLow(X86_REG_YMM0_HI));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_YMM0_HI));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVDQU_ymm_moves_all_256_bits)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x1111111111111111ULL, 0x2222222222222222ULL);
	setXmm(X86_REG_YMM1_HI, 0x3333333333333333ULL, 0x4444444444444444ULL);

	emulate("vmovdqu ymm0, ymm1");

	EXPECT_EQ(0x2222222222222222ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x1111111111111111ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_EQ(0x4444444444444444ULL, xmmLow(X86_REG_YMM0_HI));
	EXPECT_EQ(0x3333333333333333ULL, xmmHigh(X86_REG_YMM0_HI));
	EXPECT_NO_VALUE_CALLED();
}

// A VEX-encoded 128-bit write ZEROES the upper half of its destination; a
// legacy SSE write leaves it alone. That difference is the entire reason
// vzeroupper exists, and it is the one thing this can get wrong without
// producing a visibly odd value -- the low half is right either way, and the
// upper only matters to the next 256-bit read.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, VEX_128bit_write_zeroes_the_upper_half)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x1111111111111111ULL, 0x2222222222222222ULL);
	setXmm(X86_REG_YMM0_HI, 0xdeadbeefdeadbeefULL, 0xcafecafecafecafeULL);

	emulate("vmovdqu xmm0, xmm1");

	EXPECT_EQ(0x2222222222222222ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x1111111111111111ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmLow(X86_REG_YMM0_HI));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_YMM0_HI));
	EXPECT_NO_VALUE_CALLED();
}

// The companion: a legacy SSE write to the same register must NOT disturb the
// upper half. If it did, vzeroupper would be pointless and every mixed
// SSE/AVX sequence would lose data.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, legacy_SSE_write_leaves_the_upper_half_alone)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x1111111111111111ULL, 0x2222222222222222ULL);
	setXmm(X86_REG_YMM0_HI, 0xdeadbeefdeadbeefULL, 0xcafecafecafecafeULL);

	emulate("movdqu xmm0, xmm1");

	EXPECT_EQ(0x2222222222222222ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xdeadbeefdeadbeefULL, xmmHigh(X86_REG_YMM0_HI));
	EXPECT_EQ(0xcafecafecafecafeULL, xmmLow(X86_REG_YMM0_HI));
	EXPECT_NO_VALUE_CALLED();
}

// vpcmpeqb is 23,150 occurrences across its two signatures, and it is how an
// AVX2 string routine asks which of thirty-two bytes matches. A lane of the
// result is all ones or all zeroes.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPEQB_ymm)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x00112233aabbccddULL);
	setXmm(X86_REG_YMM1_HI, 0xffffffffffffffffULL, 0x0011223344556677ULL);
	setXmm(X86_REG_XMM2, 0x0000000000000000ULL, 0x0011223300000000ULL);
	setXmm(X86_REG_YMM2_HI, 0x0000000000000000ULL, 0x0011223344556677ULL);

	emulate("vpcmpeqb ymm0, ymm1, ymm2");

	// Low 128: the top four bytes match, the bottom four do not; the high
	// quadword is 0 against 0, so every one of those bytes matches.
	EXPECT_EQ(0xffffffff00000000ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xffffffffffffffffULL, xmmHigh(X86_REG_XMM0));
	// Upper 128: the low quadword matches exactly; the high one does not.
	EXPECT_EQ(0xffffffffffffffffULL, xmmLow(X86_REG_YMM0_HI));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_YMM0_HI));
	EXPECT_NO_VALUE_CALLED();
}

// vpaddb and vpaddd differ only in whether a carry crosses a byte boundary, so
// any operand whose lanes do not carry gives the same answer for both. Every
// lane here carries.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPADDB_ymm_does_not_carry_between_lanes)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0xffffffffffffffffULL, 0xffffffffffffffffULL);
	setXmm(X86_REG_YMM1_HI, 0xffffffffffffffffULL, 0xffffffffffffffffULL);
	setXmm(X86_REG_XMM2, 0x0101010101010101ULL, 0x0101010101010101ULL);
	setXmm(X86_REG_YMM2_HI, 0x0101010101010101ULL, 0x0101010101010101ULL);

	emulate("vpaddb ymm0, ymm1, ymm2");

	EXPECT_EQ(0ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmLow(X86_REG_YMM0_HI));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_YMM0_HI));
	EXPECT_NO_VALUE_CALLED();
}

// ANDN inverts the FIRST operand, not the second. With these operands the two
// readings give 0x00ff.. and 0xff00.., which is as far apart as they get.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPANDN_inverts_the_first_operand)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0xffffffff00000000ULL);
	setXmm(X86_REG_YMM1_HI, 0ULL, 0ULL);
	setXmm(X86_REG_XMM2, 0x0000000000000000ULL, 0xffffffffffffffffULL);
	setXmm(X86_REG_YMM2_HI, 0ULL, 0ULL);

	emulate("vpandn ymm0, ymm1, ymm2");

	EXPECT_EQ(0x00000000ffffffffULL, xmmLow(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

// vpmovmskb collects the TOP bit of each byte lane -- thirty-two of them from
// a YMM source -- into a general-purpose register. Taking the LOW bit instead
// is the quiet way to get this wrong: for the all-ones and all-zeroes lanes a
// compare produces, the two readings agree exactly, so these lanes are
// deliberately neither.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPMOVMSKB_ymm_takes_the_top_bit_of_each_lane)
{
	ONLY_MODE_64;

	// Bytes, from the bottom: 01 80 01 80 ... -- low bit set on the even
	// lanes, top bit set on the odd ones.
	setXmm(X86_REG_XMM1, 0x8001800180018001ULL, 0x8001800180018001ULL);
	setXmm(X86_REG_YMM1_HI, 0x8001800180018001ULL, 0x8001800180018001ULL);

	emulate("vpmovmskb eax, ymm1");

	// Every odd lane, all thirty-two of them: 0xaaaaaaaa. Reading the low bit
	// would answer 0x55555555.
	EXPECT_EQ(0xaaaaaaaaULL, getRegisterValueUnsigned(X86_REG_EAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPMOVMSKB_xmm_is_sixteen_bits)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x8001800180018001ULL, 0x8001800180018001ULL);

	emulate("vpmovmskb eax, xmm1");

	EXPECT_EQ(0xaaaaULL, getRegisterValueUnsigned(X86_REG_EAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPMINUB_ymm_is_unsigned)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0ULL, 0x00000000000000ffULL);
	setXmm(X86_REG_YMM1_HI, 0ULL, 0ULL);
	setXmm(X86_REG_XMM2, 0ULL, 0x0000000000000001ULL);
	setXmm(X86_REG_YMM2_HI, 0ULL, 0ULL);

	emulate("vpminub ymm0, ymm1, ymm2");

	// 0xff is 255 unsigned, so 1 is the minimum. A signed reading would keep
	// 0xff, since it is -1.
	EXPECT_EQ(0x0000000000000001ULL, xmmLow(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, sub_register_write_keeps_the_rest_of_its_parent)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1122334455667788},
	});

	emulate("mov al, 0xff");

	// Only the bottom byte changes.
	EXPECT_EQ(0x11223344556677ff, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, high_byte_write_is_offset_by_eight)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1122334455667788},
	});

	emulate("mov ah, 0xff");

	// The SECOND byte, not the first: ah is bits 15..8, and the shift by 8 is
	// the one thing the general mask cannot derive from the widths alone.
	EXPECT_EQ(0x112233445566ff88, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, word_write_keeps_the_rest_of_its_parent)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1122334455667788},
	});

	emulate("mov ax, 0xbeef");

	EXPECT_EQ(0x1122334455660000 | 0xbeef, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_VALUE_CALLED();
}

// The one sub-register write that is NOT a merge: x86-64 zero-extends a 32-bit
// write to the whole 64-bit register, and storeRegister has a case for it
// above the masking path. A generalisation that reached the mask here would
// answer 0x11223344deadbeef.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, dword_write_zero_extends_rather_than_merging)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1122334455667788},
	});

	emulate("mov eax, 0xdeadbeef");

	EXPECT_EQ(0x00000000deadbeef, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAA_decimal_carry)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xa},
		{X86_REG_AH, 0x4},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});

	emulate("aaa");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AH, X86_REG_AF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x5},
		{X86_REG_AL, 0x0},
		{X86_REG_AF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAA_decimal_carry_af)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x0},
		{X86_REG_AH, 0x4},
		{X86_REG_AF, true},
		{X86_REG_CF, false},
	});

	emulate("aaa");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AH, X86_REG_AF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x5},
		{X86_REG_AL, 0x6},
		{X86_REG_AF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAA_no_decimal_carry)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x2},
		{X86_REG_AH, 0x4},
		{X86_REG_AF, false},
		{X86_REG_CF, true},
	});

	emulate("aaa");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AH, X86_REG_AF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x4ULL},
		{X86_REG_AL, 0x2ULL},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_AAS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAS_decimal_carry)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xa},
		{X86_REG_AH, 0x4},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});

	emulate("aas");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AH, X86_REG_AF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x3ULL},
		{X86_REG_AL, 0x4ULL},
		{X86_REG_AF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAS_decimal_carry_af)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x0},
		{X86_REG_AH, 0x4},
		{X86_REG_AF, true},
		{X86_REG_CF, false},
	});

	emulate("aas");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AH, X86_REG_AF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x3},
		{X86_REG_AL, 0xa}, // (0x0 - 0x6) & 0xf = 0xa
		{X86_REG_AF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAS_no_decimal_carry)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x2},
		{X86_REG_AH, 0x4},
		{X86_REG_AF, false},
		{X86_REG_CF, true},
	});

	emulate("aas");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AH, X86_REG_AF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x4ULL},
		{X86_REG_AL, 0x2ULL},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_DAA
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DAA_decimal_carry)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xa},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});

	emulate("daa");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AF, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x10ULL},
		{X86_REG_AF, true},
		{X86_REG_CF, false},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DAA_decimal_carry_cf)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xa},
		{X86_REG_AF, false},
		{X86_REG_CF, true},
	});

	emulate("daa");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AF, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x70ULL},
		{X86_REG_AF, true},
		{X86_REG_CF, true},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DAA_decimal_carry_af_cf)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xf0},
		{X86_REG_AF, true},
		{X86_REG_CF, true},
	});

	emulate("daa");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AF, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x56ULL}, // 0xf0 + 0x6 + 0x60 = 0x156 (overflow) = 0x56
		{X86_REG_AF, true},
		{X86_REG_CF, true},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DAA_no_decimal_carry)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xf0},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});

	emulate("daa");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AF, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x50ULL}, // 0xf0 + 0x60 = 0x150 (overflow) = 0x50
		{X86_REG_AF, false},
		{X86_REG_CF, true},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_DAS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DAS_decimal_carry)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xa},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});

	emulate("das");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AF, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x4ULL}, // 0xa - 0x6 - 0x60 = 0xa4 (negative -92)
		{X86_REG_AF, true},
		{X86_REG_CF, false},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DAS_decimal_carry_cf)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xa},
		{X86_REG_AF, false},
		{X86_REG_CF, true},
	});

	emulate("das");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AF, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0xa4ULL}, // 0xa - 0x6 - 0x60 = 0xa4 (negative -92)
		{X86_REG_AF, true},
		{X86_REG_CF, true},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DAS_decimal_carry_af_cf)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xf0},
		{X86_REG_AF, true},
		{X86_REG_CF, true},
	});

	emulate("das");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AF, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x8aULL}, // 0xf0 - 0x6 - 0x60 = 0x8a
		{X86_REG_AF, true},
		{X86_REG_CF, true},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DAS_no_decimal_carry)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xf0},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});

	emulate("das");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_AF, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x90ULL}, // 0xf0 - 0x60 = 0x90
		{X86_REG_AF, false},
		{X86_REG_CF, true},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_AAD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAD_default_val)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x80},
		{X86_REG_AH, 0x10},
	});

	emulate("aad");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x0ULL}, // 0x0
		{X86_REG_AL, 0x20ULL}, // 0x80 + (0x10 * 0xa) = 0x120 (overflow) = 0x20
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_PF, false},
	}); // according to Ollydbg, CF, OF are also set
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAD_imm_val)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x80},
		{X86_REG_AH, 0x10},
	});

	emulate("aad 0x2");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x0ULL}, // 0x0
		{X86_REG_AL, 0xa0ULL}, // 0x80 + (0x10 * 0x2) = 0xa0
		{X86_REG_SF, true},
		{X86_REG_ZF, false},
		{X86_REG_PF, true},
	}); // according to Ollydbg, CF, OF are also set
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAD_default_val_overflow)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x80},
		{X86_REG_AH, 0x10},
	});

	emulate("aad");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x0ULL}, // 0x0
		{X86_REG_AL, 0x20ULL}, // 0x80 + (0x10 * 0xa) = 0x120 (overflow) = 0x20
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_AAM
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAM_flags_false)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x80},
		{X86_REG_AH, 0x12}, // this should be overwritten
	});

	emulate("aam");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0xcULL}, // 0x80 / 0xa
		{X86_REG_AL, 0x8ULL}, // 0x80 % 0xa
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAM_pf)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x81},
		{X86_REG_AH, 0x12}, // this should be overwritten
	});

	emulate("aam");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0xcULL}, // 0x81 / 0xa
		{X86_REG_AL, 0x9ULL}, // 0x81 % 0xa
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_PF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAM_zf_pf)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0x82},
		{X86_REG_AH, 0x12}, // this should be overwritten
	});

	emulate("aam");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0xdULL}, // 0x82 / 0xa
		{X86_REG_AL, 0x0ULL}, // 0x82 % 0xa
		{X86_REG_SF, false},
		{X86_REG_ZF, true},
		{X86_REG_PF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AAM_imm)
{
	SKIP_MODE_64; // undef op

	setRegisters({
		{X86_REG_AL, 0xf5},
		{X86_REG_AH, 0x12}, // this should be overwritten
	});

	emulate("aam 0x23");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x7ULL}, // 0xf5 / 0x23
		{X86_REG_AL, 0x0ULL}, // 0xf5 % 0x23
		{X86_REG_SF, false},
		{X86_REG_ZF, true},
		{X86_REG_PF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_ADC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADC_reg16_imm16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x1200},
		{X86_REG_CF, 0x1},
	});

	emulate("adc cx, 0x34");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x1235ULL},
		{X86_REG_PF, true},
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADC_reg32_imm32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0xffffff00},
		{X86_REG_CF, 0x1},
	});

	emulate("adc ecx, 0xff");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0x0ULL},
		{X86_REG_PF, true},
		{X86_REG_SF, false},
		{X86_REG_ZF, true},
		{X86_REG_OF, false},
		{X86_REG_AF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADC_reg64_imm64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RCX, 0xffffffffffff0000},
		{X86_REG_CF, 0x0},
	});

	emulate("adc rcx, 0xffff");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RCX, 0xffffffffffffffffULL},
		{X86_REG_PF, true},
		{X86_REG_SF, true},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_ADCX
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADCX_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0xffffff00},
		{X86_REG_EAX, 0xff},
		{X86_REG_CF, 0x1},
	});

	emulate("adcx ecx, eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX, X86_REG_EAX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0x0ULL},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_ADOX
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADOX_reg32_ref32)
{
	SKIP_MODE_16

	setRegisters({
		{X86_REG_ECX, 0xffffff00},
		{X86_REG_EAX, 0xff},
		{X86_REG_OF, 0x1},
	});

	emulate("adox ecx, eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX, X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0x0ULL},
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_ADD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADD_reg8_imm8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DL, 0xf0},
	});

	emulate("add dl, 0x12");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DL, 0x2ULL},
		{X86_REG_PF, false},
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADD_reg16_mem16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0xff00},
	});
	setMemory({
		{0x1234, 0xff_w},
	});

	emulate("add dx, [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DX, 0xffffULL},
		{X86_REG_PF, true},
		{X86_REG_SF, true},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADD_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12340000},
		{X86_REG_ECX, 0x00005678},
	});

	emulate("add eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_PF, true},
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADD_reg32_reg32_bin)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_EAX, 0x12340000},
		{X86_REG_ECX, 0x00005678},
	});

	emulate_bin("01 c8");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_PF, true},
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADD_reg64_imm32)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RDX, 0x100},
	});

	emulate("add rdx, -0x200");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, -0x100}, // 0xffffffffffffff00
		{X86_REG_PF, true},
		{X86_REG_SF, true},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_XADD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XADD_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12340000},
		{X86_REG_ECX, 0x00005678},
	});

	emulate("xadd eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_ECX, 0x12340000},
		{X86_REG_PF, true},
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_AND
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AND_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_ECX, 0x10305070},
	});

	emulate("and eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x10305070},
		{X86_REG_PF, false},
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_AND_reg32_reg32_zf)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_ECX, 0x00000000},
	});

	emulate("and eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x0},
		{X86_REG_PF, true},
		{X86_REG_SF, false},
		{X86_REG_ZF, true},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_TEST
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_TEST_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_ECX, 0x10305070},
	});

	emulate("test eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_PF, false},
		{X86_REG_SF, false},
		{X86_REG_ZF, false},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_TEST_reg32_reg32_zf)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_ECX, 0x00000000},
	});

	emulate("test eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_PF, true},
		{X86_REG_SF, false},
		{X86_REG_ZF, true},
		{X86_REG_OF, false},
		{X86_REG_AF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_BSF
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSF_reg16_reg16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 1<<5 | 1<<10},
	});

	emulate("bsf ax, dx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_DX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 5}, // least significant set bit
		{X86_REG_ZF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSF_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 1<<20 | 1<<25},
	});

	emulate("bsf eax, edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 20}, // least significant set bit
		{X86_REG_ZF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSF_reg32_reg32_src_zero)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 1234}, // will not be changed
		{X86_REG_EDX, 0},
	});

	emulate("bsf eax, edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 1234},
		{X86_REG_ZF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSF_reg64_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RDX, 1ULL<<40 | 1ULL<<50},
	});

	emulate("bsf rax, rdx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 40}, // least significant set bit
		{X86_REG_ZF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_BSR
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSR_reg16_reg16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 1<<5 | 1<<10},
	});

	emulate("bsr ax, dx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_DX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 10}, // most significant set bit
		{X86_REG_ZF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSR_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 1<<20 | 1<<25},
	});

	emulate("bsr eax, edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 25}, // most significant set bit
		{X86_REG_ZF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSR_reg32_reg32_src_zero)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 1234}, // will not be changed
		{X86_REG_EDX, 0},
	});

	emulate("bsr eax, edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 1234},
		{X86_REG_ZF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSR_reg64_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RDX, 1ULL<<40 | 1ULL<<50},
	});

	emulate("bsr rax, rdx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 50}, // most significant set bit
		{X86_REG_ZF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_BSWAP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSWAP_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0x12345678},
	});

	emulate("bswap edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EDX, 0x78563412},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BSWAP_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RDX, 0x0123456789abcdef},
	});

	emulate("bswap rdx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0xefcdab8967452301},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_BT
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BT_r32_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_EDX, 0xf0f0f0f0},
	});

	emulate("bt edx, 0x2");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BT_r64_true)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_RDX, 0xf0f0f0f0},
	});

	emulate("bt rdx, 0x46"); // 0x46 & 0x1f = 0x6

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_BTC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BTC_r32_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_EDX, 0x000000f0},
	});

	emulate("btc edx, 0x2");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_EDX, 0x000000f4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BTC_r64_true)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_RDX, 0x000000f0},
	});

	emulate("btc rdx, 0x46"); // 0x46 & 0x1f = 0x6

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0x000000b0},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_BTR
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BTR_r32_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_EDX, 0x000000f0},
	});

	emulate("btr edx, 0x2");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_EDX, 0x000000f0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BTR_r64_true)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_RDX, 0x000000f0},
	});

	emulate("btr rdx, 0x46"); // 0x46 & 0x1f = 0x6

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0x000000b0},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_BTS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BTS_r32_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_EDX, 0x000000f0},
	});

	emulate("bts edx, 0x2");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_EDX, 0x000000f4},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BTS_r64_true)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_RDX, 0x000000f0},
	});

	emulate("bts rdx, 0x46"); // 0x46 & 0x1f = 0x6

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0x000000f0},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CBW
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CBW_sign)
{
	SKIP_MODE_16; // For some reason, 16 bit mode does not like this.

	setRegisters({
		{X86_REG_AX, 0x12f0},
	});

	emulate("cbw");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0xfff0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CBW_no_sign)
{
	SKIP_MODE_16; // For some reason, 16 bit mode does not like this.

	setRegisters({
		{X86_REG_AX, 0x120f},
	});

	emulate("cbw");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x000f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CWDE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CWDE_sign)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234f000},
	});

	emulate("cwde");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xfffff000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CWDE_no_sign)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12340fff},
	});

	emulate("cwde");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x00000fff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CDQE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CDQE_sign)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x12345678f0000000},
	});

	emulate("cdqe");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0xfffffffff0000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CDQE_no_sign)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x123456780000000f},
	});

	emulate("cdqe");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x000000000000000f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CWD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CWD_sign)
{
	SKIP_MODE_16; // For some reason, 16 bit mode does not like this.

	setRegisters({
		{X86_REG_AX, 0xf000},
		{X86_REG_DX, 0x1234},
	});

	emulate("cwd");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DX, 0xffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CWD_no_sign)
{
	SKIP_MODE_16; // For some reason, 16 bit mode does not like this.

	setRegisters({
		{X86_REG_AX, 0x000f},
		{X86_REG_DX, 0x1234},
	});

	emulate("cwd");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DX, 0x0000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CDQ
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CDQ_sign)
{
	SKIP_MODE_16; // For some reason, 16 bit mode does not like this.

	setRegisters({
		{X86_REG_EAX, 0xf0000000},
		{X86_REG_EDX, 0x12345678},
	});

	emulate("cdq");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EDX, 0xffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CDQ_no_sign)
{
	SKIP_MODE_16; // For some reason, 16 bit mode does not like this.

	setRegisters({
		{X86_REG_EAX, 0x0000000f},
		{X86_REG_EDX, 0x12345678},
	});

	emulate("cdq");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DX, 0x00000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CQO
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CQO_sign)
{
	ONLY_MODE_64; // For some reason, 16 bit mode does not like this.

	setRegisters({
		{X86_REG_RAX, 0xf000000000000000},
		{X86_REG_RDX, 0x0123456789abcdef},
	});

	emulate("cqo");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0xffffffffffffffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CQO_no_sign)
{
	ONLY_MODE_64; // For some reason, 16 bit mode does not like this.

	setRegisters({
		{X86_REG_RAX, 0x000000000000000f},
		{X86_REG_RDX, 0x0123456789abcdef},
	});

	emulate("cqo");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0x0000000000000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CLC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CLC_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
	});

	emulate("clc");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CLC_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
	});

	emulate("clc");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CLD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CLD_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DF, true},
	});

	emulate("cld");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CLD_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DF, false},
	});

	emulate("cld");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMC_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
	});

	emulate("cmc");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMC_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
	});

	emulate("cmc");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMPXCHG
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG_r8_eq)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x12},
		{X86_REG_CL, 0x12},
		{X86_REG_DL, 0x34},
		{X86_REG_ZF, false},
	});

	emulate("cmpxchg cl, dl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_CL, X86_REG_DL, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x12},
		{X86_REG_CL, 0x34},
		{X86_REG_ZF, true},
		{X86_REG_CF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_OF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG_r8_ne)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x12},
		{X86_REG_CL, 0x34},
		{X86_REG_DL, 0x56},
		{X86_REG_ZF, true},
	});

	emulate("cmpxchg cl, dl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_CL, X86_REG_DL, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x34},
		{X86_REG_CL, 0x34},
		{X86_REG_ZF, false},
		{X86_REG_CF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_OF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG_r16_eq)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1234},
		{X86_REG_CX, 0x1234},
		{X86_REG_DX, 0x5678},
		{X86_REG_ZF, false},
	});

	emulate("cmpxchg cx, dx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_DX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1234},
		{X86_REG_CX, 0x5678},
		{X86_REG_ZF, true},
		{X86_REG_CF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_OF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG_r16_ne)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1234},
		{X86_REG_CX, 0x5678},
		{X86_REG_DX, 0x90ab},
		{X86_REG_ZF, true},
	});

	emulate("cmpxchg cx, dx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_DX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x5678},
		{X86_REG_CX, 0x5678},
		{X86_REG_ZF, false},
		{X86_REG_CF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_OF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG_r32_eq)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
		{X86_REG_ECX, 0x1234},
		{X86_REG_EDX, 0x5678},
		{X86_REG_ZF, false},
	});

	emulate("cmpxchg ecx, edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX, X86_REG_EDX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x1234},
		{X86_REG_ECX, 0x5678},
		{X86_REG_ZF, true},
		{X86_REG_CF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_OF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG_r32_ne)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
		{X86_REG_ECX, 0x5678},
		{X86_REG_EDX, 0x90ab},
		{X86_REG_ZF, true},
	});

	emulate("cmpxchg ecx, edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX, X86_REG_EDX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x5678},
		{X86_REG_ECX, 0x5678},
		{X86_REG_ZF, false},
		{X86_REG_CF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_OF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG_r64_eq)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1234},
		{X86_REG_RCX, 0x1234},
		{X86_REG_RDX, 0x5678},
		{X86_REG_ZF, false},
	});

	emulate("cmpxchg rcx, rdx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x1234},
		{X86_REG_RCX, 0x5678},
		{X86_REG_ZF, true},
		{X86_REG_CF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_OF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG_r64_ne)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1234},
		{X86_REG_RCX, 0x5678},
		{X86_REG_RDX, 0x90ab},
		{X86_REG_ZF, true},
	});

	emulate("cmpxchg rcx, rdx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x5678},
		{X86_REG_RCX, 0x5678},
		{X86_REG_ZF, false},
		{X86_REG_CF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_OF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMPXCHG8B
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG8B_eq)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0x01234567},
		{X86_REG_EAX, 0x89abcdef},
		{X86_REG_ECX, 0x11111111},
		{X86_REG_EBX, 0x22222222},
		{X86_REG_ZF, false},

	});
	setMemory({
		{0x1000, 0x0123456789abcdef_qw},
	});

	emulate("cmpxchg8b [0x1000]");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX, X86_REG_EAX, X86_REG_ECX, X86_REG_EBX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, true},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x1111111122222222_qw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMPXCHG8B_ne)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0x11111111},
		{X86_REG_EAX, 0x22222222},
		{X86_REG_ECX, 0x11111111},
		{X86_REG_EBX, 0x22222222},
		{X86_REG_ZF, true},

	});
	setMemory({
		{0x1000, 0x0123456789abcdef_qw},
	});

	emulate("cmpxchg8b [0x1000]");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EDX, 0x01234567},
		{X86_REG_EAX, 0x89abcdef},
		{X86_REG_ZF, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMPXCHG16B
//

// TODO: this is the same thing as X86_INS_CMPXCHG8B but on 128 bit integers.
// Right now, StoredValue can not work with such a big numbers. Add this test
// when it is refactored to use llvm::APInt.

//
// X86_INS_DEC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DEC_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x0},
	});

	emulate("dec ax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0xffff},
		{X86_REG_ZF, false},
		{X86_REG_PF, true},
		{X86_REG_AF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DEC_r32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
	});

	emulate("dec eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x1233},
		{X86_REG_ZF, false},
		{X86_REG_PF, true},
		{X86_REG_AF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DEC_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1},
	});

	emulate("dec rax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x0},
		{X86_REG_ZF, true},
		{X86_REG_PF, true},
		{X86_REG_AF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_INC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_INC_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0xffff},
	});

	emulate("inc ax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x0},
		{X86_REG_ZF, true},
		{X86_REG_PF, true},
		{X86_REG_AF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_INC_r32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
	});

	emulate("inc eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x1235},
		{X86_REG_ZF, false},
		{X86_REG_PF, true},
		{X86_REG_AF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_INC_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x0},
	});

	emulate("inc rax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x1},
		{X86_REG_ZF, false},
		{X86_REG_PF, false},
		{X86_REG_AF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_DIV
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DIV_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0x0f},
		{X86_REG_AX, 0x123},
	});

	emulate("div cl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x6}, // remainder
		{X86_REG_AL, 0x13}, // quotient
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DIV_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x1234},
		{X86_REG_DX, 0x12},
		{X86_REG_AX, 0x345},
	});

	emulate("div cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_DX, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DX, 0x5e1}, // remainder
		{X86_REG_AX, 0xfd}, // quotient
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DIV_r32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0x1234},
		{X86_REG_EDX, 0x12},
		{X86_REG_EAX, 0x345},
	});

	emulate("div ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX, X86_REG_EDX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EDX, 0xb1d}, // remainder
		{X86_REG_EAX, 0xfd24b2}, // quotient
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DIV_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RCX, 0x1234},
		{X86_REG_RDX, 0x12},
		{X86_REG_RAX, 0x345},
	});

	emulate("div rcx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RCX, X86_REG_RDX, X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0x7d}, // remainder
		{X86_REG_RAX, 0xfd24b26e4f8bfa}, // quotient
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_IDIV
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IDIV_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0x0f},
		{X86_REG_AX, 0x123},
	});

	emulate("idiv cl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x6}, // remainder
		{X86_REG_AL, 0x13}, // quotient
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IDIV_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x1234},
		{X86_REG_DX, 0x12},
		{X86_REG_AX, 0x345},
	});

	emulate("idiv cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_DX, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DX, 0x5e1}, // remainder
		{X86_REG_AX, 0xfd}, // quotient
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IDIV_r32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0x1234},
		{X86_REG_EDX, 0x12},
		{X86_REG_EAX, 0x345},
	});

	emulate("idiv ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX, X86_REG_EDX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EDX, 0xb1d}, // remainder
		{X86_REG_EAX, 0xfd24b2}, // quotient
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IDIV_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RCX, 0x1234},
		{X86_REG_RDX, 0x12},
		{X86_REG_RAX, 0x345},
	});

	emulate("idiv rcx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RCX, X86_REG_RDX, X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0x7d}, // remainder
		{X86_REG_RAX, 0xfd24b26e4f8bfa}, // quotient
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Batch AA -- the forms whose answer does not fit in one register.
//
// Every IDIV test above this point divides by a POSITIVE number, which is why
// none of them noticed that the divisor was zero-extended. Every expected
// value below was executed on the host CPU and read back, not worked out on
// paper; see scripts/ci/x86_wide_oracle.c.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IDIV_r64_negative_divisor)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RCX, 0xfffffffffffffffd}, // -3
		{X86_REG_RDX, 0x0},
		{X86_REG_RAX, 0xa}, // 10
	});

	emulate("idiv rcx");

	// 10 / -3 is -3 remainder 1. Zero-extending the divisor made it
	// 18446744073709551613, so the quotient came out 0 and the remainder 10.
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RCX, X86_REG_RDX, X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RDX, 0x1},
		{X86_REG_RAX, 0xfffffffffffffffd},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IDIV_r32_negative_divisor)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0xffffedcc}, // -0x1234
		{X86_REG_EDX, 0x12},
		{X86_REG_EAX, 0x345},
	});

	emulate("idiv ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX, X86_REG_EDX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EDX, 0xb1d},
		{X86_REG_EAX, 0xff02db4e},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IDIV_r8_negative_divisor)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0xf1}, // -15
		{X86_REG_AX, 0x123},
	});

	emulate("idiv cl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0x6},  // remainder
		{X86_REG_AL, 0xed}, // quotient, -19
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r8_overflows_into_the_sign_bit)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0x08},
		{X86_REG_AL, 0x10},
	});

	emulate("imul cl");

	// 16 * 8 is 128. The high half is zero, but 128 does not fit in a signed
	// byte, so the hardware sets both flags. The old test -- "the high half
	// is neither zero nor all ones" -- set neither.
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL, X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x80},
		{X86_REG_OF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r8_high_all_ones_low_positive)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0x03}, {X86_REG_AL, 0xd5}, // -43
	});

	emulate("imul cl");

	// -43 * 3 is -129: the high half IS all ones, and it still does not fit
	// in a signed byte. This is the other direction of the same mistake.
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL, X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0xff7f},
		{X86_REG_OF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r32_high_all_ones_low_positive)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0x3}, {X86_REG_EAX, 0xd5555555}, // -715827883
	});

	emulate("imul ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x7fffffff},
		{X86_REG_EDX, 0xffffffff},
		{X86_REG_OF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MUL_r8_unsigned_high_zero_is_no_overflow)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0x08},
		{X86_REG_AL, 0x10},
	});

	emulate("mul cl");

	// The same operands as the IMUL test above. Unsigned, 128 fits in a byte
	// pair with a zero high half, so neither flag is set -- the IMUL fix must
	// not leak into MUL.
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL, X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x80},
		{X86_REG_OF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHLD_r32_count_masks_to_five_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0xaaaaaaaa12345678},
		{X86_REG_RCX, 0x00000000abcdef21},
	});

	emulate("shld eax, ecx, cl");

	// cl is 0x21. A 32-bit operand masks the count to five bits, so this is a
	// shift by ONE. Masking by the processor mode instead gave 33, and a
	// shift of an i32 by 33 is poison.
	EXPECT_EQ(0x000000002468acf1ULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHLD_r32_count_of_32_is_a_count_of_zero)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0xaaaaaaaa12345678},
		{X86_REG_RCX, 0x00000000abcdef20},
	});

	emulate("shld eax, ecx, cl");

	// cl is 0x20. Five bits of it is zero, so nothing moves. Six bits of it
	// is 32, which runs the body and folds ecx in.
	//
	// The test above, with cl=0x21, does NOT catch that: the emulator reduces
	// a shift amount modulo the width exactly as the hardware does, so a
	// shift by 33 of an i32 gives the same answer as a shift by 1 and the
	// wrong mask is invisible. It is not invisible in the IR -- `shl i32 x,
	// 33` is poison, and the optimiser is entitled to do anything with it --
	// but a test has to fail, not merely be right for a bad reason. This
	// count is the one that fails.
	EXPECT_EQ(0x0000000012345678ULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHRD_r32_count_masks_to_zero)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0xaaaaaaaa12345678},
		{X86_REG_RCX, 0x00000000abcdef60},
	});

	emulate("shrd eax, ecx, cl");

	// cl is 0x60: masked to five bits it is zero, so the value does not move.
	// The register is still WRITTEN, though, and a 32-bit write clears the
	// top half of RAX.
	EXPECT_EQ(0x0000000012345678ULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHL_r32_zero_count_still_clears_the_top_half)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0xaaaaaaaa12345678},
		{X86_REG_RCX, 0x0},
	});

	emulate("shl eax, cl");

	EXPECT_EQ(0x0000000012345678ULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROL_r32_count_masks_to_zero_still_writes)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0xaaaaaaaa12345678},
		{X86_REG_RCX, 0x20},
	});

	emulate("rol eax, cl");

	// A rotate by 32 of a 32-bit value is the identity, and the count masks
	// to zero besides -- but the destination is written either way.
	EXPECT_EQ(0x0000000012345678ULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_JMP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JMP_absolute)
{
	ALL_MODES;

	emulate("jmp 0x1234");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JMP_reg16)
{
	SKIP_MODE_64;

	setRegisters({
		{X86_REG_AX, 0x5678},
	});

	emulate("jmp ax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x5678}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JMP_reg32)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
	});

	emulate("jmp eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x12345678}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JMP_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x12345678},
	});

	emulate("jmp rax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x12345678}},
	});
}

//
// X86_INS_LJMP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LJMP_absolute)
{
	SKIP_MODE_64;

	emulate("ljmp  0x1234:0x5678");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CS, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getBranchFunction(), {0x5678}},
	});
}

//
// X86_INS_CALL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CALL_absolute_16)
{
	ONLY_MODE_16;

	setRegisters({
		{X86_REG_SP, 0x100},
	});

	emulate("call 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SP, 0xfe}, // 0x100 - 0x2
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xfe, 0x1006_w} // 0x100 - 0x2, 0x1000 (addr) + 0x6 (size) = next addr
	});
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CALL_absolute_32)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_ESP, 0x100},
	});

	emulate("call 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ESP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ESP, 0xfc}, // 0x100 - 0x4
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xfc, 0x1005_dw} // 0x100 - 0x4, 0x1000 (addr) + 0x5 (size) = next addr
	});
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CALL_absolute_64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RSP, 0x100},
	});

	emulate("call 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RSP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RSP, 0xf8}, // 0x100 - 0x8
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xf8, 0x1005_qw} // 0x100 - 0x8, 0x1000 (addr) + 0x5 (size) = next addr
	});
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CALL_reg16)
{
	ONLY_MODE_16;

	setRegisters({
		{X86_REG_SP, 0x100},
		{X86_REG_CX, 0x1234},
	});

	emulate("call cx", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SP, X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SP, 0xfe}, // 0x100 - 0x2
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xfe, 0x1002_w} // 0x100 - 0x2, 0x1000 (addr) + 0x2 (size) = next addr
	});
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CALL_reg32)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_ESP, 0x100},
		{X86_REG_ECX, 0x1234},
	});

	emulate("call ecx", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ESP, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ESP, 0xfc}, // 0x100 - 0x4
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xfc, 0x1002_dw} // 0x100 - 0x4, 0x1000 (addr) + 0x2 (size) = next addr
	});
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CALL_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RSP, 0x100},
		{X86_REG_RCX, 0x1234},
	});

	emulate("call rcx", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RSP, X86_REG_RCX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RSP, 0xf8}, // 0x100 - 0x8
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xf8, 0x1002_qw} // 0x100 - 0x8, 0x1000 (addr) + 0x2 (size) = next addr
	});
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCallFunction(), {0x1234}},
	});
}

//
// X86_INS_LAHF
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LAHF)
{
	SKIP_MODE_64;

	setRegisters({
		{X86_REG_AH, 0x12}, // will be overwritten
		{X86_REG_SF, true},
		{X86_REG_ZF, true},
		{X86_REG_AF, true},
		{X86_REG_PF, true},
		{X86_REG_CF, true},
	});

	emulate("lahf");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_ZF, X86_REG_AF, X86_REG_PF, X86_REG_CF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AH, 0xd7}, // 11010111
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_LEA
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LEA_32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
		{X86_REG_EDX, 0xa},
	});

	emulate("lea ecx, [eax + edx * 8 + 64]");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0x12c4}, // 0x1234 + 0xa * 8 + 64
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LEA_64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1234},
		{X86_REG_RDX, 0xa},
	});

	emulate("lea rcx, [rax + rdx * 8 + 64]");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_RDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RCX, 0x12c4}, // 0x1234 + 0xa * 8 + 64
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_LEAVE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LEAVE_16)
{
	ONLY_MODE_16;

	setRegisters({
		{X86_REG_SP, 0x1234},
		{X86_REG_BP, 0x5678},
	});
	setMemory({
		{0x5678, 0xffff_w},
	});

	emulate("leave");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_BP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SP, 0x567a}, // 0x5678 + 2
		{X86_REG_BP, 0xffff},
	});
	EXPECT_MEMORY_LOADED({0x5678});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LEAVE_32)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_ESP, 0x1234},
		{X86_REG_EBP, 0x5678},
	});
	setMemory({
		{0x5678, 0xffffffff_dw},
	});

	emulate("leave");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EBP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ESP, 0x567c}, // 0x5678 + 4
		{X86_REG_EBP, 0xffffffff},
	});
	EXPECT_MEMORY_LOADED({0x5678});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LEAVE_64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RSP, 0x1234},
		{X86_REG_RBP, 0x5678},
	});
	setMemory({
		{0x5678, 0xffffffffffffffff_qw},
	});

	emulate("leave");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RBP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RSP, 0x5680}, // 0x5678 + 8
		{X86_REG_RBP, 0xffffffffffffffff},
	});
	EXPECT_MEMORY_LOADED({0x5678});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_LDS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LDS_16)
{
	ONLY_MODE_16;

	setMemory({
		{0x1000, 0x1234_w},
		{0x1002, 0x90ab_w},
	});

	emulate("lds ax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DS, 0x90ab},
		{X86_REG_AX, 0x1234},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1002});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LDS_32)
{
	ONLY_MODE_32;

	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x90ab_w},
	});

	emulate("lds eax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DS, 0x90ab},
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_LES
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LES_16)
{
	ONLY_MODE_16;

	setMemory({
		{0x1000, 0x1234_w},
		{0x1002, 0x90ab_w},
	});

	emulate("les ax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ES, 0x90ab},
		{X86_REG_AX, 0x1234},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1002});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LES_32)
{
	ONLY_MODE_32;

	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x90ab_w},
	});

	emulate("les eax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ES, 0x90ab},
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_LFS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LFS_16)
{
	ONLY_MODE_16;

	setMemory({
		{0x1000, 0x1234_w},
		{0x1002, 0x90ab_w},
	});

	emulate("lfs ax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_FS, 0x90ab},
		{X86_REG_AX, 0x1234},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1002});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LFS_32)
{
	ONLY_MODE_32;

	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x90ab_w},
	});

	emulate("lfs eax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_FS, 0x90ab},
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_LGS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LGS_16)
{
	ONLY_MODE_16;

	setMemory({
		{0x1000, 0x1234_w},
		{0x1002, 0x90ab_w},
	});

	emulate("lgs ax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_GS, 0x90ab},
		{X86_REG_AX, 0x1234},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1002});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LGS_32)
{
	ONLY_MODE_32;

	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x90ab_w},
	});

	emulate("lgs eax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_GS, 0x90ab},
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_LSS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LSS_16)
{
	ONLY_MODE_16;

	setMemory({
		{0x1000, 0x1234_w},
		{0x1002, 0x90ab_w},
	});

	emulate("lss ax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SS, 0x90ab},
		{X86_REG_AX, 0x1234},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1002});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LSS_32)
{
	ONLY_MODE_32;

	setMemory({
		{0x1000, 0x12345678_dw},
		{0x1004, 0x90ab_w},
	});

	emulate("lss eax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SS, 0x90ab},
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_MEMORY_LOADED({0x1000, 0x1004});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_MOV
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_reg8_reg8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x12},
	});

	emulate("mov cl, al");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CL, 0x12},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_reg8_mem8)
{
	ALL_MODES;

	setMemory({
		{0x1000, 0x12_b},
	});

	emulate("mov cl, [0x1000]");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CL, 0x12},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_reg16_imm16)
{
	ALL_MODES;

	emulate("mov cx, 0x1234");

	if (GetParam() != CS_MODE_16)
	{
		EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX});
	}
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
	});

	emulate("mov ecx, eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0x12345678},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_mem32_reg32)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
	});

	emulate("mov [0x1234], eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0x12345678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_reg64_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x0123456789abcdef},
	});

	emulate("mov rcx, rax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RCX, 0x0123456789abcdef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_from_ds_addr_space_1)
{
	SKIP_MODE_16;

	setMemory({
		{0x1000, 0x12345678_dw},
	});

	// should be the same as "mov eax, ds:0x1000"
	emulate("mov eax, [0x1000]");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_from_ds_addr_space_2)
{
	SKIP_MODE_16;

	setMemory({
		{0x1000, 0x12345678_dw},
	});

	// should be the same as "mov eax, [0x1000]"
	emulate("mov eax, ds:0x1000");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_from_gs_addr_space)
{
	SKIP_MODE_16;

	emulate("mov eax, gs:0x1000");

	bool loadInsnOk = false;
	for (llvm::inst_iterator I = llvm::inst_begin(_function),
			E = llvm::inst_end(_function); I != E; ++I)
	{
		if (llvm::LoadInst* l = llvm::dyn_cast<llvm::LoadInst>(&*I))
		{
			llvm::ConstantExpr* ce = llvm::dyn_cast<llvm::ConstantExpr>(l->getPointerOperand());
			llvm::ConstantInt* ci = ce ? llvm::dyn_cast<llvm::ConstantInt>(ce->getOperand(0)) : nullptr;

			loadInsnOk = ce
					&& ci
					&& ce->getOpcode() == llvm::Instruction::IntToPtr
					&& ce->getType()->getPointerAddressSpace()
						== static_cast<unsigned>(x86_addr_space::GS);
			if (loadInsnOk)
			{
				break;
			}
		}
	}

	EXPECT_TRUE(loadInsnOk);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_from_fs_addr_space)
{
	SKIP_MODE_16;

	emulate("mov eax, fs:0x1000");

	bool loadInsnOk = false;
	for (llvm::inst_iterator I = llvm::inst_begin(_function),
			E = llvm::inst_end(_function); I != E; ++I)
	{
		if (llvm::LoadInst* l = llvm::dyn_cast<llvm::LoadInst>(&*I))
		{
			llvm::ConstantExpr* ce = llvm::dyn_cast<llvm::ConstantExpr>(l->getPointerOperand());
			llvm::ConstantInt* ci = ce ? llvm::dyn_cast<llvm::ConstantInt>(ce->getOperand(0)) : nullptr;

			loadInsnOk = ce
					&& ci
					&& ce->getOpcode() == llvm::Instruction::IntToPtr
					&& ce->getType()->getPointerAddressSpace()
						== static_cast<unsigned>(x86_addr_space::FS);
			if (loadInsnOk)
			{
				break;
			}
		}
	}

	EXPECT_TRUE(loadInsnOk);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_from_ss_addr_space)
{
	SKIP_MODE_16;

	emulate("mov eax, ss:0x1000");

	bool loadInsnOk = false;
	for (llvm::inst_iterator I = llvm::inst_begin(_function),
			E = llvm::inst_end(_function); I != E; ++I)
	{
		if (llvm::LoadInst* l = llvm::dyn_cast<llvm::LoadInst>(&*I))
		{
			llvm::ConstantExpr* ce = llvm::dyn_cast<llvm::ConstantExpr>(l->getPointerOperand());
			llvm::ConstantInt* ci = ce ? llvm::dyn_cast<llvm::ConstantInt>(ce->getOperand(0)) : nullptr;

			loadInsnOk = ce
					&& ci
					&& ce->getOpcode() == llvm::Instruction::IntToPtr
					&& ce->getType()->getPointerAddressSpace()
						== static_cast<unsigned>(x86_addr_space::SS);
			if (loadInsnOk)
			{
				break;
			}
		}
	}

	EXPECT_TRUE(loadInsnOk);
}

//
// X86_INS_MOVABS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOV_reg64_imm64)
{
	ONLY_MODE_64;

	emulate("movabs rcx, 0x0123456789abcdef");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RCX, 0x0123456789abcdef},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_MOVSX
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSX_sign)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_AX, 0xff00},
	});

	emulate("movsx ecx, ax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0xffffff00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSX_unsign)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_AX, 0x00ff},
	});

	emulate("movsx ecx, ax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0x000000ff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_MOVSXD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSXD_sign)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_EAX, 0xff000000},
	});

	emulate("movsxd rcx, eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RCX, 0xffffffffff000000},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSXD_unsign)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_EAX, 0x000000ff},
	});

	emulate("movsxd rcx, eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RCX, 0x00000000000000ff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_MOVZX
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVZX)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_AX, 0xff00},
	});

	emulate("movzx ecx, ax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0x0000ff00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_MUL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MUL_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0x0f},
		{X86_REG_AL, 0xa0},
	});

	emulate("mul cl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL, X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x0960},
		{X86_REG_OF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MUL_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0x0f},
		{X86_REG_AX, 0xa0},
	});

	emulate("mul dx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x0960},
		{X86_REG_DX, 0x0000},
		{X86_REG_OF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MUL_r32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0x0f},
		{X86_REG_EAX, 0xa0},
	});

	emulate("mul edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x0960},
		{X86_REG_EDX, 0x0000},
		{X86_REG_OF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MUL_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RDX, 0x0f},
		{X86_REG_RAX, 0xa0},
	});

	emulate("mul rdx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RDX, X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x0960},
		{X86_REG_RDX, 0x0000},
		{X86_REG_OF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_IMUL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0x0f},
		{X86_REG_AL, 0xa0},
	});

	emulate("imul cl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL, X86_REG_AL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0xfa60},
		{X86_REG_OF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0x0f},
		{X86_REG_AX, 0xa0},
	});

	emulate("imul dx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x0960},
		{X86_REG_DX, 0x0000},
		{X86_REG_OF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0x0f},
		{X86_REG_EAX, 0xa0},
	});

	emulate("imul edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x0960},
		{X86_REG_EDX, 0x0000},
		{X86_REG_OF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RDX, 0x0f},
		{X86_REG_RAX, 0xa0},
	});

	emulate("imul rdx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RDX, X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x0960},
		{X86_REG_RDX, 0x0000},
		{X86_REG_OF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r32_binary)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0x0f000000},
		{X86_REG_EAX, 0xa0000000},
	});

	emulate("imul eax, edx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x0},
		{X86_REG_OF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_IMUL_r32_ternary)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0x00001234},
	});

	emulate("imul eax, edx, 0xf0");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EDX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x1110c0},
		{X86_REG_OF, false},
		{X86_REG_CF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_NEG
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NEG_reg8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0x4d}, // 01001101
		{X86_REG_CF, false},
	});

	emulate("neg cl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CL, 0xb3}, // 10110011
		{X86_REG_CF, true},
		{X86_REG_OF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NEG_reg16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x0},
		{X86_REG_CF, true},
	});

	emulate("neg cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x0}, // yes, this is really 0x0, not oxffff
		{X86_REG_CF, false},
		{X86_REG_OF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NEG_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0x4d}, // 01001101
		{X86_REG_CF, false},
	});

	emulate("neg ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0xffffffb3}, // 10110011
		{X86_REG_CF, true},
		{X86_REG_OF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NEG_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RCX, 0x4d}, // 01001101
		{X86_REG_CF, false},
	});

	emulate("neg rcx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RCX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RCX, 0xffffffffffffffb3}, // 10110011
		{X86_REG_CF, true},
		{X86_REG_OF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_NOP, X86_INS_UD2, X86_INS_UD2B, X86_INS_FNOP, X86_INS_HLT
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NOP)
{
	ALL_MODES;

	emulate("nop");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_UD2)
{
	ALL_MODES;

	emulate("ud2");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_UD2B)
{
	ALL_MODES;

	emulate("ud2b");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FNOP)
{
	ALL_MODES;

	emulate("fnop");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_NOT
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NOT_reg8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CL, 0xf0},
	});

	emulate("not cl");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CL});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CL, 0x0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NOT_reg16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0xf0f0},
	});

	emulate("not cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x0f0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NOT_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0xf0f0f0f0},
	});

	emulate("not ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0x0f0f0f0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_NOT_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RCX, 0xf0f0f0f0f0f0f0f0},
	});

	emulate("not rcx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RCX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RCX, 0x0f0f0f0f0f0f0f0f},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_OR
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_OR_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_ECX, 0x10305070},
	});

	emulate("or eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_PF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_OF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_CF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_OR_reg64_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x0123456700000000},
		{X86_REG_RCX, 0x0000000089abcdef},
	});

	emulate("or rax, rcx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_RCX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x0123456789abcdef},
		{X86_REG_PF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_OF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_CF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_POP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_POP_reg16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SP, 0xfe},
	});
	setMemory({
		{0xfe, 0x1234_w}
	});

	emulate("pop ax");

	if (GetParam() == CS_MODE_16)
	{
		EXPECT_JUST_REGISTERS_LOADED({X86_REG_SP});
	}
	else
	{
		EXPECT_JUST_REGISTERS_LOADED({X86_REG_SP, X86_REG_AX});
	}
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SP, 0x100}, // 0xfe + 0x2
		{X86_REG_AX, 0x1234},
	});
	EXPECT_JUST_MEMORY_LOADED({0xfe});
	EXPECT_NO_MEMORY_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_POP_reg32)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_ESP, 0xfc},
	});
	setMemory({
		{0xfc, 0x12345678_dw}
	});

	emulate("pop eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ESP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ESP, 0x100}, // 0xfc + 0x4
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_JUST_MEMORY_LOADED({0xfc});
	EXPECT_NO_MEMORY_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_POP_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RSP, 0xf8},
	});
	setMemory({
		{0xf8, 0x0123456789abcdef_qw}
	});

	emulate("pop rax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RSP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RSP, 0x100}, // 0xf8 + 0x8
		{X86_REG_RAX, 0x0123456789abcdef},
	});
	EXPECT_JUST_MEMORY_LOADED({0xf8});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_POPAW (POPA)
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_POPAW)
{
	ONLY_MODE_16;

	setRegisters({
		{X86_REG_SP, 0x100},
	});
	setMemory({
		{0x100, 0x0001_w},
		{0x102, 0x0002_w},
		{0x104, 0x0003_w},
		// skip next 2 bytes
		{0x108, 0x0004_w},
		{0x10a, 0x0005_w},
		{0x10c, 0x0006_w},
		{0x10e, 0x0007_w},
	});

	emulate("popaw");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SP, 0x110}, // 0x100 + 7 * 2 + 2 (2 more bytes are skipped)
		{X86_REG_DI, 0x0001},
		{X86_REG_SI, 0x0002},
		{X86_REG_BP, 0x0003},
		{X86_REG_BX, 0x0004},
		{X86_REG_DX, 0x0005},
		{X86_REG_CX, 0x0006},
		{X86_REG_AX, 0x0007},
	});
	EXPECT_JUST_MEMORY_LOADED({
		0x100, 0x102, 0x104, 0x108, 0x10a, 0x10c, 0x10e
	});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_POPAL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_POPAL)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_SP, 0x100},
	});
	setMemory({
		{0x100, 0x0001_dw},
		{0x104, 0x0002_dw},
		{0x108, 0x0003_dw},
		// skip next 4 bytes
		{0x110, 0x0004_dw},
		{0x114, 0x0005_dw},
		{0x118, 0x0006_dw},
		{0x11c, 0x0007_dw},
	});

	emulate("popal");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ESP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ESP, 0x120}, // 0x100 + 7 * 4 + 4 (4 more bytes are skipped)
		{X86_REG_EDI, 0x0001},
		{X86_REG_ESI, 0x0002},
		{X86_REG_EBP, 0x0003},
		{X86_REG_EBX, 0x0004},
		{X86_REG_EDX, 0x0005},
		{X86_REG_ECX, 0x0006},
		{X86_REG_EAX, 0x0007},
	});
	EXPECT_JUST_MEMORY_LOADED({
		0x100, 0x104, 0x108, 0x110, 0x114, 0x118, 0x11c
	});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_PUSH
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUSH_reg16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SP, 0x100},
		{X86_REG_AX, 0x1234},
	});

	emulate("push ax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SP, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SP, 0xfe}, // 0x100 - 0x2
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xfe, 0x1234_w} // 0x100 - 0x2
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUSH_reg32)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_ESP, 0x100},
		{X86_REG_EAX, 0x12345678},
	});

	emulate("push eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ESP, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ESP, 0xfc}, // 0x100 - 0x4
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xfc, 0x12345678_dw} // 0x100 - 0x4
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUSH_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RSP, 0x100},
		{X86_REG_RAX, 0x0123456789abcdef},
	});

	emulate("push rax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RSP, X86_REG_RAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RSP, 0xf8}, // 0x100 - 0x8
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xf8, 0x0123456789abcdef_qw} // 0x100 - 0x8
	});
}

//
// X86_INS_PUSHAW
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUSHAW)
{
	ONLY_MODE_16;

	setRegisters({
		{X86_REG_SP, 0x100},
		{X86_REG_DI, 0x0001},
		{X86_REG_SI, 0x0002},
		{X86_REG_BP, 0x0003},
		{X86_REG_BX, 0x0004},
		{X86_REG_DX, 0x0005},
		{X86_REG_CX, 0x0006},
		{X86_REG_AX, 0x0007},
	});

	emulate("pushaw");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SP, X86_REG_DI, X86_REG_SI, X86_REG_BP, X86_REG_BX, X86_REG_DX, X86_REG_CX, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SP, 0xf0}, // 0x100 - (7 * 2 + 2 (2 more bytes are skipped))

	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xfe, 0x0007_w},
		{0xfc, 0x0006_w},
		{0xfa, 0x0005_w},
		{0xf8, 0x0004_w},
		{0xf6, ANY},
		{0xf4, 0x0003_w},
		{0xf2, 0x0002_w},
		{0xf0, 0x0001_w},
	});
}

//
// X86_INS_PUSHAL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUSHAL)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_ESP, 0x100},
		{X86_REG_EDI, 0x0001},
		{X86_REG_ESI, 0x0002},
		{X86_REG_EBP, 0x0003},
		{X86_REG_EBX, 0x0004},
		{X86_REG_EDX, 0x0005},
		{X86_REG_ECX, 0x0006},
		{X86_REG_EAX, 0x0007},
	});

	emulate("pushal");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ESP, X86_REG_EDI, X86_REG_ESI, X86_REG_EBP, X86_REG_EBX, X86_REG_EDX, X86_REG_ECX, X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ESP, 0xe0}, // 0x100 - (7 * 4 + 4 (4 more bytes are skipped))

	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0xfc, 0x0007_w},
		{0xf8, 0x0006_w},
		{0xf4, 0x0005_w},
		{0xf0, 0x0004_w},
		{0xec, ANY},
		{0xe8, 0x0003_w},
		{0xe4, 0x0002_w},
		{0xe0, 0x0001_w},
	});
}

//
// X86_INS_SAHF
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAHF)
{
	SKIP_MODE_64;

	setRegisters({
		{X86_REG_AH, 0xff},
	});

	emulate("sahf");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AH});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_SF, true},
		{X86_REG_ZF, true},
		{X86_REG_AF, true},
		{X86_REG_PF, true},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SALC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SALC_cf)
{
	SKIP_MODE_64;

	setRegisters({
		{X86_REG_AL, 0x12},
		{X86_REG_CF, true},
	});

	emulate("salc");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0xff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SALC_no_cf)
{
	SKIP_MODE_64;

	setRegisters({
		{X86_REG_AL, 0x12},
		{X86_REG_CF, false},
	});

	emulate("salc");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x00},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_STC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_STC)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
	});

	emulate("stc");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_STD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_STD)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DF, false},
	});

	emulate("std");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_DF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SBB
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SBB_cf_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
		{X86_REG_EBX, 0x567},
		{X86_REG_CF, false}
	});

	emulate("sbb eax, ebx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_EBX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x1234 - (0x567 + 0x0)},
		{X86_REG_CF, false},
		{X86_REG_OF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SBB_cf_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
		{X86_REG_EBX, 0x567},
		{X86_REG_CF, true}
	});

	emulate("sbb eax, ebx");

	// Every flag here is the hardware's answer, executed on this machine.
	// The test used to pin CF to `true` -- 0x1234 - 0x568 does not borrow --
	// and leave the other five as ANY, which is why the carry being applied
	// twice went unnoticed for as long as it did.
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_EBX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x1234 - (0x567 + 0x1)},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_PF, true},
		{X86_REG_AF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SBB_eax_eax_cf_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234}, // in this case, it does not matter what is here
		{X86_REG_CF, false}
	});

	emulate("sbb eax, eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x0},
		{X86_REG_CF, false},
		{X86_REG_OF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SBB_eax_eax_cf_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234}, // in this case, it does not matter what is here
		{X86_REG_CF, true}
	});

	emulate("sbb eax, eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xffffffff},
		{X86_REG_CF, true},
		{X86_REG_OF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
		{X86_REG_AF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SHL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHL_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x24}, // 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("shl al, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x20}, // 00000001 | 00100000 = 0x120 = 0x20
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHL_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x2400}, // 00100100 0...
		{X86_REG_OF, true} // should not be affected
	});

	emulate("shl ax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2000}, // 00000001 | 00100000 = 0x120 = 0x20
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHL_r32_cf_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x24000000}, // 00100100 0...
		{X86_REG_OF, false} // should not be affected
	});

	emulate("shl eax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x20000000}, // 00000001 | 00100000 = 0x120 = 0x20
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHL_r32_cf_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x24000000}, // 00100100 0...
		{X86_REG_OF, false} // should not be affected
	});

	emulate("shl eax, 0x4");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x40000000}, // 00000010 | 01000000 = 0x140 = 0x40
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHL_r32_of_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xc0000000}, // 11000000 0...
		{X86_REG_OF, true}
	});

	emulate("shl eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x80000000}, // 00000010 | 01000000 = 0x140 = 0x40
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHL_r32_of_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xa0000000}, // 10100000 0...
		{X86_REG_OF, false}
	});

	emulate("shl eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x40000000}, // 00000010 | 01000000 = 0x140 = 0x40
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHL_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x2400000000000000}, // 00100100 0...
		{X86_REG_OF, true} // should not be affected
	});

	emulate("shl rax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x2000000000000000}, // 00000001 | 00100000 = 0x120 = 0x20
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SAL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAL_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x24}, // 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("sal al, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x20}, // 00000001 | 00100000 = 0x120 = 0x20
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAL_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x2400}, // 00100100 0...
		{X86_REG_OF, true} // should not be affected
	});

	emulate("sal ax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2000}, // 00000001 | 00100000 = 0x120 = 0x20
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAL_r32_cf_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x24000000}, // 00100100 0...
		{X86_REG_OF, false} // should not be affected
	});

	emulate("sal eax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x20000000}, // 00000001 | 00100000 = 0x120 = 0x20
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAL_r32_cf_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x24000000}, // 00100100 0...
		{X86_REG_OF, false} // should not be affected
	});

	emulate("sal eax, 0x4");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x40000000}, // 00000010 | 01000000 = 0x140 = 0x40
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAL_r32_of_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xc0000000}, // 11000000 0...
		{X86_REG_OF, true}
	});

	emulate("sal eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x80000000}, // 00000010 | 01000000 = 0x140 = 0x40
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAL_r32_of_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xa0000000}, // 10100000 0...
		{X86_REG_OF, false}
	});

	emulate("sal eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x40000000}, // 00000010 | 01000000 = 0x140 = 0x40
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAL_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x2400000000000000}, // 00100100 0...
		{X86_REG_OF, true} // should not be affected
	});

	emulate("sal rax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x2000000000000000}, // 00000001 | 00100000 = 0x120 = 0x20
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SHR
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHR_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x24}, // 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("shr al, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x4}, // 00000100 = 0x4
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHR_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x8024}, // 10000000 00100100
		{X86_REG_OF, true} // should not be affected
	});

	emulate("shr ax, 0x8");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x80}, // 10000000 = 0x80
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHR_r32_cf_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x00000024}, // 0... 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("shr eax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x4}, // 00000100 = 0x4
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHR_r32_cf_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x00000024}, // 0... 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("shr eax, 24");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x0}, // 0x0
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHR_r32_of_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x7e000000}, // 01111110 0...
		{X86_REG_OF, true}
	});

	emulate("shr eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x3f000000}, // 00111111 0... = 0x3f 0...
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHR_r32_of_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xf0000000}, // 11110000 0...
		{X86_REG_OF, false}
	});

	emulate("shr eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x78000000}, // 01111000 0... = 0x78 0...
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHR_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x0000000000000024}, // 0... 00100100
		{X86_REG_OF, true} // should not be affected
	});

	emulate("shr rax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x4}, // 00000100 = 0x4
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SAR
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAR_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x24}, // 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("sar al, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x4}, // 00000100 = 0x4
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAR_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x8024}, // 10000000 00100100
		{X86_REG_OF, true} // should not be affected
	});

	emulate("sar ax, 0x8");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0xff80}, // 11111111 10000000 = 0xff 0x80
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAR_r32_cf_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x80000024}, // 10... 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("sar eax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xf0000004}, // 11110000 0... 00000100
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAR_r32_cf_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x00000024}, // 0... 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("sar eax, 24");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x0}, // 0x0
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAR_r32_of_false_1)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x7e000000}, // 01111110 0...
		{X86_REG_OF, true}
	});

	emulate("sar eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x3f000000}, // 00111111 0... = 0x3f 0...
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAR_r32_of_false_2)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xf0000000}, // 11110000 0...
		{X86_REG_OF, true}
	});

	emulate("sar eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xf8000000}, // 11111000 0... = 0xf8 0...
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, false},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SAR_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x0000000000000024}, // 0... 00100100
		{X86_REG_OF, true} // should not be affected
	});

	emulate("sar rax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x4}, // 00000100 = 0x4
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
		{X86_REG_ZF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_PF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_ROL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROL_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x24}, // 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("rol al, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x21}, // 00100 001
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROL_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x2400}, // 00100100 0...
		{X86_REG_OF, true} // should not be affected
	});

	emulate("rol ax, 0x5");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x8004}, // 10000000 00000100
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROL_r32_mask)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x00240000}, // 00100100 0...
		{X86_REG_OF, false} // should not be affected
	});

	emulate("rol eax, 0x48"); // 0x48 and 0x1f = 0x8

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x24000000},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROL_r32_of_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xc0000000}, // 11000000 0...
		{X86_REG_OF, true}
	});

	emulate("rol eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x80000001},
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false}, // CF xor MSB
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROL_r32_of_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xa0000000}, // 10100000 0...
		{X86_REG_OF, false}
	});

	emulate("rol eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x40000001},
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true}, // CF xor MSB
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROL_r64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x2400000000000000}, // 00100100 0...
		{X86_REG_OF, true} // should not be affected
	});

	emulate("rol rax, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x2000000000000001},
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_ROR
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROR_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AL, 0x24}, // 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("ror al, 0x3");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x84}, // 100 00100
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROR_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x8024}, // 10000000 00100100
		{X86_REG_OF, true} // should not be affected
	});

	emulate("ror ax, 0x5");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2401}, // 00100 10000000 001
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROR_r32_mask)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x00000024}, // 0... 00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("ror eax, 0x48"); // 0x48 and 0x1f = 0x8

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x24000000}, // 00100100 0..
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROR_r32_of_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xfe000001}, // 11111110 0...1
		{X86_REG_OF, true}
	});

	emulate("ror eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xff000000}, // 11111111 0..
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, false}, // xor 2 MSBs
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROR_r32_of_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xfe000000}, // 11111110 0...
		{X86_REG_OF, false}
	});

	emulate("ror eax");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x7f000000}, // 01111111 0..
		{X86_REG_CF, false}, // last shifted
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ROR_r64_mask)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x00000000000000ff},
		{X86_REG_OF, true} // should not be affected
	});

	emulate("ror rax, 0x88"); // 0x88 & 0x3f = 0x8

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0xff00000000000000},
		{X86_REG_CF, true}, // last shifted
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_RCR
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCR_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true}, // 1
		{X86_REG_AL, 0x24}, //  00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("rcr al, 0x3"); // 1 00100100 -> 100 1 00100

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_AL, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, true},
		{X86_REG_AL, 0x24},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCR_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},   // 1
		{X86_REG_AX, 0x8024}, //  10000000 00100100
		{X86_REG_OF, true} // should not be affected
	});

	emulate("rcr ax, 0x5"); // 1 10000000 00100100 -> 00100 1 10000000 001

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_AX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_AX, 0x4c01},
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCR_r32_mask)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, false},       // 0
		{X86_REG_EAX, 0x00000024}, //  0... 00100100
		{X86_REG_OF, false} // should not be affected
	});

	// 0x48 and 0x1f = 0x8
	// 0 0... 00100100 -> 00100100 0 0..
	emulate("rcr eax, 0x48");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_EAX, 0x48000000},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCR_r32_of_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, false},       // 0
		{X86_REG_EAX, 0xfe000000}, //  11111110 0...
		{X86_REG_OF, false}
	});

	emulate("rcr eax"); // 0 11111110 0... -> 0 0 11111110 0...

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_EAX, 0x7f000000},
		{X86_REG_OF, true}, // xor 2 MSBs of result (not CF, checked by olly)
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCR_r32_of_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, true},        // 1
		{X86_REG_EAX, 0xfe000000}, //  11111110 0...
		{X86_REG_OF, true}
	});

	emulate("rcr eax"); // 1 11111110 0... -> 0 1 11111110 0...

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_EAX, 0xff000000},
		{X86_REG_OF, false}, // xor 2 MSBs of result (not CF, checked by olly)
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCR_r64_mask)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_CF, true},                // 1
		{X86_REG_RAX, 0x00000000000000ff}, //  0... 11111111
		{X86_REG_OF, true} // should not be affected
	});

	emulate("rcr rax, 0x88"); // 1 0... 11111111 -> 11111111 1 0...

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_RAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, true},
		{X86_REG_RAX, 0xff00000000000000},
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_RCL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCL_r8)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true}, // 1
		{X86_REG_AL, 0x24}, //  00100100
		{X86_REG_OF, false} // should not be affected
	});

	emulate("rcl al, 0x3"); // 1 00|100100 -> 100100 1 00

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_AL, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, true},
		{X86_REG_AL, 0x24},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCL_r16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},   // 1
		{X86_REG_AX, 0x8024}, //  10000000 00100100
		{X86_REG_OF, true} // should not be affected
	});

	emulate("rcl ax, 0x5"); // 1 1000|0000 00100100

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_AX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_AX, 0x498},
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCL_r32_mask)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, false},       // 0
		{X86_REG_EAX, 0x00000024}, //  0... 00100100
		{X86_REG_OF, false} // should not be affected
	});

	// 0x48 and 0x1f = 0x8
	// 0 0000000|0 0... 00100100
	emulate("rcl eax, 0x48");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_EAX, 0x2400},
		{X86_REG_OF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCL_r32_of_false)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, false},       // 0
		{X86_REG_EAX, 0xfe000000}, //  11111110 0...
		{X86_REG_OF, true}
	});

	emulate("rcl eax"); // 0| 11111110 0...

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, true},
		{X86_REG_EAX, 0xfc000000},
		{X86_REG_OF, false}, // CF xor MSB
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCL_r32_of_true)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_CF, false},        // 0
		{X86_REG_EAX, 0x7e000000}, //  01111110 0...
		{X86_REG_OF, false}
	});

	emulate("rcl eax"); // 0 |01111110 0...

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_EAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_EAX, 0xfc000000},
		{X86_REG_OF, true}, // CF xor MSB
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RCL_r64_mask)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_CF, true},                // 1
		{X86_REG_RAX, 0x00000000000000ff}, //  0... 11111111
		{X86_REG_OF, true} // should not be affected
	});

	emulate("rcl rax, 0x88"); // 1 0000000|0 0... 11111111

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_RAX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CF, false},
		{X86_REG_RAX, 0x000000000000ff80},
		{X86_REG_OF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_XCHG
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XCHG_reg16_reg16)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1234},
		{X86_REG_CX, 0x5678},
	});

	emulate("xchg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x5678},
		{X86_REG_CX, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XCHG_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
		{X86_REG_ECX, 0x5678},
	});

	emulate("xchg eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x5678},
		{X86_REG_ECX, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XCHG_reg32_mem32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0x5678},
	});
	setMemory({
		{0x1234, 0xffff_dw},
	});

	emulate("xchg ecx, [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ECX, 0xffff},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0x5678_dw}
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XCHG_reg64_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1234},
		{X86_REG_RCX, 0x5678},
	});

	emulate("xchg rax, rcx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_RCX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x5678},
		{X86_REG_RCX, 0x1234},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_XLATB
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XLATB_16)
{
	ONLY_MODE_16;

	setRegisters({
		{X86_REG_AL, 0x0034},
		{X86_REG_BX, 0x1200},
	});
	setMemory({
		{0x1234, 0x11_b},
	});

	emulate("xlatb");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_BX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x11},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XLATB_32)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_AL, 0x0034},
		{X86_REG_EBX, 0x1200},
	});
	setMemory({
		{0x1234, 0x11_b},
	});

	emulate("xlatb");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_EBX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x11},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XLATB_64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_AL, 0x0034},
		{X86_REG_RBX, 0x1200},
	});
	setMemory({
		{0x1234, 0x11_b},
	});

	emulate("xlatb");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AL, X86_REG_RBX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, 0x11},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_XOR
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XOR_reg32_reg32)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_ECX, 0x10305070},
	});

	emulate("xor eax, ecx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX, X86_REG_ECX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x02040608},
		{X86_REG_PF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_OF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_CF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XOR_reg64_reg64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x0123456700000000},
		{X86_REG_RCX, 0x0000000089abcdef},
	});

	emulate("xor rax, rcx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_RAX, X86_REG_RCX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 0x0123456789abcdef},
		{X86_REG_PF, ANY},
		{X86_REG_SF, ANY},
		{X86_REG_ZF, ANY},
		{X86_REG_OF, ANY},
		{X86_REG_AF, ANY},
		{X86_REG_CF, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_LOOP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOP_r16_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0xa},
	});

	emulate("loop 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOP_r16_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x1},
	});

	emulate("loop 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOP_r16_jump_underflow)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x0},
	});

	emulate("loop 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0xffff},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1012}},
	});
}

//
// X86_INS_LOOPE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOPE_r16_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0xa},
		{X86_REG_ZF, true},
	});

	emulate("loope 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOPE_r16_no_jump_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0xa},
		{X86_REG_ZF, false},
	});

	emulate("loope 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOPE_r16_no_jump_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x1},
		{X86_REG_ZF, true},
	});

	emulate("loope 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOPE_r16_no_jump_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x1},
		{X86_REG_ZF, false},
	});

	emulate("loope 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1012}},
	});
}

//
// X86_INS_LOOPNE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOPNE_r16_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0xa},
		{X86_REG_ZF, false},
	});

	emulate("loopne 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOPNE_r16_no_jump_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0xa},
		{X86_REG_ZF, true},
	});

	emulate("loopne 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x9},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOPNE_r16_no_jump_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x1},
		{X86_REG_ZF, false},
	});

	emulate("loopne 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1012}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LOOPNE_r16_no_jump_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CX, 0x1},
		{X86_REG_ZF, true},
	});

	emulate("loopne 0x1012", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_CX, 0x0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1012}},
	});
}

//
// X86_INS_JAE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JAE_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
	});

	emulate("jae 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JAE_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
	});

	emulate("jae 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JA
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JA_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});

	emulate("ja 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JA_no_jump_cf)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});

	emulate("ja 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JA_no_jump_zf)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});

	emulate("ja 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JA_no_jump_cf_zf)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_ZF, true},
	});

	emulate("ja 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JBE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JBE_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});

	emulate("jbe 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JBE_jump_cf)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});

	emulate("jbe 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JBE_jump_zf)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});

	emulate("jbe 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JBE_jump_cf_zf)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_ZF, true},
	});

	emulate("jbe 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

//
// X86_INS_JB
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JB_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
	});

	emulate("jb 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JB_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
	});

	emulate("jb 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JE_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
	});

	emulate("je 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JE_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
	});

	emulate("je 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JGE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JGE_jump_eq_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("jge 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JGE_jump_ef_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("jge 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JGE_no_jump_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("jge 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JGE_no_jump_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("jge 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JG
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JG_jump_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("jg 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JG_jump_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("jg 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JG_no_jump_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("jg 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JG_no_jump_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("jg 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JG_no_jump_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("jg 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JG_no_jump_4)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("jg 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JG_no_jump_5)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("jg 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JG_no_jump_6)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("jg 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JLE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JLE_jump_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("jle 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JLE_jump_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("jle 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JLE_jump_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("jle 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JLE_jump_4)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("jle 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JLE_jump_5)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("jle 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JLE_jump_6)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("jle 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JLE_no_jump_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("jle 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JLE_no_jump_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("jle 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JL_jump_ne_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("jl 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JL_jump_ne_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("jl 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JL_no_jump_eq_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("jl 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JL_no_jump_eq_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("jl 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JNE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JNE_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
	});

	emulate("jne 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JNE_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
	});

	emulate("jne 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JNO
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JNO_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_OF, false},
	});

	emulate("jno 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JNO_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_OF, true},
	});

	emulate("jno 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JNP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JNP_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_PF, false},
	});

	emulate("jnp 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_PF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JNP_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_PF, true},
	});

	emulate("jnp 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_PF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JNS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JNS_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
	});

	emulate("jns 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JNS_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
	});

	emulate("jns 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JO
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JO_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_OF, true},
	});

	emulate("jo 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JO_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_OF, false},
	});

	emulate("jo 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_OF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JP_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_PF, true},
	});

	emulate("jp 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_PF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JP_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_PF, false},
	});

	emulate("jp 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_PF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_JS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JS_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
	});

	emulate("js 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {true, 0x1234}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_JS_no_jump)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
	});

	emulate("js 0x1234", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_translator->getCondBranchFunction(), {false, 0x1234}},
	});
}

//
// X86_INS_SETAE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETAE_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
	});

	emulate("setae al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETAE_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
	});

	emulate("setae al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETA
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETA_set_true_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});

	emulate("seta al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETA_set_false_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});

	emulate("seta al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETA_set_false_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});

	emulate("seta al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETA_set_false_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_ZF, true},
	});

	emulate("seta al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETBE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETBE_set_false_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});

	emulate("setbe al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETBE_set_true_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});

	emulate("setbe al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETBE_set_true_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});

	emulate("setbe al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETBE_set_true_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
		{X86_REG_ZF, true},
	});

	emulate("setbe al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETB
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETB_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, true},
	});

	emulate("setb al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETB_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_CF, false},
	});

	emulate("setb al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_CF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETE_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
	});

	emulate("sete al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETE_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
	});

	emulate("sete al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETGE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETGE_set_true_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("setge al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETGE_set_true_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("setge al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETGE_set_false_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("setge al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETGE_set_false_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("setge al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETG
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETG_set_true_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("setg al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETG_set_true_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("setg al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETG_set_false_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("setg al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETG_set_false_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("setg al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETG_set_false_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("setg al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETG_set_false_4)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("setg al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETG_set_false_5)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("setg al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETG_set_false_6)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("setg al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETLE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETLE_set_true_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("setle al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETLE_set_true_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("setle al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETLE_set_true_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("setle al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETLE_set_true_4)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("setle al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETLE_set_true_5)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("setle al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETLE_set_true_6)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("setle al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETLE_set_false_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("setle al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETLE_set_false_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("setle al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETL_set_true_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("setl al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETL_set_true_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("setl al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETL_set_false_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("setl al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETL_set_false_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("setl al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETNE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETNE_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, false},
	});

	emulate("setne al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETNE_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_ZF, true},
	});

	emulate("setne al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ZF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETNO
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETNO_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_OF, false},
	});

	emulate("setno al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETNO_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_OF, true},
	});

	emulate("setno al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETNP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETNP_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_PF, false},
	});

	emulate("setnp al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_PF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETNP_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_PF, true},
	});

	emulate("setnp al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_PF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETNS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETNS_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
	});

	emulate("setns al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETNS_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
	});

	emulate("setns al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETO
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETO_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_OF, true},
	});

	emulate("seto al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETO_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_OF, false},
	});

	emulate("seto al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_OF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETP_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_PF, true},
	});

	emulate("setp al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_PF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETP_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_PF, false},
	});

	emulate("setp al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_PF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_SETS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETS_set_true)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, true},
	});

	emulate("sets al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SETS_set_false)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_SF, false},
	});

	emulate("sets al", 0x1000);

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_SF, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AL, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVAE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVAE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, false},
	});

	emulate("cmovae ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVAE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, true},
	});

	emulate("cmovae ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVA
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVA_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});

	emulate("cmova ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVA_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});

	emulate("cmova ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVA_no_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});

	emulate("cmova ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVA_no_move_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, true},
		{X86_REG_ZF, true},
	});

	emulate("cmova ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVBE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVBE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});

	emulate("cmovbe ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVBE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});

	emulate("cmovbe ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVBE_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});

	emulate("cmovbe ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVBE_move_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, true},
		{X86_REG_ZF, true},
	});

	emulate("cmovbe ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVB
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVB_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, true},
	});

	emulate("cmovb ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVB_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_CF, false},
	});

	emulate("cmovb ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
	});

	emulate("cmove ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
	});

	emulate("cmove ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVGE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVGE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("cmovge ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVGE_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("cmovge ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVGE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("cmovge ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVGE_no_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("cmovge ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVG
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVG_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("cmovg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVG_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("cmovg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVG_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("cmovg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVG_no_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("cmovg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVG_no_move_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("cmovg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVG_no_move_4)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("cmovg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVG_no_move_5)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("cmovg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVG_no_move_6)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("cmovg ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVLE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVLE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("cmovle ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVLE_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("cmovle ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVLE_move_3)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("cmovle ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVLE_move_4)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("cmovle ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVLE_move_5)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("cmovle ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVLE_move_6)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("cmovle ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVLE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("cmovle ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVLE_no_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("cmovle ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVL
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVL_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, true},
		{X86_REG_OF, false},
	});

	emulate("cmovl ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVL_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, false},
		{X86_REG_OF, true},
	});

	emulate("cmovl ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVL_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, true},
		{X86_REG_OF, true},
	});

	emulate("cmovl ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVL_no_move_2)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, false},
		{X86_REG_OF, false},
	});

	emulate("cmovl ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVNE
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVNE_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, false},
	});

	emulate("cmovne ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVNE_no_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_ZF, true},
	});

	emulate("cmovne ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVNO
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVNO_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_OF, false},
	});

	emulate("cmovno ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVNO_no_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_OF, true},
	});

	emulate("cmovno ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVNP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVNP_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_PF, false},
	});

	emulate("cmovnp ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_PF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVNP_no_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_PF, true},
	});

	emulate("cmovnp ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_PF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVNS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVNS_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, false},
	});

	emulate("cmovns ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVNS_no_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, true},
	});

	emulate("cmovns ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVO
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVO_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_OF, true},
	});

	emulate("cmovo ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVO_no_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_OF, false},
	});

	emulate("cmovo ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_OF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVP_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_PF, true},
	});

	emulate("cmovp ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_PF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVP_no_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_PF, false},
	});

	emulate("cmovp ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_PF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_CMOVS
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVS_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, true},
	});

	emulate("cmovs ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x2222},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CMOVS_no_move)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_AX, 0x1111},
		{X86_REG_CX, 0x2222},
		{X86_REG_SF, false},
	});

	emulate("cmovs ax, cx");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_AX, X86_REG_CX, X86_REG_SF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0x1111},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_FCMOVB
//

// DA C0+i	FCMOVB ST(0), ST(i)		Move if below (CF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVB_move_1)
{
	ALL_MODES;

	setRegisters({
		 {X87_REG_TOP, 0x5},
		 {X86_REG_ST5, 3.14}, // st(0)
		 {X86_REG_ST6, 15.7}, // st(1)
		 {X86_REG_CF, true},
	});

	emulate("fcmovb st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DA C0+i	FCMOVB ST(0), ST(i)		Move if below (CF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVB_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 15.7}, // st(3)
		{X86_REG_CF, false},
	});

	emulate("fcmovb st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCMOVE
//

// DA C8+i	FCMOVE ST(0), ST(i)		Move if equal (ZF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_ZF, true},
	});

	emulate("fcmove st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DA C8+i	FCMOVE ST(0), ST(i)		Move if equal (ZF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 15.7}, // st(3)
		{X86_REG_ZF, false},
	});

	emulate("fcmove st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCMOVBE
//

// DA D0+i	FCMOVBE ST(0), ST(i)	Move if below or equal (CF=1 or ZF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVBE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_CF, true},
		{X86_REG_ZF, true},
	});

	emulate("fcmovbe st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DA D0+i	FCMOVBE ST(0), ST(i)	Move if below or equal (CF=1 or ZF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVBE_move_2)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});

	emulate("fcmovbe st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DA D0+i	FCMOVBE ST(0), ST(i)	Move if below or equal (CF=1 or ZF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVBE_move_3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});

	emulate("fcmovbe st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DA D0+i	FCMOVBE ST(0), ST(i)	Move if below or equal (CF=1 or ZF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVBE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 15.7}, // st(3)
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});

	emulate("fcmovbe st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCMOVU
//

// DA D8+i	FCMOVU ST(0), ST(i)		Move if unordered (PF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVU_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_PF, true},
	});

	emulate("fcmovu st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_PF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DA D8+i	FCMOVU ST(0), ST(i)		Move if unordered (PF=1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVU_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 15.7}, // st(3)
		{X86_REG_PF, false},
	});

	emulate("fcmovu st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5, X86_REG_PF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCMOVNB
//

// DB C0+i	FCMOVNB ST(0), ST(i)	Move if not below (CF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNB_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_CF, false},
	});

	emulate("fcmovnb st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DB C0+i	FCMOVNB ST(0), ST(i)	Move if not below (CF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNB_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 15.7}, // st(3)
		{X86_REG_CF, true},
	});

	emulate("fcmovnb st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5, X86_REG_CF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCMOVNE
//

// DB C8+i	FCMOVNE ST(0), ST(i)	Move if not equal (ZF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_ZF, false},
	});

	emulate("fcmovne st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DB C8+i	FCMOVNE ST(0), ST(i)	Move if not equal (ZF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 15.7}, // st(3)
		{X86_REG_ZF, true},
	});

	emulate("fcmovne st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCMOVNBE
//

// DB D0+i	FCMOVNBE ST(0), ST(i)	Move if not below or equal (CF=0 and ZF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNBE_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});

	emulate("fcmovnbe st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DB D0+i	FCMOVNBE ST(0), ST(i)	Move if not below or equal (CF=0 and ZF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNBE_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});

	emulate("fcmovnbe st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DB D0+i	FCMOVNBE ST(0), ST(i)	Move if not below or equal (CF=0 and ZF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNBE_no_move_2)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});

	emulate("fcmovnbe st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DB D0+i	FCMOVNBE ST(0), ST(i)	Move if not below or equal (CF=0 and ZF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNBE_no_move_3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 15.7}, // st(3)
		{X86_REG_CF, true},
		{X86_REG_ZF, true},
	});

	emulate("fcmovnbe st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5, X86_REG_CF, X86_REG_ZF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCMOVNU
//

// DB D8+i	FCMOVNU ST(0), ST(i)	Move if not unordered (PF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVUN_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
		{X86_REG_ST6, 15.7}, // st(1)
		{X86_REG_PF, false},
	});

	emulate("fcmovnu st(0), st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ST6, X86_REG_PF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 15.7},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DB D8+i	FCMOVNU ST(0), ST(i)	Move if not unordered (PF=0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCMOVNU_no_move_1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 15.7}, // st(3)
		{X86_REG_PF, true},
	});

	emulate("fcmovnu st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5, X86_REG_PF});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FLD
//

// D9 /0	FLD m32fp	Push m32fp onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLD_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
	});

	setMemory({
		{0x1234, 3.14_f32},
	});

	emulate("fld dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST4, 3.14},
		{X87_REG_TOP, 0x4},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DD /0	FLD m64fp	Push m64fp onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLD_m64)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
	});

	setMemory({
		{0x1234, 3.14_f64},
	});

	emulate("fld qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST4, 3.14},
		{X87_REG_TOP, 0x4},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D9 C0+i	FLD ST(i)	Push ST(i) onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLD_stX)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x4},
		{X86_REG_ST6, 3.14}, // st(2)
	});

	emulate("fld st(2)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST6});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST3, 3.14},
		{X87_REG_TOP, 0x3},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FILD
//

// DF /0	FILD m16int	Push m16int onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FILD_m16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
	});

	setMemory({
		{0x1234, 123_w},
	});

	emulate("fild word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST4, 123.0},
		{X87_REG_TOP, 0x4},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DB /0	FILD m32int	Push m32int onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FILD_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
	});

	setMemory({
		{0x1234, 123_dw},
	});

	emulate("fild dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST4, 123.0},
		{X87_REG_TOP, 0x4},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DF /5	FILD m64int	Push m64int onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FILD_m64)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
	});

	setMemory({
		{0x1234, 123_qw},
	});

	emulate("fild qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST4, 123.0},
		{X87_REG_TOP, 0x4},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FBLD
//

// DF /4	FBLD m80dec		Convert BCD value to floating-point and push onto the FPU stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FBLD)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
	});

	setMemory({
		{0x1234, 1234.0},
	});

	emulate("fbld [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, ANY},
		{X87_REG_TOP, 0x4},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("__asm_fbld"), {1234.0}},
	});
}

//
// X86_INS_FBSTP
//

// DF /6	FBSTP m80bcd	Store ST(0) in m80bcd and pop ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FBSTP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 1234.0},
	});

	emulate("fbstp [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x6},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("__asm_fbstp"), {ANY}},
	});
}

//
// X86_INS_FST
//

// D9 /2	FST m32fp	Copy ST(0) to m32fp.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FST_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14}, // st(0)
	});

	emulate("fst dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3.14_f32},
	});
}

// DD /2	FST m64fp	Copy ST(0) to m64fp.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FST_m64)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14}, // st(0)
	});

	emulate("fst qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3.14_f64},
	});
}

// DD D0+i	FST ST(i)	Copy ST(0) to ST(i).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FST_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14}, // st(0)
	});

	emulate("fst st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST4, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FSTP
//

// D9 /3	FSTP m32fp	Copy ST(0) to m32fp and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSTP_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14}, // st(0)
	});

	emulate("fstp dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3.14_f32},
	});
}

// DD /3	FSTP m64fp	Copy ST(0) to m64fp and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSTP_m64)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14}, // st(0)
	});

	emulate("fstp qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3.14_f64},
	});
}

// DD D8+i	FSTP ST(i)	Copy ST(0) to ST(i) and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSTP_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14}, // st(0)
	});

	emulate("fstp st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST4, 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FIST
//

// DF /2	FIST m16int	Store ST(0) in m16int.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIST_16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14},
	});

	emulate("fist word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3_w},
	});
}

// DB /2	FIST m32int	Store ST(0) in m32int.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIST_32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14},
	});

	emulate("fist dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3_dw},
	});
}

//
// X86_INS_FISTP
//

// DF /3	FISTP m16int	Store ST(0) in m16int and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISTP_16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14},
	});

	emulate("fistp word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3_w},
	});
}

// DB /3	FISTP m32int	Store ST(0) in m32int and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISTP_32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14},
	});

	emulate("fistp dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3_dw},
	});
}

// DF /7	FISTP m64int	Store ST(0) in m64int and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISTP_64)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.14},
	});

	emulate("fistp qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3_qw},
	});
}

//
// X86_INS_FISTTP
//

// DF /1	FISTTP m16int	Store ST(0) in m16int with truncation.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISTTP_16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.7}, // should trunc to 3
	});

	emulate("fisttp word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3_w},
	});
}

// DB /1	FISTTP m32int	Store ST(0) in m32int with truncation.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISTTP_32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.7}, // should trunc to 3
	});

	emulate("fisttp dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3_dw},
	});
}

// DD /1	FISTTP m64int	Store ST(0) in m64int with truncation.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISTTP_64)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 3.7}, // should trunc to 3
	});

	emulate("fisttp qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
	});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 3_qw},
	});
}

//
// X86_INS_FMUL
//

// D8 /1	FMUL m32fp	Multiply ST(0) by m32fp and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FMUL_d8)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.14_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
	});

	emulate("fmul dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 * 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DC /1	FMUL m64fp	Multiply ST(0) by m64fp and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FMUL_dc)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.14_f64},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
	});

	emulate("fmul qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 * 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 C8+i	FMUL ST(0), ST(i)	Multiply ST(0) by ST(i) and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FMUL_d8_c8)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fmul st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 * 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DC C8+i	FMUL ST(i), ST(0)	Multiply ST(i) by ST(0) and store result in ST(i)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FMUL_dc_c8)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fmul st(3), st(0)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 * 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FMUL_mem_complex)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_ECX, 0x100},
		{X86_REG_EDX, 0x4},
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14},
	});
	setMemory({
		{0x1354, 3.14}
	});

	emulate("fmul qword ptr [0x1234 + ecx + edx * 8");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5, X86_REG_ECX, X86_REG_EDX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 * 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1354});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FMULP
//

// DE C8+i	FMULP ST(i), ST(0)	Multiply ST(i) by ST(0), store result in ST(i), and pop the register stack
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FMULP_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fmulp st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST5, 3.14 * 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DE C9	FMULP	Multiply ST(1) by ST(0), store result in ST(1), and pop the register stack
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FMULP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST3, 3.14}, // st(1)
	});

	emulate("fmulp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, 3.14 * 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FIMUL
//

// DA /1	FIMUL m32int	Multiply ST(0) by m32int and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIMUL_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_dw},
	});

	emulate("fimul dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 * 3.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DE /1	FIMUL m16int	Multiply ST(0) by m16int and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIMUL_m16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_w},
	});

	emulate("fimul word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 * 3.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FADD
//

// D8 /0	FADD m32fp	Add m32fp to ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FADD_d8)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.14_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
	});

	emulate("fadd dword ptr [0x1234]");

	// FADD m32fp does NOT pop -- TOP is read to find st(0) and not written.
	// This test asserted a pop until the translator stopped emitting one.
	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 + 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DC /0	FADD m64fp	Add m64fp to ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FADD_dc)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.14_f64},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
	});

	emulate("fadd qword ptr [0x1234]");

	// FADD m64fp does not pop either.
	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 + 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 C0+i	FADD ST(0), ST(i)	Add ST(0) to ST(i) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FADD_d8_c0)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fadd st(0), st(3)");

	// Only the DE form pops. Confirmed on the hardware with fnstsw either
	// side of each encoding.
	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 + 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DC C0+i	FADD ST(i), ST(0)	Add ST(i) to ST(0) and store result in ST(i).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FADD_dc_c0)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fadd st(3), st(0)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 + 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FADDP
//

// DE C0+i	FADDP ST(i), ST(0)	Add ST(0) to ST(i), store result in ST(i), and pop the register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FADDP_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("faddp st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST5, 3.14 + 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DE C1	FADDP	Add ST(0) to ST(1), store result in ST(1), and pop the register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FADDP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST3, 3.14}, // st(1)
	});

	emulate("faddp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, 3.14 + 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FIADD
//

// DA /0	FIADD m32int	Add m32int to ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIADD_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_dw},
	});

	emulate("fiadd dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 + 3.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DE /0	FIADD m16int	Add m16int to ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIADD_m16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_w},
	});

	emulate("fiadd word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 + 3.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FTST
//

// D9 E4	FTST	Compare ST(0) with 0.0.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FTST_gt)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	emulate("ftst");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, false},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// D9 E4	FTST	Compare ST(0) with 0.0.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FTST_lt)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, -3.14}, // st(0)
	});

	emulate("ftst");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// D9 E4	FTST	Compare ST(0) with 0.0.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FTST_eq)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 0.0}, // st(0)
	});

	emulate("ftst");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, false},
		{X87_REG_C2, false},
		{X87_REG_C3, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCOM
//

// D8 /2	FCOM m32fp	Compare ST(0) with m32fp.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOM_m32_gt)
{
	ALL_MODES;

	setMemory({
		{0x1234, 2.0_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
	});

	emulate("fcom dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, false},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 /2	FCOM m32fp	Compare ST(0) with m32fp.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOM_m32_lt)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.0_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 2.0}, // st(0)
	});

	emulate("fcom dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 /2	FCOM m32fp	Compare ST(0) with m32fp.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOM_m32_eq)
{
	ALL_MODES;

	setMemory({
		{0x1234, 2.0_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 2.0}, // st(0)
	});

	emulate("fcom dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, false},
		{X87_REG_C2, false},
		{X87_REG_C3, true},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DC /2	FCOM m64fp	Compare ST(0) with m64fp.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOM_m64_lt)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.0_f64},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 2.0}, // st(0)
	});

	emulate("fcom qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 D0+i	FCOM ST(i)	Compare ST(0) with ST(i).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOM_stX)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fcom st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// D8 D1	FCOM	Compare ST(0) with ST(1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOM_st1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST3, 5.0}, // st(1)
	});

	emulate("fcom");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCOMP
//

// D8 D8+i	FCOMP ST(i)	Compare ST(0) with ST(i) and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOMP_stX)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST3, 5.0}, // st(1)
	});

	emulate("fcomp st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCOMPP
//

// DE D9	FCOMPP	Compare ST(0) with ST(1) and pop register stack twice.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOMPP_stX)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST3, 5.0}, // st(1)
	});

	emulate("fcompp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x4},
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FUCOM
//

// DD E0+i	FUCOM ST(i)	Compare ST(0) with ST(i).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FUCOM_stX)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fucom st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DD E1	FUCOM	Compare ST(0) with ST(1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FUCOM_st1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST3, 5.0}, // st(1)
	});

	emulate("fucom");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FUCOMP
//

// DD E8+i	FUCOMP ST(i)	Compare ST(0) with ST(i) and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FUCOMP_stX)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST3, 5.0}, // st(1)
	});

	emulate("fucomp st(1)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DD E9	FUCOMP	Compare ST(0) with ST(1) and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FUCOMP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST3, 5.0}, // st(1)
	});

	emulate("fucomp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FUCOMPP
//

// DA E9	FUCOMPP	Compare ST(0) with ST(1) and pop register stack twice.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FUCOMPP_stX)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST3, 5.0}, // st(1)
	});

	emulate("fucompp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x4},
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCOMI
//

// DB F0+i	FCOMI ST, ST(i)	Compare ST(0) with ST(i) and set status flags accordingly.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOMI_lt)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fcomi st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, false},
		{X86_REG_PF, false},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCOMIP
//

// DF F0+i	FCOMIP ST, ST(i)	Compare ST(0) with ST(i), set status flags
// accordingly, and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOMIP_lt)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fcomip st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ZF, false},
		{X86_REG_PF, false},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FUCOMI
//

// DB E8+i	FUCOMI ST, ST(i)	Compare ST(0) with ST(i), check for ordered values,
// and set status flags accordingly.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FUCOMI_lt)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fucomi st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, false},
		{X86_REG_PF, false},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FUCOMIP
//

// DF E8+i	FUCOMIP ST, ST(i)	Compare ST(0) with ST(i), check for ordered values,
// set status flags accordingly, and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FUCOMIP_lt)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fucomip st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ZF, false},
		{X86_REG_PF, false},
		{X86_REG_CF, true},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FICOM
//

// DE /2	FICOM m16int	Compare ST(0) with m16int.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FICOM_m16_gt)
{
	ALL_MODES;

	setMemory({
		{0x1234, 2_w},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 1.0}, // st(0)
	});

	emulate("ficom word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DA /2	FICOM m32int	Compare ST(0) with m32int.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FICOM_m32_gt)
{
	ALL_MODES;

	setMemory({
		{0x1234, 2_dw},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 1.0}, // st(0)
	});

	emulate("ficom dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FICOMP
//

// DE /2	FICOM m16int	Compare ST(0) with m16int.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FICOMP_m16_gt)
{
	ALL_MODES;

	setMemory({
		{0x1234, 2_w},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 1.0}, // st(0)
	});

	emulate("ficomp word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DA /2	FICOMP m32int	Compare ST(0) with m32int.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FICOMP_m32_gt)
{
	ALL_MODES;

	setMemory({
		{0x1234, 2_dw},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 1.0}, // st(0)
	});

	emulate("ficomp dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X87_REG_C0, true},
		{X87_REG_C2, false},
		{X87_REG_C3, false},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FDIV
//

// D8 /6	FDIV m32fp	Divide ST(0) by m32fp and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIV_d8)
{
	ALL_MODES;

	setMemory({
		{0x1234, 5.0_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
	});

	emulate("fdiv dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 10.0 / 5.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DC /6	FDIV m64fp	Divide ST(0) by m64fp and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIV_dc)
{
	ALL_MODES;

	setMemory({
		{0x1234, 5.0_f64},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
	});

	emulate("fdiv qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 10.0 / 5.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 F0+i	FDIV ST(0), ST(i)	Divide ST(0) by ST(i) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIV_st0_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fdiv st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 10.0 / 5.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DC F8+i	FDIV ST(i), ST(0)	Divide ST(i) by ST(0) and store result in ST(i).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIV_st3_st0)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fdiv st(3), st(0)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 5.0 / 10.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FDIVP
//

// DE F8+i	FDIVP ST(i), ST(0)	Divide ST(i) by ST(0), store result in ST(i),
// and pop the register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIVP_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.123}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fdivp st(3), st(0)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST5, 3.14 / 10.123},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DE F9	FDIVP	Divide ST(1) by ST(0), store result in ST(1), and pop the
// register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIVP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.123}, // st(0)
		{X86_REG_ST3, 3.14}, // st(1)
	});

	emulate("fdivp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, 3.14 / 10.123},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FIDIV
//

// DE /6	FIDIV m16int	Divide ST(0) by m64int and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIDIV_m16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_w},
	});

	emulate("fidiv word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 / 3.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DA /6	FIDIV m32int	Divide ST(0) by m32int and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIDIV_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_dw},
	});

	emulate("fidiv dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 / 3.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FDIVR
//

// D8 /7	FDIVR m32fp	Divide m32fp by ST(0) and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIVR_d8)
{
	ALL_MODES;

	setMemory({
		{0x1234, 5.0_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
	});

	emulate("fdivr dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 5.0 / 10.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DC /7	FDIVR m64fp	Divide m64fp by ST(0) and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIVR_dc)
{
	ALL_MODES;

	setMemory({
		{0x1234, 5.0_f64},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
	});

	emulate("fdivr qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 5.0 / 10.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 F8+i	FDIVR ST(0), ST(i)	Divide ST(i) by ST(0) and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIVR_st0_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fdivr st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 5.0 / 10.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DC F0+i	FDIVR ST(i), ST(0)	Divide ST(0) by ST(i) and store result in ST(i)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIVR_st3_st0)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
		{X86_REG_ST5, 5.0}, // st(3)
	});

	emulate("fdivr st(3), st(0)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 10.0 / 5.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FDIVRP
//

// DE F0+i	FDIVRP ST(i), ST(0)	Divide ST(0) by ST(i), store result in ST(i),
// and pop the register stack
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIVRP_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.123}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fdivrp st(3), st(0)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST5, 10.123 / 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DE F1	FDIVRP	Divide ST(0) by ST(1), store result in ST(1), and pop the
// register stack
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDIVRP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.123}, // st(0)
		{X86_REG_ST3, 3.14}, // st(1)
	});

	emulate("fdivrp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, 10.123 / 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FIDIVR
//

// DE /7	FIDIVR m16int	Divide m16int by ST(0) and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIDIVR_m16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 10_w},
	});

	emulate("fidivr word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 10.0 / 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DA /7	FIDIVR m32int	Divide m32int by ST(0) and store result in ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FIDIVR_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 10_dw},
	});

	emulate("fidivr dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 10.0 / 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FPREM
//

// D9 F8	FPREM		Replace ST(0) with the remainder obtained from dividing ST(0) by ST(1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FPREM)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST3, 3.0}, // st(1)
	});

	emulate("fprem");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, fmod(3.14, 3.0)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FPREM1
//

// D9 F5	FPREM1		Replace ST(0) with the IEEE remainder obtained from dividing ST(0) by ST(1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FPREM1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x4},
		{X86_REG_ST4, 3.6}, // st(0)
		{X86_REG_ST5, 3.0}, // st(1)
	});

	emulate("fprem1");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST4, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST4, fmod(3.6, 3.0)},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FSUB
//

// D8 /4	FSUB m32fp	Subtract m32fp from ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUB_d8)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.14_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
	});

	emulate("fsub dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 10.0 - 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DC /4	FSUB m64fp	Subtract m64fp from ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUB_dc)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.14_f64},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
	});

	emulate("fsub qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 10.0 - 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 E0+i	FSUB ST(0), ST(i)	Subtract ST(i) from ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUB_d8_e0)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fsub st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 10.0 - 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DC E8+i	FSUB ST(i), ST(0)	Subtract ST(0) from ST(i) and store result in ST(i).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUB_dc_e8)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 10.0}, // st(3)
	});

	emulate("fsub st(3), st(0)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 10.0 - 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FSUBP
//

// DE E8+i	FSUBP ST(i), ST(0)	Subtract ST(0) from ST(i), store result in ST(i),
// and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUBP_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 10.0}, // st(3)
	});

	emulate("fsubp st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST5, 10.0 - 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DE E9	FSUBP	Subtract ST(0) from ST(1), store result in ST(1),
// and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUBP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST3, 10.0}, // st(1)
	});

	emulate("fsubp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, 10.0 - 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FISUB
//

// DA /4	FISUB m32int	Subtract m32int from ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISUB_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_dw},
	});

	emulate("fisub dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 - 3.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DE /4	FISUB m16int	Subtract m16int from ST(0) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISUB_m16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_w},
	});

	emulate("fisub word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.14 - 3.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FSUBR
//

// D8 /5	FSUBR m32fp	Subtract ST(0) from m32fp and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUBR_d8)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.14_f32},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
	});

	emulate("fsubr dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 - 10.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DC /5	FSUBR m64fp	Subtract ST(0) from m64fp and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUBR_dc)
{
	ALL_MODES;

	setMemory({
		{0x1234, 3.14_f64},
	});

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
	});

	emulate("fsubr qword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 - 10.0},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// D8 E8+i	FSUBR ST(0), ST(i)	Subtract ST(0) from ST(i) and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUBR_d8_e0)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fsubr st(0), st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14 - 10.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DC E0+i	FSUBR ST(i), ST(0)	Subtract ST(i) from ST(0) and store result in ST(i).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUBR_dc_e8)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0}, // st(0)
		{X86_REG_ST5, 3.14}, // st(3)
	});

	emulate("fsubr st(3), st(0)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 10.0 - 3.14},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FSUBRP
//

// DE E0+i	FSUBRP ST(i), ST(0)	Subtract ST(i) from ST(0), store result in ST(i),
// and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUBRP_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST5, 10.0}, // st(3)
	});

	emulate("fsubrp st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST5, 3.14 - 10.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// DE E1	FSUBRP	Subtract ST(1) from ST(0), store result in ST(1),
// and pop register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSUBRP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 3.14}, // st(0)
		{X86_REG_ST3, 10.0}, // st(1)
	});

	emulate("fsubrp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, 3.14 - 10.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FISUBR
//

// DA /5	FISUBR m32int	Subtract ST(0) from m32int and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISUBR_m32)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_dw},
	});

	emulate("fisubr dword ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.0 - 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

// DE /5	FISUBR m16int	Subtract ST(0) from m16int and store result in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FISUBR_m16)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x5},
		{X86_REG_ST5, 3.14}, // st(0)
	});

	setMemory({
		{0x1234, 3_w},
	});

	emulate("fisubr word ptr [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST5, 3.0 - 3.14},
	});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
}

//
// X86_INS_FABS
// llvm.fabs.*() can not be lowered, so we need to check call.
//

// D9 E1	FABS	Replace ST with its absolute value.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FABS)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 10.0},
	});

	emulate("fabs");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST1, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f80"), {10.0}},
	});
}

//
// X86_INS_FCHS
//

// D9 E0	FCHS	Complements sign of ST(0)
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCHS)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 10.0},
	});

	emulate("fchs");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST1, -10.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FSQRT
// llvm.sqrt.*() is transformed to sqrtl().
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSQRT)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 10.0},
	});

	emulate("fsqrt");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST1, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("sqrtl"), {10.0}},
	});
}

//
// X86_INS_FSCALE
//

// D9 FD	FSCALE		Scale ST(0) by ST(1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSCALE)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x0},
		{X86_REG_ST0, 10.0},
		{X86_REG_ST1, 4.4},
	});

	emulate("fscale");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST0, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST0, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("roundl"), {4.4}},
		{_module.getFunction("exp2l"), {ANY}},
	});
}

//
// X86_INS_FXCH
//

// D9 C9	FXCH	Exchange the contents of ST(0) and ST(1).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXCH)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0},
		{X86_REG_ST3, 3.14},
	});

	emulate("fxch");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
		{X86_REG_ST3, 10.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// D9 C8+i	FXCH ST(i)	Exchange the contents of ST(0) and ST(i).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXCH_st3)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0},
		{X86_REG_ST5, 3.14},
	});

	emulate("fxch st(3)");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST5});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14},
		{X86_REG_ST5, 10.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FCOS
//

// D9 FF	FCOS	Replace ST(0) with its cosine.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FCOS_compute)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 10.0}, // st(0)
	});

	emulate("fcos");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST1, ANY},
		{X87_REG_C2, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f80"), {10.0}},
		{_module.getFunction("cosl"), {10.0}},
	});
}

//
// X86_INS_FSIN
//

// D9 FE	FSIN		Replace ST(0) with the approximate of its sine.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSIN_compute)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 10.0},
	});

	emulate("fsin");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST1, ANY},
		{X87_REG_C2, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f80"), {10.0}},
		{_module.getFunction("sinl"), {10.0}},
	});
}

//
// X86_INS_FSINCOS
//

// D9 FB	FSINCOS		Compute the sine and cosine of ST(0); replace ST(0) with the approximate sine,
// and push the approximate cosine onto the register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FSINCOS_compute)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 10.0},
	});

	emulate("fsincos");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST1, ANY},
		{X86_REG_ST0, ANY},
		{X87_REG_TOP, 0x0},
		{X87_REG_C2, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("llvm.fabs.f80"), {10.0}},
		{_module.getFunction("sinl"), {10.0}},
		{_module.getFunction("cosl"), {10.0}},
	});
}

//
// X86_INS_FPATAN
//

// D9 F3	FPATAN		Replace ST(1) with arctan(ST(1)/ST(0)) and pop the register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FPATAN_compute)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST2, 20.0},
		{X86_REG_ST1, 10.0},
	});

	emulate("fpatan");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("__asm_fpatan"), {20.0, 10.0}},
	});
}

//
// X86_INS_FPTAN
//

// D9 F2	FPTAN		Replace ST(0) with its approximate tangent and push 1 onto the FPU stack.
//Description
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FPTAN_compute)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 10.0},
	});

	emulate("fptan");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST2, ANY},
		{X86_REG_ST1, 1.0},
		{X87_REG_C2, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("__asm_fptan"), {10.0}},
	});
}

//
// X86_INS_F2XM1
//

// D9 F0	F2XM1		Replace ST(0) with 2^{ST(0) – 1}.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_F2XM1_compute)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 17.0},
	});

	emulate("f2xm1");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST1});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST1, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("exp2l"), {16.0}},
	});
}

//
// X86_INS_FYL2X
//

// D9 F1	FYL2X		Replace ST(1) with (ST(1) ∗ log2ST(0)) and pop the register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FYL2X_compute)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 16.0},
		{X86_REG_ST3, 7.0},
	});

	emulate("fyl2x");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("log2l"), {16.0}},
	});
}

//
// X86_INS_FYL2XP1
//

// D9 F9	FYL2XP1		Replace ST(1) with ST(1) ∗ log2(ST(0) + 1.0) and pop the register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FYL2XP1_compute)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 16.0},
		{X86_REG_ST3, 7.0},
	});

	emulate("fyl2xp1");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST2, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("log2l"), {17.0}},
	});
}

//
// X86_INS_FLD1
//

// D9 E8	FLD1	Push +1.0 onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLD1)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fld1");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 1.0},
		{X87_REG_TOP, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FLDL2T
//

// D9 E9	FLDL2T	Push log_2(10) onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLDL2T)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fldl2t");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, static_cast<double>(std::log2(10.0L))},
		{X87_REG_TOP, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FLDL2E
//

// D9 EA	FLDL2E	Push log_2(e) onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLDL2E)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fldl2e");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, static_cast<double>(std::log2(std::exp(1.0L)))},
		{X87_REG_TOP, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FLDPI
//

// D9 EB	FLDPI	Push pi onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLDPI)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fldpi");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.14159265358979323846},
		{X87_REG_TOP, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FLDLG2
//

// D9 EC	FLDLG2	Push log_10(2) onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLDLG2)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fldlg2");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, static_cast<double>(std::log10(2.0L))},
		{X87_REG_TOP, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FLDLN2
//

// D9 ED	FLDLN2	Push log_e(2) onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLDLN2)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fldln2");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, static_cast<double>(std::log(2.0L))},
		{X87_REG_TOP, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FLDZ
//

// D9 EE	FLDZ	Push +0.0 onto the FPU register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLDZ)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fldz");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 0.0},
		{X87_REG_TOP, 2},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FINCSTP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FINCSTP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fincstp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x4},
		{X87_REG_C1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FINCSTP_rotate)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x7},
	});

	emulate("fincstp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x0},
		{X87_REG_C1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FDECSTP
//

// D9 F6	FDECSTP	Decrement TOP field in FPU status word.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDECSTP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
	});

	emulate("fdecstp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x2},
		{X87_REG_C1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

// D9 F6	FDECSTP	Decrement TOP field in FPU status word.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FDECSTP_rotate)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x0},
	});

	emulate("fdecstp");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP});
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x7},
		{X87_REG_C1, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FRNDINT
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FRNDINT)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, 10.123},
	});

	emulate("frndint");

	EXPECT_JUST_REGISTERS_LOADED({X87_REG_TOP, X86_REG_ST3});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST3, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("roundl"), {10.123}}, // not llvm.round.f80
	});
}

//
// X86_INS_CPUID
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CPUID)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
	});

	emulate("cpuid");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, ANY},
		{X86_REG_EBX, ANY},
		{X86_REG_ECX, ANY},
		{X86_REG_EDX, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_cpuid"), {0x1234}},
	});
}

//
// X86_INS_OUTSB
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_OUTSB)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0x1234},
		{X86_REG_SI, 0x1234},
	});
	setMemory({
		{0x1234, 0x56_b},
	});

	emulate("outsb");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX, X86_REG_SI});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_outsb"), {0x1234, 0x56}},
	});
}

//
// X86_INS_OUTSW
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_OUTSW)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0x1234},
		{X86_REG_SI, 0x1234},
	});
	setMemory({
		{0x1234, 0x5678_w},
	});

	emulate("outsw");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX, X86_REG_SI});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_outsw"), {0x1234, 0x5678}},
	});
}

//
// X86_INS_OUTSD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_OUTSD)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0x1234},
		{X86_REG_SI, 0x1234},
	});
	setMemory({
		{0x1234, 0x567890ab_dw},
	});

	emulate("outsd");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX, X86_REG_SI});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_outsd"), {0x1234, 0x567890ab}},
	});
}

//
// X86_INS_INSB
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_INSB)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0x1234},
		{X86_REG_DI, 0x5678},
	});

	emulate("insb");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX, X86_REG_DI});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x5678, ANY}
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_insb"), {0x1234}},
	});
}

//
// X86_INS_INSW
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_INSW)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0x1234},
		{X86_REG_DI, 0x5678},
	});

	emulate("insw");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX, X86_REG_DI});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x5678, ANY}
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_insw"), {0x1234}},
	});
}

//
// X86_INS_INSD
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_INSD)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_DX, 0x1234},
		{X86_REG_DI, 0x5678},
	});

	emulate("insd");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_DX, X86_REG_DI});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x5678, ANY}
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_insd"), {0x1234}},
	});
}

//
// X86_INS_RDTSC
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RDTSC)
{
	SKIP_MODE_16;

	emulate("rdtsc");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, ANY},
		{X86_REG_EDX, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_rdtsc"), {}},
	});
}

//
// X86_INS_RDTSCP
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RDTSCP)
{
	SKIP_MODE_16;

	emulate("rdtscp");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, ANY},
		{X86_REG_EDX, ANY},
		{X86_REG_ECX, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_rdtscp"), {}},
	});
}

//
// X86_INS_FNSTSW
//

// DD /7	FNSTSW m2byte	Store FPU status word at m2byte without checking for pending unmasked floating-point exceptions.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FNSTSW_m2byte)
{
	ALL_MODES;

	setRegisters({
		{X86_REG_FPSW, 0xFF},
	});

	emulate("fnstsw [0x1234]");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_FPSW});
	EXPECT_NO_REGISTERS_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, 0xFF_dw},
	});
}

// DF E0	FNSTSW AX	Store FPU status word in AX register without checking for pending unmasked floating-point exceptions.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FNSTSW_AX)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_FPSW, 0xFF},
	});

	emulate("fnstsw AX");

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_FPSW, X86_REG_AX});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_AX, 0xFF},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// X86_INS_FNCLEX
//

// DB E2	FNCLEX		Clear floating-point exception flags without checking
// 						for pending unmasked floating-point exceptions.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FNCLEX)
{
	ALL_MODES;

	emulate("fnclex");

	EXPECT_NO_REGISTERS_LOADED();
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_FPSW, ANY},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fnclex"), {}},
	});
}

//
// X86_INS_FLDCW
//

// D9 /5	FLDCW m2byte	Load FPU control word from m2byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLDCW)
{
	ALL_MODES;

	emulate("fldcw [0x1234]");

	// fldcw to NOP because FPU control world is not supported
	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_FLDENV
//

// D9 /4	FLDENV m14/28byte	Load FPU environment from m14byte or m28byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FLDENV)
{
	ALL_MODES;

	setMemory({
		{0x1234, 0xf},
	});

	emulate("fldenv [0x1234]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fldenv"), {0xf}},
	});
}

//
// X86_INS_FNSAVE
//

// DD /6	FNSAVE* m94/108byte		Store FPU environment to m94byte or
// m108byte without checking for pending unmasked floating-point exceptions.
// Then re-initialize the FPU.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FNSAVE)
{
	ALL_MODES;

	emulate("fnsave [0x1234]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, ANY},
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fnsave"), {}},
	});
}

//
// X86_INS_FRSTOR
//

// DD /4	FRSTOR m94/108byte	Load FPU state from m94byte or m108byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FRSTOR)
{
	ALL_MODES;

	setMemory({
		{0x1234, 0xffff},
	});

	emulate("frstor [0x1234]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_frstor"), {0xffff}},
	});
}

//
// X86_INS_FNSTENV
//

// D9 /6	FNSTENV* m14/28byte		Store FPU environment to m14byte or m28byte
// without checking for pending unmasked floating-point exceptions. Then mask
// all floating-point exceptions.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FNSTENV)
{
	ALL_MODES;

	emulate("fnstenv [0x1234]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, ANY},
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fnstenv"), {}},
	});
}

//
// X86_INS_FNSTCW
//

// D9 /7	FNSTCW* m2byte		Store FPU control word to m2byte without checking
// for pending unmasked floating-point exceptions.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FNSTCW)
{
	ALL_MODES;

	emulate("fnstcw [0x1225]");

	// translate like NOP because FPU control word is not supported in decompiler
	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_FXSAVE
//

// 0F AE /0		FXSAVE m512byte		Save the x87 FPU, MMX, XMM, and MXCSR register state to m512byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXSAVE_memory_operand)
{
	SKIP_MODE_64;

	emulate("fxsave [0x1234]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, ANY},
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fxsave"), {}},
	});
}

// 0F AE /0		FXSAVE m512byte		Save the x87 FPU, MMX, XMM, and MXCSR register state to m512byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXSAVE_register_operand)
{
	ONLY_MODE_32;

	setRegisters({
		{X86_REG_EAX, 0x1234},
	});

	emulate("fxsave [eax]");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, ANY},
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fxsave"), {}},
	});
}

//
// X86_INS_FXSAVE64
//

// REX.W+ 0F AE /0		FXSAVE64 m512byte	Save the x87 FPU, MMX, XMM, and MXCSR register state to m512byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXSAVE64_memory_operand)
{
	ONLY_MODE_64;

	emulate("fxsave64 [0x1234]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, ANY},
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fxsave64"), {}},
	});
}

// REX.W+ 0F AE /0		FXSAVE64 m512byte	Save the x87 FPU, MMX, XMM, and MXCSR register state to m512byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXSAVE64_register_operand)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_EAX, 0x1234},
	});

	emulate("fxsave64 [eax]");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_NO_MEMORY_LOADED();
	EXPECT_JUST_MEMORY_STORED({
		{0x1234, ANY},
	});
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fxsave64"), {}},
	});
}

//
// X86_INS_FXRSTOR
//

// 0F AE /1		FXRSTOR m512byte	Restore the x87 FPU, MMX, XMM, and MXCSR register state from m512byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXRSTOR_memory_operand)
{
	SKIP_MODE_64;

	setMemory({
		{0x1234, 0xffff},
	});

	emulate("fxrstor [0x1234]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fxrstor"), {0xffff}},
	});
}

// 0F AE /1		FXRSTOR m512byte	Restore the x87 FPU, MMX, XMM, and MXCSR register state from m512byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXRSTOR_register_operand)
{
	ONLY_MODE_32;

	setMemory({
		{0x1234, 0xffff},
	});
	setRegisters({
		{X86_REG_EAX, 0x1234},
	});

	emulate("fxrstor [eax]");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fxrstor"), {0xffff}},
	});
}

//
// X86_INS_FXRSTOR64
//

// REX.W+ 0F AE /1	FXRSTOR64 m512byte	Restore the x87 FPU, MMX, XMM, and MXCSR register state from m512byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXRSTOR64_memory_operand)
{
	ONLY_MODE_64;

	setMemory({
		{0x1234, 0xffff},
	});

	emulate("fxrstor64 [0x1234]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fxrstor64"), {0xffff}},
	});
}

// REX.W+ 0F AE /1	FXRSTOR64 m512byte	Restore the x87 FPU, MMX, XMM, and MXCSR register state from m512byte.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXRSTOR64_register_operand)
{
	ONLY_MODE_64;

	setMemory({
		{0x1234, 0xffff},
	});
	setRegisters({
		{X86_REG_EAX, 0x1234},
	});

	emulate("fxrstor64 [eax]");

	EXPECT_NO_REGISTERS_STORED();
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_EAX});
	EXPECT_JUST_MEMORY_LOADED({0x1234});
	EXPECT_NO_MEMORY_STORED();
	EXPECT_JUST_VALUES_CALLED({
		{_module.getFunction("__asm_fxrstor64"), {0xffff}},
	});
}

//
// X86_INS_FXAM
//

// D9 E5	FXAM	Classify value or number in ST(0).
TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXAM)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 17.0},
	});

	emulate("fxam");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_FPSW, ANY},
	});
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ST1, X87_REG_TOP});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("__asm_fxam"), {17.0}},
	});
}

//
// X86_INS_FXTRACT
//

// D9 F4	FXTRACT		Separate value in ST(0) into exponent and significand,
// store exponent in ST(0), and push the significand onto the register stack.
TEST_P(Capstone2LlvmIrTranslatorX86Tests, MemoryLoadAttachesPointeeMetadata)
{
	SKIP_MODE_16;
	auto* f = translate(assemble("mov eax, dword ptr [0x1234]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, StackPushAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("push eax"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, StackPopAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("pop eax"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, XlatLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("xlatb"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CallPushAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("call 0x1234"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LeaveLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("leave"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, EnterPushAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("enter 8, 0"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, RetLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("ret"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, RetfLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("retf"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LdsLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lds eax, [0x1000]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LcallPushAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lcall 0x7:0x1234"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LjmpLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("ljmp dword ptr [0x1000]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PushaStoreAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("pushal"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PopaLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("popal"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PushfStoreAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("pushfd"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PopfLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("popfd"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, FxsaveStoreAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("fxsave [eax]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, FxrstorLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("fxrstor [eax]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, MemoryStoreAttachesPointeeMetadata)
{
	SKIP_MODE_16;
	auto* f = translate(assemble("mov dword ptr [0x1234], eax"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, RegisterStoreAttachesPointeeMetadata)
{
	SKIP_MODE_16;
	auto* f = translate(assemble("mov eax, 1"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, XchgMemEmitsAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("xchg ecx, dword ptr [eax]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, XchgMemAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("xchg ecx, dword ptr [eax]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, XchgRegRegIsNotAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("xchg eax, ecx"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(isa<AtomicRMWInst>(&*it));
	}
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockAddEmitsAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock add dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<AtomicRMWInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockAddAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock add dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, AddWithoutLockIsNotAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("add dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(isa<AtomicRMWInst>(&*it));
	}
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockSubEmitsAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock sub dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			found = true;
			EXPECT_EQ(rmw->getOperation(), AtomicRMWInst::Sub);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockSubAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock sub dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, SubMemWithoutLockIsNotAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("sub dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(isa<AtomicRMWInst>(&*it));
	}
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockCmpxchgEmitsAtomicCmpXchg)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock cmpxchg dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<AtomicCmpXchgInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockCmpxchgAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock cmpxchg dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* cx = dyn_cast<AtomicCmpXchgInst>(&*it))
		{
			if (cx->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CmpxchgWithoutLockIsNotAtomicCmpXchg)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("cmpxchg dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(isa<AtomicCmpXchgInst>(&*it));
	}
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, MfenceEmitsSeqCstFence)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("mfence"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LfenceEmitsAcquireFence)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lfence"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* fence = dyn_cast<FenceInst>(&*it))
		{
			found = true;
			EXPECT_EQ(fence->getOrdering(), AtomicOrdering::Acquire);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, SfenceEmitsReleaseFence)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("sfence"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* fence = dyn_cast<FenceInst>(&*it))
		{
			found = true;
			EXPECT_EQ(fence->getOrdering(), AtomicOrdering::Release);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockNotAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock not dword ptr [eax]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockNotEmitsAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock not dword ptr [eax]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			found = true;
			EXPECT_EQ(rmw->getOperation(), AtomicRMWInst::Xor);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, NotMemWithoutLockIsNotAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("not dword ptr [eax]"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(isa<AtomicRMWInst>(&*it));
	}
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockBtsEmitsAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock bts dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			found = true;
			EXPECT_EQ(rmw->getOperation(), AtomicRMWInst::Or);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockBtsAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock bts dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, BtsMemWithoutLockIsNotAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("bts dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(isa<AtomicRMWInst>(&*it));
	}
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockBtrEmitsAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock btr dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			found = true;
			EXPECT_EQ(rmw->getOperation(), AtomicRMWInst::And);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockBtrAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock btr dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockBtcEmitsAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock btc dword ptr [eax], ecx"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* rmw = dyn_cast<AtomicRMWInst>(&*it))
		{
			found = true;
			EXPECT_EQ(rmw->getOperation(), AtomicRMWInst::Xor);
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockBtcAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock btc dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockIncEmitsAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock inc dword ptr [eax]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<AtomicRMWInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockIncAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock inc dword ptr [eax]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockDecAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock dec dword ptr [eax]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockOrAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock or dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockXorAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock xor dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockAndAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock and dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockXaddAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock xadd dword ptr [eax], ecx"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, IncWithoutLockIsNotAtomicRmw)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("inc dword ptr [eax]"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(isa<AtomicRMWInst>(&*it));
	}
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockCmpxchg8bEmitsAtomicCmpXchg)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock cmpxchg8b [eax]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<AtomicCmpXchgInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockCmpxchg8bAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lock cmpxchg8b [eax]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* cx = dyn_cast<AtomicCmpXchgInst>(&*it))
		{
			if (cx->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, Cmpxchg8bWithoutLockIsNotAtomicCmpXchg)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("cmpxchg8b [eax]"));
	ASSERT_NE(nullptr, f);
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		EXPECT_FALSE(isa<AtomicCmpXchgInst>(&*it));
	}
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockCmpxchg16bEmitsAtomicCmpXchg)
{
	ONLY_MODE_64;
	auto* f = translate(assemble("lock cmpxchg16b [rax]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (isa<AtomicCmpXchgInst>(&*it))
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LockCmpxchg16bAttachesPointeeMetadata)
{
	ONLY_MODE_64;
	auto* f = translate(assemble("lock cmpxchg16b [rax]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* cx = dyn_cast<AtomicCmpXchgInst>(&*it))
		{
			if (cx->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_FXTRACT)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x1},
		{X86_REG_ST1, 17.0},
	});

	emulate("fxtract");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST1, ANY},
		{X86_REG_ST0, ANY},
		{X87_REG_TOP, 0x0},
	});
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_ST1, X87_REG_TOP});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_VALUES_CALLED({
		{_module.getFunction("__pseudo_get_significand"), {17.0}},
		{_module.getFunction("__pseudo_get_exponent"), {17.0}},
	});
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, StosStoreAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("stosd"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LodsLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lodsd"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, MovsStoreAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("movs dword ptr es:[edi], dword ptr [esi]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, RepStosAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("rep stosd"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* i2p = dyn_cast<IntToPtrInst>(&*it))
		{
			if (i2p->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, RepMovsAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("rep movs dword ptr es:[edi], dword ptr [esi]"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* i2p = dyn_cast<IntToPtrInst>(&*it))
		{
			if (i2p->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, ScasLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("scasd"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CmpsLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("cmps dword ptr [esi], dword ptr es:[edi]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, StosbStoreAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("stosb"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, StoswStoreAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("stosw"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LodsbLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lodsb"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, LodswLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("lodsw"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, MovsbStoreAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("movs byte ptr es:[edi], byte ptr [esi]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, ScasbLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("scasb"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CmpsbLoadAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("cmps byte ptr [esi], byte ptr es:[edi]"));
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

TEST_P(Capstone2LlvmIrTranslatorX86Tests, RepStosbAttachesPointeeMetadata)
{
	ONLY_MODE_32;
	auto* f = translate(assemble("rep stosb"));
	ASSERT_NE(nullptr, f);
	bool found = false;
	for (auto it = inst_begin(f), e = inst_end(f); it != e; ++it)
	{
		if (auto* i2p = dyn_cast<IntToPtrInst>(&*it))
		{
			if (i2p->getMetadata("retdec.pointee"))
			{
				found = true;
				break;
			}
		}
	}
	EXPECT_TRUE(found);
}

//
// ==========================================================================
// SSE, and the double-precision half of it in particular
// ==========================================================================
//
// src/capstone2llvmir/x86/x86_sse.cpp is 668 lines and, before these, nothing
// in this suite executed one of them: no test in the file so much as named an
// XMM register. Two of its translators -- PSHUFD and the PSLLDQ/PSRLDQ pair --
// were not reachable at all, having been written, declared and compiled but
// never given a dispatch entry.
//
// The double-precision instructions are the ones that matter most. x86-64
// passes and returns a double in an XMM register and compiles `a + b` to
// ADDSD, and every one of ADDSD, SUBSD, MULSD, DIVSD, UCOMISD, SQRTSD, MAXSD,
// MINSD, CVTTSD2SI and the SSE form of MOVSD was `nullptr`.
//

namespace {

uint64_t dbits(double d)
{
	uint64_t u;
	std::memcpy(&u, &d, sizeof(u));
	return u;
}

uint32_t fbits(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, sizeof(u));
	return u;
}

} // anonymous namespace

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADDSD_touches_only_the_low_lane)
{
	SKIP_MODE_16;

	// The upper lane is what tells ADDSD from ADDPD. With 8.0 over 1.0 and
	// 4.0 over 2.0, the scalar answer keeps 8.0 up there and the packed one
	// would put 12.0.
	setXmm(X86_REG_XMM0, dbits(8.0), dbits(1.0));
	setXmm(X86_REG_XMM1, dbits(4.0), dbits(2.0));

	emulate("addsd xmm0, xmm1");

	EXPECT_EQ(dbits(3.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(8.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADDPD_adds_both_lanes)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, dbits(8.0), dbits(1.0));
	setXmm(X86_REG_XMM1, dbits(4.0), dbits(2.0));

	emulate("addpd xmm0, xmm1");

	EXPECT_EQ(dbits(3.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(12.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SUBSD_subtracts_the_source)
{
	SKIP_MODE_16;

	// Not commutative, so the operand order is checkable: 10 - 3, not 3 - 10.
	setXmm(X86_REG_XMM0, 0, dbits(10.0));
	setXmm(X86_REG_XMM1, 0, dbits(3.0));

	emulate("subsd xmm0, xmm1");

	EXPECT_EQ(dbits(7.0), xmmLow(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MULSD_multiplies_the_low_lane)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, dbits(5.0), dbits(1.5));
	setXmm(X86_REG_XMM1, dbits(7.0), dbits(4.0));

	emulate("mulsd xmm0, xmm1");

	EXPECT_EQ(dbits(6.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(5.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_DIVSD_divides_by_the_source)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, dbits(9.0));
	setXmm(X86_REG_XMM1, 0, dbits(2.0));

	emulate("divsd xmm0, xmm1");

	EXPECT_EQ(dbits(4.5), xmmLow(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SUBSS_subtracts_the_low_float)
{
	SKIP_MODE_16;

	// SUBSS and SUBPS were `nullptr` while ADDPS, MULPS and DIVPS were not,
	// which is the shape of an unfinished list rather than a decision.
	setXmm(X86_REG_XMM0, 0, fbits(10.0f));
	setXmm(X86_REG_XMM1, 0, fbits(3.0f));

	emulate("subss xmm0, xmm1");

	EXPECT_EQ(fbits(7.0f), static_cast<uint32_t>(xmmLow(X86_REG_XMM0)));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ADDSS_touches_only_the_low_float)
{
	SKIP_MODE_16;

	// ADDSS was wired before this branch, to a function that also handled
	// ADDPS; nothing had ever run it.
	setXmm(X86_REG_XMM0, 0, (uint64_t(fbits(9.0f)) << 32) | fbits(1.0f));
	setXmm(X86_REG_XMM1, 0, (uint64_t(fbits(5.0f)) << 32) | fbits(2.0f));

	emulate("addss xmm0, xmm1");

	EXPECT_EQ(fbits(3.0f), static_cast<uint32_t>(xmmLow(X86_REG_XMM0)));
	EXPECT_EQ(fbits(9.0f), static_cast<uint32_t>(xmmLow(X86_REG_XMM0) >> 32));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SQRTSD_roots_the_source_and_keeps_the_destination)
{
	SKIP_MODE_16;

	// The shape no arithmetic instruction has: the operand read is the
	// SOURCE's low lane, the lanes kept are the DESTINATION's. Rooting the
	// destination in place would answer 3.0 here, not 5.0.
	setXmm(X86_REG_XMM0, dbits(7.0), dbits(9.0));
	setXmm(X86_REG_XMM1, dbits(1.0), dbits(25.0));

	emulate("sqrtsd xmm0, xmm1");

	EXPECT_EQ(dbits(5.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(7.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MAXSD_returns_the_source_when_the_source_is_NaN)
{
	SKIP_MODE_16;

	// x86 defines MAXSD as `dst > src ? dst : src`, so a NaN anywhere in the
	// comparison makes the comparison false and yields the SOURCE.
	//
	// The NaN goes in the SOURCE, not the destination, and that is the whole
	// point. With it in the destination both readings agree on 2.0, so the
	// test would pass against llvm.maxnum and prove nothing. Here x86 answers
	// NaN and llvm.maxnum answers 2.0, which is why this is a select and not
	// an intrinsic.
	setXmm(X86_REG_XMM0, 0, dbits(2.0));
	setXmm(X86_REG_XMM1, 0, 0x7ff8000000000000ULL); // quiet NaN

	emulate("maxsd xmm0, xmm1");

	EXPECT_EQ(0x7ff8000000000000ULL, xmmLow(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MAXSD_returns_the_larger)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, dbits(6.0));
	setXmm(X86_REG_XMM1, 0, dbits(2.0));

	emulate("maxsd xmm0, xmm1");

	EXPECT_EQ(dbits(6.0), xmmLow(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MINSD_returns_the_smaller)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, dbits(6.0));
	setXmm(X86_REG_XMM1, 0, dbits(2.0));

	emulate("minsd xmm0, xmm1");

	EXPECT_EQ(dbits(2.0), xmmLow(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_UCOMISD_less_sets_CF_alone)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, dbits(1.0));
	setXmm(X86_REG_XMM1, 0, dbits(2.0));

	emulate("ucomisd xmm0, xmm1");

	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_PF));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_UCOMISD_greater_clears_everything)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, dbits(2.0));
	setXmm(X86_REG_XMM1, 0, dbits(1.0));

	emulate("ucomisd xmm0, xmm1");

	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_PF));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_UCOMISD_equal_sets_ZF_alone)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, dbits(2.0));
	setXmm(X86_REG_XMM1, 0, dbits(2.0));

	emulate("ucomisd xmm0, xmm1");

	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_PF));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_UCOMISD_unordered_sets_all_three)
{
	SKIP_MODE_16;

	// The row that separates the three unordered-inclusive predicates from
	// the ordered ones: a NaN sets ZF, PF and CF together.
	setXmm(X86_REG_XMM0, 0, 0x7ff8000000000000ULL);
	setXmm(X86_REG_XMM1, 0, dbits(2.0));

	emulate("ucomisd xmm0, xmm1");

	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_PF));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_XORPD_is_how_a_compiler_negates)
{
	SKIP_MODE_16;

	// `xorpd xmm0, [sign mask]` is what gcc emits for unary minus on a
	// double, and it has to reach the whole 128 bits: the sign bit of the
	// upper lane is bit 127.
	setXmm(X86_REG_XMM0, dbits(4.0), dbits(1.0));
	setXmm(X86_REG_XMM1, 0x8000000000000000ULL, 0x8000000000000000ULL);

	emulate("xorpd xmm0, xmm1");

	EXPECT_EQ(dbits(-1.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(-4.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ANDNPS_complements_the_destination)
{
	SKIP_MODE_16;

	// ANDN is `(~dst) & src` -- the destination is the complemented side,
	// which is the reverse of what the operand order suggests.
	// Complementing the source instead would answer 0 here, not 0xf0f0f0f0.
	setXmm(X86_REG_XMM0, 0, 0x000000000f0f0f0fULL);
	setXmm(X86_REG_XMM1, 0, 0x00000000ffffffffULL);

	emulate("andnps xmm0, xmm1");

	EXPECT_EQ(0x00000000f0f0f0f0ULL, xmmLow(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSD_between_registers_keeps_the_upper_lane)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, dbits(8.0), dbits(1.0));
	setXmm(X86_REG_XMM1, dbits(4.0), dbits(2.0));

	emulate("movsd xmm0, xmm1");

	EXPECT_EQ(dbits(2.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(8.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSD_from_memory_zeroes_the_upper_lane)
{
	ONLY_MODE_64;

	// The register form preserves the upper lane and the memory form clears
	// it. Translating both as a whole-register move gets one of them wrong
	// whichever way it is written.
	setXmm(X86_REG_XMM0, dbits(8.0), dbits(1.0));
	setRegisters({
		{X86_REG_RAX, 0x1000},
	});
	setMemoryValueUnsigned(0x1000, dbits(3.5), 64);

	emulate("movsd xmm0, qword ptr [rax]");

	EXPECT_EQ(dbits(3.5), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSD_to_memory_writes_the_low_lane)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, dbits(8.0), dbits(3.5));
	setRegisters({
		{X86_REG_RAX, 0x1000},
	});

	emulate("movsd qword ptr [rax], xmm0");

	EXPECT_EQ(dbits(3.5), getMemoryValueUnsigned(0x1000, 64));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSD_with_two_memory_operands_is_still_the_string_move)
{
	// Keystone 0.9.2 refuses a bare `rep movsd` in every mode ("invalid
	// mnemonic") and wants the operands spelled out, which pins the register
	// names and so the mode. Which operands are XMM registers is not a
	// property of the mode, so one mode asks the whole question.
	ONLY_MODE_32;

	// Capstone gives the SSE instruction and the string instruction the same
	// id, so the entry has to serve both. Sending every MOVSD down the SSE
	// path would quietly turn `rep movsd` into a four-byte store -- the same
	// bug this branch fixes, pointing the other way. `rep movsd` becomes a
	// named memcpy call, and nothing on the SSE path emits one.
	auto* f = translate(assemble("rep movsd dword ptr [edi], dword ptr [esi]"));
	ASSERT_NE(nullptr, f);
	EXPECT_NE(std::string::npos, dumpFunction(f).find("__asm_rep_movsd_memcpy"));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVSS_between_registers_keeps_bits_127_32)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbb11111111ULL);
	setXmm(X86_REG_XMM1, 0, 0x0000000022222222ULL);

	emulate("movss xmm0, xmm1");

	EXPECT_EQ(0xbbbbbbbb22222222ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, xmmHigh(X86_REG_XMM0));
}

//
// Batch AB -- the AVX forms of the comparisons and conversions, and what a
// conversion answers when the input does not fit.
//
// Every expected value below was executed on the host CPU; see
// scripts/ci/x86_sse_oracle.c.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VUCOMISD_is_translated_at_all)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0, dbits(1.0));
	setXmm(X86_REG_XMM1, 0, dbits(2.0));

	emulate("vucomisd xmm0, xmm1");

	// `vucomisd` is what a compiler emits for a double comparison on any
	// machine built this decade. It was dispatched to nullptr, so an AVX
	// binary got a pseudo-assembly call here and an SSE binary got a
	// comparison.
	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VUCOMISD_is_a_DOUBLE_comparison)
{
	ONLY_MODE_64;

	// 1.0 and 2.0 as doubles have a low half of zero, so read as two FLOATS
	// both operands are +0.0 and the comparison answers "equal". Wiring the
	// VEX id to translateSseComi without telling the body that it is a
	// double form gives exactly that -- 913 wrong answers out of 9,610 when
	// it was measured, and nothing crashes, because the instruction IS
	// translated, just as the wrong operation.
	setXmm(X86_REG_XMM0, 0, dbits(1.0));
	setXmm(X86_REG_XMM1, 0, dbits(2.0));

	emulate("vucomisd xmm0, xmm1");

	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_CF)); // 1.0 < 2.0
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_ZF)); // not equal
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VUCOMISD_unordered_sets_all_three)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0, 0x7ff8000000000000ULL); // quiet NaN
	setXmm(X86_REG_XMM1, 0, dbits(1.0));

	emulate("vucomisd xmm0, xmm1");

	// The one row of the table nothing but a NaN reaches.
	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_AF));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VCOMISS_is_a_float_comparison)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0, fbits(1.0f));
	setXmm(X86_REG_XMM1, 0, fbits(2.0f));

	emulate("vcomiss xmm0, xmm1");

	EXPECT_EQ(1, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(0, getRegisterValueUnsigned(X86_REG_ZF));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVMSKPD_is_two_lanes_not_four)
{
	ONLY_MODE_64;

	// Sign bit set on the UPPER double only. Read as four floats the answer
	// would be 0x8, not 0x2.
	setXmm(X86_REG_XMM1, 0x8000000000000000ULL, 0x0000000000000000ULL);

	emulate("vmovmskpd eax, xmm1");

	EXPECT_EQ(0x2, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVMSKPS_is_four_lanes)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x8000000000000000ULL, 0x8000000000000000ULL);

	emulate("vmovmskps eax, xmm1");

	EXPECT_EQ(0xa, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VCVTTSD2SI_is_a_double_conversion)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0, dbits(2.7));

	emulate("vcvttsd2si eax, xmm1");

	EXPECT_EQ(2, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VCVTSD2SI_rounds_to_nearest_even)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0, dbits(2.5));

	emulate("vcvtsd2si eax, xmm1");

	// To NEAREST EVEN, so 2, not 3.
	EXPECT_EQ(2, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CVTTSD2SI_infinity_is_the_integer_indefinite)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0, 0x7ff0000000000000ULL); // +inf

	emulate("cvttsd2si eax, xmm1");

	// x86 answers with the destination's minimum signed value when the input
	// does not fit. LLVM's fptosi calls that poison, which is not a value the
	// decompiler can print and does not stay where it is put.
	EXPECT_EQ(0x80000000ULL, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CVTTSD2SI_nan_is_the_integer_indefinite)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0, 0x7ff8000000000000ULL);

	emulate("cvttsd2si eax, xmm1");

	EXPECT_EQ(0x80000000ULL, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CVTTSD2SI_out_of_range_is_the_integer_indefinite)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0, dbits(1.0e24));

	emulate("cvttsd2si eax, xmm1");

	EXPECT_EQ(0x80000000ULL, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CVTSD2SI_negative_infinity_is_the_integer_indefinite)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0, 0xfff0000000000000ULL);

	emulate("cvtsd2si eax, xmm1");

	EXPECT_EQ(0x80000000ULL, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CVTTSS2SI_infinity_is_the_integer_indefinite)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0, 0x7f800000ULL); // +inf as a float

	emulate("cvttss2si eax, xmm1");

	EXPECT_EQ(0x80000000ULL, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CVTTSD2SI_in_range_is_unaffected_by_the_guard)
{
	ONLY_MODE_64;

	// The range check must not disturb the ordinary answer, which is the
	// whole reason the bound is 2^31 with a strict comparison rather than
	// 2^31-1: the latter is not representable in binary floating point.
	setXmm(X86_REG_XMM1, 0, dbits(2147483647.0));

	emulate("cvttsd2si eax, xmm1");

	EXPECT_EQ(0x7fffffffULL, getRegisterValueUnsigned(X86_REG_RAX));
}

//
// Batch AC -- the packed shifts and the widening moves.
//
// All twenty of these fell through to pseudo-assembly. Every expected value
// below was executed on the host CPU; see scripts/ci/x86_vec_oracle.c.
//

//
// Batch AD -- saturating arithmetic, packing, the packed multiplies and the
// two reductions. All twenty fell through to pseudo-assembly. Every expected
// value was executed on the host CPU; see scripts/ci/x86_vec_oracle.c.
//

//
// Batch AE -- PSHUFB, the horizontal adds and subtracts, PABS, PSIGN and the
// three odd ones out. All sixteen fell through to pseudo-assembly. Every
// expected value was executed on the host CPU; see
// scripts/ci/x86_vec_oracle.c.
//

//
// Batch AF -- the immediate-controlled shuffles and blends. Expected values
// executed on the host CPU; see scripts/ci/x86_vec_oracle.c.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSHUFHW_permutes_only_the_high_half)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x0005000600070008ULL, 0x0001000200030004ULL);

	emulate("pshufhw xmm0, xmm1, 0x1b");

	// 0x1b reverses the four words it touches. The LOW quadword is copied
	// through untouched -- treating this as a whole-register permutation
	// would scramble the half that is supposed to be left alone.
	EXPECT_EQ(0x0001000200030004ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0008000700060005ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSHUFLW_permutes_only_the_low_half)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x0005000600070008ULL, 0x0001000200030004ULL);

	emulate("pshuflw xmm0, xmm1, 0x1b");

	EXPECT_EQ(0x0004000300020001ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0005000600070008ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PBLENDW_set_bit_takes_the_second_operand)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x2222222222222222ULL, 0x1111111111111111ULL);
	setXmm(X86_REG_XMM1, 0xbbbbbbbbbbbbbbbbULL, 0xaaaaaaaaaaaaaaaaULL);

	emulate("pblendw xmm0, xmm1, 0xa5");

	// 0xa5 is 10100101: words 0, 2, 5 and 7 come from the second operand.
	EXPECT_EQ(0x1111aaaa1111aaaaULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xbbbb2222bbbb2222ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, BLENDPS_selects_four_dwords)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x3333333344444444ULL, 0x1111111122222222ULL);
	setXmm(X86_REG_XMM1, 0xccccccccddddddddULL, 0xaaaaaaaabbbbbbbbULL);

	emulate("blendps xmm0, xmm1, 0x09");

	EXPECT_EQ(0x11111111bbbbbbbbULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xcccccccc44444444ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, BLENDPD_selects_two_quadwords)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x2222222222222222ULL, 0x1111111111111111ULL);
	setXmm(X86_REG_XMM1, 0xbbbbbbbbbbbbbbbbULL, 0xaaaaaaaaaaaaaaaaULL);

	emulate("blendpd xmm0, xmm1, 0x02");

	// Bit 1 set: the high quadword comes from the second operand, the low
	// one stays. BLENDPS and BLENDPD are floating point only in name --
	// nothing is interpreted, so they are one operation at two widths.
	EXPECT_EQ(0x1111111111111111ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xbbbbbbbbbbbbbbbbULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSHUFB_top_bit_of_the_control_writes_zero)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0f0e0d0c0b0a0908ULL, 0x0706050403020100ULL);
	setXmm(X86_REG_XMM1, 0x0405068384858687ULL, 0x8000810102820303ULL);

	emulate("pshufb xmm0, xmm1");

	// A control byte with its top bit set writes ZERO instead of selecting a
	// lane. Masking with 0x0f and forgetting the top bit answers with a
	// source byte everywhere the hardware answers zero.
	EXPECT_EQ(0x0000000102000303ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0405060000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSIGNB_has_three_outcomes_per_lane)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0xf0f1f2f3f4f5f6f7ULL, 0x0102030405060708ULL);
	setXmm(X86_REG_XMM1, 0x0100ff0001ff0001ULL, 0x01ff0001ff000100ULL);

	emulate("psignb xmm0, xmm1");

	// Negative negates, positive keeps, and a control of exactly ZERO writes
	// zero. The zero case is the one a `negative ? -a : a` drops.
	EXPECT_EQ(0x01fe0004fb000700ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xf0000e00f40b00f7ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PABSB_of_the_minimum_signed_value_is_itself)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x8081828384858687ULL, 0x807f01ff02fe0380ULL);

	emulate("pabsb xmm0, xmm1");

	// -128 has no positive counterpart and x86 answers with it unchanged
	// rather than saturating to 127. Negating and letting it wrap is
	// exactly that.
	EXPECT_EQ(0x807f010102020380ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x807f7e7d7c7b7a79ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PABSD_of_the_minimum_signed_value_is_itself)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x000000017fffffffULL, 0x80000000ffffffffULL);

	emulate("pabsd xmm0, xmm1");

	EXPECT_EQ(0x8000000000000001ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x000000017fffffffULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PHADDW_adds_adjacent_pairs_within_each_operand)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0005000600070008ULL, 0x0001000200030004ULL);
	setXmm(X86_REG_XMM1, 0x000e000f00100011ULL, 0x000a000b000c000dULL);

	emulate("phaddw xmm0, xmm1");

	// Not lane against lane across the two registers: pairs WITHIN each, the
	// first operand's four sums in the low half and the second's in the high.
	EXPECT_EQ(0x000b000f00030007ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x001d002100150019ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PHSUBW_subtracts_the_second_of_each_pair)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x000a0003000a0004ULL, 0x000a0001000a0002ULL);
	setXmm(X86_REG_XMM1, 0x0014000700140008ULL, 0x0014000500140006ULL);

	emulate("phsubw xmm0, xmm1");

	// a[0]-a[1], not a[1]-a[0].
	EXPECT_EQ(0xfff9fffafff7fff8ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xfff3fff4fff1fff2ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PHADDSW_saturates_where_PHADDW_wraps)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0001000200030004ULL, 0x7fff7fff80008000ULL);
	setXmm(X86_REG_XMM1, 0x0005000600070008ULL, 0x7fff7fff80008000ULL);

	emulate("phaddsw xmm0, xmm1");

	EXPECT_EQ(0x000300077fff8000ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x000b000f7fff8000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMULHRSW_rounds_rather_than_truncates)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x8000000100020003ULL, 0x4000400040004000ULL);
	setXmm(X86_REG_XMM1, 0x8000000100020003ULL, 0x4000200010000800ULL);

	emulate("pmulhrsw xmm0, xmm1");

	// (a*b >> 14) + 1 >> 1. The +1 is what rounds; dropping it is off by one
	// on every product whose bit 14 is set.
	EXPECT_EQ(0x2000100008000400ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x8000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMADDUBSW_first_operand_unsigned_second_signed)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x8080808080808080ULL, 0xff80017f02fe0301ULL);
	setXmm(X86_REG_XMM1, 0x8080808080808080ULL, 0x01ff7f800102feffULL);

	emulate("pmaddubsw xmm0, xmm1");

	// Widening both the same way is wrong whichever way is picked. The pair
	// sum then saturates, which PMADDWD does not do.
	EXPECT_EQ(0x007fc0ff01fefff9ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x8000800080008000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PHMINPOSUW_finds_the_minimum_and_its_index)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x000a0001000b000cULL, 0x0005000300070009ULL);

	emulate("phminposuw xmm0, xmm1");

	// The value in bits 15:0 and the index in bits 18:16; here the minimum is
	// 1 at word 6.
	EXPECT_EQ(0x0000000000060001ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PHMINPOSUW_ties_take_the_lowest_index)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0x0002000200020002ULL, 0x0002000200020002ULL);

	emulate("phminposuw xmm0, xmm1");

	// Every word equal, so the index is 0. Scanning upward with a strict
	// comparison gives that; `<=` would answer with the last of the tie.
	EXPECT_EQ(0x0000000000000002ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PADDSB_saturates_in_both_directions)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x7f008001807f7f01ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0100ff01ff807f01ULL);

	emulate("paddsb xmm0, xmm1");

	// 127 + 1 is 127 and -128 + -128 is -128. A plain vector add gives
	// -128 and 0, which look like answers.
	EXPECT_EQ(0x7f00800280ff7f02ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PADDUSB_saturates_at_255)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0xff01ff80017f00feULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x01ff01800180ff02ULL);

	emulate("paddusb xmm0, xmm1");

	EXPECT_EQ(0xffffffff02ffffffULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSUBUSB_clamps_at_zero)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0001027f80fffe03ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0102017f81000105ULL);

	emulate("psubusb xmm0, xmm1");

	// The unsigned difference goes NEGATIVE before it is clamped, so the
	// lower bound has to be tested with a signed comparison. An unsigned
	// one reads the intermediate as enormous and clamps upward instead.
	EXPECT_EQ(0x0000010000fffd00ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSUBSW_saturates_both_ends)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x80007fff00010000ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x00010001ffff8000ULL);

	emulate("psubsw xmm0, xmm1");

	EXPECT_EQ(0x80007ffe00027fffULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PACKSSWB_low_half_is_the_first_operand)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0100ffff00010002ULL, 0x007f0080ff80007fULL);
	setXmm(X86_REG_XMM1, 0x0005000600070008ULL, 0x80007fff00030004ULL);

	emulate("packsswb xmm0, xmm1");

	// The destination's low half comes from the FIRST operand and its
	// high half from the second.
	EXPECT_EQ(0x7fff01027f7f807fULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x05060708807f0304ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PACKUSWB_reads_a_signed_source)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x00ff000100020003ULL, 0xffff0100007f0080ULL);
	setXmm(X86_REG_XMM1, 0x0006000700080009ULL, 0x8000010000040005ULL);

	emulate("packuswb xmm0, xmm1");

	// -1 becomes 0, not 255. The source is signed even though the
	// destination is unsigned; reading it as unsigned gets 255 by
	// arriving at the same bit pattern backwards.
	EXPECT_EQ(0xff01020300ff7f80ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0607080900ff0405ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMULLW_keeps_the_low_half)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0xffff000400050006ULL, 0x80007fff00020003ULL);
	setXmm(X86_REG_XMM1, 0x0002000200020002ULL, 0x0002000200030004ULL);

	emulate("pmullw xmm0, xmm1");

	EXPECT_EQ(0x0000fffe0006000cULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xfffe0008000a000cULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMULHW_keeps_the_signed_high_half)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0003000400050006ULL, 0x80007fffffff0002ULL);
	setXmm(X86_REG_XMM1, 0x0003000400050006ULL, 0x80007fffffff0002ULL);

	emulate("pmulhw xmm0, xmm1");

	// -1 squared is 1, whose high word is 0.
	EXPECT_EQ(0x40003fff00000000ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMULHUW_keeps_the_unsigned_high_half)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0003000400050006ULL, 0x80007fffffff0002ULL);
	setXmm(X86_REG_XMM1, 0x0003000400050006ULL, 0x80007fffffff0002ULL);

	emulate("pmulhuw xmm0, xmm1");

	// The same operands as PMULHW. 0xffff squared unsigned is
	// 0xfffe0001, whose high word is 0xfffe -- the one difference
	// between these two instructions.
	EXPECT_EQ(0x40003ffffffe0000ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000000000000000ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMULUDQ_reads_only_the_even_dwords)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0xcafebabe00000003ULL, 0xdeadbeefffffffffULL);
	setXmm(X86_REG_XMM1, 0x0badf00d00000005ULL, 0xfeedface00000002ULL);

	emulate("pmuludq xmm0, xmm1");

	// Lanes 1 and 3 are not multiplied at all.
	EXPECT_EQ(0x00000001fffffffeULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x000000000000000fULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMULDQ_sign_extends_the_even_dwords)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0xcafebabe00000003ULL, 0xdeadbeeffffffffeULL);
	setXmm(X86_REG_XMM1, 0x0badf00dfffffffbULL, 0xfeedface00000003ULL);

	emulate("pmuldq xmm0, xmm1");

	EXPECT_EQ(0xfffffffffffffffaULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xfffffffffffffff1ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMADDWD_adds_adjacent_products)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x8000800000050006ULL, 0x0001000200030004ULL);
	setXmm(X86_REG_XMM1, 0x800080000009000aULL, 0x0005000600070008ULL);

	emulate("pmaddwd xmm0, xmm1");

	// The high lane is -32768 squared twice, which wraps to 0x80000000.
	// That is the defined answer, so the addition is left to wrap.
	EXPECT_EQ(0x0000001100000035ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x8000000000000069ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSADBW_sums_each_group_of_eight)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0xff00ff00ff00ff00ULL, 0x0102030405060708ULL);
	setXmm(X86_REG_XMM1, 0x00ff00ff00ff00ffULL, 0x0807060504030201ULL);

	emulate("psadbw xmm0, xmm1");

	// Eight absolute differences per group, each group's total in the
	// low word of its own quadword. The high group is eight times 255.
	EXPECT_EQ(0x0000000000000020ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x00000000000007f8ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSLLW_shifts_every_word)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0005000600070008ULL, 0x0001000200030004ULL);
	setXmm(X86_REG_XMM1, 0ULL, 4ULL);

	emulate("psllw xmm0, xmm1");

	EXPECT_EQ(0x0010002000300040ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0050006000700080ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSLLW_by_the_element_width_is_zero)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0005000600070008ULL, 0x0001000200030004ULL);
	setXmm(X86_REG_XMM1, 0ULL, 16ULL);

	emulate("psllw xmm0, xmm1");

	// Not undefined and not poison: a count at or past the element width
	// gives zero. `shl <8 x i16> %v, 16` is poison, so the count has to be
	// clamped and the answer selected back.
	EXPECT_EQ(0ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSLLW_by_an_enormous_count_is_still_zero)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0005000600070008ULL, 0x0001000200030004ULL);
	setXmm(X86_REG_XMM1, 0ULL, 1ULL << 40);

	emulate("psllw xmm0, xmm1");

	// The count is a full 64-bit value, so "too big" has to be asked of all
	// of it -- truncating it to the element width first would read this as a
	// shift by zero.
	EXPECT_EQ(0ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSLLW_ignores_the_high_half_of_the_count)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0005000600070008ULL, 0x0001000200030004ULL);
	setXmm(X86_REG_XMM1, 0xdeadbeefdeadbeefULL, 4ULL);

	emulate("psllw xmm0, xmm1");

	// The count is the LOW quadword. Reading the whole register would make
	// this an enormous count and answer zero.
	EXPECT_EQ(0x0010002000300040ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0050006000700080ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSRAW_past_the_width_broadcasts_the_sign)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0xffff000180007fffULL, 0x8000000700018000ULL);
	setXmm(X86_REG_XMM1, 0ULL, 20ULL);

	emulate("psraw xmm0, xmm1");

	// The arithmetic right shift is the one case where "too big" is NOT
	// zero: every element becomes its own sign bit.
	EXPECT_EQ(0xffff00000000ffffULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xffff0000ffff0000ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSRAD_by_the_element_width_broadcasts_the_sign)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x7fffffff00000001ULL, 0x80000001ffffffffULL);
	setXmm(X86_REG_XMM1, 0ULL, 32ULL);

	emulate("psrad xmm0, xmm1");

	EXPECT_EQ(0xffffffffffffffffULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSRLQ_shifts_both_quadwords)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0xffffffffffffffffULL, 0x8000000000000000ULL);
	setXmm(X86_REG_XMM1, 0ULL, 63ULL);

	emulate("psrlq xmm0, xmm1");

	EXPECT_EQ(1ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(1ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSLLW_immediate_form)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x0005000600070008ULL, 0x0001000200030004ULL);

	emulate("psllw xmm0, 3");

	// Capstone gives the immediate form the same instruction id as the
	// register one and distinguishes them only by the operand's type.
	EXPECT_EQ(0x0008001000180020ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0028003000380040ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PSRLD_immediate_past_the_width)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x00000001ffffffffULL, 0xffffffff80000000ULL);

	emulate("psrld xmm0, 33");

	EXPECT_EQ(0ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMOVSXBW_widens_the_low_eight_bytes)
{
	ONLY_MODE_64;

	// The upper half of the source is noise: pmovsxbw reads eight bytes and
	// a translator that reads more would still answer plausibly for the
	// lanes it does write.
	setXmm(X86_REG_XMM1, 0xdeadbeefdeadbeefULL, 0x01ff7f80fe020103ULL);

	emulate("pmovsxbw xmm0, xmm1");

	EXPECT_EQ(0xfffe000200010003ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0001ffff007fff80ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMOVZXBW_zero_extends_where_PMOVSXBW_signs)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0xdeadbeefdeadbeefULL, 0x01ff7f80fe020103ULL);

	emulate("pmovzxbw xmm0, xmm1");

	EXPECT_EQ(0x00fe000200010003ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x000100ff007f0080ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMOVSXBQ_reads_only_two_bytes)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0xdeadbeefdeadbeefULL, 0x0102030405067f80ULL);

	emulate("pmovsxbq xmm0, xmm1");

	// Two quadwords out means two bytes in. Everything above the low sixteen
	// bits of the source is untouched.
	EXPECT_EQ(0xffffffffffffff80ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x000000000000007fULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMOVZXBQ_reads_only_two_bytes)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0xdeadbeefdeadbeefULL, 0x0102030405067f80ULL);

	emulate("pmovzxbq xmm0, xmm1");

	EXPECT_EQ(0x0000000000000080ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x000000000000007fULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMOVSXDQ_widens_two_dwords)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0xdeadbeefdeadbeefULL, 0x80000001fffffffeULL);

	emulate("pmovsxdq xmm0, xmm1");

	EXPECT_EQ(0xfffffffffffffffeULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xffffffff80000001ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PMOVZXWD_widens_four_words)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM1, 0xdeadbeefdeadbeefULL, 0x8000000100027fffULL);

	emulate("pmovzxwd xmm0, xmm1");

	EXPECT_EQ(0x0000000200007fffULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x0000800000000001ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CVTTPS2DQ_indefinite_is_per_lane)
{
	ONLY_MODE_64;

	// 1.5, +inf, -2.5, 1.0e24 -- two lanes in range and two not. The range
	// guard works elementwise, so the packed forms get it too; without a test
	// here it would be a fix applied to five instructions and measured on
	// four, which is the mistake Batch AA nearly made with the shifts.
	setXmm(
		X86_REG_XMM1,
		(uint64_t)0xc0200000ULL | ((uint64_t)0x6a000000ULL << 32),
		(uint64_t)0x3fc00000ULL | ((uint64_t)0x7f800000ULL << 32));

	emulate("cvttps2dq xmm0, xmm1");

	EXPECT_EQ(0x8000000000000001ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x80000000fffffffeULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, CVTPS2DQ_rounds_and_keeps_the_indefinite)
{
	ONLY_MODE_64;

	setXmm(
		X86_REG_XMM1,
		(uint64_t)0xc0200000ULL | ((uint64_t)0x6a000000ULL << 32),
		(uint64_t)0x3fc00000ULL | ((uint64_t)0x7f800000ULL << 32));

	emulate("cvtps2dq xmm0, xmm1");

	// 1.5 rounds to 2 and -2.5 to -2, both to nearest EVEN; the two lanes
	// that do not fit stay indefinite.
	EXPECT_EQ(0x8000000000000002ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x80000000fffffffeULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CVTTSD2SI_truncates_toward_zero)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0, dbits(2.7));

	emulate("cvttsd2si rax, xmm0");

	EXPECT_EQ(2, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CVTSD2SI_rounds_to_nearest)
{
	ONLY_MODE_64;

	// The one without the T rounds. The comment on translateCvtSd2Si used to
	// say "round toward nearest (C default); use FPToSI (truncation)", and
	// FPToSI is what it did, so this answered 2.
	setXmm(X86_REG_XMM0, 0, dbits(2.7));

	emulate("cvtsd2si rax, xmm0");

	EXPECT_EQ(3, getRegisterValueUnsigned(X86_REG_RAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CVTSS2SD_widens_the_low_lane)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, dbits(8.0), 0);
	setXmm(X86_REG_XMM1, 0, fbits(2.5f));

	emulate("cvtss2sd xmm0, xmm1");

	EXPECT_EQ(dbits(2.5), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(8.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_CVTSD2SS_narrows_the_low_lane)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, 0x1111111122222222ULL);
	setXmm(X86_REG_XMM1, 0, dbits(2.5));

	emulate("cvtsd2ss xmm0, xmm1");

	EXPECT_EQ(fbits(2.5f), static_cast<uint32_t>(xmmLow(X86_REG_XMM0)));
	EXPECT_EQ(0x11111111u, static_cast<uint32_t>(xmmLow(X86_REG_XMM0) >> 32));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_UNPCKLPD_interleaves_the_low_lanes)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, dbits(8.0), dbits(1.0));
	setXmm(X86_REG_XMM1, dbits(4.0), dbits(2.0));

	emulate("unpcklpd xmm0, xmm1");

	EXPECT_EQ(dbits(1.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(2.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_UNPCKHPD_interleaves_the_high_lanes)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, dbits(8.0), dbits(1.0));
	setXmm(X86_REG_XMM1, dbits(4.0), dbits(2.0));

	emulate("unpckhpd xmm0, xmm1");

	EXPECT_EQ(dbits(8.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(4.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHUFPD_takes_the_high_lane_from_the_source)
{
	SKIP_MODE_16;

	// imm 0b01: result lane 0 = dst lane 1, result lane 1 = src lane 0.
	setXmm(X86_REG_XMM0, dbits(8.0), dbits(1.0));
	setXmm(X86_REG_XMM1, dbits(4.0), dbits(2.0));

	emulate("shufpd xmm0, xmm1, 1");

	EXPECT_EQ(dbits(8.0), xmmLow(X86_REG_XMM0));
	EXPECT_EQ(dbits(2.0), xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVMSKPD_gathers_the_sign_bits)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, dbits(-4.0), dbits(1.0));

	emulate("movmskpd eax, xmm0");

	EXPECT_EQ(2, getRegisterValueUnsigned(X86_REG_EAX));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PSHUFD_reaches_its_translator_at_all)
{
	SKIP_MODE_16;

	// translateSsePshufd has been in x86_sse.cpp since the file was written
	// and X86_INS_PSHUFD pointed at nullptr, so none of it ran. 0x1b is
	// 0b00_01_10_11: reverse the four lanes.
	setXmm(X86_REG_XMM1, 0x4444444433333333ULL, 0x2222222211111111ULL);

	emulate("pshufd xmm0, xmm1, 0x1b");

	EXPECT_EQ(0x3333333344444444ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x1111111122222222ULL, xmmHigh(X86_REG_XMM0));
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PSLLDQ_shifts_by_bytes)
{
	SKIP_MODE_16;

	// The other translator nothing could reach. The shift count is in BYTES,
	// which is what the DQ suffix means and the only thing that separates
	// this from PSLLQ.
	setXmm(X86_REG_XMM0, 0, 0x00000000000000ffULL);

	emulate("pslldq xmm0, 1");

	EXPECT_EQ(0x000000000000ff00ULL, xmmLow(X86_REG_XMM0));
}


//
// X86_INS_TZCNT, X86_INS_LZCNT, X86_INS_POPCNT
//
// All three pointed at nullptr. TZCNT alone is 11,662 occurrences in the
// static corpus -- the sixth most frequent untranslated instruction -- because
// it is what a compiler emits for __builtin_ctz on anything from Haswell on.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_TZCNT_counts_trailing_zeros)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 1 << 20 | 1 << 25},
	});

	emulate("tzcnt eax, edx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 20},
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_TZCNT_of_zero_is_the_width_not_the_old_destination)
{
	// The whole test. BSF and TZCNT have the same encoding but for the F3
	// prefix and are NOT the same instruction: for a zero source BSF leaves
	// the destination untouched and sets ZF, TZCNT writes the operand width
	// and sets CF. Translating one as the other is a wrong answer exactly
	// here, and nowhere else -- which is why it would survive a test that
	// only ever passed it a non-zero source.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 1234},
		{X86_REG_EDX, 0},
	});

	emulate("tzcnt eax, edx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 32}, // not 1234
		{X86_REG_CF, true},
		{X86_REG_ZF, false}, // the result is 32, and 32 is not zero
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_TZCNT_of_zero_reg64_is_64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RDX, 0},
	});

	emulate("tzcnt rax, rdx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 64},
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LZCNT_counts_leading_zeros)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 1 << 20 | 1 << 25},
	});

	emulate("lzcnt eax, edx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 6}, // 31 - 25
		{X86_REG_CF, false},
		{X86_REG_ZF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LZCNT_of_zero_is_the_width_not_the_old_destination)
{
	// BSR's half of the same distinction: for a zero source it leaves the
	// destination alone and sets ZF, LZCNT writes the width and sets CF.
	// (That the two disagree for a NON-zero source as well -- BSR answers
	// the index of the top set bit, 25, and LZCNT the count of zeros above
	// it, 6 -- is what the test above pins down.)
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 1234},
		{X86_REG_EDX, 0},
	});

	emulate("lzcnt eax, edx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 32},
		{X86_REG_CF, true},
		{X86_REG_ZF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_LZCNT_sets_ZF_when_the_top_bit_is_set)
{
	// The one source for which the result is zero, and so the only one that
	// tells ZF apart from a constant false.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0x80000000},
	});

	emulate("lzcnt eax, edx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0},
		{X86_REG_CF, false},
		{X86_REG_ZF, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_POPCNT_counts_set_bits_and_clears_five_flags)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0b1011},
	});

	emulate("popcnt eax, edx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 3},
		{X86_REG_ZF, false},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_POPCNT_of_zero_sets_ZF)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EDX, 0},
	});

	emulate("popcnt eax, edx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0},
		{X86_REG_ZF, true},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_POPCNT_reg64_counts_all_64)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RDX, 0xffffffffffffffffULL},
	});

	emulate("popcnt rax, rdx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_RAX, 64},
		{X86_REG_ZF, false},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

//
// X86_INS_PMOVMSKB
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PMOVMSKB_gathers_sixteen_sign_bits)
{
	// The most frequent untranslated instruction in the static corpus,
	// 21,102 occurrences. Lanes 0, 6, 7 and 9 have their top bit set, so the
	// answer is 0b10_1100_0001. The two lanes in the upper half are there
	// because a translator that only looked at the low 64 bits would still
	// pass a test whose set bits all lived down there.
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x000000000000ff00ULL, 0x8080000000000080ULL);

	emulate("pmovmskb eax, xmm0");

	EXPECT_EQ(0x2c1, getRegisterValueUnsigned(X86_REG_EAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PMOVMSKB_of_all_ones_is_all_sixteen_bits)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0xffffffffffffffffULL, 0xffffffffffffffffULL);

	emulate("pmovmskb eax, xmm0");

	EXPECT_EQ(0xffff, getRegisterValueUnsigned(X86_REG_EAX));
	EXPECT_NO_VALUE_CALLED();
}

//
// Prefetch hints
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PREFETCHT0_is_a_hint_with_no_effect)
{
	// Same argument as ARM's PLD, ARM64's PRFM and MIPS's PREF, all of which
	// went to translateNop earlier in this branch: a prefetch names an
	// address, does not read it, writes nothing and cannot fault. As a
	// nullptr entry it came out as an __asm_prefetcht0 call with a memory
	// operand, which reads as a side effect the instruction does not have.
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1000},
	});

	emulate("prefetcht0 byte ptr [rax]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PREFETCHW_is_a_hint_with_no_effect)
{
	// PREFETCHW asks for the line in an exclusive state, which is still only
	// a cache-coherence hint: it does not write.
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1000},
	});

	emulate("prefetchw byte ptr [rax]");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// Non-temporal stores and loads
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVNTDQ_stores_all_128_bits)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
	setRegisters({
		{X86_REG_RAX, 0x1000},
	});

	emulate("movntdq xmmword ptr [rax], xmm0");

	EXPECT_EQ(0x99aabbccddeeff00ULL, memLow128(0x1000));
	EXPECT_EQ(0x1122334455667788ULL, memHigh128(0x1000));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVNTPS_stores_all_128_bits)
{
	ONLY_MODE_64;

	setXmm(X86_REG_XMM0, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
	setRegisters({
		{X86_REG_RAX, 0x1000},
	});

	emulate("movntps xmmword ptr [rax], xmm0");

	EXPECT_EQ(0x99aabbccddeeff00ULL, memLow128(0x1000));
	EXPECT_EQ(0x1122334455667788ULL, memHigh128(0x1000));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVNTDQA_loads_all_128_bits)
{
	// The only one of the family that goes the other way.
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1000},
	});
	setMemoryValue128(0x1000, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);

	emulate("movntdqa xmm0, xmmword ptr [rax]");

	EXPECT_EQ(0x99aabbccddeeff00ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x1122334455667788ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVNTI_stores_the_register)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x1000},
		{X86_REG_EDX, 0x11223344},
	});

	emulate("movnti dword ptr [rax], edx");

	EXPECT_EQ(0x11223344, getMemoryValueUnsigned(0x1000, 32));
	EXPECT_NO_VALUE_CALLED();
}


//
// Packed integer compare, unpack, min/max, align and average.
//
// Everything below was a nullptr entry. Between them they are ~28,500
// occurrences in the static corpus, and every one of them is decided by a
// property the mnemonic spells out and the IR does not: signedness, lane
// width, or which half of the operand is taken. Each test therefore uses
// inputs on which the wrong reading gives a different answer, because inputs
// on which it does not are the reason a wrong reading survives.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PCMPGTB_compares_signed)
{
	// Byte 0 is 0xff against 0x01. Signed that is -1 against 1 and the answer
	// is 0x00; unsigned it is 255 against 1 and the answer is 0xff. Byte 1 is
	// 5 against 2, which is greater either way, so the result is not all
	// zeroes and a stuck-at-zero translator cannot pass.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, 0x00000000000005ffULL);
	setXmm(X86_REG_XMM1, 0, 0x0000000000000201ULL);

	emulate("pcmpgtb xmm0, xmm1");

	EXPECT_EQ(0x000000000000ff00ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PCMPGTD_compares_signed)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, 0x00000005ffffffffULL);
	setXmm(X86_REG_XMM1, 0, 0x0000000200000001ULL);

	emulate("pcmpgtd xmm0, xmm1");

	EXPECT_EQ(0xffffffff00000000ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PCMPEQQ_compares_64_bit_lanes)
{
	// The lane width is the whole test. The low halves differ as 64-bit lanes
	// and agree in their bottom 32 bits, so a 32-bit reading answers
	// 0x00000000ffffffff where the instruction answers zero. That reading was
	// not hypothetical: translateSsePcmpeq used to end its width switch with
	// `default: bits = 32`.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0x1111111111111111ULL, 0x2222222211111111ULL);
	setXmm(X86_REG_XMM1, 0x1111111111111111ULL, 0x3333333311111111ULL);

	emulate("pcmpeqq xmm0, xmm1");

	EXPECT_EQ(0ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xffffffffffffffffULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUNPCKLWD_interleaves_16_bit_lanes)
{
	// The width that the old `(id == PUNPCKLBW) ? 8 : 32` would have got
	// wrong: as 32-bit lanes the answer is 0x5555111166662222, as 16-bit ones
	// it is 0x6666222255551111.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, 0x4444333322221111ULL);
	setXmm(X86_REG_XMM1, 0, 0x8888777766665555ULL);

	emulate("punpcklwd xmm0, xmm1");

	EXPECT_EQ(0x6666222255551111ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x8888444477773333ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUNPCKLQDQ_takes_the_low_half_of_each)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0xaaaaaaaaaaaaaaaaULL, 0x1111111111111111ULL);
	setXmm(X86_REG_XMM1, 0xbbbbbbbbbbbbbbbbULL, 0x2222222222222222ULL);

	emulate("punpcklqdq xmm0, xmm1");

	EXPECT_EQ(0x1111111111111111ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x2222222222222222ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUNPCKHQDQ_takes_the_high_half_of_each)
{
	// Same operands as the L form above, and no overlap in the answer: this
	// is the test that separates the two halves rather than the two widths.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0xaaaaaaaaaaaaaaaaULL, 0x1111111111111111ULL);
	setXmm(X86_REG_XMM1, 0xbbbbbbbbbbbbbbbbULL, 0x2222222222222222ULL);

	emulate("punpckhqdq xmm0, xmm1");

	EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xbbbbbbbbbbbbbbbbULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PUNPCKHBW_interleaves_the_top_eight_bytes)
{
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0x0807060504030201ULL, 0);
	setXmm(X86_REG_XMM1, 0x1817161514131211ULL, 0);

	emulate("punpckhbw xmm0, xmm1");

	EXPECT_EQ(0x1404130312021101ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x1808170716061505ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PMINUB_compares_unsigned)
{
	// 0xff against 0x01 and 0x80 against 0x0f. Unsigned the answers are 0x01
	// and 0x0f; signed they are 0xff and 0x80 -- every bit different.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, 0x00000000000080ffULL);
	setXmm(X86_REG_XMM1, 0, 0x0000000000000f01ULL);

	emulate("pminub xmm0, xmm1");

	EXPECT_EQ(0x0000000000000f01ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PMAXSW_compares_signed)
{
	// The other half of the same question. Signed the answers are 1 and 5;
	// unsigned they are 0xffff and 0x8000.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, 0x000000000005ffffULL);
	setXmm(X86_REG_XMM1, 0, 0x0000000080000001ULL);

	emulate("pmaxsw xmm0, xmm1");

	EXPECT_EQ(0x0000000000050001ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PALIGNR_takes_a_window_across_both)
{
	// imm 4: the result starts four bytes into the source and runs into the
	// bottom four bytes of the destination. Every byte of the answer comes
	// from a different place than it would with imm 0 or imm 16.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
	setXmm(X86_REG_XMM1, 0xaabbccddeeff0011ULL, 0x2233445566778899ULL);

	emulate("palignr xmm0, xmm1, 4");

	EXPECT_EQ(0xeeff001122334455ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xddeeff00aabbccddULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PALIGNR_of_zero_is_the_source)
{
	// One of the two shift amounts that would be a shift of exactly the
	// operand width, which is poison, if this were spelled as one i128 shift
	// pair without separating the cases.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
	setXmm(X86_REG_XMM1, 0xaabbccddeeff0011ULL, 0x2233445566778899ULL);

	emulate("palignr xmm0, xmm1, 0");

	EXPECT_EQ(0x2233445566778899ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0xaabbccddeeff0011ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PALIGNR_of_sixteen_is_the_destination)
{
	// The other one.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
	setXmm(X86_REG_XMM1, 0xaabbccddeeff0011ULL, 0x2233445566778899ULL);

	emulate("palignr xmm0, xmm1, 16");

	EXPECT_EQ(0x99aabbccddeeff00ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0x1122334455667788ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PALIGNR_past_the_top_is_zero)
{
	// Architecturally zero, not a wrapped shift: the window has moved
	// entirely past the top of the concatenation.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
	setXmm(X86_REG_XMM1, 0xaabbccddeeff0011ULL, 0x2233445566778899ULL);

	emulate("palignr xmm0, xmm1, 32");

	EXPECT_EQ(0ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PAVGB_adds_one_bit_wider_than_the_lane)
{
	// 0xff and 0x02 average to 0x81, and 0x80 and 0x81 also average to 0x81.
	// Both sums carry out of eight bits, so an implementation that adds at
	// the lane's own width answers 0x01 for each -- which is why the inputs
	// are these and not two values that happen to fit.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, 0x00000000000080ffULL);
	setXmm(X86_REG_XMM1, 0, 0x0000000000008102ULL);

	emulate("pavgb xmm0, xmm1");

	EXPECT_EQ(0x0000000000008181ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PAVGW_rounds_up)
{
	// 0xffff and 0x0002 average to 0x8001 and not 0x8000: the +1 is in the
	// instruction, and dropping it moves every odd sum by one.
	SKIP_MODE_16;

	setXmm(X86_REG_XMM0, 0, 0x000000000000ffffULL);
	setXmm(X86_REG_XMM1, 0, 0x0000000000000002ULL);

	emulate("pavgw xmm0, xmm1");

	EXPECT_EQ(0x0000000000008001ULL, xmmLow(X86_REG_XMM0));
	EXPECT_EQ(0ULL, xmmHigh(X86_REG_XMM0));
	EXPECT_NO_VALUE_CALLED();
}


//
// BMI1 and BMI2, MOVBE, PAUSE.
//
// What is left of the non-AVX gap after Batch B, and all of it on general
// purpose registers: SHRX 807, SARX 756, BZHI 633, SHLX 381, BLSMSK 1,008,
// MOVBE 672, PAUSE 168, ANDN 84, BLSR 84.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ANDN_complements_the_first_source)
{
	// ~ebx & ecx, not ~ecx & ebx. The reversed reading answers 0x0d0b0907
	// here, so the operands are deliberately not symmetric.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x0f0f0f0f},
		{X86_REG_ECX, 0x12345678},
	});

	emulate("andn eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x10305070},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_ANDN_sets_SF_from_the_result)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0},
		{X86_REG_ECX, 0x80000000},
	});

	emulate("andn eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x80000000},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BLSI_isolates_the_lowest_set_bit)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x18}, // bits 3 and 4
	});

	emulate("blsi eax, ebx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x08},
		{X86_REG_CF, true}, // source is NOT zero
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BLSMSK_masks_up_to_the_lowest_set_bit)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x18},
	});

	emulate("blsmsk eax, ebx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x0f},
		{X86_REG_CF, false}, // source is not zero
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BLSR_clears_the_lowest_set_bit)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x18},
	});

	emulate("blsr eax, ebx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x10},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BLSI_CF_runs_the_other_way_from_BLSR)
{
	// The one input on which the three disagree about CF. BLSI sets it when
	// the source is NOT zero; BLSR and BLSMSK set it when the source IS.
	// Copying one instruction's CF to the others inverts a flag the branch
	// after it reads, on exactly this input and no other.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0},
	});

	emulate("blsi eax, ebx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BLSR_of_zero_sets_CF)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0},
	});

	emulate("blsr eax, ebx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_ZF, true},
		{X86_REG_SF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BEXTR_extracts_a_field)
{
	// Control 0x0408: start at bit 8, take 4 bits. 0x12345678 >> 8 is
	// 0x123456, and its bottom nibble is 6.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
		{X86_REG_ECX, 0x0408},
	});

	emulate("bextr eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x6},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BEXTR_past_the_top_is_zero)
{
	// start = 0x40, which is past a 32-bit operand. An unguarded LLVM shift
	// by more than the width is poison; the instruction's answer is zero.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
		{X86_REG_ECX, 0x0440},
	});

	emulate("bextr eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BEXTR_of_the_full_width_is_the_source)
{
	// len = 0x20 on a 32-bit operand: the mask is every bit, which `1 << len`
	// cannot produce without overflowing.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
		{X86_REG_ECX, 0x2000},
	});

	emulate("bextr eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BZHI_zeroes_from_the_index_up)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
		{X86_REG_ECX, 8},
	});

	emulate("bzhi eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x78},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_BZHI_index_past_the_width_sets_CF)
{
	// CF here is not BLSR's question. It reports that the index was at or
	// past the operand width, in which case nothing is zeroed.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
		{X86_REG_ECX, 32},
	});

	emulate("bzhi eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHLX_touches_no_flags)
{
	// The entire reason a compiler emits SHLX rather than SHL: the shift can
	// be scheduled across a comparison because it does not disturb the
	// flags. EXPECT_JUST_REGISTERS_STORED is the assertion -- it fails if
	// anything other than EAX was written.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
		{X86_REG_ECX, 4},
	});

	emulate("shlx eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x23456780},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHRX_shifts_in_zeroes)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x80000000},
		{X86_REG_ECX, 4},
	});

	emulate("shrx eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x08000000},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SARX_shifts_in_the_sign)
{
	// Same source and count as SHRX above, because the only thing that
	// separates the two is what comes in at the top.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x80000000},
		{X86_REG_ECX, 4},
	});

	emulate("sarx eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xf8000000},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_SHLX_masks_the_count_to_the_width)
{
	// 36 & 31 == 4: the hardware masks the count to the operand width, and an
	// unmasked LLVM shift by 36 of an i32 is poison, so translateShiftX()
	// masks it.
	//
	// This test pins the architectural answer and CANNOT falsify that mask.
	// tests/llvmir-emul's getShiftAmount() applies
	// `(NextPowerOf2(width - 1) - 1) & amount` to any over-wide shift, which
	// for a 32-bit value is `& 31` -- the same mask, so removing the
	// translator's leaves the suite green. Verified by doing exactly that.
	// Second instance of the emulator being more forgiving than LLVM, after
	// llvm.cttz's is_zero_poison.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
		{X86_REG_ECX, 36},
	});

	emulate("shlx eax, ebx, ecx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x23456780},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RORX_rotates_rather_than_shifts)
{
	// The nibble that leaves the bottom comes back at the top: a shift would
	// answer 0x01234567.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
	});

	emulate("rorx eax, ebx, 4");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x81234567},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_RORX_by_zero_is_the_source)
{
	// The one immediate for which the second shift would be by the full
	// operand width, which is poison.
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EBX, 0x12345678},
	});

	emulate("rorx eax, ebx, 0");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x12345678},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_MOVBE_reverses_the_bytes)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RBX, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate("movbe eax, dword ptr [rbx]");

	EXPECT_EQ(0x78563412, getRegisterValueUnsigned(X86_REG_EAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, X86_INS_PAUSE_is_a_hint_with_no_effect)
{
	// `rep nop`. It is a scheduling hint to the spin-wait predictor and does
	// nothing architecturally.
	SKIP_MODE_16;

	emulate("pause");

	EXPECT_NO_REGISTERS_LOADED_STORED();
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

//
// ============================================================================
// AVX-512 opmask registers (k0..k7)
// ============================================================================
//
// Keystone 0.9.2 does not assemble these, so every test below uses the
// encoding that llvm-mc produces, verified against capstone's decode.
//
// Two properties are worth stating because they are what these tests are
// actually for:
//
//   * The width comes from the mnemonic suffix. Capstone reports `size = 2`
//     for EVERY opmask operand -- `kmovq k1, rax` and `kmovb k1, eax` both
//     say 2 -- so the operand cannot supply it.
//   * The destination bits above that width are ZEROED. k0..k7 are i64
//     globals, so every test that writes one pre-loads it with all ones and
//     then checks the result is not merged into.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVW_zeroes_the_bits_above_its_width)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_K2, 0x1234},
	});

	emulate_bin("c5 f8 90 ca"); // kmovw k1, k2

	// Not 0xffffffffffff1234: KMOVW clears k1[63:16].
	EXPECT_EQ(0x1234ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_K2});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVB_gpr_reads_only_eight_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_EAX, 0x12345678},
	});

	emulate_bin("c5 f9 92 c8"); // kmovb k1, eax

	// KMOVW would answer 0x5678 and KMOVD 0x12345678; capstone calls the
	// opmask operand two bytes wide for all three.
	EXPECT_EQ(0x78ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVD_gpr_reads_thirty_two_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_EAX, 0x12345678},
	});

	emulate_bin("c5 fb 92 c8"); // kmovd k1, eax

	EXPECT_EQ(0x12345678ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVQ_gpr_reads_sixty_four_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_RAX, 0x123456789abcdef0ULL},
	});

	emulate_bin("c4 e1 fb 92 c8"); // kmovq k1, rax

	EXPECT_EQ(0x123456789abcdef0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVD_to_gpr_truncates_then_zero_extends)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffff12345678ULL},
	});

	emulate_bin("c5 fb 93 c1"); // kmovd eax, k1

	// The k register holds 64 bits; KMOVD takes the low 32 and the 32-bit
	// write then zeroes the top half of rax.
	EXPECT_EQ(0x12345678ULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVB_to_gpr_zero_extends_one_byte)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffULL},
	});

	emulate_bin("c5 f9 93 c1"); // kmovb eax, k1

	EXPECT_EQ(0xffULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVQ_to_gpr)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0x123456789abcdef0ULL},
	});

	emulate_bin("c4 e1 fb 93 c1"); // kmovq rax, k1

	EXPECT_EQ(0x123456789abcdef0ULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVW_loads_two_bytes_from_memory)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_RAX, 0x1000},
	});
	setMemory({
		{0x1000, 0x1234_w},
	});

	emulate_bin("c5 f8 90 08"); // kmovw k1, word ptr [rax]

	EXPECT_EQ(0x1234ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVW_stores_two_bytes_to_memory)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffff1234ULL},
		{X86_REG_RAX, 0x1000},
	});
	setMemory({
		{0x1000, 0x0000_w},
	});

	emulate_bin("c5 f8 91 08"); // kmovw word ptr [rax], k1

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x1234_w},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KANDNW_negates_the_vvvv_operand)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_K2, 0xf0f0},
		{X86_REG_K3, 0xff00},
	});

	emulate_bin("c5 ec 42 cb"); // kandnw k1, k2, k3

	// ~k2 & k3 == 0x0f0f & 0xff00 == 0x0f00.
	// The other reading, k2 & ~k3, would answer 0x00f0.
	EXPECT_EQ(0x0f00ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_K2, X86_REG_K3});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KANDW)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_K2, 0xf0f0},
		{X86_REG_K3, 0xff00},
	});

	emulate_bin("c5 ec 41 cb"); // kandw k1, k2, k3

	EXPECT_EQ(0xf000ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KORW)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0xf0f0},
		{X86_REG_K3, 0xff00},
	});

	emulate_bin("c5 ec 45 cb"); // korw k1, k2, k3

	EXPECT_EQ(0xfff0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KXORW)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0xf0f0},
		{X86_REG_K3, 0xff00},
	});

	emulate_bin("c5 ec 47 cb"); // kxorw k1, k2, k3

	EXPECT_EQ(0x0ff0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KXNORW_complements_within_sixteen_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0xf0f0},
		{X86_REG_K3, 0xff00},
	});

	emulate_bin("c5 ec 46 cb"); // kxnorw k1, k2, k3

	// ~(0xf0f0 ^ 0xff00) at 16 bits. Complementing the i64 register instead
	// would answer 0xfffffffffffff00f.
	EXPECT_EQ(0xf00fULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KANDQ_operates_on_all_sixty_four_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0xf0f0f0f0f0f0f0f0ULL},
		{X86_REG_K3, 0xff00ff00ff00ff00ULL},
	});

	emulate_bin("c4 e1 ec 41 cb"); // kandq k1, k2, k3

	EXPECT_EQ(0xf000f000f000f000ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KNOTW)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0x1234},
	});

	emulate_bin("c5 f8 44 ca"); // knotw k1, k2

	EXPECT_EQ(0xedcbULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KNOTB_complements_one_byte)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0x1234},
	});

	emulate_bin("c5 f9 44 ca"); // knotb k1, k2

	// ~0x34 within eight bits. KNOTW would answer 0xedcb.
	EXPECT_EQ(0xcbULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KADDW_wraps_at_sixteen_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0xffff},
		{X86_REG_K3, 2},
	});

	emulate_bin("c5 ec 4a cb"); // kaddw k1, k2, k3

	// Adding as i64 would answer 0x10001 and leave a carry out of the mask.
	EXPECT_EQ(0x0001ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KADDB_wraps_at_eight_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0xff},
		{X86_REG_K3, 2},
	});

	emulate_bin("c5 ed 4a cb"); // kaddb k1, k2, k3

	EXPECT_EQ(0x01ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KSHIFTLW)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0x1234},
	});

	emulate_bin("c4 e3 f9 32 ca 04"); // kshiftlw k1, k2, 4

	EXPECT_EQ(0x2340ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KSHIFTLW_by_the_full_width_is_zero)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_K2, 0x1234},
	});

	emulate_bin("c4 e3 f9 32 ca 10"); // kshiftlw k1, k2, 16

	// The count is NOT taken modulo the width -- a masking implementation
	// would shift by zero and answer 0x1234 -- and an i16 `shl` by 16 is
	// poison, so the zero has to be produced explicitly.
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KSHIFTLB_by_the_full_width_is_zero)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_K2, 0x12},
	});

	emulate_bin("c4 e3 79 32 ca 08"); // kshiftlb k1, k2, 8

	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KSHIFTRW)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0x1234},
	});

	emulate_bin("c4 e3 f9 30 ca 04"); // kshiftrw k1, k2, 4

	EXPECT_EQ(0x0123ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KSHIFTRD_shifts_in_zeroes_not_sign)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0x87654321},
	});

	emulate_bin("c4 e3 79 31 ca 08"); // kshiftrd k1, k2, 8

	// An arithmetic shift of the negative i32 would answer 0xff876543.
	EXPECT_EQ(0x00876543ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KUNPCKBW_puts_the_vvvv_operand_high)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_K2, 0xaa},
		{X86_REG_K3, 0x55},
	});

	emulate_bin("c5 ed 4b cb"); // kunpckbw k1, k2, k3

	// SRC1 (k2, the VEX.vvvv operand) is the HIGH byte. The other order
	// would answer 0x55aa.
	EXPECT_EQ(0xaa55ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KUNPCKWD)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0xaaaa},
		{X86_REG_K3, 0x5555},
	});

	emulate_bin("c5 ec 4b cb"); // kunpckwd k1, k2, k3

	EXPECT_EQ(0xaaaa5555ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KUNPCKDQ_ignores_the_sources_upper_halves)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0xffffffffaaaaaaaaULL},
		{X86_REG_K3, 0xffffffff55555555ULL},
	});

	emulate_bin("c4 e1 ec 4b cb"); // kunpckdq k1, k2, k3

	EXPECT_EQ(0xaaaaaaaa55555555ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KORTESTW_sets_ZF_and_clears_everything_else)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0},
	});

	emulate_bin("c5 f8 98 ca"); // kortestw k1, k2

	EXPECT_JUST_REGISTERS_LOADED({X86_REG_K1, X86_REG_K2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, true},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KORTESTW_sets_CF_when_all_sixteen_bits_are_set)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xff00},
		{X86_REG_K2, 0x00ff},
	});

	emulate_bin("c5 f8 98 ca"); // kortestw k1, k2

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, false},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KORTESTB_compares_against_eight_ones)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0x10f0},
		{X86_REG_K2, 0x000f},
	});

	emulate_bin("c5 f9 98 ca"); // kortestb k1, k2

	// At eight bits the OR is 0xff, so CF is set. Reading sixteen bits
	// would give 0x10ff and clear it.
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, false},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KORTESTQ_compares_against_sixty_four_ones)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffff00000000ULL},
		{X86_REG_K2, 0x00000000ffffffffULL},
	});

	emulate_bin("c4 e1 f8 98 ca"); // kortestq k1, k2

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, false},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KTESTW_ZF_is_the_AND_and_CF_is_the_ANDN)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xff00},
		{X86_REG_K2, 0x00ff},
	});

	emulate_bin("c5 f8 99 ca"); // ktestw k1, k2

	// k2 & k1 == 0 -> ZF. k2 & ~k1 == 0x00ff -> CF clear. Swapping the two
	// flags would answer the other way round.
	EXPECT_JUST_REGISTERS_LOADED({X86_REG_K1, X86_REG_K2});
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, true},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KTESTW_CF_uses_the_first_operand_as_the_negated_one)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffff},
		{X86_REG_K2, 0x00ff},
	});

	emulate_bin("c5 f8 99 ca"); // ktestw k1, k2

	// k2 & k1 == 0x00ff -> ZF clear. k2 & ~k1 == 0 -> CF set. Negating the
	// second operand instead -- k1 & ~k2 == 0xff00 -- would clear CF.
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, false},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KTESTB_looks_at_eight_bits)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xff00},
		{X86_REG_K2, 0xff00},
	});

	emulate_bin("c5 f9 99 ca"); // ktestb k1, k2

	// Both low bytes are zero, so ZF is set and CF with it. At sixteen bits
	// the AND would be 0xff00 and ZF would be clear.
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ZF, true},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_SF, false},
		{X86_REG_AF, false},
		{X86_REG_PF, false},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVQ_between_opmask_registers)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0},
		{X86_REG_K2, 0x123456789abcdef0ULL},
	});

	emulate_bin("c4 e1 f8 90 ca"); // kmovq k1, k2

	EXPECT_EQ(0x123456789abcdef0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, KMOVD_between_opmask_registers_drops_the_upper_half)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_K1, 0xffffffffffffffffULL},
		{X86_REG_K2, 0x123456789abcdef0ULL},
	});

	emulate_bin("c4 e1 f9 90 ca"); // kmovd k1, k2

	EXPECT_EQ(0x9abcdef0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

//
// ============================================================================
// ZMM = ZMMH:YMMH:XMM, and the EVEX registers past fifteen
// ============================================================================
//
// A vector register is three globals. The low 128 bits are the XMM global,
// which is what makes a legacy SSE write and an EVEX read of the same
// register see each other; above it sit YMMn_HI (255:128) and ZMMn_HI
// (511:256). All thirty-two registers are held the same way -- 16..31 have
// no legacy alias to worry about, but a uniform rule is one rule.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVDQU64_copies_all_five_hundred_and_twelve_bits)
{
	ONLY_MODE_64;

	const uint64_t src[8] = {
		0x1111111111111111ULL,
		0x2222222222222222ULL,
		0x3333333333333333ULL,
		0x4444444444444444ULL,
		0x5555555555555555ULL,
		0x6666666666666666ULL,
		0x7777777777777777ULL,
		0x8888888888888888ULL,
	};
	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(1, src);
	setZmm(2, junk);

	emulate_bin("62 f1 fe 48 6f d1"); // vmovdqu64 zmm2, zmm1

	// Every word distinct, so a copy that reached only the low 128 or 256
	// bits leaves all-ones behind where this expects a pattern.
	for (unsigned w = 0; w < 8; ++w)
	{
		EXPECT_EQ(src[w], zmmWord(2, w)) << "quadword " << w;
	}
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVDQU64_reaches_the_EVEX_only_registers)
{
	ONLY_MODE_64;

	const uint64_t src[8] = {
		0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL, 0xccccccccccccccccULL, 0xddddddddddddddddULL, 0, 0, 0, 0};
	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(16, src);
	setZmm(17, junk);

	emulate_bin("62 a1 fe 28 6f c8"); // vmovdqu64 ymm17, ymm16

	// ymm16 and ymm17 are past the sixteen a VEX-encoded instruction can
	// name. A predicate that stopped at fifteen sends this to pseudo-assembly.
	EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, zmmWord(17, 0));
	EXPECT_EQ(0xbbbbbbbbbbbbbbbbULL, zmmWord(17, 1));
	EXPECT_EQ(0xccccccccccccccccULL, zmmWord(17, 2));
	EXPECT_EQ(0xddddddddddddddddULL, zmmWord(17, 3));
	// A 256-bit write clears 511:256.
	EXPECT_EQ(0ULL, zmmWord(17, 4));
	EXPECT_EQ(0ULL, zmmWord(17, 7));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVDQA64_reaches_registers_past_sixteen_at_full_width)
{
	ONLY_MODE_64;

	const uint64_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(16, src);
	setZmm(20, junk);

	emulate_bin("62 a1 fd 48 6f e0"); // vmovdqa64 zmm20, zmm16

	for (unsigned w = 0; w < 8; ++w)
	{
		EXPECT_EQ(src[w], zmmWord(20, w)) << "quadword " << w;
	}
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, The_low_quarter_of_a_ZMM_register_IS_the_XMM_register)
{
	ONLY_MODE_64;

	const uint64_t src[8] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL, 9, 9, 9, 9, 9, 9};
	setZmm(3, src);

	emulate_bin("c5 f9 6f eb"); // vmovdqa xmm5, xmm3

	// The 128-bit read of xmm3 must see what was written as zmm3's low
	// quarter. Holding zmm3 in its own i512 global would answer zero here.
	EXPECT_EQ(0x0123456789abcdefULL, zmmWord(5, 0));
	EXPECT_EQ(0xfedcba9876543210ULL, zmmWord(5, 1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, A_256_bit_write_zeroes_bits_511_to_256)
{
	ONLY_MODE_64;

	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	const uint64_t src[8] = {1, 2, 3, 4, 0, 0, 0, 0};
	setZmm(1, junk);
	setZmm(2, src);

	emulate_bin("c5 fe 6f ca"); // vmovdqu ymm1, ymm2

	EXPECT_EQ(1ULL, zmmWord(1, 0));
	EXPECT_EQ(4ULL, zmmWord(1, 3));
	EXPECT_EQ(0ULL, zmmWord(1, 4));
	EXPECT_EQ(0ULL, zmmWord(1, 5));
	EXPECT_EQ(0ULL, zmmWord(1, 6));
	EXPECT_EQ(0ULL, zmmWord(1, 7));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, A_128_bit_VEX_write_zeroes_bits_511_to_128)
{
	ONLY_MODE_64;

	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	const uint64_t src[8] = {7, 8, 0, 0, 0, 0, 0, 0};
	setZmm(1, junk);
	setZmm(2, src);

	emulate_bin("c5 fa 6f ca"); // vmovdqu xmm1, xmm2

	EXPECT_EQ(7ULL, zmmWord(1, 0));
	EXPECT_EQ(8ULL, zmmWord(1, 1));
	for (unsigned w = 2; w < 8; ++w)
	{
		EXPECT_EQ(0ULL, zmmWord(1, w)) << "quadword " << w;
	}
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPADDB_adds_sixty_four_byte_lanes)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {0x01ff01ff01ff01ffULL, 0, 0, 0, 0, 0, 0, 0x01ff01ff01ff01ffULL};
	const uint64_t b[8] = {0x0101010101010101ULL, 0, 0, 0, 0, 0, 0, 0x0101010101010101ULL};
	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(2, a);
	setZmm(1, b);
	setZmm(3, junk);

	emulate_bin("62 f1 6d 48 fc d9"); // vpaddb zmm3, zmm2, zmm1

	// 0xff + 0x01 wraps within its own byte; a whole-register add would
	// carry into the neighbouring lane. The top quadword proves the lanes
	// past 256 bits are being added at all.
	EXPECT_EQ(0x0200020002000200ULL, zmmWord(3, 0));
	EXPECT_EQ(0x0200020002000200ULL, zmmWord(3, 7));
	EXPECT_EQ(0ULL, zmmWord(3, 3));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VZEROUPPER_zeroes_the_ZMM_high_quarter_too)
{
	ONLY_MODE_64;

	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(1, junk);

	emulate_bin("c5 f8 77"); // vzeroupper

	// The low 128 bits survive; everything above them goes.
	EXPECT_EQ(~0ULL, zmmWord(1, 0));
	EXPECT_EQ(~0ULL, zmmWord(1, 1));
	for (unsigned w = 2; w < 8; ++w)
	{
		EXPECT_EQ(0ULL, zmmWord(1, w)) << "quadword " << w;
	}
}

//
// The EVEX modifiers this does not model, and must therefore not answer.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, An_EVEX_broadcast_operand_is_not_read_at_the_register_width)
{
	ONLY_MODE_64;

	// vpaddd zmm2, zmm1, dword ptr [rdi]{1to16}
	auto* f = translate(retdec::utils::hexStringToBytes("62 f1 75 58 fe 17"));
	ASSERT_NE(nullptr, f);

	// Capstone reports this operand as four bytes and leaves avx_bcast at
	// zero -- the {1to16} appears only in op_str. Taking the width from the
	// register operands instead would emit a 512-bit load of memory the
	// instruction never touches.
	// A load whose pointer is a global is a register read; only a load
	// through an IntToPtr is a read of the program's memory.
	for (auto it = llvm::inst_begin(f), e = llvm::inst_end(f); it != e; ++it)
	{
		auto* l = dyn_cast<LoadInst>(&*it);
		if (l == nullptr || !isa<IntToPtrInst>(l->getPointerOperand()))
		{
			continue;
		}
		EXPECT_EQ(32u, l->getType()->getPrimitiveSizeInBits())
			<< "a broadcast operand was not read at the width capstone reported";
	}
	EXPECT_TRUE(callsPseudoAsm(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, A_write_masked_EVEX_move_is_not_answered_as_the_unmasked_one)
{
	ONLY_MODE_64;

	// vmovdqu8 zmm1{k2}, zmmword ptr [rdi]
	auto* f = translate(retdec::utils::hexStringToBytes("62 f1 7f 4a 6f 0f"));
	ASSERT_NE(nullptr, f);

	// Capstone surfaces the write mask as an extra operand rather than a
	// flag. Answering the unmasked form would overwrite every lane k2 says
	// to leave alone, so the only correct outcome is the pseudo-assembly
	// call -- an opaque value is honest where a wrong one is not.
	EXPECT_TRUE(callsPseudoAsm(f)) << dumpFunction(f);
}

//
// The scalar moves. Every form that writes a vector register clears
// everything above the element it writes.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVQ_register_to_register_clears_the_upper_half)
{
	ONLY_MODE_64;

	const uint64_t src[8] = {0x0123456789abcdefULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(1, src);
	setZmm(2, junk);

	emulate_bin("c5 fa 7e d1"); // vmovq xmm2, xmm1

	// This is the form whose only purpose is to clear. Routing it through a
	// whole-register move would copy all 128 bits and leave bits 127:64 set.
	EXPECT_EQ(0x0123456789abcdefULL, zmmWord(2, 0));
	for (unsigned w = 1; w < 8; ++w)
	{
		EXPECT_EQ(0ULL, zmmWord(2, w)) << "quadword " << w;
	}
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVD_from_a_gpr_writes_thirty_two_bits_and_clears_the_rest)
{
	ONLY_MODE_64;

	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(0, junk);
	setRegisters({
		{X86_REG_RSI, 0xffffffff12345678ULL},
	});

	emulate_bin("c5 f9 6e c6"); // vmovd xmm0, esi

	// Thirty-two bits, not sixty-four: the source is esi.
	EXPECT_EQ(0x12345678ULL, zmmWord(0, 0));
	for (unsigned w = 1; w < 8; ++w)
	{
		EXPECT_EQ(0ULL, zmmWord(0, w)) << "quadword " << w;
	}
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVD_to_a_gpr_takes_the_low_thirty_two_bits)
{
	ONLY_MODE_64;

	const uint64_t src[8] = {0xfedcba9812345678ULL, ~0ULL, 0, 0, 0, 0, 0, 0};
	setZmm(0, src);

	emulate_bin("c5 f9 7e c6"); // vmovd esi, xmm0

	EXPECT_EQ(0x12345678ULL, getRegisterValueUnsigned(X86_REG_RSI));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVQ_from_a_gpr_writes_sixty_four_bits)
{
	ONLY_MODE_64;

	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(1, junk);
	setRegisters({
		{X86_REG_RSI, 0x0123456789abcdefULL},
	});

	emulate_bin("c4 e1 f9 6e ce"); // vmovq xmm1, rsi

	EXPECT_EQ(0x0123456789abcdefULL, zmmWord(1, 0));
	for (unsigned w = 1; w < 8; ++w)
	{
		EXPECT_EQ(0ULL, zmmWord(1, w)) << "quadword " << w;
	}
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVQ_to_a_gpr_takes_the_low_sixty_four_bits)
{
	ONLY_MODE_64;

	const uint64_t src[8] = {0x0123456789abcdefULL, ~0ULL, 0, 0, 0, 0, 0, 0};
	setZmm(1, src);

	emulate_bin("c4 e1 f9 7e ce"); // vmovq rsi, xmm1

	EXPECT_EQ(0x0123456789abcdefULL, getRegisterValueUnsigned(X86_REG_RSI));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVQ_loads_eight_bytes_and_clears_the_rest)
{
	ONLY_MODE_64;

	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(1, junk);
	setRegisters({
		{X86_REG_RSI, 0x1000},
	});
	setMemory({
		{0x1000, 0x0123456789abcdef_qw},
	});

	emulate_bin("c5 fa 7e 0e"); // vmovq xmm1, qword ptr [rsi]

	EXPECT_EQ(0x0123456789abcdefULL, zmmWord(1, 0));
	EXPECT_EQ(0ULL, zmmWord(1, 1));
	EXPECT_EQ(0ULL, zmmWord(1, 7));
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVQ_stores_eight_bytes)
{
	ONLY_MODE_64;

	const uint64_t src[8] = {0x0123456789abcdefULL, ~0ULL, 0, 0, 0, 0, 0, 0};
	setZmm(1, src);
	setRegisters({
		{X86_REG_RSI, 0x1000},
	});
	setMemory({
		{0x1000, 0x0_qw},
	});

	emulate_bin("c5 f9 d6 0e"); // vmovq qword ptr [rsi], xmm1

	EXPECT_JUST_MEMORY_STORED({
		{0x1000, 0x0123456789abcdef_qw},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VADDPS_adds_sixteen_float_lanes)
{
	ONLY_MODE_64;

	// Two 1.0f lanes per quadword, and two 2.0f.
	const uint64_t a[8] = {
		0x3f8000003f800000ULL,
		0x3f8000003f800000ULL,
		0x3f8000003f800000ULL,
		0x3f8000003f800000ULL,
		0x3f8000003f800000ULL,
		0x3f8000003f800000ULL,
		0x3f8000003f800000ULL,
		0x3f8000003f800000ULL};
	const uint64_t b[8] = {
		0x4000000040000000ULL,
		0x4000000040000000ULL,
		0x4000000040000000ULL,
		0x4000000040000000ULL,
		0x4000000040000000ULL,
		0x4000000040000000ULL,
		0x4000000040000000ULL,
		0x4000000040000000ULL};
	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(2, a);
	setZmm(1, b);
	setZmm(3, junk);

	emulate_bin("62 f1 6c 48 58 d9"); // vaddps zmm3, zmm2, zmm1

	// 1.0f + 2.0f = 3.0f in every one of the sixteen lanes, including the
	// eight past 256 bits.
	for (unsigned w = 0; w < 8; ++w)
	{
		EXPECT_EQ(0x4040000040400000ULL, zmmWord(3, w)) << "quadword " << w;
	}
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, An_embedded_rounding_mode_is_not_answered_as_the_plain_form)
{
	ONLY_MODE_64;

	// vaddps zmm3, zmm2, zmm1, {rn-sae}
	auto* f = translate(retdec::utils::hexStringToBytes("62 f1 6c 18 58 d9"));
	ASSERT_NE(nullptr, f);

	// Three plain register operands and no write mask: nothing about this
	// instruction's SHAPE distinguishes it from the ordinary vaddps. Only
	// avx_sae and avx_rm do, and suppressing exceptions while pinning the
	// rounding mode is not what an IEEE fadd computes.
	EXPECT_TRUE(callsPseudoAsm(f)) << dumpFunction(f);
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VMOVD_loads_four_bytes_not_eight)
{
	ONLY_MODE_64;

	const uint64_t junk[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(1, junk);
	setRegisters({
		{X86_REG_RSI, 0x1000},
	});
	setMemory({
		{0x1000, 0x12345678_dw},
	});

	emulate_bin("c5 f9 6e 0e"); // vmovd xmm1, dword ptr [rsi]

	// Four bytes. Reading eight would disagree with the operand size capstone
	// reported and send the instruction to pseudo-assembly instead.
	EXPECT_EQ(0x12345678ULL, zmmWord(1, 0));
	for (unsigned w = 1; w < 8; ++w)
	{
		EXPECT_EQ(0ULL, zmmWord(1, w)) << "quadword " << w;
	}
	EXPECT_JUST_MEMORY_LOADED({0x1000});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, A_fallback_reading_a_ZMM_operand_reads_the_slices_not_the_dead_global)
{
	ONLY_MODE_64;

	// vmovdqu8 zmm1{k2}, zmmword ptr [rdi] -- a merge-masked move, which
	// reads zmm1's old value and which this translator declines to model.
	auto* f = translate(retdec::utils::hexStringToBytes("62 f1 7f 4a 6f 0f"));
	ASSERT_NE(nullptr, f);

	// The i512 X86_REG_ZMM1 global exists in the register file and no
	// translated instruction ever writes it. A fallback that read it would
	// be reading a register that is permanently zero while looking, in the
	// output, exactly like one that had been read correctly.
	bool readsXmm1 = false;
	bool readsDeadZmm1 = false;
	for (auto it = llvm::inst_begin(f), e = llvm::inst_end(f); it != e; ++it)
	{
		auto* l = dyn_cast<LoadInst>(&*it);
		if (l == nullptr)
		{
			continue;
		}
		if (l->getPointerOperand() == getRegister(X86_REG_XMM1))
		{
			readsXmm1 = true;
		}
		if (l->getPointerOperand() == getRegister(X86_REG_ZMM1))
		{
			readsDeadZmm1 = true;
		}
	}
	EXPECT_TRUE(readsXmm1) << dumpFunction(f);
	EXPECT_FALSE(readsDeadZmm1) << dumpFunction(f);
}

//
// ============================================================================
// Comparisons whose destination is an opmask register
// ============================================================================
//
// Capstone 5.0.9 returns the WRONG instruction id for the whole EVEX VPCMP
// family: `X86_INS_VPCMPB + predicate`, ignoring element width and signedness.
// One id therefore covers up to nine different instructions, and one of them
// is the id of an unrelated SSE4.2 string instruction. Several tests below
// exist only to prove the translator reads the mnemonic and not the id --
// each one names the instruction the id claims it is.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPEQB_produces_one_bit_per_byte_lane)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0102030405060708ULL,
		0x0102030405060708ULL,
		0x0102030405060708ULL,
		0x0102030405060708ULL,
		0x0102030405060708ULL,
		0x0102030405060708ULL,
		0x0102030405060708ULL,
		0x0102030405060708ULL};
	const uint64_t b[8] = {
		0x0102030405060700ULL,
		0x0102030405060700ULL,
		0x0102030405060700ULL,
		0x0102030405060700ULL,
		0x0102030405060700ULL,
		0x0102030405060700ULL,
		0x0102030405060700ULL,
		0x0102030405060700ULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0xffffffffffffffffULL}});

	emulate_bin("62 f1 6d 48 74 c9"); // vpcmpeqb k1, zmm2, zmm1

	// Byte 0 of each quadword differs and the other seven match, so every
	// group of eight bits reads 0xfe. Lane 0 is bit 0: the pattern is not
	// symmetric, so a reversed lane order would answer 0x7f7f...
	EXPECT_EQ(0xfefefefefefefefeULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPEQB_at_128_bits_zeroes_the_mask_above_sixteen_lanes)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {0x0102030405060708ULL, 0x0102030405060708ULL, 0, 0, 0, 0, 0, 0};
	const uint64_t b[8] = {0x0102030405060700ULL, 0x0102030405060700ULL, 0, 0, 0, 0, 0, 0};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0xffffffffffffffffULL}});

	emulate_bin("62 f1 6d 08 74 c9"); // vpcmpeqb k1, xmm2, xmm1

	// Sixteen lanes, and k1[63:16] cleared.
	EXPECT_EQ(0xfefeULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPLTUB_is_unsigned)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL};
	const uint64_t b[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0}});

	emulate_bin("62 f3 6d 48 3e c9 01"); // vpcmpltub k1, zmm2, zmm1

	// 1 < 255 unsigned in every lane. Signed it would be 1 < -1, which is
	// false everywhere -- and capstone gives vpcmpltub and vpcmpltb the SAME
	// id, so only the mnemonic separates them.
	EXPECT_EQ(0xffffffffffffffffULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPLTB_is_signed_and_shares_its_id_with_VPCMPLTUB)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL};
	const uint64_t b[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, ~0ULL}});

	emulate_bin("62 f3 6d 48 3f c9 01"); // vpcmpltb k1, zmm2, zmm1

	// Same id (X86_INS_VPCMPD), same operands, opposite answer: 1 < -1 is
	// false in every lane.
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPLEUB_is_not_the_VPCMPEQB_its_id_claims)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL};
	const uint64_t b[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0}});

	emulate_bin("62 f3 6d 48 3e c9 02"); // vpcmpleub k1, zmm2, zmm1

	// Capstone reports this as X86_INS_VPCMPEQB. 1 <= 255 unsigned is true
	// everywhere; a byte equality would be false everywhere.
	EXPECT_EQ(0xffffffffffffffffULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPNLEB_is_not_the_VPCMPESTRI_its_id_claims)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL};
	const uint64_t b[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0}});

	emulate_bin("62 f3 6d 48 3f c9 06"); // vpcmpnleb k1, zmm2, zmm1

	// Capstone reports this 512-bit vector compare as X86_INS_VPCMPESTRI,
	// an SSE4.2 string instruction. Signed 1 > -1 is true in every lane.
	EXPECT_EQ(0xffffffffffffffffULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPNEQUB_is_not_the_VPCMPEQQ_its_id_claims)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL,
		0x0101010101010101ULL};
	const uint64_t b[8] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0}});

	emulate_bin("62 f3 6d 48 3e c9 04"); // vpcmpnequb k1, zmm2, zmm1

	// Capstone reports this as X86_INS_VPCMPEQQ. Sixty-four byte lanes all
	// differ, so the answer is all ones; eight quadword equalities would
	// answer zero.
	EXPECT_EQ(0xffffffffffffffffULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPB_with_the_FALSE_predicate_clears_every_lane)
{
	ONLY_MODE_64;

	const uint64_t v[8] = {7, 7, 7, 7, 7, 7, 7, 7};
	setZmm(2, v);
	setZmm(1, v);
	setRegisters({{X86_REG_K1, ~0ULL}});

	emulate_bin("62 f3 6d 48 3f c9 03"); // vpcmpb k1, zmm2, zmm1, 3

	// Predicates 3 and 7 have no mnemonic of their own -- capstone renders
	// both as plain `vpcmpb` and puts the predicate in a fourth operand. The
	// operands are equal, so reading the predicate as EQ would answer all
	// ones.
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPB_with_the_TRUE_predicate_sets_every_lane)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	const uint64_t b[8] = {8, 7, 6, 5, 4, 3, 2, 1};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0}});

	emulate_bin("62 f3 6d 48 3f c9 07"); // vpcmpb k1, zmm2, zmm1, 7

	EXPECT_EQ(0xffffffffffffffffULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPEQD_compares_sixteen_dword_lanes)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0000000100000001ULL,
		0x0000000100000001ULL,
		0x0000000100000001ULL,
		0x0000000100000001ULL,
		0x0000000100000001ULL,
		0x0000000100000001ULL,
		0x0000000100000001ULL,
		0x0000000100000001ULL};
	const uint64_t b[8] = {
		0x0000000100000002ULL,
		0x0000000100000002ULL,
		0x0000000100000002ULL,
		0x0000000100000002ULL,
		0x0000000100000002ULL,
		0x0000000100000002ULL,
		0x0000000100000002ULL,
		0x0000000100000002ULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, ~0ULL}});

	emulate_bin("62 f1 6d 48 76 c9"); // vpcmpeqd k1, zmm2, zmm1

	// The low dword of each quadword differs and the high one matches:
	// sixteen lanes, alternating, and everything above them cleared.
	EXPECT_EQ(0xaaaaULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPEQQ_compares_eight_quadword_lanes)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	const uint64_t b[8] = {1, 0, 3, 0, 5, 0, 7, 0};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, ~0ULL}});

	emulate_bin("62 f2 ed 48 29 c9"); // vpcmpeqq k1, zmm2, zmm1

	// Lanes 0, 2, 4 and 6 match.
	EXPECT_EQ(0x55ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPTESTMB_sets_a_lane_when_the_AND_is_non_zero)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL};
	const uint64_t b[8] = {
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0}});

	emulate_bin("62 f2 6d 48 26 c9"); // vptestmb k1, zmm2, zmm1

	// Even byte lanes AND to 0x0f, odd ones to zero.
	EXPECT_EQ(0x5555555555555555ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPTESTNMB_is_the_complement)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL};
	const uint64_t b[8] = {
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL,
		0x00ff00ff00ff00ffULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0}});

	emulate_bin("62 f2 6e 48 26 c9"); // vptestnmb k1, zmm2, zmm1

	EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPTESTMD_tests_dword_lanes_not_byte_ones)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL,
		0x0f0f0f0f0f0f0f0fULL};
	const uint64_t b[8] = {
		0x00000000000000ffULL,
		0x00000000000000ffULL,
		0x00000000000000ffULL,
		0x00000000000000ffULL,
		0x00000000000000ffULL,
		0x00000000000000ffULL,
		0x00000000000000ffULL,
		0x00000000000000ffULL};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, 0}});

	emulate_bin("62 f2 6d 48 27 c9"); // vptestmd k1, zmm2, zmm1

	// Only the low dword of each quadword has any bit in common: sixteen
	// lanes, every other one set. Testing bytes would answer
	// 0x0101010101010101 across sixty-four lanes instead.
	EXPECT_EQ(0x5555ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPEQB_compares_the_EVEX_only_registers)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {
		0x0102030405060708ULL, 0x0102030405060708ULL, 0x0102030405060708ULL, 0x0102030405060708ULL, 0, 0, 0, 0};
	const uint64_t b[8] = {
		0x0102030405060700ULL, 0x0102030405060700ULL, 0x0102030405060700ULL, 0x0102030405060700ULL, 0, 0, 0, 0};
	setZmm(17, a);
	setZmm(16, b);
	setRegisters({{X86_REG_K1, ~0ULL}});

	emulate_bin("62 b1 75 20 74 c8"); // vpcmpeqb k1, ymm17, ymm16

	// Thirty-two lanes, and k1[63:32] cleared.
	EXPECT_EQ(0xfefefefeULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPLTUQ_compares_quadwords_unsigned)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {1, 1, 1, 1, 1, 1, 1, 1};
	const uint64_t b[8] = {~0ULL, 0, ~0ULL, 0, ~0ULL, 0, ~0ULL, 0};
	setZmm(2, a);
	setZmm(1, b);
	setRegisters({{X86_REG_K1, ~0ULL}});

	emulate_bin("62 f3 ed 48 1e c9 01"); // vpcmpltuq k1, zmm2, zmm1

	// 1 < 0xffffffffffffffff unsigned is true; signed it would be 1 < -1,
	// false. Lanes 1, 3, 5 and 7 compare against zero and are false either
	// way, so the answer names the signedness on its own.
	EXPECT_EQ(0x55ULL, getRegisterValueUnsigned(X86_REG_K1));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, VPCMPGTB_with_a_vector_destination_is_still_the_AVX2_compare)
{
	ONLY_MODE_64;

	const uint64_t a[8] = {0x0000000000000001ULL, 0, 0, 0, 0, 0, 0, 0};
	const uint64_t b[8] = {0x00000000000000ffULL, 0, 0, 0, 0, 0, 0, 0};
	setZmm(2, a);
	setZmm(1, b);
	setZmm(3, a);

	emulate_bin("c5 e9 64 d9"); // vpcmpgtb xmm3, xmm2, xmm1

	// Signed 1 > -1 in lane 0, and every other lane is 0 > 0. The result
	// goes into a VECTOR register as all-ones lanes, not into a mask.
	EXPECT_EQ(0xffULL, zmmWord(3, 0));
	EXPECT_EQ(0ULL, zmmWord(3, 1));
	EXPECT_NO_VALUE_CALLED();
}

//
// ============================================================================
// SSE4.2 string comparison: PCMPISTRI
// ============================================================================
//
// Every expected value below was read off the hardware, not worked out by
// hand. A C model of this instruction was first checked against a real
// pcmpistri over 1,760,000 (operand, operand, imm8) triples spanning 40
// control bytes, all four aggregations, both formats, both polarities that do
// anything and both output selections; it matched on every one, each flag
// included. These tests are that oracle's answers for the cases that pin down
// each field of the imm8.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_each_identical_strings)
{
	ONLY_MODE_64;

	// EqualEach, negative polarity: identical and no null anywhere, so every lane matches, the inversion clears them
	// all and the index is the element count.
	setXmm(X86_REG_XMM0, 0x706f6e6d6c6b6a69ULL, 0x6867666564636261ULL);
	setXmm(X86_REG_XMM1, 0x706f6e6d6c6b6a69ULL, 0x6867666564636261ULL);

	emulate_bin("66 0f 3a 63 c1 1a"); // pcmpistri xmm0, xmm1, 0x1a

	EXPECT_EQ(16ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_each_first_difference)
{
	ONLY_MODE_64;

	// The strings differ at index 3 and the negative polarity turns that into the only set bit.
	setXmm(X86_REG_XMM0, 0x706f6e6d6c6b6a69ULL, 0x6867666564636261ULL);
	setXmm(X86_REG_XMM1, 0x706f6e6d6c6b6a69ULL, 0x6867666558636261ULL);

	emulate_bin("66 0f 3a 63 c1 1a"); // pcmpistri xmm0, xmm1, 0x1a

	EXPECT_EQ(3ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_each_both_terminated)
{
	ONLY_MODE_64;

	// Both end at index 3. The elements past the end count as EQUAL -- that forced true is what makes a matching strcmp
	// answer 16 with CF clear.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000636261ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000000636261ULL);

	emulate_bin("66 0f 3a 63 c1 1a"); // pcmpistri xmm0, xmm1, 0x1a

	EXPECT_EQ(16ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_each_differing_short_strings)
{
	ONLY_MODE_64;

	// Two short strings differing at index 2.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000636261ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000000646261ULL);

	emulate_bin("66 0f 3a 63 c1 1a"); // pcmpistri xmm0, xmm1, 0x1a

	EXPECT_EQ(2ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_each_masked_negative)
{
	ONLY_MODE_64;

	// Masked negative inverts only where the second operand is still inside its string.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000636261ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000666564636261ULL);

	emulate_bin("66 0f 3a 63 c1 3a"); // pcmpistri xmm0, xmm1, 0x3a

	EXPECT_EQ(3ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_any_positive)
{
	ONLY_MODE_64;

	// EqualAny: which elements of the second operand appear anywhere in the first. Only the valid part of the first
	// operand counts as the set.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x00000000007a7978ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0066656478636261ULL);

	emulate_bin("66 0f 3a 63 c1 02"); // pcmpistri xmm0, xmm1, 0x02

	EXPECT_EQ(3ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_any_negative_sets_OF_from_bit_zero)
{
	ONLY_MODE_64;

	// The same comparison with the result inverted: OF is bit 0 of the final mask.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x00000000007a7978ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0066656478636261ULL);

	emulate_bin("66 0f 3a 63 c1 12"); // pcmpistri xmm0, xmm1, 0x12

	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_ranges)
{
	ONLY_MODE_64;

	// Ranges: the first operand is a list of (low, high) pairs.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000007a61ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000062613231ULL);

	emulate_bin("66 0f 3a 63 c1 04"); // pcmpistri xmm0, xmm1, 0x04

	EXPECT_EQ(2ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_ordered_substring)
{
	ONLY_MODE_64;

	// EqualOrdered: the offset in the second operand at which the first occurs.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000006362ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000064636261ULL);

	emulate_bin("66 0f 3a 63 c1 0c"); // pcmpistri xmm0, xmm1, 0x0c

	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_most_significant_index)
{
	ONLY_MODE_64;

	// Bit 6 selects the highest set bit instead of the lowest.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000000061ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000061786178ULL);

	emulate_bin("66 0f 3a 63 c1 40"); // pcmpistri xmm0, xmm1, 0x40

	EXPECT_EQ(3ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_least_significant_index)
{
	ONLY_MODE_64;

	// The same operands with bit 6 clear, so that the two answers differ.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000000061ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000061786178ULL);

	emulate_bin("66 0f 3a 63 c1 00"); // pcmpistri xmm0, xmm1, 0x00

	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_word_format)
{
	ONLY_MODE_64;

	// Format 01: eight word lanes rather than sixteen byte lanes.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000001234ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000012349999ULL);

	emulate_bin("66 0f 3a 63 c1 01"); // pcmpistri xmm0, xmm1, 0x01

	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_signed_ranges_are_signed)
{
	ONLY_MODE_64;

	// Format 10 makes the range comparison signed: 0xf0 is -16, which is inside the range -32..-1 and outside 0..127.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x000000000000ffe0ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x000000000000f010ULL);

	emulate_bin("66 0f 3a 63 c1 44"); // pcmpistri xmm0, xmm1, 0x44

	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_unsigned_ranges_are_unsigned)
{
	ONLY_MODE_64;

	// The same bytes read unsigned: 0xf0 is 240, outside 224..255? -- whatever the hardware says.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x000000000000ffe0ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x000000000000f010ULL);

	emulate_bin("66 0f 3a 63 c1 18"); // pcmpistri xmm0, xmm1, 0x18

	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_AF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_PF));
	EXPECT_NO_VALUE_CALLED();
}


//
// The cases below exist because the fourteen above did not catch five real
// mistakes. Each of those five was reverted alone and run against the
// differential oracle, which failed thousands of times on every one; these
// are the specific inputs that make each visible to the committed suite.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_equal_any_ignores_the_set_past_its_terminator)
{
	ONLY_MODE_64;

	// The 'z' in the first operand sits past its null and is not part of the set, so nothing matches. Comparing against
	// it anyway would answer index 0.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x00000000007a0061ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x000000000000007aULL);

	emulate_bin("66 0f 3a 63 c1 00"); // pcmpistri xmm0, xmm1, 0x00

	EXPECT_EQ(16ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_ranges_ignores_a_pair_past_the_terminator)
{
	ONLY_MODE_64;

	// The second pair begins at the null, so it is not a range. Taking it would give 0x00..'q', which contains '1' and
	// would answer index 0.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000071007a61ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000000000031ULL);

	emulate_bin("66 0f 3a 63 c1 04"); // pcmpistri xmm0, xmm1, 0x04

	EXPECT_EQ(16ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_masked_negative_differs_from_plain_negative)
{
	ONLY_MODE_64;

	// Inverting only inside the second operand's string answers 3; inverting everything answers 2.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000636261ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000000006261ULL);

	emulate_bin("66 0f 3a 63 c1 3a"); // pcmpistri xmm0, xmm1, 0x3a

	EXPECT_EQ(3ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_plain_negative_on_the_same_operands)
{
	ONLY_MODE_64;

	// The same operands under plain negative polarity, so the two answers differ.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000636261ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000000006261ULL);

	emulate_bin("66 0f 3a 63 c1 1a"); // pcmpistri xmm0, xmm1, 0x1a

	EXPECT_EQ(2ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_ZF_is_the_second_operand_and_SF_the_first)
{
	ONLY_MODE_64;

	// Only the second operand has a terminator. ZF follows the second operand and SF the first, so swapping them is
	// visible here and nowhere that both or neither terminate.
	setXmm(X86_REG_XMM0, 0x706f6e6d6c6b6a69ULL, 0x6867666564636261ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000000636261ULL);

	emulate_bin("66 0f 3a 63 c1 1a"); // pcmpistri xmm0, xmm1, 0x1a

	EXPECT_EQ(3ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_SF_alone_when_only_the_first_terminates)
{
	ONLY_MODE_64;

	// The mirror image: only the first operand has a terminator.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x0000000000636261ULL);
	setXmm(X86_REG_XMM1, 0x706f6e6d6c6b6a69ULL, 0x6867666564636261ULL);

	emulate_bin("66 0f 3a 63 c1 1a"); // pcmpistri xmm0, xmm1, 0x1a

	EXPECT_EQ(3ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, PCMPISTRI_signed_ranges_read_the_bounds_as_signed)
{
	ONLY_MODE_64;

	// Format 10 makes this the range -32..32, which contains 5. Read unsigned it is 224..32, which is empty and would
	// answer the element count.
	setXmm(X86_REG_XMM0, 0x0000000000000000ULL, 0x00000000000020e0ULL);
	setXmm(X86_REG_XMM1, 0x0000000000000000ULL, 0x0000000000000005ULL);

	emulate_bin("66 0f 3a 63 c1 06"); // pcmpistri xmm0, xmm1, 0x06

	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_RCX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_ZF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_SF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_NO_VALUE_CALLED();
}


//
// FADD is the one x87 arithmetic instruction whose popping form has no
// capstone id of its own. `fmulp` is X86_INS_FMULP, `fsubp` is
// X86_INS_FSUBP, `fdivp` is X86_INS_FDIVP, `fstp` is X86_INS_FSTP -- and
// `faddp` is X86_INS_FADD. The pop was gated on that id, which is true for
// every FADD form, so all four non-popping ones popped.
//
// The expectations below were checked against the hardware, with `fnstsw`
// either side of each encoding: only DE pops.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, FADDP_pops_and_the_others_do_not)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 1.0}, // st(0)
		{X86_REG_ST3, 2.0}, // st(1)
	});

	emulate_bin("de c1"); // faddp st(1), st(0)

	// The result lands in st(1) and the stack pops, so TOP moves 2 -> 3.
	EXPECT_JUST_REGISTERS_STORED({
		{X87_REG_TOP, 0x3},
		{X86_REG_ST3, 3.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, FADD_d8_does_not_move_TOP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 1.0}, // st(0)
		{X86_REG_ST3, 2.0}, // st(1)
	});

	emulate_bin("d8 c1"); // fadd st, st(1)

	// Same two registers, one opcode byte different, and no pop.
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 3.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, FADD_dc_does_not_move_TOP)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 1.0}, // st(0)
		{X86_REG_ST3, 2.0}, // st(1)
	});

	emulate_bin("dc c1"); // fadd st(1), st

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST3, 3.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, Two_FADDs_in_a_row_still_see_the_same_stack)
{
	ALL_MODES;

	setRegisters({
		{X87_REG_TOP, 0x2},
		{X86_REG_ST2, 1.0}, // st(0)
		{X86_REG_ST3, 2.0}, // st(1)
	});

	// This is what the bug cost: the second instruction's st(1) is only the
	// same register as the first's if the first did not pop.
	emulate_bin("d8 c1 d8 c1"); // fadd st, st(1) twice

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_ST2, 5.0},
	});
	EXPECT_NO_MEMORY_LOADED_STORED();
}

//
// SBB and NEG, against the hardware
// ---------------------------------
// The expectations below were executed on this machine's CPU, not derived
// from the manual. SBB applied its incoming carry twice -- once folding it
// into the operand and once inside the flag helpers, which load CF
// themselves -- so the borrow was wrong for almost every pair with a carry
// in. ADC next door has always kept its operands and passed the carry
// explicitly, which is why ADC was clean.
//

TEST_P(Capstone2LlvmIrTranslatorX86Tests, SBB_with_a_carry_in_that_does_not_borrow)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
		{X86_REG_EBX, 0x567},
		{X86_REG_CF, true},
	});

	emulate("sbb eax, ebx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xccc},
		{X86_REG_CF, false},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_PF, true},
		{X86_REG_AF, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, SBB_borrows_when_the_carry_in_tips_it_over)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x0},
		{X86_REG_EBX, 0x1},
		{X86_REG_CF, true},
	});

	emulate("sbb eax, ebx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xfffffffe},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_PF, false},
		{X86_REG_AF, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, SBB_of_equal_operands_borrows_only_with_a_carry_in)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1000},
		{X86_REG_EBX, 0x1000},
		{X86_REG_CF, true},
	});

	emulate("sbb eax, ebx");

	// a == b is the boundary the carry decides, and it is where `op1 + carry`
	// would have to be computed without wrapping.
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xffffffff},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_PF, true},
		{X86_REG_AF, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, NEG_of_the_minimum_signed_value_overflows)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x8000000000000000ULL},
	});

	emulate("neg rax");

	// The one input NEG overflows on: its negation is itself. OF was
	// hardcoded to zero.
	EXPECT_EQ(0x8000000000000000ULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, NEG_of_anything_else_does_not_overflow)
{
	ONLY_MODE_64;

	setRegisters({
		{X86_REG_RAX, 0x8000000000000001ULL},
	});

	emulate("neg rax");

	EXPECT_EQ(0x7fffffffffffffffULL, getRegisterValueUnsigned(X86_REG_RAX));
	EXPECT_EQ(0ULL, getRegisterValueUnsigned(X86_REG_OF));
	EXPECT_EQ(1ULL, getRegisterValueUnsigned(X86_REG_CF));
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, SBB_when_the_subtrahend_plus_carry_would_wrap)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0x1234},
		{X86_REG_EBX, 0xffffffff},
		{X86_REG_CF, true},
	});

	emulate("sbb eax, ebx");

	// `op1 + carry` is 0x100000000, which does not fit: computing the borrow
	// as `op0 < op1 + carry` wraps it to zero and answers "no borrow". The
	// hardware borrows.
	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0x1234},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, false},
		{X86_REG_PF, false},
		{X86_REG_AF, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

TEST_P(Capstone2LlvmIrTranslatorX86Tests, SBB_all_ones_from_all_ones_with_a_carry_in)
{
	SKIP_MODE_16;

	setRegisters({
		{X86_REG_EAX, 0xffffffff},
		{X86_REG_EBX, 0xffffffff},
		{X86_REG_CF, true},
	});

	emulate("sbb eax, ebx");

	EXPECT_JUST_REGISTERS_STORED({
		{X86_REG_EAX, 0xffffffff},
		{X86_REG_CF, true},
		{X86_REG_OF, false},
		{X86_REG_ZF, false},
		{X86_REG_SF, true},
		{X86_REG_PF, true},
		{X86_REG_AF, true},
	});
	EXPECT_NO_VALUE_CALLED();
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
