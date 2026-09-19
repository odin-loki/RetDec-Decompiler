/**
 * @file src/capstone2llvmir/xcore/xcore_init.cpp
 * @brief Initializations for XCore implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "capstone2llvmir/xcore/xcore_impl.h"

namespace retdec {
namespace capstone2llvmir {

void Capstone2LlvmIrTranslatorXcore_impl::initializeArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorXcore_impl::initializeRegNameMap()
{
	// Capstone already names the XS1 register file.
}

void Capstone2LlvmIrTranslatorXcore_impl::initializeRegTypeMap()
{
	auto* i32 = llvm::IntegerType::getInt32Ty(_module->getContext());

	std::map<uint32_t, llvm::Type*> r2t = {
			{XCORE_REG_CP, i32},
			{XCORE_REG_DP, i32},
			{XCORE_REG_LR, i32},
			{XCORE_REG_SP, i32},
			{XCORE_REG_R0, i32},
			{XCORE_REG_R1, i32},
			{XCORE_REG_R2, i32},
			{XCORE_REG_R3, i32},
			{XCORE_REG_R4, i32},
			{XCORE_REG_R5, i32},
			{XCORE_REG_R6, i32},
			{XCORE_REG_R7, i32},
			{XCORE_REG_R8, i32},
			{XCORE_REG_R9, i32},
			{XCORE_REG_R10, i32},
			{XCORE_REG_R11, i32},
			{XCORE_REG_PC, i32},
			{XCORE_REG_SCP, i32},
			{XCORE_REG_SSR, i32},
			{XCORE_REG_ET, i32},
			{XCORE_REG_ED, i32},
			{XCORE_REG_SED, i32},
			{XCORE_REG_KEP, i32},
			{XCORE_REG_KSP, i32},
			{XCORE_REG_ID, i32},
	};

	_reg2type = std::move(r2t);
}

void Capstone2LlvmIrTranslatorXcore_impl::initializePseudoCallInstructionIDs()
{
	_callInsnIds = {
			XCORE_INS_BL,
			XCORE_INS_BLA,
			XCORE_INS_BLAT,
			XCORE_INS_DCALL,
			XCORE_INS_KCALL,
	};

	_returnInsnIds = {
			XCORE_INS_RETSP,
			XCORE_INS_DRET,
			XCORE_INS_KRET,
	};

	_branchInsnIds = {
			XCORE_INS_BU,
			XCORE_INS_BAU,
			XCORE_INS_BRU,
	};

	_condBranchInsnIds = {
			XCORE_INS_BF,
			XCORE_INS_BT,
	};

	_controlFlowInsnIds = {
			XCORE_INS_BL,
			XCORE_INS_BLA,
			XCORE_INS_BLAT,
			XCORE_INS_BU,
			XCORE_INS_BAU,
			XCORE_INS_BRU,
			XCORE_INS_BF,
			XCORE_INS_BT,
			XCORE_INS_RETSP,
			XCORE_INS_DCALL,
			XCORE_INS_DRET,
			XCORE_INS_KCALL,
			XCORE_INS_KRET,
	};
}

std::map<std::size_t, void (Capstone2LlvmIrTranslatorXcore_impl::*)(cs_insn* i, cs_xcore*, llvm::IRBuilder<>&)>
	Capstone2LlvmIrTranslatorXcore_impl::_i2fm = {
		{XCORE_INS_ADD, &Capstone2LlvmIrTranslatorXcore_impl::translateAdd},
		{XCORE_INS_SUB, &Capstone2LlvmIrTranslatorXcore_impl::translateSub},
		{XCORE_INS_AND, &Capstone2LlvmIrTranslatorXcore_impl::translateAnd},
		{XCORE_INS_ANDNOT, &Capstone2LlvmIrTranslatorXcore_impl::translateAndnot},
		{XCORE_INS_OR, &Capstone2LlvmIrTranslatorXcore_impl::translateOr},
		{XCORE_INS_XOR, &Capstone2LlvmIrTranslatorXcore_impl::translateXor},
		{XCORE_INS_NOT, &Capstone2LlvmIrTranslatorXcore_impl::translateNot},
		{XCORE_INS_NEG, &Capstone2LlvmIrTranslatorXcore_impl::translateNeg},
		{XCORE_INS_MUL, &Capstone2LlvmIrTranslatorXcore_impl::translateMul},
		{XCORE_INS_SHL, &Capstone2LlvmIrTranslatorXcore_impl::translateShl},
		{XCORE_INS_SHR, &Capstone2LlvmIrTranslatorXcore_impl::translateShr},
		{XCORE_INS_ASHR, &Capstone2LlvmIrTranslatorXcore_impl::translateAshr},
		{XCORE_INS_EQ, &Capstone2LlvmIrTranslatorXcore_impl::translateEq},
		{XCORE_INS_LSS, &Capstone2LlvmIrTranslatorXcore_impl::translateLss},
		{XCORE_INS_LSU, &Capstone2LlvmIrTranslatorXcore_impl::translateLsu},
		{XCORE_INS_CLZ, &Capstone2LlvmIrTranslatorXcore_impl::translateClz},
		{XCORE_INS_LDC, &Capstone2LlvmIrTranslatorXcore_impl::translateLdc},
		{XCORE_INS_LDAW, &Capstone2LlvmIrTranslatorXcore_impl::translateLdaw},
		{XCORE_INS_LDW, &Capstone2LlvmIrTranslatorXcore_impl::translateLoad},
		{XCORE_INS_LD16S, &Capstone2LlvmIrTranslatorXcore_impl::translateLoad},
		{XCORE_INS_LD8U, &Capstone2LlvmIrTranslatorXcore_impl::translateLoad},
		{XCORE_INS_STW, &Capstone2LlvmIrTranslatorXcore_impl::translateStore},
		{XCORE_INS_ST16, &Capstone2LlvmIrTranslatorXcore_impl::translateStore},
		{XCORE_INS_ST8, &Capstone2LlvmIrTranslatorXcore_impl::translateStore},
		{XCORE_INS_BU, &Capstone2LlvmIrTranslatorXcore_impl::translateBu},
		{XCORE_INS_BAU, &Capstone2LlvmIrTranslatorXcore_impl::translateBau},
		{XCORE_INS_BRU, &Capstone2LlvmIrTranslatorXcore_impl::translateBru},
		{XCORE_INS_BF, &Capstone2LlvmIrTranslatorXcore_impl::translateBf},
		{XCORE_INS_BT, &Capstone2LlvmIrTranslatorXcore_impl::translateBt},
		{XCORE_INS_BL, &Capstone2LlvmIrTranslatorXcore_impl::translateBl},
		{XCORE_INS_BLA, &Capstone2LlvmIrTranslatorXcore_impl::translateBla},
		{XCORE_INS_RETSP, &Capstone2LlvmIrTranslatorXcore_impl::translateRetsp},
		{XCORE_INS_ENTSP, &Capstone2LlvmIrTranslatorXcore_impl::translateEntsp},
};

} // namespace capstone2llvmir
} // namespace retdec
