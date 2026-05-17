/**
 * @file tests/java_emitter/java_emitter_test.cpp
 * @brief Unit tests for the Java source emitter pipeline.
 *
 * Tests verify that emitted Java source:
 *   1. Contains the correct structural elements (package, imports, class, methods).
 *   2. Has correct type rendering (primitives, references, arrays, generics).
 *   3. Handles import deduplication and collision resolution.
 *   4. Emits well-formed method bodies for known BcCFG patterns.
 *   5. Detects and renders string concatenation, lambdas, for-each loops.
 *   6. Emits enums, interfaces, annotations, and records.
 */

#include <memory>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/java_emitter/java_class_emitter.h"
#include "retdec/java_emitter/java_expr_emitter.h"
#include "retdec/java_emitter/java_file_emitter.h"
#include "retdec/java_emitter/java_stmt_emitter.h"
#include "retdec/java_emitter/java_type_printer.h"
#include "retdec/jvm_reconstruct/jvm_reconstruct.h"

using namespace retdec::bc_module;
using namespace retdec::java_emitter;
using namespace retdec::jvm_reconstruct;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static BcType classType(const std::string& name) {
    return types::Class(name);
}

static BcType arrayType(BcType elem, int dims = 1) {
    return types::Array(std::move(elem), dims);
}

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Build a minimal BcClass with the given fqName.
static BcClass makeClass(const std::string& fqName,
                          const std::string& pkg = "",
                          bool isInterface = false) {
    BcClass cls;
    size_t dot = fqName.rfind('.');
    cls.fqName = fqName;
    cls.name = (dot == std::string::npos) ? fqName : fqName.substr(dot + 1);
    cls.packageName = pkg.empty() ? (dot == std::string::npos ? "" : fqName.substr(0, dot))
                                   : pkg;
    cls.isInterface = isInterface;
    return cls;
}

// ─── ImportSet tests ──────────────────────────────────────────────────────────

TEST(ImportSet, JavaLangNotImported) {
    ImportSet imports("com.example", "Foo");
    std::string name = imports.require("java.lang.String");
    EXPECT_EQ("String", name);
    EXPECT_TRUE(imports.importLines().empty());
}

TEST(ImportSet, SamePackageNotImported) {
    ImportSet imports("com.example", "Foo");
    std::string name = imports.require("com.example.Bar");
    EXPECT_EQ("Bar", name);
    EXPECT_TRUE(imports.importLines().empty());
}

TEST(ImportSet, OtherPackageImported) {
    ImportSet imports("com.example", "Foo");
    std::string name = imports.require("java.util.List");
    EXPECT_EQ("List", name);
    auto lines = imports.importLines();
    ASSERT_EQ(1u, lines.size());
    EXPECT_EQ("import java.util.List;", lines[0]);
}

TEST(ImportSet, CollisionUsesFullName) {
    ImportSet imports("com.example", "Foo");
    imports.require("java.util.Date");
    // Second Date from different package causes collision.
    std::string name = imports.require("java.sql.Date");
    EXPECT_EQ("java.sql.Date", name);
}

TEST(ImportSet, MultipleImportsSorted) {
    ImportSet imports("", "Foo");
    imports.require("java.util.Map");
    imports.require("java.util.ArrayList");
    imports.require("java.io.IOException");
    auto lines = imports.importLines();
    // Should be sorted: io, util.ArrayList, util.Map.
    ASSERT_EQ(3u, lines.size());
    EXPECT_LT(lines[0], lines[1]);
    EXPECT_LT(lines[1], lines[2]);
}

TEST(ImportSet, DuplicateRequireReturnsSameName) {
    ImportSet imports("", "Foo");
    std::string a = imports.require("java.util.List");
    std::string b = imports.require("java.util.List");
    EXPECT_EQ(a, b);
    EXPECT_EQ(1u, imports.importLines().size());
}

// ─── JavaTypePrinter tests ────────────────────────────────────────────────────

TEST(JavaTypePrinter, Primitives) {
    ImportSet imports;
    JavaTypePrinter tp(imports);
    EXPECT_EQ("void",    tp.print(types::Void()));
    EXPECT_EQ("boolean", tp.print(types::Bool()));
    EXPECT_EQ("byte",    tp.print(types::Byte()));
    EXPECT_EQ("short",   tp.print(types::Short()));
    EXPECT_EQ("char",    tp.print(types::Char()));
    EXPECT_EQ("int",     tp.print(types::Int()));
    EXPECT_EQ("long",    tp.print(types::Long()));
    EXPECT_EQ("float",   tp.print(types::Float()));
    EXPECT_EQ("double",  tp.print(types::Double()));
}

TEST(JavaTypePrinter, JavaLangClass) {
    ImportSet imports("com.example", "Test");
    JavaTypePrinter tp(imports);
    std::string result = tp.print(classType("java.lang.String"));
    EXPECT_EQ("String", result);
    EXPECT_TRUE(imports.importLines().empty()); // No import needed.
}

TEST(JavaTypePrinter, OtherClass) {
    ImportSet imports("com.example", "Test");
    JavaTypePrinter tp(imports);
    std::string result = tp.print(classType("java.util.ArrayList"));
    EXPECT_EQ("ArrayList", result);
    auto lines = imports.importLines();
    ASSERT_EQ(1u, lines.size());
    EXPECT_EQ("import java.util.ArrayList;", lines[0]);
}

TEST(JavaTypePrinter, SlashSeparatorNormalized) {
    ImportSet imports("", "Test");
    JavaTypePrinter tp(imports);
    std::string result = tp.print(classType("java/util/HashMap"));
    EXPECT_EQ("HashMap", result);
    EXPECT_EQ(1u, imports.importLines().size());
}

TEST(JavaTypePrinter, IntArray) {
    ImportSet imports;
    JavaTypePrinter tp(imports);
    EXPECT_EQ("int[]", tp.print(arrayType(types::Int())));
}

TEST(JavaTypePrinter, StringArray) {
    ImportSet imports("", "Test");
    JavaTypePrinter tp(imports);
    EXPECT_EQ("String[]", tp.print(arrayType(classType("java.lang.String"))));
}

TEST(JavaTypePrinter, TwoDimArray) {
    ImportSet imports;
    JavaTypePrinter tp(imports);
    EXPECT_EQ("int[][]", tp.print(arrayType(types::Int(), 2)));
}

TEST(JavaTypePrinter, GenericType) {
    ImportSet imports("", "Test");
    JavaTypePrinter tp(imports);
    BcType listOfString = types::Generic(classType("java.util.List"),
                                          {classType("java.lang.String")});
    std::string result = tp.print(listOfString);
    EXPECT_EQ("List<String>", result);
    EXPECT_EQ(1u, imports.importLines().size()); // java.util.List
}

TEST(JavaTypePrinter, TypeVariable) {
    ImportSet imports;
    JavaTypePrinter tp(imports);
    EXPECT_EQ("T", tp.print(types::TypeVar("T")));
    EXPECT_EQ("E", tp.print(types::TypeVar("E")));
}

TEST(JavaTypePrinter, WildcardTypes) {
    ImportSet imports("", "Test");
    JavaTypePrinter tp(imports);
    EXPECT_EQ("?", tp.print(types::Wildcard()));
    EXPECT_EQ("? extends Comparable",
              tp.print(types::BoundedAbove(classType("java.lang.Comparable"))));
    EXPECT_EQ("? super Object",
              tp.print(types::BoundedBelow(classType("java.lang.Object"))));
}

TEST(JavaTypePrinter, PrintMethod) {
    ImportSet imports("", "Test");
    JavaTypePrinter tp(imports);
    BcFuncType func;
    func.returnType = std::make_shared<BcType>(types::Int());
    func.params = {
        std::make_shared<BcType>(types::Long()),
        std::make_shared<BcType>(classType("java.lang.String")),
    };
    std::vector<std::string> params;
    std::string ret = tp.printMethod(func, params);
    EXPECT_EQ("int", ret);
    ASSERT_EQ(2u, params.size());
    EXPECT_EQ("long", params[0]);
    EXPECT_EQ("String", params[1]);
}

TEST(JavaTypePrinter, VoidMethod) {
    ImportSet imports;
    JavaTypePrinter tp(imports);
    BcFuncType func;
    // returnType = nullptr means void
    std::vector<std::string> params;
    std::string ret = tp.printMethod(func, params);
    EXPECT_EQ("void", ret);
}

// ─── JavaClassEmitter tests ───────────────────────────────────────────────────

TEST(JavaClassEmitter, EmitsPublicClass) {
    ImportSet imports;
    JavaTypePrinter tp(imports);
    JavaClassEmitter emitter(imports, tp, nullptr);

    BcClass cls = makeClass("com.example.Foo", "com.example");
    cls.access = BcAccess::Public;

    CodeWriter writer;
    emitter.emitClass(cls, writer);
    std::string out = writer.str();

    EXPECT_TRUE(contains(out, "public class Foo {"));
    EXPECT_TRUE(contains(out, "}"));
}

TEST(JavaClassEmitter, EmitsInterface) {
    ImportSet imports;
    JavaTypePrinter tp(imports);
    JavaClassEmitter emitter(imports, tp, nullptr);

    BcClass cls = makeClass("com.example.IFoo", "com.example", true);
    cls.access = BcAccess::Public;

    CodeWriter writer;
    emitter.emitClass(cls, writer);
    EXPECT_TRUE(contains(writer.str(), "public interface IFoo {"));
}

TEST(JavaClassEmitter, EmitsEnum) {
    ImportSet imports;
    JavaTypePrinter tp(imports);
    JavaClassEmitter emitter(imports, tp, nullptr);

    BcClass cls = makeClass("com.example.Color", "com.example");
    cls.isEnum = true;
    cls.access = BcAccess::Public;

    BcField red;
    red.name   = "RED";
    red.type   = classType("com.example.Color");
    red.access = BcAccess::Public | BcAccess::Static | BcAccess::Final;
    cls.fields.push_back(red);

    BcField green = red;
    green.name = "GREEN";
    cls.fields.push_back(green);

    CodeWriter writer;
    emitter.emitClass(cls, writer);
    std::string out = writer.str();
    EXPECT_TRUE(contains(out, "enum Color {"));
}

TEST(JavaClassEmitter, EmitsExtendsAndImplements) {
    ImportSet imports("com.example", "Foo");
    JavaTypePrinter tp(imports);
    JavaClassEmitter emitter(imports, tp, nullptr);

    BcClass cls = makeClass("com.example.Foo", "com.example");
    cls.access = BcAccess::Public;
    cls.superClass = classType("com.example.Base");
    cls.interfaces = {classType("java.io.Serializable")};

    CodeWriter writer;
    emitter.emitClass(cls, writer);
    std::string out = writer.str();
    EXPECT_TRUE(contains(out, "extends Base"));
    EXPECT_TRUE(contains(out, "implements Serializable"));
}

TEST(JavaClassEmitter, EmitsField) {
    ImportSet imports("com.example", "Foo");
    JavaTypePrinter tp(imports);
    JavaClassEmitter emitter(imports, tp, nullptr);

    BcClass cls = makeClass("com.example.Foo", "com.example");
    cls.access = BcAccess::Public;

    BcField f;
    f.name   = "count";
    f.type   = types::Int();
    f.access = BcAccess::Private;
    cls.fields.push_back(f);

    CodeWriter writer;
    emitter.emitClass(cls, writer);
    EXPECT_TRUE(contains(writer.str(), "private int count;"));
}

TEST(JavaClassEmitter, EmitsStaticFinalFieldWithInit) {
    ImportSet imports("", "Foo");
    JavaTypePrinter tp(imports);
    JavaClassEmitter emitter(imports, tp, nullptr);

    BcClass cls = makeClass("Foo");
    cls.access = BcAccess::Public;

    BcField f;
    f.name   = "MAX";
    f.type   = types::Int();
    f.access = BcAccess::Public | BcAccess::Static | BcAccess::Final;
    f.constantIntValue = 100;
    cls.fields.push_back(f);

    CodeWriter writer;
    emitter.emitClass(cls, writer);
    EXPECT_TRUE(contains(writer.str(), "public static final int MAX = 100;"));
}

TEST(JavaClassEmitter, EmitsAbstractMethod) {
    ImportSet imports("", "Foo");
    JavaTypePrinter tp(imports);
    JavaClassEmitter emitter(imports, tp, nullptr);

    BcClass cls = makeClass("Foo");
    cls.access = BcAccess::Public | BcAccess::Abstract;
    cls.isAbstract = true;

    BcMethod m;
    m.name = "compute";
    m.isAbstract = true;
    m.access = BcAccess::Public | BcAccess::Abstract;
    m.descriptor.returnType = std::make_shared<BcType>(types::Int());
    cls.methods.push_back(m);

    CodeWriter writer;
    emitter.emitClass(cls, writer);
    std::string out = writer.str();
    EXPECT_TRUE(contains(out, "public abstract int compute();"));
}

TEST(JavaClassEmitter, EmitsAnnotation) {
    ImportSet imports("", "Foo");
    JavaTypePrinter tp(imports);
    JavaClassEmitter emitter(imports, tp, nullptr);

    BcClass cls = makeClass("Foo");
    cls.access = BcAccess::Public;

    BcAnnotation ann;
    ann.typeName = "java.lang.Deprecated";
    ann.isVisible = true;
    cls.annotations.push_back(ann);

    CodeWriter writer;
    emitter.emitClass(cls, writer);
    EXPECT_TRUE(contains(writer.str(), "@Deprecated"));
}

// ─── JavaFileEmitter tests ────────────────────────────────────────────────────

TEST(JavaFileEmitter, EmitsPackageDeclaration) {
    FileEmitOptions opts;
    opts.runReconstruction = false;
    JavaFileEmitter emitter(opts);

    BcClass cls = makeClass("com.example.Foo", "com.example");
    cls.access = BcAccess::Public;

    JavaFileResult result = emitter.emitClass(cls);
    EXPECT_EQ("Foo", result.className);
    EXPECT_EQ("com.example", result.packageName);
    EXPECT_EQ("com/example/Foo.java", result.relativePath);
    EXPECT_TRUE(contains(result.source, "package com.example;"));
}

TEST(JavaFileEmitter, EmitsHeaderComment) {
    FileEmitOptions opts;
    opts.runReconstruction = false;
    opts.emitHeader = true;
    opts.version = "RetDec v1.0";
    JavaFileEmitter emitter(opts);

    BcClass cls = makeClass("Foo");
    cls.access = BcAccess::Public;

    JavaFileResult result = emitter.emitClass(cls);
    EXPECT_TRUE(contains(result.source, "// Decompiled by RetDec v1.0"));
}

TEST(JavaFileEmitter, EmitsImports) {
    FileEmitOptions opts;
    opts.runReconstruction = false;
    JavaFileEmitter emitter(opts);

    BcClass cls = makeClass("com.example.Foo", "com.example");
    cls.access = BcAccess::Public;

    BcField f;
    f.name = "list";
    f.type = classType("java.util.ArrayList");
    f.access = BcAccess::Private;
    cls.fields.push_back(f);

    JavaFileResult result = emitter.emitClass(cls);
    EXPECT_TRUE(contains(result.source, "import java.util.ArrayList;"));
}

TEST(JavaFileEmitter, DefaultPackageNoPackageDecl) {
    FileEmitOptions opts;
    opts.runReconstruction = false;
    JavaFileEmitter emitter(opts);

    BcClass cls = makeClass("Foo");
    cls.access = BcAccess::Public;

    JavaFileResult result = emitter.emitClass(cls);
    EXPECT_FALSE(contains(result.source, "package ;"));
    EXPECT_FALSE(contains(result.source, "package\n"));
    EXPECT_EQ("Foo.java", result.relativePath);
}

TEST(JavaFileEmitter, EmitsModule) {
    FileEmitOptions opts;
    opts.runReconstruction = false;
    JavaFileEmitter emitter(opts);

    BcModule module("Test", SourceLang::Java);
    BcClass cls1 = makeClass("com.example.A", "com.example");
    cls1.access = BcAccess::Public;
    BcClass cls2 = makeClass("com.example.B", "com.example");
    cls2.access = BcAccess::Public;
    module.addClass(cls1);
    module.addClass(cls2);

    JavaModuleResult result = emitter.emitModule(module);
    EXPECT_EQ(2u, result.files.size());
}

// ─── CodeWriter tests ─────────────────────────────────────────────────────────

TEST(CodeWriter, IndentsAndDedents) {
    CodeWriter w(4);
    w.writeLine("class Foo {");
    w.indent();
    w.writeLine("void bar() {}");
    w.dedent();
    w.writeLine("}");
    std::string out = w.str();
    EXPECT_TRUE(contains(out, "class Foo {"));
    EXPECT_TRUE(contains(out, "    void bar() {}"));
    EXPECT_TRUE(contains(out, "}"));
}

TEST(CodeWriter, BlankLine) {
    CodeWriter w;
    w.writeLine("a");
    w.writeLine();
    w.writeLine("b");
    std::string out = w.str();
    EXPECT_TRUE(contains(out, "a\n\nb"));
}

// ─── ExprContext and ExprEmitter tests ────────────────────────────────────────

static ReconstructResult makeEmptyRecon() {
    return ReconstructResult{};
}

TEST(JavaExprEmitter, PushIntLiteral) {
    BcMethod method;
    method.access = BcAccess::Static;
    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports;
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    BcInstruction insn;
    insn.id = 0; insn.offset = 0;
    insn.opcode = BcOpcode::PushInt;
    insn.operands.push_back(BcIntOperand{42});

    std::vector<ExprNode> stack;
    emitter.emitInsn(insn, stack);
    ASSERT_EQ(1u, stack.size());
    EXPECT_EQ("42", stack[0].text);
}

TEST(JavaExprEmitter, PushStringLiteral) {
    BcMethod method;
    method.access = BcAccess::Static;
    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports;
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    BcInstruction insn;
    insn.id = 0; insn.offset = 0;
    insn.opcode = BcOpcode::PushString;
    insn.operands.push_back(BcStringOperand{"hello"});

    std::vector<ExprNode> stack;
    emitter.emitInsn(insn, stack);
    ASSERT_EQ(1u, stack.size());
    EXPECT_EQ("\"hello\"", stack[0].text);
}

TEST(JavaExprEmitter, AddTwoInts) {
    BcMethod method;
    method.access = BcAccess::Static;
    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports;
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    std::vector<ExprNode> stack;
    // Push 3.
    BcInstruction i1; i1.id = 0; i1.opcode = BcOpcode::PushInt;
    i1.operands.push_back(BcIntOperand{3});
    emitter.emitInsn(i1, stack);

    // Push 4.
    BcInstruction i2; i2.id = 1; i2.opcode = BcOpcode::PushInt;
    i2.operands.push_back(BcIntOperand{4});
    emitter.emitInsn(i2, stack);

    // Add.
    BcInstruction i3; i3.id = 2; i3.opcode = BcOpcode::Add;
    emitter.emitInsn(i3, stack);

    ASSERT_EQ(1u, stack.size());
    EXPECT_EQ("3 + 4", stack[0].text);
}

TEST(JavaExprEmitter, LoadLocalVariable) {
    BcMethod method;
    method.access = BcAccess::Static;
    BcLocalVar lv; lv.index = 0; lv.name = "counter"; lv.type = types::Int();
    method.locals.push_back(lv);

    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports;
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    BcInstruction insn;
    insn.id = 0; insn.opcode = BcOpcode::LoadLocal;
    insn.operands.push_back(BcLocalOperand{0});

    std::vector<ExprNode> stack;
    emitter.emitInsn(insn, stack);
    ASSERT_EQ(1u, stack.size());
    EXPECT_EQ("counter", stack[0].text);
}

TEST(JavaExprEmitter, StoreLocalReturnsAssignment) {
    BcMethod method;
    method.access = BcAccess::Static;
    BcLocalVar lv; lv.index = 1; lv.name = "x"; lv.type = types::Int();
    method.locals.push_back(lv);

    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports;
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    std::vector<ExprNode> stack;
    // Push a value.
    BcInstruction push; push.id = 0; push.opcode = BcOpcode::PushInt;
    push.operands.push_back(BcIntOperand{99});
    emitter.emitInsn(push, stack);

    // Store.
    BcInstruction store; store.id = 1; store.opcode = BcOpcode::StoreLocal;
    store.operands.push_back(BcLocalOperand{1});
    std::string result = emitter.emitInsn(store, stack);

    EXPECT_TRUE(result.find("x") != std::string::npos);
    EXPECT_TRUE(result.find("99") != std::string::npos);
    EXPECT_TRUE(stack.empty()); // Value was consumed.
}

TEST(JavaExprEmitter, CastInt) {
    BcMethod method;
    method.access = BcAccess::Static;
    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports;
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    std::vector<ExprNode> stack;
    BcInstruction push; push.id = 0; push.opcode = BcOpcode::PushLong;
    push.operands.push_back(BcIntOperand{1000L});
    emitter.emitInsn(push, stack);

    BcInstruction cast; cast.id = 1; cast.opcode = BcOpcode::L2I;
    emitter.emitInsn(cast, stack);

    ASSERT_EQ(1u, stack.size());
    EXPECT_EQ("(int)1000L", stack[0].text);
}

TEST(JavaExprEmitter, ArrayLength) {
    BcMethod method;
    method.access = BcAccess::Static;
    BcLocalVar lv; lv.index = 0; lv.name = "arr"; lv.type = arrayType(types::Int());
    method.locals.push_back(lv);

    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports;
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    std::vector<ExprNode> stack;
    BcInstruction load; load.id = 0; load.opcode = BcOpcode::LoadLocal;
    load.operands.push_back(BcLocalOperand{0});
    emitter.emitInsn(load, stack);

    BcInstruction len; len.id = 1; len.opcode = BcOpcode::ArrayLength;
    emitter.emitInsn(len, stack);

    ASSERT_EQ(1u, stack.size());
    EXPECT_EQ("arr.length", stack[0].text);
}

TEST(JavaExprEmitter, Instanceof) {
    BcMethod method;
    method.access = BcAccess::Static;
    BcLocalVar lv; lv.index = 0; lv.name = "obj";
    lv.type = classType("java.lang.Object");
    method.locals.push_back(lv);

    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports("", "Test");
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    std::vector<ExprNode> stack;
    BcInstruction load; load.id = 0; load.opcode = BcOpcode::LoadLocal;
    load.operands.push_back(BcLocalOperand{0});
    emitter.emitInsn(load, stack);

    BcInstruction iof; iof.id = 1; iof.opcode = BcOpcode::Instanceof;
    iof.operands.push_back(BcTypeOperand{classType("java.lang.String")});
    emitter.emitInsn(iof, stack);

    ASSERT_EQ(1u, stack.size());
    EXPECT_EQ("obj instanceof String", stack[0].text);
}

TEST(JavaExprEmitter, ReturnValueStatement) {
    BcMethod method;
    method.access = BcAccess::Static;
    ReconstructResult recon = makeEmptyRecon();
    ImportSet imports;
    JavaTypePrinter tp(imports);
    ExprContext ctx(method, recon, tp);
    JavaExprEmitter emitter(ctx);

    std::vector<ExprNode> stack;
    BcInstruction push; push.id = 0; push.opcode = BcOpcode::PushInt;
    push.operands.push_back(BcIntOperand{0});
    emitter.emitInsn(push, stack);

    BcInstruction ret; ret.id = 1; ret.opcode = BcOpcode::ReturnValue;
    std::string result = emitter.emitInsn(ret, stack);

    EXPECT_EQ("return 0", result);
    EXPECT_TRUE(stack.empty());
}

// ─── JavaStmtEmitter integration test ────────────────────────────────────────

TEST(JavaStmtEmitter, EmitsSimpleMethodBody) {
    // Build: int x = 1 + 2; return x;
    BcMethod method;
    method.name = "test";
    method.access = BcAccess::Static;
    method.descriptor.returnType = std::make_shared<BcType>(types::Int());

    BcLocalVar lv; lv.index = 0; lv.name = "x"; lv.type = types::Int();
    method.locals.push_back(lv);

    auto& blk = method.cfg.addBlock();
    {
        BcInstruction i; i.id = 0; i.opcode = BcOpcode::PushInt;
        i.operands.push_back(BcIntOperand{1}); blk.instrs.push_back(i);
    }
    {
        BcInstruction i; i.id = 1; i.opcode = BcOpcode::PushInt;
        i.operands.push_back(BcIntOperand{2}); blk.instrs.push_back(i);
    }
    {
        BcInstruction i; i.id = 2; i.opcode = BcOpcode::Add; blk.instrs.push_back(i);
    }
    {
        BcInstruction i; i.id = 3; i.opcode = BcOpcode::StoreLocal;
        i.operands.push_back(BcLocalOperand{0}); blk.instrs.push_back(i);
    }
    {
        BcInstruction i; i.id = 4; i.opcode = BcOpcode::LoadLocal;
        i.operands.push_back(BcLocalOperand{0}); blk.instrs.push_back(i);
    }
    {
        BcInstruction i; i.id = 5; i.opcode = BcOpcode::ReturnValue;
        blk.instrs.push_back(i);
    }

    ReconstructResult recon;
    ImportSet imports;
    JavaTypePrinter tp(imports);
    JavaStmtEmitter stmtEmit(method, recon, tp);
    std::string body = stmtEmit.emitBody();

    EXPECT_TRUE(contains(body, "{"));
    EXPECT_TRUE(contains(body, "}"));
    EXPECT_TRUE(contains(body, "return"));
}

TEST(JavaStmtEmitter, LocalVarDeclarations) {
    BcMethod method;
    method.name = "test";
    method.access = BcAccess::Static;

    // Add non-param locals.
    BcLocalVar lv1; lv1.index = 0; lv1.name = "count"; lv1.type = types::Int();
    lv1.isParam = false;
    BcLocalVar lv2; lv2.index = 1; lv2.name = "flag"; lv2.type = types::Bool();
    lv2.isParam = false;
    method.locals = {lv1, lv2};

    auto& blk = method.cfg.addBlock();
    {
        BcInstruction i; i.id = 0; i.opcode = BcOpcode::Return;
        blk.instrs.push_back(i);
    }

    ReconstructResult recon;
    ImportSet imports;
    JavaTypePrinter tp(imports);
    JavaStmtEmitter stmtEmit(method, recon, tp);
    std::string body = stmtEmit.emitBody();

    EXPECT_TRUE(contains(body, "int count;"));
    EXPECT_TRUE(contains(body, "boolean flag;"));
}
