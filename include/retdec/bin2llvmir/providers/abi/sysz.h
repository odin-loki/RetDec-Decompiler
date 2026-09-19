/**
 * @file include/retdec/bin2llvmir/providers/abi/sysz.h
 * @brief ABI information for SystemZ (ELF s390x).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_ABI_SYSZ_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_ABI_SYSZ_H

#include "retdec/bin2llvmir/providers/abi/abi.h"

namespace retdec {
namespace bin2llvmir {

/**
 * ELF s390x (zSeries) ABI: arguments in r2-r6, return in r2,
 * return address in r14, stack pointer in r15.
 */
class AbiSysz : public Abi
{
	public:
		AbiSysz(llvm::Module* m, Config* c);

	public:
		virtual bool isGeneralPurposeRegister(const llvm::Value* val) const override;
		virtual bool isNopInstruction(cs_insn* insn) override;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
