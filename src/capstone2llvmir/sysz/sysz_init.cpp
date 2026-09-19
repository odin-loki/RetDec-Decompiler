/**
 * @file src/capstone2llvmir/sysz/sysz_init.cpp
 * @brief Initializations for SystemZ implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "capstone2llvmir/sysz/sysz_impl.h"

namespace retdec {
namespace capstone2llvmir {

void Capstone2LlvmIrTranslatorSysz_impl::initializeArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorSysz_impl::initializeRegNameMap()
{
	// Capstone names GPRs; CC is modelled as an extra 8-bit PSW condition code.
	_reg2name = {
			{SYSZ_REG_CC, "cc"},
	};
}

void Capstone2LlvmIrTranslatorSysz_impl::initializeRegTypeMap()
{
	auto* i8 = llvm::IntegerType::getInt8Ty(_module->getContext());
	auto* i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
	auto* i64 = llvm::IntegerType::getInt64Ty(_module->getContext());
	auto* i128 = llvm::IntegerType::getInt128Ty(_module->getContext());
	auto* f64 = llvm::Type::getDoubleTy(_module->getContext());
	auto* f128 = llvm::Type::getFP128Ty(_module->getContext());

	std::map<uint32_t, llvm::Type*> r2t = {
			{SYSZ_REG_0, i64},
			{SYSZ_REG_1, i64},
			{SYSZ_REG_2, i64},
			{SYSZ_REG_3, i64},
			{SYSZ_REG_4, i64},
			{SYSZ_REG_5, i64},
			{SYSZ_REG_6, i64},
			{SYSZ_REG_7, i64},
			{SYSZ_REG_8, i64},
			{SYSZ_REG_9, i64},
			{SYSZ_REG_10, i64},
			{SYSZ_REG_11, i64},
			{SYSZ_REG_12, i64},
			{SYSZ_REG_13, i64},
			{SYSZ_REG_14, i64},
			{SYSZ_REG_15, i64},
			{SYSZ_REG_CC, i8},
			{SYSZ_REG_R0L, i32},

			{SYSZ_REG_F0, f64},
			{SYSZ_REG_F1, f64},
			{SYSZ_REG_F2, f64},
			{SYSZ_REG_F3, f64},
			{SYSZ_REG_F4, f64},
			{SYSZ_REG_F5, f64},
			{SYSZ_REG_F6, f64},
			{SYSZ_REG_F7, f64},
			{SYSZ_REG_F8, f64},
			{SYSZ_REG_F9, f64},
			{SYSZ_REG_F10, f64},
			{SYSZ_REG_F11, f64},
			{SYSZ_REG_F12, f64},
			{SYSZ_REG_F13, f64},
			{SYSZ_REG_F14, f64},
			{SYSZ_REG_F15, f64},
			{SYSZ_REG_F16, f64},
			{SYSZ_REG_F17, f64},
			{SYSZ_REG_F18, f64},
			{SYSZ_REG_F19, f64},
			{SYSZ_REG_F20, f64},
			{SYSZ_REG_F21, f64},
			{SYSZ_REG_F22, f64},
			{SYSZ_REG_F23, f64},
			{SYSZ_REG_F24, f64},
			{SYSZ_REG_F25, f64},
			{SYSZ_REG_F26, f64},
			{SYSZ_REG_F27, f64},
			{SYSZ_REG_F28, f64},
			{SYSZ_REG_F29, f64},
			{SYSZ_REG_F30, f64},
			{SYSZ_REG_F31, f64},
			{SYSZ_REG_F0Q, f128},
			{SYSZ_REG_F4Q, f128},

			{SYSZ_REG_A0, i32},
			{SYSZ_REG_A1, i32},
			{SYSZ_REG_A2, i32},
			{SYSZ_REG_A3, i32},
			{SYSZ_REG_A4, i32},
			{SYSZ_REG_A5, i32},
			{SYSZ_REG_A6, i32},
			{SYSZ_REG_A7, i32},
			{SYSZ_REG_A8, i32},
			{SYSZ_REG_A9, i32},
			{SYSZ_REG_A10, i32},
			{SYSZ_REG_A11, i32},
			{SYSZ_REG_A12, i32},
			{SYSZ_REG_A13, i32},
			{SYSZ_REG_A14, i32},
			{SYSZ_REG_A15, i32},

			{SYSZ_REG_C0, i32},
			{SYSZ_REG_C1, i32},
			{SYSZ_REG_C2, i32},
			{SYSZ_REG_C3, i32},
			{SYSZ_REG_C4, i32},
			{SYSZ_REG_C5, i32},
			{SYSZ_REG_C6, i32},
			{SYSZ_REG_C7, i32},
			{SYSZ_REG_C8, i32},
			{SYSZ_REG_C9, i32},
			{SYSZ_REG_C10, i32},
			{SYSZ_REG_C11, i32},
			{SYSZ_REG_C12, i32},
			{SYSZ_REG_C13, i32},
			{SYSZ_REG_C14, i32},
			{SYSZ_REG_C15, i32},

			{SYSZ_REG_V0, i128},
			{SYSZ_REG_V1, i128},
			{SYSZ_REG_V2, i128},
			{SYSZ_REG_V3, i128},
			{SYSZ_REG_V4, i128},
			{SYSZ_REG_V5, i128},
			{SYSZ_REG_V6, i128},
			{SYSZ_REG_V7, i128},
			{SYSZ_REG_V8, i128},
			{SYSZ_REG_V9, i128},
			{SYSZ_REG_V10, i128},
			{SYSZ_REG_V11, i128},
			{SYSZ_REG_V12, i128},
			{SYSZ_REG_V13, i128},
			{SYSZ_REG_V14, i128},
			{SYSZ_REG_V15, i128},
			{SYSZ_REG_V16, i128},
			{SYSZ_REG_V17, i128},
			{SYSZ_REG_V18, i128},
			{SYSZ_REG_V19, i128},
			{SYSZ_REG_V20, i128},
			{SYSZ_REG_V21, i128},
			{SYSZ_REG_V22, i128},
			{SYSZ_REG_V23, i128},
			{SYSZ_REG_V24, i128},
			{SYSZ_REG_V25, i128},
			{SYSZ_REG_V26, i128},
			{SYSZ_REG_V27, i128},
			{SYSZ_REG_V28, i128},
			{SYSZ_REG_V29, i128},
			{SYSZ_REG_V30, i128},
			{SYSZ_REG_V31, i128},
	};

	_reg2type = std::move(r2t);
}

void Capstone2LlvmIrTranslatorSysz_impl::initializePseudoCallInstructionIDs()
{
	_callInsnIds = {
			SYSZ_INS_BASR,
			SYSZ_INS_BRAS,
			SYSZ_INS_BRASL,
	};

	_returnInsnIds = {
			// Returns are BR / BCR to r14; classified during translation.
	};

	_branchInsnIds = {
			SYSZ_INS_BR,
			SYSZ_INS_J,
			SYSZ_INS_JG,
	};

	_condBranchInsnIds = {
			SYSZ_INS_BCR,
			SYSZ_INS_BRC,
			SYSZ_INS_BRCL,
			SYSZ_INS_JE,
			SYSZ_INS_JGE,
			SYSZ_INS_JH,
			SYSZ_INS_JGH,
			SYSZ_INS_JL,
			SYSZ_INS_JGL,
			SYSZ_INS_JNE,
			SYSZ_INS_JGNE,
			SYSZ_INS_JO,
			SYSZ_INS_JGO,
			SYSZ_INS_JNO,
			SYSZ_INS_JGNO,
			SYSZ_INS_JHE,
			SYSZ_INS_JGHE,
			SYSZ_INS_JLE,
			SYSZ_INS_JGLE,
			SYSZ_INS_JLH,
			SYSZ_INS_JGLH,
			SYSZ_INS_JNL,
			SYSZ_INS_JGNL,
			SYSZ_INS_JNH,
			SYSZ_INS_JGNH,
			SYSZ_INS_JNLE,
			SYSZ_INS_JGNLE,
			SYSZ_INS_JNHE,
			SYSZ_INS_JGNHE,
			SYSZ_INS_JNLH,
			SYSZ_INS_JGNLH,
			SYSZ_INS_JZ,
			SYSZ_INS_JGZ,
			SYSZ_INS_JNZ,
			SYSZ_INS_JGNZ,
	};

	_controlFlowInsnIds = {
			SYSZ_INS_BR,
			SYSZ_INS_BCR,
			SYSZ_INS_BRC,
			SYSZ_INS_BRCL,
			SYSZ_INS_BASR,
			SYSZ_INS_BRAS,
			SYSZ_INS_BRASL,
			SYSZ_INS_J,
			SYSZ_INS_JG,
	};
}

std::map<std::size_t, void (Capstone2LlvmIrTranslatorSysz_impl::*)(cs_insn* i, cs_sysz*, llvm::IRBuilder<>&)>
	Capstone2LlvmIrTranslatorSysz_impl::_i2fm = {
		{SYSZ_INS_LR, &Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg32},
		{SYSZ_INS_LGR, &Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg64},
		{SYSZ_INS_AR, &Capstone2LlvmIrTranslatorSysz_impl::translateAdd32},
		{SYSZ_INS_AGR, &Capstone2LlvmIrTranslatorSysz_impl::translateAdd64},
		{SYSZ_INS_SR, &Capstone2LlvmIrTranslatorSysz_impl::translateSub32},
		{SYSZ_INS_SGR, &Capstone2LlvmIrTranslatorSysz_impl::translateSub64},
		{SYSZ_INS_NR, &Capstone2LlvmIrTranslatorSysz_impl::translateLogical32},
		{SYSZ_INS_OR, &Capstone2LlvmIrTranslatorSysz_impl::translateLogical32},
		{SYSZ_INS_XR, &Capstone2LlvmIrTranslatorSysz_impl::translateLogical32},
		{SYSZ_INS_L, &Capstone2LlvmIrTranslatorSysz_impl::translateLoad32},
		{SYSZ_INS_LG, &Capstone2LlvmIrTranslatorSysz_impl::translateLoad64},
		{SYSZ_INS_ST, &Capstone2LlvmIrTranslatorSysz_impl::translateStore32},
		{SYSZ_INS_STG, &Capstone2LlvmIrTranslatorSysz_impl::translateStore64},
		{SYSZ_INS_LA, &Capstone2LlvmIrTranslatorSysz_impl::translateLa},
		{SYSZ_INS_BR, &Capstone2LlvmIrTranslatorSysz_impl::translateBr},
		{SYSZ_INS_BCR, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_BRC, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_BRCL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JG, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGNE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JO, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGO, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNO, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGNO, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JHE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGHE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JLE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGLE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JLH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGLH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGNL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGNH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNLE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGNLE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNHE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGNHE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNLH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGNLH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JZ, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGZ, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNZ, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JGNZ, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_BASR, &Capstone2LlvmIrTranslatorSysz_impl::translateBasr},
		{SYSZ_INS_BRAS, &Capstone2LlvmIrTranslatorSysz_impl::translateBrasl},
		{SYSZ_INS_BRASL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrasl},
};

} // namespace capstone2llvmir
} // namespace retdec
