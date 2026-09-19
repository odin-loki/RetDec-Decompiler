/**
 * @file src/capstone2llvmir/sysz/sysz_impl.h
 * @brief SystemZ implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef CAPSTONE2LLVMIR_SYSZ_SYSZ_IMPL_H
#define CAPSTONE2LLVMIR_SYSZ_SYSZ_IMPL_H

#include "retdec/capstone2llvmir/sysz/sysz.h"
#include "capstone2llvmir/capstone2llvmir_impl.h"

namespace retdec {
namespace capstone2llvmir {

class Capstone2LlvmIrTranslatorSysz_impl :
		public Capstone2LlvmIrTranslator_impl<cs_sysz, cs_sysz_op>,
		public Capstone2LlvmIrTranslatorSysz
{
	public:
		Capstone2LlvmIrTranslatorSysz_impl(
				llvm::Module* m,
				cs_mode basic = CS_MODE_BIG_ENDIAN,
				cs_mode extra = CS_MODE_LITTLE_ENDIAN);
//
//==============================================================================
// Mode query & modification methods - from Capstone2LlvmIrTranslator.
//==============================================================================
//
	public:
		virtual bool isAllowedBasicMode(cs_mode m) override;
		virtual bool isAllowedExtraMode(cs_mode m) override;
		virtual uint32_t getArchByteSize() override;
//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//
	protected:
		virtual void initializeArchSpecific() override;
		virtual void initializeRegNameMap() override;
		virtual void initializeRegTypeMap() override;
		virtual void initializePseudoCallInstructionIDs() override;
		virtual void generateEnvironmentArchSpecific() override;
		virtual void generateDataLayout() override;
		virtual void generateRegisters() override;
		virtual uint32_t getCarryRegister() override;

		virtual void translateInstruction(
				cs_insn* i,
				llvm::IRBuilder<>& irb) override;
//
//==============================================================================
// SystemZ-specific methods.
//==============================================================================
//
	protected:
		virtual llvm::Value* loadRegister(
				uint32_t r,
				llvm::IRBuilder<>& irb,
				llvm::Type* dstType = nullptr,
				eOpConv ct = eOpConv::THROW) override;
		virtual llvm::Value* loadOp(
				cs_sysz_op& op,
				llvm::IRBuilder<>& irb,
				llvm::Type* ty = nullptr,
				bool lea = false) override;

		virtual llvm::StoreInst* storeRegister(
				uint32_t r,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;
		virtual llvm::Instruction* storeOp(
				cs_sysz_op& op,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;

		virtual bool isOperandRegister(cs_sysz_op& op) override;

		llvm::Value* generateMemAddress(cs_sysz_op& op, llvm::IRBuilder<>& irb);
		llvm::Value* loadAddrReg(uint32_t r, llvm::IRBuilder<>& irb);
		llvm::Value* extractLow32(llvm::Value* val, llvm::IRBuilder<>& irb);
		llvm::Value* depositLow32(
				uint32_t r,
				llvm::Value* lo32,
				llvm::IRBuilder<>& irb);
		void storeCcSigned(
				llvm::Value* result,
				llvm::Value* overflow,
				llvm::IRBuilder<>& irb);
		void storeCcLogical(llvm::Value* result, llvm::IRBuilder<>& irb);
		void storeCcFp(llvm::Value* result, llvm::IRBuilder<>& irb);
		void storeCcFpCompare(llvm::Value* a, llvm::Value* b, llvm::IRBuilder<>& irb);
		void storeCcCompare(llvm::Value* a, llvm::Value* b, llvm::IRBuilder<>& irb);
		llvm::Value* loadFp(cs_sysz_op& op, llvm::IRBuilder<>& irb, llvm::Type* ty);
		void storeFp(cs_sysz_op& op, llvm::Value* val, llvm::IRBuilder<>& irb);
		bool isFpSingleInsn(unsigned id) const;
		llvm::Type* fpTypeForInsn(unsigned id, llvm::IRBuilder<>& irb) const;
		llvm::Value* generateCondition(systemz_cc cc, llvm::IRBuilder<>& irb);
		llvm::Value* loadBranchTarget(cs_sysz* si, llvm::IRBuilder<>& irb);
		systemz_cc conditionFromInsn(cs_insn* i, cs_sysz* si);
		bool isAlwaysCondition(systemz_cc cc, cs_insn* i);
		unsigned gprIndex(uint32_t r) const;
		uint32_t gpr64(unsigned idx) const;
		uint64_t ssLength(cs_sysz* si) const;
		unsigned lastImm(cs_sysz* si, unsigned def) const;
		unsigned vectorLaneBits(cs_insn* i, cs_sysz* si) const;
		unsigned vectorFpLaneBits(cs_insn* i, cs_sysz* si) const;
		void storeCcUnsignedCompare(
				llvm::Value* a,
				llvm::Value* b,
				llvm::IRBuilder<>& irb);
//
//==============================================================================
// SystemZ implementation data.
//==============================================================================
//
	protected:
		static std::map<
			std::size_t,
			void (Capstone2LlvmIrTranslatorSysz_impl::*)(
					cs_insn* i,
					cs_sysz*,
					llvm::IRBuilder<>&)> _i2fm;
//
//==============================================================================
// SystemZ instruction translation methods.
//==============================================================================
//
	protected:
		void translateLoadReg32(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLoadReg64(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateAdd32(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateAdd64(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateSub32(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateSub64(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLogical32(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLoad32(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLoad64(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateStore32(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateStore64(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLa(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLay(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateBr(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateBrc(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateBasr(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateBrasl(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLogical64(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateImm64(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateAddImm(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateCompare(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateExtend32(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateShift64(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFpArith(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFpCompare(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFpLoad(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFpStore(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFpMove(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLdeb(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLedbr(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVectorLoad(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVectorStore(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVlr(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVlrep(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVleg(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVsteg(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateSs(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateMvcl(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLoadMultiple(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateStoreMultiple(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateCs(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateCds(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVectorArith(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVectorFpArith(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVectorMul(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVperm(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVpdi(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVrep(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVpk(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFidbr(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFpConvert(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFpSqrt(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateFpFma(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLoc(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateRisbg(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateLaa(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVsel(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVectorShift(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVectorUnary(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVllez(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVupk(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVseg(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVgef(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVgbm(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVgm(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVlei(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateVfee(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		llvm::Value* generateCcMask(unsigned mask, llvm::IRBuilder<>& irb);
		bool isLoc64(unsigned id) const;
};

} // namespace capstone2llvmir
} // namespace retdec

#endif
