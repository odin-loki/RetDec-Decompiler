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
#include "retdec/llvmir2hll/ir/bit_and_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_or_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_shl_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_shr_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_xor_op_expr.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/deref_op_expr.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/mod_op_expr.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/obtainer/call_info_obtainers/pessim_call_info_obtainer.h"
#include "retdec/llvmir2hll/optimizer/optimizer_manager.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/llvmir2hll/support/types.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"

#include <cstdio>
#include <regex>
#include <set>
#include <vector>
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

/// Whether any `break` or `continue` below a statement has no loop or switch
/// around it.
///
/// OrderedAllVisitor already walks nested statements and successors; all this
/// adds is a depth counter incremented on the way into a loop or a switch, so
/// a BreakStmt seen at depth zero is one C will reject.
class StrandedBreakFinder : private OrderedAllVisitor {
public:
	static bool hasOne(ShPtr<Statement> start)
	{
		StrandedBreakFinder f;
		f.visitStmt(start);
		return f.found;
	}

private:
	template <typename T>
	void inLoop(ShPtr<T> stmt)
	{
		++depth;
		OrderedAllVisitor::visit(stmt);
		--depth;
	}

	void visit(ShPtr<WhileLoopStmt> stmt) override
	{
		inLoop(stmt);
	}
	void visit(ShPtr<ForLoopStmt> stmt) override
	{
		inLoop(stmt);
	}
	void visit(ShPtr<UForLoopStmt> stmt) override
	{
		inLoop(stmt);
	}
	void visit(ShPtr<SwitchStmt> stmt) override
	{
		inLoop(stmt);
	}

	void visit(ShPtr<BreakStmt> stmt) override
	{
		if (depth == 0)
		{
			found = true;
		}
		OrderedAllVisitor::visit(stmt);
	}

	void visit(ShPtr<ContinueStmt> stmt) override
	{
		if (depth == 0)
		{
			found = true;
		}
		OrderedAllVisitor::visit(stmt);
	}

	std::size_t depth = 0;
	bool found = false;
};

/// Whether any bitwise or modulo operator below a statement has a pointer for
/// an operand.
///
/// The third of CC-01's questions. C requires integer operands for `|`, `&`,
/// `^`, `<<`, `>>` and `%`; gcc says `invalid operands to binary | (have
/// 'void *' and 'int')`, which is what it said about generated_bloom_filter.
/// Like the other two scans this is a property of the BIR, so it needs no
/// front end.
class PointerBitwiseOperandFinder : private OrderedAllVisitor {
public:
	static bool hasOne(ShPtr<Statement> start)
	{
		PointerBitwiseOperandFinder f;
		f.visitStmt(start);
		return f.found;
	}

private:
	/// The type an operand actually has in C.
	///
	/// UnaryOpExpr::getType() returns its operand's type, and DerefOpExpr does
	/// not override it -- so a dereference reports the *pointer's* type rather
	/// than the pointee's, and asking `isa<PointerType>` about `*p` says yes
	/// for every well-typed `*p | x` in the tree. Unwrapping one dereference
	/// is what makes this a scan for the defect rather than for the shape.
	static ShPtr<Type> semanticTypeOf(ShPtr<Expression> expr)
	{
		if (auto deref = cast<DerefOpExpr>(expr))
		{
			if (auto ptrType = cast<PointerType>(deref->getOperand()->getType()))
			{
				return ptrType->getContainedType();
			}
		}
		return expr->getType();
	}

	void check(ShPtr<Expression> op)
	{
		if (op && isa<PointerType>(semanticTypeOf(op)))
		{
			found = true;
		}
	}

	template <typename T>
	void checkBoth(ShPtr<T> expr)
	{
		check(expr->getFirstOperand());
		check(expr->getSecondOperand());
		OrderedAllVisitor::visit(expr);
	}

	void visit(ShPtr<BitOrOpExpr> expr) override
	{
		checkBoth(expr);
	}
	void visit(ShPtr<BitAndOpExpr> expr) override
	{
		checkBoth(expr);
	}
	void visit(ShPtr<BitXorOpExpr> expr) override
	{
		checkBoth(expr);
	}
	void visit(ShPtr<BitShlOpExpr> expr) override
	{
		checkBoth(expr);
	}
	void visit(ShPtr<BitShrOpExpr> expr) override
	{
		checkBoth(expr);
	}
	void visit(ShPtr<ModOpExpr> expr) override
	{
		checkBoth(expr);
	}

	bool found = false;
};

} // namespace

class OptimizerManagerPipelineTests : public LLVMIR2BIRConverterBaseTests {
protected:
	/// Runs the optimizer pipeline over @a m, with @a disabled turned off.
	void runPipeline(ShPtr<Module> m, const StringSet& disabled = {}, const StringSet& enabled = {})
	{
		std::string sink;
		llvm::raw_string_ostream sinkStream(sink);
		auto aa = SimpleAliasAnalysis::create();
		aa->init(m);
		OptimizerManager om(
			enabled,
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

	/// Every label the emitted C defines twice in one function.
	///
	/// The other half of the label question, and the same kind of compile
	/// error: `duplicate label 'lab_x'`. preserveLabel() copies a label
	/// rather than moving it, so a pass that puts one statement's label on
	/// two of them leaves both emitted.
	static std::set<std::string> duplicateLabels(const std::string& code)
	{
		std::set<std::string> dupes;
		std::set<std::string> seen;
		const std::regex labelRe(R"(^\s*([A-Za-z_]\w*)\s*:(?!:))");

		std::istringstream in(code);
		for (std::string line; std::getline(in, line);)
		{
			std::smatch m;
			if (std::regex_search(line, m, labelRe) && !seen.insert(m[1]).second)
			{
				dupes.insert(m[1]);
			}
			if (line == "}")
			{
				seen.clear();
			}
		}
		return dupes;
	}

	/// Everything a C compiler would reject about @a code that this can see.
	static std::set<std::string> labelProblems(const std::string& code)
	{
		auto problems = undefinedLabels(code);
		for (const auto& d: duplicateLabels(code))
		{
			problems.insert("duplicate:" + d);
		}
		return problems;
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

	/// Every function holding a `break` or `continue` with nothing to break
	/// out of.
	///
	/// The other half of CC-01's question. `break statement not within loop or
	/// switch` is what a C compiler says about hash_table-gcc-O2, and it is a
	/// property of the BIR, not of the text: a BreakStmt is stranded exactly
	/// when no loop or switch encloses it. Walking down from each function
	/// body and carrying that one bit answers it without a front end.
	static StringSet functionsWithAStrandedBreak(ShPtr<Module> m)
	{
		StringSet bad;
		for (auto i = m->func_definition_begin(); i != m->func_definition_end(); ++i)
		{
			if (StrandedBreakFinder::hasOne((*i)->getBody()))
			{
				bad.insert((*i)->getName());
			}
		}
		return bad;
	}

	/// Every function applying a bitwise operator to a pointer.
	static StringSet functionsWithAPointerBitwiseOperand(ShPtr<Module> m)
	{
		StringSet bad;
		for (auto i = m->func_definition_begin(); i != m->func_definition_end(); ++i)
		{
			if (PointerBitwiseOperandFinder::hasOne((*i)->getBody()))
			{
				bad.insert((*i)->getName());
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
	const auto missing = labelProblems(code);
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

TEST_F(OptimizerManagerPipelineTests, TheDuplicateLabelScanFindsOne)
{
	// `duplicate label 'lab_1'` is the same kind of whole-file rejection as
	// an undefined one, so the scan for it needs the same treatment.
	const std::string bad = "void f(void) {\n  lab_1:\n    x();\n  lab_1:\n    return;\n}\n";
	EXPECT_EQ(std::set<std::string>{"lab_1"}, duplicateLabels(bad));

	const std::string good = "void f(void) {\n  lab_1:\n    return;\n}\n";
	EXPECT_TRUE(duplicateLabels(good).empty());

	// Function-scoped again: the same name in two functions is fine.
	const std::string twice =
		"void f(void) {\n  lab_1:\n    return;\n}\n"
		"void g(void) {\n  lab_1:\n    return;\n}\n";
	EXPECT_TRUE(duplicateLabels(twice).empty());
}


// The LLVM IR retdec's front end produced for generated_shell_sort-gcc-O2,
// kept by CC-01's --save-failures and reduced to the one function that fails:
// everything else in the module, the uselistorder block, and the `samesign`
// flag on one icmp, which the pinned LLVM emits and the system one cannot
// parse. Nothing else is changed.
constexpr const char* kShellSortFunction1080 = R"(source_filename = "test"
target datalayout = "e-m:e-p:64:64-i64:64-f80:128-n8:16:32:64-S128"
@global_var_3fb8 = global i64 0
@global_var_2010 = constant i64 34359738377
@global_var_2020 = constant i64 17179869189
@global_var_2004 = constant [4 x i8] c"%d \00"
@global_var_2008 = constant i64 10
@global_var_4010 = global i64 0

define i64 @function_1080() {
dec_label_pc_1080:
  %common.ret.op.reg2mem = alloca i64, align 8, !insn.addr !9
  %r12.0.reg2mem = alloca i64, align 8, !insn.addr !9
  %r14.0.be.reg2mem = alloca i64, align 8, !insn.addr !9
  %.pre-phi.reg2mem = alloca ptr, align 8, !insn.addr !9
  %rdx.0.reg2mem = alloca i64, align 8, !insn.addr !9
  %rcx.0.reg2mem = alloca i64, align 8, !insn.addr !9
  %rax.0.reg2mem = alloca i64, align 8, !insn.addr !9
  %rbx.0.reg2mem = alloca i64, align 8, !insn.addr !9
  %r14.0.reg2mem = alloca i64, align 8, !insn.addr !9
  %.reg2mem = alloca i1, align 1, !insn.addr !9
  %rdi.0.reg2mem = alloca i64, align 8, !insn.addr !9
  %stack_var_-48 = alloca i64, align 8
  %stack_var_-72 = alloca i64, align 8
  %0 = call i64 @__readfsqword(i64 40), !insn.addr !10
  store i64 %0, ptr %stack_var_-48, align 8, !retdec.pointee !1, !insn.addr !10
  %1 = ptrtoint ptr %stack_var_-72 to i64, !insn.addr !10
  store i64 3, ptr %rdi.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !11
  store i1 false, ptr %.reg2mem, align 1, !retdec.pointee !12, !insn.addr !11
  br label %dec_label_pc_10d0, !insn.addr !11

dec_label_pc_10d0:                                ; preds = %dec_label_pc_113d, %dec_label_pc_1080
  %.reload = load i1, ptr %.reg2mem, align 1, !retdec.pointee !12
  %rdi.0.reload = load i64, ptr %rdi.0.reg2mem, align 8, !retdec.pointee !1
  %sext = mul i64 %rdi.0.reload, 4
  %.neg = mul nsw i64 %rdi.0.reload, -4294967296
  %2 = add i64 %sext, %1, !insn.addr !11
  %3 = ashr exact i64 %.neg, 30
  store i64 %rdi.0.reload, ptr %r14.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !13
  store i64 %2, ptr %rbx.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !13
  br label %dec_label_pc_10f0, !insn.addr !13

dec_label_pc_10f0:                                ; preds = %dec_label_pc_10f0.backedge, %dec_label_pc_10d0
  %rbx.0.reload = load i64, ptr %rbx.0.reg2mem, align 8, !retdec.pointee !1
  %r14.0.reload = load i64, ptr %r14.0.reg2mem, align 8, !retdec.pointee !1
  %.not = icmp slt i64 %r14.0.reload, %rdi.0.reload, !insn.addr !14
  br i1 %.not, label %dec_label_pc_1155, label %dec_label_pc_111d.preheader, !insn.addr !14

dec_label_pc_111d.preheader:                      ; preds = %dec_label_pc_10f0
  %4 = inttoptr i64 %rbx.0.reload to ptr, !retdec.pointee !15, !insn.addr !13
  %5 = load i32, ptr %4, align 4, !retdec.pointee !15, !insn.addr !13
  %6 = sub i64 %rbx.0.reload, %sext, !insn.addr !13
  %7 = and i64 %r14.0.reload, 4294967295
  store i64 %6, ptr %rax.0.reg2mem, align 8, !retdec.pointee !1
  store i64 %rbx.0.reload, ptr %rcx.0.reg2mem, align 8, !retdec.pointee !1
  store i64 %7, ptr %rdx.0.reg2mem, align 8, !retdec.pointee !1
  br label %dec_label_pc_111d

dec_label_pc_1110:                                ; preds = %dec_label_pc_111d
  %rdx.0.reload = load i64, ptr %rdx.0.reg2mem, align 8, !retdec.pointee !1
  %8 = sub nsw i64 %rdx.0.reload, %rdi.0.reload
  %9 = and i64 %8, 4294967295
  %10 = inttoptr i64 %rcx.0.reload to ptr, !retdec.pointee !15, !insn.addr !16
  store i32 %15, ptr %10, align 4, !retdec.pointee !15, !insn.addr !16
  %11 = add i64 %rax.0.reload, %3, !insn.addr !16
  %12 = add i64 %rcx.0.reload, %3, !insn.addr !16
  %13 = icmp ult i64 %9, %rdi.0.reload, !insn.addr !17
  store i64 %11, ptr %rax.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !17
  store i64 %12, ptr %rcx.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !17
  store i64 %9, ptr %rdx.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !17
  store ptr %14, ptr %.pre-phi.reg2mem, align 8, !retdec.pointee !18, !insn.addr !17
  br i1 %13, label %dec_label_pc_112c, label %dec_label_pc_111d, !insn.addr !17

dec_label_pc_111d:                                ; preds = %dec_label_pc_111d.preheader, %dec_label_pc_1110
  %rcx.0.reload = load i64, ptr %rcx.0.reg2mem, align 8, !retdec.pointee !1
  %rax.0.reload = load i64, ptr %rax.0.reg2mem, align 8, !retdec.pointee !1
  %14 = inttoptr i64 %rax.0.reload to ptr, !retdec.pointee !15
  %15 = load i32, ptr %14, align 4, !retdec.pointee !15, !insn.addr !17
  %16 = icmp ult i32 %5, %15
  br i1 %16, label %dec_label_pc_1110, label %dec_label_pc_111d.dec_label_pc_112c_crit_edge, !insn.addr !19

dec_label_pc_111d.dec_label_pc_112c_crit_edge:    ; preds = %dec_label_pc_111d
  %.pre = inttoptr i64 %rcx.0.reload to ptr, !retdec.pointee !15, !insn.addr !19
  store ptr %.pre, ptr %.pre-phi.reg2mem, align 8, !retdec.pointee !18
  br label %dec_label_pc_112c

dec_label_pc_112c:                                ; preds = %dec_label_pc_1110, %dec_label_pc_111d.dec_label_pc_112c_crit_edge
  %.pre-phi.reload = load ptr, ptr %.pre-phi.reg2mem, align 8, !retdec.pointee !18
  %17 = add nsw i64 %r14.0.reload, 1
  %18 = and i64 %17, 4294967295
  %19 = bitcast ptr %.pre-phi.reload to ptr, !retdec.pointee !18
  store i32 %5, ptr %19, align 4, !retdec.pointee !15, !insn.addr !19
  %.not2 = icmp eq i64 %18, 6
  store i64 %18, ptr %r14.0.be.reg2mem, align 8, !retdec.pointee !1, !insn.addr !20
  br i1 %.not2, label %dec_label_pc_113d, label %dec_label_pc_10f0.backedge, !insn.addr !20

dec_label_pc_10f0.backedge:                       ; preds = %dec_label_pc_112c, %dec_label_pc_1155
  %r14.0.be.reload = load i64, ptr %r14.0.be.reg2mem, align 8, !retdec.pointee !1
  %rbx.0.be = add i64 %rbx.0.reload, 4
  store i64 %r14.0.be.reload, ptr %r14.0.reg2mem, align 8, !retdec.pointee !1
  store i64 %rbx.0.be, ptr %rbx.0.reg2mem, align 8, !retdec.pointee !1
  br label %dec_label_pc_10f0

dec_label_pc_113d:                                ; preds = %dec_label_pc_112c
  store i64 1, ptr %rdi.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !21
  store i1 true, ptr %.reg2mem, align 1, !retdec.pointee !12, !insn.addr !21
  br i1 %.reload, label %dec_label_pc_115f, label %dec_label_pc_10d0, !insn.addr !21

dec_label_pc_1155:                                ; preds = %dec_label_pc_10f0
  %20 = add nsw i64 %r14.0.reload, 1
  %21 = and i64 %20, 4294967295
  store i64 %21, ptr %r14.0.be.reg2mem, align 8, !retdec.pointee !1, !insn.addr !22
  br label %dec_label_pc_10f0.backedge, !insn.addr !22

dec_label_pc_115f:                                ; preds = %dec_label_pc_113d
  %22 = ptrtoint ptr %stack_var_-48 to i64, !insn.addr !22
  store i64 %1, ptr %r12.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !23
  br label %dec_label_pc_116b, !insn.addr !23

dec_label_pc_116b:                                ; preds = %dec_label_pc_116b, %dec_label_pc_115f
  %r12.0.reload = load i64, ptr %r12.0.reg2mem, align 8, !retdec.pointee !1
  %23 = inttoptr i64 %r12.0.reload to ptr, !retdec.pointee !15, !insn.addr !23
  %24 = load i32, ptr %23, align 4, !retdec.pointee !15, !insn.addr !23
  %25 = add i64 %r12.0.reload, 4, !insn.addr !23
  %26 = call i64 @__printf_chk(i64 2, i64 ptrtoint (ptr @global_var_2004 to i64), i32 %24), !insn.addr !24
  %.not3 = icmp eq i64 %25, %22
  store i64 %25, ptr %r12.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !25
  br i1 %.not3, label %dec_label_pc_1187, label %dec_label_pc_116b, !insn.addr !25

dec_label_pc_1187:                                ; preds = %dec_label_pc_116b
  %27 = call i64 @__printf_chk(i64 2, i64 ptrtoint (ptr @global_var_2008 to i64), i32 %24), !insn.addr !26
  %28 = load i64, ptr %stack_var_-48, align 8, !retdec.pointee !1, !insn.addr !26
  %29 = call i64 @__readfsqword(i64 40), !insn.addr !27
  %.not4 = icmp eq i64 %28, %29
  store i64 0, ptr %common.ret.op.reg2mem, align 8, !retdec.pointee !1, !insn.addr !28
  br i1 %.not4, label %common.ret, label %dec_label_pc_11b9, !insn.addr !28

common.ret:                                       ; preds = %dec_label_pc_1187, %dec_label_pc_11b9
  %common.ret.op.reload = load i64, ptr %common.ret.op.reg2mem, align 8, !retdec.pointee !1
  ret i64 %common.ret.op.reload, !insn.addr !29

dec_label_pc_11b9:                                ; preds = %dec_label_pc_1187
  %30 = call i64 @__stack_chk_fail(), !insn.addr !30
  store i64 %30, ptr %common.ret.op.reg2mem, align 8, !retdec.pointee !1
  br label %common.ret

}

declare i64 @__stack_chk_fail()
declare i64 @__printf_chk(i64, i64, i32)
declare i64 @__libc_start_main(i64, i64, ptr, i32, i32, i64, ptr, i64, i64)
declare i64 @__gmon_start__()
declare i64 @__cxa_finalize(i64)
declare i64 @__asm_hlt()
declare i64 @__readfsqword(i64)
declare i64 @__readUndefQword()

!0 = !{i64 4096}
!1 = !{!"i64"}
!2 = !{i64 4114}
!3 = !{i64 4116}
!4 = !{i64 4122}
!5 = !{i64 4134}
!6 = !{i64 4180}
!7 = !{i64 4196}
!8 = !{i64 4212}
!9 = !{i64 4224}
!10 = !{i64 4259}
!11 = !{i64 4294}
!12 = !{!"i1"}
!13 = !{i64 4331}
!14 = !{i64 4354}
!15 = !{!"i32"}
!16 = !{i64 4356}
!17 = !{i64 4379}
!18 = !{!"ptr"}
!19 = !{i64 4394}
!20 = !{i64 4411}
!21 = !{i64 4422}
!22 = !{i64 4445}
!23 = !{i64 4452}
!24 = !{i64 4477}
!25 = !{i64 4485}
!26 = !{i64 4501}
!27 = !{i64 4511}
!28 = !{i64 4520}
!29 = !{i64 4536}
!30 = !{i64 4537}
!31 = !{i64 4575}
!32 = !{i64 4581}
!33 = !{i64 4623}
!34 = !{i64 4688}
!35 = !{i64 4704}
!36 = !{i64 4715}
!37 = !{i64 4729}
!38 = !{i64 4738}
!39 = !{i64 4756}
!40 = !{i64 4743}
!41 = !{!"i8"}
!42 = !{i64 4772}
!43 = !{i64 4792}
)";

// The three passes the label was lost in, one at a time.
//
// Which pass it was took five rounds to answer by reading sources, and about
// two minutes once this harness existed: run the pipeline with `enabled` set
// to one pass, delta-debug down to the smallest set that still strands the
// label, fix, repeat. Each pass gets its own test because each has its own
// guard, and a test that runs all thirty would pass on two of three.
TEST_F(OptimizerManagerPipelineTests, WhileTrueToWhileCondAloneKeepsTheLabel)
{
	auto module = convertLLVMIR2BIR(kShellSortFunction1080);
	ASSERT_TRUE(module);
	runPipeline(module, StringSet(), StringSet{"WhileTrueToWhileCond"});
	EXPECT_TRUE(labelProblems(emitC(module)).empty());
}

TEST_F(OptimizerManagerPipelineTests, WhileTrueToUForLoopAloneKeepsTheLabel)
{
	auto module = convertLLVMIR2BIR(kShellSortFunction1080);
	ASSERT_TRUE(module);
	runPipeline(module, StringSet(), StringSet{"WhileTrueToUForLoop"});
	EXPECT_TRUE(labelProblems(emitC(module)).empty());
}

TEST_F(OptimizerManagerPipelineTests, WhileTrueToForLoopAloneKeepsTheLabel)
{
	auto module = convertLLVMIR2BIR(kShellSortFunction1080);
	ASSERT_TRUE(module);
	runPipeline(module, StringSet(), StringSet{"WhileTrueToForLoop"});
	EXPECT_TRUE(labelProblems(emitC(module)).empty());
}

TEST_F(OptimizerManagerPipelineTests, ShellSortFunction1080EmitsNoUndefinedLabel)
{
	auto module = convertLLVMIR2BIR(kShellSortFunction1080);
	ASSERT_TRUE(module);

	runPipeline(module);

	const auto code = emitC(module);
	const auto missing = labelProblems(code);
	EXPECT_TRUE(missing.empty()) << "the emitted C jumps to " << (missing.empty() ? std::string() : *missing.begin())
								 << " and never defines it:\n"
								 << code;
	EXPECT_TRUE(functionsWithAStrandedGoto(module).empty());
}


/// The function CC-01 reports `break statement not within loop or switch` on,
/// from hash_table-gcc-O2, reduced to itself.
///
/// The IR is one loop and nothing clever: 12b0 tests a bucket's count, 12c2
/// calls strcmp, 12d1 decrements a counter and branches back to 12b0, and
/// there are two exits -- 12e9 on a match and 12e0 when the counter runs out.
/// The C the back end emits for it puts 12b0's test, 12c2's call and the
/// `break` at function scope and leaves `while (true) { }` empty after them,
/// so the break has nothing to break out of.
///
/// Kept verbatim from the artifact scripts/ci/check_emitted_c_compiles.sh
/// --save-failures wrote in ctest-linux run 274, minus the other twelve
/// functions of that module.
constexpr const char* kHashTableFunction1270 = R"(source_filename = "test"
target datalayout = "e-m:e-p:64:64-i64:64-f80:128-n8:16:32:64-S128"

@global_var_3fa8 = global i64 0
@global_var_2004 = constant [6 x i8] c"alpha\00"
@global_var_200a = constant [5 x i8] c"beta\00"
@global_var_200f = constant [7 x i8] c"%d %d\0A\00"
@global_var_4010 = global i64 0

define i64 @function_1270(ptr %arg1, i64 %arg2) {
dec_label_pc_1270:
  %storemerge.reg2mem = alloca i64, align 8, !insn.addr !35
  %r12.0.reg2mem = alloca i32, align 4, !insn.addr !35
  %rbx.2.in.reg2mem = alloca i64, align 8, !insn.addr !35
  %rbx.1.reg2mem = alloca i64, align 8, !insn.addr !35
  %rbx.0.reg2mem = alloca i64, align 8, !insn.addr !35
  %rdx.0.reg2mem = alloca i64, align 8, !insn.addr !35
  %rax.0.reg2mem = alloca i64, align 8, !insn.addr !35
  %0 = urem i64 %arg2, 256
  %1 = icmp eq i64 %0, 0
  store i64 %0, ptr %rax.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !36
  store i64 %arg2, ptr %rdx.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !36
  store i64 2166136261, ptr %rbx.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !36
  store i64 2166136261, ptr %rbx.1.reg2mem, align 8, !retdec.pointee !1, !insn.addr !36
  br i1 %1, label %dec_label_pc_12a3, label %dec_label_pc_1290, !insn.addr !36

dec_label_pc_1290:                                ; preds = %dec_label_pc_1270, %dec_label_pc_1290
  %rbx.0.reload = load i64, ptr %rbx.0.reg2mem, align 8, !retdec.pointee !1
  %rdx.0.reload = load i64, ptr %rdx.0.reg2mem, align 8, !retdec.pointee !1
  %rax.0.reload = load i64, ptr %rax.0.reg2mem, align 8, !retdec.pointee !1
  %2 = add i64 %rdx.0.reload, 1, !insn.addr !37
  %3 = xor i64 %rbx.0.reload, %rax.0.reload
  %4 = inttoptr i64 %2 to ptr, !retdec.pointee !33, !insn.addr !37
  %5 = load i8, ptr %4, align 1, !retdec.pointee !33, !insn.addr !37
  %6 = zext i8 %5 to i64, !insn.addr !37
  %7 = mul nuw nsw i64 %3, 16777619
  %8 = and i64 %7, 4294967295
  %.not = icmp eq i8 %5, 0, !insn.addr !37
  store i64 %6, ptr %rax.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !38
  store i64 %2, ptr %rdx.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !38
  store i64 %8, ptr %rbx.0.reg2mem, align 8, !retdec.pointee !1, !insn.addr !38
  store i64 %8, ptr %rbx.1.reg2mem, align 8, !retdec.pointee !1, !insn.addr !38
  br i1 %.not, label %dec_label_pc_12a3, label %dec_label_pc_1290, !insn.addr !38

dec_label_pc_12a3:                                ; preds = %dec_label_pc_1290, %dec_label_pc_1270
  %9 = ptrtoint ptr %arg1 to i64
  %rbx.1.reload = load i64, ptr %rbx.1.reg2mem, align 8, !retdec.pointee !1
  store i64 %rbx.1.reload, ptr %rbx.2.in.reg2mem, align 8, !retdec.pointee !1, !insn.addr !39
  store i32 32, ptr %r12.0.reg2mem, align 4, !retdec.pointee !40, !insn.addr !39
  br label %dec_label_pc_12b0, !insn.addr !39

dec_label_pc_12b0:                                ; preds = %dec_label_pc_12d1, %dec_label_pc_12a3
  %r12.0.reload = load i32, ptr %r12.0.reg2mem, align 4, !retdec.pointee !40
  %rbx.2.in.reload = load i64, ptr %rbx.2.in.reg2mem, align 8, !retdec.pointee !1
  %rbx.2 = urem i64 %rbx.2.in.reload, 32
  %sr_shl1 = mul nuw nsw i64 %rbx.2, 24
  %10 = add i64 %sr_shl1, %9, !insn.addr !39
  %11 = add i64 %10, 20, !insn.addr !39
  %12 = inttoptr i64 %11 to ptr, !retdec.pointee !40, !insn.addr !39
  %13 = load i32, ptr %12, align 4, !retdec.pointee !40, !insn.addr !39
  %14 = icmp eq i32 %13, 0, !insn.addr !39
  br i1 %14, label %dec_label_pc_12d1, label %dec_label_pc_12c2, !insn.addr !41

dec_label_pc_12c2:                                ; preds = %dec_label_pc_12b0
  %15 = call i64 @strcmp(i64 %10, i64 %arg2), !insn.addr !42
  %16 = and i64 %15, 4294967295
  %17 = icmp eq i64 %16, 0
  br i1 %17, label %dec_label_pc_12e9, label %dec_label_pc_12d1, !insn.addr !43

dec_label_pc_12d1:                                ; preds = %dec_label_pc_12c2, %dec_label_pc_12b0
  %18 = add nuw nsw i64 %rbx.2, 1
  %19 = add i32 %r12.0.reload, -1
  %.not1 = icmp eq i32 %19, 0, !insn.addr !43
  store i64 %18, ptr %rbx.2.in.reg2mem, align 8, !retdec.pointee !1, !insn.addr !44
  store i32 %19, ptr %r12.0.reg2mem, align 4, !retdec.pointee !40, !insn.addr !44
  store i64 4294967295, ptr %storemerge.reg2mem, align 8, !retdec.pointee !1, !insn.addr !44
  br i1 %.not1, label %dec_label_pc_12e0, label %dec_label_pc_12b0, !insn.addr !44

dec_label_pc_12e0:                                ; preds = %dec_label_pc_12d1, %dec_label_pc_12e9
  %storemerge.reload = load i64, ptr %storemerge.reg2mem, align 8, !retdec.pointee !1
  ret i64 %storemerge.reload, !insn.addr !45

dec_label_pc_12e9:                                ; preds = %dec_label_pc_12c2
  %20 = add i64 %10, 16, !insn.addr !45
  %21 = inttoptr i64 %20 to ptr, !retdec.pointee !40, !insn.addr !45
  %22 = load i32, ptr %21, align 4, !retdec.pointee !40, !insn.addr !45
  %23 = zext i32 %22 to i64, !insn.addr !45
  store i64 %23, ptr %storemerge.reg2mem, align 8, !retdec.pointee !1, !insn.addr !46
  br label %dec_label_pc_12e0, !insn.addr !46

; uselistorder directives
  uselistorder i64 %arg2, { 2, 0, 1 }
  uselistorder ptr %storemerge.reg2mem, { 2, 0, 1 }
  uselistorder ptr %r12.0.reg2mem, { 1, 0, 2 }
  uselistorder ptr %rbx.2.in.reg2mem, { 1, 0, 2 }
  uselistorder ptr %rbx.0.reg2mem, { 2, 0, 1 }
  uselistorder ptr %rdx.0.reg2mem, { 2, 0, 1 }
  uselistorder ptr %rax.0.reg2mem, { 2, 0, 1 }
  uselistorder label %dec_label_pc_1290, { 1, 0 }
  uselistorder label %dec_label_pc_12e0, { 1, 0 }
}

!0 = !{i64 4096}
!1 = !{!"i64"}
!2 = !{i64 4114}
!3 = !{i64 4116}
!4 = !{i64 4122}
!5 = !{i64 4134}
!6 = !{i64 4212}
!7 = !{i64 4228}
!8 = !{i64 4244}
!9 = !{i64 4260}
!10 = !{i64 4276}
!11 = !{i64 4288}
!12 = !{i64 4330}
!13 = !{i64 4355}
!14 = !{i64 4361}
!15 = !{i64 4377}
!16 = !{i64 4388}
!17 = !{i64 4401}
!18 = !{i64 4424}
!19 = !{i64 4437}
!20 = !{i64 4446}
!21 = !{i64 4461}
!22 = !{i64 4462}
!23 = !{i64 4511}
!24 = !{i64 4517}
!25 = !{i64 4559}
!26 = !{i64 4624}
!27 = !{i64 4640}
!28 = !{i64 4651}
!29 = !{i64 4665}
!30 = !{i64 4674}
!31 = !{i64 4692}
!32 = !{i64 4679}
!33 = !{!"i8"}
!34 = !{i64 4708}
!35 = !{i64 4720}
!36 = !{i64 4739}
!37 = !{i64 4749}
!38 = !{i64 4769}
!39 = !{i64 4780}
!40 = !{!"i32"}
!41 = !{i64 4800}
!42 = !{i64 4808}
!43 = !{i64 4815}
!44 = !{i64 4827}
!45 = !{i64 4840}
!46 = !{i64 4845}
!47 = !{i64 4864}
!48 = !{i64 4892}
!49 = !{i64 4906}
!50 = !{i64 4929}
!51 = !{i64 4940}
!52 = !{i64 4950}
!53 = !{i64 4957}
!54 = !{i64 4969}
!55 = !{i64 4987}
!56 = !{i64 5000}
!57 = !{i64 5009}
!58 = !{i64 5031}
!59 = !{i64 5052}
declare i64 @strcmp(i64, i64))";

/// The scan is worth nothing if it cannot see a break that is really stranded,
/// so here is one built by hand: an IfStmt at function scope with a BreakStmt
/// inside it and no loop anywhere.
TEST_F(OptimizerManagerPipelineTests, TheStrandedBreakScanFindsOne)
{
	auto module = convertLLVMIR2BIR(R"(
		define i32 @f(i32 %x) {
		entry:
		  %c = icmp eq i32 %x, 0
		  br i1 %c, label %t, label %e
		t:
		  ret i32 1
		e:
		  ret i32 2
		}
	)");

	auto f = module->getFuncByName("f");
	ASSERT_TRUE(f);
	f->getBody()->setSuccessor(BreakStmt::create());

	EXPECT_FALSE(functionsWithAStrandedBreak(module).empty());
}

/// And nothing to report on a module whose only break is inside its loop.
TEST_F(OptimizerManagerPipelineTests, TheStrandedBreakScanPassesAWellFormedLoop)
{
	auto module = convertLLVMIR2BIR(R"(
		define i32 @f(i32 %n) {
		entry:
		  br label %loop
		loop:
		  %i = phi i32 [ 0, %entry ], [ %next, %body ]
		  %c = icmp slt i32 %i, %n
		  br i1 %c, label %body, label %done
		body:
		  %next = add i32 %i, 1
		  br label %loop
		done:
		  ret i32 %i
		}
	)");

	runPipeline(module);

	EXPECT_TRUE(functionsWithAStrandedBreak(module).empty());
}

/// The reduced function, through the whole pipeline. This is the assertion
/// CC-01 makes on the corpus, made here in a second rather than a CI round.
TEST_F(OptimizerManagerPipelineTests, HashTableFunction1270KeepsItsBreakInsideALoop)
{
	auto module = convertLLVMIR2BIR(kHashTableFunction1270);

	runPipeline(module);

	EXPECT_TRUE(functionsWithAStrandedBreak(module).empty()) << "a break was emitted with no loop or switch around it";
	EXPECT_TRUE(labelProblems(emitC(module)).empty());
	EXPECT_TRUE(functionsWithAStrandedGoto(module).empty());
}


constexpr const char* kBloomFilterFunction1140 = R"(source_filename = "test"
target datalayout = "e-m:e-p:64:64-i64:64-f80:128-n8:16:32:64-S128"

@global_var_3ff0 = global i64 0
@global_var_4018 = global i64 0
@global_var_3fe0 = global i64 0
@global_var_4025 = external global i8
@global_var_2004 = constant [4 x i8] c"%d\0A\00"

define i64 @function_1140() {
dec_label_pc_1140:
  %0 = load i8, ptr inttoptr (i64 16421 to ptr), align 1, !retdec.pointee !18
  %1 = or i8 %0, 8
  store i8 %1, ptr @global_var_4025, align 1, !retdec.pointee !18
  %2 = call i64 @printf(ptr nonnull @global_var_2004, i64 1), !insn.addr !20
  ret i64 0, !insn.addr !21
}

declare i64 @printf(ptr, i64)
!0 = !{i64 4096}
!1 = !{!"i64"}
!2 = !{i64 4114}
!3 = !{i64 4116}
!4 = !{i64 4122}
!5 = !{i64 4134}
!6 = !{i64 4144}
!7 = !{i64 4160}
!8 = !{i64 4207}
!9 = !{i64 4213}
!10 = !{i64 4255}
!11 = !{i64 4320}
!12 = !{i64 4336}
!13 = !{i64 4347}
!14 = !{i64 4361}
!15 = !{i64 4370}
!16 = !{i64 4388}
!17 = !{i64 4375}
!18 = !{!"i8"}
!19 = !{i64 4404}
!20 = !{i64 4438}
!21 = !{i64 4446}
!22 = !{i64 4460})";


/// The address-literal load CC-01 reports `invalid operands to binary |` on,
/// from generated_bloom_filter-clang-O2, reduced to itself. Eight lines:
///
///     %0 = load i8, ptr inttoptr (i64 16421 to ptr), align 1
///     %1 = or i8 %0, 8
///     store i8 %1, ptr @global_var_4025, align 1
///
/// Every type in it is stated. The emitted C was `g4 = *(void * *)0x4025 | 8`
/// beside `unsigned char g4;`.
///
/// Wrong straight out of the converter, before any pass runs, which is why
/// both of these check the unoptimised module as well.
TEST_F(OptimizerManagerPipelineTests, AnAddressLiteralLoadTakesTheTypeItLoads)
{
	auto module = convertLLVMIR2BIR(kBloomFilterFunction1140);

	EXPECT_TRUE(functionsWithAPointerBitwiseOperand(module).empty())
		<< "a pointer reached a bitwise operator straight out of the converter";
	EXPECT_THAT(emitC(module), HasSubstr("*(unsigned char *)0x4025"));
}

TEST_F(OptimizerManagerPipelineTests, AndKeepsItThroughThePipeline)
{
	auto module = convertLLVMIR2BIR(kBloomFilterFunction1140);

	runPipeline(module);

	EXPECT_TRUE(functionsWithAPointerBitwiseOperand(module).empty());
	EXPECT_THAT(emitC(module), Not(HasSubstr("(void * *)0x4025")));
}


} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
