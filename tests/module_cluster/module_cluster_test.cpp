/**
 * @file tests/module_cluster/module_cluster_test.cpp
 * @brief Unit tests for Module Clustering and CMake Project Generation.
 */

#include "retdec/module_cluster/module_cluster.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <string>

using namespace retdec::module_cluster;

// ─── LouvainClusterer ─────────────────────────────────────────────────────────

TEST(LouvainClusterer, EmptyGraphReturnsEmpty) {
    LouvainClusterer c;
    CallGraph g;
    auto result = c.cluster(g);
    EXPECT_TRUE(result.empty());
}

TEST(LouvainClusterer, SingleNodeOwnCommunity) {
    LouvainClusterer c;
    CallGraph g;
    FunctionMeta fn; fn.name = "foo"; fn.address = 0x1000;
    g.functions.push_back(fn);
    auto result = c.cluster(g);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.begin()->first, "foo");
}

TEST(LouvainClusterer, TwoConnectedNodesInSameCommunity) {
    LouvainClusterer c;
    CallGraph g;
    FunctionMeta f1; f1.name = "a";
    FunctionMeta f2; f2.name = "b";
    g.functions = {f1, f2};
    g.edges.push_back({"a", "b", 10});
    auto result = c.cluster(g);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result["a"], result["b"]);
}

TEST(LouvainClusterer, TwoDisconnectedGroupsInDifferentCommunities) {
    LouvainClusterer c(LouvainConfig{1.0, 100, false});
    CallGraph g;
    // Group 1: a-b-c strongly connected
    for (const char* n : {"a","b","c"}) { FunctionMeta f; f.name=n; g.functions.push_back(f); }
    // Group 2: x-y-z strongly connected
    for (const char* n : {"x","y","z"}) { FunctionMeta f; f.name=n; g.functions.push_back(f); }
    // Strong intra-group edges
    g.edges = {{"a","b",100},{"b","a",100},{"b","c",100},{"c","b",100},{"a","c",100},{"c","a",100},
               {"x","y",100},{"y","x",100},{"y","z",100},{"z","y",100},{"x","z",100},{"z","x",100}};
    // No inter-group edges
    auto result = c.cluster(g);
    EXPECT_EQ(result.size(), 6u);
    // All of a,b,c should be in same community
    EXPECT_EQ(result["a"], result["b"]);
    EXPECT_EQ(result["b"], result["c"]);
    // All of x,y,z should be in same community
    EXPECT_EQ(result["x"], result["y"]);
    EXPECT_EQ(result["y"], result["z"]);
    // The two groups should be different
    EXPECT_NE(result["a"], result["x"]);
}

TEST(LouvainClusterer, ModularityIsComputedNonNeg) {
    LouvainClusterer c;
    CallGraph g;
    for (const char* n : {"a","b","c"}) { FunctionMeta f; f.name=n; g.functions.push_back(f); }
    g.edges = {{"a","b",5},{"b","c",5}};
    c.cluster(g);
    EXPECT_GE(c.lastModularity(), 0.0);
}

TEST(LouvainClusterer, SeedByNamePrefixGroups) {
    LouvainClusterer c(LouvainConfig{1.0, 100, true});
    CallGraph g;
    FunctionMeta f1; f1.name = "net_connect"; f1.namePrefix = "net_";
    FunctionMeta f2; f2.name = "net_send";    f2.namePrefix = "net_";
    FunctionMeta f3; f3.name = "crypto_init"; f3.namePrefix = "crypto_";
    g.functions = {f1, f2, f3};
    g.edges = {{"net_connect","net_send",1}};
    auto result = c.cluster(g);
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(result["net_connect"], result["net_send"]);
}

// ─── ModuleNamer ─────────────────────────────────────────────────────────────

TEST(ModuleNamer, CommonPrefixUsed) {
    ModuleNamer namer;
    std::vector<FunctionMeta> funcs;
    for (const char* n : {"net_connect","net_send","net_recv"}) {
        FunctionMeta f; f.name = n; funcs.push_back(f);
    }
    auto name = namer.name(funcs, 0);
    EXPECT_EQ(name, "net");
}

TEST(ModuleNamer, NamePrefixField) {
    ModuleNamer namer;
    FunctionMeta f1; f1.name = "func_a"; f1.namePrefix = "crypto_";
    FunctionMeta f2; f2.name = "func_b"; f2.namePrefix = "crypto_";
    auto name = namer.name({f1, f2}, 0);
    EXPECT_NE(name.find("crypto"), std::string::npos);
}

TEST(ModuleNamer, LibrarySignal) {
    ModuleNamer namer;
    FunctionMeta f;
    f.name = "foo";
    f.calledLibSyms = {"SSL_new", "SSL_connect", "BIO_new"};
    auto name = namer.name({f}, 0);
    EXPECT_NE(name.find("ssl"), std::string::npos);
}

TEST(ModuleNamer, SourceFileSignal) {
    ModuleNamer namer;
    FunctionMeta f;
    f.name       = "do_work";
    f.sourceFile = "/home/user/src/renderer.cpp";
    auto name = namer.name({f}, 0);
    EXPECT_NE(name.find("renderer"), std::string::npos);
}

TEST(ModuleNamer, FallbackId) {
    ModuleNamer namer;
    FunctionMeta f; f.name = "x";
    auto name = namer.name({f}, 7);
    EXPECT_NE(name.find("7"), std::string::npos);
}

// ─── HeaderInference ─────────────────────────────────────────────────────────

TEST(HeaderInference, StdlibForMalloc) {
    HeaderInference hi;
    auto headers = hi.infer({"malloc", "free"});
    EXPECT_NE(std::find(headers.begin(), headers.end(), "<stdlib.h>"), headers.end());
}

TEST(HeaderInference, StdioForPrintf) {
    HeaderInference hi;
    auto headers = hi.infer({"printf", "fprintf"});
    EXPECT_NE(std::find(headers.begin(), headers.end(), "<stdio.h>"), headers.end());
}

TEST(HeaderInference, OpenSSL) {
    HeaderInference hi;
    auto headers = hi.infer({"SSL_new", "SSL_connect"});
    EXPECT_FALSE(headers.empty());
    bool hasSSL = std::any_of(headers.begin(), headers.end(),
        [](const auto& h){ return h.find("openssl") != std::string::npos; });
    EXPECT_TRUE(hasSSL);
}

TEST(HeaderInference, CudaRuntime) {
    HeaderInference hi;
    auto headers = hi.infer({"cudaMalloc", "cudaMemcpy"});
    EXPECT_NE(std::find(headers.begin(), headers.end(), "<cuda_runtime.h>"), headers.end());
}

TEST(HeaderInference, Pthread) {
    HeaderInference hi;
    auto headers = hi.infer({"pthread_create", "pthread_join"});
    EXPECT_NE(std::find(headers.begin(), headers.end(), "<pthread.h>"), headers.end());
}

TEST(HeaderInference, NoDuplicates) {
    HeaderInference hi;
    auto headers = hi.infer({"malloc", "free", "realloc", "calloc"});
    // All map to <stdlib.h>, should appear once
    int count = static_cast<int>(std::count(headers.begin(), headers.end(), "<stdlib.h>"));
    EXPECT_EQ(count, 1);
}

TEST(HeaderInference, UnknownSymbol) {
    HeaderInference hi;
    auto headers = hi.infer({"totally_unknown_func"});
    EXPECT_TRUE(headers.empty());
}

// ─── CMakeEmitter ─────────────────────────────────────────────────────────────

static ClusterResult makeSimpleResult() {
    ClusterResult r;
    r.projectName = "my_proj";
    r.modularity  = 0.5;

    Module m1;
    m1.name          = "net";
    m1.functions     = {"net_connect", "net_send"};
    m1.headers       = {"<sys/socket.h>", "<pthread.h>"};
    m1.cohesion      = 0.8;
    m1.isEntryModule = false;

    Module m2;
    m2.name          = "entry";
    m2.functions     = {"main"};
    m2.isEntryModule = true;
    m2.dependencies  = {"net"};

    r.modules = {m2, m1};
    return r;
}

TEST(CMakeEmitter, EmitContainsProjectName) {
    CMakeEmitter emitter;
    CMakeConfig cfg; cfg.projectName = "test_project";
    ClusterResult r = makeSimpleResult();
    r.projectName = "test_project";
    auto s = emitter.emit(r, cfg);
    EXPECT_NE(s.find("test_project"), std::string::npos);
}

TEST(CMakeEmitter, EmitContainsCxxStandard) {
    CMakeEmitter emitter;
    CMakeConfig cfg; cfg.cxxStandard = "17";
    auto s = emitter.emit(makeSimpleResult(), cfg);
    EXPECT_NE(s.find("CMAKE_CXX_STANDARD"), std::string::npos);
    EXPECT_NE(s.find("17"), std::string::npos);
}

TEST(CMakeEmitter, EmitContainsModuleName) {
    CMakeEmitter emitter;
    auto s = emitter.emit(makeSimpleResult());
    EXPECT_NE(s.find("add_library(net"), std::string::npos);
}

TEST(CMakeEmitter, EmitContainsAddExecutable) {
    CMakeEmitter emitter;
    CMakeConfig cfg; cfg.projectName = "my_proj";
    auto s = emitter.emit(makeSimpleResult(), cfg);
    EXPECT_NE(s.find("add_executable"), std::string::npos);
}

TEST(CMakeEmitter, EmitContainsFindPackageForPthread) {
    CMakeEmitter emitter;
    auto s = emitter.emit(makeSimpleResult());
    EXPECT_NE(s.find("find_package(Threads"), std::string::npos);
}

TEST(CMakeEmitter, EmitWithTesting) {
    CMakeEmitter emitter;
    CMakeConfig cfg;
    cfg.addTesting = true;
    auto s = emitter.emit(makeSimpleResult(), cfg);
    EXPECT_NE(s.find("enable_testing"), std::string::npos);
    EXPECT_NE(s.find("GTest"), std::string::npos);
}

TEST(CMakeEmitter, EmitWithInstall) {
    CMakeEmitter emitter;
    CMakeConfig cfg; cfg.addInstall = true;
    auto s = emitter.emit(makeSimpleResult(), cfg);
    EXPECT_NE(s.find("install"), std::string::npos);
}

TEST(CMakeEmitter, EmitEmptyResult) {
    CMakeEmitter emitter;
    ClusterResult r; r.projectName = "empty";
    auto s = emitter.emit(r);
    EXPECT_NE(s.find("project(empty"), std::string::npos);
}

// ─── ModuleClusterer (full pipeline) ─────────────────────────────────────────

TEST(ModuleClusterer, EmptyGraph) {
    ModuleClusterer c;
    CallGraph g;
    auto result = c.cluster(g, "proj");
    EXPECT_TRUE(result.modules.empty());
    EXPECT_EQ(result.projectName, "proj");
}

TEST(ModuleClusterer, SingleFunction) {
    ModuleClusterer c;
    CallGraph g;
    FunctionMeta f; f.name = "main"; f.isEntryPoint = true;
    g.functions.push_back(f);
    auto result = c.cluster(g, "tiny");
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0].functions.size(), 1u);
}

TEST(ModuleClusterer, EntryPointModuleMarked) {
    ModuleClusterer c;
    CallGraph g;
    FunctionMeta f; f.name = "main"; f.isEntryPoint = true;
    g.functions.push_back(f);
    auto result = c.cluster(g);
    EXPECT_TRUE(result.entryModule() != nullptr);
    EXPECT_TRUE(result.entryModule()->isEntryModule);
}

TEST(ModuleClusterer, TwoModulesFromStrongComponents) {
    ModuleClusterer c(LouvainConfig{1.0, 200, false});
    CallGraph g;
    for (const char* n : {"a","b","c","x","y","z"}) {
        FunctionMeta f; f.name = n; g.functions.push_back(f);
    }
    g.edges = {
        {"a","b",100},{"b","a",100},{"b","c",100},{"c","b",100},{"a","c",100},{"c","a",100},
        {"x","y",100},{"y","x",100},{"y","z",100},{"z","y",100},{"x","z",100},{"z","x",100},
        {"a","x",1},  // weak inter-group link
    };
    auto result = c.cluster(g);
    EXPECT_GE(result.modules.size(), 1u);
    // All 6 functions should be assigned
    int total = 0;
    for (const auto& m : result.modules) total += static_cast<int>(m.functions.size());
    EXPECT_EQ(total, 6);
}

TEST(ModuleClusterer, StringLocalityGroups) {
    ModuleClusterer c;
    CallGraph g;
    FunctionMeta f1; f1.name = "fn_a"; f1.referencedStrings = {"str_pool_section_42"};
    FunctionMeta f2; f2.name = "fn_b"; f2.referencedStrings = {"str_pool_section_42"};
    FunctionMeta f3; f3.name = "fn_c"; f3.referencedStrings = {"str_pool_section_99"};
    g.functions = {f1, f2, f3};
    auto result = c.cluster(g);
    // fn_a and fn_b share a string → should end up together
    const auto* ma = result.findModule("fn_a");
    const auto* mb = result.findModule("fn_b");
    ASSERT_NE(ma, nullptr);
    ASSERT_NE(mb, nullptr);
    EXPECT_EQ(ma->name, mb->name);
}

TEST(ModuleClusterer, DebugSymbolGroups) {
    ModuleClusterer c;
    CallGraph g;
    FunctionMeta f1; f1.name = "fn1"; f1.sourceFile = "net.cpp";
    FunctionMeta f2; f2.name = "fn2"; f2.sourceFile = "net.cpp";
    FunctionMeta f3; f3.name = "fn3"; f3.sourceFile = "crypto.cpp";
    g.functions = {f1, f2, f3};
    auto result = c.cluster(g);
    const auto* m1 = result.findModule("fn1");
    const auto* m2 = result.findModule("fn2");
    ASSERT_NE(m1, nullptr);
    ASSERT_NE(m2, nullptr);
    EXPECT_EQ(m1->name, m2->name);
}

TEST(ModuleClusterer, CohesionIsComputed) {
    ModuleClusterer c;
    CallGraph g;
    FunctionMeta f1; f1.name = "net_a"; f1.namePrefix = "net_";
    FunctionMeta f2; f2.name = "net_b"; f2.namePrefix = "net_";
    g.functions = {f1, f2};
    g.edges = {{"net_a","net_b",5}};
    auto result = c.cluster(g);
    bool anyNonZero = false;
    for (const auto& m : result.modules)
        if (m.cohesion > 0.0) anyNonZero = true;
    EXPECT_TRUE(anyNonZero);
}

TEST(ModuleClusterer, FindModuleByFunction) {
    ModuleClusterer c;
    CallGraph g;
    FunctionMeta f; f.name = "target_fn";
    g.functions.push_back(f);
    auto result = c.cluster(g);
    const auto* m = result.findModule("target_fn");
    ASSERT_NE(m, nullptr);
    EXPECT_NE(std::find(m->functions.begin(), m->functions.end(), "target_fn"),
              m->functions.end());
}

TEST(ModuleClusterer, HeadersInferredFromLibCalls) {
    ModuleClusterer c;
    CallGraph g;
    FunctionMeta f;
    f.name = "do_alloc";
    f.calledLibSyms = {"malloc", "free"};
    g.functions.push_back(f);
    auto result = c.cluster(g);
    ASSERT_FALSE(result.modules.empty());
    const auto& m = result.modules[0];
    EXPECT_FALSE(m.headers.empty());
    bool hasStdlib = std::any_of(m.headers.begin(), m.headers.end(),
        [](const auto& h){ return h == "<stdlib.h>"; });
    EXPECT_TRUE(hasStdlib);
}

TEST(ModuleClusterer, CMakeEmitPipeline) {
    ModuleClusterer c;
    CallGraph g;
    FunctionMeta f1; f1.name = "main"; f1.isEntryPoint = true;
    FunctionMeta f2; f2.name = "net_send"; f2.namePrefix = "net_";
    f2.calledLibSyms = {"socket", "send"};
    g.functions = {f1, f2};
    g.edges = {{"main","net_send",1}};

    auto result = c.cluster(g, "myapp");

    CMakeEmitter emitter;
    CMakeConfig cfg; cfg.projectName = "myapp";
    auto cmake = emitter.emit(result, cfg);
    EXPECT_NE(cmake.find("project(myapp"), std::string::npos);
    EXPECT_NE(cmake.find("add_executable"), std::string::npos);
    EXPECT_FALSE(cmake.empty());
}
