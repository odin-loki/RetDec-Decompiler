/**
* @file src/llvmir2hll/support/global_vars_sorter.cpp
* @brief Implementation of GlobalVarsSorter.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

#include "retdec/llvmir2hll/ir/expression.h"
#include "retdec/llvmir2hll/ir/global_var_def.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/support/global_vars_sorter.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"
#include "retdec/utils/container.h"
#include "retdec/utils/non_copyable.h"

using retdec::utils::hasItem;

namespace retdec {
namespace llvmir2hll {

namespace {

/**
* @brief Sorter of global variables according to their interdependencies.
*/
class InterdependencySorter: private OrderedAllVisitor,
		private retdec::utils::NonCopyable {
public:
	/**
	 * @brief Implementation of GlobalVarsSorter::sortByInterdependencies().
	 *
	 * This used to be a std::sort() over a comparator that read "p1 < p2 if
	 * p2's initializer names p1, and otherwise by the original name".  That is
	 * not an ordering.  Two globals that name each other -- `int *a = &b; int
	 * *b = &a;` -- make it answer "less than" in both directions, and even
	 * without a cycle it is not transitive: with `a = &c`, `b = &d`, `c = &b`,
	 * it reports a < b (neither names the other, so the names decide), b < c
	 * (c names b), and c < a (a names c).  std::sort is entitled to anything
	 * at all when handed a comparator like that, including running off the end
	 * of the range; what it actually produced was an order that could differ
	 * between two runs of the same input.
	 *
	 * The dependency relation is a partial order, so it is applied as one: the
	 * definitions are put in the order independent ones would take -- those
	 * whose initializer names nothing first, then by the variable's original
	 * name -- and then emitted earliest-first, a definition only once every
	 * global its initializer names has been emitted.  A set of globals that
	 * cannot be untangled (they name each other, directly or through others)
	 * keeps that base order.
	 */
	static GlobalVarDefVector sort(const GlobalVarDefVector& globalVars)
	{
		ShPtr<InterdependencySorter> sorter(new InterdependencySorter(globalVars));
		return sorter->sortImpl(globalVars);
	}

private:
	/// Returns whether @a p1 comes before @a p2 when neither depends on the
	/// other.  A strict weak ordering: a bool key, then a string key.
	bool independentOrder(const ShPtr<GlobalVarDef>& p1, const ShPtr<GlobalVarDef>& p2)
	{
		// A variable with no initializer, or one whose initializer names no
		// variable, is emitted before one that has something to say.
		const bool p1Indep = varToUsedVarsMap[p1->getVar()].empty();
		const bool p2Indep = varToUsedVarsMap[p2->getVar()].empty();
		if (p1Indep != p2Indep)
		{
			return p1Indep;
		}

		// The original names are used rather than the current ones so that
		// the variables and their comments (address, original name) appear in
		// a sorted order.  Variables are renamed before HLL emission, and the
		// renaming loses the groups the frontend emits them in -- registers
		// in one group, other variables in others -- so sorting on the
		// current name would mix the groups up.
		return p1->getVar()->getInitialName() < p2->getVar()->getInitialName();
	}

	GlobalVarDefVector sortImpl(const GlobalVarDefVector& globalVars)
	{
		const std::size_t n = globalVars.size();

		GlobalVarDefVector base(globalVars);
		std::stable_sort(
			base.begin(), base.end(), [this](const ShPtr<GlobalVarDef>& p1, const ShPtr<GlobalVarDef>& p2) {
				return independentOrder(p1, p2);
			});

		// Where each global variable sits in the base order.  A variable
		// defined twice keeps its first position; a variable named by an
		// initializer but not defined here is absent, and cannot block.
		std::map<ShPtr<Variable>, std::size_t> varToIdx;
		for (std::size_t i = 0; i < n; ++i)
		{
			varToIdx.emplace(base[i]->getVar(), i);
		}

		// dependents[j] lists the definitions whose initializer names base[j].
		std::vector<std::vector<std::size_t>> dependents(n);
		std::vector<std::size_t> waitingOn(n, 0);
		for (std::size_t i = 0; i < n; ++i)
		{
			for (const auto& used: varToUsedVarsMap[base[i]->getVar()])
			{
				auto j = varToIdx.find(used);
				if (j == varToIdx.end() || j->second == i)
				{
					// Not one of these globals, or the definition naming
					// itself.  Neither can be waited for.
					continue;
				}
				dependents[j->second].push_back(i);
				++waitingOn[i];
			}
		}

		// Emit in base order, earliest definition whose dependencies are all
		// out first.  `ready` and `notEmitted` are ordered by index, so the
		// result is a function of the base order alone.
		std::set<std::size_t> ready;
		std::set<std::size_t> notEmitted;
		for (std::size_t i = 0; i < n; ++i)
		{
			notEmitted.insert(i);
			if (waitingOn[i] == 0)
			{
				ready.insert(i);
			}
		}

		GlobalVarDefVector sorted;
		sorted.reserve(n);
		while (!notEmitted.empty())
		{
			std::size_t next;
			if (!ready.empty())
			{
				next = *ready.begin();
				ready.erase(ready.begin());
			}
			else
			{
				// Every definition left is waiting on another one that is
				// also left: they name each other, directly or through
				// others, and no order satisfies all of them.  Take the
				// earliest in the base order and carry on.
				next = *notEmitted.begin();
			}
			notEmitted.erase(next);
			sorted.push_back(base[next]);

			for (auto dep: dependents[next])
			{
				if (waitingOn[dep] > 0 && --waitingOn[dep] == 0 && notEmitted.count(dep))
				{
					ready.insert(dep);
				}
			}
		}

		return sorted;
	}

	explicit InterdependencySorter(const GlobalVarDefVector &globalVars) {
		// Compute used variables in the initializers of all global variables.
		for (const auto &varInitPair : globalVars) {
			usedVarsInLastInit.clear();
			if (ShPtr<Expression> init = varInitPair->getInitializer()) {
				init->accept(this);
			}
			varToUsedVarsMap[varInitPair->getVar()] = usedVarsInLastInit;
		}
	}

	/// @name Visitor Interface
	/// @{
	using OrderedAllVisitor::visit;
	virtual void visit(ShPtr<Variable> var) override {
		usedVarsInLastInit.insert(var);
	}
	/// @}

private:
	/// Used variables in the last initializer.
	VarSet usedVarsInLastInit;

	/// Mapping of a variable into the set of variables used in in its
	/// initializer.
	std::map<ShPtr<Variable>, VarSet> varToUsedVarsMap;
};

} // anonymous namespace

/**
* @brief Sorts the given vector of global variables by their interdependencies.
*
* For example, if it contains the following two global variables
* @code
* int g = 5;
* @endcode
* and
* @code
* int *p = &g;
* @endcode
* then they are ordered in this way because of their interdependencies.
*
* @par Preconditions
*  - the variables can be sorted in this way, i.e. there are no dependency
*    loops that would prevent the variables from being sorted
*/
GlobalVarDefVector GlobalVarsSorter::sortByInterdependencies(
		const GlobalVarDefVector &globalVars) {
	return InterdependencySorter::sort(globalVars);
}

} // namespace llvmir2hll
} // namespace retdec
