/**
 * @file include/retdec/bin2llvmir/providers/abi/sparc64.h
 * @brief ABI information for SPARC V9 (SPARC64).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_ABI_SPARC64_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_ABI_SPARC64_H

#include "retdec/bin2llvmir/providers/abi/abi.h"

namespace retdec {
namespace bin2llvmir {

class AbiSparc64 : public Abi
{
	// Ctors, dtors.
	//
	public:
		AbiSparc64(llvm::Module* m, Config* c);

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
