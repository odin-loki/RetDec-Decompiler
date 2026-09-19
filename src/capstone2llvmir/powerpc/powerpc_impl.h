/**
 * @file src/capstone2llvmir/powerpc/powerpc_impl.h
 * @brief PowerPC implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef CAPSTONE2LLVMIR_POWERPC_POWERPC_IMPL_H
#define CAPSTONE2LLVMIR_POWERPC_POWERPC_IMPL_H

#include <llvm/IR/Intrinsics.h>

#include "retdec/capstone2llvmir/powerpc/powerpc.h"
#include "capstone2llvmir/capstone2llvmir_impl.h"

namespace retdec {
namespace capstone2llvmir {

class Capstone2LlvmIrTranslatorPowerpc_impl :
		public Capstone2LlvmIrTranslator_impl<cs_ppc, cs_ppc_op>,
		public Capstone2LlvmIrTranslatorPowerpc
{
	public:
		Capstone2LlvmIrTranslatorPowerpc_impl(
				llvm::Module* m,
				cs_mode basic = CS_MODE_32,
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
// PowerPC-specific methods.
//==============================================================================
//
	protected:
		virtual llvm::Value* loadRegister(
				uint32_t r,
				llvm::IRBuilder<>& irb,
				llvm::Type* dstType = nullptr,
				eOpConv ct = eOpConv::THROW) override;
		virtual llvm::Value* loadOp(
				cs_ppc_op& op,
				llvm::IRBuilder<>& irb,
				llvm::Type* ty = nullptr,
				bool lea = false) override;

		virtual llvm::StoreInst* storeRegister(
				uint32_t r,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;
		virtual llvm::Instruction* storeOp(
				cs_ppc_op& op,
				llvm::Value* val,
				llvm::IRBuilder<>& irb,
				eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST) override;

		void storeCrX(
				llvm::IRBuilder<>& irb,
				uint32_t crReg,
				llvm::Value* op0,
				llvm::Value* op1 = nullptr,
				bool signedCmp = true);
		void storeCr0(llvm::IRBuilder<>& irb, cs_ppc* pi, llvm::Value* val);
		/// CR0 for a word operation whose result is zero-extended into the
		/// register; see the definition. The sign-extending word operations
		/// must keep using storeCr0().
		void storeCr0Word(llvm::IRBuilder<>& irb, cs_ppc* pi, llvm::Value* val);

		/// A float-to-integer conversion with Power's defined answer for NaN and
		/// out-of-range inputs; see the definition. Power's NaN answer is the
		/// destination MINIMUM, not zero as on ARM.
		llvm::Value* generateFpToIntBounded(llvm::Value* v, llvm::Type* intTy, bool isSigned, llvm::IRBuilder<>& irb);
		void crFieldRegisters(uint32_t crReg, uint32_t& ltR, uint32_t& gtR, uint32_t& eqR, uint32_t& soR);

		llvm::Value* roundToSingle(llvm::IRBuilder<>& irb, llvm::Value* val);
		llvm::Value* loadOpDouble(cs_ppc_op& op, llvm::IRBuilder<>& irb);
		llvm::Value* loadOpI128(cs_ppc_op& op, llvm::IRBuilder<>& irb);
		void storeOpI128(cs_ppc_op& op, llvm::Value* val, llvm::IRBuilder<>& irb);
		llvm::Value* fpIntrinsic(llvm::IRBuilder<>& irb, llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Value*> args);
		static bool isSinglePrecisionForm(unsigned id);
		uint32_t canonicalRegister(uint32_t r) const;
		llvm::Value* i128HighDouble(llvm::Value* v, llvm::IRBuilder<>& irb);
		llvm::Value* insertHighDouble(llvm::Value* vec, llvm::Value* d, llvm::IRBuilder<>& irb);
		llvm::Value* asVec128(llvm::Value* v, llvm::Type* elemTy, unsigned n, llvm::IRBuilder<>& irb);

		std::tuple<llvm::Value*, llvm::Value*, llvm::Value*, llvm::Value*> loadCrX(
				llvm::IRBuilder<>& irb,
				uint32_t crReg);
		llvm::Value* loadCrX(
				llvm::IRBuilder<>& irb,
				uint32_t crReg,
				ppc_pred type);

		bool isGeneralPurposeRegister(uint32_t r);
		uint32_t getGeneralPurposeRegisterIndex(uint32_t r);
		uint32_t crBitIndexToCrRegister(uint32_t idx);
		bool isCrRegister(uint32_t r);
		bool isCrRegister(cs_ppc_op& op);
		bool isCrBitRegister(uint32_t r);
		llvm::Value* generateIndexedAddress(cs_ppc* pi, llvm::Value* base, llvm::Value* index, llvm::IRBuilder<>& irb);
		llvm::Value* loadIndexedEffectiveAddress(cs_ppc* pi, llvm::IRBuilder<>& irb);
		virtual bool isOperandRegister(cs_ppc_op& op) override;
//
//==============================================================================
// PowerPC implementation data.
//==============================================================================
//
	protected:
		static std::map<
			std::size_t,
			void (Capstone2LlvmIrTranslatorPowerpc_impl::*)(
					cs_insn* i,
					cs_ppc*,
					llvm::IRBuilder<>&)> _i2fm;
//
//==============================================================================
// PowerPC instruction translation methods.
//==============================================================================
//
	protected:
		void translateAdd(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateAddc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateAdde(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateAddis(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateAddme(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateAddze(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateAnd(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateAndc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateAndis(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateB(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateClrlwi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateCmp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		llvm::Value* narrowToWord(llvm::Value* v, llvm::IRBuilder<>& irb);
		void translateCntlzw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateCrModifTernary(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateCrNotMove(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateCrBitOp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMfcr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateCrSetClr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateDivw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateEqv(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateExtendSign(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateFcmp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateFence(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateFpArithm(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateFpTernary(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateFpUnary(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLoadStoreByteReverse(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLoadStoreMultiple(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLis(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLoad(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLoadFloat(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLoadFloatIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLoadIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMcrf(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMfctr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMflr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMtcrf(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMtctr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMtlr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMulhw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateMullw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateNand(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateNeg(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateNop(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateNor(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateNot(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateOr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateOrc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateOris(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateRotateComplex5op(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateRotateWordMask(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateRotateDoubleMask(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateRotlw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateShiftLeft(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateShiftRight(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateSlwi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateSrwi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateSraw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateStore(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateStoreFloat(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateStoreFloatIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateStoreIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateSubf(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateSubfc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateSubfe(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateSubfme(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateSubfze(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateXor(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateXoris(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateVecLogical(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateVecLoadIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateVecStoreIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateVecSplatImm(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateVecFpArith(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateVecIntAdd(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateXxpermdi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateXxspltw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateLoadFloatAsInt(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateStoreFloatAsInt(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
		void translateVsxMove(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb);
};

} // namespace capstone2llvmir
} // namespace retdec

#endif
