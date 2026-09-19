/**
 * @file src/capstone2llvmir/arm/arm_impl.h
 * @brief ARM implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef CAPSTONE2LLVMIR_ARM_ARM_IMPL_H
#define CAPSTONE2LLVMIR_ARM_ARM_IMPL_H

#include <llvm/IR/Intrinsics.h>

#include "retdec/capstone2llvmir/arm/arm.h"
#include "capstone2llvmir/capstone2llvmir_impl.h"

namespace retdec {
namespace capstone2llvmir {

class Capstone2LlvmIrTranslatorArm_impl :
		public Capstone2LlvmIrTranslator_impl<cs_arm, cs_arm_op>,
		public Capstone2LlvmIrTranslatorArm
{
	public:
		Capstone2LlvmIrTranslatorArm_impl(
				llvm::Module* m,
				cs_mode basic = CS_MODE_ARM,
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
// ARM-specific methods.
//==============================================================================
//
	protected:
		llvm::Value* getCurrentPc(cs_insn* i);

		virtual llvm::Value* loadRegister(
				uint32_t r,
				llvm::IRBuilder<>& irb,
				llvm::Type* dstType = nullptr,
				eOpConv ct = eOpConv::THROW) override;
		virtual llvm::Value* loadOp(
				cs_arm_op& op,
				llvm::IRBuilder<>& irb,
				llvm::Type* ty = nullptr,
				bool lea = false) override;

		virtual llvm::Instruction* storeRegister(
				uint32_t r,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;
		virtual llvm::Instruction* storeOp(
				cs_arm_op& op,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;

		llvm::Value* generateInsnConditionCode(
				llvm::IRBuilder<>& irb,
				cs_arm* ai);

		/// The index term of a memory operand, with its shift and sign but
		/// without generateOperandShift()'s flag writes; see the definition.
		llvm::Value* loadMemIndexTerm(cs_arm_op& op, llvm::IRBuilder<>& irb);
		/// A float-to-integer conversion with ARM's defined answer for NaN and
		/// out-of-range inputs; see the definition.
		llvm::Value*
		generateFpToIntSaturating(llvm::Value* v, llvm::Type* intTy, bool isSigned, llvm::IRBuilder<>& irb);
		llvm::Value* generateOperandShift(
				llvm::IRBuilder<>& irb,
				cs_arm_op& op,
				llvm::Value* val);
		enum class eShiftKind
		{
			Lsl,
			Lsr,
			Asr
		};
		llvm::Value* generateShiftCommon(llvm::IRBuilder<>& irb, llvm::Value* val, llvm::Value* n, eShiftKind kind);
		llvm::Value* generateShiftAsr(
				llvm::IRBuilder<>& irb,
				llvm::Value* val,
				llvm::Value* n);
		llvm::Value* generateShiftLsl(
				llvm::IRBuilder<>& irb,
				llvm::Value* val,
				llvm::Value* n);
		llvm::Value* generateShiftLsr(
				llvm::IRBuilder<>& irb,
				llvm::Value* val,
				llvm::Value* n);
		llvm::Value* generateShiftRor(
				llvm::IRBuilder<>& irb,
				llvm::Value* val,
				llvm::Value* n);
		llvm::Value* generateShiftRrx(
				llvm::IRBuilder<>& irb,
				llvm::Value* val,
				llvm::Value* n);

		uint32_t sysregNumberTranslation(uint32_t r);
//
//==============================================================================
// Helper methods.
//==============================================================================
//
	protected:
		virtual bool isOperandRegister(cs_arm_op& op) override;
		virtual uint8_t getOperandAccess(cs_arm_op& op) override;
//
//==============================================================================
// ARM implementation data.
//==============================================================================
//
	protected:
		static std::map<
			std::size_t,
			void (Capstone2LlvmIrTranslatorArm_impl::*)(
					cs_insn* i,
					cs_arm*,
					llvm::IRBuilder<>&)> _i2fm;
//
//==============================================================================
// ARM instruction translation methods.
//==============================================================================
//
	protected:
		void translateAdc(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		static bool isFpRegister(uint32_t r);
		static bool isSingleView(uint32_t r);
		static uint32_t singleViewParent(uint32_t r, unsigned& offset);
		llvm::Value* loadSingleView(uint32_t r, llvm::IRBuilder<>& irb);
		llvm::StoreInst* storeSingleView(uint32_t r, llvm::Value* val, llvm::IRBuilder<>& irb);
		static bool isScalarVfp(cs_arm* ai);
		llvm::Type* vfpTypeOfReg(uint32_t r, llvm::IRBuilder<>& irb);
		llvm::Value* loadVfpOp(cs_arm_op& op, llvm::IRBuilder<>& irb, llvm::Type* ty);

		void translateVfpArithm(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateVfpCmp(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateVfpCvt(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateVfpLoadStore(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateVfpMla(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateVfpMov(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateVfpUnary(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateVmrs(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);

		void translateAdd(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateAnd(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateB(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateBl(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateCbnz(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateCbz(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateClz(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateMrc(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateBitfield(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateSxt(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateParallelArith(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateSel(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateRev16(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateEor(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateLdmStm(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateLdr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateLdrd(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateMla(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateMls(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateMov(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateMovt(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateMovw(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateMul(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateNop(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateHint(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateAdr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateOrn(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateVfpPushPop(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateFence(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateOrr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateRev(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateSbc(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateShifts(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateStr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateStrex(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateSub(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateSwp(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateUmlal(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateUmull(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateUxtah(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateUxtb(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateUxtb16(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateUxth(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);

		void translateDiv(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateTbb(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translatePkh(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateSat(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void translateHalfwordMul(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb);
		void annotateBxBlxIfNeeded(cs_insn* i, cs_arm* ai, llvm::CallInst* call);
};

} // namespace capstone2llvmir
} // namespace retdec

#endif
