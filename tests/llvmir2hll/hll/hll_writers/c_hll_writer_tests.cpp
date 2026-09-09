/**
* @file tests/llvmir2hll/hll/hll_writers/c_hll_writer_tests.cpp
* @brief Tests for the @c c_hll_writer module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <gtest/gtest.h>

#include <vector>

#include "retdec/llvmir2hll/hll/hll_writers/c_hll_writer.h"
#include "llvmir2hll/hll/hll_writers/hll_writer_tests.h"
#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/assign_op_expr.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/const_float.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/const_string.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/float_type.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/unreachable_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/utils/string.h"

using namespace ::testing;

using retdec::utils::contains;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c c_hll_writer module.
*/
class CHLLWriterTests: public HLLWriterTests {
protected:
	virtual void SetUp() override;
};

void CHLLWriterTests::SetUp() {
	HLLWriterTests::SetUp();

	writer = CHLLWriter::create(codeStream);
}

TEST_F(CHLLWriterTests,
EmitsNonEmptyCode) {
	auto code = emitCodeForCurrentModule();

	ASSERT_FALSE(code.empty());
}

//
// Emission of floating-point literals.
//

TEST_F(CHLLWriterTests,
FloatLiteralIsEmittedWithCorrectSuffix) {
	//
	// float g = 0.0f;
	//
	module->addGlobalVar(
		Variable::create("g", FloatType::create(32)),
		ConstFloat::create(llvm::APFloat(llvm::APFloat::IEEEsingle(), "0.0"))
	);

	auto code = emitCodeForCurrentModule();

	ASSERT_TRUE(contains(code, " = 0.0f;")) << code;
}

TEST_F(CHLLWriterTests,
DoubleLiteralIsEmittedWithoutSuffix) {
	//
	// double g = 0.0; // (floating-point literals are double by default)
	//
	module->addGlobalVar(
		Variable::create("g", FloatType::create(64)),
		ConstFloat::create(llvm::APFloat(llvm::APFloat::IEEEdouble(), "0.0"))
	);

	auto code = emitCodeForCurrentModule();

	ASSERT_TRUE(contains(code, " = 0.0;")) << code;
}

TEST_F(CHLLWriterTests,
LongDoubleLiteralIsEmittedWithCorrectSuffix) {
	//
	// long double g = 0.0L;
	//
	module->addGlobalVar(
		Variable::create("g", FloatType::create(80)),
		ConstFloat::create(llvm::APFloat(llvm::APFloat::x87DoubleExtended(), "0.0"))
	);

	auto code = emitCodeForCurrentModule();

	ASSERT_TRUE(contains(code, " = 0.0L;")) << code;
}

//
// Emission of strings.
//

TEST_F(CHLLWriterTests,
Emits8BitStringLiteral) {
	//
	// void test() {
	//     printf("wide string");
	// }
	//
	auto printfFunc = addFuncDecl("printf");
	ExprVector args;
	args.push_back({ConstString::create("ascii string")});
	auto printfCallStmt = CallStmt::create(
		CallExpr::create(
			printfFunc->getAsVar(),
			args
		)
	);
	testFunc->setBody(printfCallStmt);

	auto code = emitCodeForCurrentModule();

	ASSERT_TRUE(contains(code, "printf(\"ascii string\");")) << code;
}

TEST_F(CHLLWriterTests,
EmitsWideStringLiteral) {
	//
	// void test() {
	//     wprintf(L"wide string");
	// }
	//
	auto wprintfFunc = addFuncDecl("wprintf");
	ExprVector args;
	args.push_back(
		ConstString::create({'w', 'i', 'd', 'e', ' ', 's', 't', 'r', 'i', 'n', 'g'}, 16)
	);
	auto wprintfCallStmt = CallStmt::create(
		CallExpr::create(
			wprintfFunc->getAsVar(),
			args
		)
	);
	testFunc->setBody(wprintfCallStmt);

	auto code = emitCodeForCurrentModule();

	ASSERT_TRUE(contains(code, "wprintf(L\"wide string\");")) << code;
}

//
// Emission of universal for loops.
//

TEST_F(CHLLWriterTests,
EmitsUForLoopStmtWithInitCondStep) {
	//
	// void test() {
	//     for (i = 0; i < 10; ++i) {
	//     }
	// }
	//
	auto varI = Variable::create("i", IntType::create(32));
	testFunc->addLocalVar(varI);
	auto loop = UForLoopStmt::create(
		AssignOpExpr::create(varI, ConstInt::create(0, 32)),
		LtOpExpr::create(varI, ConstInt::create(10, 32)),
		AssignOpExpr::create(
			varI,
			AddOpExpr::create(varI, ConstInt::create(1, 32))
		),
		EmptyStmt::create()
	);
	testFunc->setBody(loop);

	auto code = emitCodeForCurrentModule();

	ASSERT_TRUE(contains(code, "for (i = 0; i < 10; i++)")) << code;
}

TEST_F(CHLLWriterTests,
EmitsUForLoopStmtWithoutInitCondStep) {
	//
	// void test() {
	//     for (;;) {
	//     }
	// }
	//
	auto loop = UForLoopStmt::create(
		ShPtr<Expression>(),
		ShPtr<Expression>(),
		ShPtr<Expression>(),
		EmptyStmt::create()
	);
	testFunc->setBody(loop);

	auto code = emitCodeForCurrentModule();

	ASSERT_TRUE(contains(code, "for (;;)")) << code;
}

TEST_F(CHLLWriterTests,
EmitsVarDefOfInitOfUForLoopStmtWhenLoopHasItsInitMarked) {
	//
	// void test() {
	//     for (int32_t i = 0; ;) {
	//     }
	// }
	//
	auto varI = Variable::create("i", IntType::create(32));
	testFunc->addLocalVar(varI);
	auto loop = UForLoopStmt::create(
		AssignOpExpr::create(varI, ConstInt::create(0, 32)),
		ShPtr<Expression>(),
		ShPtr<Expression>(),
		EmptyStmt::create()
	);
	loop->markInitAsDefinition();
	testFunc->setBody(loop);

	auto code = emitCodeForCurrentModule();

	ASSERT_TRUE(contains(code, "for (int32_t i = 0;")) << code;
}


//
// A label with nothing after it, and a goto naming a label nobody wrote.
//
// emitGotoLabelIfNeeded() already knows that C forbids a label at the end of a
// compound statement: it emits a trailing ';' for a labelled VarDefStmt and
// for a labelled trailing EmptyStmt. UnreachableStmt is the third case and was
// not covered -- visit(UnreachableStmt) emits nothing on purpose, so a
// labelled unreachable terminator, which is what a block after a noreturn call
// such as abort() or __stack_chk_fail() ends in, put the label immediately
// before the closing brace.
//
// The same statement showed a second defect: HLLWriter::getRawGotoLabel()
// invents "generated_N" for a statement carrying neither a label nor LLVM
// basic-block metadata, and it is asked twice -- once to write `goto <name>`
// and once to write `<name>:`. Without memoisation the two answers differed
// and the emitted C said `label 'lab_generated_0' used but not defined`, the
// same error scripts/ci/check_emitted_c_compiles.sh reports from the corpus.
//

namespace {

/// Labels the emitted code jumps to but never defines.
std::vector<std::string> undefinedGotoTargets(const std::string& code)
{
	std::vector<std::string> missing;
	for (std::size_t i = code.find("goto "); i != std::string::npos; i = code.find("goto ", i + 1))
	{
		const auto nameStart = i + 5;
		const auto nameEnd = code.find(';', nameStart);
		if (nameEnd == std::string::npos)
		{
			continue;
		}
		auto name = code.substr(nameStart, nameEnd - nameStart);
		if (code.find("\n" + name + ":") == std::string::npos && code.find(" " + name + ":") == std::string::npos)
		{
			missing.push_back(name);
		}
	}
	return missing;
}

} // namespace

TEST_F(CHLLWriterTests, LabelledUnreachableStatementIsFollowedByAStatement)
{
	//
	// void test() {
	//     goto L;
	//     L: <unreachable>
	// }
	//
	auto unreachable = UnreachableStmt::create();
	auto gotoStmt = GotoStmt::create(unreachable);
	gotoStmt->setSuccessor(unreachable);
	testFunc->setBody(gotoStmt);

	auto code = emitCodeForCurrentModule();

	const auto colon = code.find(":\n");
	EXPECT_EQ(std::string::npos, colon) << "a label with nothing after it ends the block, which C rejects:\n" << code;
}

TEST_F(CHLLWriterTests, EveryGeneratedGotoLabelIsAlsoEmitted)
{
	//
	// The target has no label and no LLVM metadata, so the writer has to
	// invent a name -- and use the same one in both places.
	//
	auto unreachable = UnreachableStmt::create();
	auto gotoStmt = GotoStmt::create(unreachable);
	gotoStmt->setSuccessor(unreachable);
	testFunc->setBody(gotoStmt);

	auto code = emitCodeForCurrentModule();

	auto missing = undefinedGotoTargets(code);
	EXPECT_TRUE(missing.empty()) << "goto " << (missing.empty() ? std::string() : missing.front())
								 << " names a label that is never emitted:\n"
								 << code;
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
