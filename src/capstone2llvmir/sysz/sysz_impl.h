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
		llvm::Value* generateCondition(sysz_cc cc, llvm::IRBuilder<>& irb);
		llvm::Value* loadBranchTarget(cs_sysz* si, llvm::IRBuilder<>& irb);
		sysz_cc conditionFromInsn(cs_insn* i, cs_sysz* si);
		bool isAlwaysCondition(sysz_cc cc, cs_insn* i);
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
		void translateBr(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateBrc(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateBasr(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
		void translateBrasl(cs_insn* i, cs_sysz* si, llvm::IRBuilder<>& irb);
};

} // namespace capstone2llvmir
} // namespace retdec

#endif
