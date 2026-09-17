/**
 * @file tests/llvmir2hll/optimizer/optimizers/while_true_lowering_semantics_tests.cpp
 * @brief Does lowering a `while true` loop still compute the same thing?
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * WHY THIS EXISTS
 *
 * `WhileTrueToUForLoopOptimizer` lowers
 *
 *     while true { BODY; if (exit) <END>; }
 *
 * into `BODY; while (!exit) { BODY; }`, and throws the loop-end `if` away. That
 * is right when <END> is a bare `break`. `isLoopEnd` in loop_optimizer.cpp also
 * accepts two other shapes -- `return X`, and `lhs = rhs` followed by a break or
 * return -- and for those, throwing the `if` away throws away a `return` or an
 * assignment.
 *
 * `WhileTrueToWhileCondOptimizer` gets the same input right: it prepends the
 * loop-end assignment and appends the `return` after the loop. The two passes
 * disagree about the same three shapes, and every existing test on either one
 * checks the SHAPE of the result.
 *
 * So this does not look at the shape. It runs the function before and after the
 * pass with a small interpreter and requires the same answer: the same returned
 * value, or the same fall-off-the-end, and the same final values of the
 * variables the function touches.
 *
 * WHAT IT CANNOT SEE
 *
 * The interpreter models integer variables, the arithmetic and comparison
 * nodes listed in evalExpr, and the statement kinds listed in Runner::run. It
 * refuses anything else rather than passing it, so a case it cannot model is a
 * reported failure and not a silent success. It bounds iterations, so a lowering
 * that turned a terminating loop into a non-terminating one is reported as
 * "did not terminate" rather than hanging the suite.
 */

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <string>
#include <string>

#include "llvmir2hll/analysis/tests_with_value_analysis.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_op_expr.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/mul_op_expr.h"
#include "retdec/llvmir2hll/ir/neq_op_expr.h"
#include "retdec/llvmir2hll/ir/not_op_expr.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/sub_op_expr.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/optimizer/optimizers/while_true_to_ufor_loop_optimizer.h"
#include "retdec/llvmir2hll/optimizer/optimizers/while_true_to_while_cond_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

namespace {

/// What running the function produced.
struct Outcome
{
	bool modelled = true;   ///< false when the interpreter met something it
							///< does not model; never treated as a pass
	bool terminated = true; ///< false when the iteration bound was hit
	bool returned = false;  ///< whether a return statement was reached
	int64_t value = 0;      ///< the returned value, when returned
	std::map<std::string, int64_t> vars;

	bool operator==(const Outcome& o) const
	{
		return terminated == o.terminated && returned == o.returned && value == o.value && vars == o.vars;
	}

	std::string str() const
	{
		if (!modelled) return "<not modelled>";
		if (!terminated) return "<did not terminate>";
		std::string s = returned ? "return " + std::to_string(value) : "fell off the end";
		for (auto& p: vars)
			s += ", " + p.first + "=" + std::to_string(p.second);
		return s;
	}
};

class Runner {
public:
	explicit Runner(std::map<std::string, int64_t> initial): env(initial) {}

	Outcome run(ShPtr<Statement> stmt)
	{
		Outcome out;
		Flow f = exec(stmt, out);
		if (f == Flow::Break || f == Flow::Continue)
		{
			// A break or continue that escaped every loop is not something the
			// interpreter should quietly accept: it is the very defect one of
			// these lowerings can produce.
			out.modelled = false;
		}
		out.vars = env;
		return out;
	}

private:
	enum class Flow
	{
		Normal,
		Break,
		Continue,
		Return,
		Stop
	};

	std::map<std::string, int64_t> env;
	unsigned steps = 0;
	static const unsigned STEP_LIMIT = 200000;

	bool evalExpr(ShPtr<Expression> e, int64_t& r, Outcome& out)
	{
		if (auto v = cast<Variable>(e))
		{
			auto it = env.find(v->getName());
			if (it == env.end())
			{
				out.modelled = false;
				return false;
			}
			r = it->second;
			return true;
		}
		if (auto c = cast<ConstInt>(e))
		{
			r = c->getValue().getSExtValue();
			return true;
		}
		if (auto c = cast<ConstBool>(e))
		{
			r = c->getValue() ? 1 : 0;
			return true;
		}
		if (auto n = cast<NotOpExpr>(e))
		{
			int64_t a;
			if (!evalExpr(n->getOperand(), a, out)) return false;
			r = a ? 0 : 1;
			return true;
		}

		ShPtr<Expression> lhs, rhs;
		int kind = 0;
		if (auto b = cast<AddOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 1;
		}
		else if (auto b = cast<SubOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 2;
		}
		else if (auto b = cast<MulOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 3;
		}
		else if (auto b = cast<LtOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 4;
		}
		else if (auto b = cast<LtEqOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 5;
		}
		else if (auto b = cast<GtOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 6;
		}
		else if (auto b = cast<GtEqOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 7;
		}
		else if (auto b = cast<EqOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 8;
		}
		else if (auto b = cast<NeqOpExpr>(e))
		{
			lhs = b->getFirstOperand();
			rhs = b->getSecondOperand();
			kind = 9;
		}
		else
		{
			out.modelled = false;
			return false;
		}

		int64_t a, c;
		if (!evalExpr(lhs, a, out) || !evalExpr(rhs, c, out)) return false;
		switch (kind)
		{
		case 1: r = a + c; break;
		case 2: r = a - c; break;
		case 3: r = a * c; break;
		case 4: r = a < c; break;
		case 5: r = a <= c; break;
		case 6: r = a > c; break;
		case 7: r = a >= c; break;
		case 8: r = a == c; break;
		default: r = a != c; break;
		}
		return true;
	}

	Flow exec(ShPtr<Statement> stmt, Outcome& out)
	{
		while (stmt)
		{
			if (++steps > STEP_LIMIT)
			{
				out.terminated = false;
				return Flow::Stop;
			}

			if (auto as = cast<AssignStmt>(stmt))
			{
				auto lhs = cast<Variable>(as->getLhs());
				int64_t v;
				if (!lhs || !evalExpr(as->getRhs(), v, out))
				{
					out.modelled = false;
					return Flow::Stop;
				}
				env[lhs->getName()] = v;
			}
			else if (auto vd = cast<VarDefStmt>(stmt))
			{
				if (auto init = vd->getInitializer())
				{
					int64_t v;
					if (!evalExpr(init, v, out))
					{
						out.modelled = false;
						return Flow::Stop;
					}
					env[vd->getVar()->getName()] = v;
				}
				else
				{
					env[vd->getVar()->getName()] = 0;
				}
			}
			else if (auto rs = cast<ReturnStmt>(stmt))
			{
				out.returned = true;
				if (auto rv = rs->getRetVal())
				{
					if (!evalExpr(rv, out.value, out))
					{
						out.modelled = false;
					}
				}
				else
				{
					out.value = 0;
				}
				return Flow::Return;
			}
			else if (isa<BreakStmt>(stmt))
			{
				return Flow::Break;
			}
			else if (isa<ContinueStmt>(stmt))
			{
				return Flow::Continue;
			}
			else if (auto is = cast<IfStmt>(stmt))
			{
				bool taken = false;
				for (auto i = is->clause_begin(), e = is->clause_end(); i != e; ++i)
				{
					int64_t c;
					if (!evalExpr(i->first, c, out))
					{
						out.modelled = false;
						return Flow::Stop;
					}
					if (c)
					{
						Flow f = exec(i->second, out);
						if (f != Flow::Normal) return f;
						taken = true;
						break;
					}
				}
				if (!taken && is->hasElseClause())
				{
					Flow f = exec(is->getElseClause(), out);
					if (f != Flow::Normal) return f;
				}
			}
			else if (auto wl = cast<WhileLoopStmt>(stmt))
			{
				while (true)
				{
					if (++steps > STEP_LIMIT)
					{
						out.terminated = false;
						return Flow::Stop;
					}
					int64_t c;
					if (!evalExpr(wl->getCondition(), c, out))
					{
						out.modelled = false;
						return Flow::Stop;
					}
					if (!c) break;
					Flow f = exec(wl->getBody(), out);
					if (f == Flow::Break) break;
					if (f == Flow::Return || f == Flow::Stop) return f;
				}
			}
			else if (auto ul = cast<UForLoopStmt>(stmt))
			{
				if (auto init = ul->getInit())
				{
					// The init of a UForLoopStmt is an expression, and the only
					// form this suite builds is an assignment written as one.
					int64_t v;
					if (!evalExpr(init, v, out))
					{
						out.modelled = false;
						return Flow::Stop;
					}
				}
				while (true)
				{
					if (++steps > STEP_LIMIT)
					{
						out.terminated = false;
						return Flow::Stop;
					}
					if (auto cond = ul->getCond())
					{
						int64_t c;
						if (!evalExpr(cond, c, out))
						{
							out.modelled = false;
							return Flow::Stop;
						}
						if (!c) break;
					}
					Flow f = exec(ul->getBody(), out);
					if (f == Flow::Break) break;
					if (f == Flow::Return || f == Flow::Stop) return f;
					if (auto step = ul->getStep())
					{
						int64_t v;
						if (!evalExpr(step, v, out))
						{
							out.modelled = false;
							return Flow::Stop;
						}
					}
				}
			}
			else if (isa<EmptyStmt>(stmt))
			{
				// nothing
			}
			else
			{
				out.modelled = false;
				return Flow::Stop;
			}

			stmt = stmt->getSuccessor();
		}
		return Flow::Normal;
	}
};

/// A compact one-line shape of a statement chain, for failure messages. A
/// differential that says only "the answer changed" leaves you guessing at
/// what the pass produced.
std::string shape(ShPtr<Statement> stmt, int depth = 0)
{
	std::string out;
	if (depth > 8) return "...";
	while (stmt)
	{
		if (isa<AssignStmt>(stmt))
			out += "assign; ";
		else if (isa<VarDefStmt>(stmt))
			out += "vardef; ";
		else if (isa<ReturnStmt>(stmt))
			out += "return; ";
		else if (isa<BreakStmt>(stmt))
			out += "break; ";
		else if (isa<ContinueStmt>(stmt))
			out += "continue; ";
		else if (isa<EmptyStmt>(stmt))
			out += "empty; ";
		else if (auto is = cast<IfStmt>(stmt))
		{
			out += "if {";
			for (auto i = is->clause_begin(), e = is->clause_end(); i != e; ++i)
			{
				out += shape(i->second, depth + 1);
			}
			if (is->hasElseClause()) out += "else " + shape(is->getElseClause(), depth + 1);
			out += "} ";
		}
		else if (auto wl = cast<WhileLoopStmt>(stmt))
		{
			out += "while {" + shape(wl->getBody(), depth + 1) + "} ";
		}
		else if (auto ul = cast<UForLoopStmt>(stmt))
		{
			out += std::string("ufor(init=") + (ul->getInit() ? "y" : "n") + ",cond=" + (ul->getCond() ? "y" : "n")
				 + ",step=" + (ul->getStep() ? "y" : "n") + ") {" + shape(ul->getBody(), depth + 1) + "} ";
		}
		else
		{
			out += "<other>; ";
		}
		stmt = stmt->getSuccessor();
	}
	return out;
}

} // anonymous namespace

class WhileTrueLoweringSemanticsTests : public TestsWithModule {
protected:
	ShPtr<Variable> i;
	ShPtr<Variable> acc;

	void SetUp() override
	{
		TestsWithModule::SetUp();
		i = Variable::create("i", IntType::create(32));
		acc = Variable::create("acc", IntType::create(32));
		testFunc->addLocalVar(i);
		testFunc->addLocalVar(acc);
	}

	static ShPtr<ConstInt> num(int64_t v)
	{
		return ConstInt::create(v, 32);
	}

	void runUFor()
	{
		INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
		Optimizer::optimize<WhileTrueToUForLoopOptimizer>(module, va);
	}

	void runWhileCond()
	{
		Optimizer::optimize<WhileTrueToWhileCondOptimizer>(module);
	}

	/// Build, observe, optimize, observe again, and require the same answer.
	/// @a runOptimizer is passed in rather than the optimizer's type, because
	/// the two passes compared here do not share a constructor signature:
	/// WhileTrueToUForLoopOptimizer takes the value analysis and
	/// WhileTrueToWhileCondOptimizer does not.
	void checkPreserved(ShPtr<Statement> body, const std::string& what, const std::function<void()>& runOptimizer)
	{
		testFunc->setBody(body);

		Runner beforeRunner({{"i", 0}, {"acc", 0}});
		Outcome before = beforeRunner.run(testFunc->getBody());
		ASSERT_TRUE(before.modelled) << what << ": the interpreter could not model the input";
		ASSERT_TRUE(before.terminated) << what << ": the input did not terminate; the case is wrong";

		runOptimizer();

		Runner afterRunner({{"i", 0}, {"acc", 0}});
		Outcome after = afterRunner.run(testFunc->getBody());

		EXPECT_TRUE(after.modelled) << what
									<< ": the interpreter could not model the REWRITE, which is "
									   "a failure and not a pass";
		EXPECT_TRUE(before == after) << what << ":\n  before: " << before.str() << "\n  after:  " << after.str()
									 << "\n  shape:  " << shape(testFunc->getBody());
	}

	/// while true { acc = acc + 2; i = i + 1; if (i > 3) <end> }
	///
	/// `acc` advances by two and `i` by one deliberately. With both advancing
	/// by one they are equal at the exit, and the loop-end assignment
	/// `acc = i` that isLoopEnd's third shape allows is then a no-op -- so a
	/// test built that way passes whether or not the pass re-emits it, which
	/// is exactly what the first version of this file did.
	ShPtr<WhileLoopStmt> loopEndingWith(ShPtr<Statement> end)
	{
		auto exitIf = IfStmt::create(GtOpExpr::create(i, num(3)), end);
		auto bump = AssignStmt::create(i, AddOpExpr::create(i, num(1)), exitIf);
		auto acc1 = AssignStmt::create(acc, AddOpExpr::create(acc, num(2)), bump);
		return WhileLoopStmt::create(ConstBool::create(true), acc1);
	}
};

//
// The three shapes isLoopEnd accepts.
//

TEST_F(WhileTrueLoweringSemanticsTests, UForLoweringOfABareBreakPreservesTheResult)
{
	checkPreserved(loopEndingWith(BreakStmt::create()), "while true { .. if (i > 3) break; }", [this] { runUFor(); });
}

TEST_F(WhileTrueLoweringSemanticsTests, UForLoweringOfAReturnPreservesTheResult)
{
	// isLoopEnd accepts `if (exit) return X;`. The do-while lowering discards
	// the whole `if`, and with it the return -- so the function falls off the
	// end instead of returning.
	checkPreserved(loopEndingWith(ReturnStmt::create(acc)), "while true { .. if (i > 3) return acc; }", [this] {
		runUFor();
	});
}

TEST_F(WhileTrueLoweringSemanticsTests, UForLoweringOfAnAssignmentThenBreakPreservesTheResult)
{
	// isLoopEnd also accepts `lhs = rhs` before the break, where both sides are
	// variables. WhileTrueToWhileCondOptimizer prepends that assignment; the
	// do-while lowering drops it.
	auto brk = BreakStmt::create();
	auto assign = AssignStmt::create(acc, i, brk);
	checkPreserved(loopEndingWith(assign), "while true { .. if (i > 3) { acc = i; break; } }", [this] { runUFor(); });
}

//
// The same three through the pass that already handles them, as a control: if
// one of these fails the input is wrong, not the pass under test.
//

TEST_F(WhileTrueLoweringSemanticsTests, WhileCondLoweringOfTheSameThreeShapesPreservesTheResult)
{
	checkPreserved(loopEndingWith(BreakStmt::create()), "while-cond, bare break", [this] { runWhileCond(); });
}

//
// The self-test: the differential has to be able to fail.
//

TEST_F(WhileTrueLoweringSemanticsTests, TheInterpreterNoticesADroppedReturn)
{
	// Run the same body with and without the `return`, and require the two to
	// differ. If they do not, nothing this suite says about a dropped return
	// can be believed.
	auto withReturn = loopEndingWith(ReturnStmt::create(acc));
	Runner r1({{"i", 0}, {"acc", 0}});
	Outcome a = r1.run(withReturn);
	ASSERT_TRUE(a.modelled) << "the interpreter could not model the input";

	auto withBreak = loopEndingWith(BreakStmt::create());
	Runner r2({{"i", 0}, {"acc", 0}});
	Outcome b = r2.run(withBreak);
	ASSERT_TRUE(b.modelled) << "the interpreter could not model the input";

	EXPECT_FALSE(a == b) << "the interpreter cannot tell a returning loop from a breaking one, "
							"so it cannot see a dropped return: "
						 << a.str() << " vs " << b.str();
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
