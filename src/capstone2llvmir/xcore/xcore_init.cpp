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
			XCORE_INS_ECALLF,
			XCORE_INS_ECALLT,
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
			XCORE_INS_ECALLF,
			XCORE_INS_ECALLT,
	};
}

std::map<std::size_t, void (Capstone2LlvmIrTranslatorXcore_impl::*)(cs_insn* i, cs_xcore*, llvm::IRBuilder<>&)>
	Capstone2LlvmIrTranslatorXcore_impl::_i2fm = {
		{XCORE_INS_INVALID, nullptr},

		{XCORE_INS_ADD, &Capstone2LlvmIrTranslatorXcore_impl::translateAdd},
		{XCORE_INS_ANDNOT, &Capstone2LlvmIrTranslatorXcore_impl::translateAndnot},
		{XCORE_INS_AND, &Capstone2LlvmIrTranslatorXcore_impl::translateAnd},
		{XCORE_INS_ASHR, &Capstone2LlvmIrTranslatorXcore_impl::translateAshr},
		{XCORE_INS_BAU, &Capstone2LlvmIrTranslatorXcore_impl::translateBau},
		{XCORE_INS_BITREV, &Capstone2LlvmIrTranslatorXcore_impl::translateBitrev},
		{XCORE_INS_BLA, &Capstone2LlvmIrTranslatorXcore_impl::translateBla},
		{XCORE_INS_BLAT, &Capstone2LlvmIrTranslatorXcore_impl::translateBlat},
		{XCORE_INS_BL, &Capstone2LlvmIrTranslatorXcore_impl::translateBl},
		{XCORE_INS_BF, &Capstone2LlvmIrTranslatorXcore_impl::translateBf},
		{XCORE_INS_BT, &Capstone2LlvmIrTranslatorXcore_impl::translateBt},
		{XCORE_INS_BU, &Capstone2LlvmIrTranslatorXcore_impl::translateBu},
		{XCORE_INS_BRU, &Capstone2LlvmIrTranslatorXcore_impl::translateBru},
		{XCORE_INS_BYTEREV, &Capstone2LlvmIrTranslatorXcore_impl::translateByterev},
		{XCORE_INS_CHKCT, nullptr},
		{XCORE_INS_CLRE, nullptr},
		{XCORE_INS_CLRPT, nullptr},
		{XCORE_INS_CLRSR, &Capstone2LlvmIrTranslatorXcore_impl::translateClrsr},
		{XCORE_INS_CLZ, &Capstone2LlvmIrTranslatorXcore_impl::translateClz},
		{XCORE_INS_CRC8, &Capstone2LlvmIrTranslatorXcore_impl::translateCrc8},
		{XCORE_INS_CRC32, &Capstone2LlvmIrTranslatorXcore_impl::translateCrc32},
		{XCORE_INS_DCALL, &Capstone2LlvmIrTranslatorXcore_impl::translateDcall},
		{XCORE_INS_DENTSP, &Capstone2LlvmIrTranslatorXcore_impl::translateDentsp},
		{XCORE_INS_DGETREG, nullptr},
		{XCORE_INS_DIVS, &Capstone2LlvmIrTranslatorXcore_impl::translateDiv},
		{XCORE_INS_DIVU, &Capstone2LlvmIrTranslatorXcore_impl::translateDiv},
		{XCORE_INS_DRESTSP, &Capstone2LlvmIrTranslatorXcore_impl::translateDrestsp},
		{XCORE_INS_DRET, &Capstone2LlvmIrTranslatorXcore_impl::translateDret},
		{XCORE_INS_ECALLF, &Capstone2LlvmIrTranslatorXcore_impl::translateEcall},
		{XCORE_INS_ECALLT, &Capstone2LlvmIrTranslatorXcore_impl::translateEcall},
		{XCORE_INS_EDU, nullptr},
		{XCORE_INS_EEF, nullptr},
		{XCORE_INS_EET, nullptr},
		{XCORE_INS_EEU, nullptr},
		{XCORE_INS_ENDIN, nullptr},
		{XCORE_INS_ENTSP, &Capstone2LlvmIrTranslatorXcore_impl::translateEntsp},
		{XCORE_INS_EQ, &Capstone2LlvmIrTranslatorXcore_impl::translateEq},
		{XCORE_INS_EXTDP, &Capstone2LlvmIrTranslatorXcore_impl::translateExtdp},
		{XCORE_INS_EXTSP, &Capstone2LlvmIrTranslatorXcore_impl::translateExtsp},
		{XCORE_INS_FREER, nullptr},
		{XCORE_INS_FREET, nullptr},
		{XCORE_INS_GETD, nullptr},
		{XCORE_INS_GET, &Capstone2LlvmIrTranslatorXcore_impl::translateGet},
		{XCORE_INS_GETN, nullptr},
		{XCORE_INS_GETR, nullptr},
		{XCORE_INS_GETSR, &Capstone2LlvmIrTranslatorXcore_impl::translateGetsr},
		{XCORE_INS_GETST, nullptr},
		{XCORE_INS_GETTS, nullptr},
		{XCORE_INS_INCT, nullptr},
		{XCORE_INS_INIT, nullptr},
		{XCORE_INS_INPW, nullptr},
		{XCORE_INS_INSHR, nullptr},
		{XCORE_INS_INT, nullptr},
		{XCORE_INS_IN, nullptr},
		{XCORE_INS_KCALL, &Capstone2LlvmIrTranslatorXcore_impl::translateKcall},
		{XCORE_INS_KENTSP, &Capstone2LlvmIrTranslatorXcore_impl::translateKentsp},
		{XCORE_INS_KRESTSP, &Capstone2LlvmIrTranslatorXcore_impl::translateKrestsp},
		{XCORE_INS_KRET, &Capstone2LlvmIrTranslatorXcore_impl::translateKret},
		{XCORE_INS_LADD, &Capstone2LlvmIrTranslatorXcore_impl::translateLadd},
		{XCORE_INS_LD16S, &Capstone2LlvmIrTranslatorXcore_impl::translateLoad},
		{XCORE_INS_LD8U, &Capstone2LlvmIrTranslatorXcore_impl::translateLoad},
		{XCORE_INS_LDA16, &Capstone2LlvmIrTranslatorXcore_impl::translateLdaw},
		{XCORE_INS_LDAP, &Capstone2LlvmIrTranslatorXcore_impl::translateLdap},
		{XCORE_INS_LDAW, &Capstone2LlvmIrTranslatorXcore_impl::translateLdaw},
		{XCORE_INS_LDC, &Capstone2LlvmIrTranslatorXcore_impl::translateLdc},
		{XCORE_INS_LDW, &Capstone2LlvmIrTranslatorXcore_impl::translateLoad},
		{XCORE_INS_LDIVU, &Capstone2LlvmIrTranslatorXcore_impl::translateLdivu},
		{XCORE_INS_LMUL, &Capstone2LlvmIrTranslatorXcore_impl::translateLmul},
		{XCORE_INS_LSS, &Capstone2LlvmIrTranslatorXcore_impl::translateLss},
		{XCORE_INS_LSUB, &Capstone2LlvmIrTranslatorXcore_impl::translateLsub},
		{XCORE_INS_LSU, &Capstone2LlvmIrTranslatorXcore_impl::translateLsu},
		{XCORE_INS_MACCS, &Capstone2LlvmIrTranslatorXcore_impl::translateMacc},
		{XCORE_INS_MACCU, &Capstone2LlvmIrTranslatorXcore_impl::translateMacc},
		{XCORE_INS_MJOIN, nullptr},
		{XCORE_INS_MKMSK, &Capstone2LlvmIrTranslatorXcore_impl::translateMkmsk},
		{XCORE_INS_MSYNC, nullptr},
		{XCORE_INS_MUL, &Capstone2LlvmIrTranslatorXcore_impl::translateMul},
		{XCORE_INS_NEG, &Capstone2LlvmIrTranslatorXcore_impl::translateNeg},
		{XCORE_INS_NOT, &Capstone2LlvmIrTranslatorXcore_impl::translateNot},
		{XCORE_INS_OR, &Capstone2LlvmIrTranslatorXcore_impl::translateOr},
		{XCORE_INS_OUTCT, nullptr},
		{XCORE_INS_OUTPW, nullptr},
		{XCORE_INS_OUTSHR, nullptr},
		{XCORE_INS_OUTT, nullptr},
		{XCORE_INS_OUT, nullptr},
		{XCORE_INS_PEEK, nullptr},
		{XCORE_INS_REMS, &Capstone2LlvmIrTranslatorXcore_impl::translateRem},
		{XCORE_INS_REMU, &Capstone2LlvmIrTranslatorXcore_impl::translateRem},
		{XCORE_INS_RETSP, &Capstone2LlvmIrTranslatorXcore_impl::translateRetsp},
		{XCORE_INS_SETCLK, nullptr},
		{XCORE_INS_SET, &Capstone2LlvmIrTranslatorXcore_impl::translateSet},
		{XCORE_INS_SETC, nullptr},
		{XCORE_INS_SETD, nullptr},
		{XCORE_INS_SETEV, nullptr},
		{XCORE_INS_SETN, nullptr},
		{XCORE_INS_SETPSC, nullptr},
		{XCORE_INS_SETPT, nullptr},
		{XCORE_INS_SETRDY, nullptr},
		{XCORE_INS_SETSR, &Capstone2LlvmIrTranslatorXcore_impl::translateSetsr},
		{XCORE_INS_SETTW, nullptr},
		{XCORE_INS_SETV, nullptr},
		{XCORE_INS_SEXT, &Capstone2LlvmIrTranslatorXcore_impl::translateSext},
		{XCORE_INS_SHL, &Capstone2LlvmIrTranslatorXcore_impl::translateShl},
		{XCORE_INS_SHR, &Capstone2LlvmIrTranslatorXcore_impl::translateShr},
		{XCORE_INS_SSYNC, &Capstone2LlvmIrTranslatorXcore_impl::translateNop},
		{XCORE_INS_ST16, &Capstone2LlvmIrTranslatorXcore_impl::translateStore},
		{XCORE_INS_ST8, &Capstone2LlvmIrTranslatorXcore_impl::translateStore},
		{XCORE_INS_STW, &Capstone2LlvmIrTranslatorXcore_impl::translateStore},
		{XCORE_INS_SUB, &Capstone2LlvmIrTranslatorXcore_impl::translateSub},
		{XCORE_INS_SYNCR, nullptr},
		{XCORE_INS_TESTCT, nullptr},
		{XCORE_INS_TESTLCL, nullptr},
		{XCORE_INS_TESTWCT, nullptr},
		{XCORE_INS_TSETMR, nullptr},
		{XCORE_INS_START, nullptr},
		{XCORE_INS_WAITEF, nullptr},
		{XCORE_INS_WAITET, nullptr},
		{XCORE_INS_WAITEU, nullptr},
		{XCORE_INS_XOR, &Capstone2LlvmIrTranslatorXcore_impl::translateXor},
		{XCORE_INS_ZEXT, &Capstone2LlvmIrTranslatorXcore_impl::translateZext},
};

} // namespace capstone2llvmir
} // namespace retdec
