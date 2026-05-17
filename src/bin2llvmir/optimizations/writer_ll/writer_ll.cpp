/**
 * @file src/bin2llvmir/optimizations/writer_ll/writer_ll.cpp
 * @brief Generate the current LLVM IR.
 * @copyright (c) 2020 Odin Loch Trading as Imortek
 */

#include <memory>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdlib>

#include "retdec/bin2llvmir/optimizations/writer_ll/writer_ll.h"
#include "retdec/bin2llvmir/providers/config.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char LlvmIrWriter::ID = 0;

static RegisterPass<LlvmIrWriter> X(
		"retdec-write-ll",
		"Generate the current LLVM IR",
		 false, // Only looks at CFG
		 false // Analysis Pass
);

LlvmIrWriter::LlvmIrWriter() :
		ModulePass(ID)
{

}

/**
 * Create assembly output file object.
 */
std::unique_ptr<ToolOutputFile> createAssemblyOutputFile(
		const std::string& outputFile)
{
	std::unique_ptr<ToolOutputFile> Out;


	if (outputFile.empty())
	{
		throw std::runtime_error("LLVM IR output file was not specified");
	}

	std::error_code EC;
	Out.reset(new ToolOutputFile(outputFile, EC, sys::fs::F_None));
	if (EC)
	{
		throw std::runtime_error(
			"failed to create llvm::ToolOutputFile for .ll: " + EC.message()
		);
	}

	return Out;
}

bool LlvmIrWriter::runOnModule(Module& M)
{
	auto* c = ConfigProvider::getConfig(&M);

	auto out = c->getConfig().parameters.getOutputLlvmirFile();
	if (out.empty())
	{
		return false;
	}
	const char* traceEnv = std::getenv("RETDEC_WRITE_LL_TRACE");
	if (traceEnv && *traceEnv && std::string(traceEnv) != "0")
	{
		llvm::errs() << "[writer-ll] output: " << out << "\n";
		llvm::errs().flush();
	}

	std::unique_ptr<ToolOutputFile> llOut = createAssemblyOutputFile(out);
	raw_ostream* llOs = &llOut->os();
	bool ShouldPreserveUseListOrder = true;
	M.print(*llOs, nullptr, ShouldPreserveUseListOrder);
	llOut->os().flush();
	llOut->os().clear_error();
	llOut->keep();
	(void)llOut.release();

	return false;
}

} // namespace bin2llvmir
} // namespace retdec
