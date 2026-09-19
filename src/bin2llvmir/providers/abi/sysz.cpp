/**
 * @file src/bin2llvmir/providers/abi/sysz.cpp
 * @brief ABI information for SystemZ (ELF s390x).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/bin2llvmir/providers/abi/sysz.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiSysz::AbiSysz(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(SYSZ_REG_ENDING);
	_id2regs.resize(SYSZ_REG_ENDING, nullptr);
	_regStackPointerId = SYSZ_REG_15;
	_regFunctionReturnId = SYSZ_REG_2;

	// Linux s390x: svc 0; syscall number in r1, args r2-r7, return in r2.
	_regSyscallId = SYSZ_REG_1;
	_regSyscallReturn = SYSZ_REG_2;
	_syscallRegs = {
			SYSZ_REG_2,
			SYSZ_REG_3,
			SYSZ_REG_4,
			SYSZ_REG_5,
			SYSZ_REG_6,
			SYSZ_REG_7};

	// No dedicated CC_SYSZ in CallingConvention::ID yet; ELF s390x is
	// otherwise described by r2-r6 / r14 / r15 above.
	_defcc = CallingConvention::ID::CC_UNKNOWN;
}

bool AbiSysz::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return SYSZ_REG_0 <= rid && rid <= SYSZ_REG_15;
}

bool AbiSysz::isNopInstruction(cs_insn* insn)
{
	// BCR 0,0 (07 00) is the architected NOP. Capstone may keep it as BCR.
	if (insn->id == SYSZ_INS_BCR
			&& insn->size >= 2
			&& insn->bytes[0] == 0x07
			&& insn->bytes[1] == 0x00)
	{
		return true;
	}

	return false;
}

} // namespace bin2llvmir
} // namespace retdec
