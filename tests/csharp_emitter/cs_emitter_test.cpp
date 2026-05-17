/**
 * @file tests/csharp_emitter/cs_emitter_test.cpp
 * @brief Unit tests for the C# source emitter.
 *
 * Tests cover:
 *   - CsWriter (indentation, brace blocks, string literals, type aliases)
 *   - CsExprEmitter (all expr variants, precedence, type emission)
 *   - CsStmtEmitter (all stmt kinds → correct C# syntax)
 *   - CsTypeEmitter (class/struct/enum/interface/delegate headers, fields, methods)
 *   - CsFileEmitter (full file layout, using directives, namespace, golden files)
 */

#include <memory>
#include "retdec/csharp_emitter/cs_expr_emitter.h"
#include "retdec/csharp_emitter/cs_file_emitter.h"
#include "retdec/csharp_emitter/cs_stmt_emitter.h"
#include "retdec/csharp_emitter/cs_type_emitter.h"
#include "retdec/csharp_emitter/cs_writer.h"

#include "retdec/cil_reconstruct/cil_stack_sim.h"
#include "retdec/cil_reconstruct/cil_var_recovery.h"

#include "retdec/bc_module/bc_module.h"
#include "retdec/bc_module/bc_type.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <string>

using namespace retdec::csharp_emitter;
using namespace retdec::cil_reconstruct;
using namespace retdec::bc_module;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

static CilExprPtr makeConst(int64_t v) {
    return makeExprConst(v, types::Int());
}

static CilExprPtr makeConstStr(const std::string& s) {
    ExprConst c; c.strVal = s; c.isString = true; c.type = types::ClrString();
    return std::make_shared<CilExpr>(std::move(c), types::ClrString());
}

static CilExprPtr makeLocal(uint32_t idx, const std::string& name) {
    return makeExprLocal(idx, name, types::Int());
}

static CilExprPtr makeArg(uint32_t idx, const std::string& name) {
    return makeExprArg(idx, name, types::Int());
}

static CilExprPtr makeBinExpr(BinOpKind op, CilExprPtr l, CilExprPtr r) {
    ExprBinOp b{op, l, r, types::Int()};
    return std::make_shared<CilExpr>(std::move(b), types::Int());
}

// ─── CsWriter tests ──────────────────────────────────────────────────────────

TEST(CsWriter, LineEmitsIndentedText) {
    CsWriter w;
    w.line("int x = 1;");
    EXPECT_EQ("int x = 1;\n", w.str());
}

TEST(CsWriter, IndentIncreasesPrefix) {
    CsWriter w;
    w.indent();
    w.line("x;");
    EXPECT_EQ("    x;\n", w.str());
}

TEST(CsWriter, BlockGuardManagesBraces) {
    CsWriter w;
    w.line("class Foo");
    {
        auto g = w.block();
        w.line("int x;");
    }
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "class Foo"));
    EXPECT_TRUE(contains(s, "{"));
    EXPECT_TRUE(contains(s, "    int x;"));
    EXPECT_TRUE(contains(s, "}"));
}

TEST(CsWriter, NestedBlocks) {
    CsWriter w;
    {
        auto g = w.block();
        {
            auto g2 = w.block();
            w.line("deep;");
        }
    }
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "        deep;"));
}

TEST(CsWriter, BlankLineEmitsNewline) {
    CsWriter w;
    w.line("a;");
    w.blank();
    w.line("b;");
    EXPECT_TRUE(contains(w.str(), "a;\n\nb;\n"));
}

TEST(CsWriter, SafeNameEscapesKeyword) {
    EXPECT_EQ("@int",    CsWriter::safeName("int"));
    EXPECT_EQ("@class",  CsWriter::safeName("class"));
    EXPECT_EQ("@string", CsWriter::safeName("string"));
    EXPECT_EQ("foo",     CsWriter::safeName("foo"));
}

TEST(CsWriter, SafeNamePrefixesDigit) {
    EXPECT_EQ("_1name", CsWriter::safeName("1name"));
}

TEST(CsWriter, IsKeyword) {
    EXPECT_TRUE(CsWriter::isKeyword("class"));
    EXPECT_TRUE(CsWriter::isKeyword("async"));
    EXPECT_TRUE(CsWriter::isKeyword("var"));
    EXPECT_FALSE(CsWriter::isKeyword("foo"));
    EXPECT_FALSE(CsWriter::isKeyword("MyClass"));
}

TEST(CsWriter, StringLiteralSimple) {
    CsWriter w;
    EXPECT_EQ("\"hello\"", w.stringLiteral("hello"));
}

TEST(CsWriter, StringLiteralEscapesSpecial) {
    CsWriter w;
    std::string s = w.stringLiteral("line1\nline2");
    // Should use verbatim or escaped
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(contains(s, "line1") && contains(s, "line2"));
}

TEST(CsWriter, StringLiteralWithBackslash) {
    CsWriter w;
    std::string s = w.stringLiteral("C:\\path\\file");
    EXPECT_TRUE(contains(s, "@\"C:\\path\\file\"") || contains(s, "C:\\\\path\\\\file"));
}

TEST(CsWriter, CharLiteralAscii) {
    EXPECT_EQ("'A'",  CsWriter::charLiteral('A'));
    EXPECT_EQ("'\\n'", CsWriter::charLiteral('\n'));
    EXPECT_EQ("'\\''", CsWriter::charLiteral('\''));
}

TEST(CsWriter, ClrToCsharpTypeAliases) {
    EXPECT_EQ("int",    CsWriter::clrToCsharpType("System.Int32"));
    EXPECT_EQ("string", CsWriter::clrToCsharpType("System.String"));
    EXPECT_EQ("bool",   CsWriter::clrToCsharpType("System.Boolean"));
    EXPECT_EQ("long",   CsWriter::clrToCsharpType("System.Int64"));
    EXPECT_EQ("double", CsWriter::clrToCsharpType("System.Double"));
    EXPECT_EQ("void",   CsWriter::clrToCsharpType("System.Void"));
    EXPECT_EQ("object", CsWriter::clrToCsharpType("System.Object"));
    EXPECT_EQ("Foo",    CsWriter::clrToCsharpType("MyNs.Foo"));
}

TEST(CsWriter, ClrToCsharpTypeStripsSystemPrefix) {
    EXPECT_EQ("IO.Stream", CsWriter::clrToCsharpType("System.IO.Stream"));
}

TEST(CsWriter, XmlDocEmitsCorrectly) {
    CsWriter w;
    w.xmlDoc("The summary.",
             {{"param1", "First parameter"}, {"param2", "Second"}},
             "The return value.");
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "/// <summary>"));
    EXPECT_TRUE(contains(s, "The summary."));
    EXPECT_TRUE(contains(s, "<param name=\"param1\">First parameter</param>"));
    EXPECT_TRUE(contains(s, "<returns>The return value.</returns>"));
}

TEST(CsWriter, CommentLine) {
    CsWriter w;
    w.comment("This is a comment");
    EXPECT_EQ("// This is a comment\n", w.str());
}

TEST(CsWriter, Reset) {
    CsWriter w;
    w.line("test;");
    w.reset();
    EXPECT_EQ("", w.str());
    EXPECT_EQ(0, w.indentLevel());
}

// ─── CsExprEmitter tests ─────────────────────────────────────────────────────

TEST(CsExprEmitter, EmitIntConst) {
    CsWriter w;
    CsExprEmitter e(w);
    EXPECT_EQ("42", e.emit(makeConst(42)));
    EXPECT_EQ("-1", e.emit(makeConst(-1)));
    EXPECT_EQ("0",  e.emit(makeConst(0)));
}

TEST(CsExprEmitter, EmitNull) {
    CsWriter w;
    CsExprEmitter e(w);
    EXPECT_EQ("null", e.emit(makeExprNull()));
}

TEST(CsExprEmitter, EmitLocal) {
    CsWriter w;
    CsExprEmitter e(w);
    EXPECT_EQ("counter", e.emit(makeLocal(0, "counter")));
}

TEST(CsExprEmitter, EmitArg) {
    CsWriter w;
    CsExprEmitter e(w);
    EXPECT_EQ("x", e.emit(makeArg(0, "x")));
}

TEST(CsExprEmitter, EmitBinOpAdd) {
    CsWriter w;
    CsExprEmitter e(w);
    auto expr = makeBinExpr(BinOpKind::Add, makeConst(1), makeConst(2));
    EXPECT_EQ("1 + 2", e.emit(expr));
}

TEST(CsExprEmitter, EmitBinOpSubParenthesised) {
    CsWriter w;
    CsExprEmitter e(w);
    // (1 + 2) - 3: the (1+2) sub-expression at lower prec needs parens in some contexts
    auto add  = makeBinExpr(BinOpKind::Add, makeConst(1), makeConst(2));
    auto mul  = makeBinExpr(BinOpKind::Mul, add, makeConst(3));
    std::string s = e.emit(mul);
    // The add should be parenthesised inside a mul context
    EXPECT_TRUE(contains(s, "(1 + 2)") || contains(s, "1 + 2"));
    EXPECT_TRUE(contains(s, "* 3"));
}

TEST(CsExprEmitter, EmitBinOpEq) {
    CsWriter w;
    CsExprEmitter e(w);
    auto expr = makeBinExpr(BinOpKind::Eq, makeLocal(0, "x"), makeConst(0));
    EXPECT_EQ("x == 0", e.emit(expr));
}

TEST(CsExprEmitter, EmitUnOpNeg) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprUnOp u{UnOpKind::Neg, makeLocal(0, "x"), types::Int()};
    auto expr = std::make_shared<CilExpr>(std::move(u), types::Int());
    EXPECT_EQ("-x", e.emit(expr));
}

TEST(CsExprEmitter, EmitConvI4) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprUnOp u{UnOpKind::ConvI4, makeLocal(0, "x"), types::Int()};
    auto expr = std::make_shared<CilExpr>(std::move(u), types::Int());
    EXPECT_EQ("(int)x", e.emit(expr));
}

TEST(CsExprEmitter, EmitStringConst) {
    CsWriter w;
    CsExprEmitter e(w);
    auto expr = makeConstStr("Hello, World!");
    EXPECT_EQ("\"Hello, World!\"", e.emit(expr));
}

TEST(CsExprEmitter, EmitCast) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprCast c{makeLocal(0, "obj"), types::Int()};
    auto expr = std::make_shared<CilExpr>(std::move(c), types::Int());
    EXPECT_EQ("(int)obj", e.emit(expr));
}

TEST(CsExprEmitter, EmitIsinst) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprIsinst ii{makeLocal(0, "obj"), types::ClrString()};
    auto expr = std::make_shared<CilExpr>(std::move(ii), types::ClrString());
    std::string s = e.emit(expr);
    EXPECT_TRUE(contains(s, " is "));
    EXPECT_TRUE(contains(s, "string") || contains(s, "String"));
}

TEST(CsExprEmitter, EmitFieldAccess) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprField ef{makeLocal(0, "obj"), "MyClass", "Name", types::ClrString()};
    auto expr = std::make_shared<CilExpr>(std::move(ef), types::ClrString());
    EXPECT_EQ("obj.Name", e.emit(expr));
}

TEST(CsExprEmitter, EmitStaticFieldAccess) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprSField esf{"System.Console", "Out", types::ClrObject()};
    auto expr = std::make_shared<CilExpr>(std::move(esf), types::ClrObject());
    std::string s = e.emit(expr);
    EXPECT_TRUE(contains(s, "Console.Out") || contains(s, "Out"));
}

TEST(CsExprEmitter, EmitCall) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprCall call;
    call.className  = "System.Console";
    call.methodName = "WriteLine";
    call.args.push_back(makeConstStr("hello"));
    call.retType = types::Void();
    auto expr = std::make_shared<CilExpr>(std::move(call), types::Void());
    std::string s = e.emit(expr);
    EXPECT_TRUE(contains(s, "WriteLine"));
    EXPECT_TRUE(contains(s, "\"hello\""));
}

TEST(CsExprEmitter, EmitInstanceCall) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprCall call;
    call.obj        = makeLocal(0, "sb");
    call.methodName = "Append";
    call.args.push_back(makeConstStr(" world"));
    call.retType = types::ClrObject();
    auto expr = std::make_shared<CilExpr>(std::move(call), types::ClrObject());
    std::string s = e.emit(expr);
    EXPECT_TRUE(contains(s, "sb.Append("));
}

TEST(CsExprEmitter, EmitNewobj) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprNewobj no{"System.Text.StringBuilder", "", {}, types::ClrObject()};
    auto expr = std::make_shared<CilExpr>(std::move(no), types::ClrObject());
    std::string s = e.emit(expr);
    EXPECT_TRUE(contains(s, "new "));
    EXPECT_TRUE(contains(s, "StringBuilder") || contains(s, "System.Text.StringBuilder"));
    EXPECT_TRUE(contains(s, "()"));
}

TEST(CsExprEmitter, EmitNewarr) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprNewarr na{types::Int(), makeConst(10)};
    auto expr = std::make_shared<CilExpr>(std::move(na), types::Array(types::Int()));
    std::string s = e.emit(expr);
    EXPECT_TRUE(contains(s, "new int[10]"));
}

TEST(CsExprEmitter, EmitLdelem) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprLdelem le{makeLocal(0, "arr"), makeConst(0), types::Int()};
    auto expr = std::make_shared<CilExpr>(std::move(le), types::Int());
    EXPECT_EQ("arr[0]", e.emit(expr));
}

TEST(CsExprEmitter, EmitDefault) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprDefault d{types::Int()};
    auto expr = std::make_shared<CilExpr>(std::move(d), types::Int());
    std::string s = e.emit(expr);
    EXPECT_TRUE(contains(s, "default"));
}

TEST(CsExprEmitter, EmitSizeof) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprSizeof sz{types::Int()};
    auto expr = std::make_shared<CilExpr>(std::move(sz), types::UInt());
    EXPECT_EQ("sizeof(int)", e.emit(expr));
}

TEST(CsExprEmitter, EmitLdToken) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprLdToken lt{"System.Int32"};
    auto expr = std::make_shared<CilExpr>(std::move(lt), types::ClrObject());
    EXPECT_EQ("typeof(System.Int32)", e.emit(expr));
}

TEST(CsExprEmitter, EmitLocAlloc) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprLocAlloc la{makeConst(16), types::UByte()};
    auto expr = std::make_shared<CilExpr>(std::move(la), types::Long());
    EXPECT_EQ("stackalloc byte[16]", e.emit(expr));
}

TEST(CsExprEmitter, EmitTernary) {
    CsWriter w;
    CsExprEmitter e(w);
    ExprTernary t{makeLocal(0, "cond"), makeConst(1), makeConst(0), types::Int()};
    auto expr = std::make_shared<CilExpr>(std::move(t), types::Int());
    std::string s = e.emit(expr);
    EXPECT_TRUE(contains(s, " ? "));
    EXPECT_TRUE(contains(s, " : "));
}

TEST(CsExprEmitter, EmitTypeIntAlias) {
    CsWriter w;
    CsExprEmitter e(w);
    EXPECT_EQ("int",    e.emitType(types::Int()));
    EXPECT_EQ("long",   e.emitType(types::Long()));
    EXPECT_EQ("bool",   e.emitType(types::Bool()));
    EXPECT_EQ("double", e.emitType(types::Double()));
    EXPECT_EQ("string", e.emitType(types::ClrString()));
    EXPECT_EQ("object", e.emitType(types::ClrObject()));
    EXPECT_EQ("void",   e.emitType(types::Void()));
}

TEST(CsExprEmitter, EmitNullExprReturnsNull) {
    CsWriter w;
    CsExprEmitter e(w);
    EXPECT_EQ("null", e.emit(nullptr));
}

TEST(CsExprEmitter, PrecedenceParenthesesMulBeforeAdd) {
    CsWriter w;
    CsExprEmitter e(w);
    // (a + b) * c → requires parens around a+b
    auto add = makeBinExpr(BinOpKind::Add, makeLocal(0, "a"), makeLocal(1, "b"));
    auto mul = makeBinExpr(BinOpKind::Mul, add, makeLocal(2, "c"));
    std::string s = e.emit(mul);
    EXPECT_TRUE(contains(s, "(a + b) * c"));
}

TEST(CsExprEmitter, PrecedenceNoParensWhenNotNeeded) {
    CsWriter w;
    CsExprEmitter e(w);
    // a + b + c → no parens needed
    auto add1 = makeBinExpr(BinOpKind::Add, makeLocal(0, "a"), makeLocal(1, "b"));
    auto add2 = makeBinExpr(BinOpKind::Add, add1, makeLocal(2, "c"));
    std::string s = e.emit(add2);
    EXPECT_FALSE(contains(s, "("));
}

TEST(CsExprEmitter, EmitArgListMultiple) {
    CsWriter w;
    CsExprEmitter e(w);
    auto args = std::vector<CilExprPtr>{makeConst(1), makeConst(2), makeConst(3)};
    EXPECT_EQ("1, 2, 3", e.emitArgList(args));
}

// ─── CsStmtEmitter tests ──────────────────────────────────────────────────────

TEST(CsStmtEmitter, EmitLocalDeclWithInit) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind     = StmtKind::LocalDecl;
    s.declType = types::Int();
    s.target   = makeLocal(0, "x");
    s.expr     = makeConst(42);
    st.emitStmt(s);
    EXPECT_TRUE(contains(w.str(), "x = 42;"));
}

TEST(CsStmtEmitter, EmitLocalDeclWithoutInit) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind     = StmtKind::LocalDecl;
    s.declType = types::Int();
    s.target   = makeLocal(0, "n");
    st.emitStmt(s);
    EXPECT_TRUE(contains(w.str(), "n;"));
}

TEST(CsStmtEmitter, EmitAssign) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind   = StmtKind::Assign;
    s.target = makeLocal(0, "x");
    s.expr   = makeConst(99);
    st.emitStmt(s);
    EXPECT_EQ("x = 99;\n", w.str());
}

TEST(CsStmtEmitter, EmitReturnVoid) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind = StmtKind::Return;
    st.emitStmt(s);
    EXPECT_EQ("return;\n", w.str());
}

TEST(CsStmtEmitter, EmitReturnValue) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind = StmtKind::Return;
    s.expr = makeConst(0);
    st.emitStmt(s);
    EXPECT_EQ("return 0;\n", w.str());
}

TEST(CsStmtEmitter, EmitThrow) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind = StmtKind::Throw;
    ExprNewobj no{"System.Exception", "", {}, types::ClrObject()};
    s.expr = std::make_shared<CilExpr>(std::move(no), types::ClrObject());
    st.emitStmt(s);
    EXPECT_TRUE(contains(w.str(), "throw new Exception()"));
}

TEST(CsStmtEmitter, EmitRethrow) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind = StmtKind::Rethrow;
    st.emitStmt(s);
    EXPECT_EQ("throw;\n", w.str());
}

TEST(CsStmtEmitter, EmitGoto) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind      = StmtKind::Goto;
    s.labelName = "L10";
    st.emitStmt(s);
    EXPECT_EQ("goto L10;\n", w.str());
}

TEST(CsStmtEmitter, EmitExprStmtCallSemicolon) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind = StmtKind::ExprStmt;
    ExprCall call;
    call.className  = "Console";
    call.methodName = "WriteLine";
    call.args.push_back(makeConst(1));
    call.retType = types::Void();
    s.expr = std::make_shared<CilExpr>(std::move(call), types::Void());
    st.emitStmt(s);
    EXPECT_TRUE(contains(w.str(), "Console.WriteLine(1);"));
}

TEST(CsStmtEmitter, EmitYieldReturn) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind = StmtKind::YieldReturn;
    s.expr = makeConst(5);
    st.emitStmt(s);
    EXPECT_EQ("yield return 5;\n", w.str());
}

TEST(CsStmtEmitter, EmitYieldBreak) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt s;
    s.kind = StmtKind::YieldBreak;
    st.emitStmt(s);
    EXPECT_EQ("yield break;\n", w.str());
}

TEST(CsStmtEmitter, EmitTryCatchFinally) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt tryStmt;
    tryStmt.kind = StmtKind::Try;
    // Try body
    CilStmt retStmt;
    retStmt.kind = StmtKind::Return;
    tryStmt.tryBody.push_back(std::move(retStmt));
    // Catch clause
    CilStmt::CatchClause cc;
    cc.catchType = types::ClrObject();
    cc.varName   = "ex";
    CilStmt rethrow;
    rethrow.kind = StmtKind::Rethrow;
    cc.body.push_back(std::move(rethrow));
    tryStmt.catches.push_back(std::move(cc));
    // Finally
    CilStmt nop;
    nop.kind = StmtKind::ExprStmt;
    tryStmt.finallyBody.push_back(std::move(nop));

    st.emitStmt(tryStmt);
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "try"));
    EXPECT_TRUE(contains(s, "catch"));
    EXPECT_TRUE(contains(s, "finally"));
}

TEST(CsStmtEmitter, EmitForEach) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt fe;
    fe.kind        = StmtKind::ForEach;
    fe.iterVarName = "item";
    fe.iterVarType = types::Int();
    fe.expr        = makeLocal(0, "items");
    CilStmt body;
    body.kind = StmtKind::ExprStmt;
    fe.loopBody.push_back(std::move(body));

    st.emitStmt(fe);
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "foreach"));
    EXPECT_TRUE(contains(s, "item"));
    EXPECT_TRUE(contains(s, "items"));
}

TEST(CsStmtEmitter, EmitLock) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt ls;
    ls.kind = StmtKind::Lock;
    ls.expr = makeLocal(0, "mutex");
    CilStmt body;
    body.kind = StmtKind::ExprStmt;
    ls.loopBody.push_back(std::move(body));

    st.emitStmt(ls);
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "lock (mutex)"));
}

TEST(CsStmtEmitter, EmitUsing) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt us;
    us.kind        = StmtKind::Using;
    us.iterVarName = "stream";
    ExprNewobj no{"System.IO.FileStream", "", {makeConstStr("file.txt")}, types::ClrObject()};
    us.expr = std::make_shared<CilExpr>(std::move(no), types::ClrObject());

    st.emitStmt(us);
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "using"));
    EXPECT_TRUE(contains(s, "stream"));
}

TEST(CsStmtEmitter, EmitSwitch) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt sw;
    sw.kind = StmtKind::Switch;
    sw.expr = makeLocal(0, "x");

    CilStmt::SwitchCase c0;
    c0.values.push_back(makeConst(1));
    CilStmt ret1;
    ret1.kind = StmtKind::Return;
    ret1.expr = makeConstStr("one");
    c0.body.push_back(std::move(ret1));
    sw.cases.push_back(std::move(c0));

    CilStmt::SwitchCase def;
    // default case: no values
    CilStmt ret2;
    ret2.kind = StmtKind::Return;
    ret2.expr = makeConstStr("other");
    def.body.push_back(std::move(ret2));
    sw.cases.push_back(std::move(def));

    st.emitStmt(sw);
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "switch (x)"));
    EXPECT_TRUE(contains(s, "case 1:"));
    EXPECT_TRUE(contains(s, "default:"));
}

TEST(CsStmtEmitter, EmitStackalloc) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);

    CilStmt sa;
    sa.kind   = StmtKind::Stackalloc;
    sa.target = makeLocal(0, "buf");
    ExprLocAlloc la{makeConst(32), types::UByte()};
    sa.expr = std::make_shared<CilExpr>(std::move(la), types::Long());
    st.emitStmt(sa);
    std::string s = w.str();
    EXPECT_TRUE(contains(s, "stackalloc byte[32]"));
}

// ─── CsTypeEmitter tests ──────────────────────────────────────────────────────

static BcClass makeSimpleClass(const std::string& name, const std::string& ns = "") {
    BcClass cls;
    cls.name        = name;
    cls.fqName      = ns.empty() ? name : ns + "." + name;
    cls.packageName = ns;
    cls.access      = BcAccess::Public;
    return cls;
}

TEST(CsTypeEmitter, EmitsClassHeader) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);
    CsTypeEmitter te(w, ex, st);

    BcClass cls = makeSimpleClass("Calculator");
    BcModule mod("M", SourceLang::CSharp);
    std::unordered_map<std::string, CilReconstructResult> results;
    te.emitClass(cls, results, mod);

    std::string s = w.str();
    EXPECT_TRUE(contains(s, "public"));
    EXPECT_TRUE(contains(s, "class Calculator"));
    EXPECT_TRUE(contains(s, "{"));
    EXPECT_TRUE(contains(s, "}"));
}

TEST(CsTypeEmitter, EmitsEnum) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);
    CsTypeEmitter te(w, ex, st);

    BcClass cls = makeSimpleClass("Color");
    cls.isEnum = true;
    cls.enumConstants = {"Red", "Green", "Blue"};
    BcField rField; rField.name = "Red";   rField.constantIntValue = 0;
    BcField gField; gField.name = "Green"; gField.constantIntValue = 1;
    BcField bField; bField.name = "Blue";  bField.constantIntValue = 2;
    cls.fields = {rField, gField, bField};

    BcModule mod("M", SourceLang::CSharp);
    std::unordered_map<std::string, CilReconstructResult> results;
    te.emitClass(cls, results, mod);

    std::string s = w.str();
    EXPECT_TRUE(contains(s, "enum Color"));
    EXPECT_TRUE(contains(s, "Red = 0"));
    EXPECT_TRUE(contains(s, "Green = 1"));
    EXPECT_TRUE(contains(s, "Blue = 2"));
}

TEST(CsTypeEmitter, EmitsInterface) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);
    CsTypeEmitter te(w, ex, st);

    BcClass cls = makeSimpleClass("IFoo");
    cls.isInterface = true;

    BcMethod m;
    m.name = "DoSomething";
    m.descriptor.returnType = std::make_shared<BcType>(types::Void());
    m.isAbstract = true;
    cls.methods.push_back(std::move(m));

    BcModule mod("M", SourceLang::CSharp);
    std::unordered_map<std::string, CilReconstructResult> results;
    te.emitClass(cls, results, mod);

    std::string s = w.str();
    EXPECT_TRUE(contains(s, "interface IFoo"));
    EXPECT_TRUE(contains(s, "DoSomething"));
}

TEST(CsTypeEmitter, EmitsFieldsCorrectly) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);
    CsTypeEmitter te(w, ex, st);

    BcClass cls = makeSimpleClass("MyClass");
    BcField f1; f1.name = "_count"; f1.type = types::Int();
    f1.access = BcAccess::Private;
    BcField f2; f2.name = "MaxValue"; f2.type = types::Int();
    f2.access = static_cast<BcAccess>(
        static_cast<uint32_t>(BcAccess::Public) |
        static_cast<uint32_t>(BcAccess::Static) |
        static_cast<uint32_t>(BcAccess::Final));
    f2.constantIntValue = 100;
    cls.fields = {f1, f2};

    BcModule mod("M", SourceLang::CSharp);
    std::unordered_map<std::string, CilReconstructResult> results;
    te.emitClass(cls, results, mod);

    std::string s = w.str();
    EXPECT_TRUE(contains(s, "_count"));
    EXPECT_TRUE(contains(s, "MaxValue"));
}

TEST(CsTypeEmitter, EmitsGenericClass) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);
    CsTypeEmitter te(w, ex, st);

    BcClass cls = makeSimpleClass("List");
    cls.typeParams = {"T"};

    BcModule mod("M", SourceLang::CSharp);
    std::unordered_map<std::string, CilReconstructResult> results;
    te.emitClass(cls, results, mod);

    std::string s = w.str();
    EXPECT_TRUE(contains(s, "List<T>") || contains(s, "class List"));
    EXPECT_TRUE(contains(s, "<T>"));
}

TEST(CsTypeEmitter, EmitsSealedClass) {
    CsWriter w;
    CsExprEmitter ex(w);
    CsStmtEmitter st(w, ex);
    CsTypeEmitter te(w, ex, st);

    BcClass cls = makeSimpleClass("MyRecord");
    cls.access = static_cast<BcAccess>(
        static_cast<uint32_t>(BcAccess::Public) |
        static_cast<uint32_t>(BcAccess::Sealed));

    BcModule mod("M", SourceLang::CSharp);
    std::unordered_map<std::string, CilReconstructResult> results;
    te.emitClass(cls, results, mod);

    std::string s = w.str();
    EXPECT_TRUE(contains(s, "sealed"));
}

// ─── CsFileEmitter tests ──────────────────────────────────────────────────────

TEST(CsFileEmitter, EmitsAutoGenHeader) {
    BcModule module("TestLib", SourceLang::CSharp);
    BcClass& cls = module.addClass(makeSimpleClass("Foo", "TestLib"));
    (void)cls;

    CsFileEmitter emitter;
    std::unordered_map<std::string, CilReconstructResult> results;
    std::string src = emitter.emitClass(cls, module, results);

    EXPECT_TRUE(contains(src, "<auto-generated"));
    EXPECT_TRUE(contains(src, "TestLib"));
}

TEST(CsFileEmitter, EmitsNullableEnable) {
    BcModule module("M", SourceLang::CSharp);
    BcClass& cls = module.addClass(makeSimpleClass("Bar"));

    CsFileEmitter emitter;
    std::unordered_map<std::string, CilReconstructResult> results;
    std::string src = emitter.emitClass(cls, module, results);

    EXPECT_TRUE(contains(src, "#nullable enable"));
}

TEST(CsFileEmitter, EmitsUsingDirectives) {
    BcModule module("M", SourceLang::CSharp);
    BcClass& cls = module.addClass(makeSimpleClass("Baz"));

    CsFileEmitter emitter;
    std::unordered_map<std::string, CilReconstructResult> results;
    std::string src = emitter.emitClass(cls, module, results);

    EXPECT_TRUE(contains(src, "using System;"));
}

TEST(CsFileEmitter, EmitsFileScopedNamespace) {
    BcModule module("M", SourceLang::CSharp);
    BcClass& cls = module.addClass(makeSimpleClass("MyClass", "MyApp"));

    CsEmitOptions opts;
    opts.fileScopedNamespace = true;
    opts.csharpVersion       = 10;
    CsFileEmitter emitter(opts);

    std::unordered_map<std::string, CilReconstructResult> results;
    std::string src = emitter.emitClass(cls, module, results);

    EXPECT_TRUE(contains(src, "namespace MyApp;"));
    // No closing brace for namespace in file-scoped form
    EXPECT_FALSE(contains(src, "namespace MyApp\n{"));
}

TEST(CsFileEmitter, EmitsClassicNamespace) {
    BcModule module("M", SourceLang::CSharp);
    BcClass& cls = module.addClass(makeSimpleClass("MyClass", "MyApp"));

    CsEmitOptions opts;
    opts.fileScopedNamespace = false;
    CsFileEmitter emitter(opts);

    std::unordered_map<std::string, CilReconstructResult> results;
    std::string src = emitter.emitClass(cls, module, results);

    EXPECT_TRUE(contains(src, "namespace MyApp"));
    EXPECT_TRUE(contains(src, "{"));
    EXPECT_TRUE(contains(src, "}"));
}

TEST(CsFileEmitter, EmitModuleProducesOneFilePerClass) {
    BcModule module("M", SourceLang::CSharp);
    module.addClass(makeSimpleClass("Alpha", "Ns"));
    module.addClass(makeSimpleClass("Beta",  "Ns"));
    module.addClass(makeSimpleClass("Gamma", "Ns"));

    CsFileEmitter emitter;
    std::unordered_map<std::string, CilReconstructResult> results;
    auto result = emitter.emitModule(module, results);

    EXPECT_EQ(3, result.emittedClasses);
    EXPECT_EQ(3, (int)result.files.size());
}

TEST(CsFileEmitter, SkipsCompilerGeneratedClasses) {
    BcModule module("M", SourceLang::CSharp);
    module.addClass(makeSimpleClass("<DoWorkAsync>d__5", "Ns")); // state machine
    module.addClass(makeSimpleClass("RealClass", "Ns"));

    CsEmitOptions opts;
    opts.omitCompilerGenerated = true;
    CsFileEmitter emitter(opts);
    std::unordered_map<std::string, CilReconstructResult> results;
    auto result = emitter.emitModule(module, results);

    EXPECT_EQ(1, result.emittedClasses);
    EXPECT_EQ(1, result.skippedClasses);
}

TEST(CsFileEmitter, FileNameFromClassName) {
    BcModule module("M", SourceLang::CSharp);
    BcClass& cls = module.addClass(makeSimpleClass("Calculator", "MyApp.Math"));

    CsFileEmitter emitter;
    std::unordered_map<std::string, CilReconstructResult> results;
    auto result = emitter.emitModule(module, results);

    bool found = false;
    for (const auto& [name, _] : result.files) {
        if (contains(name, "Calculator.cs")) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(CsFileEmitter, GoldenFileSimpleClass) {
    // End-to-end: build a simple class, emit it, verify key syntax elements
    BcModule module("HelloLib", SourceLang::CSharp);
    BcClass cls = makeSimpleClass("Greeter", "HelloLib");

    BcField nameField;
    nameField.name   = "_name";
    nameField.type   = types::ClrString();
    nameField.access = BcAccess::Private;
    cls.fields.push_back(std::move(nameField));

    BcMethod ctor;
    ctor.name         = ".ctor";
    ctor.isConstructor = true;
    ctor.access       = BcAccess::Public;
    ctor.paramNames   = {"name"};
    ctor.descriptor.params.push_back(std::make_shared<BcType>(types::ClrString()));
    ctor.descriptor.returnType = std::make_shared<BcType>(types::Void());
    cls.methods.push_back(std::move(ctor));

    BcMethod greet;
    greet.name   = "Greet";
    greet.access = BcAccess::Public;
    greet.descriptor.returnType = std::make_shared<BcType>(types::ClrString());
    cls.methods.push_back(std::move(greet));

    BcClass& ref = module.addClass(std::move(cls));

    CsEmitOptions opts;
    opts.csharpVersion       = 12;
    opts.fileScopedNamespace = true;
    CsFileEmitter emitter(opts);

    std::unordered_map<std::string, CilReconstructResult> results;
    std::string src = emitter.emitClass(ref, module, results);

    EXPECT_TRUE(contains(src, "public class Greeter"));
    EXPECT_TRUE(contains(src, "_name"));
    EXPECT_TRUE(contains(src, "Greet"));
    EXPECT_TRUE(contains(src, "namespace HelloLib;"));
    EXPECT_TRUE(contains(src, "#nullable enable"));
}
