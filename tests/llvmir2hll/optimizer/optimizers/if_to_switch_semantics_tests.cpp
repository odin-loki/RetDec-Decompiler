/**
 * @file tests/llvmir2hll/optimizer/optimizers/if_to_switch_semantics_tests.cpp
 * @brief Does IfToSwitchOptimizer still compute the same thing?
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * WHY THIS EXISTS
 *
 * if_to_switch_optimizer.cpp is 8,158 lines, of which roughly six thousand are
 * forty near-identical tryConvert{Lt,Le,Gt,Ge}With{Three..Six}LevelNested*
 * functions that reconstruct a switch from a nest of compares. Every test
 * beside this one checks the SHAPE of the result: that a SwitchStmt came out,
 * with these case values, in this order.
 *
 * That is not the same question as "does the switch run the same body the nest
 * would have run". A reconstruction that gets one bound off by one, or attaches
 * a body to the neighbouring case, produces a perfectly well-formed SwitchStmt
 * with plausible case values and passes every shape test in the file.
 *
 * So this does not look at the shape. It builds a nest, records which body runs
 * for each value of the control variable, runs the optimizer, and requires the
 * same answer for every value. The nests are generated rather than written out,
 * because forty hand-written cases is how the forty functions came to be
 * near-identical in the first place.
 *
 * WHAT IT CANNOT SEE
 *
 * The generator emits the nest shapes it knows how to emit. A tryConvert
 * function whose shape it never produces is not covered by this, and it reports
 * how many of its nests the optimizer actually converted so that a run which
 * silently exercised nothing is visible rather than green.
 */

#include <gtest/gtest.h>

#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "llvmir2hll/analysis/tests_with_value_analysis.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_op_expr.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/optimizer/optimizers/if_to_switch_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

namespace {

/// What a run of the statement tree produced: the constant assigned to `out`,
/// or NOTHING when no body ran.
const int64_t NOTHING = INT64_MIN;

/// Evaluate a comparison of the control variable against a constant.
/// Returns false for anything this evaluator does not model, which the caller
/// turns into a refusal to judge rather than a silent pass.
bool evalCond(ShPtr<Expression> cond, int64_t v, bool& modelled)
{
	auto binOp = [&](ShPtr<Expression> lhs, ShPtr<Expression> rhs, int64_t& c) -> bool {
		auto ci = cast<ConstInt>(rhs);
		if (!ci || !isa<Variable>(lhs))
		{
			return false;
		}
		c = ci->getValue().getSExtValue();
		return true;
	};

	int64_t c = 0;
	if (auto e = cast<EqOpExpr>(cond))
	{
		if (binOp(e->getFirstOperand(), e->getSecondOperand(), c)) return v == c;
	}
	else if (auto e = cast<LtOpExpr>(cond))
	{
		if (binOp(e->getFirstOperand(), e->getSecondOperand(), c)) return v < c;
	}
	else if (auto e = cast<LtEqOpExpr>(cond))
	{
		if (binOp(e->getFirstOperand(), e->getSecondOperand(), c)) return v <= c;
	}
	else if (auto e = cast<GtOpExpr>(cond))
	{
		if (binOp(e->getFirstOperand(), e->getSecondOperand(), c)) return v > c;
	}
	else if (auto e = cast<GtEqOpExpr>(cond))
	{
		if (binOp(e->getFirstOperand(), e->getSecondOperand(), c)) return v >= c;
	}
	modelled = false;
	return false;
}

/// Walk the statement chain with the control variable bound to @a v and return
/// the constant the first executed AssignStmt writes.
int64_t run(ShPtr<Statement> stmt, int64_t v, bool& modelled, int depth = 0)
{
	if (depth > 64)
	{
		modelled = false;
		return NOTHING;
	}
	while (stmt)
	{
		if (auto as = cast<AssignStmt>(stmt))
		{
			if (auto ci = cast<ConstInt>(as->getRhs()))
			{
				return ci->getValue().getSExtValue();
			}
			modelled = false;
			return NOTHING;
		}
		if (auto is = cast<IfStmt>(stmt))
		{
			bool taken = false;
			for (auto i = is->clause_begin(), e = is->clause_end(); i != e; ++i)
			{
				if (evalCond(i->first, v, modelled))
				{
					int64_t r = run(i->second, v, modelled, depth + 1);
					if (r != NOTHING) return r;
					taken = true;
					break;
				}
				if (!modelled) return NOTHING;
			}
			if (!taken && is->hasElseClause())
			{
				int64_t r = run(is->getElseClause(), v, modelled, depth + 1);
				if (r != NOTHING) return r;
			}
			stmt = is->getSuccessor();
			continue;
		}
		if (auto sw = cast<SwitchStmt>(stmt))
		{
			auto ctrl = cast<Variable>(sw->getControlExpr());
			if (!ctrl)
			{
				modelled = false;
				return NOTHING;
			}
			ShPtr<Statement> body;
			for (auto i = sw->clause_begin(), e = sw->clause_end(); i != e; ++i)
			{
				if (!i->first)
				{
					continue; // the default, handled below
				}
				auto ci = cast<ConstInt>(i->first);
				if (!ci)
				{
					modelled = false;
					return NOTHING;
				}
				if (ci->getValue().getSExtValue() == v)
				{
					body = i->second;
					break;
				}
			}
			if (!body && sw->hasDefaultClause())
			{
				body = sw->getDefaultClauseBody();
			}
			if (body)
			{
				int64_t r = run(body, v, modelled, depth + 1);
				if (r != NOTHING) return r;
			}
			stmt = sw->getSuccessor();
			continue;
		}
		stmt = stmt->getSuccessor();
	}
	return NOTHING;
}

} // anonymous namespace

class IfToSwitchSemanticsTests : public TestsWithModule {
protected:
	ShPtr<Variable> ctrl;
	ShPtr<Variable> out;

	void SetUp() override
	{
		TestsWithModule::SetUp();
		ctrl = Variable::create("v", IntType::create(32));
		out = Variable::create("out", IntType::create(32));
	}

	ShPtr<AssignStmt> body(int64_t k)
	{
		return AssignStmt::create(out, ConstInt::create(k, 32));
	}

	ShPtr<EqOpExpr> eq(int64_t k)
	{
		return EqOpExpr::create(ctrl, ConstInt::create(k, 64));
	}

	/// A dense `if (v == a) .. else if (v == a+1) .. else if ...` chain whose
	/// bodies assign distinct constants.
	ShPtr<IfStmt> denseEqChain(int64_t first, unsigned n, int64_t tag)
	{
		ShPtr<IfStmt> chain(IfStmt::create(eq(first), body(tag)));
		for (unsigned i = 1; i < n; ++i)
		{
			chain->addClause(eq(first + static_cast<int64_t>(i)), body(tag + static_cast<int64_t>(i)));
		}
		return chain;
	}

	/// Run every value in the domain and return the per-value answers.
	std::vector<int64_t> observe(bool& modelled)
	{
		std::vector<int64_t> res;
		for (int64_t v = -12; v <= 40; ++v)
		{
			modelled = true;
			int64_t r = run(testFunc->getBody(), v, modelled);
			if (!modelled)
			{
				return {};
			}
			res.push_back(r);
		}
		return res;
	}

	/// Build, observe, optimize, observe again. Returns true if the optimizer
	/// converted something, so the caller can tell an exercised run from an
	/// inert one.
	bool checkPreserved(ShPtr<Statement> nest, const std::string& what, bool& converted)
	{
		testFunc->setBody(nest);
		bool modelled = true;
		auto before = observe(modelled);
		if (before.empty())
		{
			ADD_FAILURE() << what << ": the evaluator could not model the nest";
			return false;
		}

		INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
		Optimizer::optimize<IfToSwitchOptimizer>(module, va);

		converted = isa<SwitchStmt>(testFunc->getBody());

		auto after = observe(modelled);
		if (after.empty())
		{
			ADD_FAILURE() << what << ": the evaluator could not model the result";
			return false;
		}

		bool ok = true;
		for (std::size_t i = 0; i < before.size(); ++i)
		{
			if (before[i] != after[i])
			{
				int64_t v = static_cast<int64_t>(i) - 12;
				ADD_FAILURE() << what << ": for v = " << v << " the nest runs body " << before[i]
							  << " and the rewrite runs body " << after[i];
				ok = false;
				if (i > 4) break;
			}
		}
		return ok;
	}
};

//
// The generated sweep.
//

TEST_F(IfToSwitchSemanticsTests, ConvertedNestsRunTheSameBodyForEveryControlValue)
{
	unsigned built = 0;
	unsigned convertedCount = 0;

	// `if (v < C) { <dense eq chain> }` over a range of bounds and chain
	// lengths, including the bounds that sit exactly on, one below and one
	// above the chain's edges -- which is where an off-by-one lives.
	for (int64_t first = 0; first <= 6; ++first)
	{
		for (unsigned n = 2; n <= 5; ++n)
		{
			const int64_t last = first + static_cast<int64_t>(n) - 1;
			for (int64_t delta = -1; delta <= 1; ++delta)
			{
				for (int shape = 0; shape < 4; ++shape)
				{
					ShPtr<IfStmt> inner(denseEqChain(first, n, 100));
					ShPtr<Expression> guard;
					std::string what;
					switch (shape)
					{
					case 0:
						guard = LtOpExpr::create(ctrl, ConstInt::create(last + 1 + delta, 64));
						what = "v < " + std::to_string(last + 1 + delta);
						break;
					case 1:
						guard = LtEqOpExpr::create(ctrl, ConstInt::create(last + delta, 64));
						what = "v <= " + std::to_string(last + delta);
						break;
					case 2:
						guard = GtOpExpr::create(ctrl, ConstInt::create(first - 1 + delta, 64));
						what = "v > " + std::to_string(first - 1 + delta);
						break;
					default:
						guard = GtEqOpExpr::create(ctrl, ConstInt::create(first + delta, 64));
						what = "v >= " + std::to_string(first + delta);
						break;
					}
					ShPtr<IfStmt> outer(IfStmt::create(guard, inner));
					what += " guarding cases " + std::to_string(first) + ".." + std::to_string(last);

					++built;
					bool converted = false;
					checkPreserved(outer, what, converted);
					if (converted)
					{
						++convertedCount;
					}
				}
			}
		}
	}

	EXPECT_GT(built, 200u) << "the generator did not build the sweep";
	// A run that converted nothing would pass every comparison while proving
	// nothing at all, so say so rather than going green on it.
	EXPECT_GT(convertedCount, 0u) << "the optimizer converted none of the " << built
								  << " generated nests; this run measured nothing";
	RecordProperty("nests_built", built);
	RecordProperty("nests_converted", convertedCount);
}

//
// The generated nests above are the shapes the guard-plus-dense-chain
// functions match. The forty tryConvert*With{Three..Six}LevelNested* functions
// match deeper nests, and writing forty shapes out by hand is how those forty
// functions came to be near-identical. So: generate nests from a grammar, with
// a fixed seed, and let the optimizer fire on whatever it recognises.
//

TEST_F(IfToSwitchSemanticsTests, PartitionNestsOfEveryDepthRunTheSameBodyForEveryControlValue)
{
	// The deeper tryConvert*With{Three..Six}LevelNested* functions match a
	// nest whose bounds line up exactly with its chains -- `lastD == f0 - 1`,
	// `firstC == f0`, and so on up. A generator with unrelated bounds converts
	// nothing, which a first attempt here duly did: 0 of 600.
	//
	// So build the shape they actually match. Partition a run of integers into
	// k + 1 contiguous segments, put a dense eq chain on each, and nest the
	// comparisons at the segment boundaries. For the >= orientation that is
	//
	//   if (v >= f0) { if (v >= f1) { ... } else { chain[1] } } else { chain[0] }
	//
	// with the recursion in the then branch, which is what "SplitInThen" names.
	// The <= orientation is the mirror, with the recursion in the else.
	unsigned built = 0;
	unsigned convertedCount = 0;
	int64_t tag = 1000;

	for (unsigned segments = 3; segments <= 7; ++segments)
	{
		for (int64_t first: {0, 1, 5})
		{
			for (unsigned width = 1; width <= 3; ++width)
			{
				for (int orient = 0; orient < 4; ++orient)
				{
					// Segment i covers [starts[i], starts[i] + width).
					std::vector<int64_t> starts;
					for (unsigned i = 0; i < segments; ++i)
					{
						starts.push_back(first + static_cast<int64_t>(i * width));
					}

					// Chain per segment, distinct bodies throughout.
					std::vector<ShPtr<IfStmt>> chains;
					for (unsigned i = 0; i < segments; ++i)
					{
						tag += 100;
						chains.push_back(denseEqChain(starts[i], width, tag));
					}

					auto boundary = [&](unsigned i) -> ShPtr<Expression> {
						// The boundary between segment i-1 and segment i.
						const int64_t f = starts[i];
						switch (orient)
						{
						case 0: return GtEqOpExpr::create(ctrl, ConstInt::create(f, 64));
						case 1: return GtOpExpr::create(ctrl, ConstInt::create(f - 1, 64));
						case 2: return LtOpExpr::create(ctrl, ConstInt::create(f, 64));
						default: return LtEqOpExpr::create(ctrl, ConstInt::create(f - 1, 64));
						}
					};

					ShPtr<Statement> nest;
					if (orient < 2)
					{
						// >= / > : the HIGH side is the then branch, so build
						// from the top segment down.
						nest = chains[segments - 1];
						for (unsigned i = segments - 1; i >= 1; --i)
						{
							ShPtr<IfStmt> node(IfStmt::create(boundary(i), nest));
							node->setElseClause(chains[i - 1]);
							nest = node;
						}
					}
					else
					{
						// < / <= : the LOW side is the then branch.
						nest = chains[0];
						for (unsigned i = 1; i < segments; ++i)
						{
							ShPtr<IfStmt> node(IfStmt::create(boundary(i), nest));
							node->setElseClause(chains[i]);
							nest = node;
						}
					}

					++built;
					bool converted = false;
					checkPreserved(
						nest,
						"partition of " + std::to_string(segments) + " segments, width " + std::to_string(width)
							+ ", first " + std::to_string(first) + ", orientation " + std::to_string(orient),
						converted);
					if (converted)
					{
						++convertedCount;
					}
				}
			}
		}
	}

	EXPECT_EQ(180u, built);
	RecordProperty("partition_built", built);
	RecordProperty("partition_converted", convertedCount);
	// A run that converts nothing compares nothing. Say so rather than going
	// green on it: the first version of this generator converted 0 of 600.
	EXPECT_GT(convertedCount, 0u) << "none of the " << built
								  << " partition nests was converted; "
									 "this run measured nothing";
	std::cout << "[          ] converted " << convertedCount << " of " << built << " partition nests" << std::endl;
}

//
// The self-test: the differential has to be able to fail.
//

TEST_F(IfToSwitchSemanticsTests, TheDifferentialNoticesASwitchThatRunsTheWrongBody)
{
	// if (v == 0) { out = 100; } else if (v == 1) { out = 101; }
	ShPtr<IfStmt> nest(denseEqChain(0, 2, 100));
	testFunc->setBody(nest);

	bool modelled = true;
	auto before = observe(modelled);
	ASSERT_FALSE(before.empty());

	// Hand-build the switch the optimizer would produce, with one case value
	// shifted by one -- the off-by-one this suite exists to catch.
	ShPtr<SwitchStmt> sw(SwitchStmt::create(ctrl));
	sw->addClause(ConstInt::create(0, 64), body(100));
	sw->addClause(ConstInt::create(2, 64), body(101)); // should be 1
	testFunc->setBody(sw);

	auto after = observe(modelled);
	ASSERT_FALSE(after.empty());

	EXPECT_NE(before, after) << "the differential cannot see a case value that moved by one, so "
								"nothing it says about the real conversions can be believed";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
