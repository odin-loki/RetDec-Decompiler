/**
 * @file tests/py_reconstruct/py_reconstruct_test.cpp
 * @brief Unit tests for Python CFG reconstruction and AST node construction.
 */

#include <memory>
#include "retdec/py_reconstruct/py_ast_nodes.h"
#include "retdec/py_reconstruct/py_stack_sim.h"
#include "retdec/py_reconstruct/py_cfg_builder.h"
#include "retdec/pyc_parser/py_code_object.h"
#include "retdec/pyc_parser/pyc_magic.h"

#include <gtest/gtest.h>

using namespace retdec::py_reconstruct;
using namespace retdec::pyc_parser;

// ─── AST node factory helpers ─────────────────────────────────────────────────

TEST(PyAstNodes, MakeConst_Int)
{
	auto e = makeConst(int64_t{42});
	ASSERT_NE(nullptr, e);
	EXPECT_EQ(PyExpr::Kind::Constant, e->kind);
	EXPECT_EQ(PyExpr::ConstKind::Int, e->constKind);
	EXPECT_EQ(42LL, e->ival);
}

TEST(PyAstNodes, MakeConst_Float)
{
	auto e = makeConst(3.14);
	ASSERT_NE(nullptr, e);
	EXPECT_EQ(PyExpr::ConstKind::Float, e->constKind);
	EXPECT_NEAR(3.14, e->fval, 1e-10);
}

TEST(PyAstNodes, MakeConst_Str)
{
	auto e = makeConst("hello", false);
	ASSERT_NE(nullptr, e);
	EXPECT_EQ(PyExpr::ConstKind::Str, e->constKind);
	EXPECT_EQ("hello", e->sval);
}

TEST(PyAstNodes, MakeConst_Bytes)
{
	auto e = makeConst("\x01\x02", true);
	ASSERT_NE(nullptr, e);
	EXPECT_EQ(PyExpr::ConstKind::Bytes, e->constKind);
}

TEST(PyAstNodes, MakeNone)
{
	auto e = makeNone();
	ASSERT_NE(nullptr, e);
	EXPECT_EQ(PyExpr::Kind::Constant, e->kind);
	EXPECT_EQ(PyExpr::ConstKind::None_, e->constKind);
}

TEST(PyAstNodes, MakeName)
{
	auto e = makeName("x", ExprCtx::Load);
	ASSERT_NE(nullptr, e);
	EXPECT_EQ(PyExpr::Kind::Name, e->kind);
	EXPECT_EQ("x", e->name);
	EXPECT_EQ(ExprCtx::Load, e->ctx);
}

TEST(PyAstNodes, MakeNameStore)
{
	auto e = makeName("y", ExprCtx::Store);
	EXPECT_EQ(ExprCtx::Store, e->ctx);
}

TEST(PyAstNodes, MakeCall_NoArgs)
{
	auto func = makeName("foo");
	auto call = makeCall(func, {});
	ASSERT_NE(nullptr, call);
	EXPECT_EQ(PyExpr::Kind::Call, call->kind);
	ASSERT_EQ(1u, call->children.size());
	EXPECT_EQ("foo", call->children[0]->name);
	EXPECT_TRUE(call->values.empty());
}

TEST(PyAstNodes, MakeCall_WithArgs)
{
	auto func = makeName("print");
	auto arg1 = makeConst(int64_t{1});
	auto arg2 = makeConst("x", false);
	auto call = makeCall(func, {arg1, arg2});
	EXPECT_EQ(2u, call->values.size());
}

TEST(PyAstNodes, MakeAttr)
{
	auto obj = makeName("self");
	auto attr = makeAttr(obj, "x");
	ASSERT_NE(nullptr, attr);
	EXPECT_EQ(PyExpr::Kind::Attribute, attr->kind);
	EXPECT_EQ("x", attr->attr);
	EXPECT_EQ(ExprCtx::Load, attr->ctx);
}

TEST(PyAstNodes, MakeReturn_WithValue)
{
	auto val = makeConst(int64_t{0});
	auto ret = makeReturn(val);
	ASSERT_NE(nullptr, ret);
	EXPECT_EQ(PyStmt::Kind::Return, ret->kind);
	EXPECT_NE(nullptr, ret->expr);
	EXPECT_EQ(0LL, ret->expr->ival);
}

TEST(PyAstNodes, MakeReturn_None)
{
	auto ret = makeReturn(nullptr);
	EXPECT_EQ(PyStmt::Kind::Return, ret->kind);
	EXPECT_EQ(nullptr, ret->expr);
}

TEST(PyAstNodes, MakeAssign)
{
	auto tgt = makeName("x", ExprCtx::Store);
	auto val = makeConst(int64_t{1});
	auto asgn = makeAssign({tgt}, val);
	ASSERT_NE(nullptr, asgn);
	EXPECT_EQ(PyStmt::Kind::Assign, asgn->kind);
	ASSERT_EQ(1u, asgn->targets.size());
	EXPECT_EQ("x", asgn->targets[0]->name);
}

TEST(PyAstNodes, MakePass)
{
	auto p = makePass();
	EXPECT_EQ(PyStmt::Kind::Pass, p->kind);
}

TEST(PyAstNodes, MakeExprStmt)
{
	auto call = makeCall(makeName("foo"), {});
	auto s = makeExprStmt(call);
	EXPECT_EQ(PyStmt::Kind::Expr, s->kind);
}

// ─── PyArguments ──────────────────────────────────────────────────────────────

TEST(PyAstNodes, PyArgumentsDefault)
{
	PyArguments args;
	EXPECT_TRUE(args.args.empty());
	EXPECT_TRUE(args.posonlyargs.empty());
	EXPECT_FALSE(args.vararg.has_value());
	EXPECT_FALSE(args.kwarg.has_value());
}

// ─── PyMatchCase / PyPattern ──────────────────────────────────────────────────

TEST(PyAstNodes, PyMatchCaseDefault)
{
	PyMatchCase mc;
	EXPECT_EQ(nullptr, mc.pattern);
	EXPECT_EQ(nullptr, mc.guard);
	EXPECT_TRUE(mc.body.empty());
}

// ─── PyStackSimulator: simple code objects ────────────────────────────────────

static PyCodeObject makeSimpleCode(
	PythonVersion ver,
	std::vector<uint8_t> bytecode,
	std::vector<PyCodeObject::Const> consts = {},
	std::vector<std::string> varnames = {},
	std::vector<std::string> names = {})
{
	PyCodeObject code;
	code.version = ver;
	code.co_code = std::move(bytecode);
	code.co_consts = std::move(consts);
	code.co_varnames = std::move(varnames);
	code.co_names = std::move(names);
	code.co_firstlineno = 1;
	code.co_name = "test_func";
	return code;
}

// LOAD_CONST 0, RETURN_VALUE (Python 3.10 bytecode for "return None")
TEST(PyStackSimulator, ReturnNone)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const none_const;
	none_const.kind = PyCodeObject::Const::Kind::None;

	auto code = makeSimpleCode(
		ver,
		{100, 0, 83, 0}, // LOAD_CONST 0, RETURN_VALUE
		{none_const});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	// Should have at least a return
	EXPECT_FALSE(stmts.empty());
	bool hasReturn = false;
	for (const auto& s: stmts)
		if (s->kind == PyStmt::Kind::Return)
		{
			hasReturn = true;
			break;
		}
	EXPECT_TRUE(hasReturn);
}

// LOAD_FAST 0, RETURN_VALUE (return x)
TEST(PyStackSimulator, ReturnVar)
{
	PythonVersion ver{3, 10, 0, ""};
	auto code = makeSimpleCode(
		ver,
		{124, 0, 83, 0}, // LOAD_FAST 0, RETURN_VALUE
		{},
		{"x"});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	bool hasReturn = false;
	for (const auto& s: stmts)
	{
		if (s->kind == PyStmt::Kind::Return)
		{
			hasReturn = true;
			if (s->expr)
			{
				EXPECT_EQ(PyExpr::Kind::Name, s->expr->kind);
				EXPECT_EQ("x", s->expr->name);
			}
		}
	}
	EXPECT_TRUE(hasReturn);
}

// LOAD_CONST 1 (42), STORE_FAST 0, LOAD_FAST 0, RETURN_VALUE
TEST(PyStackSimulator, AssignAndReturn)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const c42;
	c42.kind = PyCodeObject::Const::Kind::Int;
	c42.ival = 42;

	auto code = makeSimpleCode(
		ver,
		{100, 1, 125, 0, 124, 0, 83, 0}, // LOAD_CONST 1, STORE_FAST 0, LOAD_FAST 0, RETURN_VALUE
		{{}, c42},                       // co_consts[0]=None, [1]=42
		{"x"});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();
	EXPECT_GE(stmts.size(), 2u);
}

// LOAD_FAST 0, LOAD_FAST 1, BINARY_ADD, RETURN_VALUE
TEST(PyStackSimulator, BinaryAdd)
{
	PythonVersion ver{3, 10, 0, ""};
	auto code = makeSimpleCode(
		ver,
		{124, 0, 124, 1, 23, 0, 83, 0}, // LOAD_FAST 0, LOAD_FAST 1, BINARY_ADD, RETURN_VALUE
		{},
		{"a", "b"});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	bool hasReturn = false;
	for (const auto& s: stmts)
	{
		if (s->kind == PyStmt::Kind::Return && s->expr)
		{
			hasReturn = true;
			EXPECT_EQ(PyExpr::Kind::BinOp, s->expr->kind);
			EXPECT_EQ(BinOp::Add, s->expr->binOp);
		}
	}
	EXPECT_TRUE(hasReturn);
}

// LOAD_FAST 0, UNARY_NOT, RETURN_VALUE
TEST(PyStackSimulator, UnaryNot)
{
	PythonVersion ver{3, 10, 0, ""};
	auto code = makeSimpleCode(
		ver,
		{124, 0, 12, 0, 83, 0}, // LOAD_FAST 0, UNARY_NOT, RETURN_VALUE
		{},
		{"flag"});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	bool found = false;
	for (const auto& s: stmts)
	{
		if (s->kind == PyStmt::Kind::Return && s->expr && s->expr->kind == PyExpr::Kind::UnaryOp
			&& s->expr->unaryOp == UnaryOp::Not)
		{
			found = true;
		}
	}
	EXPECT_TRUE(found);
}

// BUILD_LIST 0, RETURN_VALUE → return []
TEST(PyStackSimulator, BuildEmptyList)
{
	PythonVersion ver{3, 10, 0, ""};
	auto code = makeSimpleCode(ver, {103, 0, 83, 0}); // BUILD_LIST 0, RETURN_VALUE

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	bool found = false;
	for (const auto& s: stmts)
	{
		if (s->kind == PyStmt::Kind::Return && s->expr && s->expr->kind == PyExpr::Kind::List)
		{
			found = true;
			EXPECT_TRUE(s->expr->values.empty());
		}
	}
	EXPECT_TRUE(found);
}

// LOAD_GLOBAL 'print', LOAD_FAST 'x', CALL_FUNCTION 1, POP_TOP, LOAD_CONST None, RETURN_VALUE
TEST(PyStackSimulator, CallFunctionStatement)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const none_c;
	none_c.kind = PyCodeObject::Const::Kind::None;

	auto code = makeSimpleCode(
		ver,
		// LOAD_GLOBAL 0, LOAD_FAST 0, CALL_FUNCTION 1, POP_TOP, LOAD_CONST 0, RETURN_VALUE
		{116, 0, 124, 0, 131, 1, 1, 0, 100, 0, 83, 0},
		{none_c},
		{"x"},
		{"print"});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();
	EXPECT_FALSE(stmts.empty());
}

TEST(PyStackSimulator, EmptyCode)
{
	PythonVersion ver{3, 10, 0, ""};
	auto code = makeSimpleCode(ver, {});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();
	EXPECT_FALSE(stmts.empty()); // should emit pass
	EXPECT_EQ(PyStmt::Kind::Pass, stmts[0]->kind);
}

TEST(PyStackSimulator, NoWarningsForSimpleCode)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const none_c;
	none_c.kind = PyCodeObject::Const::Kind::None;
	auto code = makeSimpleCode(ver, {100, 0, 83, 0}, {none_c});

	PyStackSimulator sim(code);
	sim.simulate();
	EXPECT_TRUE(sim.warnings().empty());
}

// ─── PyCfgBuilder ─────────────────────────────────────────────────────────────

TEST(PyCfgBuilder, BuildSimpleFunction)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const none_c;
	none_c.kind = PyCodeObject::Const::Kind::None;
	auto code = makeSimpleCode(ver, {100, 0, 83, 0}, {none_c});

	PyCfgBuilder builder;
	auto stmts = builder.build(code);
	EXPECT_FALSE(stmts.empty());
}

TEST(PyCfgBuilder, NoWarningsForSimple)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const none_c;
	none_c.kind = PyCodeObject::Const::Kind::None;
	auto code = makeSimpleCode(ver, {100, 0, 83, 0}, {none_c});

	PyCfgBuilder builder;
	builder.build(code);
	EXPECT_TRUE(builder.warnings().empty());
}

// ─── PyReconstructor ─────────────────────────────────────────────────────────

TEST(PyReconstructor, ReconstructSimpleModule)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const none_c;
	none_c.kind = PyCodeObject::Const::Kind::None;
	auto code = makeSimpleCode(ver, {100, 0, 83, 0}, {none_c});
	code.co_name = "<module>";
	code.co_filename = "test.py";

	PyReconstructor rec;
	auto mod = rec.reconstruct(code, 3, 10, "test.py");
	EXPECT_EQ("test.py", mod.filename);
	EXPECT_EQ(3, mod.pythonMajor);
	EXPECT_EQ(10, mod.pythonMinor);
	EXPECT_FALSE(mod.body.empty());
}

TEST(PyReconstructor, ReconstructWithNestedFunction)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const none_c;
	none_c.kind = PyCodeObject::Const::Kind::None;

	// Nested code object for "def inner(): pass"
	auto innerCode = std::make_shared<PyCodeObject>();
	innerCode->version = ver;
	innerCode->co_name = "inner";
	innerCode->co_code = {100, 0, 83, 0};
	innerCode->co_consts = {none_c};

	PyCodeObject::Const codeConst;
	codeConst.kind = PyCodeObject::Const::Kind::Code;
	codeConst.code = innerCode;

	auto rootCode = makeSimpleCode(ver, {100, 0, 83, 0}, {none_c, codeConst});
	rootCode.co_name = "<module>";

	PyReconstructor::Options opts;
	opts.skipCompGenerated = false;
	PyReconstructor rec(opts);
	auto mod = rec.reconstruct(rootCode, 3, 10);
	EXPECT_FALSE(mod.body.empty());
}

TEST(PyReconstructor, SkipsCompilerGenerated)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const none_c;
	none_c.kind = PyCodeObject::Const::Kind::None;

	auto genexprCode = std::make_shared<PyCodeObject>();
	genexprCode->version = ver;
	genexprCode->co_name = "<genexpr>";
	genexprCode->co_code = {100, 0, 83, 0};
	genexprCode->co_consts = {none_c};

	PyCodeObject::Const codeConst;
	codeConst.kind = PyCodeObject::Const::Kind::Code;
	codeConst.code = genexprCode;

	auto rootCode = makeSimpleCode(ver, {100, 0, 83, 0}, {none_c, codeConst});
	rootCode.co_name = "<module>";

	PyReconstructor::Options opts;
	opts.skipCompGenerated = true;
	PyReconstructor rec(opts);
	auto mod = rec.reconstruct(rootCode, 3, 10);

	// No FunctionDef for <genexpr> should appear
	for (const auto& s: mod.body)
	{
		if (s->kind == PyStmt::Kind::FunctionDef)
		{
			EXPECT_NE("<genexpr>", s->funcName);
		}
	}
}

TEST(PyReconstructor, DocstringInjected)
{
	PythonVersion ver{3, 10, 0, ""};
	PyCodeObject::Const doc_c;
	doc_c.kind = PyCodeObject::Const::Kind::Str;
	doc_c.sval = "Module docstring.";
	PyCodeObject::Const none_c;
	none_c.kind = PyCodeObject::Const::Kind::None;

	auto code = makeSimpleCode(ver, {100, 1, 83, 0}, {doc_c, none_c});
	code.co_name = "<module>";

	PyReconstructor::Options opts;
	opts.addDocstrings = true;
	PyReconstructor rec(opts);
	auto mod = rec.reconstruct(code, 3, 10);

	bool hasDocStr = false;
	for (const auto& s: mod.body)
	{
		if (s->kind == PyStmt::Kind::Expr && s->expr && s->expr->kind == PyExpr::Kind::Constant
			&& s->expr->constKind == PyExpr::ConstKind::Str && s->expr->sval == "Module docstring.")
		{
			hasDocStr = true;
		}
	}
	EXPECT_TRUE(hasDocStr);
}

// ─── SrcLoc / PyModule defaults ──────────────────────────────────────────────

TEST(PyAstNodes, SrcLocDefault)
{
	SrcLoc loc;
	EXPECT_EQ(0, loc.line);
	EXPECT_EQ(0, loc.col);
}

TEST(PyAstNodes, PyModuleDefault)
{
	PyModule mod;
	EXPECT_TRUE(mod.body.empty());
	EXPECT_EQ(3, mod.pythonMajor);
}

TEST(PyAstNodes, ExprCtxLoad)
{
	auto e = makeName("x");
	EXPECT_EQ(ExprCtx::Load, e->ctx);
}

// PyAstNodes.BoolOpAndOr used to live here. It default-constructed a PyExpr,
// assigned boolOp and values, and asserted that boolOp and values held what
// had just been assigned -- no py_reconstruct code ran between the write and
// the read, so it tested C++ struct assignment. Nothing in py_reconstruct sets
// boolOp at all; the field is consumed by the emitter, and
// tests/py_emitter/py_emitter_test.cpp's ExprEmitterTest.BoolOp_And and
// BoolOp_Or already assert the text it produces. It is dropped rather than
// re-stated here, where there is no producer to test against.

// cmpOps, unlike boolOp, does have a producer: PyStackSimulator::buildCompare
// maps a COMPARE_OP argument onto it. That is what this asserts now, instead
// of asserting the value the test had just written into the node itself.
TEST(PyStackSimulator, CompareOpSetsCmpOpFromArgument)
{
	PythonVersion ver{3, 10, 0, ""};
	struct Case
	{
		uint8_t arg;
		CmpOp expected;
	};
	// The table in buildCompare, in order.
	const Case cases[] = {
		{0, CmpOp::Lt},
		{1, CmpOp::LtE},
		{2, CmpOp::Eq},
		{3, CmpOp::NotEq},
		{4, CmpOp::Gt},
		{5, CmpOp::GtE},
	};

	for (const auto& c: cases)
	{
		// LOAD_FAST 0, LOAD_FAST 1, COMPARE_OP <arg>, RETURN_VALUE
		auto code = makeSimpleCode(ver, {124, 0, 124, 1, 107, c.arg, 83, 0}, {}, {"a", "b"});

		PyStackSimulator sim(code);
		auto stmts = sim.simulate();

		bool checked = false;
		for (const auto& s: stmts)
		{
			if (s->kind != PyStmt::Kind::Return || !s->expr) continue;
			ASSERT_EQ(PyExpr::Kind::Compare, s->expr->kind);
			ASSERT_EQ(1u, s->expr->cmpOps.size());
			EXPECT_EQ(c.expected, s->expr->cmpOps[0]) << "for COMPARE_OP arg " << int(c.arg);
			checked = true;
		}
		EXPECT_TRUE(checked) << "no compare reached the return for arg " << int(c.arg);
	}
}

TEST(PyStackSimulator, CompareOpFallsBackToEqualForAnUnknownArgument)
{
	PythonVersion ver{3, 10, 0, ""};
	// 200 is past the end of the ten-entry table.
	auto code = makeSimpleCode(ver, {124, 0, 124, 1, 107, 200, 83, 0}, {}, {"a", "b"});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	bool checked = false;
	for (const auto& s: stmts)
	{
		if (s->kind != PyStmt::Kind::Return || !s->expr) continue;
		ASSERT_EQ(1u, s->expr->cmpOps.size());
		EXPECT_EQ(CmpOp::Eq, s->expr->cmpOps[0]);
		checked = true;
	}
	EXPECT_TRUE(checked);
}

// ─── PyStackSimulator: operand counts a file made up ─────────────────────────
//
// Every count below is written in the instruction stream by the file being
// decompiled, and nothing in the file has to supply the operands it names.

// BUILD_TUPLE 5, CALL_FUNCTION_KW 0.  The keyword-name tuple is five long and
// the call is zero wide, so the split between positional and keyword arguments
// used to land at -5 and index the argument vector from behind its start.
TEST(PyStackSimulator, MoreKeywordNamesThanTheCallIsWide)
{
	PythonVersion ver{3, 10, 0, ""};
	auto code = makeSimpleCode(ver, {102, 5, 140, 0});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	// The call survives; every keyword it kept has a value from the vector.
	for (const auto& s: stmts)
	{
		if (!s->expr || s->expr->kind != PyExpr::Kind::Call) continue;
		for (const auto& kw: s->expr->keywords)
			EXPECT_NE(nullptr, kw.value);
	}
}

// The same shape with the tuple longer than a call that does have arguments:
// the names past the end of the argument list have nothing to bind to.
TEST(PyStackSimulator, KeywordNamesPastTheArgumentListAreDropped)
{
	PythonVersion ver{3, 10, 0, ""};
	// LOAD_FAST 0, LOAD_FAST 0, BUILD_TUPLE 9, CALL_FUNCTION_KW 2
	auto code = makeSimpleCode(
		ver, {124, 0, 124, 0, 102, 9, 140, 2}, {}, {"x"});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	for (const auto& s: stmts)
	{
		if (!s->expr || s->expr->kind != PyExpr::Kind::Call) continue;
		EXPECT_LE(s->expr->keywords.size() + s->expr->values.size(), 2u);
		for (const auto& kw: s->expr->keywords)
			EXPECT_NE(nullptr, kw.value);
	}
}

// EXTENDED_ARG x3 then BUILD_TUPLE: 0x7FFFFFFF operands out of an eight-byte
// body.  Bounded, this returns; unbounded it inserts at the front of a vector
// two billion times.
TEST(PyStackSimulator, AnExtendedArgCountLargerThanTheBodyIsTruncated)
{
	PythonVersion ver{3, 10, 0, ""};
	// ... then RETURN_VALUE, so the tuple reaches a statement.
	auto code = makeSimpleCode(
		ver, {90, 0x7F, 90, 0xFF, 90, 0xFF, 102, 0xFF, 83, 0});

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	bool sawTuple = false;
	for (const auto& s: stmts)
	{
		if (!s->expr || s->expr->kind != PyExpr::Kind::Tuple) continue;
		sawTuple = true;
		EXPECT_LE(s->expr->values.size(), 1024u);
	}
	EXPECT_TRUE(sawTuple);
	EXPECT_FALSE(sim.warnings().empty()) << "the truncation should be reported";
}

// A fourth EXTENDED_ARG used to shift a signed accumulator past its width.
TEST(PyStackSimulator, MoreExtendedArgPrefixesThanAnOpargCanHold)
{
	PythonVersion ver{3, 10, 0, ""};
	std::vector<uint8_t> bc;
	for (int i = 0; i < 40; ++i) { bc.push_back(90); bc.push_back(0xFF); }
	bc.push_back(103); // BUILD_LIST
	bc.push_back(0xFF);

	auto code = makeSimpleCode(ver, bc);
	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	for (const auto& s: stmts)
	{
		if (!s->expr || s->expr->kind != PyExpr::Kind::List) continue;
		EXPECT_LE(s->expr->values.size(), 1024u);
	}
}

// UNPACK_SEQUENCE only pushes, so no stack depth limits it.
TEST(PyStackSimulator, AnUnpackWiderThanTheBodyIsTruncated)
{
	PythonVersion ver{3, 10, 0, ""};
	auto code = makeSimpleCode(
		ver, {90, 0x7F, 90, 0xFF, 90, 0xFF, 92, 0xFF}); // UNPACK_SEQUENCE

	PyStackSimulator sim(code);
	sim.simulate();

	EXPECT_FALSE(sim.warnings().empty()) << "the truncation should be reported";
}

// BUILD_MAP consumes two entries per count, so its bound is half the depth.
TEST(PyStackSimulator, AMapWiderThanTheBodyIsTruncated)
{
	PythonVersion ver{3, 10, 0, ""};
	auto code = makeSimpleCode(
		ver, {90, 0x7F, 90, 0xFF, 90, 0xFF, 105, 0xFF}); // BUILD_MAP

	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	for (const auto& s: stmts)
	{
		if (!s->expr || s->expr->kind != PyExpr::Kind::Dict) continue;
		EXPECT_EQ(s->expr->keys.size(), s->expr->values.size());
		EXPECT_LE(s->expr->values.size(), 1024u);
	}
}

// A count that names exactly what the stack holds is not truncated.
TEST(PyStackSimulator, AnHonestCountIsLeftAlone)
{
	PythonVersion ver{3, 10, 0, ""};
	std::vector<uint8_t> bc;
	for (int i = 0; i < 300; ++i) { bc.push_back(124); bc.push_back(0); } // LOAD_FAST 0
	bc.push_back(90); bc.push_back(1);  // EXTENDED_ARG 1
	bc.push_back(102); bc.push_back(44); // BUILD_TUPLE 300
	bc.push_back(83); bc.push_back(0);   // RETURN_VALUE

	auto code = makeSimpleCode(ver, bc, {}, {"x"});
	PyStackSimulator sim(code);
	auto stmts = sim.simulate();

	bool checked = false;
	for (const auto& s: stmts)
	{
		if (s->kind != PyStmt::Kind::Return || !s->expr) continue;
		if (s->expr->kind != PyExpr::Kind::Tuple) continue;
		EXPECT_EQ(300u, s->expr->values.size());
		checked = true;
	}
	EXPECT_TRUE(checked);
	EXPECT_TRUE(sim.warnings().empty()) << "a count the stack can meet is not truncated";
}
