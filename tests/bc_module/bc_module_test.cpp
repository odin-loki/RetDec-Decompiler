/**
 * @file tests/bc_module/bc_module_test.cpp
 * @brief Unit tests for BcModule — the managed-language IR.
 *
 * Coverage:
 *   - BcType equality and string rendering (all primitive kinds, ref kinds,
 *     generics, arrays, wildcards, function types).
 *   - BcInstruction stack-effect computation.
 *   - BcCFG: predecessor/successor correctness, exception handlers,
 *     CFG verification.
 *   - BcModule: string pool interning, class/field/method management,
 *     BcModule::verify.
 *   - JSON round-trip: serialise then deserialise, verify structural equality.
 *   - 5 golden BcModules: Hello World in Java, C#, Python, Wasm, Lua.
 */

#include <memory>
#include "retdec/bc_module/bc_json.h"
#include "retdec/bc_module/bc_module.h"

#include <gtest/gtest.h>
#include <string>

using namespace retdec::bc_module;
using namespace retdec::bc_module::types;
using namespace retdec::bc_module::json;

// ══════════════════════════════════════════════════════════════════════════════
// BcType — equality
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcTypeEquality, PrimSameKind) {
    EXPECT_EQ(Int(), Int());
    EXPECT_EQ(Void(), Void());
    EXPECT_EQ(Long(), Long());
    EXPECT_EQ(Double(), Double());
    EXPECT_EQ(Bool(), Bool());
}

TEST(BcTypeEquality, PrimDifferentKind) {
    EXPECT_NE(Int(), Long());
    EXPECT_NE(Float(), Double());
    EXPECT_NE(Bool(), Byte());
    EXPECT_NE(Void(), Bool());
}

TEST(BcTypeEquality, ClassSameRef) {
    EXPECT_EQ(Class("java/lang/String"), Class("java/lang/String"));
    EXPECT_EQ(Class("System.Int32"),     Class("System.Int32"));
}

TEST(BcTypeEquality, ClassDifferentRef) {
    EXPECT_NE(Class("java/lang/String"), Class("java/lang/Object"));
    EXPECT_NE(Class("A"), Class("B"));
}

TEST(BcTypeEquality, ArrayEquality) {
    EXPECT_EQ(Array(Int()), Array(Int()));
    EXPECT_EQ(Array(Class("java/lang/String")), Array(Class("java/lang/String")));
    EXPECT_NE(Array(Int()), Array(Long()));
    EXPECT_NE(Array(Int(), 1), Array(Int(), 2));
}

TEST(BcTypeEquality, GenericEquality) {
    auto listInt  = Generic(Class("java/util/List"), {Int()});
    auto listInt2 = Generic(Class("java/util/List"), {Int()});
    auto listLong = Generic(Class("java/util/List"), {Long()});
    EXPECT_EQ(listInt, listInt2);
    EXPECT_NE(listInt, listLong);
}

TEST(BcTypeEquality, WildcardEquality) {
    EXPECT_EQ(Wildcard(), Wildcard());
    EXPECT_NE(Wildcard(), BoundedAbove(Int()));
    EXPECT_EQ(BoundedAbove(Class("Comparable")), BoundedAbove(Class("Comparable")));
    EXPECT_NE(BoundedAbove(Int()), BoundedBelow(Int()));
}

TEST(BcTypeEquality, FuncEquality) {
    auto f1 = Func({Int(), Long()}, Bool());
    auto f2 = Func({Int(), Long()}, Bool());
    auto f3 = Func({Int()},         Bool());
    EXPECT_EQ(f1, f2);
    EXPECT_NE(f1, f3);
}

TEST(BcTypeEquality, PrimVsRef) {
    EXPECT_NE(Int(), Class("java/lang/Integer"));
}

// ══════════════════════════════════════════════════════════════════════════════
// BcType — string rendering
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcTypeString, PrimToString) {
    EXPECT_EQ(Void().toString(),   "void");
    EXPECT_EQ(Bool().toString(),   "boolean");
    EXPECT_EQ(Int().toString(),    "int");
    EXPECT_EQ(Long().toString(),   "long");
    EXPECT_EQ(Float().toString(),  "float");
    EXPECT_EQ(Double().toString(), "double");
    EXPECT_EQ(Char().toString(),   "char");
    EXPECT_EQ(Short().toString(),  "short");
    EXPECT_EQ(Byte().toString(),   "byte");
    EXPECT_EQ(NilType().toString(),"nil");
}

TEST(BcTypeString, ClassToString) {
    EXPECT_EQ(Class("java/lang/String").toString(), "java/lang/String");
    EXPECT_EQ(Class("MyClass").toString(), "MyClass");
}

TEST(BcTypeString, ArrayToString) {
    EXPECT_EQ(Array(Int()).toString(), "int[]");
    EXPECT_EQ(Array(Int(), 2).toString(), "int[][]");
}

TEST(BcTypeString, GenericToString) {
    auto t = Generic(Class("java/util/List"), {Class("java/lang/String")});
    EXPECT_EQ(t.toString(), "java/util/List<java/lang/String>");
}

TEST(BcTypeString, WildcardToString) {
    EXPECT_EQ(Wildcard().toString(), "?");
    EXPECT_EQ(BoundedAbove(Int()).toString(), "? extends int");
    EXPECT_EQ(BoundedBelow(Long()).toString(), "? super long");
}

TEST(BcTypeString, FuncToString) {
    auto t = Func({Int(), Bool()}, Void());
    EXPECT_EQ(t.toString(), "(int, boolean) -> void");
}

// ══════════════════════════════════════════════════════════════════════════════
// BcType — JVM descriptor
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcTypeDescriptor, PrimDescriptors) {
    EXPECT_EQ(Void().jvmDescriptor(),   "V");
    EXPECT_EQ(Bool().jvmDescriptor(),   "Z");
    EXPECT_EQ(Byte().jvmDescriptor(),   "B");
    EXPECT_EQ(Char().jvmDescriptor(),   "C");
    EXPECT_EQ(Short().jvmDescriptor(),  "S");
    EXPECT_EQ(Int().jvmDescriptor(),    "I");
    EXPECT_EQ(Long().jvmDescriptor(),   "J");
    EXPECT_EQ(Float().jvmDescriptor(),  "F");
    EXPECT_EQ(Double().jvmDescriptor(), "D");
}

TEST(BcTypeDescriptor, ClassDescriptor) {
    EXPECT_EQ(Class("java/lang/String").jvmDescriptor(), "Ljava/lang/String;");
    EXPECT_EQ(Object().jvmDescriptor(), "Ljava/lang/Object;");
}

TEST(BcTypeDescriptor, ArrayDescriptor) {
    EXPECT_EQ(Array(Int()).jvmDescriptor(), "[I");
    EXPECT_EQ(Array(Class("java/lang/String")).jvmDescriptor(), "[Ljava/lang/String;");
    EXPECT_EQ(Array(Int(), 2).jvmDescriptor(), "[[I");
}

TEST(BcTypeDescriptor, FuncDescriptor) {
    auto ft = Func({Int(), Class("java/lang/String")}, Void());
    EXPECT_EQ(ft.jvmDescriptor(), "(ILjava/lang/String;)V");
}

// ══════════════════════════════════════════════════════════════════════════════
// BcType — CLR and Python names
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcTypeClr, PrimClrNames) {
    EXPECT_EQ(Int().clrName(),    "System.Int32");
    EXPECT_EQ(Long().clrName(),   "System.Int64");
    EXPECT_EQ(Float().clrName(),  "System.Single");
    EXPECT_EQ(Double().clrName(), "System.Double");
    EXPECT_EQ(Bool().clrName(),   "System.Boolean");
    EXPECT_EQ(Void().clrName(),   "System.Void");
}

TEST(BcTypeClr, ArrayClrName) {
    EXPECT_EQ(Array(Int()).clrName(), "System.Int32[]");
}

TEST(BcTypePython, PrimPythonAnnotations) {
    EXPECT_EQ(Int().pythonAnnotation(),    "int");
    EXPECT_EQ(Bool().pythonAnnotation(),   "bool");
    EXPECT_EQ(Float().pythonAnnotation(),  "float");
    EXPECT_EQ(Double().pythonAnnotation(), "float");
    EXPECT_EQ(Void().pythonAnnotation(),   "None");
    EXPECT_EQ(Char().pythonAnnotation(),   "str");
}

TEST(BcTypePython, ClassPythonAnnotation) {
    EXPECT_EQ(Class("java/lang/String").pythonAnnotation(), "str");
    EXPECT_EQ(Class("java/lang/Object").pythonAnnotation(), "object");
    EXPECT_EQ(Class("com/example/Foo").pythonAnnotation(),  "Foo");
}

// ══════════════════════════════════════════════════════════════════════════════
// BcType — property queries
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcTypeProps, IsVoid) {
    EXPECT_TRUE(Void().isVoid());
    EXPECT_FALSE(Int().isVoid());
    EXPECT_FALSE(Object().isVoid());
}

TEST(BcTypeProps, IsIntegral) {
    EXPECT_TRUE(Int().isIntegral());
    EXPECT_TRUE(Long().isIntegral());
    EXPECT_TRUE(Bool().isIntegral());
    EXPECT_TRUE(Char().isIntegral());
    EXPECT_FALSE(Float().isIntegral());
    EXPECT_FALSE(Double().isIntegral());
    EXPECT_FALSE(Class("Foo").isIntegral());
}

TEST(BcTypeProps, IsFloating) {
    EXPECT_TRUE(Float().isFloating());
    EXPECT_TRUE(Double().isFloating());
    EXPECT_FALSE(Int().isFloating());
}

TEST(BcTypeProps, IsNullable) {
    EXPECT_TRUE(Class("Foo").isNullable());
    EXPECT_TRUE(Array(Int()).isNullable());
    EXPECT_TRUE(NilType().isNullable());
    EXPECT_FALSE(Int().isNullable());
    EXPECT_FALSE(Long().isNullable());
}

TEST(BcTypeProps, JvmSlots) {
    EXPECT_EQ(Int().jvmSlots(),    1);
    EXPECT_EQ(Long().jvmSlots(),   2);
    EXPECT_EQ(Double().jvmSlots(), 2);
    EXPECT_EQ(Float().jvmSlots(),  1);
    EXPECT_EQ(Object().jvmSlots(), 1);
}

// ══════════════════════════════════════════════════════════════════════════════
// BcInstruction — stack effects
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcStackEffect, BasicOps) {
    EXPECT_EQ(stackEffectOf(BcOpcode::Add).pop,  2);
    EXPECT_EQ(stackEffectOf(BcOpcode::Add).push, 1);
    EXPECT_EQ(stackEffectOf(BcOpcode::Add).net(), -1);
}

TEST(BcStackEffect, Push) {
    auto e = stackEffectOf(BcOpcode::PushInt);
    EXPECT_EQ(e.pop, 0);
    EXPECT_EQ(e.push, 1);
}

TEST(BcStackEffect, StoreLocal) {
    auto e = stackEffectOf(BcOpcode::StoreLocal);
    EXPECT_EQ(e.pop, 1);
    EXPECT_EQ(e.push, 0);
}

TEST(BcStackEffect, ArrayLoad) {
    auto e = stackEffectOf(BcOpcode::ArrayLoad);
    EXPECT_EQ(e.pop, 2);
    EXPECT_EQ(e.push, 1);
}

TEST(BcStackEffect, ArrayStore) {
    auto e = stackEffectOf(BcOpcode::ArrayStore);
    EXPECT_EQ(e.pop, 3);
    EXPECT_EQ(e.push, 0);
}

TEST(BcStackEffect, InvocationVariable) {
    auto e = stackEffectOf(BcOpcode::InvokeVirtual);
    EXPECT_TRUE(e.isVariable());
}

TEST(BcStackEffect, Return) {
    EXPECT_EQ(stackEffectOf(BcOpcode::Return).pop, 0);
    EXPECT_EQ(stackEffectOf(BcOpcode::Return).push, 0);
    EXPECT_EQ(stackEffectOf(BcOpcode::ReturnValue).pop, 1);
}

TEST(BcStackEffect, Nop) {
    auto e = stackEffectOf(BcOpcode::Nop);
    EXPECT_EQ(e.pop, 0);
    EXPECT_EQ(e.push, 0);
    EXPECT_EQ(e.net(), 0);
}

TEST(BcStackEffect, DupFamily) {
    EXPECT_EQ(stackEffectOf(BcOpcode::Dup).pop, 1);
    EXPECT_EQ(stackEffectOf(BcOpcode::Dup).push, 2);
    EXPECT_EQ(stackEffectOf(BcOpcode::Swap).pop, 2);
    EXPECT_EQ(stackEffectOf(BcOpcode::Swap).push, 2);
}

TEST(BcStackEffect, FieldAccess) {
    EXPECT_EQ(stackEffectOf(BcOpcode::GetField).pop, 1);
    EXPECT_EQ(stackEffectOf(BcOpcode::GetField).push, 1);
    EXPECT_EQ(stackEffectOf(BcOpcode::GetStatic).pop, 0);
    EXPECT_EQ(stackEffectOf(BcOpcode::PutStatic).pop, 1);
}

TEST(BcStackEffect, WasmOps) {
    EXPECT_EQ(stackEffectOf(BcOpcode::WasmLoad).pop, 1);
    EXPECT_EQ(stackEffectOf(BcOpcode::WasmStore).pop, 2);
    EXPECT_EQ(stackEffectOf(BcOpcode::WasmSelect).pop, 3);
    EXPECT_EQ(stackEffectOf(BcOpcode::WasmSelect).push, 1);
}

TEST(BcStackEffect, LuaOps) {
    EXPECT_EQ(stackEffectOf(BcOpcode::LuaGetField).pop, 1);
    EXPECT_EQ(stackEffectOf(BcOpcode::LuaGetField).push, 1);
    EXPECT_EQ(stackEffectOf(BcOpcode::LuaSetField).pop, 2);
    EXPECT_EQ(stackEffectOf(BcOpcode::LuaLength).pop, 1);
    EXPECT_EQ(stackEffectOf(BcOpcode::LuaLength).push, 1);
}

// ══════════════════════════════════════════════════════════════════════════════
// BcCFG
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcCFG, AddBlock) {
    BcCFG cfg;
    auto& b0 = cfg.addBlock();
    auto& b1 = cfg.addBlock();
    EXPECT_EQ(b0.id, 0u);
    EXPECT_EQ(b1.id, 1u);
    EXPECT_EQ(cfg.blockCount(), 2u);
}

TEST(BcCFG, AddEdge) {
    BcCFG cfg;
    cfg.addBlock();
    cfg.addBlock();
    cfg.addEdge(0, 1);
    EXPECT_TRUE(cfg.hasEdge(0, 1));
    EXPECT_FALSE(cfg.hasEdge(1, 0));
    const auto& b0 = cfg.block(0);
    const auto& b1 = cfg.block(1);
    EXPECT_EQ(b0.succs.size(), 1u); EXPECT_EQ(b0.succs[0], 1u);
    EXPECT_EQ(b1.preds.size(), 1u); EXPECT_EQ(b1.preds[0], 0u);
}

TEST(BcCFG, RemoveEdge) {
    BcCFG cfg;
    cfg.addBlock(); cfg.addBlock();
    cfg.addEdge(0, 1);
    cfg.removeEdge(0, 1);
    EXPECT_FALSE(cfg.hasEdge(0, 1));
    EXPECT_TRUE(cfg.block(0).succs.empty());
    EXPECT_TRUE(cfg.block(1).preds.empty());
}

TEST(BcCFG, NoDuplicateEdge) {
    BcCFG cfg;
    cfg.addBlock(); cfg.addBlock();
    cfg.addEdge(0, 1);
    cfg.addEdge(0, 1); // duplicate — should be ignored
    EXPECT_EQ(cfg.block(0).succs.size(), 1u);
    EXPECT_EQ(cfg.block(1).preds.size(), 1u);
}

TEST(BcCFG, MultipleSuccessors) {
    BcCFG cfg;
    cfg.addBlock(); cfg.addBlock(); cfg.addBlock();
    cfg.addEdge(0, 1);
    cfg.addEdge(0, 2);
    EXPECT_EQ(cfg.block(0).succs.size(), 2u);
    EXPECT_EQ(cfg.block(1).preds.size(), 1u);
    EXPECT_EQ(cfg.block(2).preds.size(), 1u);
}

TEST(BcCFG, ExceptionHandler) {
    BcCFG cfg;
    cfg.addBlock(); // 0 — body
    cfg.addBlock(); // 1 — handler
    BcExceptionHandler eh;
    eh.startOffset  = 0;
    eh.endOffset    = 10;
    eh.handlerBlock = 1;
    eh.catchType    = Class("java/lang/RuntimeException");
    cfg.addExceptionHandler(eh);
    EXPECT_EQ(cfg.handlers().size(), 1u);
    EXPECT_EQ(cfg.handlers()[0].handlerBlock, 1u);
    EXPECT_TRUE(cfg.handlers()[0].catchType.has_value());
}

TEST(BcCFG, Verify_OK) {
    BcCFG cfg;
    cfg.addBlock(); cfg.addBlock();
    cfg.addEdge(0, 1);
    std::string err;
    EXPECT_TRUE(cfg.verify(err));
}

TEST(BcCFG, Verify_BadSuccessor) {
    BcCFG cfg;
    cfg.addBlock(); // block 0
    // Manually corrupt successor list.
    cfg.block(0).succs.push_back(99);
    std::string err;
    EXPECT_FALSE(cfg.verify(err));
    EXPECT_FALSE(err.empty());
}

TEST(BcCFG, Verify_BadHandler) {
    BcCFG cfg;
    cfg.addBlock(); // block 0
    BcExceptionHandler eh;
    eh.handlerBlock = 99;
    cfg.addExceptionHandler(eh);
    std::string err;
    EXPECT_FALSE(cfg.verify(err));
}

TEST(BcCFG, TerminatorDetection) {
    BcCFG cfg;
    auto& b = cfg.addBlock();
    EXPECT_FALSE(b.hasTerminator());
    BcInstruction ret;
    ret.opcode = BcOpcode::Return;
    b.instrs.push_back(ret);
    EXPECT_TRUE(b.hasTerminator());
    EXPECT_EQ(b.terminator()->opcode, BcOpcode::Return);
}

// ══════════════════════════════════════════════════════════════════════════════
// BcModule
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcModule, ConstructAndName) {
    BcModule mod("TestModule", SourceLang::Java);
    EXPECT_EQ(mod.name(), "TestModule");
    EXPECT_EQ(mod.sourceLang(), SourceLang::Java);
}

TEST(BcModule, StringPoolInterning) {
    BcModule mod("M", SourceLang::Unknown);
    uint32_t i1 = mod.internString("hello");
    uint32_t i2 = mod.internString("world");
    uint32_t i3 = mod.internString("hello");   // should return same index
    EXPECT_EQ(i1, i3);
    EXPECT_NE(i1, i2);
    EXPECT_EQ(mod.string(i1), "hello");
    EXPECT_EQ(mod.string(i2), "world");
    EXPECT_EQ(mod.stringCount(), 2u);
}

TEST(BcModule, AddAndFindClass) {
    BcModule mod("M", SourceLang::Java);
    BcClass cls;
    cls.name   = "Main";
    cls.fqName = "com/example/Main";
    mod.addClass(cls);
    EXPECT_NE(mod.findClass("com/example/Main"), nullptr);
    EXPECT_EQ(mod.findClass("com/example/Main")->name, "Main");
    EXPECT_EQ(mod.findClass("DoesNotExist"), nullptr);
}

TEST(BcModule, ClassFindMethod) {
    BcClass cls;
    cls.name = "Foo";
    BcMethod m;
    m.name = "bar";
    cls.methods.push_back(m);
    EXPECT_NE(cls.findMethod("bar"), nullptr);
    EXPECT_EQ(cls.findMethod("baz"), nullptr);
}

TEST(BcModule, ClassFindField) {
    BcClass cls;
    BcField f;
    f.name = "count";
    f.type = Int();
    cls.fields.push_back(f);
    EXPECT_NE(cls.findField("count"), nullptr);
    EXPECT_EQ(cls.findField("size"), nullptr);
}

TEST(BcModule, ExternalRefs) {
    BcModule mod("M", SourceLang::Java);
    mod.addExternalRef("java/io/PrintStream", "import java.io.PrintStream;");
    EXPECT_EQ(mod.externalRefs().at("java/io/PrintStream"), "import java.io.PrintStream;");
}

TEST(BcModule, SourceLangName) {
    EXPECT_EQ(sourceLangName(SourceLang::Java),        "Java");
    EXPECT_EQ(sourceLangName(SourceLang::CSharp),      "CSharp");
    EXPECT_EQ(sourceLangName(SourceLang::Python),      "Python");
    EXPECT_EQ(sourceLangName(SourceLang::WebAssembly), "WebAssembly");
    EXPECT_EQ(sourceLangName(SourceLang::Lua),         "Lua");
    EXPECT_EQ(sourceLangName(SourceLang::Unknown),     "Unknown");
}

TEST(BcModule, VerifyEmptyModule) {
    BcModule mod("Empty", SourceLang::Unknown);
    std::string err;
    EXPECT_TRUE(mod.verify(err));
}

TEST(BcModule, VerifyWithValidCFG) {
    BcModule mod("M", SourceLang::Java);
    BcClass cls;
    cls.fqName = "Foo";
    BcMethod mth;
    mth.name = "go";
    auto& b0 = mth.cfg.addBlock();
    auto& b1 = mth.cfg.addBlock();
    mth.cfg.addEdge(0, 1);
    BcInstruction ret; ret.opcode = BcOpcode::Return;
    b1.instrs.push_back(ret);
    (void)b0;
    cls.methods.push_back(std::move(mth));
    mod.addClass(std::move(cls));
    std::string err;
    EXPECT_TRUE(mod.verify(err));
}

// ══════════════════════════════════════════════════════════════════════════════
// JSON round-trip
// ══════════════════════════════════════════════════════════════════════════════

TEST(BcJson, SerialiseNonEmpty) {
    BcModule mod("Hello", SourceLang::Java);
    mod.internString("Hello, World!");
    std::string s = serialiseModule(mod);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("\"Hello\""), std::string::npos);
    EXPECT_NE(s.find("Java"), std::string::npos);
}

TEST(BcJson, RoundTripEmpty) {
    BcModule mod("Empty", SourceLang::Unknown);
    std::string diff;
    EXPECT_TRUE(roundTripEquals(mod, &diff)) << diff;
}

TEST(BcJson, RoundTripWithClass) {
    BcModule mod("Lib", SourceLang::Java);
    BcClass cls;
    cls.name   = "Util";
    cls.fqName = "com/example/Util";
    BcField f;
    f.name = "value";
    f.type = Int();
    cls.fields.push_back(f);
    BcMethod m;
    m.name = "compute";
    cls.methods.push_back(std::move(m));
    mod.addClass(std::move(cls));
    std::string diff;
    EXPECT_TRUE(roundTripEquals(mod, &diff)) << diff;
}

TEST(BcJson, DeserialiseInvalidJson) {
    auto res = deserialiseModule("{not valid json!!!");
    // Parser either fails or returns partial — should not crash.
    SUCCEED(); // reaching here is the test
}

TEST(BcJson, RoundTripStringPool) {
    BcModule mod("M", SourceLang::Java);
    mod.internString("alpha");
    mod.internString("beta");
    mod.internString("gamma");
    std::string diff;
    EXPECT_TRUE(roundTripEquals(mod, &diff)) << diff;
}

// ══════════════════════════════════════════════════════════════════════════════
// Golden file BcModules — Hello World in 5 languages
//
// Each golden module is hand-crafted to represent the simplest possible
// "Hello, World!" program in the respective language.  The test verifies:
//   1. The module serialises to non-empty JSON without crashing.
//   2. The JSON round-trips correctly (names, class count, field/method count).
//   3. The module passes BcModule::verify.
// ══════════════════════════════════════════════════════════════════════════════

// ── Helper ───────────────────────────────────────────────────────────────────
static BcMethod makeVoidMethod(const std::string& name, bool isStatic = false) {
    BcMethod m;
    m.name = name;
    BcFuncType ft;
    ft.returnType = std::make_shared<BcType>(Void());
    m.descriptor  = ft;
    if (isStatic) m.access = BcAccess::Static;
    auto& entry = m.cfg.addBlock();
    // PushString "Hello, World!"
    BcInstruction pushS;
    pushS.id     = 0;
    pushS.opcode = BcOpcode::PushString;
    pushS.operands.push_back(BcStringOperand{"Hello, World!"});
    pushS.effect  = {0, 1};
    entry.instrs.push_back(pushS);
    // Return
    BcInstruction ret;
    ret.id     = 1;
    ret.opcode = BcOpcode::Return;
    ret.effect = {0, 0};
    entry.instrs.push_back(ret);
    return m;
}

// ── 1. Java Hello World ───────────────────────────────────────────────────────
TEST(GoldenModule, JavaHelloWorld) {
    BcModule mod("HelloWorld", SourceLang::Java);
    mod.internString("Hello, World!");
    mod.addExternalRef("java/io/PrintStream", "import java.io.PrintStream;");

    BcClass cls;
    cls.name   = "HelloWorld";
    cls.fqName = "HelloWorld";
    cls.superClass = Object();
    cls.access = BcAccess::Public;
    cls.sourceFile = "HelloWorld.java";

    // public static void main(String[] args)
    BcMethod main = makeVoidMethod("main", /*isStatic=*/true);
    main.isConstructor = false;
    main.access = BcAccess::Public | BcAccess::Static;
    main.paramNames.push_back("args");
    main.descriptor.params.push_back(std::make_shared<BcType>(Array(String())));
    cls.methods.push_back(std::move(main));

    mod.addClass(std::move(cls));

    // Validate
    std::string err;
    EXPECT_TRUE(mod.verify(err)) << err;

    // Round-trip
    std::string diff;
    EXPECT_TRUE(roundTripEquals(mod, &diff)) << diff;

    // Serialised JSON sanity checks
    std::string s = serialiseModule(mod);
    EXPECT_NE(s.find("HelloWorld"), std::string::npos);
    EXPECT_NE(s.find("Java"), std::string::npos);
    EXPECT_NE(s.find("main"), std::string::npos);
}

// ── 2. C# Hello World ────────────────────────────────────────────────────────
TEST(GoldenModule, CSharpHelloWorld) {
    BcModule mod("HelloWorld", SourceLang::CSharp);
    mod.internString("Hello, World!");

    BcClass cls;
    cls.name   = "Program";
    cls.fqName = "HelloWorld.Program";
    cls.superClass = ClrObject();
    cls.access = BcAccess::Public;

    // static void Main(string[] args)
    BcMethod main = makeVoidMethod("Main", /*isStatic=*/true);
    main.access = BcAccess::Public | BcAccess::Static;
    main.paramNames.push_back("args");
    main.descriptor.params.push_back(
        std::make_shared<BcType>(Array(ClrString())));
    cls.methods.push_back(std::move(main));

    mod.addClass(std::move(cls));

    std::string err;
    EXPECT_TRUE(mod.verify(err)) << err;
    std::string diff;
    EXPECT_TRUE(roundTripEquals(mod, &diff)) << diff;

    std::string s = serialiseModule(mod);
    EXPECT_NE(s.find("CSharp"), std::string::npos);
    EXPECT_NE(s.find("Program"), std::string::npos);
}

// ── 3. Python Hello World ─────────────────────────────────────────────────────
TEST(GoldenModule, PythonHelloWorld) {
    BcModule mod("hello", SourceLang::Python);
    mod.internString("Hello, World!");

    // Python modules have no explicit class; represent as __module__ class.
    BcClass cls;
    cls.name   = "__module__";
    cls.fqName = "hello.__module__";
    cls.access = BcAccess::None;

    // The top-level code object maps to a method named "<module>".
    BcMethod moduleBody = makeVoidMethod("<module>", /*isStatic=*/true);
    cls.methods.push_back(std::move(moduleBody));

    mod.addClass(std::move(cls));

    std::string err;
    EXPECT_TRUE(mod.verify(err)) << err;
    std::string diff;
    EXPECT_TRUE(roundTripEquals(mod, &diff)) << diff;

    std::string s = serialiseModule(mod);
    EXPECT_NE(s.find("Python"), std::string::npos);
}

// ── 4. WebAssembly Hello World ────────────────────────────────────────────────
TEST(GoldenModule, WasmHelloWorld) {
    BcModule mod("hello", SourceLang::WebAssembly);
    mod.internString("Hello, World!");
    mod.targetFramework = "wasm32";

    // Wasm: exported function "start" (i32 offset, i32 len → void).
    BcClass cls;
    cls.name   = "__module__";
    cls.fqName = "hello.__module__";
    cls.access = BcAccess::None;

    BcMethod start;
    start.name = "start";
    BcFuncType ft;
    ft.params.push_back(std::make_shared<BcType>(Int())); // ptr
    ft.params.push_back(std::make_shared<BcType>(Int())); // len
    ft.returnType = std::make_shared<BcType>(Void());
    start.descriptor = ft;
    start.access     = BcAccess::Public | BcAccess::Static;

    auto& b = start.cfg.addBlock();
    BcInstruction loadLocal;
    loadLocal.opcode = BcOpcode::WasmLoad;
    loadLocal.operands.push_back(BcLocalOperand{0});
    loadLocal.effect = {1, 1};
    b.instrs.push_back(loadLocal);
    BcInstruction ret;
    ret.opcode = BcOpcode::Return;
    b.instrs.push_back(ret);

    cls.methods.push_back(std::move(start));
    mod.addClass(std::move(cls));

    std::string err;
    EXPECT_TRUE(mod.verify(err)) << err;
    std::string diff;
    EXPECT_TRUE(roundTripEquals(mod, &diff)) << diff;

    std::string s = serialiseModule(mod);
    EXPECT_NE(s.find("WebAssembly"), std::string::npos);
}

// ── 5. Lua Hello World ────────────────────────────────────────────────────────
TEST(GoldenModule, LuaHelloWorld) {
    BcModule mod("hello", SourceLang::Lua);
    mod.internString("Hello, World!");

    BcClass cls;
    cls.name   = "__chunk__";
    cls.fqName = "hello.__chunk__";
    cls.access = BcAccess::None;

    // Top-level Lua chunk maps to a "main chunk" method.
    BcMethod chunk = makeVoidMethod("<main chunk>", /*isStatic=*/true);
    // Lua "print" call.
    BcInstruction pyLoad;
    pyLoad.opcode = BcOpcode::PyLoadName;   // re-use for name load
    pyLoad.operands.push_back(BcStringOperand{"print"});
    pyLoad.effect = {0, 1};

    BcInstruction pushS;
    pushS.opcode = BcOpcode::PushString;
    pushS.operands.push_back(BcStringOperand{"Hello, World!"});
    pushS.effect = {0, 1};

    BcInstruction call;
    call.opcode = BcOpcode::LuaCall;
    call.operands.push_back(BcIntOperand{1}); // nargs
    call.operands.push_back(BcIntOperand{0}); // nresults
    call.effect = {-1, -1};  // variable

    auto& b = chunk.cfg.block(0);
    b.instrs.insert(b.instrs.begin(), pyLoad);
    b.instrs.insert(b.instrs.begin() + 1, pushS);
    b.instrs.insert(b.instrs.begin() + 2, call);

    cls.methods.push_back(std::move(chunk));
    mod.addClass(std::move(cls));

    std::string err;
    EXPECT_TRUE(mod.verify(err)) << err;
    std::string diff;
    EXPECT_TRUE(roundTripEquals(mod, &diff)) << diff;

    std::string s = serialiseModule(mod);
    EXPECT_NE(s.find("Lua"), std::string::npos);
}
