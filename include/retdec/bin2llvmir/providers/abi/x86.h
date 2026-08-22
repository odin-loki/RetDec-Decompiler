/**
 * @file include/retdec/bin2llvmir/providers/abi/x86.h
 * @brief ABI information for x86.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_ABI_X86_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_ABI_X86_H

#include "retdec/bin2llvmir/providers/abi/abi.h"

namespace retdec {
namespace bin2llvmir {

class AbiX86 : public Abi
{
	// Ctors, dtors.
	//
	public:
		AbiX86(llvm::Module* m, Config* c);

	// Registers.
	//
	public:
		virtual bool isGeneralPurposeRegister(const llvm::Value* val) const override;

	// Instructions.
	//
	public:
		virtual bool isNopInstruction(cs_insn* insn) override;

	// Calling conventions.
	//
	private:
		CallingConvention::ID fetchDefaultCC() const;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
