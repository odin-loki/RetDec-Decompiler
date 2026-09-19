/**
 * @file src/capstone2llvmir/xcore/xcore_impl.h
 * @brief XCore implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef CAPSTONE2LLVMIR_XCORE_XCORE_IMPL_H
#define CAPSTONE2LLVMIR_XCORE_XCORE_IMPL_H

#include "retdec/capstone2llvmir/xcore/xcore.h"
#include "capstone2llvmir/capstone2llvmir_impl.h"

namespace retdec {
namespace capstone2llvmir {

class Capstone2LlvmIrTranslatorXcore_impl :
		public Capstone2LlvmIrTranslator_impl<cs_xcore, cs_xcore_op>,
		public Capstone2LlvmIrTranslatorXcore
{
	public:
		Capstone2LlvmIrTranslatorXcore_impl(
				llvm::Module* m,
				cs_mode basic = CS_MODE_LITTLE_ENDIAN,
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
// XCore-specific methods.
//==============================================================================
//
	protected:
		virtual llvm::Value* loadRegister(
				uint32_t r,
				llvm::IRBuilder<>& irb,
				llvm::Type* dstType = nullptr,
				eOpConv ct = eOpConv::THROW) override;
		virtual llvm::Value* loadOp(
				cs_xcore_op& op,
				llvm::IRBuilder<>& irb,
				llvm::Type* ty = nullptr,
				bool lea = false) override;

		virtual llvm::StoreInst* storeRegister(
				uint32_t r,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;
		virtual llvm::Instruction* storeOp(
				cs_xcore_op& op,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;

		virtual bool isOperandRegister(cs_xcore_op& op) override;

		llvm::Value* generateMemAddress(
				cs_xcore_op& op,
				llvm::IRBuilder<>& irb,
				unsigned scale);
		llvm::Value* generateShift(
				llvm::Value* val,
				llvm::Value* amount,
				bool arithmetic,
				bool left,
				llvm::IRBuilder<>& irb);

//
//==============================================================================
// XCore implementation data.
//==============================================================================
//
	protected:
		static std::map<
			std::size_t,
			void (Capstone2LlvmIrTranslatorXcore_impl::*)(
					cs_insn* i,
					cs_xcore*,
					llvm::IRBuilder<>&)> _i2fm;
//
//==============================================================================
// XCore instruction translation methods.
//==============================================================================
//
	protected:
		void translateAdd(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateSub(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateAnd(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateAndnot(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateOr(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateXor(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateNot(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateNeg(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateMul(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateShl(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateShr(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateAshr(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateEq(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateLss(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateLsu(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateClz(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateLdc(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateLdaw(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateLoad(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateStore(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateBu(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateBau(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateBru(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateBf(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateBt(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateBl(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateBla(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateRetsp(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
		void translateEntsp(cs_insn* i, cs_xcore* xi, llvm::IRBuilder<>& irb);
};

} // namespace capstone2llvmir
} // namespace retdec

#endif
