/**
* @file src/llvmir2hll/optimizer/optimizers/unused_global_var_optimizer.cpp
* @brief Implementation of UnusedGlobalVarOptimizer.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/global_var_def.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/optimizer/optimizers/unused_global_var_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/utils/container.h"

using retdec::utils::hasItem;

namespace retdec {
namespace llvmir2hll {

/**
* @brief Constructs a new optimizer.
*
* @param[in] module Module to be optimized.
*
* @par Preconditions
*  - @a module is non-null
*/
UnusedGlobalVarOptimizer::UnusedGlobalVarOptimizer(ShPtr<Module> module):
	Optimizer(module), globalVars(module->getGlobalVars()) {
		PRECONDITION_NON_NULL(module);
	}

void UnusedGlobalVarOptimizer::doOptimization() {
	computeUsedGlobalVars();
	removeUnusedGlobalVars();
}

void UnusedGlobalVarOptimizer::visit(ShPtr<Variable> var) {
	if (isGlobal(var)) {
		usedGlobalVars.insert(var);
	}
}

/**
* @brief Computes used global variables.
*/
void UnusedGlobalVarOptimizer::computeUsedGlobalVars() {
	// Initializers of global variables.
	for (auto i = module->global_var_begin(), e = module->global_var_end();
			i != e; ++i) {
		if (ShPtr<Expression> init = (*i)->getInitializer()) {
			init->accept(this);
		}
	}

	// Function bodies.
	for (auto i = module->func_definition_begin(),
			e = module->func_definition_end(); i != e; ++i) {
		(*i)->accept(this);
	}
}

/**
* @brief Removes unused global variables from the module.
*
* A global variable is removed only when ALL of the following hold:
*   (1) It is not read by any function body or global initializer.
*   (2) It does not have external linkage.
*
* Condition (2) guards against removing variables that are visible to — and
* potentially read by — other translation units.  Removing an externally-linked
* symbol would silently break linking even though it appears unused within the
* current module.
*/
void UnusedGlobalVarOptimizer::removeUnusedGlobalVars() {
	for (auto &var : globalVars) {
		if (!isUsed(var) && !hasExternalLinkage(var)) {
			module->removeGlobalVar(var);
		}
	}
}

/**
* @brief Is the given variable global?
*/
bool UnusedGlobalVarOptimizer::isGlobal(ShPtr<Variable> var) const {
	return hasItem(globalVars, var);
}

/**
* @brief Is the given global variable used?
*/
bool UnusedGlobalVarOptimizer::isUsed(ShPtr<Variable> var) const {
	return hasItem(usedGlobalVars, var);
}

/**
* @brief Returns @c true if @a var has external linkage.
*
* Externally-linked globals may be accessed from other translation units, so
* they must not be removed even when they appear to be unused within the
* current module.
*/
bool UnusedGlobalVarOptimizer::hasExternalLinkage(ShPtr<Variable> var) const {
	return var->isExternal();
}

} // namespace llvmir2hll
} // namespace retdec
