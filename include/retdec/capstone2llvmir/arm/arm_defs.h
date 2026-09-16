/**
 * @file include/retdec/capstone2llvmir/arm/arm_defs.h
 * @brief Additional (on top of Capstone) definitions for ARM translator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_ARM_ARM_DEFS_H
#define RETDEC_CAPSTONE2LLVMIR_ARM_ARM_DEFS_H

#include <capstone/arm.h>

enum arm_reg_cpsr_flags
{
	ARM_REG_CPSR_N = ARM_REG_ENDING + 1,
	ARM_REG_CPSR_Z,
	ARM_REG_CPSR_C,
	ARM_REG_CPSR_V,
};

/**
 * The problem: ARM uses two sets of registers (two enums), ordinary registers
 * (enum arm_reg) and system registers (enum arm_sysreg).
 * Most system registers have enum numbers greater than 256 - these numbers do
 * not overlap with ordinary register numbers.
 * But 8 registers from arm_sysreg overlap with ordinary registers.
 * These are SPSR and CPSR related registers.
 * We cannot use these Capstone enums, since they would collide in our maps with
 * ordinary registers.
 * Moreover, these 8 registers denote flag registers and can be OR combined.
 * Therefore, if we wanted to capture their full semantics, we would either have
 * to create registers for all combinations (e.g. C, X, S, F, CX, CS, CF, XS,
 * XF, SF, CXS, ...) and use appropriate variant depending on asm instruction
 * as Capstone/IDA does (e.g. msr cpsr_fc, r6), or simulate this on a single
 * register using bit setting (i.e. and/or operations).
 * Instead, we ignore work with individual flags - we create only two registers
 * and use them every time any flag is modified.
 */
enum arm_sysreg_extension
{
	ARM_SYSREG_SPSR = ARM_REG_CPSR_V + 1,
	ARM_SYSREG_CPSR,
};

/**
 * The thread pointer.
 *
 * ARM has no register for it in Capstone's enum because it is not a register
 * in the ordinary sense: it is read out of coprocessor 15 with
 * `mrc p15, 0, Rt, c13, c0, 3`, which Capstone reports as six immediate and
 * register operands and nothing else. Every one of the 11,697 `mrc`
 * instructions in the static parity corpus is that exact encoding -- it is how
 * glibc finds thread-local storage on ARM -- and without somewhere to put the
 * value they all became `__asm_mrc(15, 0, 13, 0, 3)`, which no later pass can
 * do anything with.
 *
 * Synthetic register ids past *_REG_ENDING are how this translator already
 * models CPSR flags, ARM's SPSR/CPSR pair and MIPS's double-precision FP
 * pairs; Abi::addRegister() resizes its id table for anything past the end, so
 * there is nothing else to change.
 */
enum arm_reg_thread_pointer
{
	ARM_REG_TPIDRURO = ARM_SYSREG_CPSR + 1,
};

/**
 * The four GE bits of the APSR.
 *
 * They are what the ARMv6 parallel add and subtract instructions produce and
 * the only thing SEL consumes: `uadd8 r0, r1, r2` records, for each of the
 * four byte lanes, whether that lane carried, and `sel r3, r4, r5` then picks
 * byte n from r4 or r5 according to GE[n]. Between them they are how ARM
 * spells a lane-wise select without a NEON register, which is the idiom every
 * hand-written ARMv6 memchr and strlen is built out of.
 *
 * Four i1 registers rather than a nibble of a wider one, for the reason
 * PowerPC's condition register needed: a four-bit register that the branches
 * do not read is a second representation to keep in step with, and the one
 * this translator can act on is the bit.
 *
 * Ids past ARM_REG_ENDING, the same way ARM_REG_CPSR_N and ARM_REG_TPIDRURO
 * are. Abi::addRegister() grows _id2regs to fit.
 */
enum arm_reg_ge_flags
{
	ARM_REG_CPSR_GE0 = ARM_REG_TPIDRURO + 1,
	ARM_REG_CPSR_GE1,
	ARM_REG_CPSR_GE2,
	ARM_REG_CPSR_GE3,
};

#endif
