/**
 * @file include/retdec/bin2llvmir/providers/abi/xcore.h
 * @brief ABI information for XCore (XS1, 32-bit).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_ABI_XCORE_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_ABI_XCORE_H

#include "retdec/bin2llvmir/providers/abi/abi.h"

namespace retdec {
namespace bin2llvmir {

/**
 * XMOS XS1 ABI: arguments in r0-r3, return in r0, link register lr,
 * stack pointer sp. 32-bit only.
 */
class AbiXcore : public Abi
{
	public:
		AbiXcore(llvm::Module* m, Config* c);

	public:
		virtual bool isGeneralPurposeRegister(const llvm::Value* val) const override;
		virtual bool isNopInstruction(cs_insn* insn) override;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
