/**
 * @file include/retdec/capstone2llvmir/riscv/riscv_defs.h
 * @brief Additional (on top of Capstone) definitions for RISC-V translator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_RISCV_RISCV_DEFS_H
#define RETDEC_CAPSTONE2LLVMIR_RISCV_RISCV_DEFS_H

#include <capstone/riscv.h>

/**
 * Program counter. Capstone 5.0.9 has no PC id in @c riscv_reg because PC
 * is not a GPR; AUIPC/JAL/branches read the instruction address.
 * Synthetic id past @c RISCV_REG_ENDING, same pattern as @c ARM64_REG_PC.
 */
enum riscv_reg_pc
{
	RISCV_REG_PC = RISCV_REG_ENDING + 1,
};

#endif
