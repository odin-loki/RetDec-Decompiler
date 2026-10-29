/**
 * @file include/retdec/bin2llvmir/optimizations/provider_init/provider_init.h
 * @brief One time providers initialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_PROVIDER_INIT_PROVIDER_INIT_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_PROVIDER_INIT_PROVIDER_INIT_H

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace retdec {

namespace config {

class Config;

} // namespace config
namespace bin2llvmir {

class ProviderInitialization : public llvm::ModulePass
{
	public:
		static char ID;
		ProviderInitialization(retdec::config::Config* c = nullptr);
		virtual bool runOnModule(llvm::Module& m) override;
		virtual bool doFinalization(llvm::Module& m) override;

		void setConfig(retdec::config::Config* c);

	private:
		retdec::config::Config* _config = nullptr;
};

/**
 * Drops every provider entry belonging to @a m: config, file image, ABI,
 * demangler, debug info, LTI, names and the asm-instruction maps. Call it
 * when a pipeline is done with its module so concurrent pipelines do not
 * accumulate each other's state.
 */
void releaseProviders(llvm::Module* m);

} // namespace bin2llvmir
} // namespace retdec

#endif
