/**
 * @file src/capstone2llvmir/sparc/sparc_init.cpp
 * @brief Initializations for SPARC implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "capstone2llvmir/sparc/sparc_impl.h"

namespace retdec {
namespace capstone2llvmir {

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorSparc_impl::initializeArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorSparc_impl::initializeRegNameMap()
{
	// Capstone names g0..g7, o0..o5, o7, sp, l0..l7, i0..i5, i7, fp.
}

void Capstone2LlvmIrTranslatorSparc_impl::initializeRegTypeMap()
{
	auto* i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
	auto* i64 = llvm::IntegerType::getInt64Ty(_module->getContext());
	auto* f32 = llvm::Type::getFloatTy(_module->getContext());
	auto* f64 = llvm::Type::getDoubleTy(_module->getContext());
	auto* defTy = isV9() ? i64 : i32;

	std::map<uint32_t, llvm::Type*> r2t = {
		{SPARC_REG_G0, defTy},
		{SPARC_REG_G1, defTy},
		{SPARC_REG_G2, defTy},
		{SPARC_REG_G3, defTy},
		{SPARC_REG_G4, defTy},
		{SPARC_REG_G5, defTy},
		{SPARC_REG_G6, defTy},
		{SPARC_REG_G7, defTy},

		{SPARC_REG_O0, defTy},
		{SPARC_REG_O1, defTy},
		{SPARC_REG_O2, defTy},
		{SPARC_REG_O3, defTy},
		{SPARC_REG_O4, defTy},
		{SPARC_REG_O5, defTy},
		{SPARC_REG_O7, defTy},
		{SPARC_REG_SP, defTy},

		{SPARC_REG_L0, defTy},
		{SPARC_REG_L1, defTy},
		{SPARC_REG_L2, defTy},
		{SPARC_REG_L3, defTy},
		{SPARC_REG_L4, defTy},
		{SPARC_REG_L5, defTy},
		{SPARC_REG_L6, defTy},
		{SPARC_REG_L7, defTy},

		{SPARC_REG_I0, defTy},
		{SPARC_REG_I1, defTy},
		{SPARC_REG_I2, defTy},
		{SPARC_REG_I3, defTy},
		{SPARC_REG_I4, defTy},
		{SPARC_REG_I5, defTy},
		{SPARC_REG_I7, defTy},
		{SPARC_REG_FP, defTy},

		{SPARC_REG_ICC, i32},
		{SPARC_REG_Y, defTy},
		{SPARC_REG_FSR, i32},
		// GSR is ASR 19. VIS ALIGNADDR / FALIGNDATA / FPACK* read the
		// low align (2:0) and scale (6:3) fields.
		{SPARC_REG_ASR19, i64},

		{SPARC_REG_FCC0, i32},
		{SPARC_REG_FCC1, i32},
		{SPARC_REG_FCC2, i32},
		{SPARC_REG_FCC3, i32},

		{SPARC_REG_F0, f32}, {SPARC_REG_F1, f32}, {SPARC_REG_F2, f32}, {SPARC_REG_F3, f32},
		{SPARC_REG_F4, f32}, {SPARC_REG_F5, f32}, {SPARC_REG_F6, f32}, {SPARC_REG_F7, f32},
		{SPARC_REG_F8, f32}, {SPARC_REG_F9, f32}, {SPARC_REG_F10, f32}, {SPARC_REG_F11, f32},
		{SPARC_REG_F12, f32}, {SPARC_REG_F13, f32}, {SPARC_REG_F14, f32}, {SPARC_REG_F15, f32},
		{SPARC_REG_F16, f32}, {SPARC_REG_F17, f32}, {SPARC_REG_F18, f32}, {SPARC_REG_F19, f32},
		{SPARC_REG_F20, f32}, {SPARC_REG_F21, f32}, {SPARC_REG_F22, f32}, {SPARC_REG_F23, f32},
		{SPARC_REG_F24, f32}, {SPARC_REG_F25, f32}, {SPARC_REG_F26, f32}, {SPARC_REG_F27, f32},
		{SPARC_REG_F28, f32}, {SPARC_REG_F29, f32}, {SPARC_REG_F30, f32}, {SPARC_REG_F31, f32},

		{SPARC_REG_D0, f64}, {SPARC_REG_D1, f64}, {SPARC_REG_D2, f64}, {SPARC_REG_D3, f64},
		{SPARC_REG_D4, f64}, {SPARC_REG_D5, f64}, {SPARC_REG_D6, f64}, {SPARC_REG_D7, f64},
		{SPARC_REG_D8, f64}, {SPARC_REG_D9, f64}, {SPARC_REG_D10, f64}, {SPARC_REG_D11, f64},
		{SPARC_REG_D12, f64}, {SPARC_REG_D13, f64}, {SPARC_REG_D14, f64}, {SPARC_REG_D15, f64},
		{SPARC_REG_D16, f64}, {SPARC_REG_D17, f64}, {SPARC_REG_D18, f64}, {SPARC_REG_D19, f64},
		{SPARC_REG_D20, f64}, {SPARC_REG_D21, f64}, {SPARC_REG_D22, f64}, {SPARC_REG_D23, f64},
		{SPARC_REG_D24, f64}, {SPARC_REG_D25, f64}, {SPARC_REG_D26, f64}, {SPARC_REG_D27, f64},
		{SPARC_REG_D28, f64}, {SPARC_REG_D29, f64}, {SPARC_REG_D30, f64}, {SPARC_REG_D31, f64},
	};

	_reg2type = std::move(r2t);
}

void Capstone2LlvmIrTranslatorSparc_impl::initializePseudoCallInstructionIDs()
{
	_callInsnIds =
	{
			SPARC_INS_CALL,
	};

	_returnInsnIds =
	{
			SPARC_INS_RET,
			SPARC_INS_RETL,
			SPARC_INS_RETT,
	};

	_branchInsnIds =
	{
			SPARC_INS_JMPL,
	};

	_condBranchInsnIds =
	{
			SPARC_INS_B,
			SPARC_INS_FB,
			SPARC_INS_BR,
			SPARC_INS_BRGEZ,
			SPARC_INS_BRGZ,
			SPARC_INS_BRLEZ,
			SPARC_INS_BRLZ,
			SPARC_INS_BRNZ,
			SPARC_INS_BRZ,
	};

	_controlFlowInsnIds =
	{
			// Categorized by the id sets above.
	};
}

//
//==============================================================================
// Instruction translation map initialization.
//==============================================================================
//

std::map<std::size_t, void (Capstone2LlvmIrTranslatorSparc_impl::*)(cs_insn* i, cs_sparc*, llvm::IRBuilder<>&)>
	Capstone2LlvmIrTranslatorSparc_impl::_i2fm = {
		{SPARC_INS_ADD, &Capstone2LlvmIrTranslatorSparc_impl::translateAdd},
		{SPARC_INS_ADDCC, &Capstone2LlvmIrTranslatorSparc_impl::translateAdd},
		{SPARC_INS_ADDX, &Capstone2LlvmIrTranslatorSparc_impl::translateAdd},
		{SPARC_INS_ADDXCC, &Capstone2LlvmIrTranslatorSparc_impl::translateAdd},
		{SPARC_INS_ADDXC, &Capstone2LlvmIrTranslatorSparc_impl::translateAdd},
		{SPARC_INS_ADDXCCC, &Capstone2LlvmIrTranslatorSparc_impl::translateAdd},

		{SPARC_INS_ALIGNADDR, &Capstone2LlvmIrTranslatorSparc_impl::translateVisAlign},
		{SPARC_INS_ALIGNADDRL, &Capstone2LlvmIrTranslatorSparc_impl::translateVisAlign},
		{SPARC_INS_ARRAY8, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_ARRAY16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_ARRAY32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},

		{SPARC_INS_AND, &Capstone2LlvmIrTranslatorSparc_impl::translateAnd},
		{SPARC_INS_ANDCC, &Capstone2LlvmIrTranslatorSparc_impl::translateAnd},
		{SPARC_INS_ANDN, &Capstone2LlvmIrTranslatorSparc_impl::translateAnd},
		{SPARC_INS_ANDNCC, &Capstone2LlvmIrTranslatorSparc_impl::translateAnd},

		{SPARC_INS_B, &Capstone2LlvmIrTranslatorSparc_impl::translateB},
		{SPARC_INS_BSHUFFLE, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_BR, &Capstone2LlvmIrTranslatorSparc_impl::translateBr},
		{SPARC_INS_BRGEZ, &Capstone2LlvmIrTranslatorSparc_impl::translateBr},
		{SPARC_INS_BRGZ, &Capstone2LlvmIrTranslatorSparc_impl::translateBr},
		{SPARC_INS_BRLEZ, &Capstone2LlvmIrTranslatorSparc_impl::translateBr},
		{SPARC_INS_BRLZ, &Capstone2LlvmIrTranslatorSparc_impl::translateBr},
		{SPARC_INS_BRNZ, &Capstone2LlvmIrTranslatorSparc_impl::translateBr},
		{SPARC_INS_BRZ, &Capstone2LlvmIrTranslatorSparc_impl::translateBr},

		{SPARC_INS_CALL, &Capstone2LlvmIrTranslatorSparc_impl::translateCall},
		{SPARC_INS_CASA, &Capstone2LlvmIrTranslatorSparc_impl::translateCas},
		{SPARC_INS_CASXA, &Capstone2LlvmIrTranslatorSparc_impl::translateCas},
		{SPARC_INS_CMP, &Capstone2LlvmIrTranslatorSparc_impl::translateCmp},

		{SPARC_INS_EDGE8, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE8L, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE8LN, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE8N, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE16L, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE16LN, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE16N, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE32L, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE32LN, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_EDGE32N, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},

		{SPARC_INS_FABSD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FABSS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FADDD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FADDS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FALIGNDATA, &Capstone2LlvmIrTranslatorSparc_impl::translateVisAlign},
		{SPARC_INS_FAND, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FANDNOT1, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FANDNOT1S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FANDNOT2, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FANDNOT2S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FANDS, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FB, &Capstone2LlvmIrTranslatorSparc_impl::translateB},
		{SPARC_INS_FCMPD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpCmp},
		{SPARC_INS_FCMPED, &Capstone2LlvmIrTranslatorSparc_impl::translateFpCmp},
		{SPARC_INS_FCMPEQ16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FCMPEQ32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FCMPES, &Capstone2LlvmIrTranslatorSparc_impl::translateFpCmp},
		{SPARC_INS_FCMPGT16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FCMPGT32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FCMPLE16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FCMPLE32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FCMPNE16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FCMPNE32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FCMPS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpCmp},
		{SPARC_INS_FDIVD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FDIVS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FDTOI, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FDTOS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FDTOX, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FCHKSM16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FEXPAND, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FITOD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FITOS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FMEAN16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FMOVD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FMOVRD, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_FMOVRS, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_FMOVS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FMUL8SUX16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FMUL8ULX16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FMUL8X16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FMUL8X16AL, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FMUL8X16AU, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FMULD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FMULD8SUX16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FMULD8ULX16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FMULS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FNEGD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FNEGS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FNOR, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FNORS, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FNOT1, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FNOT1S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FNOT2, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FNOT2S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FONE, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FONES, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FOR, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FORNOT1, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FORNOT1S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FORNOT2, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FORNOT2S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FORS, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FPACK16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPACK32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPACKFIX, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPADD16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPADD16S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPADD32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPADD32S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPADD64, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPMERGE, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPSUB16, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPSUB16S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPSUB32, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FPSUB32S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_FSRC1, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FSRC1S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FSRC2, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FSRC2S, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FSQRTD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FSQRTS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FSMULD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FSTOD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FSTOI, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FSTOX, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FSUBD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FSUBS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpArith},
		{SPARC_INS_FXNOR, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FXNORS, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FXOR, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FXORS, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FXTOD, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FXTOS, &Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary},
		{SPARC_INS_FZERO, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},
		{SPARC_INS_FZEROS, &Capstone2LlvmIrTranslatorSparc_impl::translateVisLogical},

		{SPARC_INS_FLUSH, &Capstone2LlvmIrTranslatorSparc_impl::translateNop},

		{SPARC_INS_JMPL, &Capstone2LlvmIrTranslatorSparc_impl::translateJmpl},

		{SPARC_INS_LD, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDA, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDD, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDDA, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDSB, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDSH, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDSW, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDUB, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDUH, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDX, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDSTUB, &Capstone2LlvmIrTranslatorSparc_impl::translateAtomicXchg},
		{SPARC_INS_LDSTUBA, &Capstone2LlvmIrTranslatorSparc_impl::translateAtomicXchg},

		{SPARC_INS_MEMBAR, &Capstone2LlvmIrTranslatorSparc_impl::translateFence},
		{SPARC_INS_MOV, &Capstone2LlvmIrTranslatorSparc_impl::translateMov},
		{SPARC_INS_MOVR, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_ALIAS_MOVRZ, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_ALIAS_MOVRLEZ, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_ALIAS_MOVRLZ, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_ALIAS_MOVRNZ, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_ALIAS_MOVRGZ, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_ALIAS_MOVRGEZ, &Capstone2LlvmIrTranslatorSparc_impl::translateMovr},
		{SPARC_INS_MOVDTOX, &Capstone2LlvmIrTranslatorSparc_impl::translateMovFpInt},
		{SPARC_INS_MOVSTOSW, &Capstone2LlvmIrTranslatorSparc_impl::translateMovFpInt},
		{SPARC_INS_MOVSTOUW, &Capstone2LlvmIrTranslatorSparc_impl::translateMovFpInt},
		{SPARC_INS_MOVWTOS, &Capstone2LlvmIrTranslatorSparc_impl::translateMovFpInt},
		{SPARC_INS_MOVXTOD, &Capstone2LlvmIrTranslatorSparc_impl::translateMovFpInt},
		{SPARC_INS_MULX, &Capstone2LlvmIrTranslatorSparc_impl::translateMul},
		{SPARC_INS_NOP, &Capstone2LlvmIrTranslatorSparc_impl::translateNop},

		{SPARC_INS_PDIST, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_PDISTN, &Capstone2LlvmIrTranslatorSparc_impl::translateVisPacked},
		{SPARC_INS_POPC, &Capstone2LlvmIrTranslatorSparc_impl::translatePopc},

		{SPARC_INS_OR, &Capstone2LlvmIrTranslatorSparc_impl::translateOr},
		{SPARC_INS_ORCC, &Capstone2LlvmIrTranslatorSparc_impl::translateOr},
		{SPARC_INS_ORN, &Capstone2LlvmIrTranslatorSparc_impl::translateOr},
		{SPARC_INS_ORNCC, &Capstone2LlvmIrTranslatorSparc_impl::translateOr},

		{SPARC_INS_RD, &Capstone2LlvmIrTranslatorSparc_impl::translateRd},
		{SPARC_INS_RESTORE, &Capstone2LlvmIrTranslatorSparc_impl::translateRestore},
		{SPARC_INS_RET, &Capstone2LlvmIrTranslatorSparc_impl::translateRet},
		{SPARC_INS_RETL, &Capstone2LlvmIrTranslatorSparc_impl::translateRet},
		{SPARC_INS_RETT, &Capstone2LlvmIrTranslatorSparc_impl::translateRett},
		{SPARC_INS_SAVE, &Capstone2LlvmIrTranslatorSparc_impl::translateSave},
		{SPARC_INS_SDIV, &Capstone2LlvmIrTranslatorSparc_impl::translateDiv},
		{SPARC_INS_SDIVCC, &Capstone2LlvmIrTranslatorSparc_impl::translateDiv},
		{SPARC_INS_SDIVX, &Capstone2LlvmIrTranslatorSparc_impl::translateDiv},
		{SPARC_INS_SETHI, &Capstone2LlvmIrTranslatorSparc_impl::translateSethi},

		{SPARC_INS_SLL, &Capstone2LlvmIrTranslatorSparc_impl::translateShift},
		{SPARC_INS_SLLX, &Capstone2LlvmIrTranslatorSparc_impl::translateShift},
		{SPARC_INS_SMUL, &Capstone2LlvmIrTranslatorSparc_impl::translateMul},
		{SPARC_INS_SMULCC, &Capstone2LlvmIrTranslatorSparc_impl::translateMul},
		{SPARC_INS_SRA, &Capstone2LlvmIrTranslatorSparc_impl::translateShift},
		{SPARC_INS_SRAX, &Capstone2LlvmIrTranslatorSparc_impl::translateShift},
		{SPARC_INS_SRL, &Capstone2LlvmIrTranslatorSparc_impl::translateShift},
		{SPARC_INS_SRLX, &Capstone2LlvmIrTranslatorSparc_impl::translateShift},

		{SPARC_INS_ST, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STA, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STB, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STBAR, &Capstone2LlvmIrTranslatorSparc_impl::translateFence},
		{SPARC_INS_STD, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STDA, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STH, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STX, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},

		{SPARC_INS_SUB, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},
		{SPARC_INS_SUBCC, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},
		{SPARC_INS_SUBX, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},
		{SPARC_INS_SUBXCC, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},
		{SPARC_INS_SWAP, &Capstone2LlvmIrTranslatorSparc_impl::translateAtomicXchg},
		{SPARC_INS_SWAPA, &Capstone2LlvmIrTranslatorSparc_impl::translateAtomicXchg},

		{SPARC_INS_TA, &Capstone2LlvmIrTranslatorSparc_impl::translateTrap},
		{SPARC_INS_TADDCC, &Capstone2LlvmIrTranslatorSparc_impl::translateAdd},
		{SPARC_INS_TADDCCTV, &Capstone2LlvmIrTranslatorSparc_impl::translateAdd},
		{SPARC_INS_T, &Capstone2LlvmIrTranslatorSparc_impl::translateTrap},
		{SPARC_INS_TSUBCC, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},
		{SPARC_INS_TSUBCCTV, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},

		{SPARC_INS_UDIV, &Capstone2LlvmIrTranslatorSparc_impl::translateDiv},
		{SPARC_INS_UDIVCC, &Capstone2LlvmIrTranslatorSparc_impl::translateDiv},
		{SPARC_INS_UDIVX, &Capstone2LlvmIrTranslatorSparc_impl::translateDiv},
		{SPARC_INS_UMUL, &Capstone2LlvmIrTranslatorSparc_impl::translateMul},
		{SPARC_INS_UMULCC, &Capstone2LlvmIrTranslatorSparc_impl::translateMul},
		{SPARC_INS_UMULXHI, &Capstone2LlvmIrTranslatorSparc_impl::translateMul},

		{SPARC_INS_WR, &Capstone2LlvmIrTranslatorSparc_impl::translateWr},

		{SPARC_INS_XNOR, &Capstone2LlvmIrTranslatorSparc_impl::translateXor},
		{SPARC_INS_XNORCC, &Capstone2LlvmIrTranslatorSparc_impl::translateXor},
		{SPARC_INS_XOR, &Capstone2LlvmIrTranslatorSparc_impl::translateXor},
		{SPARC_INS_XORCC, &Capstone2LlvmIrTranslatorSparc_impl::translateXor},
};

} // namespace capstone2llvmir
} // namespace retdec
