/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/unused_global_var_optimizer.h
* @brief Removes global variables that are not used.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_UNUSED_GLOBAL_VAR_OPTIMIZER_H
#define RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_UNUSED_GLOBAL_VAR_OPTIMIZER_H

#include "retdec/llvmir2hll/optimizer/optimizer.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

/**
* @brief Removes global variables that are not used.
*
* A global variable is removed only when it is not read by any function body
* or global initializer AND it does not have external linkage.  Externally-
* linked globals are preserved even when they appear unused locally because
* other translation units may reference them.
*
* This is a concrete optimizer which should not be subclassed.
*/
class UnusedGlobalVarOptimizer final: public Optimizer {
public:
	UnusedGlobalVarOptimizer(ShPtr<Module> module);

	virtual std::string getId() const override { return "UnusedGlobalVar"; }

private:
	virtual void doOptimization() override;

	/// @name Visitor Interface
	/// @{
	using OrderedAllVisitor::visit;
	virtual void visit(ShPtr<Variable> var) override;
	/// @}

	void computeUsedGlobalVars();
	void removeUnusedGlobalVars();
	bool isGlobal(ShPtr<Variable> var) const;
	bool isUsed(ShPtr<Variable> var) const;
	bool hasExternalLinkage(ShPtr<Variable> var) const;

private:
	/// Global variables in @c module. This is here to speedup the optimization.
	/// By using this set, we do not have to ask @c module every time we need
	/// such information.
	VarSet globalVars;

	/// Global variables that are used.
	VarSet usedGlobalVars;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
