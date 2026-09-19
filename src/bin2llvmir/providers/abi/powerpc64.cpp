/**
 * @file src/bin2llvmir/providers/abi/powerpc64.cpp
 * @brief ABI information for PowerPC 64.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include "retdec/bin2llvmir/providers/abi/powerpc64.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiPowerpc64::AbiPowerpc64(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(PPC_REG_ENDING);
	_id2regs.resize(PPC_REG_ENDING, nullptr);
	_regStackPointerId = PPC_REG_R1;
	_regFunctionReturnId = PPC_REG_R3;

	// Linux ppc64 (ELFv1 and ELFv2 share this sc layout): number in r0,
	// args in r3–r8, integer result in r3. ELFv1 descriptors vs ELFv2
	// r12/TOC are calling-convention gaps — see docs/internal/wire-ppc.md.
	_regSyscallId = PPC_REG_R0;
	_regSyscallReturn = PPC_REG_R3;
	_syscallRegs = {
			PPC_REG_R3,
			PPC_REG_R4,
			PPC_REG_R5,
			PPC_REG_R6,
			PPC_REG_R7,
			PPC_REG_R8};

	_defcc = CallingConvention::ID::CC_POWERPC64;
}

bool AbiPowerpc64::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return PPC_REG_R0 <= rid && rid <= PPC_REG_R31;
}

bool AbiPowerpc64::isNopInstruction(cs_insn* insn)
{
	// True NOP variants.
	//
	if (insn->id == PPC_INS_NOP
			|| insn->id == PPC_INS_XNOP)
	{
		return true;
	}

	return false;
}

} // namespace bin2llvmir
} // namespace retdec
