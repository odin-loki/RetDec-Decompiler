/**
 * @file tests/py_emitter/py_emitter_test.cpp
 * @brief Unit tests for the Python source emitter.
 */

#include <memory>
#include "retdec/py_emitter/py_writer.h"
#include "retdec/py_emitter/py_expr_emitter.h"
#include "retdec/py_emitter/py_stmt_emitter.h"
#include "retdec/py_emitter/py_file_emitter.h"
#include "retdec/py_reconstruct/py_ast_nodes.h"

#include <gtest/gtest.h>

using namespace retdec::py_emitter;
using namespace retdec::py_reconstruct;

// ─── PyWriter ────────────────────────────────────────────────────────────────

TEST(PyWriter, BasicLine) {
    PyWriter w;
    w.line("x = 1");
    EXPECT_EQ("x = 1\n", w.str());
}

TEST(PyWriter, Indent) {
    PyWriter w;
    w.line("def f():");
    w.indent();
    w.line("pass");
    w.dedent();
    EXPECT_EQ("def f():\n    pass\n", w.str());
}

TEST(PyWriter, DoubleIndent) {
    PyWriter w;
    w.indent();
    w.indent();
    w.line("x = 1");
    EXPECT_EQ("        x = 1\n", w.str());
}

TEST(PyWriter, BlankLine) {
    PyWriter w;
    w.line("a = 1");
    w.blank();
    w.line("b = 2");
    EXPECT_EQ("a = 1\n\nb = 2\n", w.str());
}

TEST(PyWriter, Comment) {
    PyWriter w;
    w.comment("hello");
    EXPECT_EQ("# hello\n", w.str());
}

TEST(PyWriter, SafeName_Keyword) {
    EXPECT_EQ("for_", PyWriter::safeName("for"));
    EXPECT_EQ("if_", PyWriter::safeName("if"));
    EXPECT_EQ("class_", PyWriter::safeName("class"));
}

TEST(PyWriter, SafeName_Normal) {
    EXPECT_EQ("x", PyWriter::safeName("x"));
    EXPECT_EQ("my_var", PyWriter::safeName("my_var"));
}

TEST(PyWriter, SafeName_Empty) {
    EXPECT_EQ("_", PyWriter::safeName(""));
}

TEST(PyWriter, IsKeyword) {
    EXPECT_TRUE(PyWriter::isKeyword("for"));
    EXPECT_TRUE(PyWriter::isKeyword("def"));
    EXPECT_FALSE(PyWriter::isKeyword("foo"));
}

TEST(PyWriter, StrLiteral_Simple) {
    PyWriter w;
    EXPECT_EQ("\"hello\"", w.strLiteral("hello"));
}

TEST(PyWriter, StrLiteral_Escape) {
    PyWriter w;
    auto s = w.strLiteral("a\"b");
    EXPECT_EQ("\"a\\\"b\"", s);
}

TEST(PyWriter, StrLiteral_Newline) {
    PyWriter w;
    auto s = w.strLiteral("a\nb");
    EXPECT_EQ("\"a\\nb\"", s);
}

TEST(PyWriter, BytesLiteral) {
    PyWriter w;
    EXPECT_EQ(0u, w.bytesLiteral("abc").find("b\""));
}

TEST(PyWriter, Reset) {
    PyWriter w;
    w.line("x = 1");
    w.reset();
    EXPECT_EQ("", w.str());
}

// ─── PyExprEmitter ───────────────────────────────────────────────────────────

class ExprEmitterTest : public ::testing::Test {
protected:
    PyWriter      writer;
    PyExprEmitter emitter{writer};
};

TEST_F(ExprEmitterTest, Constant_Int) {
    EXPECT_EQ("42", emitter.emit(makeConst(int64_t{42})));
}

TEST_F(ExprEmitterTest, Constant_Float) {
    EXPECT_NE("", emitter.emit(makeConst(3.14)));
}

TEST_F(ExprEmitterTest, Constant_Str) {
    EXPECT_EQ("\"hello\"", emitter.emit(makeConst("hello", false)));
}

TEST_F(ExprEmitterTest, Constant_None) {
    EXPECT_EQ("None", emitter.emit(makeNone()));
}

TEST_F(ExprEmitterTest, Constant_TrueFalse) {
    auto t = std::make_shared<PyExpr>();
    t->kind = PyExpr::Kind::Constant;
    t->constKind = PyExpr::ConstKind::True_;
    EXPECT_EQ("True", emitter.emit(t));

    auto f = std::make_shared<PyExpr>();
    f->kind = PyExpr::Kind::Constant;
    f->constKind = PyExpr::ConstKind::False_;
    EXPECT_EQ("False", emitter.emit(f));
}

TEST_F(ExprEmitterTest, Constant_Ellipsis) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Constant;
    e->constKind = PyExpr::ConstKind::Ellipsis_;
    EXPECT_EQ("...", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Name) {
    EXPECT_EQ("x", emitter.emit(makeName("x")));
}

TEST_F(ExprEmitterTest, NameKeyword) {
    EXPECT_EQ("class_", emitter.emit(makeName("class")));
}

TEST_F(ExprEmitterTest, BinOp_Add) {
    auto e = std::make_shared<PyExpr>();
    e->kind  = PyExpr::Kind::BinOp;
    e->binOp = BinOp::Add;
    e->children = {makeName("a"), makeName("b")};
    EXPECT_EQ("a + b", emitter.emit(e));
}

TEST_F(ExprEmitterTest, BinOp_Pow) {
    auto e = std::make_shared<PyExpr>();
    e->kind  = PyExpr::Kind::BinOp;
    e->binOp = BinOp::Pow;
    e->children = {makeName("a"), makeConst(int64_t{2})};
    EXPECT_EQ("a ** 2", emitter.emit(e));
}

TEST_F(ExprEmitterTest, BinOp_Precedence_ParenAdded) {
    // (a + b) * c  → should NOT add parens for *, but (a+b) inside * needs parens
    auto add = std::make_shared<PyExpr>();
    add->kind  = PyExpr::Kind::BinOp;
    add->binOp = BinOp::Add;
    add->children = {makeName("a"), makeName("b")};

    auto mul = std::make_shared<PyExpr>();
    mul->kind  = PyExpr::Kind::BinOp;
    mul->binOp = BinOp::Mult;
    mul->children = {add, makeName("c")};

    std::string s = emitter.emit(mul);
    EXPECT_NE(std::string::npos, s.find("(a + b)"));
}

TEST_F(ExprEmitterTest, UnaryOp_Not) {
    auto e = std::make_shared<PyExpr>();
    e->kind     = PyExpr::Kind::UnaryOp;
    e->unaryOp  = UnaryOp::Not;
    e->children = {makeName("x")};
    EXPECT_EQ("not x", emitter.emit(e));
}

TEST_F(ExprEmitterTest, UnaryOp_Neg) {
    auto e = std::make_shared<PyExpr>();
    e->kind     = PyExpr::Kind::UnaryOp;
    e->unaryOp  = UnaryOp::USub;
    e->children = {makeName("x")};
    EXPECT_EQ("-x", emitter.emit(e));
}

TEST_F(ExprEmitterTest, UnaryOp_Invert) {
    auto e = std::make_shared<PyExpr>();
    e->kind     = PyExpr::Kind::UnaryOp;
    e->unaryOp  = UnaryOp::Invert;
    e->children = {makeName("x")};
    EXPECT_EQ("~x", emitter.emit(e));
}

TEST_F(ExprEmitterTest, BoolOp_And) {
    auto e = std::make_shared<PyExpr>();
    e->kind   = PyExpr::Kind::BoolOp;
    e->boolOp = BoolOp::And;
    e->values = {makeName("a"), makeName("b"), makeName("c")};
    EXPECT_EQ("a and b and c", emitter.emit(e));
}

TEST_F(ExprEmitterTest, BoolOp_Or) {
    auto e = std::make_shared<PyExpr>();
    e->kind   = PyExpr::Kind::BoolOp;
    e->boolOp = BoolOp::Or;
    e->values = {makeName("x"), makeName("y")};
    EXPECT_EQ("x or y", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Compare_Eq) {
    auto e = std::make_shared<PyExpr>();
    e->kind    = PyExpr::Kind::Compare;
    e->cmpOps  = {CmpOp::Eq};
    e->children= {makeName("a")};
    e->values  = {makeName("b")};
    EXPECT_EQ("a == b", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Compare_IsNot) {
    auto e = std::make_shared<PyExpr>();
    e->kind    = PyExpr::Kind::Compare;
    e->cmpOps  = {CmpOp::IsNot};
    e->children= {makeName("x")};
    e->values  = {makeNone()};
    EXPECT_EQ("x is not None", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Compare_NotIn) {
    auto e = std::make_shared<PyExpr>();
    e->kind    = PyExpr::Kind::Compare;
    e->cmpOps  = {CmpOp::NotIn};
    e->children= {makeName("x")};
    e->values  = {makeName("lst")};
    EXPECT_EQ("x not in lst", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Call_NoArgs) {
    EXPECT_EQ("foo()", emitter.emit(makeCall(makeName("foo"), {})));
}

TEST_F(ExprEmitterTest, Call_WithArgs) {
    auto c = makeCall(makeName("print"), {makeName("x"), makeConst(int64_t{42})});
    EXPECT_EQ("print(x, 42)", emitter.emit(c));
}

TEST_F(ExprEmitterTest, Call_WithKwargs) {
    auto c = makeCall(makeName("f"), {});
    PyKeyword kw;
    kw.arg   = "sep";
    kw.value = makeConst(", ", false);
    c->keywords.push_back(kw);
    std::string s = emitter.emit(c);
    EXPECT_NE(std::string::npos, s.find("sep="));
}

TEST_F(ExprEmitterTest, Attribute) {
    EXPECT_EQ("self.x", emitter.emit(makeAttr(makeName("self"), "x")));
}

TEST_F(ExprEmitterTest, Subscript) {
    auto e = std::make_shared<PyExpr>();
    e->kind     = PyExpr::Kind::Subscript;
    e->children = {makeName("lst"), makeConst(int64_t{0})};
    EXPECT_EQ("lst[0]", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Subscript_Slice) {
    auto e = std::make_shared<PyExpr>();
    e->kind      = PyExpr::Kind::Subscript;
    e->isSlice   = true;
    e->children  = {makeName("lst")};
    e->sliceLower = makeConst(int64_t{1});
    e->sliceUpper = makeConst(int64_t{5});
    std::string s = emitter.emit(e);
    EXPECT_EQ("lst[1:5]", s);
}

TEST_F(ExprEmitterTest, List_Empty) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::List;
    EXPECT_EQ("[]", emitter.emit(e));
}

TEST_F(ExprEmitterTest, List_WithElements) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::List;
    e->values = {makeConst(int64_t{1}), makeConst(int64_t{2})};
    EXPECT_EQ("[1, 2]", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Tuple_Single) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Tuple;
    e->values = {makeConst(int64_t{1})};
    std::string s = emitter.emit(e);
    EXPECT_NE(std::string::npos, s.find(","));
}

TEST_F(ExprEmitterTest, Set_Empty) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Set;
    EXPECT_EQ("set()", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Dict_Simple) {
    auto e = std::make_shared<PyExpr>();
    e->kind   = PyExpr::Kind::Dict;
    e->keys   = {makeConst("a", false)};
    e->values = {makeConst(int64_t{1})};
    std::string s = emitter.emit(e);
    EXPECT_NE(std::string::npos, s.find("\"a\": 1"));
}

TEST_F(ExprEmitterTest, Starred) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Starred;
    e->children = {makeName("args")};
    EXPECT_EQ("*args", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Yield) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Yield;
    e->children = {makeConst(int64_t{42})};
    EXPECT_EQ("yield 42", emitter.emit(e));
}

TEST_F(ExprEmitterTest, YieldFrom) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::YieldFrom;
    e->children = {makeName("gen")};
    EXPECT_EQ("yield from gen", emitter.emit(e));
}

TEST_F(ExprEmitterTest, Await) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Await;
    e->children = {makeCall(makeName("coro"), {})};
    EXPECT_EQ("await coro()", emitter.emit(e));
}

// ─── PyStmtEmitter ───────────────────────────────────────────────────────────

class StmtEmitterTest : public ::testing::Test {
protected:
    PyWriter      writer;
    PyExprEmitter expr{writer};
    PyStmtEmitter stmt{writer, expr, {}};
};

TEST_F(StmtEmitterTest, Pass) {
    PyStmt s; s.kind = PyStmt::Kind::Pass;
    stmt.emitStmt(s);
    EXPECT_EQ("pass\n", writer.str());
}

TEST_F(StmtEmitterTest, Break) {
    PyStmt s; s.kind = PyStmt::Kind::Break;
    stmt.emitStmt(s);
    EXPECT_EQ("break\n", writer.str());
}

TEST_F(StmtEmitterTest, Continue) {
    PyStmt s; s.kind = PyStmt::Kind::Continue;
    stmt.emitStmt(s);
    EXPECT_EQ("continue\n", writer.str());
}

TEST_F(StmtEmitterTest, ReturnNone) {
    PyStmt s; s.kind = PyStmt::Kind::Return;
    stmt.emitStmt(s);
    EXPECT_EQ("return\n", writer.str());
}

TEST_F(StmtEmitterTest, ReturnValue) {
    auto s = makeReturn(makeConst(int64_t{42}));
    stmt.emitStmt(*s);
    EXPECT_EQ("return 42\n", writer.str());
}

TEST_F(StmtEmitterTest, Assign) {
    auto s = makeAssign({makeName("x", ExprCtx::Store)}, makeConst(int64_t{1}));
    stmt.emitStmt(*s);
    EXPECT_EQ("x = 1\n", writer.str());
}

TEST_F(StmtEmitterTest, MultiAssign) {
    auto s = makeAssign(
        {makeName("a", ExprCtx::Store), makeName("b", ExprCtx::Store)},
        makeConst(int64_t{0}));
    stmt.emitStmt(*s);
    EXPECT_EQ("a = b = 0\n", writer.str());
}

TEST_F(StmtEmitterTest, AugAssign) {
    PyStmt s;
    s.kind    = PyStmt::Kind::AugAssign;
    s.augOp   = AugOp::Add;
    s.targets = {makeName("x", ExprCtx::Store)};
    s.expr    = makeConst(int64_t{1});
    stmt.emitStmt(s);
    EXPECT_EQ("x += 1\n", writer.str());
}

TEST_F(StmtEmitterTest, Delete) {
    PyStmt s;
    s.kind = PyStmt::Kind::Delete;
    s.expr = makeName("x");
    stmt.emitStmt(s);
    EXPECT_EQ("del x\n", writer.str());
}

TEST_F(StmtEmitterTest, RaiseSimple) {
    PyStmt s;
    s.kind = PyStmt::Kind::Raise;
    s.expr = makeName("ValueError");
    stmt.emitStmt(s);
    EXPECT_EQ("raise ValueError\n", writer.str());
}

TEST_F(StmtEmitterTest, RaiseFrom) {
    PyStmt s;
    s.kind  = PyStmt::Kind::Raise;
    s.expr  = makeName("ValueError");
    s.expr2 = makeName("e");
    stmt.emitStmt(s);
    EXPECT_EQ("raise ValueError from e\n", writer.str());
}

TEST_F(StmtEmitterTest, GlobalDecl) {
    PyStmt s;
    s.kind  = PyStmt::Kind::Global;
    s.names = {"x", "y"};
    stmt.emitStmt(s);
    EXPECT_EQ("global x, y\n", writer.str());
}

TEST_F(StmtEmitterTest, NonlocalDecl) {
    PyStmt s;
    s.kind  = PyStmt::Kind::Nonlocal;
    s.names = {"count"};
    stmt.emitStmt(s);
    EXPECT_EQ("nonlocal count\n", writer.str());
}

TEST_F(StmtEmitterTest, Import) {
    PyStmt s;
    s.kind = PyStmt::Kind::Import;
    PyAlias a; a.name = "os";
    s.aliases = {a};
    stmt.emitStmt(s);
    EXPECT_EQ("import os\n", writer.str());
}

TEST_F(StmtEmitterTest, ImportAs) {
    PyStmt s;
    s.kind = PyStmt::Kind::Import;
    PyAlias a; a.name = "numpy"; a.asname = "np";
    s.aliases = {a};
    stmt.emitStmt(s);
    EXPECT_EQ("import numpy as np\n", writer.str());
}

TEST_F(StmtEmitterTest, ImportFrom) {
    PyStmt s;
    s.kind   = PyStmt::Kind::ImportFrom;
    s.module = "os.path";
    PyAlias a; a.name = "join";
    s.aliases = {a};
    stmt.emitStmt(s);
    EXPECT_EQ("from os.path import join\n", writer.str());
}

TEST_F(StmtEmitterTest, IfSimple) {
    PyStmt s;
    s.kind = PyStmt::Kind::If;
    s.expr = makeName("x");
    s.body = {makePass()};
    stmt.emitStmt(s);
    std::string out = writer.str();
    EXPECT_NE(std::string::npos, out.find("if x:"));
    EXPECT_NE(std::string::npos, out.find("pass"));
}

TEST_F(StmtEmitterTest, IfElse) {
    PyStmt s;
    s.kind   = PyStmt::Kind::If;
    s.expr   = makeName("flag");
    s.body   = {makeReturn(makeConst(int64_t{1}))};
    s.orelse = {makeReturn(makeConst(int64_t{0}))};
    stmt.emitStmt(s);
    std::string out = writer.str();
    EXPECT_NE(std::string::npos, out.find("if flag:"));
    EXPECT_NE(std::string::npos, out.find("else:"));
}

TEST_F(StmtEmitterTest, FunctionDef_Simple) {
    PyStmt s;
    s.kind     = PyStmt::Kind::FunctionDef;
    s.funcName = "foo";
    s.body     = {makePass()};
    stmt.emitStmt(s);
    std::string out = writer.str();
    EXPECT_NE(std::string::npos, out.find("def foo():"));
    EXPECT_NE(std::string::npos, out.find("pass"));
}

TEST_F(StmtEmitterTest, FunctionDef_WithArgs) {
    PyStmt s;
    s.kind     = PyStmt::Kind::FunctionDef;
    s.funcName = "add";
    s.body     = {makeReturn(makeName("x"))};
    auto a1 = std::make_shared<PyArg>(); a1->arg = "x";
    auto a2 = std::make_shared<PyArg>(); a2->arg = "y";
    s.funcArgs.args = {a1, a2};
    stmt.emitStmt(s);
    std::string out = writer.str();
    EXPECT_NE(std::string::npos, out.find("def add(x, y):"));
}

TEST_F(StmtEmitterTest, AsyncFunctionDef) {
    PyStmt s;
    s.kind     = PyStmt::Kind::AsyncFunctionDef;
    s.funcName = "fetch";
    s.body     = {makePass()};
    stmt.emitStmt(s);
    EXPECT_NE(std::string::npos, writer.str().find("async def fetch"));
}

TEST_F(StmtEmitterTest, ClassDef_Simple) {
    PyStmt s;
    s.kind      = PyStmt::Kind::ClassDef;
    s.className = "Foo";
    s.body      = {makePass()};
    stmt.emitStmt(s);
    std::string out = writer.str();
    EXPECT_NE(std::string::npos, out.find("class Foo:"));
}

TEST_F(StmtEmitterTest, ClassDef_WithBase) {
    PyStmt s;
    s.kind      = PyStmt::Kind::ClassDef;
    s.className = "Dog";
    s.bases     = {makeName("Animal")};
    s.body      = {makePass()};
    stmt.emitStmt(s);
    std::string out = writer.str();
    EXPECT_NE(std::string::npos, out.find("class Dog(Animal):"));
}

TEST_F(StmtEmitterTest, TryExcept) {
    PyStmt s;
    s.kind = PyStmt::Kind::Try;
    s.body = {makePass()};
    PyExceptHandler h;
    h.type = makeName("Exception");
    h.name = "e";
    h.body = {makePass()};
    s.handlers = {h};
    stmt.emitStmt(s);
    std::string out = writer.str();
    EXPECT_NE(std::string::npos, out.find("try:"));
    EXPECT_NE(std::string::npos, out.find("except Exception as e:"));
}

TEST_F(StmtEmitterTest, TryFinally) {
    PyStmt s;
    s.kind      = PyStmt::Kind::Try;
    s.body      = {makePass()};
    s.finalbody = {makePass()};
    stmt.emitStmt(s);
    std::string out = writer.str();
    EXPECT_NE(std::string::npos, out.find("finally:"));
}

// ─── PyFileEmitter ───────────────────────────────────────────────────────────

TEST(PyFileEmitter, EmptyModule) {
    PyModule mod;
    mod.pythonMajor = 3;
    mod.pythonMinor = 10;
    mod.filename    = "empty.py";

    PyFileEmitter em;
    auto result = em.emit(mod);
    EXPECT_FALSE(result.source.empty());
    EXPECT_TRUE(result.warnings.empty());
}

TEST(PyFileEmitter, ModuleWithImport) {
    PyModule mod;
    mod.pythonMajor = 3;
    mod.pythonMinor = 10;

    PyStmt imp;
    imp.kind = PyStmt::Kind::Import;
    PyAlias a; a.name = "os";
    imp.aliases = {a};
    mod.body.push_back(std::make_shared<PyStmt>(imp));

    PyEmitOptions opts;
    opts.emitFileHeader = false;
    PyFileEmitter em(opts);
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("import os"));
}

TEST(PyFileEmitter, HeaderComment) {
    PyModule mod;
    mod.pythonMajor = 3;
    mod.pythonMinor = 10;
    mod.filename    = "test.py";

    PyEmitOptions opts;
    opts.emitFileHeader = true;
    PyFileEmitter em(opts);
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("Auto-generated"));
    EXPECT_NE(std::string::npos, result.source.find("test.py"));
}

TEST(PyFileEmitter, FunctionDefWithBlanks) {
    PyModule mod;
    mod.pythonMajor = 3;
    mod.pythonMinor = 10;

    {
        auto s = std::make_shared<PyStmt>();
        s->kind     = PyStmt::Kind::FunctionDef;
        s->funcName = "foo";
        s->body     = {makePass()};
        mod.body.push_back(s);
    }
    {
        auto s = std::make_shared<PyStmt>();
        s->kind     = PyStmt::Kind::FunctionDef;
        s->funcName = "bar";
        s->body     = {makePass()};
        mod.body.push_back(s);
    }

    PyEmitOptions opts;
    opts.emitFileHeader = false;
    opts.blankLinesBetweenTopLevel = 2;
    PyFileEmitter em(opts);
    auto result = em.emit(mod);

    // Should have 2 blank lines between functions
    EXPECT_NE(std::string::npos, result.source.find("\n\n"));
}

TEST(PyFileEmitter, SortedImports) {
    PyModule mod;
    mod.pythonMajor = 3;

    auto addImp = [&](const std::string& name) {
        PyStmt s; s.kind = PyStmt::Kind::Import;
        PyAlias a; a.name = name; s.aliases = {a};
        mod.body.push_back(std::make_shared<PyStmt>(s));
    };
    addImp("os");
    addImp("abc");
    addImp("sys");

    PyEmitOptions opts;
    opts.emitFileHeader = false;
    opts.sortImports    = true;
    PyFileEmitter em(opts);
    auto result = em.emit(mod);

    size_t posAbc = result.source.find("abc");
    size_t posOs  = result.source.find("import os");
    size_t posSys = result.source.find("import sys");
    EXPECT_LT(posAbc, posOs);
    EXPECT_LT(posOs,  posSys);
}

TEST(PyFileEmitter, FullFunction) {
    // def factorial(n):
    //     if n == 0:
    //         return 1
    //     return n * factorial(n - 1)

    PyModule mod;
    mod.pythonMajor = 3;
    mod.pythonMinor = 10;

    auto funcStmt = std::make_shared<PyStmt>();
    funcStmt->kind     = PyStmt::Kind::FunctionDef;
    funcStmt->funcName = "factorial";
    auto arg = std::make_shared<PyArg>(); arg->arg = "n";
    funcStmt->funcArgs.args = {arg};

    // if n == 0: return 1
    auto ifStmt = std::make_shared<PyStmt>();
    ifStmt->kind = PyStmt::Kind::If;
    auto cmp = std::make_shared<PyExpr>();
    cmp->kind    = PyExpr::Kind::Compare;
    cmp->cmpOps  = {CmpOp::Eq};
    cmp->children= {makeName("n")};
    cmp->values  = {makeConst(int64_t{0})};
    ifStmt->expr = cmp;
    ifStmt->body = {makeReturn(makeConst(int64_t{1}))};

    // return n * factorial(n - 1)
    auto subExpr = std::make_shared<PyExpr>();
    subExpr->kind  = PyExpr::Kind::BinOp;
    subExpr->binOp = BinOp::Sub;
    subExpr->children = {makeName("n"), makeConst(int64_t{1})};
    auto recCall = makeCall(makeName("factorial"), {subExpr});
    auto mulExpr = std::make_shared<PyExpr>();
    mulExpr->kind  = PyExpr::Kind::BinOp;
    mulExpr->binOp = BinOp::Mult;
    mulExpr->children = {makeName("n"), recCall};

    funcStmt->body = {ifStmt, makeReturn(mulExpr)};
    mod.body.push_back(funcStmt);

    PyEmitOptions opts;
    opts.emitFileHeader = false;
    PyFileEmitter em(opts);
    auto result = em.emit(mod);

    EXPECT_NE(std::string::npos, result.source.find("def factorial(n):"));
    EXPECT_NE(std::string::npos, result.source.find("if n == 0:"));
    EXPECT_NE(std::string::npos, result.source.find("return 1"));
    EXPECT_NE(std::string::npos, result.source.find("factorial(n - 1)"));
}
