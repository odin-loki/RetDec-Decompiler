/**
 * @file src/bin2llvmir/providers/abi/mips64.cpp
 * @brief ABI information for MIPS.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include "retdec/bin2llvmir/providers/abi/mips64.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiMips64::AbiMips64(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(MIPS_REG_ENDING);
	_id2regs.resize(MIPS_REG_ENDING, nullptr);
	_regStackPointerId = MIPS_REG_SP;
	_regZeroReg = MIPS_REG_ZERO;
	_regFunctionReturnId = MIPS_REG_V0;

	// N64 reserved / special GPRs (same ids as O32):
	//   MIPS_REG_GP ($28)  PIC / small-data pointer
	//   MIPS_REG_K0 ($26)  kernel / exception scratch
	//   MIPS_REG_K1 ($27)  kernel / exception scratch

	// system calls (a0–a3 plus t0–t3 = a4–a7 on n64)
	_regSyscallId = MIPS_REG_V0;
	_regSyscallReturn = MIPS_REG_V0;
	_syscallRegs = {
			MIPS_REG_A0,
			MIPS_REG_A1,
			MIPS_REG_A2,
			MIPS_REG_A3,
			MIPS_REG_T0,
			MIPS_REG_T1,
			MIPS_REG_T2,
			MIPS_REG_T3};

	_defcc = CallingConvention::ID::CC_MIPS64;
}

bool AbiMips64::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return MIPS_REG_0 <= rid && rid <= MIPS_REG_31;
}

bool AbiMips64::isNopInstruction(cs_insn* insn)
{
	// True NOP variants.
	//
	if (insn->id == MIPS_INS_NOP
			|| insn->id == MIPS_INS_SSNOP)
	{
		return true;
	}

	return false;
}

} // namespace bin2llvmir
} // namespace retdec
