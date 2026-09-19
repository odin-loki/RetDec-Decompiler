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

	auto* f32 = llvm::Type::getFloatTy(_module->getContext());

	std::map<uint32_t, llvm::Type*> r2t = {
			{SYSZ_REG_CC, i8},

			{SYSZ_REG_F0Q, f128},
			{SYSZ_REG_F1Q, f128},
			{SYSZ_REG_F4Q, f128},
			{SYSZ_REG_F5Q, f128},
			{SYSZ_REG_F8Q, f128},
			{SYSZ_REG_F9Q, f128},
			{SYSZ_REG_F12Q, f128},
			{SYSZ_REG_F13Q, f128},

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

	// Capstone 6 names 64-bit GPRs R0D..R15D (compat: SYSZ_REG_0 == R0D).
	// R*L / R*H are aliased onto these in generateRegisters().
	for (uint32_t r = SYSZ_REG_R0D; r <= SYSZ_REG_R15D; ++r)
	{
		r2t[r] = i64;
	}
	for (uint32_t r = SYSZ_REG_F0D; r <= SYSZ_REG_F31D; ++r)
	{
		r2t[r] = f64;
	}
	for (uint32_t r = SYSZ_REG_F0S; r <= SYSZ_REG_F31S; ++r)
	{
		r2t[r] = f32;
	}

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
			SYSZ_INS_J_G_LU_,
	};

	_condBranchInsnIds = {
			SYSZ_INS_BCR,
			SYSZ_INS_BRC,
			SYSZ_INS_BRCL,
			SYSZ_INS_JE,
			SYSZ_INS_J_G_L_E,
			SYSZ_INS_JH,
			SYSZ_INS_J_G_L_H,
			SYSZ_INS_JL,
			SYSZ_INS_J_G_L_L,
			SYSZ_INS_JNE,
			SYSZ_INS_J_G_L_NE,
			SYSZ_INS_JO,
			SYSZ_INS_J_G_L_O,
			SYSZ_INS_JNO,
			SYSZ_INS_J_G_L_NO,
			SYSZ_INS_JHE,
			SYSZ_INS_J_G_L_HE,
			SYSZ_INS_JLE,
			SYSZ_INS_J_G_L_LE,
			SYSZ_INS_JLH,
			SYSZ_INS_J_G_L_LH,
			SYSZ_INS_JNL,
			SYSZ_INS_J_G_L_NL,
			SYSZ_INS_JNH,
			SYSZ_INS_J_G_L_NH,
			SYSZ_INS_JNLE,
			SYSZ_INS_J_G_L_NLE,
			SYSZ_INS_JNHE,
			SYSZ_INS_J_G_L_NHE,
			SYSZ_INS_JNLH,
			SYSZ_INS_J_G_L_NLH,
			SYSZ_INS_JZ,
			SYSZ_INS_J_G_L_Z,
			SYSZ_INS_JNZ,
			SYSZ_INS_J_G_L_NZ,
			SYSZ_INS_JM,
			SYSZ_INS_J_G_L_M,
			SYSZ_INS_JP,
			SYSZ_INS_J_G_L_P,
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
			SYSZ_INS_J_G_LU_,
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
		{SYSZ_INS_J_G_LU_, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_E, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_H, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_L, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_NE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JO, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_O, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNO, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_NO, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JHE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_HE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JLE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_LE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JLH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_LH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_NL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_NH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNLE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_NLE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNHE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_NHE, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNLH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_NLH, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JZ, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_Z, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JNZ, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_NZ, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JM, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_M, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_JP, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_J_G_L_P, &Capstone2LlvmIrTranslatorSysz_impl::translateBrc},
		{SYSZ_INS_BASR, &Capstone2LlvmIrTranslatorSysz_impl::translateBasr},
		{SYSZ_INS_BRAS, &Capstone2LlvmIrTranslatorSysz_impl::translateBrasl},
		{SYSZ_INS_BRASL, &Capstone2LlvmIrTranslatorSysz_impl::translateBrasl},

		{SYSZ_INS_NGR, &Capstone2LlvmIrTranslatorSysz_impl::translateLogical64},
		{SYSZ_INS_OGR, &Capstone2LlvmIrTranslatorSysz_impl::translateLogical64},
		{SYSZ_INS_XGR, &Capstone2LlvmIrTranslatorSysz_impl::translateLogical64},
		{SYSZ_INS_LGHI, &Capstone2LlvmIrTranslatorSysz_impl::translateImm64},
		{SYSZ_INS_LHI, &Capstone2LlvmIrTranslatorSysz_impl::translateImm64},
		{SYSZ_INS_AGHI, &Capstone2LlvmIrTranslatorSysz_impl::translateAddImm},
		{SYSZ_INS_AHI, &Capstone2LlvmIrTranslatorSysz_impl::translateAddImm},
		{SYSZ_INS_CR, &Capstone2LlvmIrTranslatorSysz_impl::translateCompare},
		{SYSZ_INS_CGR, &Capstone2LlvmIrTranslatorSysz_impl::translateCompare},
		{SYSZ_INS_CHI, &Capstone2LlvmIrTranslatorSysz_impl::translateCompare},
		{SYSZ_INS_CGHI, &Capstone2LlvmIrTranslatorSysz_impl::translateCompare},
		{SYSZ_INS_LGFR, &Capstone2LlvmIrTranslatorSysz_impl::translateExtend32},
		{SYSZ_INS_LLGFR, &Capstone2LlvmIrTranslatorSysz_impl::translateExtend32},
		{SYSZ_INS_LGF, &Capstone2LlvmIrTranslatorSysz_impl::translateExtend32},
		{SYSZ_INS_LLGF, &Capstone2LlvmIrTranslatorSysz_impl::translateExtend32},
		{SYSZ_INS_SLLG, &Capstone2LlvmIrTranslatorSysz_impl::translateShift64},
		{SYSZ_INS_SRLG, &Capstone2LlvmIrTranslatorSysz_impl::translateShift64},
		{SYSZ_INS_SRAG, &Capstone2LlvmIrTranslatorSysz_impl::translateShift64},
		{SYSZ_INS_LAY, &Capstone2LlvmIrTranslatorSysz_impl::translateLay},
		{SYSZ_INS_LARL, &Capstone2LlvmIrTranslatorSysz_impl::translateLay},
		{SYSZ_INS_LTGR, &Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg64},
		{SYSZ_INS_LTR, &Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg32},

		{SYSZ_INS_AEBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_AEB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_ADBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_ADB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_SEBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_SEB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_SDBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_SDB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_MEEBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_MEEB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_MDBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_MDB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_DEBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_DEB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_DDBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_DDB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpArith},
		{SYSZ_INS_CEBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpCompare},
		{SYSZ_INS_CEB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpCompare},
		{SYSZ_INS_CDBR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpCompare},
		{SYSZ_INS_CDB, &Capstone2LlvmIrTranslatorSysz_impl::translateFpCompare},
		{SYSZ_INS_LE, &Capstone2LlvmIrTranslatorSysz_impl::translateFpLoad},
		{SYSZ_INS_LEY, &Capstone2LlvmIrTranslatorSysz_impl::translateFpLoad},
		{SYSZ_INS_LD, &Capstone2LlvmIrTranslatorSysz_impl::translateFpLoad},
		{SYSZ_INS_LDY, &Capstone2LlvmIrTranslatorSysz_impl::translateFpLoad},
		{SYSZ_INS_STE, &Capstone2LlvmIrTranslatorSysz_impl::translateFpStore},
		{SYSZ_INS_STEY, &Capstone2LlvmIrTranslatorSysz_impl::translateFpStore},
		{SYSZ_INS_STD, &Capstone2LlvmIrTranslatorSysz_impl::translateFpStore},
		{SYSZ_INS_STDY, &Capstone2LlvmIrTranslatorSysz_impl::translateFpStore},
		{SYSZ_INS_LER, &Capstone2LlvmIrTranslatorSysz_impl::translateFpMove},
		{SYSZ_INS_LDR, &Capstone2LlvmIrTranslatorSysz_impl::translateFpMove},
		{SYSZ_INS_LDEB, &Capstone2LlvmIrTranslatorSysz_impl::translateLdeb},
		{SYSZ_INS_LDEBR, &Capstone2LlvmIrTranslatorSysz_impl::translateLdeb},
		{SYSZ_INS_LEDBR, &Capstone2LlvmIrTranslatorSysz_impl::translateLedbr},

		{SYSZ_INS_VL, &Capstone2LlvmIrTranslatorSysz_impl::translateVectorLoad},
		{SYSZ_INS_VST, &Capstone2LlvmIrTranslatorSysz_impl::translateVectorStore},
		{SYSZ_INS_VLR, &Capstone2LlvmIrTranslatorSysz_impl::translateVlr},
		{SYSZ_INS_VLREP, &Capstone2LlvmIrTranslatorSysz_impl::translateVlrep},
		{SYSZ_INS_VLREPB, &Capstone2LlvmIrTranslatorSysz_impl::translateVlrep},
		{SYSZ_INS_VLREPH, &Capstone2LlvmIrTranslatorSysz_impl::translateVlrep},
		{SYSZ_INS_VLREPF, &Capstone2LlvmIrTranslatorSysz_impl::translateVlrep},
		{SYSZ_INS_VLREPG, &Capstone2LlvmIrTranslatorSysz_impl::translateVlrep},
		{SYSZ_INS_VLEG, &Capstone2LlvmIrTranslatorSysz_impl::translateVleg},
		{SYSZ_INS_VLEF, &Capstone2LlvmIrTranslatorSysz_impl::translateVleg},
		{SYSZ_INS_VSTEG, &Capstone2LlvmIrTranslatorSysz_impl::translateVsteg},
		{SYSZ_INS_VSTEF, &Capstone2LlvmIrTranslatorSysz_impl::translateVsteg},
};

} // namespace capstone2llvmir
} // namespace retdec
