/**
 * @file src/bin2llvmir/providers/abi/arm64.cpp
 * @brief ABI information for ARM64.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <capstone/arm64.h>

#include "retdec/bin2llvmir/providers/abi/arm64.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiArm64::AbiArm64(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(ARM64_REG_ENDING);
	_id2regs.resize(ARM64_REG_ENDING, nullptr);
	_regStackPointerId = ARM64_REG_SP;
	_regFunctionReturnId = ARM64_REG_X0;
	_regZeroReg = ARM64_REG_XZR;

	// Linux AAPCS64 syscalls: number in X8, args X0–X5, result X0.
	// The same X8 is the AAPCS64 / Windows ARM64 *indirect result*
	// location for large returns (see docs/internal/wire-arm64.md).
	_regSyscallId = ARM64_REG_X8;
	_regSyscallReturn = ARM64_REG_X0;
	_syscallRegs = {
			ARM64_REG_X0,
			ARM64_REG_X1,
			ARM64_REG_X2,
			ARM64_REG_X3,
			ARM64_REG_X4,
			ARM64_REG_X5};

	_defcc = CallingConvention::ID::CC_ARM64;
}

bool AbiArm64::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return ARM64_REG_X0 <= rid && rid <= ARM64_REG_X30;
}

bool AbiArm64::isNopInstruction(cs_insn* insn)
{
	// True NOPs and the AArch64 hints the lifter models as the identity
	// (see arm64_init.cpp). Matching them here lets the decoder skip
	// PAC/BTI landing pads the way it skips x86 NOP/INT3.
	switch (insn->id)
	{
		case ARM64_INS_NOP:
		case ARM64_INS_HINT:
		case ARM64_INS_BTI:
		case ARM64_INS_PACIASP:
		case ARM64_INS_AUTIASP:
		case ARM64_INS_PACIAZ:
		case ARM64_INS_AUTIAZ:
		case ARM64_INS_PACIBSP:
		case ARM64_INS_AUTIBSP:
		case ARM64_INS_XPACLRI:
		case ARM64_INS_PRFM:
		case ARM64_INS_PRFUM:
			return true;
		default:
			return false;
	}
}

} // namespace bin2llvmir
} // namespace retdec
