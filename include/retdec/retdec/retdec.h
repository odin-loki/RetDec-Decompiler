/**
 * \file include/retdec/retdec/retdec.h
 * \brief RetDec library.
 * \copyright (c) 2019 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_RETDEC_RETDEC_H
#define RETDEC_RETDEC_RETDEC_H

#include <memory>
#include <capstone/capstone.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "retdec/common/basic_block.h"
#include "retdec/common/function.h"
#include "retdec/config/config.h"

namespace retdec {
namespace config { class Parameters; }

/**
 * Configure the global RetDec logger from decompiler parameters.
 * Sets the output log file (or stdout), error file, and verbosity.
 * Shared by retdec-decompiler and the retdec library to avoid drift.
 */
void setLogsFrom(const retdec::config::Parameters& params);

struct LlvmModuleContextPair
{
	LlvmModuleContextPair(LlvmModuleContextPair&&) = default;
	~LlvmModuleContextPair()
	{
		// Order matters: module destructor uses context.
		module.reset();
		context.reset();
	}
	std::unique_ptr<llvm::Module> module;
	std::unique_ptr<llvm::LLVMContext> context;
};

/**
 * \param[in]  inputPath Path the the input file to disassemble.
 * \param[out] fs        Set of functions to fill.
 * \return Pointer to LLVM module created by the disassembly,
 *         or \c nullptr if the disassembly failed.
 */
LlvmModuleContextPair disassemble(
		const std::string& inputPath,
		retdec::common::FunctionSet* fs = nullptr
);

/**
 * Run a decompilation according to a \p config configuration.
 * If \p outString is set, decompilation output will be returned
 * in this string. Otherwise, output file is expected to be set in \p config.
 */
bool decompile(
		retdec::config::Config& config,
		std::string* outString = nullptr
);

/**
 * Run decompilation passes up to (but not including) \p stopBeforePass.
 * Returns the LLVM module and context for Stage 3 emulation-based unpacking.
 * Returns empty pair on failure.
 */
LlvmModuleContextPair decompileToLlvmIr(
		retdec::config::Config& config,
		const std::string& stopBeforePass = "retdec-llvmir2hll"
);

/**
 * Stage 3: Attempt emulation-based unpacking when no plugin matched.
 * Lifts the packed binary to LLVM IR, emulates from entry point, and dumps
 * memory to \p outputPath if successful. Returns true if unpacked output
 * was produced.
 */
bool tryEmulationUnpacking(
		retdec::config::Config& config,
		const std::string& outputPath
);

} // namespace retdec

#endif
