/**
 * @file tests/llvmir2hll/optimizer/optimizer_manager_pipeline_tests.cpp
 * @brief The whole llvmir2hll pipeline, from LLVM IR to optimised BIR.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 *
 * Every other test here exercises one pass on a module built by hand. That
 * leaves the thing the decompiler actually does -- convert, then run
 * thirty-odd passes in order, each on the output of the last -- covered by
 * nothing but the corpus, which needs the front end and a CI round trip.
 *
 * CC-01 reported `label 'lab_0x112c' used but not defined' on
 * generated_shell_sort-gcc-O2 for four rounds. Reading pass sources found
 * four real defects and fixed them; the error did not move. This is the
 * harness that can answer the question directly: convert LLVM IR, run the
 * pipeline, and ask whether every goto still has a target the emitter
 * reaches. With `disabledOpts` it also names the pass, by bisection.
 */

#include <gtest/gtest.h>

#include <llvm/Support/raw_ostream.h>

#include "llvmir2hll/llvm/llvmir2bir_converter_tests/base_tests.h"
#include "retdec/llvmir2hll/analysis/alias_analysis/alias_analyses/simple_alias_analysis.h"
#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/evaluator/arithm_expr_evaluators/strict_arithm_expr_evaluator.h"
#include "retdec/llvmir2hll/hll/hll_writers/c_hll_writer.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/obtainer/call_info_obtainers/pessim_call_info_obtainer.h"
#include "retdec/llvmir2hll/optimizer/optimizer_manager.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/llvmir2hll/support/types.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"

#include <regex>
#include <set>
#include <sstream>

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

namespace {

/// Every statement the emitter walks from @a start.
class ReachableStmtCollector : private OrderedAllVisitor {
public:
	static StmtUSet collect(ShPtr<Statement> start)
	{
		ReachableStmtCollector c;
		c.visitStmt(start);
		return c.seen;
	}

private:
	void visitStmt(ShPtr<Statement> stmt, bool visitSuccessors = true, bool visitNestedStmts = true) override
	{
		if (stmt)
		{
			seen.insert(stmt);
		}
		OrderedAllVisitor::visitStmt(stmt, visitSuccessors, visitNestedStmts);
	}

	StmtUSet seen;
};

} // namespace

class OptimizerManagerPipelineTests : public LLVMIR2BIRConverterBaseTests {
protected:
	/// Runs the optimizer pipeline over @a m, with @a disabled turned off.
	void runPipeline(ShPtr<Module> m, const StringSet& disabled = {})
	{
		std::string sink;
		llvm::raw_string_ostream sinkStream(sink);
		auto aa = SimpleAliasAnalysis::create();
		aa->init(m);
		OptimizerManager om(
			StringSet(),
			disabled,
			CHLLWriter::create(sinkStream),
			ValueAnalysis::create(aa, true),
			PessimCallInfoObtainer::create(),
			StrictArithmExprEvaluator::create(),
			false);
		om.optimize(m);
	}

	/// The C the writer produces for @a m, after the pipeline.
	static std::string emitC(ShPtr<Module> m)
	{
		std::string code;
		llvm::raw_string_ostream stream(code);
		auto writer = CHLLWriter::create(stream);
		writer->emitTargetCode(m);
		stream.flush();
		return code;
	}

	/// Every label the emitted C jumps to but never defines.
	///
	/// This is CC-01's question -- `label 'lab_0x112c' used but not defined'
	/// is a C compiler asking exactly this -- answered without a front end, a
	/// corpus binary or a CI round trip. A label is function-scoped in C, so
	/// the scan resets at each closing brace in column one, which is where
	/// CHLLWriter ends a function.
	static std::set<std::string> undefinedLabels(const std::string& code)
	{
		std::set<std::string> missing;
		std::set<std::string> jumpedTo;
		std::set<std::string> defined;

		const std::regex gotoRe(R"(\bgoto\s+([A-Za-z_]\w*)\s*;)");
		const std::regex labelRe(R"(^\s*([A-Za-z_]\w*)\s*:(?!:))");

		const auto closeFunction = [&]() {
			for (const auto& l: jumpedTo)
				if (!defined.count(l)) missing.insert(l);
			jumpedTo.clear();
			defined.clear();
		};

		std::istringstream in(code);
		for (std::string line; std::getline(in, line);)
		{
			std::smatch m;
			if (std::regex_search(line, m, gotoRe))
				jumpedTo.insert(m[1]);
			else if (std::regex_search(line, m, labelRe))
				defined.insert(m[1]);
			if (line == "}") closeFunction();
		}
		closeFunction();
		return missing;
	}

	/// The names of every function with a goto the emitter cannot resolve.
	static StringSet functionsWithAStrandedGoto(ShPtr<Module> m)
	{
		StringSet bad;
		for (auto i = m->func_definition_begin(); i != m->func_definition_end(); ++i)
		{
			auto reachable = ReachableStmtCollector::collect((*i)->getBody());
			for (const auto& stmt: reachable)
			{
				auto g = cast<GotoStmt>(stmt);
				if (g && !reachable.count(g->getTarget()))
				{
					bad.insert((*i)->getName());
				}
			}
		}
		return bad;
	}
};

TEST_F(OptimizerManagerPipelineTests, ANestedMultiExitLoopKeepsItsGotoResolvable)
{
	auto module = convertLLVMIR2BIR(R"(
		declare void @test(i32)

		define void @function(i32 %n) {
		entry:
			br label %outer
		outer:
			%i = phi i32 [ 0, %entry ], [ %inext, %outerlatch ]
			br label %inner
		inner:
			%j = phi i32 [ 0, %outer ], [ %jnext, %innerlatch ]
			%c1 = icmp slt i32 %j, %n
			br i1 %c1, label %innerbody, label %innerexit
		innerbody:
			%c2 = icmp eq i32 %j, 5
			br i1 %c2, label %join, label %innerlatch
		innerlatch:
			%jnext = add i32 %j, 1
			call void @test(i32 7)
			br label %inner
		innerexit:
			call void @test(i32 1)
			br label %join
		join:
			call void @test(i32 2)
			%c3 = icmp eq i32 %i, 9
			br i1 %c3, label %out, label %outerlatch
		outerlatch:
			%inext = add i32 %i, 1
			br label %outer
		out:
			ret void
		}
	)");
	ASSERT_TRUE(module);

	runPipeline(module);

	EXPECT_TRUE(functionsWithAStrandedGoto(module).empty());
}


// The shape generated_shell_sort-gcc-O2 has, read off the emitted C: an outer
// `while (true)` whose body starts with an `if` that continues the loop, an
// inner loop with two exits, a phi at the join those exits meet, and the join
// deciding whether to break out of the outer loop.
TEST_F(OptimizerManagerPipelineTests, TheShellSortShapeKeepsItsGotoResolvable)
{
	auto module = convertLLVMIR2BIR(R"(
		declare void @test(i32)

		define i32 @function(i32 %n, i32* %p) {
		entry:
			br label %outer
		outer:
			%v8 = phi i32 [ 3, %entry ], [ %v8next, %outerlatch ]
			%v9 = phi i32 [ 0, %entry ], [ %v9next, %outerlatch ]
			%c0 = icmp slt i32 %v8, %n
			br i1 %c0, label %bump, label %inner.pre
		bump:
			%v8bump = add i32 %v8, 1
			br label %outerlatch
		inner.pre:
			%v12 = load i32, i32* %p
			br label %inner
		inner:
			%v13 = phi i32 [ %v8, %inner.pre ], [ %v13next, %innerlatch ]
			%v17 = phi i32 [ %n, %inner.pre ], [ %v17next, %innerlatch ]
			%ci = icmp slt i32 %v12, %v17
			br i1 %ci, label %body, label %normalexit
		body:
			%v13next = sub i32 %v13, %n
			store i32 %v17, i32* %p
			%ce = icmp slt i32 %v13next, %n
			br i1 %ce, label %join, label %innerlatch
		innerlatch:
			%v17next = load i32, i32* %p
			br label %inner
		normalexit:
			%v19 = add i32 %v8, 1
			br label %join
		join:
			%v18 = phi i32 [ %v13next, %body ], [ %v19, %normalexit ]
			store i32 %v18, i32* %p
			%c6 = icmp eq i32 %v18, 6
			br i1 %c6, label %out, label %outerlatch
		outerlatch:
			%v8next = phi i32 [ %v8bump, %bump ], [ %v18, %join ]
			%v9next = add i32 %v9, 4
			br label %outer
		out:
			call void @test(i32 2)
			ret i32 %v9
		}
	)");
	ASSERT_TRUE(module);

	runPipeline(module);

	EXPECT_TRUE(functionsWithAStrandedGoto(module).empty());
}

// An irreducible region: two blocks that jump into each other's middle. This
// is the case retdec cannot structure at all, so it is the one that leans
// hardest on the goto machinery.
TEST_F(OptimizerManagerPipelineTests, AnIrreducibleRegionKeepsItsGotosResolvable)
{
	auto module = convertLLVMIR2BIR(R"(
		declare void @test(i32)

		define void @function(i32 %n) {
		entry:
			%c = icmp slt i32 %n, 0
			br i1 %c, label %a, label %b
		a:
			call void @test(i32 1)
			%ca = icmp slt i32 %n, 10
			br i1 %ca, label %b, label %done
		b:
			call void @test(i32 2)
			%cb = icmp slt i32 %n, 20
			br i1 %cb, label %a, label %done
		done:
			call void @test(i32 3)
			ret void
		}
	)");
	ASSERT_TRUE(module);

	runPipeline(module);

	EXPECT_TRUE(functionsWithAStrandedGoto(module).empty());
}


// The emitted C itself, which is the thing CC-01 hands to a compiler. The BIR
// invariant above is the same question one step earlier; this one catches a
// label the tree holds but the writer does not write.
TEST_F(OptimizerManagerPipelineTests, TheEmittedCDefinesEveryLabelItJumpsTo)
{
	auto module = convertLLVMIR2BIR(R"(
		declare void @test(i32)

		define void @function(i32 %n) {
		entry:
			br label %outer
		outer:
			%i = phi i32 [ 0, %entry ], [ %inext, %outerlatch ]
			br label %inner
		inner:
			%j = phi i32 [ 0, %outer ], [ %jnext, %innerlatch ]
			%c1 = icmp slt i32 %j, %n
			br i1 %c1, label %innerbody, label %innerexit
		innerbody:
			%c2 = icmp eq i32 %j, 5
			br i1 %c2, label %join, label %innerlatch
		innerlatch:
			%jnext = add i32 %j, 1
			call void @test(i32 7)
			br label %inner
		innerexit:
			call void @test(i32 1)
			br label %join
		join:
			call void @test(i32 2)
			%c3 = icmp eq i32 %i, 9
			br i1 %c3, label %out, label %outerlatch
		outerlatch:
			%inext = add i32 %i, 1
			br label %outer
		out:
			ret void
		}
	)");
	ASSERT_TRUE(module);

	runPipeline(module);

	const auto code = emitC(module);
	const auto missing = undefinedLabels(code);
	EXPECT_TRUE(missing.empty()) << "the emitted C jumps to " << (missing.empty() ? "" : *missing.begin())
								 << " and never defines it:\n"
								 << code;
}

TEST_F(OptimizerManagerPipelineTests, TheUndefinedLabelScanFindsOne)
{
	// The scan is the assertion in the test above, so it needs its own check:
	// one that cannot find a missing label passes on everything.
	const std::string bad = "void f(void) {\n    goto lab_1;\n    return;\n}\n";
	EXPECT_EQ(std::set<std::string>{"lab_1"}, undefinedLabels(bad));

	const std::string good = "void f(void) {\n    goto lab_1;\n  lab_1:\n    return;\n}\n";
	EXPECT_TRUE(undefinedLabels(good).empty());

	// And a label is function-scoped: defining it in another function is not
	// defining it here.
	const std::string split =
		"void f(void) {\n    goto lab_1;\n}\n"
		"void g(void) {\n  lab_1:\n    return;\n}\n";
	EXPECT_EQ(std::set<std::string>{"lab_1"}, undefinedLabels(split));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
