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
	auto* defTy = isV9()
			? llvm::IntegerType::getInt64Ty(_module->getContext())
			: i32;

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
		{SPARC_REG_XCC, i32},
		{SPARC_REG_Y, defTy},

		{SPARC_REG_FCC0, i32},
		{SPARC_REG_FCC1, i32},
		{SPARC_REG_FCC2, i32},
		{SPARC_REG_FCC3, i32},
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
			SPARC_INS_JMP,
			SPARC_INS_JMPL,
	};

	_condBranchInsnIds =
	{
			SPARC_INS_B,
			SPARC_INS_FB,
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

		{SPARC_INS_AND, &Capstone2LlvmIrTranslatorSparc_impl::translateAnd},
		{SPARC_INS_ANDCC, &Capstone2LlvmIrTranslatorSparc_impl::translateAnd},

		{SPARC_INS_B, &Capstone2LlvmIrTranslatorSparc_impl::translateB},

		{SPARC_INS_CALL, &Capstone2LlvmIrTranslatorSparc_impl::translateCall},
		{SPARC_INS_CMP, &Capstone2LlvmIrTranslatorSparc_impl::translateCmp},

		{SPARC_INS_JMP, &Capstone2LlvmIrTranslatorSparc_impl::translateJmpl},
		{SPARC_INS_JMPL, &Capstone2LlvmIrTranslatorSparc_impl::translateJmpl},

		{SPARC_INS_LD, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDSB, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDSH, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDSW, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDUB, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDUH, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},
		{SPARC_INS_LDX, &Capstone2LlvmIrTranslatorSparc_impl::translateLoad},

		{SPARC_INS_MOV, &Capstone2LlvmIrTranslatorSparc_impl::translateMov},
		{SPARC_INS_NOP, &Capstone2LlvmIrTranslatorSparc_impl::translateNop},

		{SPARC_INS_OR, &Capstone2LlvmIrTranslatorSparc_impl::translateOr},
		{SPARC_INS_ORCC, &Capstone2LlvmIrTranslatorSparc_impl::translateOr},

		{SPARC_INS_RESTORE, &Capstone2LlvmIrTranslatorSparc_impl::translateRestore},
		{SPARC_INS_RET, &Capstone2LlvmIrTranslatorSparc_impl::translateRet},
		{SPARC_INS_RETL, &Capstone2LlvmIrTranslatorSparc_impl::translateRet},
		{SPARC_INS_RETT, &Capstone2LlvmIrTranslatorSparc_impl::translateRett},
		{SPARC_INS_SAVE, &Capstone2LlvmIrTranslatorSparc_impl::translateSave},
		{SPARC_INS_SETHI, &Capstone2LlvmIrTranslatorSparc_impl::translateSethi},

		{SPARC_INS_ST, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STB, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STH, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},
		{SPARC_INS_STX, &Capstone2LlvmIrTranslatorSparc_impl::translateStore},

		{SPARC_INS_SUB, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},
		{SPARC_INS_SUBCC, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},
		{SPARC_INS_SUBX, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},
		{SPARC_INS_SUBXCC, &Capstone2LlvmIrTranslatorSparc_impl::translateSub},

		{SPARC_INS_XOR, &Capstone2LlvmIrTranslatorSparc_impl::translateXor},
		{SPARC_INS_XORCC, &Capstone2LlvmIrTranslatorSparc_impl::translateXor},
};

} // namespace capstone2llvmir
} // namespace retdec
