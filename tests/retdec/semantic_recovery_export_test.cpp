/**
 * @file tests/retdec/semantic_recovery_export_test.cpp
 * @brief Unit tests for RETDEC_EMIT_BUILDABLE sidecar writer.
 */

#include "retdec/retdec/semantic_recovery_export.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
	EXPECT_NE(b.find("uint64_t __cxa_finalize(uint64_t a1) { return 0; }"),
			std::string::npos);
	EXPECT_EQ(b.find("WEAK return __cxa_finalize"), std::string::npos);
	EXPECT_EQ(b.find("WEAK return "), std::string::npos);

	fs::remove(outC);
	fs::remove(dir / "retdec_buildable_proto.h");
	fs::remove(dir / "retdec_buildable_proto_stubs.c");
	fs::remove(dir / "retdec_buildable_proto.buildable.c");
}
