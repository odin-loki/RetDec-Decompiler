/**
 * @file include/retdec/capstone2llvmir/x86/x86_defs.h
 * @brief Additional (on top of Capstone) definitions for x86 translator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_X86_X86_DEFS_H
#define RETDEC_CAPSTONE2LLVMIR_X86_X86_DEFS_H

/**
 * A flag register addition to @c x86_reg from capstone/x86.h.
 * Translator works with flag registers explicitly, but they are modeled only
 * as @c X86_REG_EFLAGS in the original @c x86_reg enum.
 * This is intentionally not a strongly typed enum to keep it consistent
 * with @c x86_reg enum.
 */
enum x86_reg_rflags
{
// FLAGS
	X86_REG_CF = X86_REG_ENDING + 1, // 0
	// reserved 1
	X86_REG_PF, // 2
	// reserved 3
	X86_REG_AF, // 4
	// reserved 5
	X86_REG_ZF, // 6
	X86_REG_SF, // 7
	X86_REG_TF, // 8
	X86_REG_IF, // 9
	X86_REG_DF, // 10
	X86_REG_OF, // 11
	X86_REG_IOPL, // 12-13
	X86_REG_NT, // 14
	// reserved 15
// EFLAGS
	X86_REG_RF, // 16
	X86_REG_VM, // 17
	X86_REG_AC, // 18
	X86_REG_VIF, // 19
	X86_REG_VIP, // 20
	X86_REG_ID // 21
	// reserved 22-31
// RFLAGS
	// reserved 32-63
};

/**
 * An FPU status register addition to @c x86_reg from capstone/x86.h.
 * Translator works with status registers explicitly, but they are modeled only
 * as @c X86_REG_FPSW in the original @c x86_reg enum.
 * This is intentionally not a strongly typed enum to keep it consistent
 * with @c x86_reg enum.
 */
enum x87_reg_status
{
// Exception flags
// One or more fp exceptions have been detected since the bits were last
// cleared.
	X87_REG_IE = X86_REG_ID + 1,  //  0 -- invalid operation
	X87_REG_DE,  //  1 -- denormalized operand
	X87_REG_ZE,  //  2 -- zero divide
	X87_REG_OE,  //  3 -- overflow
	X87_REG_UE,  //  4 -- underflow
	X87_REG_PE,  //  5 -- precision
// Stack fault
// Indicates that stack overflow or underflow has occurred with x87 FPU data
// register stack.
	X87_REG_SF,  //  6
// Error summanry status
	X87_REG_ES, //  7
// Condition code
// Indicate the results of fp comparison and arithmetic operations.
	X87_REG_C0,  //  8
	X87_REG_C1,  //  9
	X87_REG_C2,  // 10
	X87_REG_C3,  // 14
// Top of stack pointer
	X87_REG_TOP, // 11-13
// FPU busy
	X87_REG_B    // 15
};

/**
 * An FPU control register addition to @c x86_reg from capstone/x86.h.
 * Translator works with control registers explicitly, but it looks like they
 * are not modeled in the original @c x86_reg enum.
 * This is intentionally not a strongly typed enum to keep it consistent
 * with @c x86_reg enum.
 */
enum x87_reg_control
{
// Exception Masks
	X87_REG_IM = X87_REG_B + 1, // 0 -- invalid operation
	X87_REG_DM, // 1 -- denormal operand
	X87_REG_ZM, // 2 -- zero divide
	X87_REG_OM, // 3 -- overflow
	X87_REG_UM, // 4 -- underflow
	X87_REG_PM, // 5 -- precision
	// reserved 6-7
// Precision control
	X87_REG_PC, // 8-9
// Rounding Control
	X87_REG_RC, // 10-11
// Infiinity control
	X87_REG_X, // 12
	// reserved 13-15
};

/**
 * The upper halves of the sixteen YMM registers.
 *
 * YMM_n IS XMM_n extended: bits 127..0 are the XMM register and bits 255..128
 * are an extension that only AVX can name. x86_init.cpp creates XMM, YMM and
 * ZMM as three INDEPENDENT globals -- nothing maps one to another -- so an SSE
 * write to xmm0 and an AVX read of ymm0 do not see each other, and vzeroupper
 * has nothing to zero.
 *
 * The obvious repair is to make XMM a sub-register of ZMM through the parent
 * map. The previous commit removed what blocked that (a hard-coded mask table
 * that threw for any pair of widths outside the general-purpose registers),
 * and it is still the wrong move here: only PARENT registers get globals in
 * this register file, so aliasing XMM would stop getRegister(X86_REG_XMM0)
 * resolving and every one of the 159 XMM references in the x86 test suite
 * would have to be rewritten against ZMM, through accessors that do not assert
 * past 64 bits.
 *
 * This is the decomposition the hardware manual uses instead: YMM = YMMH:XMM.
 * XMM keeps its global and every SSE translator and test is untouched; the
 * upper half is its own register; and the three rules that make AVX and SSE
 * coexist become three things you can write down and test.
 *
 *   a legacy SSE write        leaves YMMH alone
 *   a VEX-encoded 128-bit write   zeroes YMMH
 *   vzeroupper                zeroes every YMMH
 *
 * Ids past the flag registers, the same way X86_REG_CF and X87_REG_IE are.
 */
enum x86_reg_ymm_high
{
	X86_REG_YMM0_HI = X87_REG_X + 1,
	X86_REG_YMM1_HI,
	X86_REG_YMM2_HI,
	X86_REG_YMM3_HI,
	X86_REG_YMM4_HI,
	X86_REG_YMM5_HI,
	X86_REG_YMM6_HI,
	X86_REG_YMM7_HI,
	X86_REG_YMM8_HI,
	X86_REG_YMM9_HI,
	X86_REG_YMM10_HI,
	X86_REG_YMM11_HI,
	X86_REG_YMM12_HI,
	X86_REG_YMM13_HI,
	X86_REG_YMM14_HI,
	X86_REG_YMM15_HI,
	X86_REG_YMM16_HI,
	X86_REG_YMM17_HI,
	X86_REG_YMM18_HI,
	X86_REG_YMM19_HI,
	X86_REG_YMM20_HI,
	X86_REG_YMM21_HI,
	X86_REG_YMM22_HI,
	X86_REG_YMM23_HI,
	X86_REG_YMM24_HI,
	X86_REG_YMM25_HI,
	X86_REG_YMM26_HI,
	X86_REG_YMM27_HI,
	X86_REG_YMM28_HI,
	X86_REG_YMM29_HI,
	X86_REG_YMM30_HI,
	X86_REG_YMM31_HI,
};

/**
 * The top 256 bits of each ZMM register. ZMM = ZMMH:YMMH:XMM, one level up
 * from the YMM decomposition above and for the same reason: the low 128 bits
 * of zmm3 and xmm3 are the same bits, so they have to be the same global.
 *
 * All 32 exist. Only EVEX can name registers 16..31, so those have no legacy
 * SSE alias to worry about -- but they are decomposed the same way anyway,
 * because a uniform rule is one rule.
 */
enum x86_reg_zmm_high
{
	X86_REG_ZMM0_HI = X86_REG_YMM31_HI + 1,
	X86_REG_ZMM1_HI,
	X86_REG_ZMM2_HI,
	X86_REG_ZMM3_HI,
	X86_REG_ZMM4_HI,
	X86_REG_ZMM5_HI,
	X86_REG_ZMM6_HI,
	X86_REG_ZMM7_HI,
	X86_REG_ZMM8_HI,
	X86_REG_ZMM9_HI,
	X86_REG_ZMM10_HI,
	X86_REG_ZMM11_HI,
	X86_REG_ZMM12_HI,
	X86_REG_ZMM13_HI,
	X86_REG_ZMM14_HI,
	X86_REG_ZMM15_HI,
	X86_REG_ZMM16_HI,
	X86_REG_ZMM17_HI,
	X86_REG_ZMM18_HI,
	X86_REG_ZMM19_HI,
	X86_REG_ZMM20_HI,
	X86_REG_ZMM21_HI,
	X86_REG_ZMM22_HI,
	X86_REG_ZMM23_HI,
	X86_REG_ZMM24_HI,
	X86_REG_ZMM25_HI,
	X86_REG_ZMM26_HI,
	X86_REG_ZMM27_HI,
	X86_REG_ZMM28_HI,
	X86_REG_ZMM29_HI,
	X86_REG_ZMM30_HI,
	X86_REG_ZMM31_HI,
};

/**
 * Representation of x86 address spaces.
 *
 * Based on values in X86ISelDAGToDag.cpp.
 */
enum class x86_addr_space
{
	DEFAULT = 0,
	GS = 256,
	FS = 257,
	SS = 258
};

#endif
