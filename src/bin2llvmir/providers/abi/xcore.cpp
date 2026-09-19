/**
 * @file src/bin2llvmir/providers/abi/xcore.cpp
 * @brief ABI information for XCore (XS1, 32-bit).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/bin2llvmir/providers/abi/xcore.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiXcore::AbiXcore(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(XCORE_REG_ENDING);
	_id2regs.resize(XCORE_REG_ENDING, nullptr);
	_regStackPointerId = XCORE_REG_SP;
	_regFunctionReturnId = XCORE_REG_R0;

	_syscallRegs = {
			XCORE_REG_R0,
			XCORE_REG_R1,
			XCORE_REG_R2,
			XCORE_REG_R3};

	_defcc = CallingConvention::ID::CC_UNKNOWN;
}

bool AbiXcore::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return XCORE_REG_R0 <= rid && rid <= XCORE_REG_R11;
}

bool AbiXcore::isNopInstruction(cs_insn* insn)
{
	(void)insn;
	// XS1 has no dedicated NOP opcode.
	return false;
}

} // namespace bin2llvmir
} // namespace retdec
