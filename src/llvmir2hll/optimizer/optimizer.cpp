/**
* @file src/llvmir2hll/optimizer/optimizer.cpp
* @brief Implementation of Optimizer.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/optimizer/optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

/// Global deadline for the HLL optimisation phase.  Zero means "no deadline".
static std::chrono::steady_clock::time_point g_globalDeadline{};

void Optimizer::setGlobalDeadline(std::chrono::steady_clock::time_point tp) {
	g_globalDeadline = tp;
}

bool Optimizer::isGlobalDeadlineExceeded() {
	if (g_globalDeadline == std::chrono::steady_clock::time_point{}) {
		return false;
	}
	return std::chrono::steady_clock::now() >= g_globalDeadline;
}

/**
* @brief Constructs a new optimizer.
*
* @param[in] module Module to be optimized.
*
* @par Preconditions
*  - @a module is non-null
*/
Optimizer::Optimizer(ShPtr<Module> module):
	OrderedAllVisitor(), module(module) {
		PRECONDITION_NON_NULL(module);
	}

/**
* @brief Performs all the optimizations of the specific optimizer.
*
* @return Optimized module.
*
* This function calls the following functions (in the specified order), so
* subclass any of them to implement the desired behavior.
*
*  (1) doInitialization()
*  (2) doOptimization()
*  (3) doFinalization()
*/
ShPtr<Module> Optimizer::optimize() {
	doInitialization();
	doOptimization();
	doFinalization();
	return module;
}

/**
* @brief Performs pre-optimization matters.
*
* This function is called before any optimizations are done.
*
* By default, this function does nothing.
*/
void Optimizer::doInitialization() {}

/**
* @brief Performs the optimization.
*
* This function is called after @c doInitialization() and before @c
* doFinalization(), and should perform all the optimizations of the specific
* optimizer.
*
* By default, this function does nothing.
*/
void Optimizer::doOptimization() {}

/**
* @brief Performs post-optimization matters.
*
* This function is called after all optimizations are done.
*
* By default, this function does nothing.
*/
void Optimizer::doFinalization() {}

} // namespace llvmir2hll
} // namespace retdec
