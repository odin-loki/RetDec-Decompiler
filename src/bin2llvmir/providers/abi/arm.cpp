/**
 * @file src/bin2llvmir/providers/abi/arm.cpp
 * @brief ABI information for ARM.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <string>

#include "retdec/bin2llvmir/providers/abi/arm.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiArm::AbiArm(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(ARM_REG_ENDING);
	_id2regs.resize(ARM_REG_ENDING, nullptr);
	_regStackPointerId = ARM_REG_SP;
	_regFunctionReturnId = ARM_REG_R0;

	// system calls
	_regSyscallId = ARM_REG_R7;
	_regSyscallReturn = ARM_REG_R0;
	_syscallRegs = {
			ARM_REG_R0,
			ARM_REG_R1,
			ARM_REG_R2,
			ARM_REG_R3,
			ARM_REG_R4,
			ARM_REG_R5};

	_defcc = CallingConvention::ID::CC_ARM;
}

bool AbiArm::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return (ARM_REG_R0 <= rid && rid <= ARM_REG_R12)
			|| rid == ARM_REG_SP
			|| rid == ARM_REG_LR;
}

bool AbiArm::isNopInstruction(cs_insn* insn)
{
	cs_arm& insnArm = insn->detail->arm;

	// True NOP variants.
	//
	if (insn->id == ARM_INS_NOP || insn->id == ARM_INS_YIELD)
	{
		return true;
	}
	if (insn->id == ARM_INS_HINT)
	{
		std::string m(insn->mnemonic);
		return m == "nop" || m == "yield" || m == "hint";
	}
	// Classic A32 nop: andeq r0, r0, r0
	//
	if (insn->id == ARM_INS_AND
			&& insnArm.cc == ARM_CC_EQ
			&& insnArm.op_count >= 2)
	{
		bool allR0 = true;
		for (unsigned n = 0; n < insnArm.op_count; ++n)
		{
			if (insnArm.operands[n].type != ARM_OP_REG
					|| insnArm.operands[n].reg != ARM_REG_R0)
			{
				allR0 = false;
				break;
			}
		}
		if (allR0)
		{
			return true;
		}
	}
	// mov rN, rN
	//
	if (insn->id == ARM_INS_MOV
			&& insnArm.op_count == 2
			&& insnArm.operands[0].type == ARM_OP_REG
			&& insnArm.operands[1].type == ARM_OP_REG
			&& insnArm.operands[0].reg == insnArm.operands[1].reg)
	{
		return true;
	}

	return false;
}

} // namespace bin2llvmir
} // namespace retdec
