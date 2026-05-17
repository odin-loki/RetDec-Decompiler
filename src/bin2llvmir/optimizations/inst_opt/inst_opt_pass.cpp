/**
 * @file src/bin2llvmir/optimizations/inst_opt/inst_opt_pass.cpp
 * @brief LLVM instruction optimization pass.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#include <llvm/IR/InstIterator.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdlib>

#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt_pass.h"
#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

namespace {

bool instOptTraceEnabled()
{
	const char* env = std::getenv("RETDEC_INST_OPT_TRACE");
	return env && *env && std::string(env) != "0";
}

void traceInstOpt(const std::string& msg)
{
	if (!instOptTraceEnabled())
	{
		return;
	}
	llvm::errs() << "[inst-opt] " << msg << "\n";
	llvm::errs().flush();
}

}

char InstructionOptimizer::ID = 0;

static RegisterPass<InstructionOptimizer> X(
		"retdec-inst-opt",
		"LLVM instruction optimization",
		false, // Only looks at CFG
		false // Analysis Pass
);

InstructionOptimizer::InstructionOptimizer() :
		ModulePass(ID)
{

}

bool InstructionOptimizer::runOnModule(Module& m)
{
	_module = &m;
	return run();
}

bool InstructionOptimizer::runOnModuleCustom(llvm::Module& m)
{
	_module = &m;
	return run();
}

bool InstructionOptimizer::run()
{
	bool changed = false;

	for (Function& f : *_module)
	{
		traceInstOpt("function begin: " + f.getName().str());
		for (auto it = inst_begin(&f), eIt = inst_end(&f); it != eIt;)
		{
			Instruction* insn = &*it;
			++it;
			changed |= inst_opt::optimize(insn);
		}
		traceInstOpt("function end: " + f.getName().str());
	}

	return changed;
}

} // namespace bin2llvmir
} // namespace retdec
