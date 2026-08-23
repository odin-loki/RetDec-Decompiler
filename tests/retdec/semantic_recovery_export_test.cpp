/**
 * @file tests/retdec/semantic_recovery_export_test.cpp
 * @brief Unit tests for RETDEC_EMIT_BUILDABLE sidecar writer.
 */

#include "retdec/retdec/semantic_recovery_export.h"

#include "retdec/concurrency_detect/concurrency_detect.h"
#include "retdec/container_detect/container_detect.h"
#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

using retdec::analysis::maybeWriteBuildableSidecars;

namespace {

void setEmitBuildableEnv(const char* value)
{
#ifdef _WIN32
	if (value == nullptr)
	{
		_putenv_s("RETDEC_EMIT_BUILDABLE", "");
	}
	else
	{
		_putenv_s("RETDEC_EMIT_BUILDABLE", value);
	}
#else
	if (value == nullptr)
	{
		unsetenv("RETDEC_EMIT_BUILDABLE");
	}
	else
	{
		setenv("RETDEC_EMIT_BUILDABLE", value, 1);
	}
#endif
}

class EmitBuildableEnvGuard {
public:
	EmitBuildableEnvGuard()
	{
		const char* prev = std::getenv("RETDEC_EMIT_BUILDABLE");
		if (prev != nullptr)
		{
			saved_ = prev;
			had_ = true;
		}
	}

	~EmitBuildableEnvGuard()
	{
		if (had_)
		{
			setEmitBuildableEnv(saved_.c_str());
		}
		else
		{
			setEmitBuildableEnv(nullptr);
		}
	}

private:
	bool had_ = false;
	std::string saved_;
};

std::string readAll(const fs::path& path)
{
	std::ifstream in(path);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& path, const std::string& text)
{
	std::ofstream out(path, std::ios::trunc);
	out << text;
}

} // namespace

TEST(BuildableSidecars, EmptyPathIsNoop)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");
	maybeWriteBuildableSidecars("", "int foo(void) { return 0; }\n");
}

TEST(BuildableSidecars, DisabledWhenUnsetOrZero)
{
	EmitBuildableEnvGuard guard;
	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_disabled.c";
	writeAll(outC, "int foo(void) { return 0; }\n");

	setEmitBuildableEnv(nullptr);
	maybeWriteBuildableSidecars(outC.string(), "int foo(void) { return 0; }\n");
	EXPECT_FALSE(fs::exists(dir / "retdec_buildable_disabled.h"));
	EXPECT_FALSE(fs::exists(dir / "retdec_buildable_disabled_stubs.c"));
	EXPECT_FALSE(fs::exists(dir / "retdec_buildable_disabled.buildable.c"));

	setEmitBuildableEnv("0");
	maybeWriteBuildableSidecars(outC.string(), "int foo(void) { return 0; }\n");
	EXPECT_FALSE(fs::exists(dir / "retdec_buildable_disabled.h"));
	EXPECT_FALSE(fs::exists(dir / "retdec_buildable_disabled_stubs.c"));
	EXPECT_FALSE(fs::exists(dir / "retdec_buildable_disabled.buildable.c"));

	fs::remove(outC);
}

TEST(BuildableSidecars, WritesHeaderStubsAndBuildable)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_emit.c";
	const std::string original =
		"typedef int my_i;\n"
		"struct S {\n"
		"    int x;\n"
		"};\n"
		"int foo(void) {\n"
		"    if (bar()) {\n"
		"        return sizeof(int);\n"
		"    }\n"
		"    return foo();\n"
		"}\n";
	writeAll(outC, original);

	maybeWriteBuildableSidecars(outC.string(), original);

	const fs::path header = dir / "retdec_buildable_emit.h";
	const fs::path stubs = dir / "retdec_buildable_emit_stubs.c";
	const fs::path buildable = dir / "retdec_buildable_emit.buildable.c";

	ASSERT_TRUE(fs::exists(header));
	ASSERT_TRUE(fs::exists(stubs));
	ASSERT_TRUE(fs::exists(buildable));

	const std::string h = readAll(header);
	EXPECT_NE(h.find("#include <stdint.h>"), std::string::npos);
	EXPECT_NE(h.find("#include <stddef.h>"), std::string::npos);
	EXPECT_NE(h.find("#include <stdbool.h>"), std::string::npos);
	EXPECT_NE(h.find("typedef int my_i;"), std::string::npos);
	EXPECT_NE(h.find("struct S {"), std::string::npos);
	EXPECT_NE(h.find("int bar(void);"), std::string::npos);
	EXPECT_EQ(h.find("int foo(void);"), std::string::npos);
	EXPECT_EQ(h.find("int if(void);"), std::string::npos);
	EXPECT_EQ(h.find("int sizeof(void);"), std::string::npos);

	const std::string s = readAll(stubs);
	EXPECT_NE(s.find("#include \"retdec_buildable_emit.h\""), std::string::npos);
	EXPECT_NE(s.find("void __retdec_stub(void) {}"), std::string::npos);
	EXPECT_NE(s.find("int main(void) { return 0; }"), std::string::npos);

	const std::string b = readAll(buildable);
	EXPECT_EQ(b.find("#include \"retdec_buildable_emit.h\""), 0u);
	EXPECT_NE(b.find("#define strncpy("), std::string::npos);
	EXPECT_NE(b.find("int foo(void)"), std::string::npos);
	EXPECT_NE(b.find("int bar(void) { return 0; }"), std::string::npos);
	EXPECT_NE(b.find("int main(void) { return 0; }"), std::string::npos);
	EXPECT_NE(b.find("RETDEC_BUILDABLE_STUBS"), std::string::npos);

	EXPECT_EQ(readAll(outC), original);

	fs::remove(outC);
	fs::remove(header);
	fs::remove(stubs);
	fs::remove(buildable);
}

TEST(BuildableSidecars, DoesNotAddMainWhenPresent)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_main.c";
	const std::string original = "int main(void) { return 1; }\n";
	writeAll(outC, original);

	maybeWriteBuildableSidecars(outC.string(), original);

	const std::string s = readAll(dir / "retdec_buildable_main_stubs.c");
	EXPECT_NE(s.find("void __retdec_stub(void) {}"), std::string::npos);
	EXPECT_EQ(s.find("int main("), std::string::npos);
	const std::string b = readAll(dir / "retdec_buildable_main.buildable.c");
	EXPECT_EQ(b.find("int main(void) { return 0; }"), std::string::npos);
	EXPECT_EQ(readAll(outC), original);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_main.h");
	fs::remove(dir / "retdec_buildable_main_stubs.c");
	fs::remove(dir / "retdec_buildable_main.buildable.c");
}

TEST(BuildableSidecars, InjectsUndeclaredResultAndVTemps)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_temps.c";
	const std::string original =
		"uint64_t function_1020(void) {\n"
		"    return result;\n"
		"}\n"
		"uint64_t entry_point(uint64_t a1) {\n"
		"    return v1 + v2;\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);

	const std::string b = readAll(dir / "retdec_buildable_temps.buildable.c");
	EXPECT_NE(b.find("int64_t result = 0;"), std::string::npos);
	EXPECT_NE(b.find("int64_t v1 = 0;"), std::string::npos);
	EXPECT_NE(b.find("int64_t v2 = 0;"), std::string::npos);
	EXPECT_EQ(readAll(outC), original);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_temps.h");
	fs::remove(dir / "retdec_buildable_temps_stubs.c");
	fs::remove(dir / "retdec_buildable_temps.buildable.c");
}

TEST(BuildableSidecars, DoesNotInjectParamNames)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_param.c";
	const std::string original =
		"uint64_t function_1189(uint64_t result) {\n"
		"    return result;\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_param.buildable.c");
	EXPECT_EQ(b.find("int64_t result = 0;"), std::string::npos);
	EXPECT_NE(b.find("#define strcmp("), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_param.h");
	fs::remove(dir / "retdec_buildable_param_stubs.c");
	fs::remove(dir / "retdec_buildable_param.buildable.c");
}

TEST(BuildableSidecars, DoesNotStubLibcPutcharAsIntVoid)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_putchar.c";
	const std::string original =
		"void ring_put(int c) {\n"
		"    putchar(c);\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);

	const std::string h = readAll(dir / "retdec_buildable_putchar.h");
	const std::string b = readAll(dir / "retdec_buildable_putchar.buildable.c");
	EXPECT_EQ(h.find("int putchar(void);"), std::string::npos);
	EXPECT_NE(b.find("#include <stdio.h>"), std::string::npos);
	EXPECT_EQ(b.find("int putchar(void)"), std::string::npos);
	EXPECT_EQ(readAll(outC), original);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_putchar.h");
	fs::remove(dir / "retdec_buildable_putchar_stubs.c");
	fs::remove(dir / "retdec_buildable_putchar.buildable.c");
}

TEST(BuildableSidecars, ClonesFileScopePrototypeNotReturnCall)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_proto.c";
	const std::string original =
		"uint64_t __cxa_finalize(uint64_t a1);\n"
		"uint64_t foo(uint64_t a1) {\n"
		"    return __cxa_finalize(a1);\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_proto.buildable.c");
	EXPECT_NE(b.find("uint64_t __cxa_finalize(uint64_t a1) { return 0; }"), std::string::npos);
	EXPECT_EQ(b.find("WEAK return __cxa_finalize"), std::string::npos);
	EXPECT_EQ(b.find("WEAK return "), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_proto.h");
	fs::remove(dir / "retdec_buildable_proto_stubs.c");
	fs::remove(dir / "retdec_buildable_proto.buildable.c");
}

TEST(BuildableSidecars, RewritesOrphanBreakKeepsLoopBreak)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_break.c";
	const std::string original =
		"uint64_t orphan_if(void) {\n"
		"    if (1) {\n"
		"        break;\n"
		"    }\n"
		"    return 0;\n"
		"}\n"
		"uint64_t in_loop(void) {\n"
		"    for (;;) {\n"
		"        break;\n"
		"    }\n"
		"    return 0;\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_break.buildable.c");
	EXPECT_NE(b.find("/* orphan break */"), std::string::npos);
	EXPECT_NE(b.find("for (;;) {\n        break;\n    }"), std::string::npos);
	EXPECT_EQ(readAll(outC), original);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_break.h");
	fs::remove(dir / "retdec_buildable_break_stubs.c");
	fs::remove(dir / "retdec_buildable_break.buildable.c");
}

TEST(BuildableSidecars, InjectsHighVTempsAndStr)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_v33.c";
	const std::string original =
		"uint64_t f(void) {\n"
		"    return v33 + str;\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_v33.buildable.c");
	EXPECT_NE(b.find("int64_t v33 = 0;"), std::string::npos);
	EXPECT_NE(b.find("int64_t str = 0;"), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_v33.h");
	fs::remove(dir / "retdec_buildable_v33_stubs.c");
	fs::remove(dir / "retdec_buildable_v33.buildable.c");
}

TEST(BuildableSidecars, InjectsResultWhenOnlyForInitDeclaresIt)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_forresult.c";
	const std::string original =
		"uint64_t f(unsigned char * a1) {\n"
		"    for (uint32_t result = 0; a1[result]; result++) {}\n"
		"    return result;\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_forresult.buildable.c");
	EXPECT_NE(b.find("int64_t result = 0;"), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_forresult.h");
	fs::remove(dir / "retdec_buildable_forresult_stubs.c");
	fs::remove(dir / "retdec_buildable_forresult.buildable.c");
}

TEST(BuildableSidecars, DoesNotRedefineInt128Temp)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_i128.c";
	const std::string original =
		"uint64_t f(void) {\n"
		"    int128_t v6 = 0;\n"
		"    return (uint64_t)v6;\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_i128.buildable.c");
	EXPECT_EQ(b.find("int64_t v6 = 0;"), std::string::npos);
	EXPECT_NE(b.find("int128_t v6 = 0;"), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_i128.h");
	fs::remove(dir / "retdec_buildable_i128_stubs.c");
	fs::remove(dir / "retdec_buildable_i128.buildable.c");
}

TEST(BuildableSidecars, WrapsPutsArityAndHidesFortifyChk)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_arity.c";
	const std::string original =
		"uint64_t __printf_chk(uint64_t a1);\n"
		"uint64_t f(void) {\n"
		"    puts(\"x\", 1);\n"
		"    return pthread_create(0);\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_arity.buildable.c");
	EXPECT_NE(b.find("#define puts("), std::string::npos);
	EXPECT_NE(b.find("#define pthread_create("), std::string::npos);
	EXPECT_NE(b.find("retdec_pthread_any"), std::string::npos);
	EXPECT_EQ(b.find("#define __printf_chk retdec_fortify"), std::string::npos);
	EXPECT_NE(b.find("uint64_t __printf_chk("), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_arity.h");
	fs::remove(dir / "retdec_buildable_arity_stubs.c");
	fs::remove(dir / "retdec_buildable_arity.buildable.c");
}

TEST(BuildableSidecars, InjectsTempUsedAsDerefStar)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_deref.c";
	const std::string original =
		"uint64_t f(void) {\n"
		"    *v11 = 1;\n"
		"    v11 = 0;\n"
		"    return v11;\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_deref.buildable.c");
	EXPECT_NE(b.find("int64_t * v11 = 0;"), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_deref.h");
	fs::remove(dir / "retdec_buildable_deref_stubs.c");
	fs::remove(dir / "retdec_buildable_deref.buildable.c");
}

TEST(BuildableSidecars, EmitsMissingGotoLabels)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_goto.c";
	const std::string original =
		"uint64_t f(void) {\n"
		"    goto lab_0x12ec;\n"
		"    return 0;\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_goto.buildable.c");
	EXPECT_NE(b.find("lab_0x12ec: ;"), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_goto.h");
	fs::remove(dir / "retdec_buildable_goto_stubs.c");
	fs::remove(dir / "retdec_buildable_goto.buildable.c");
}

TEST(BuildableSidecars, StripsPthreadIncludeAndMacrosCalls)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_pthread.c";
	const std::string original =
		"#include <pthread.h>\n"
		"uint64_t f(uint64_t * thread) {\n"
		"    return pthread_create(thread);\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_pthread.buildable.c");
	EXPECT_EQ(b.find("#include <pthread.h>"), std::string::npos);
	EXPECT_NE(b.find("#define pthread_create("), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_pthread.h");
	fs::remove(dir / "retdec_buildable_pthread_stubs.c");
	fs::remove(dir / "retdec_buildable_pthread.buildable.c");
}

TEST(BuildableSidecars, DefinesAsmIntrinsicMacro)
{
	EmitBuildableEnvGuard guard;
	setEmitBuildableEnv("1");

	const fs::path dir = fs::temp_directory_path();
	const fs::path outC = dir / "retdec_buildable_asm.c";
	const std::string original =
		"void f(void) {\n"
		"    __asm_movups(0, 1);\n"
		"}\n";
	writeAll(outC, original);
	maybeWriteBuildableSidecars(outC.string(), original);
	const std::string b = readAll(dir / "retdec_buildable_asm.buildable.c");
	EXPECT_NE(b.find("#define __asm_movups("), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_asm.h");
	fs::remove(dir / "retdec_buildable_asm_stubs.c");
	fs::remove(dir / "retdec_buildable_asm.buildable.c");
}

TEST(SemanticExport, OpenAddressingDetailIsSymbolNameEvidence)
{
	retdec::container_detect::ContainerDetector::DetectionMap containers;
	retdec::container_detect::ContainerResult oa;
	oa.emittedType = "open_addressing_hash_table";
	oa.confidence = 0.85f;
	containers.emplace("lookup", oa);

	retdec::container_detect::ContainerResult vec;
	vec.emittedType = "std::vector<int32_t>";
	vec.confidence = 0.80f;
	containers.emplace("push", vec);

	const auto map = retdec::analysis::buildSemanticDetectionMap(
		containers,
		{},
		{},
		retdec::sort_detect::SortDetector::DetectionMap{},
		retdec::concurrency_detect::ConcurrencyModel{},
		"c");

	ASSERT_EQ(map.count("lookup"), 1u);
	ASSERT_FALSE(map.at("lookup").empty());
	EXPECT_NE(map.at("lookup").front().detail.find("evidence:symbol_name"), std::string::npos);

	ASSERT_EQ(map.count("push"), 1u);
	ASSERT_FALSE(map.at("push").empty());
	EXPECT_EQ(map.at("push").front().detail.find("evidence:symbol_name"), std::string::npos);
}

TEST(SemanticExport, IntrosortNameVariantIsSymbolNameEvidence)
{
	retdec::sort_detect::SortDetector::DetectionMap sorts;
	retdec::sort_detect::SortResult named;
	named.algorithm = retdec::sort_detect::SortAlgorithm::Introsort;
	named.compilerVariant = retdec::sort_detect::CompilerVariant::GCC;
	named.confidence = 0.80f;
	sorts.emplace("std_sort", named);

	retdec::sort_detect::SortResult structural;
	structural.algorithm = retdec::sort_detect::SortAlgorithm::Introsort;
	structural.compilerVariant = retdec::sort_detect::CompilerVariant::Unknown;
	structural.confidence = 0.80f;
	sorts.emplace("hand_roll", structural);

	const auto map = retdec::analysis::buildSemanticDetectionMap(
		{}, {}, {}, sorts, retdec::concurrency_detect::ConcurrencyModel{}, "c");

	ASSERT_EQ(map.count("std_sort"), 1u);
	ASSERT_FALSE(map.at("std_sort").empty());
	EXPECT_NE(map.at("std_sort").front().detail.find("evidence:symbol_name"), std::string::npos);

	ASSERT_EQ(map.count("hand_roll"), 1u);
	ASSERT_FALSE(map.at("hand_roll").empty());
	EXPECT_EQ(map.at("hand_roll").front().detail.find("evidence:symbol_name"), std::string::npos);
}

TEST(SemanticExport, HeapsortNameVariantIsSymbolNameEvidence)
{
	retdec::sort_detect::SortDetector::DetectionMap sorts;
	retdec::sort_detect::SortResult named;
	named.algorithm = retdec::sort_detect::SortAlgorithm::Heapsort;
	named.compilerVariant = retdec::sort_detect::CompilerVariant::GCC;
	named.confidence = 0.80f;
	sorts.emplace("std_heap", named);

	retdec::sort_detect::SortResult structural;
	structural.algorithm = retdec::sort_detect::SortAlgorithm::Heapsort;
	structural.compilerVariant = retdec::sort_detect::CompilerVariant::Unknown;
	structural.confidence = 0.80f;
	sorts.emplace("hand_roll", structural);

	const auto map = retdec::analysis::buildSemanticDetectionMap(
		{}, {}, {}, sorts, retdec::concurrency_detect::ConcurrencyModel{}, "c");

	ASSERT_EQ(map.count("std_heap"), 1u);
	ASSERT_FALSE(map.at("std_heap").empty());
	EXPECT_NE(map.at("std_heap").front().detail.find("evidence:symbol_name"), std::string::npos);

	ASSERT_EQ(map.count("hand_roll"), 1u);
	ASSERT_FALSE(map.at("hand_roll").empty());
	EXPECT_EQ(map.at("hand_roll").front().detail.find("evidence:symbol_name"), std::string::npos);
}

TEST(SemanticExport, MergesortNameVariantIsSymbolNameEvidence)
{
	retdec::sort_detect::SortDetector::DetectionMap sorts;
	retdec::sort_detect::SortResult named;
	named.algorithm = retdec::sort_detect::SortAlgorithm::Mergesort;
	named.compilerVariant = retdec::sort_detect::CompilerVariant::GCC;
	named.confidence = 0.80f;
	sorts.emplace("std_stable", named);

	retdec::sort_detect::SortResult structural;
	structural.algorithm = retdec::sort_detect::SortAlgorithm::Mergesort;
	structural.compilerVariant = retdec::sort_detect::CompilerVariant::Unknown;
	structural.confidence = 0.80f;
	sorts.emplace("hand_roll", structural);

	const auto map = retdec::analysis::buildSemanticDetectionMap(
		{}, {}, {}, sorts, retdec::concurrency_detect::ConcurrencyModel{}, "c");

	ASSERT_EQ(map.count("std_stable"), 1u);
	ASSERT_FALSE(map.at("std_stable").empty());
	EXPECT_NE(map.at("std_stable").front().detail.find("evidence:symbol_name"), std::string::npos);

	ASSERT_EQ(map.count("hand_roll"), 1u);
	ASSERT_FALSE(map.at("hand_roll").empty());
	EXPECT_EQ(map.at("hand_roll").front().detail.find("evidence:symbol_name"), std::string::npos);
}

TEST(SemanticExport, HmacPadsExportAsCryptoKind)
{
	retdec::ssa::SSAFunction fn("hmac_fn");
	auto* blk = fn.addBlock();
	ASSERT_NE(blk, nullptr);

	auto addImmXor = [&](uint64_t imm) {
		retdec::ssa::IrInstr* instr = fn.addInstr(blk->id, retdec::ssa::IrInstr::Op::Xor, 0);
		retdec::ssa::IrValue* v = fn.allocValue(retdec::ssa::ValueKind::Immediate);
		v->imm = imm;
		retdec::ssa::Use u;
		u.valueId = v->id;
		instr->uses.push_back(u);
	};
	addImmXor(0x36363636ULL);
	addImmXor(0x5c5c5c5cULL);
	fn.addInstr(blk->id, retdec::ssa::IrInstr::Op::Add, 0);
	fn.addInstr(blk->id, retdec::ssa::IrInstr::Op::Add, 0);

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendCryptoDetections(map, fn);

	ASSERT_EQ(map.count("hmac_fn"), 1u);
	bool foundHmac = false;
	for (const auto& d: map.at("hmac_fn"))
	{
		if (d.kind == "crypto" && d.label == "HMAC")
		{
			foundHmac = true;
			EXPECT_GE(d.confidence, 0.50f);
			EXPECT_EQ(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(foundHmac);
}

TEST(SemanticExport, NameOnlyAesNiDoesNotExportBelowThreshold)
{
	retdec::ssa::SSAFunction fn("aesni_only");
	auto* blk = fn.addBlock();
	ASSERT_NE(blk, nullptr);
	retdec::ssa::IrInstr* call = fn.addInstr(blk->id, retdec::ssa::IrInstr::Op::Call, 0);
	call->calleeName = "_mm_aesenc_si128";
	fn.addInstr(blk->id, retdec::ssa::IrInstr::Op::Xor, 0);
	fn.addInstr(blk->id, retdec::ssa::IrInstr::Op::Xor, 0);
	fn.addInstr(blk->id, retdec::ssa::IrInstr::Op::Xor, 0);

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendCryptoDetections(map, fn);

	if (map.count("aesni_only") == 0)
	{
		return;
	}
	for (const auto& d: map.at("aesni_only"))
	{
		EXPECT_FALSE(d.kind == "crypto" && d.label == "AES");
	}
}

TEST(SemanticExport, ProtobufSymbolsExportAsSerialNameEvidence)
{
	retdec::ssa::SSAFunction fn("ser_fn");
	ASSERT_NE(fn.addBlock(), nullptr);
	ASSERT_NE(fn.addBlock(), nullptr);
	ASSERT_NE(fn.addBlock(), nullptr);

	const std::unordered_set<std::string> sym{
		"proto::MyMessage::SerializeToString", "proto::MyMessage::ParseFromString"};
	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendSerialDetections(map, fn, sym);

	ASSERT_EQ(map.count("ser_fn"), 1u);
	ASSERT_FALSE(map.at("ser_fn").empty());
	const auto& d = map.at("ser_fn").front();
	EXPECT_EQ(d.kind, "serial");
	EXPECT_EQ(d.label, "Protobuf");
	EXPECT_GE(d.confidence, 0.40f);
	EXPECT_NE(d.detail.find("evidence:symbol_name"), std::string::npos);
}

TEST(SemanticExport, SerialPreflightSkipsTinyFunctions)
{
	retdec::ssa::SSAFunction fn("tiny");
	ASSERT_NE(fn.addBlock(), nullptr);
	const std::unordered_set<std::string> sym{"proto::MyMessage::SerializeToString"};
	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendSerialDetections(map, fn, sym);
	EXPECT_EQ(map.count("tiny"), 0u);
}

TEST(SemanticExport, RaiiAcquireReleaseExportsAsPatternNameEvidence)
{
	retdec::ssa::SSAFunction fn("raii_fn");
	auto* entry = fn.addBlock();
	ASSERT_NE(entry, nullptr);
	ASSERT_NE(fn.addBlock(), nullptr);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Store, 0);
	retdec::ssa::IrInstr* acq = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	acq->calleeName = "fopen";
	retdec::ssa::IrInstr* rel = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	rel->calleeName = "fclose";

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendPatternDetections(map, fn);

	ASSERT_EQ(map.count("raii_fn"), 1u);
	bool foundRaii = false;
	for (const auto& d: map.at("raii_fn"))
	{
		if (d.kind == "pattern" && d.label == "RAII")
		{
			foundRaii = true;
			EXPECT_GE(d.confidence, 0.45f);
			EXPECT_NE(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(foundRaii);
}

TEST(SemanticExport, CommandExecuteExportsAsPatternNameEvidence)
{
	retdec::ssa::SSAFunction fn("runQueue");
	auto* entry = fn.addBlock();
	ASSERT_NE(entry, nullptr);
	auto* loop = fn.addBlock();
	ASSERT_NE(loop, nullptr);
	loop->succs.push_back(entry->id);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	retdec::ssa::IrInstr* ex = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	ex->calleeName = "execute";
	retdec::ssa::IrInstr* pb = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	pb->calleeName = "push_back";

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendPatternDetections(map, fn);

	ASSERT_EQ(map.count("runQueue"), 1u);
	bool found = false;
	for (const auto& d: map.at("runQueue"))
	{
		if (d.kind == "pattern" && d.label == "Command")
		{
			found = true;
			EXPECT_GE(d.confidence, 0.45f);
			EXPECT_NE(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}

TEST(SemanticExport, ObserverSubscribeExportsAsPatternNameEvidence)
{
	retdec::ssa::SSAFunction fn("obs");
	auto* entry = fn.addBlock();
	ASSERT_NE(entry, nullptr);
	auto* loop = fn.addBlock();
	ASSERT_NE(loop, nullptr);
	loop->succs.push_back(entry->id);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	retdec::ssa::IrInstr* pb = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	pb->calleeName = "push_back";
	retdec::ssa::IrInstr* em = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	em->calleeName = "emit";
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Store, 0);

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendPatternDetections(map, fn);

	ASSERT_EQ(map.count("obs"), 1u);
	bool found = false;
	for (const auto& d: map.at("obs"))
	{
		if (d.kind == "pattern" && d.label == "Observer")
		{
			found = true;
			EXPECT_GE(d.confidence, 0.45f);
			EXPECT_NE(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}

TEST(SemanticExport, SingletonLockExportsAsPatternNameEvidence)
{
	retdec::ssa::SSAFunction fn("getInstance");
	auto* entry = fn.addBlock();
	ASSERT_NE(entry, nullptr);
	ASSERT_NE(fn.addBlock(), nullptr);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	retdec::ssa::IrInstr* cmp = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Compare, 0);
	retdec::ssa::IrValue* z = fn.allocValue(retdec::ssa::ValueKind::Immediate);
	z->imm = 0;
	retdec::ssa::Use u;
	u.valueId = z->id;
	cmp->uses.push_back(u);
	retdec::ssa::IrInstr* al = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	al->calleeName = "malloc";
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Store, 0);
	retdec::ssa::IrInstr* lk = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	lk->calleeName = "EnterCriticalSection";
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Ret, 0);

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendPatternDetections(map, fn);

	ASSERT_EQ(map.count("getInstance"), 1u);
	bool found = false;
	for (const auto& d: map.at("getInstance"))
	{
		if (d.kind == "pattern" && d.label == "Singleton")
		{
			found = true;
			EXPECT_GE(d.confidence, 0.45f);
			EXPECT_NE(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}

TEST(SemanticExport, CommandIndirectCallDoesNotTagSymbolName)
{
	retdec::ssa::SSAFunction fn("runQueue");
	auto* entry = fn.addBlock();
	ASSERT_NE(entry, nullptr);
	auto* loop = fn.addBlock();
	ASSERT_NE(loop, nullptr);
	loop->succs.push_back(entry->id);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Store, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendPatternDetections(map, fn);

	ASSERT_EQ(map.count("runQueue"), 1u);
	bool found = false;
	for (const auto& d: map.at("runQueue"))
	{
		if (d.kind == "pattern" && d.label == "Command")
		{
			found = true;
			EXPECT_GE(d.confidence, 0.45f);
			EXPECT_EQ(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}

TEST(SemanticExport, FactoryAllocExportsAsPatternNameEvidence)
{
	retdec::ssa::SSAFunction fn("createProduct");
	auto* entry = fn.addBlock();
	ASSERT_NE(entry, nullptr);
	ASSERT_NE(fn.addBlock(), nullptr);
	auto addImmCompare = [&](uint64_t imm) {
		retdec::ssa::IrInstr* cmp = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Compare, 0);
		retdec::ssa::IrValue* v = fn.allocValue(retdec::ssa::ValueKind::Immediate);
		v->imm = imm;
		retdec::ssa::Use u;
		u.valueId = v->id;
		cmp->uses.push_back(u);
	};
	addImmCompare(0);
	addImmCompare(1);
	retdec::ssa::IrInstr* a0 = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	a0->calleeName = "_Znwm";
	retdec::ssa::IrInstr* a1 = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	a1->calleeName = "_Znwm";
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Ret, 0);

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendPatternDetections(map, fn);

	ASSERT_EQ(map.count("createProduct"), 1u);
	bool found = false;
	for (const auto& d: map.at("createProduct"))
	{
		if (d.kind == "pattern" && d.label == "Factory")
		{
			found = true;
			EXPECT_GE(d.confidence, 0.45f);
			EXPECT_NE(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}

TEST(SemanticExport, StrategyDoAlgorithmExportsAsPatternNameEvidence)
{
	retdec::ssa::SSAFunction fn("execute");
	auto* entry = fn.addBlock();
	ASSERT_NE(entry, nullptr);
	ASSERT_NE(fn.addBlock(), nullptr);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Store, 0);
	retdec::ssa::IrInstr* call = fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);
	call->calleeName = "doAlgorithm";

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendPatternDetections(map, fn);

	ASSERT_EQ(map.count("execute"), 1u);
	bool found = false;
	for (const auto& d: map.at("execute"))
	{
		if (d.kind == "pattern" && d.label == "Strategy")
		{
			found = true;
			EXPECT_GE(d.confidence, 0.45f);
			EXPECT_NE(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}

TEST(SemanticExport, StrategyIndirectCallDoesNotTagSymbolName)
{
	retdec::ssa::SSAFunction fn("delegate");
	auto* entry = fn.addBlock();
	ASSERT_NE(entry, nullptr);
	ASSERT_NE(fn.addBlock(), nullptr);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Load, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Store, 0);
	fn.addInstr(entry->id, retdec::ssa::IrInstr::Op::Call, 0);

	retdec::analysis::SemanticDetectionMap map;
	retdec::analysis::appendPatternDetections(map, fn);

	ASSERT_EQ(map.count("delegate"), 1u);
	bool found = false;
	for (const auto& d: map.at("delegate"))
	{
		if (d.kind == "pattern" && d.label == "Strategy")
		{
			found = true;
			EXPECT_GE(d.confidence, 0.45f);
			EXPECT_EQ(d.detail.find("evidence:symbol_name"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}
