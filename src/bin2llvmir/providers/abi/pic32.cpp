/**
 * @file src/bin2llvmir/providers/abi/pic32.cpp
 * @brief ABI information for MIPS.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <algorithm>
#include <iterator>

#include "retdec/bin2llvmir/providers/abi/pic32.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiPic32::AbiPic32(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(MIPS_REG_ENDING);
	_id2regs.resize(MIPS_REG_ENDING, nullptr);
	_regStackPointerId = MIPS_REG_SP;
	_regZeroReg = MIPS_REG_ZERO;
	_regFunctionReturnId = MIPS_REG_V0;

	// Same O32 roles as AbiMips. XC32 interrupt code uses $k0/$k1 as
	// scratch; $gp is the small-data pointer the decoder seeds from the
	// last store to GP. There is no separate ABI field for these ids —
	// they are ordinary GPRs in the Capstone register file.

	// system calls
	_regSyscallId = MIPS_REG_V0;
	_regSyscallReturn = MIPS_REG_V0;
	_syscallRegs = {
			MIPS_REG_A0,
			MIPS_REG_A1,
			MIPS_REG_A2,
			MIPS_REG_A3};

	_defcc = CallingConvention::ID::CC_PIC32;
}

bool AbiPic32::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	// Capstone 6 sorts MIPS registers by name, so MIPS_REG_0 (ZERO) is
	// numbered after MIPS_REG_31 (RA) and the old range check was empty.
	static const uint32_t gprs[] = {
			MIPS_REG_0, MIPS_REG_1, MIPS_REG_2, MIPS_REG_3, MIPS_REG_4, MIPS_REG_5, MIPS_REG_6, MIPS_REG_7,
			MIPS_REG_8, MIPS_REG_9, MIPS_REG_10, MIPS_REG_11, MIPS_REG_12, MIPS_REG_13, MIPS_REG_14, MIPS_REG_15,
			MIPS_REG_16, MIPS_REG_17, MIPS_REG_18, MIPS_REG_19, MIPS_REG_20, MIPS_REG_21, MIPS_REG_22, MIPS_REG_23,
			MIPS_REG_24, MIPS_REG_25, MIPS_REG_26, MIPS_REG_27, MIPS_REG_28, MIPS_REG_29, MIPS_REG_30, MIPS_REG_31};
	return std::find(std::begin(gprs), std::end(gprs), rid) != std::end(gprs);
}

bool AbiPic32::isNopInstruction(cs_insn* insn)
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

std::size_t AbiPic32::getTypeByteSize(llvm::Type* t) const
{
	if (t->isDoubleTy())
	{
		t = Type::getFloatTy(_module->getContext());
	}

	return Abi::getTypeByteSize(t);
}

std::size_t AbiPic32::getTypeBitSize(llvm::Type* t) const
{
	if (t->isDoubleTy())
	{
		t = Type::getFloatTy(_module->getContext());
	}

	return Abi::getTypeBitSize(t);
}

} // namespace bin2llvmir
} // namespace retdec
