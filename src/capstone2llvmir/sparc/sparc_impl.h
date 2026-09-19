/**
 * @file src/capstone2llvmir/sparc/sparc_impl.h
 * @brief SPARC implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef CAPSTONE2LLVMIR_SPARC_SPARC_IMPL_H
#define CAPSTONE2LLVMIR_SPARC_SPARC_IMPL_H

#include <map>
#include <utility>

#include "retdec/capstone2llvmir/sparc/sparc.h"
#include "capstone2llvmir/capstone2llvmir_impl.h"

namespace retdec {
namespace capstone2llvmir {

class Capstone2LlvmIrTranslatorSparc_impl :
		public Capstone2LlvmIrTranslator_impl<cs_sparc, cs_sparc_op>,
		public Capstone2LlvmIrTranslatorSparc
{
	public:
		Capstone2LlvmIrTranslatorSparc_impl(
				llvm::Module* m,
				cs_mode basic = CS_MODE_LITTLE_ENDIAN,
				cs_mode extra = CS_MODE_BIG_ENDIAN);
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
// Capstone related getters - from Capstone2LlvmIrTranslator.
//==============================================================================
//
	public:
		virtual bool hasDelaySlot(uint32_t id) const override;
		virtual bool hasDelaySlotTypical(uint32_t id) const override;
		virtual bool hasDelaySlotLikely(uint32_t id) const override;
		virtual std::size_t getDelaySlot(uint32_t id) const override;
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
// SPARC-specific methods.
//==============================================================================
//
	protected:
		bool isV9() const;
		bool isBigEndian() const;
		bool isZeroRegister(uint32_t r) const;
		bool isGeneralPurposeRegister(uint32_t r) const;
		bool isFpSingleRegister(uint32_t r) const;
		bool isFpDoubleRegister(uint32_t r) const;
		bool isFccRegister(uint32_t r) const;
		bool isGprPairRegister(uint32_t r) const;
		uint32_t gprPairEven(uint32_t r) const;
		uint32_t nextGpr(uint32_t r) const;
		uint32_t fccFromField(sparc_cc_field field) const;

		llvm::Value* loadOpAddress(cs_sparc_op& op, llvm::IRBuilder<>& irb);
		std::pair<llvm::Value*, llvm::Value*> loadOpRs1Rs2(
				cs_sparc* si,
				llvm::IRBuilder<>& irb);
		cs_sparc_op* destOperand(cs_sparc* si);
		cs_sparc_op* fpDestOperand(cs_sparc* si);

		void storeIcc(
				llvm::Value* result,
				llvm::Value* op0,
				llvm::Value* op1,
				bool isSub,
				llvm::IRBuilder<>& irb);
		void storeFcc(
				llvm::Value* a,
				llvm::Value* b,
				uint32_t fccReg,
				llvm::IRBuilder<>& irb);
		llvm::Value* generateIccCondition(
				sparc_cc cc,
				uint32_t ccReg,
				unsigned nibbleShift,
				llvm::IRBuilder<>& irb);
		llvm::Value* generateFccCondition(
				sparc_cc cc,
				uint32_t fccReg,
				llvm::IRBuilder<>& irb);
		llvm::Value* generateCondition(cs_sparc* si, llvm::IRBuilder<>& irb);

		void copyOutsToIns(llvm::IRBuilder<>& irb);
		void copyInsToOuts(llvm::IRBuilder<>& irb);

		virtual llvm::Value* loadRegister(
				uint32_t r,
				llvm::IRBuilder<>& irb,
				llvm::Type* dstType = nullptr,
				eOpConv ct = eOpConv::THROW) override;
		virtual llvm::Value* loadOp(
				cs_sparc_op& op,
				llvm::IRBuilder<>& irb,
				llvm::Type* ty = nullptr,
				bool lea = false) override;

		virtual llvm::StoreInst* storeRegister(
				uint32_t r,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;
		virtual llvm::Instruction* storeOp(
				cs_sparc_op& op,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;

		virtual bool isOperandRegister(cs_sparc_op& op) override;
//
//==============================================================================
// SPARC implementation data.
//==============================================================================
//
	protected:
		static std::map<
			std::size_t,
			void (Capstone2LlvmIrTranslatorSparc_impl::*)(
					cs_insn* i,
					cs_sparc*,
					llvm::IRBuilder<>&)> _i2fm;
//
//==============================================================================
// SPARC instruction translation methods.
//==============================================================================
//
	protected:
		void translateAdd(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateAnd(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateB(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateBr(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateCall(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateCmp(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateDiv(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateFpArith(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateFpCmp(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateFpUnary(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateJmpl(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateLoad(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateMov(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateMul(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateNop(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateOr(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateRd(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateRestore(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateRet(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateRett(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateSave(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateSethi(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateShift(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateStore(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateSub(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateWr(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
		void translateXor(cs_insn* i, cs_sparc* si, llvm::IRBuilder<>& irb);
};

} // namespace capstone2llvmir
} // namespace retdec

#endif
