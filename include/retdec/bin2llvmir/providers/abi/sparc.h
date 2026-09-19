/**
 * @file include/retdec/bin2llvmir/providers/abi/sparc.h
 * @brief ABI information for SPARC V8 (32-bit).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_ABI_SPARC_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_ABI_SPARC_H

#include "retdec/bin2llvmir/providers/abi/abi.h"

namespace retdec {
namespace bin2llvmir {

class AbiSparc : public Abi
{
	// Ctors, dtors.
	//
	public:
		AbiSparc(llvm::Module* m, Config* c);

	// Registers.
	//
	public:
		virtual bool isGeneralPurposeRegister(const llvm::Value* val) const override;

	// Instructions.
	//
	public:
		virtual bool isNopInstruction(cs_insn* insn) override;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
