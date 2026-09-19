/**
 * @file src/bin2llvmir/providers/abi/riscv.cpp
 * @brief ABI information for RISC-V RV32 (psABI).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/bin2llvmir/providers/abi/riscv.h"
#include "retdec/capstone2llvmir/riscv/riscv_defs.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

AbiRiscv::AbiRiscv(llvm::Module* m, Config* c) :
		Abi(m, c)
{
	_regs.reserve(RISCV_REG_ENDING);
	_id2regs.resize(RISCV_REG_ENDING, nullptr);
	_regStackPointerId = RISCV_REG_SP;
	_regZeroReg = RISCV_REG_X0;
	_regFunctionReturnId = RISCV_REG_A0;

	// Linux / psABI: syscall number in a7, args a0–a6, return a0.
	_regSyscallId = RISCV_REG_A7;
	_regSyscallReturn = RISCV_REG_A0;
	_syscallRegs = {
			RISCV_REG_A0,
			RISCV_REG_A1,
			RISCV_REG_A2,
			RISCV_REG_A3,
			RISCV_REG_A4,
			RISCV_REG_A5,
			RISCV_REG_A6};

	// CC_RISCV is added in the shared wiring patch; until then UNKNOWN.
	_defcc = CallingConvention::ID::CC_UNKNOWN;
}

bool AbiRiscv::isGeneralPurposeRegister(const llvm::Value* val) const
{
	uint32_t rid = getRegisterId(val);
	return RISCV_REG_X0 <= rid && rid <= RISCV_REG_X31;
}

bool AbiRiscv::isNopInstruction(cs_insn* insn)
{
	if (insn->id == RISCV_INS_C_NOP)
	{
		return true;
	}

	if (insn->id == RISCV_INS_ADDI && insn->detail)
	{
		auto& ri = insn->detail->riscv;
		if (ri.op_count == 3
				&& ri.operands[0].type == RISCV_OP_REG
				&& ri.operands[0].reg == RISCV_REG_X0
				&& ri.operands[1].type == RISCV_OP_REG
				&& ri.operands[1].reg == RISCV_REG_X0
				&& ri.operands[2].type == RISCV_OP_IMM
				&& ri.operands[2].imm == 0)
		{
			return true;
		}
	}

	return false;
}

} // namespace bin2llvmir
} // namespace retdec
