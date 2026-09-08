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

static BcType classType(const std::string& name)
{
	return types::Class(name);
}

static BcType arrayType(BcType elem, int dims = 1)
{
	return types::Array(std::move(elem), dims);
}

static bool contains(const std::string& haystack, const std::string& needle)
{
	return haystack.find(needle) != std::string::npos;
}

static unsigned occurrences(const std::string& haystack, const std::string& needle)
{
	unsigned n = 0;
	for (size_t p = haystack.find(needle); p != std::string::npos; p = haystack.find(needle, p + needle.size()))
		++n;
	return n;
}

// Build a minimal BcClass with the given fqName.
static BcClass makeClass(const std::string& fqName, const std::string& pkg = "", bool isInterface = false)
{
	BcClass cls;
	size_t dot = fqName.rfind('.');
	cls.fqName = fqName;
	cls.name = (dot == std::string::npos) ? fqName : fqName.substr(dot + 1);
	cls.packageName = pkg.empty() ? (dot == std::string::npos ? "" : fqName.substr(0, dot)) : pkg;
	cls.isInterface = isInterface;
	return cls;
}

// ─── ImportSet tests ──────────────────────────────────────────────────────────

TEST(ImportSet, JavaLangNotImported)
{
	ImportSet imports("com.example", "Foo");
	std::string name = imports.require("java.lang.String");
	EXPECT_EQ("String", name);
	EXPECT_TRUE(imports.importLines().empty());
}

TEST(ImportSet, SamePackageNotImported)
{
	ImportSet imports("com.example", "Foo");
	std::string name = imports.require("com.example.Bar");
	EXPECT_EQ("Bar", name);
	EXPECT_TRUE(imports.importLines().empty());
}

TEST(ImportSet, OtherPackageImported)
{
	ImportSet imports("com.example", "Foo");
	std::string name = imports.require("java.util.List");
	EXPECT_EQ("List", name);
	auto lines = imports.importLines();
	ASSERT_EQ(1u, lines.size());
	EXPECT_EQ("import java.util.List;", lines[0]);
}

TEST(ImportSet, CollisionUsesFullName)
{
	ImportSet imports("com.example", "Foo");
	imports.require("java.util.Date");
	// Second Date from different package causes collision.
	std::string name = imports.require("java.sql.Date");
	EXPECT_EQ("java.sql.Date", name);
}

TEST(ImportSet, MultipleImportsSorted)
{
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

TEST(ImportSet, DuplicateRequireReturnsSameName)
{
	ImportSet imports("", "Foo");
	std::string a = imports.require("java.util.List");
	std::string b = imports.require("java.util.List");
	EXPECT_EQ(a, b);
	EXPECT_EQ(1u, imports.importLines().size());
}

// ─── JavaTypePrinter tests ────────────────────────────────────────────────────

TEST(JavaTypePrinter, Primitives)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	EXPECT_EQ("void", tp.print(types::Void()));
	EXPECT_EQ("boolean", tp.print(types::Bool()));
	EXPECT_EQ("byte", tp.print(types::Byte()));
	EXPECT_EQ("short", tp.print(types::Short()));
	EXPECT_EQ("char", tp.print(types::Char()));
	EXPECT_EQ("int", tp.print(types::Int()));
	EXPECT_EQ("long", tp.print(types::Long()));
	EXPECT_EQ("float", tp.print(types::Float()));
	EXPECT_EQ("double", tp.print(types::Double()));
}

TEST(JavaTypePrinter, JavaLangClass)
{
	ImportSet imports("com.example", "Test");
	JavaTypePrinter tp(imports);
	std::string result = tp.print(classType("java.lang.String"));
	EXPECT_EQ("String", result);
	EXPECT_TRUE(imports.importLines().empty()); // No import needed.
}

TEST(JavaTypePrinter, OtherClass)
{
	ImportSet imports("com.example", "Test");
	JavaTypePrinter tp(imports);
	std::string result = tp.print(classType("java.util.ArrayList"));
	EXPECT_EQ("ArrayList", result);
	auto lines = imports.importLines();
	ASSERT_EQ(1u, lines.size());
	EXPECT_EQ("import java.util.ArrayList;", lines[0]);
}

TEST(JavaTypePrinter, SlashSeparatorNormalized)
{
	ImportSet imports("", "Test");
	JavaTypePrinter tp(imports);
	std::string result = tp.print(classType("java/util/HashMap"));
	EXPECT_EQ("HashMap", result);
	EXPECT_EQ(1u, imports.importLines().size());
}

TEST(JavaTypePrinter, IntArray)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	EXPECT_EQ("int[]", tp.print(arrayType(types::Int())));
}

TEST(JavaTypePrinter, StringArray)
{
	ImportSet imports("", "Test");
	JavaTypePrinter tp(imports);
	EXPECT_EQ("String[]", tp.print(arrayType(classType("java.lang.String"))));
}

TEST(JavaTypePrinter, TwoDimArray)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	EXPECT_EQ("int[][]", tp.print(arrayType(types::Int(), 2)));
}

TEST(JavaTypePrinter, GenericType)
{
	ImportSet imports("", "Test");
	JavaTypePrinter tp(imports);
	BcType listOfString = types::Generic(classType("java.util.List"), {classType("java.lang.String")});
	std::string result = tp.print(listOfString);
	EXPECT_EQ("List<String>", result);
	EXPECT_EQ(1u, imports.importLines().size()); // java.util.List
}

TEST(JavaTypePrinter, TypeVariable)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	EXPECT_EQ("T", tp.print(types::TypeVar("T")));
	EXPECT_EQ("E", tp.print(types::TypeVar("E")));
}

TEST(JavaTypePrinter, WildcardTypes)
{
	ImportSet imports("", "Test");
	JavaTypePrinter tp(imports);
	EXPECT_EQ("?", tp.print(types::Wildcard()));
	EXPECT_EQ("? extends Comparable", tp.print(types::BoundedAbove(classType("java.lang.Comparable"))));
	EXPECT_EQ("? super Object", tp.print(types::BoundedBelow(classType("java.lang.Object"))));
}

TEST(JavaTypePrinter, PrintMethod)
{
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

TEST(JavaTypePrinter, VoidMethod)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	BcFuncType func;
	// returnType = nullptr means void
	std::vector<std::string> params;
	std::string ret = tp.printMethod(func, params);
	EXPECT_EQ("void", ret);
}

// ─── JavaClassEmitter tests ───────────────────────────────────────────────────

TEST(JavaClassEmitter, EmitsPublicClass)
{
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

TEST(JavaClassEmitter, EmitsInterface)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("com.example.IFoo", "com.example", true);
	cls.access = BcAccess::Public;

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "public interface IFoo {"));
}

TEST(JavaClassEmitter, EmitsEnum)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("com.example.Color", "com.example");
	cls.isEnum = true;
	cls.access = BcAccess::Public;

	BcField red;
	red.name = "RED";
	red.type = classType("com.example.Color");
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

TEST(JavaClassEmitter, EmitsExtendsAndImplements)
{
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

TEST(JavaClassEmitter, EmitsField)
{
	ImportSet imports("com.example", "Foo");
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("com.example.Foo", "com.example");
	cls.access = BcAccess::Public;

	BcField f;
	f.name = "count";
	f.type = types::Int();
	f.access = BcAccess::Private;
	cls.fields.push_back(f);

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "private int count;"));
}

TEST(JavaClassEmitter, EmitsStaticFinalFieldWithInit)
{
	ImportSet imports("", "Foo");
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("Foo");
	cls.access = BcAccess::Public;

	BcField f;
	f.name = "MAX";
	f.type = types::Int();
	f.access = BcAccess::Public | BcAccess::Static | BcAccess::Final;
	f.constantIntValue = 100;
	cls.fields.push_back(f);

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "public static final int MAX = 100;"));
}

TEST(JavaClassEmitter, EmitsAbstractMethod)
{
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

TEST(JavaClassEmitter, EmitsAnnotation)
{
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

TEST(JavaClassEmitter, EmitsRecordComponents)
{
	ImportSet imports("", "Point");
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("Point");
	cls.isRecord = true;
	cls.access = BcAccess::Public;

	BcField x;
	x.name = "x";
	x.type = types::Int();
	cls.fields.push_back(x);
	BcField y = x;
	y.name = "y";
	cls.fields.push_back(y);

	BcMethod ctor;
	ctor.name = "<init>";
	ctor.isConstructor = true;
	ctor.paramNames = {"x", "y"};
	ctor.descriptor.params = {std::make_shared<BcType>(types::Int()), std::make_shared<BcType>(types::Int())};
	cls.methods.push_back(ctor);

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "record Point(int x, int y) {"));
}

TEST(JavaClassEmitter, EmitsParamAnnotations)
{
	ImportSet imports("", "Foo");
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("Foo");
	cls.access = BcAccess::Public;

	BcMethod m;
	m.name = "foo";
	m.access = BcAccess::Public;
	m.isAbstract = true;
	m.descriptor.returnType = std::make_shared<BcType>(types::Void());
	m.descriptor.params = {std::make_shared<BcType>(types::Int())};
	m.paramNames = {"n"};
	BcAnnotation ann;
	ann.typeName = "java.lang.Deprecated";
	m.paramAnnotations.push_back({ann});
	cls.methods.push_back(m);

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "@Deprecated int n"));
}

TEST(JavaClassEmitter, EmitsVarArgs)
{
	ImportSet imports("", "Foo");
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("Foo");
	cls.access = BcAccess::Public;

	BcMethod m;
	m.name = "log";
	m.access = BcAccess::Public | BcAccess::VarArgs;
	m.isAbstract = true;
	m.descriptor.returnType = std::make_shared<BcType>(types::Void());
	m.descriptor.params = {std::make_shared<BcType>(arrayType(types::Int()))};
	m.paramNames = {"xs"};
	cls.methods.push_back(m);

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "int... xs"));
}

TEST(JavaClassEmitter, EmitsSourceFileComment)
{
	ImportSet imports("", "Foo");
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("Foo");
	cls.access = BcAccess::Public;
	cls.sourceFile = "Foo.java";

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "// SourceFile: Foo.java"));
}

TEST(JavaClassEmitter, EmitsModuleKeyword)
{
	ImportSet imports("", "mod");
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("mod");
	cls.access = BcAccess::Public;
	cls.isModule = true;

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "module mod"));
}

TEST(JavaClassEmitter, EmitsSealedWithPermits)
{
	ImportSet imports("", "Shape");
	JavaTypePrinter tp(imports);
	JavaClassEmitter emitter(imports, tp, nullptr);

	BcClass cls = makeClass("Shape");
	cls.access = BcAccess::Public | BcAccess::Sealed;
	cls.permittedSubclasses.push_back("Circle");

	CodeWriter writer;
	emitter.emitClass(cls, writer);
	EXPECT_TRUE(contains(writer.str(), "sealed class Shape"));
	EXPECT_TRUE(contains(writer.str(), "permits Circle"));
}

// ─── JavaFileEmitter tests ────────────────────────────────────────────────────

TEST(JavaFileEmitter, EmitsPackageDeclaration)
{
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

TEST(JavaFileEmitter, EmitsHeaderComment)
{
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

TEST(JavaFileEmitter, EmitsImports)
{
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

TEST(JavaFileEmitter, DefaultPackageNoPackageDecl)
{
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

TEST(JavaFileEmitter, EmitsModule)
{
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

TEST(CodeWriter, IndentsAndDedents)
{
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

TEST(CodeWriter, BlankLine)
{
	CodeWriter w;
	w.writeLine("a");
	w.writeLine();
	w.writeLine("b");
	std::string out = w.str();
	EXPECT_TRUE(contains(out, "a\n\nb"));
}

// ─── ExprContext and ExprEmitter tests ────────────────────────────────────────

static ReconstructResult makeEmptyRecon()
{
	return ReconstructResult{};
}

TEST(JavaExprEmitter, PushIntLiteral)
{
	BcMethod method;
	method.access = BcAccess::Static;
	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	BcInstruction insn;
	insn.id = 0;
	insn.offset = 0;
	insn.opcode = BcOpcode::PushInt;
	insn.operands.push_back(BcIntOperand{42});

	std::vector<ExprNode> stack;
	emitter.emitInsn(insn, stack);
	ASSERT_EQ(1u, stack.size());
	EXPECT_EQ("42", stack[0].text);
}

TEST(JavaExprEmitter, PushStringLiteral)
{
	BcMethod method;
	method.access = BcAccess::Static;
	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	BcInstruction insn;
	insn.id = 0;
	insn.offset = 0;
	insn.opcode = BcOpcode::PushString;
	insn.operands.push_back(BcStringOperand{"hello"});

	std::vector<ExprNode> stack;
	emitter.emitInsn(insn, stack);
	ASSERT_EQ(1u, stack.size());
	EXPECT_EQ("\"hello\"", stack[0].text);
}

TEST(JavaExprEmitter, AddTwoInts)
{
	BcMethod method;
	method.access = BcAccess::Static;
	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	std::vector<ExprNode> stack;
	// Push 3.
	BcInstruction i1;
	i1.id = 0;
	i1.opcode = BcOpcode::PushInt;
	i1.operands.push_back(BcIntOperand{3});
	emitter.emitInsn(i1, stack);

	// Push 4.
	BcInstruction i2;
	i2.id = 1;
	i2.opcode = BcOpcode::PushInt;
	i2.operands.push_back(BcIntOperand{4});
	emitter.emitInsn(i2, stack);

	// Add.
	BcInstruction i3;
	i3.id = 2;
	i3.opcode = BcOpcode::Add;
	emitter.emitInsn(i3, stack);

	ASSERT_EQ(1u, stack.size());
	EXPECT_EQ("3 + 4", stack[0].text);
}

TEST(JavaExprEmitter, LoadLocalVariable)
{
	BcMethod method;
	method.access = BcAccess::Static;
	BcLocalVar lv;
	lv.index = 0;
	lv.name = "counter";
	lv.type = types::Int();
	method.locals.push_back(lv);

	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	BcInstruction insn;
	insn.id = 0;
	insn.opcode = BcOpcode::LoadLocal;
	insn.operands.push_back(BcLocalOperand{0});

	std::vector<ExprNode> stack;
	emitter.emitInsn(insn, stack);
	ASSERT_EQ(1u, stack.size());
	EXPECT_EQ("counter", stack[0].text);
}

TEST(JavaExprEmitter, StoreLocalReturnsAssignment)
{
	BcMethod method;
	method.access = BcAccess::Static;
	BcLocalVar lv;
	lv.index = 1;
	lv.name = "x";
	lv.type = types::Int();
	method.locals.push_back(lv);

	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	std::vector<ExprNode> stack;
	// Push a value.
	BcInstruction push;
	push.id = 0;
	push.opcode = BcOpcode::PushInt;
	push.operands.push_back(BcIntOperand{99});
	emitter.emitInsn(push, stack);

	// Store.
	BcInstruction store;
	store.id = 1;
	store.opcode = BcOpcode::StoreLocal;
	store.operands.push_back(BcLocalOperand{1});
	std::string result = emitter.emitInsn(store, stack);

	EXPECT_TRUE(result.find("x") != std::string::npos);
	EXPECT_TRUE(result.find("99") != std::string::npos);
	EXPECT_TRUE(stack.empty()); // Value was consumed.
}

TEST(JavaExprEmitter, CastInt)
{
	BcMethod method;
	method.access = BcAccess::Static;
	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	std::vector<ExprNode> stack;
	BcInstruction push;
	push.id = 0;
	push.opcode = BcOpcode::PushLong;
	push.operands.push_back(BcIntOperand{1000L});
	emitter.emitInsn(push, stack);

	BcInstruction cast;
	cast.id = 1;
	cast.opcode = BcOpcode::L2I;
	emitter.emitInsn(cast, stack);

	ASSERT_EQ(1u, stack.size());
	EXPECT_EQ("(int)1000L", stack[0].text);
}

TEST(JavaExprEmitter, ArrayLength)
{
	BcMethod method;
	method.access = BcAccess::Static;
	BcLocalVar lv;
	lv.index = 0;
	lv.name = "arr";
	lv.type = arrayType(types::Int());
	method.locals.push_back(lv);

	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	std::vector<ExprNode> stack;
	BcInstruction load;
	load.id = 0;
	load.opcode = BcOpcode::LoadLocal;
	load.operands.push_back(BcLocalOperand{0});
	emitter.emitInsn(load, stack);

	BcInstruction len;
	len.id = 1;
	len.opcode = BcOpcode::ArrayLength;
	emitter.emitInsn(len, stack);

	ASSERT_EQ(1u, stack.size());
	EXPECT_EQ("arr.length", stack[0].text);
}

TEST(JavaExprEmitter, Instanceof)
{
	BcMethod method;
	method.access = BcAccess::Static;
	BcLocalVar lv;
	lv.index = 0;
	lv.name = "obj";
	lv.type = classType("java.lang.Object");
	method.locals.push_back(lv);

	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports("", "Test");
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	std::vector<ExprNode> stack;
	BcInstruction load;
	load.id = 0;
	load.opcode = BcOpcode::LoadLocal;
	load.operands.push_back(BcLocalOperand{0});
	emitter.emitInsn(load, stack);

	BcInstruction iof;
	iof.id = 1;
	iof.opcode = BcOpcode::Instanceof;
	iof.operands.push_back(BcTypeOperand{classType("java.lang.String")});
	emitter.emitInsn(iof, stack);

	ASSERT_EQ(1u, stack.size());
	EXPECT_EQ("obj instanceof String", stack[0].text);
}

TEST(JavaExprEmitter, ReturnValueStatement)
{
	BcMethod method;
	method.access = BcAccess::Static;
	ReconstructResult recon = makeEmptyRecon();
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter emitter(ctx);

	std::vector<ExprNode> stack;
	BcInstruction push;
	push.id = 0;
	push.opcode = BcOpcode::PushInt;
	push.operands.push_back(BcIntOperand{0});
	emitter.emitInsn(push, stack);

	BcInstruction ret;
	ret.id = 1;
	ret.opcode = BcOpcode::ReturnValue;
	std::string result = emitter.emitInsn(ret, stack);

	EXPECT_EQ("return 0", result);
	EXPECT_TRUE(stack.empty());
}

// ─── JavaStmtEmitter integration test ────────────────────────────────────────

TEST(JavaStmtEmitter, EmitsSimpleMethodBody)
{
	// Build: int x = 1 + 2; return x;
	BcMethod method;
	method.name = "test";
	method.access = BcAccess::Static;
	method.descriptor.returnType = std::make_shared<BcType>(types::Int());

	BcLocalVar lv;
	lv.index = 0;
	lv.name = "x";
	lv.type = types::Int();
	method.locals.push_back(lv);

	auto& blk = method.cfg.addBlock();
	{
		BcInstruction i;
		i.id = 0;
		i.opcode = BcOpcode::PushInt;
		i.operands.push_back(BcIntOperand{1});
		blk.instrs.push_back(i);
	}
	{
		BcInstruction i;
		i.id = 1;
		i.opcode = BcOpcode::PushInt;
		i.operands.push_back(BcIntOperand{2});
		blk.instrs.push_back(i);
	}
	{
		BcInstruction i;
		i.id = 2;
		i.opcode = BcOpcode::Add;
		blk.instrs.push_back(i);
	}
	{
		BcInstruction i;
		i.id = 3;
		i.opcode = BcOpcode::StoreLocal;
		i.operands.push_back(BcLocalOperand{0});
		blk.instrs.push_back(i);
	}
	{
		BcInstruction i;
		i.id = 4;
		i.opcode = BcOpcode::LoadLocal;
		i.operands.push_back(BcLocalOperand{0});
		blk.instrs.push_back(i);
	}
	{
		BcInstruction i;
		i.id = 5;
		i.opcode = BcOpcode::ReturnValue;
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

TEST(JavaStmtEmitter, LocalVarDeclarations)
{
	BcMethod method;
	method.name = "test";
	method.access = BcAccess::Static;

	// Add non-param locals.
	BcLocalVar lv1;
	lv1.index = 0;
	lv1.name = "count";
	lv1.type = types::Int();
	lv1.isParam = false;
	BcLocalVar lv2;
	lv2.index = 1;
	lv2.name = "flag";
	lv2.type = types::Bool();
	lv2.isParam = false;
	method.locals = {lv1, lv2};

	auto& blk = method.cfg.addBlock();
	{
		BcInstruction i;
		i.id = 0;
		i.opcode = BcOpcode::Return;
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

// ─── Exception handlers ──────────────────────────────────────────────────────
//
// JavaStmtEmitter::tryEmitTryCatch existed and emitFrom never called it, so no
// emitted method ever contained `try`. Worse, JvmLifter::wireExceptions records
// the handler and marks the handler block but adds no CFG edge to it, and
// emitFrom walks successors -- so a handler block has no predecessor, is never
// reached, and its body was dropped from the output entirely. A decompiled
// method silently lost every catch and finally body.

namespace {

/// One instruction at a chosen bytecode offset, so a handler's protected range
/// (which is expressed in offsets) can be mapped onto blocks.
BcInstruction at(uint32_t offset, uint32_t id, BcOpcode op)
{
	BcInstruction i;
	i.id = id;
	i.offset = offset;
	i.opcode = op;
	return i;
}

/// try { field = 1; } catch (java.io.IOException e) { field = 2; }
///
/// Block 0 is the protected region, block 1 the handler, block 2 what follows.
BcMethod makeMethodWithHandler(bool isFinally = false)
{
	BcMethod method;
	method.name = "guarded";
	method.access = BcAccess::Static;

	// Two locals, so the statements in each region are told apart by name
	// rather than by a literal that could come from anywhere.
	BcLocalVar guardedVar;
	guardedVar.index = 0;
	guardedVar.name = "guardedResult";
	guardedVar.type = types::Int();
	BcLocalVar caughtVar;
	caughtVar.index = 1;
	caughtVar.name = "caughtResult";
	caughtVar.type = types::Int();
	method.locals = {guardedVar, caughtVar};

	auto& tryBlk = method.cfg.addBlock(); // block 0, offsets 0..2
	{
		BcInstruction i = at(0, 0, BcOpcode::PushInt);
		i.operands.push_back(BcIntOperand{1});
		tryBlk.instrs.push_back(i);
		BcInstruction st = at(2, 1, BcOpcode::StoreLocal);
		st.operands.push_back(BcLocalOperand{0});
		tryBlk.instrs.push_back(st);
	}

	auto& handlerBlk = method.cfg.addBlock(); // block 1, offset 10
	{
		BcInstruction i = at(10, 2, BcOpcode::PushInt);
		i.operands.push_back(BcIntOperand{2});
		handlerBlk.instrs.push_back(i);
		BcInstruction st = at(12, 3, BcOpcode::StoreLocal);
		st.operands.push_back(BcLocalOperand{1});
		handlerBlk.instrs.push_back(st);
	}

	auto& afterBlk = method.cfg.addBlock(); // block 2, offset 20
	afterBlk.instrs.push_back(at(20, 4, BcOpcode::Return));

	method.cfg.addEdge(0, 2);

	BcExceptionHandler eh;
	eh.startOffset = 0;
	eh.endOffset = 10;
	eh.handlerBlock = 1;
	eh.isFinally = isFinally;
	if (!isFinally) eh.catchType = types::Class("java.io.IOException");
	method.cfg.addExceptionHandler(eh);
	method.cfg.block(1).isExceptionHandler = true;

	return method;
}

std::string emitGuarded(const BcMethod& method)
{
	ReconstructResult recon;
	ImportSet imports;
	JavaTypePrinter tp(imports);
	JavaStmtEmitter stmtEmit(method, recon, tp);
	return stmtEmit.emitBody();
}

} // namespace

TEST(JavaStmtEmitter, ProtectedRegionIsEmittedAsATryStatement)
{
	const std::string body = emitGuarded(makeMethodWithHandler());
	EXPECT_TRUE(contains(body, "try {")) << body;
	EXPECT_TRUE(contains(body, "catch (")) << body;
	EXPECT_TRUE(contains(body, "IOException")) << body;
}

TEST(JavaStmtEmitter, ACatchBodyIsNotDropped)
{
	// The handler block has no predecessor -- wireExceptions adds no CFG edge --
	// so nothing but the catch clause can reach it. If the emitter does not
	// emit it, the statements in it are gone from the output.
	const std::string body = emitGuarded(makeMethodWithHandler());
	EXPECT_TRUE(contains(body, "caughtResult = 2")) << body;
}

TEST(JavaStmtEmitter, TheProtectedRegionsOwnStatementsAreStillEmitted)
{
	const std::string body = emitGuarded(makeMethodWithHandler());
	EXPECT_TRUE(contains(body, "guardedResult = 1")) << body;
}

TEST(JavaStmtEmitter, ACatchAllHandlerIsEmittedAsFinally)
{
	const std::string body = emitGuarded(makeMethodWithHandler(/*isFinally=*/true));
	EXPECT_TRUE(contains(body, "try {")) << body;
	EXPECT_TRUE(contains(body, "finally {")) << body;
	EXPECT_FALSE(contains(body, "catch (")) << body;
}

TEST(JavaStmtEmitter, CodeAfterTheProtectedRegionIsStillEmittedOnce)
{
	const std::string body = emitGuarded(makeMethodWithHandler());
	size_t first = body.find("return");
	ASSERT_NE(std::string::npos, first) << body;
	EXPECT_EQ(std::string::npos, body.find("return", first + 1)) << body;
}

TEST(JavaStmtEmitter, AMethodWithNoHandlersEmitsNoTry)
{
	BcMethod plain;
	plain.name = "plain";
	plain.access = BcAccess::Static;
	auto& blk = plain.cfg.addBlock();
	blk.instrs.push_back(at(0, 0, BcOpcode::Return));

	const std::string body = emitGuarded(plain);
	EXPECT_FALSE(contains(body, "try {")) << body;
}

// ─── Local variable declaration and assignment ───────────────────────────────

TEST(JavaStmtEmitter, AStoreToADeclaredLocalKeepsItsValue)
{
	// emitBlock had a "declare on first store" branch whose unused ExprNode
	// initialiser called exprStack.pop_back(). The pop happened, so the value
	// being stored was gone by the time the store was rendered and every first
	// assignment to a named local came out as `/* stack underflow */`.
	BcMethod method;
	method.name = "assign";
	method.access = BcAccess::Static;

	BcLocalVar lv;
	lv.index = 0;
	lv.name = "total";
	lv.type = types::Int();
	method.locals.push_back(lv);

	auto& blk = method.cfg.addBlock();
	{
		BcInstruction i = at(0, 0, BcOpcode::PushInt);
		i.operands.push_back(BcIntOperand{42});
		blk.instrs.push_back(i);
		BcInstruction st = at(2, 1, BcOpcode::StoreLocal);
		st.operands.push_back(BcLocalOperand{0});
		blk.instrs.push_back(st);
		blk.instrs.push_back(at(4, 2, BcOpcode::Return));
	}

	const std::string body = emitGuarded(method);
	EXPECT_TRUE(contains(body, "total = 42;")) << body;
	EXPECT_FALSE(contains(body, "stack underflow")) << body;
}

TEST(JavaStmtEmitter, ALocalIsDeclaredExactlyOnce)
{
	// emitBody() declares every non-param local before the first block, and the
	// store site declared it a second time. Two declarations of one name in one
	// scope is not Java.
	BcMethod method;
	method.name = "assign";
	method.access = BcAccess::Static;

	BcLocalVar lv;
	lv.index = 0;
	lv.name = "total";
	lv.type = types::Int();
	method.locals.push_back(lv);

	auto& blk = method.cfg.addBlock();
	{
		BcInstruction i = at(0, 0, BcOpcode::PushInt);
		i.operands.push_back(BcIntOperand{42});
		blk.instrs.push_back(i);
		BcInstruction st = at(2, 1, BcOpcode::StoreLocal);
		st.operands.push_back(BcLocalOperand{0});
		blk.instrs.push_back(st);
		blk.instrs.push_back(at(4, 2, BcOpcode::Return));
	}

	const std::string body = emitGuarded(method);
	size_t first = body.find("int total");
	ASSERT_NE(std::string::npos, first) << body;
	EXPECT_EQ(std::string::npos, body.find("int total", first + 1)) << body;
}


// The whole statement, not a substring of it: indentation, clause order and
// brace placement are the parts a `contains` check cannot see.
TEST(JavaStmtEmitter, TheEmittedTryStatementIsWellFormedJava)
{
	const std::string expected =
		"{\n"
		"    int guardedResult;\n"
		"    int caughtResult;\n"
		"    try {\n"
		"        guardedResult = 1;\n"
		"    } catch (IOException ex) {\n"
		"        caughtResult = 2;\n"
		"    }\n"
		"    return;\n"
		"}\n";
	EXPECT_EQ(expected, emitGuarded(makeMethodWithHandler()));
}

TEST(JavaStmtEmitter, TwoHandlersOnOneRangeBecomeTwoCatchClausesOfOneTry)
{
	BcMethod method = makeMethodWithHandler();

	// A second handler over the same range, with its own handler block.
	auto& second = method.cfg.addBlock(); // block 3, offset 30
	{
		BcInstruction i = at(30, 5, BcOpcode::PushInt);
		i.operands.push_back(BcIntOperand{3});
		second.instrs.push_back(i);
		BcInstruction st = at(32, 6, BcOpcode::StoreLocal);
		st.operands.push_back(BcLocalOperand{1});
		second.instrs.push_back(st);
	}

	BcExceptionHandler eh;
	eh.startOffset = 0;
	eh.endOffset = 10;
	eh.handlerBlock = 3;
	eh.catchType = types::Class("java.lang.RuntimeException");
	method.cfg.addExceptionHandler(eh);
	method.cfg.block(3).isExceptionHandler = true;

	const std::string body = emitGuarded(method);

	// One try, two clauses, in table order -- which is the order the runtime
	// tests them and therefore the source order of the catches.
	EXPECT_EQ(1u, occurrences(body, "try {")) << body;

	const size_t io = body.find("catch (IOException");
	const size_t rt = body.find("catch (RuntimeException");
	ASSERT_NE(std::string::npos, io) << body;
	ASSERT_NE(std::string::npos, rt) << body;
	EXPECT_LT(io, rt) << body;

	// Distinct variables: the old code reused one name for every clause.
	EXPECT_TRUE(contains(body, "IOException ex)")) << body;
	EXPECT_TRUE(contains(body, "RuntimeException ex1)")) << body;
}

TEST(JavaStmtEmitter, ANestedProtectedRegionComesOutNested)
{
	// Outer try over offsets [0, 30); inner try over [0, 10). Both start at
	// block 0, so the emitter has to open the wider one first or the inner one
	// swallows the outer's body.
	BcMethod method;
	method.name = "nested";
	method.access = BcAccess::Static;

	BcLocalVar lv;
	lv.index = 0;
	lv.name = "n";
	lv.type = types::Int();
	method.locals.push_back(lv);

	auto pushStore = [&](BcBasicBlock& blk, uint32_t off, uint32_t id, int value) {
		BcInstruction i = at(off, id, BcOpcode::PushInt);
		i.operands.push_back(BcIntOperand{value});
		blk.instrs.push_back(i);
		BcInstruction st = at(off + 2, id + 1, BcOpcode::StoreLocal);
		st.operands.push_back(BcLocalOperand{0});
		blk.instrs.push_back(st);
	};

	auto& inner = method.cfg.addBlock(); // 0, offset 0   — inside both regions
	pushStore(inner, 0, 0, 1);
	auto& outer = method.cfg.addBlock(); // 1, offset 10  — outer region only
	pushStore(outer, 10, 2, 2);
	auto& innerH = method.cfg.addBlock(); // 2, offset 40 — inner handler
	pushStore(innerH, 40, 4, 3);
	auto& outerH = method.cfg.addBlock(); // 3, offset 50 — outer handler
	pushStore(outerH, 50, 6, 4);
	auto& after = method.cfg.addBlock(); // 4, offset 30
	after.instrs.push_back(at(30, 8, BcOpcode::Return));

	method.cfg.addEdge(0, 1);
	method.cfg.addEdge(1, 4);

	BcExceptionHandler innerEh;
	innerEh.startOffset = 0;
	innerEh.endOffset = 10;
	innerEh.handlerBlock = 2;
	innerEh.catchType = types::Class("java.io.IOException");
	method.cfg.addExceptionHandler(innerEh);

	BcExceptionHandler outerEh;
	outerEh.startOffset = 0;
	outerEh.endOffset = 30;
	outerEh.handlerBlock = 3;
	outerEh.catchType = types::Class("java.lang.Exception");
	method.cfg.addExceptionHandler(outerEh);

	const std::string body = emitGuarded(method);

	const size_t outerTry = body.find("try {");
	ASSERT_NE(std::string::npos, outerTry) << body;
	const size_t innerTry = body.find("try {", outerTry + 1);
	ASSERT_NE(std::string::npos, innerTry) << body;

	// The inner try is indented further than the outer one.
	const size_t outerLine = body.rfind('\n', outerTry) + 1;
	const size_t innerLine = body.rfind('\n', innerTry) + 1;
	EXPECT_LT(outerTry - outerLine, innerTry - innerLine) << body;

	// The inner catch closes before the outer one opens.
	const size_t innerCatch = body.find("catch (IOException");
	const size_t outerCatch = body.find("catch (Exception");
	ASSERT_NE(std::string::npos, innerCatch) << body;
	ASSERT_NE(std::string::npos, outerCatch) << body;
	EXPECT_LT(innerCatch, outerCatch) << body;
}

TEST(JavaStmtEmitter, AHandlerWithAnEmptyOrInvertedRangeIsIgnored)
{
	BcMethod method = makeMethodWithHandler();
	// endOffset <= startOffset protects nothing; the old code would still have
	// emitted a catch clause for it.
	BcExceptionHandler bad;
	bad.startOffset = 40;
	bad.endOffset = 40;
	bad.handlerBlock = 1;
	bad.catchType = types::Class("java.lang.Error");
	method.cfg.addExceptionHandler(bad);

	const std::string body = emitGuarded(method);
	EXPECT_FALSE(contains(body, "Error")) << body;
}

TEST(JavaStmtEmitter, AHandlerNamingABlockThatDoesNotExistIsIgnored)
{
	BcMethod method = makeMethodWithHandler();
	BcExceptionHandler bad;
	bad.startOffset = 0;
	bad.endOffset = 10;
	bad.handlerBlock = 9999;
	bad.catchType = types::Class("java.lang.Error");
	method.cfg.addExceptionHandler(bad);

	const std::string body = emitGuarded(method);
	EXPECT_FALSE(contains(body, "Error")) << body;
	EXPECT_TRUE(contains(body, "IOException")) << body;
}

// ─── Names that begin with L are not descriptors ─────────────────────────────

// slashToDot stripped a leading 'L' from every name longer than one character,
// on the theory that it came from a JVM descriptor. JVMS 4.3.2 spells an object
// type "L" ClassName ";" -- the 'L' and the ';' come as a pair -- and
// ConstPool::className hands back the internal form, which has neither. So the
// strip only ever fired on real names: List became ist, Locale became ocale.
TEST(JavaTypePrinter, ADefaultPackageClassBeginningWithLKeepsItsFirstLetter)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	for (const char* name: {"List", "Long", "Locale", "Lock", "Loader"})
	{
		BcRefType ref;
		ref.kind = BcRefKind::Class;
		ref.className = name;
		EXPECT_EQ(std::string(name), tp.printNoImport(BcType{ref}));
	}
}

// And the descriptor form still loses both halves of the wrapper.
TEST(JavaTypePrinter, ADescriptorStillLosesItsLAndSemicolon)
{
	ImportSet imports;
	JavaTypePrinter tp(imports);
	BcRefType ref;
	ref.kind = BcRefKind::Class;
	ref.className = "Ljava/lang/String;";
	EXPECT_EQ("String", tp.printNoImport(BcType{ref}));
	ref.className = "java/lang/String";
	EXPECT_EQ("String", tp.printNoImport(BcType{ref}));
}

// emitMethodCall stops collecting arguments when the expression stack runs out,
// so `args` can be empty while `hasThis` is true. `args.begin() + 1` is then one
// past the end and the range constructor sees a negative distance, which throws
// a length_error nobody catches -- std::terminate out of the emitter.
TEST(JavaExprEmitter, AnInstanceCallWithAnEmptyStackDoesNotThrow)
{
	BcMethod method;
	method.name = "f";
	ReconstructResult recon;
	ImportSet imports;
	JavaTypePrinter tp(imports);
	ExprContext ctx(method, recon, tp);
	JavaExprEmitter em(ctx);

	BcInstruction insn;
	insn.opcode = BcOpcode::InvokeVirtual;
	BcMethodRef mref;
	mref.owner = "java/io/PrintStream";
	mref.name = "println";
	insn.operands.push_back(mref);

	std::vector<ExprNode> stack; // deliberately empty
	std::string out;
	ASSERT_NO_THROW(out = em.emitInsn(insn, stack));
	EXPECT_NE(std::string::npos, out.find("println"));
}
