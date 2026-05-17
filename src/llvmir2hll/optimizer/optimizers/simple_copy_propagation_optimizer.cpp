/**
* @file src/llvmir2hll/optimizer/optimizers/simple_copy_propagation_optimizer.cpp
* @brief Implementation of SimpleCopyPropagationOptimizer.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/analysis/var_uses_visitor.h"
#include "retdec/llvmir2hll/graphs/cfg/cfg.h"
#include "retdec/llvmir2hll/graphs/cfg/cfg_builders/non_recursive_cfg_builder.h"
#include "retdec/llvmir2hll/graphs/cfg/cfg_traversals/lhs_rhs_uses_cfg_traversal.h"
#include "retdec/llvmir2hll/graphs/cg/cg_builder.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/const_array.h"
#include "retdec/llvmir2hll/ir/const_string.h"
#include "retdec/llvmir2hll/ir/const_struct.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/obtainer/call_info_obtainer.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simple_copy_propagation_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/utils/ir.h"
#include "retdec/utils/container.h"

using retdec::utils::hasItem;

namespace retdec {
namespace llvmir2hll {

/**
* @brief Constructs a new optimizer.
*
* @param[in] module Module to be optimized.
* @param[in] va Analysis of values.
* @param[in] cio Obtainer of information about function calls.
*
* @par Preconditions
*  - @a module, @a va, and @a cio are non-null
*/
SimpleCopyPropagationOptimizer::SimpleCopyPropagationOptimizer(ShPtr<Module> module,
	ShPtr<ValueAnalysis> va, ShPtr<CallInfoObtainer> cio):
		FuncOptimizer(module), cfgBuilder(NonRecursiveCFGBuilder::create()),
		va(va), cio(cio), vuv(),
		globalVars(module->getGlobalVars()), currCFG(), triedVars() {
			PRECONDITION_NON_NULL(module);
			PRECONDITION_NON_NULL(va);
			PRECONDITION_NON_NULL(cio);
	}

namespace {
#ifdef _WIN32
const unsigned PARALLEL_THRESHOLD = 48;
#else
const unsigned PARALLEL_THRESHOLD = 24;
#endif
}

void SimpleCopyPropagationOptimizer::doOptimization() {
	va->clearCache();
	va->initAliasAnalysis(module);

	std::vector<ShPtr<Function>> funcs;
	for (auto i = module->func_begin(), e = module->func_end(); i != e; ++i)
		funcs.push_back(*i);

	if (funcs.size() < PARALLEL_THRESHOLD) {
		vuv = VarUsesVisitor::create(va, true, nullptr);
		for (auto& f : funcs) {
			if (isGlobalDeadlineExceeded()) break;
			runOnFunction(f);
		}
		return;
	}

	vuv = VarUsesVisitor::create(va, true, nullptr);
	std::atomic<std::size_t> nextIdx{0};
	std::mutex exMutex;
	std::exception_ptr firstException;
	auto sharedAA = va->getAliasAnalysis();
	// Cap workers to avoid lock contention on the shared AliasAnalysis.
	// Empirically, using more than 4 threads provides diminishing returns
	// for large Rust/Go binaries where workers share alias-analysis state.
	const unsigned numThreads = std::min(
		std::min(4u, std::max(1u, static_cast<unsigned>(std::thread::hardware_concurrency()))),
		static_cast<unsigned>(funcs.size()));

	auto workerFn = [&](SimpleCopyPropagationOptimizer* opt) {
		try {
			while (true) {
				if (isGlobalDeadlineExceeded()) break;
				const std::size_t idx = nextIdx.fetch_add(1, std::memory_order_relaxed);
				if (idx >= funcs.size()) break;
				opt->runOnFunction(funcs[idx]);
			}
		} catch (...) {
			std::lock_guard<std::mutex> lock(exMutex);
			if (!firstException) firstException = std::current_exception();
		}
	};

	if (numThreads <= 1) {
		workerFn(this);
	} else {
		std::vector<std::unique_ptr<SimpleCopyPropagationOptimizer>> workers;
		for (unsigned t = 1; t < numThreads; ++t) {
			auto workerVa = ValueAnalysis::create(sharedAA, false);
			auto w = std::unique_ptr<SimpleCopyPropagationOptimizer>(
				new SimpleCopyPropagationOptimizer(module, workerVa, cio));
			// Use nullptr (no precomputation) so that workers don't each walk
			// the entire module; precomputation is O(total statements) per worker
			// and causes extreme slowdowns for large Rust/Go binaries.
			w->vuv = VarUsesVisitor::create(workerVa, true, nullptr);
			workers.push_back(std::move(w));
		}
		std::vector<std::thread> threads;
		for (auto& w : workers) threads.emplace_back(workerFn, w.get());
		workerFn(this);
		for (auto& t : threads) t.join();
	}
	if (firstException) std::rethrow_exception(firstException);
}

void SimpleCopyPropagationOptimizer::runOnFunction(ShPtr<Function> func) {
	// Honour the global HLL optimisation deadline.
	if (isGlobalDeadlineExceeded()) {
		return;
	}

	// Skip mega-functions BEFORE building the CFG.  CFG construction for a
	// function with thousands of local variables (common in Rust due to
	// monomorphization and extensive inlining) is itself O(vars²) and can
	// take many minutes.  Use the local variable count as a cheap O(1) proxy.
	constexpr std::size_t MAX_LOCAL_VARS = 500;
	if (func->getNumOfLocalVars(true) > MAX_LOCAL_VARS) {
		return;
	}

	auto _fstart = std::chrono::steady_clock::now();
	currCFG = cfgBuilder->getCFG(func);

	// If CFG construction itself took too long the function is structurally
	// complex and further analysis would be even more expensive.  Skip it.
	constexpr long PER_FUNC_CFG_MS = 200;
	auto _cfgMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - _fstart).count();
	if (_cfgMs > PER_FUNC_CFG_MS) {
		return;
	}

	// Secondary guard: skip if the CFG itself turned out large.
	constexpr std::size_t MAX_CFG_NODES = 200;
	if (currCFG->getNumberOfNodes() > MAX_CFG_NODES) {
		return;
	}

	triedVars.clear();
	FuncOptimizer::runOnFunction(func);
}

void SimpleCopyPropagationOptimizer::visit(ShPtr<AssignStmt> stmt) {
	// First, visit nested statements.
	FuncOptimizer::visit(stmt);

	tryOptimization(stmt);
}

void SimpleCopyPropagationOptimizer::visit(ShPtr<VarDefStmt> stmt) {
	// First, visit nested statements.
	FuncOptimizer::visit(stmt);

	tryOptimization(stmt);
}

/**
* @brief Tries to perform the optimization on the given statement.
*
* @par Preconditions
*  - @a stmt is either a VarDefStmt or AssignStmt
*/
void SimpleCopyPropagationOptimizer::tryOptimization(ShPtr<Statement> stmt) {
	ShPtr<Variable> lhsVar(cast<Variable>(getLhs(stmt)));
	ShPtr<Expression> rhs(getRhs(stmt));
	if (!lhsVar || !rhs) {
		// There is nothing we can do in this case.
		return;
	}

	if (hasItem(triedVars, lhsVar)) {
		// We have already tried this variable.
		return;
	}
	triedVars.insert(lhsVar);

	if (!currFunc->hasLocalVar(lhsVar)) {
		// The left-hand side is not a local variable.
		return;
	}

	if (module->hasAssignedDebugName(lhsVar)) {
		// The left-hand side has assigned a name from debug information.
		return;
	}

	if (lhsVar->isExternal()) {
		// We do not want to optimize external variables (used in a volatile
		// load/store).
		return;
	}

	if (va->mayBePointed(lhsVar)) {
		// The left-hand side may be used indirectly.
		return;
	}

	if (isa<ConstString>(rhs) || isa<ConstArray>(rhs) || isa<ConstStruct>(rhs)) {
		// The expression cannot be any of the above types.
		// TODO What about dropping this restriction?
		return;
	}

	if (lhsVar == rhs) {
		// Do not optimize self assigns, i.e. statements of the form `a = a;`.
		// This is done in other optimizations.
		return;
	}

	ShPtr<ValueData> stmtData(va->getValueData(stmt));
	if (stmtData->hasAddressOps() || stmtData->hasDerefs() ||
			stmtData->hasArrayAccesses() || stmtData->hasStructAccesses()) {
		// A forbidden construction is used.
		return;
	}

	// Try to perform the proper case of the optimization (see the class
	// description).
	if (stmtData->hasCalls()) {
		tryOptimizationCase1(stmt, lhsVar, rhs);
	}
}

/**
* @brief Tries to perform the case (1) optimization from the class description
*        on the given statement.
*
* For the preconditions, see tryOptimization(), which is the place from where
* this function should be called.
*/
void SimpleCopyPropagationOptimizer::tryOptimizationCase1(
		ShPtr<Statement> stmt, ShPtr<Variable> lhsVar, ShPtr<Expression> rhs) {
	// Currently, we can only handle the situation where the right-hand side is
	// a function call; that is, there are no other computations.
	// TODO Add some more robust analysis to handle also this case.
	ShPtr<CallExpr> rhsCall(cast<CallExpr>(rhs));
	if (!rhsCall) {
		return;
	}

	// Function needs to be a declaration.
	auto var = cast<Variable>(rhsCall->getCalledExpr());
	if (!var) {
		return;
	}
	auto fnc = module->getFuncByName(var->getName());
	if (!fnc) {
		return;
	}

	// We will need the set of variables which may be accessed when calling the
	// function from the right-hand side.
	ShPtr<ValueData> stmtData(va->getValueData(stmt));
	const VarSet &varsAccessedInCall(stmtData->getDirAccessedVars());
	// ShPtr<CallInfo> rhsCallInfo(cio->getCallInfo(rhsCall, currFunc));

	// Get the first statement where the variable is used by going through the
	// successors of stmt. During this traversal, check that the optimization
	// can be done.
	ShPtr<Statement> firstUseStmt(stmt->getSuccessor());
	ShPtr<ValueData> firstUseStmtData;
	while (firstUseStmt) {
		firstUseStmtData = va->getValueData(firstUseStmt);
		if (firstUseStmtData->isDirAccessed(lhsVar)) {
			// Got it.
			break;
		}

		// There cannot be a compound statement.
		// TODO Add some more robust analysis to handle also this case.
		if (firstUseStmt->isCompound()) {
			return;
		}

		// There cannot be other calls, dereferences, or other possibly
		// "dangerous" constructs.
		if (firstUseStmtData->hasCalls() || firstUseStmtData->hasAddressOps() ||
				firstUseStmtData->hasDerefs() ||
				firstUseStmtData->hasArrayAccesses() ||
				firstUseStmtData->hasStructAccesses()) {
			return;
		}

		// The statement cannot contain a variable which is accessed in the
		// original call from the right-hand side.
		for (auto i = firstUseStmtData->dir_all_begin(),
				e = firstUseStmtData->dir_all_end(); i != e; ++i) {
			if (hasItem(varsAccessedInCall, *i)) {
				return;
			}
		}

		// If declaration is called (function with no body), the subsequent
		// checks are not needed.
		if (fnc->isDeclaration()) {
			firstUseStmt = firstUseStmt->getSuccessor();
			continue;
		}

		// Global variables may be changed in the called function.
		// If any global is used between the original statement and the
		// target statement, skip the optimization.
		for (auto i = firstUseStmtData->dir_all_begin(),
				e = firstUseStmtData->dir_all_end(); i != e; ++i) {
			if (hasItem(globalVars, *i)) {
				return;
			}
		}

		firstUseStmt = firstUseStmt->getSuccessor();
	}

	// The statement where lhsVar is used after stmt has to exist.
	if (!firstUseStmt) {
		return;
	}

	// This variable has to be used precisely once in there.
	if (firstUseStmtData->getDirNumOfUses(lhsVar) != 1) {
		return;
	}

	// There should not be any dereferences or other constructs that may cause
	// problems.
	if (firstUseStmtData->hasAddressOps() || firstUseStmtData->hasDerefs() ||
			firstUseStmtData->hasArrayAccesses() ||
			firstUseStmtData->hasStructAccesses()) {
		return;
	}

	// If there is a call, make sure that the statement is either of the form
	//
	//     return call(a, b, c, ...)
	//
	// or
	//
	//     x = call(a, b, c, ...)
	//
	// or
	//
	//     call(a, b, c, ...)
	//
	// where a, b, c, ... are expressions that use only local variables and do
	// not contain any function calls.
	if (firstUseStmtData->hasCalls()) {
		if (firstUseStmtData->getNumOfCalls() != 1) {
			return;
		}

		if (ShPtr<ReturnStmt> returnStmt = cast<ReturnStmt>(firstUseStmt)) {
			if (!isa<CallExpr>(returnStmt->getRetVal())) {
				return;
			}
		} else if (ShPtr<AssignStmt> assignStmt = cast<AssignStmt>(firstUseStmt)) {
			if (!isa<CallExpr>(assignStmt->getRhs())) {
				return;
			}
		} else if (!isa<CallStmt>(firstUseStmt)) {
			return;
		}

		for (auto i = firstUseStmtData->dir_read_begin(),
				e = firstUseStmtData->dir_read_end(); i != e; ++i) {
			if (hasItem(globalVars, *i)) {
				return;
			}
		}
	}

	// The next statement cannot be a while or for loop. Otherwise, we would
	// optimize
	//
	//     a = rand()
	//     while (a) {
	//         // ...
	//     }
	//
	// to
	//
	//     while (rand()) {
	//         // ...
	//     }
	//
	// which may not be correct.
	if (isLoop(firstUseStmt)) {
		return;
	}

	// Check that the two uses in stmt and firstUseStmt are the only uses of
	// lhsVar, with the exception of an optional variable-defining statement.
	ShPtr<VarDefStmt> lhsDefStmt;
	ShPtr<VarUses> allLhsUses(vuv->getUses(lhsVar, currFunc));
	for (const auto &dirUse : allLhsUses->dirUses) {
		if (dirUse == stmt || dirUse == firstUseStmt) {
			continue;
		}

		lhsDefStmt = cast<VarDefStmt>(dirUse);
		if (!lhsDefStmt || lhsDefStmt->getVar() != lhsVar ||
				lhsDefStmt->getInitializer()) {
			return;
		}
	}

	// Do the optimization.
	replaceVarWithExprInStmt(lhsVar, rhs, firstUseStmt);
	va->removeFromCache(firstUseStmt);
	Statement::removeStatementButKeepDebugComment(stmt);
	currCFG->removeStmt(stmt);
	if (lhsDefStmt) {
		removeVarDefOrAssignStatement(lhsDefStmt, currFunc);
		currCFG->removeStmt(lhsDefStmt);
	}
}

} // namespace llvmir2hll
} // namespace retdec
