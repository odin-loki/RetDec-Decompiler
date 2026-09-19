/**
 * @file src/capstone2llvmir/riscv/riscv_impl.h
 * @brief RISC-V implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef CAPSTONE2LLVMIR_RISCV_RISCV_IMPL_H
#define CAPSTONE2LLVMIR_RISCV_RISCV_IMPL_H

#include "retdec/capstone2llvmir/riscv/riscv.h"
#include "capstone2llvmir/capstone2llvmir_impl.h"

namespace retdec {
namespace capstone2llvmir {

class Capstone2LlvmIrTranslatorRiscv_impl :
		public Capstone2LlvmIrTranslator_impl<cs_riscv, cs_riscv_op>,
		public Capstone2LlvmIrTranslatorRiscv
{
	public:
		Capstone2LlvmIrTranslatorRiscv_impl(
				llvm::Module* m,
				cs_mode basic = CS_MODE_RISCV32,
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
// RISC-V-specific methods.
//==============================================================================
//
	protected:
		bool isXlen64() const;
		llvm::Value* getCurrentPc(cs_insn* i);
		llvm::Value* pcRelativeTarget(cs_insn* i, llvm::Value* offset, llvm::IRBuilder<>& irb);
		llvm::Value* maskShiftAmount(llvm::IRBuilder<>& irb, llvm::Value* val, llvm::Value* amount);
		llvm::Value* narrowToWord(llvm::IRBuilder<>& irb, llvm::Value* val);
		llvm::Value* widenFromWord(llvm::IRBuilder<>& irb, llvm::Value* val);

		virtual llvm::Value* loadRegister(
				uint32_t r,
				llvm::IRBuilder<>& irb,
				llvm::Type* dstType = nullptr,
				eOpConv ct = eOpConv::THROW) override;
		virtual llvm::Value* loadOp(
				cs_riscv_op& op,
				llvm::IRBuilder<>& irb,
				llvm::Type* ty = nullptr,
				bool lea = false) override;

		virtual llvm::Instruction* storeRegister(
				uint32_t r,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;
		virtual llvm::Instruction* storeOp(
				cs_riscv_op& op,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;

		virtual bool isOperandRegister(cs_riscv_op& op) override;

		/// Sources of a 2-operand compressed ALU or a 3-operand I/R ALU.
		std::pair<llvm::Value*, llvm::Value*> loadAluSrc(
				cs_riscv* ri,
				llvm::IRBuilder<>& irb,
				eOpConv ct);
//
//==============================================================================
// RISC-V implementation data.
//==============================================================================
//
	protected:
		static std::map<
			std::size_t,
			void (Capstone2LlvmIrTranslatorRiscv_impl::*)(
					cs_insn* i,
					cs_riscv*,
					llvm::IRBuilder<>&)> _i2fm;
//
//==============================================================================
// RISC-V instruction translation methods.
//==============================================================================
//
	protected:
		void translateLui(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateAuipc(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateJal(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateJalr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateBranch(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateLoad(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateStore(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateAdd(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateSub(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateAnd(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateOr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateXor(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateSlt(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateSltu(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateSll(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateSrl(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateSra(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateOp32(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateLi(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateMv(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateEcall(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateNop(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
		void translateFence(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb);
};

} // namespace capstone2llvmir
} // namespace retdec

#endif
