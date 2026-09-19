/**
 * @file src/bin2llvmir/providers/abi/sparc64.cpp
 * @brief ABI information for SPARC V9 (SPARC64).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/bin2llvmir/providers/abi/sparc64.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiSparc64::AbiSparc64(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(SPARC_REG_ENDING);
	_id2regs.resize(SPARC_REG_ENDING, nullptr);
	_regStackPointerId = SPARC_REG_SP;
	_regZeroReg = SPARC_REG_G0;
	_regFunctionReturnId = SPARC_REG_O0;

	_regSyscallId = SPARC_REG_G1;
	_regSyscallReturn = SPARC_REG_O0;
	_syscallRegs = {
			SPARC_REG_O0,
			SPARC_REG_O1,
			SPARC_REG_O2,
			SPARC_REG_O3,
			SPARC_REG_O4,
			SPARC_REG_O5};

	// CC_SPARC64 is added in the wiring commit (docs/internal/wire-sparc.md).
	_defcc = CallingConvention::ID::CC_UNKNOWN;
}

bool AbiSparc64::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return (SPARC_REG_G0 <= rid && rid <= SPARC_REG_G7)
			|| (SPARC_REG_O0 <= rid && rid <= SPARC_REG_O5)
			|| rid == SPARC_REG_O7
			|| rid == SPARC_REG_SP
			|| (SPARC_REG_L0 <= rid && rid <= SPARC_REG_L7)
			|| (SPARC_REG_I0 <= rid && rid <= SPARC_REG_I5)
			|| rid == SPARC_REG_I7
			|| rid == SPARC_REG_FP;
}

bool AbiSparc64::isNopInstruction(cs_insn* insn)
{
	return insn->id == SPARC_INS_NOP;
}

} // namespace bin2llvmir
} // namespace retdec
