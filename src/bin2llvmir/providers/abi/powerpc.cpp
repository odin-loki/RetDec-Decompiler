/**
 * @file src/bin2llvmir/providers/abi/powerpc.cpp
 * @brief ABI information for PowerPC.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include "retdec/bin2llvmir/providers/abi/powerpc.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiPowerpc::AbiPowerpc(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(PPC_REG_ENDING);
	_id2regs.resize(PPC_REG_ENDING, nullptr);
	_regStackPointerId = PPC_REG_R1;
	_regFunctionReturnId = PPC_REG_R3;

	// Linux 32-bit SYSV: sc takes the call number in r0 and args in r3–r8.
	// The integer result comes back in r3. See docs/internal/wire-ppc.md.
	_regSyscallId = PPC_REG_R0;
	_regSyscallReturn = PPC_REG_R3;
	_syscallRegs = {
			PPC_REG_R3,
			PPC_REG_R4,
			PPC_REG_R5,
			PPC_REG_R6,
			PPC_REG_R7,
			PPC_REG_R8};

	_defcc = CallingConvention::ID::CC_POWERPC;
}

bool AbiPowerpc::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return PPC_REG_R0 <= rid && rid <= PPC_REG_R31;
}

bool AbiPowerpc::isNopInstruction(cs_insn* insn)
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
