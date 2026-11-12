/**
 * @file include/retdec/bin2llvmir/optimizations/idioms/idioms_analysis.h
 * @brief Instruction idioms analysis
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_IDIOMS_ANALYSIS_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_IDIOMS_ANALYSIS_H

#include <functional>

#include <cstdio>

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include "retdec/bin2llvmir/optimizations/idioms/idioms_abstract.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_borland.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_common.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_gcc.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_intel.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_llvm.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_magicdivmod.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_owatcom.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_types.h"
#include "retdec/bin2llvmir/optimizations/idioms/idioms_vstudio.h"
#include "retdec/bin2llvmir/providers/config.h"

namespace retdec {
namespace bin2llvmir {

class IdiomsAnalysis:
	public IdiomsBorland,
	public IdiomsCommon,
	public IdiomsGCC,
	public IdiomsIntel,
	public IdiomsLLVM,
	public IdiomsMagicDivMod,
	public IdiomsOWatcom,
	public IdiomsVStudio  {
public:
	IdiomsAnalysis(llvm::Module * M, CC_compiler cc, CC_arch arch)
	{
		init(M, cc, arch);
	}
	virtual bool doAnalysis(llvm::Function & f, llvm::Pass * p) override;

private:
	bool analyse(llvm::Function & f, llvm::Pass * p, int (IdiomsAnalysis::*exchanger)(llvm::Function &, llvm::Pass *) const, const char * fname);
	/**
	 * Calls @p exchanger through a pointer to the class that declares it.
	 *
	 * This used to take `Instruction* (IdiomsAnalysis::*)(iterator) const`
	 * and call `(this->*exchanger)(insn)`. Converting `&IdiomsMagicDivMod::f`
	 * to that type is legal, but the call is then made on an IdiomsAnalysis
	 * whose `this` the member pointer has already adjusted to a base
	 * subobject -- with the virtual IdiomsAbstract base in the hierarchy,
	 * UBSan's vptr check reports it ("member call on address ... which does
	 * not point to an object of type 'IdiomsAnalysis'"), and the sanitizers
	 * job died on it in the first decompile. Calling through a `const C*`
	 * makes the object expression the subobject the function belongs to.
	 */
	template <typename C>
	bool analyse(
		llvm::BasicBlock& bb, llvm::Instruction* (C::*exchanger)(llvm::BasicBlock::iterator) const, const char* fname)
	{
		const C* self = this;
		return analyseWith(
			bb, [self, exchanger](llvm::BasicBlock::iterator it) { return (self->*exchanger)(it); }, fname);
	}
	bool analyseWith(
		llvm::BasicBlock& bb,
		const std::function<llvm::Instruction*(llvm::BasicBlock::iterator)>& exchanger,
		const char* fname);
};

} // namespace bin2llvmir
} // namespace retdec

#endif
