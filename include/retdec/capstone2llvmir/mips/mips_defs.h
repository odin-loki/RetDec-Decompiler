/**
 * @file include/retdec/capstone2llvmir/mips/mips_defs.h
 * @brief Additional (on top of Capstone) definitions for MIPS translator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_MIPS_MIPS_DEFS_H
#define RETDEC_CAPSTONE2LLVMIR_MIPS_MIPS_DEFS_H

/**
 * 64-bit double precision floating point registers used on 32-bit systems
 * to represent floating point regiters pairs.
 * e.g. MIPS_REG_FD4 = (MIPS_REG_F4, MIPS_REG_F5)
 * In HW, there is only one register array, but it would be very hard and ugly
 * to model 64-bit operations on 2x32-bit pairs.
 */
enum mips_reg_fpu_double
{
	MIPS_REG_FD0 = MIPS_REG_ENDING + 1,
	MIPS_REG_FD2,
	MIPS_REG_FD4,
	MIPS_REG_FD6,
	MIPS_REG_FD8,
	MIPS_REG_FD10,
	MIPS_REG_FD12,
	MIPS_REG_FD14,
	MIPS_REG_FD16,
	MIPS_REG_FD18,
	MIPS_REG_FD20,
	MIPS_REG_FD22,
	MIPS_REG_FD24,
	MIPS_REG_FD26,
	MIPS_REG_FD28,
	MIPS_REG_FD30,
};

/**
 * The thread pointer, read with `rdhwr rt, $29`.
 *
 * Capstone reports the hardware-register number as an ordinary GPR id, and
 * MIPS_REG_29 is the same id as MIPS_REG_SP -- so a translation that took
 * operand 1 at face value would read the stack pointer. It is hardware
 * register 29 (ULR, "user local"), which is a different register file, and
 * there is no id for it.
 *
 * 19,389 occurrences in the static parity corpus, every one of them `$29`, and
 * they are how glibc finds thread-local storage on MIPS. It was the single
 * largest unmodelled pseudo-assembly call on any of the five architectures.
 */
enum mips_reg_hardware
{
	MIPS_REG_HWR_ULR = MIPS_REG_FD30 + 1,
};

#endif
