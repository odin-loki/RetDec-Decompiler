/**
* @file src/llvmir2hll/optimizer/optimizer.cpp
* @brief Implementation of Optimizer.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <atomic>

#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/optimizer/optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

/// Global deadline for the HLL optimisation phase, as a tick count.  Zero means
/// "no deadline".
///
/// Atomic, and stored as the underlying rep rather than as a time_point: the
/// setter and the readers are a plain write and a plain read of a 64-bit object
/// that nothing else orders, so a caller setting the budget while the
/// optimisers are asking about it is a data race in the language sense even
/// where the hardware would not tear it. The pipeline lock in
/// src/retdec/retdec.cpp keeps two decompilations out of here at once; this is
/// what makes a watchdog on another thread safe as well.
static std::atomic<std::chrono::steady_clock::rep> g_globalDeadlineTicks{0};

void Optimizer::setGlobalDeadline(std::chrono::steady_clock::time_point tp) {
	g_globalDeadlineTicks.store(tp.time_since_epoch().count(), std::memory_order_relaxed);
}

bool Optimizer::isGlobalDeadlineExceeded() {
	const auto ticks = g_globalDeadlineTicks.load(std::memory_order_relaxed);
	if (ticks == 0) {
		return false;
	}
	const std::chrono::steady_clock::time_point deadline{
		std::chrono::steady_clock::duration{ticks}};
	return std::chrono::steady_clock::now() >= deadline;
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
