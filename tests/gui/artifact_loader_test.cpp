/**
 * @file tests/gui/artifact_loader_test.cpp
 */

#include "retdec/gui/model/artifact_loader.h"

#include <forge/os/fs.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace m = retdec::gui::model;
namespace fs = std::filesystem;

namespace {

struct TempDir
{
	fs::path path;
	TempDir()
	{
		path = fs::path(forge::os::fs::make_temp_dir("retdec-gui-art-").value);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

void writeFile(const fs::path& p, std::string_view data)
{
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f << data;
}

bool contains(const std::string& hay, std::string_view needle)
{
	return hay.find(needle) != std::string::npos;
}

m::DecompileArtifacts loadOrFail(const fs::path& cPath, const m::RecoverySettings& rec = {})
{
	auto r = m::loadDecompileArtifactsFromPaths(m::pathsFromOutputC(cPath), rec);
	EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
	return r ? std::move(*r) : m::DecompileArtifacts{};
}

} // namespace

TEST(ArtifactLoader, PathsFromOutputC)
{
	const auto p = m::pathsFromOutputC("C:/out/sample.gui-decompiled.c");
	EXPECT_EQ(p.cPath, fs::path("C:/out/sample.gui-decompiled.c"));
	EXPECT_EQ(p.configPath, fs::path("C:/out/sample.gui-decompiled.config.json"));
	EXPECT_EQ(p.dsmPath, fs::path("C:/out/sample.gui-decompiled.dsm"));
	EXPECT_EQ(p.llPath, fs::path("C:/out/sample.gui-decompiled.ll"));

	const auto upper = m::pathsFromOutputC("/x/A.C");
	EXPECT_EQ(upper.configPath, fs::path("/x/A.config.json"));
	const auto other = m::pathsFromOutputC("/x/out.py");
	EXPECT_EQ(other.configPath, fs::path("/x/out.py.config.json"));
}

TEST(ArtifactLoader, LoadMinimalConfig)
{
	TempDir tmp;
	const fs::path cPath = tmp.path / "t.gui-decompiled.c";
	writeFile(cPath, "int main(void) { return 0; }\n");
	writeFile(tmp.path / "t.gui-decompiled.config.json", R"({
        "functions": [{
            "name": "main",
            "demangledName": "main",
            "startAddr": "0x401000",
            "endAddr": "0x401010",
            "callingConvention": "cdecl",
            "fncType": "decompilerDefined",
            "basicBlocks": [{
                "startAddr": "0x401000",
                "endAddr": "0x401010",
                "preds": [],
                "succs": [],
                "calls": []
            }]
        }],
        "globals": []
    })");

	const auto art = loadOrFail(cPath);
	ASSERT_EQ(art.functions.size(), 1u);
	EXPECT_EQ(art.functions[0].name, "main");
	EXPECT_EQ(art.functions[0].address, 0x401000u);
	EXPECT_EQ(art.functions[0].sizeBytes, 0x10);
	EXPECT_EQ(art.functions[0].cc, "cdecl");
	EXPECT_FALSE(art.functions[0].isLibrary);
	EXPECT_FALSE(art.cfgBlocks.empty());
	EXPECT_TRUE(art.config.is_obj());
	EXPECT_EQ(art.cPath, cPath);
	ASSERT_EQ(art.callGraphNodes.size(), 1u);
	EXPECT_EQ(art.callGraphNodes[0].name, "main");
	EXPECT_EQ(art.callGraphNodes[0].instrCount, 4);
}

TEST(ArtifactLoader, MissingOrInvalidConfigFails)
{
	TempDir tmp;
	const fs::path cPath = tmp.path / "t.gui-decompiled.c";
	auto missing = m::loadDecompileArtifactsFromPaths(m::pathsFromOutputC(cPath));
	ASSERT_FALSE(missing.has_value());
	EXPECT_TRUE(contains(missing.error(), "Missing config"));

	writeFile(tmp.path / "t.gui-decompiled.config.json", "[1, 2");
	auto bad = m::loadDecompileArtifactsFromPaths(m::pathsFromOutputC(cPath));
	ASSERT_FALSE(bad.has_value());
	EXPECT_TRUE(contains(bad.error(), "Invalid config JSON"));

	writeFile(tmp.path / "t.gui-decompiled.config.json", "[]");
	EXPECT_FALSE(m::loadDecompileArtifactsFromPaths(m::pathsFromOutputC(cPath)).has_value());
}

TEST(ArtifactLoader, ExtractDsmSlice)
{
	const std::string dsm =
		"; function: foo at 0x401000 -- 0x401010\n"
		"0x401000:   90   nop\n"
		"0x401004:   c3   ret\n"
		"; function: bar at 0x402000 -- 0x402010\n";
	const std::string slice = m::extractDsmForFunction(dsm, 0x401000, 0x401010, "foo");
	EXPECT_TRUE(contains(slice, "nop"));
	EXPECT_FALSE(contains(slice, "bar"));

	// By name when the address does not match.
	EXPECT_TRUE(contains(m::extractDsmForFunction(dsm, 0x999, 0, "bar"), "0x402000"));
	// No header: plain address range.
	const std::string noHeader = "0x10: a\n0x14: b\n0x18: c\n";
	EXPECT_EQ(m::extractDsmForFunction(noHeader, 0x14, 0x18), "0x14: b");
	EXPECT_EQ(m::extractDsmForFunction(noHeader, 0x14, 0), "0x14: b\n0x18: c");
	EXPECT_TRUE(m::extractDsmForFunction("", 0, 0).empty());
}

TEST(ArtifactLoader, ExtractDsmInstructionsForRange)
{
	const auto ins = m::extractDsmInstructionsForRange("  0x10:  push ebp  \n0x14: ret\n; comment 0x12\n", 0x10, 0x14);
	ASSERT_EQ(ins.size(), 1u);
	EXPECT_EQ(ins[0].address, 0x10u);
	EXPECT_EQ(ins[0].text, "push ebp");
}

TEST(ArtifactLoader, LoadTypeHierarchyAndCfgInstrs)
{
	TempDir tmp;
	const fs::path cPath = tmp.path / "t.gui-decompiled.c";
	writeFile(cPath, "void fn(void) {}\n");
	writeFile(tmp.path / "t.gui-decompiled.config.json", R"({
        "functions": [{
            "name": "fn",
            "startAddr": "0x401000",
            "endAddr": "0x401010",
            "basicBlocks": [{
                "startAddr": "0x401000",
                "endAddr": "0x401008",
                "preds": [],
                "succs": [],
                "calls": []
            }]
        }],
        "vtables": [{
            "name": "FnVtable",
            "address": "0x403000",
            "items": [{
                "address": "0x403000",
                "targetAddress": "0x401000",
                "targetName": "fn"
            }]
        }],
        "classes": [{
            "name": "Fn",
            "demangledName": "Fn",
            "superClasses": ["Base"],
            "virtualTables": ["FnVtable"],
            "methods": ["fn"]
        }],
        "globals": []
    })");
	writeFile(tmp.path / "t.gui-decompiled.dsm", "0x401000:   55   push ebp\n0x401004:   5d   pop ebp\n");

	const auto art = loadOrFail(cPath);
	ASSERT_EQ(art.typeHierarchyClasses.size(), 1u);
	const auto& cls = art.typeHierarchyClasses[0];
	EXPECT_EQ(cls.name, "Fn");
	EXPECT_EQ(cls.vtableAddress, 0x403000u);
	ASSERT_EQ(cls.bases.size(), 1u);
	EXPECT_EQ(cls.bases[0].base, "Base");
	ASSERT_EQ(cls.vtable.size(), 1u);
	EXPECT_EQ(cls.vtable[0].funcName, "fn");
	EXPECT_EQ(cls.methodCount, 1);
	EXPECT_FALSE(cls.isAbstract);

	const auto bbIt = art.cfgBlocks.find(0x401000u);
	ASSERT_NE(bbIt, art.cfgBlocks.end());
	ASSERT_EQ(bbIt->second.size(), 1u);
	EXPECT_GE(bbIt->second[0].instrs.size(), 1u);
	EXPECT_TRUE(contains(bbIt->second[0].instrs[0].text, "push"));

	// Instruction counts come from the .dsm instruction lines.
	ASSERT_EQ(art.functions.size(), 1u);
	EXPECT_EQ(art.functions[0].instrCount, 2);
}

TEST(ArtifactLoader, ParseStartEndLines)
{
	TempDir tmp;
	const fs::path cPath = tmp.path / "t.gui-decompiled.c";
	writeFile(cPath, "line1\nint foo(void) {\n  return 0;\n}\n");
	writeFile(tmp.path / "t.gui-decompiled.config.json", R"({
        "functions": [{
            "name": "foo",
            "startAddr": "0x401000",
            "endAddr": "0x401010",
            "startLine": "2",
            "endLine": "4"
        }],
        "globals": []
    })");

	const auto art = loadOrFail(cPath);
	ASSERT_EQ(art.functions.size(), 1u);
	EXPECT_EQ(art.functions[0].startLine, 2);
	EXPECT_EQ(art.functions[0].endLine, 4);

	const std::string slice =
		m::extractCForFunction(cPath, art.functions[0].startLine, art.functions[0].endLine, "foo");
	EXPECT_TRUE(contains(slice, "int foo"));
	EXPECT_TRUE(contains(slice, "return 0"));
	EXPECT_FALSE(contains(slice, "line1"));
}

TEST(ArtifactLoader, ParseConstantGlobals)
{
	TempDir tmp;
	const fs::path cPath = tmp.path / "t.gui-decompiled.c";
	writeFile(cPath, "int mz;\n");
	writeFile(tmp.path / "t.gui-decompiled.config.json", R"({
        "functions": [],
        "globals": [{
            "name": "g_mz",
            "type": { "llvmIr": "i32", "isWideString": false },
            "storage": { "value": "0x401000" },
            "integer": "0x5A4D"
        }]
    })");

	const auto art = loadOrFail(cPath);
	ASSERT_GE(art.constants.size(), 1u);
	EXPECT_EQ(art.strings.size(), 0u);
	EXPECT_EQ(art.constants[0].address, 0x401000u);
	EXPECT_EQ(art.constants[0].value, 0x5A4Du);
	EXPECT_EQ(art.constants[0].size, m::ConstantSize::Dword);
	EXPECT_EQ(art.constants[0].hexStr, "00005A4D");
	EXPECT_EQ(art.constants[0].decStr, "23117");
	EXPECT_EQ(art.constants[0].label, "MZ (PE)");
	EXPECT_EQ(art.constants[0].labelKind, m::ConstantLabel::MagicNumber);
}

TEST(ArtifactLoader, StringsSemanticsCallGraphAndRefs)
{
	TempDir tmp;
	const fs::path cPath = tmp.path / "s.gui-decompiled.c";
	writeFile(cPath, "");
	writeFile(tmp.path / "s.gui-decompiled.config.json", R"({
        "functions": [{
            "name": "enc",
            "startAddr": "0x1000",
            "endAddr": "0x1040",
            "fncType": "staticallyLinked",
            "semanticDetections": [
                {"kind": "Crypto", "label": "AES", "confidence": 0.9, "detail": "sbox"},
                {"kind": "stl", "label": "vector", "confidence": 0.2},
                {"kind": "vtable", "label": ""}
            ],
            "basicBlocks": [{
                "startAddr": "0x1000", "endAddr": "0x1010",
                "succs": ["0x1010", "0x0"],
                "calls": [{"targetAddr": "0x2000"}, {"targetAddr": "0"}]
            }, {
                "startAddr": "0x1010", "endAddr": "0x1040", "succs": []
            }]
        }],
        "globals": [
            {"name": "s1", "realName": "héllo", "type": {"llvmIr": "[6 x i8]"}, "storage": {"value": "0x3000"}},
            {"name": "w1", "type": {"llvmIr": "[3 x i32]", "isWideString": true}, "storage": {"value": "0x3010"}},
            {"name": "q", "type": {"llvmIr": "i64"}, "storage": {"value": "0x3020"}, "value": 22},
            {"name": "zero", "type": {"llvmIr": "i16"}, "storage": {"value": "0"}}
        ]
    })");
	writeFile(tmp.path / "s.gui-decompiled.dsm", "0x1000: mov eax, [0x3000]\n0x1004: lea 3000\n");

	const auto art = loadOrFail(cPath);
	ASSERT_EQ(art.functions.size(), 1u);
	const auto& f = art.functions[0];
	EXPECT_TRUE(f.isLibrary);
	EXPECT_TRUE(f.patterns.library);
	EXPECT_TRUE(f.patterns.crypto);
	EXPECT_FALSE(f.patterns.stl); // confidence 0.2 below the 0.6 STL floor
	EXPECT_TRUE(f.patterns.design);

	ASSERT_EQ(art.semanticDetections.size(), 2u); // the unlabelled one is dropped
	EXPECT_EQ(art.semanticDetections[0].function, "enc");
	EXPECT_EQ(art.semanticDetections[0].label, "AES");
	EXPECT_DOUBLE_EQ(art.semanticDetections[0].confidence, 0.9);
	EXPECT_EQ(art.semanticDetections[0].detail, "sbox");

	ASSERT_EQ(art.strings.size(), 2u);
	EXPECT_EQ(art.strings[0].value, "héllo");
	EXPECT_EQ(art.strings[0].length, 5);
	EXPECT_EQ(art.strings[0].address, 0x3000u);
	EXPECT_EQ(art.strings[0].refCount, 3); // "0x3000" once, "3000" twice
	EXPECT_EQ(art.strings[1].type, m::StringType::Wide);
	EXPECT_EQ(art.strings[1].value, "w1");

	ASSERT_EQ(art.constants.size(), 1u); // the all-zero one is dropped
	EXPECT_EQ(art.constants[0].size, m::ConstantSize::Qword);
	EXPECT_EQ(art.constants[0].value, 22u);
	EXPECT_EQ(art.constants[0].hexStr, "0000000000000016");
	EXPECT_EQ(art.constants[0].label, "SSH");

	ASSERT_EQ(art.callGraphNodes.size(), 2u);
	EXPECT_EQ(art.callGraphNodes[1].name, "sub_2000");
	ASSERT_EQ(art.callGraphEdges.size(), 1u);
	EXPECT_EQ(art.callGraphEdges[0].callerAddress, 0x1000u);
	EXPECT_EQ(art.callGraphEdges[0].calleeAddress, 0x2000u);

	const auto& edges = art.cfgEdges.at(0x1000);
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_EQ(edges[0].from, 0x1000u);
	EXPECT_EQ(edges[0].to, 0x1010u);
	EXPECT_EQ(art.cfgBlocks.at(0x1000).size(), 2u);
	EXPECT_EQ(art.functions[0].instrCount, 2);
}

TEST(ArtifactLoader, RecoveryFloorsComeFromSettings)
{
	TempDir tmp;
	const fs::path cPath = tmp.path / "r.gui-decompiled.c";
	writeFile(tmp.path / "r.gui-decompiled.config.json", R"({"functions": [{"name": "f", "startAddr": "0x10",
        "semanticDetections": [{"kind": "crypto", "label": "x", "confidence": 0.75}]}]})");
	m::RecoverySettings rec;
	EXPECT_TRUE(loadOrFail(cPath, rec).functions.at(0).patterns.crypto);
	rec.cryptoConfidence = 0.8;
	EXPECT_FALSE(loadOrFail(cPath, rec).functions.at(0).patterns.crypto);
}

TEST(ArtifactLoader, ExtractLlvmForFunction)
{
	const std::string ll =
		"; ModuleID = 'x'\n"
		"define dso_local i32 @main(i32 %argc) {\n"
		"entry:\n"
		"  br label %b\n"
		"b:\n"
		"  ret i32 0\n"
		"}\n"
		"define i32 @other() {\n  ret i32 1\n}\n";
	const std::string fn = m::extractLlvmForFunction(ll, "main");
	EXPECT_TRUE(fn.starts_with("define dso_local i32 @main"));
	EXPECT_TRUE(fn.ends_with("}"));
	EXPECT_FALSE(contains(fn, "@other"));
	// The Qt search ("define <name>") still works.
	EXPECT_EQ(m::extractLlvmForFunction("define foo { x }\n", "foo"), "define foo { x }");
	EXPECT_TRUE(m::extractLlvmForFunction(ll, "missing").empty());
	EXPECT_TRUE(m::extractLlvmForFunction(ll, "").empty());
}

TEST(ArtifactLoader, ExtractCForFunctionByNamePrefersDefinition)
{
	const std::string c =
		"// prototypes\n"
		"int32_t foo(int32_t a);\n"
		"\n"
		"int32_t bar(void) {\n    return foo(1);\n}\n"
		"\n"
		"int32_t foo(int32_t a) {\n    if (a) { return 1; }\n    return 0;\n}\n";
	const std::string fn = m::extractCForFunctionByName(c, "foo");
	EXPECT_TRUE(fn.starts_with("int32_t foo(int32_t a) {"));
	EXPECT_TRUE(fn.ends_with("return 0;\n}"));
	EXPECT_FALSE(contains(fn, "bar"));

	// Word boundary: "foo" does not match "xfoo".
	EXPECT_TRUE(m::extractCForFunctionByName("int xfoo(void) { }\n", "foo").empty());
	EXPECT_TRUE(m::extractCForFunctionByName(c, "nothere").empty());

	TempDir tmp;
	const fs::path cPath = tmp.path / "x.c";
	writeFile(cPath, c);
	EXPECT_EQ(m::extractCForFunction(cPath, -1, -1, "bar"), "int32_t bar(void) {\n    return foo(1);\n}");
	EXPECT_EQ(m::extractCForFunction(cPath, 2, 2, ""), "int32_t foo(int32_t a);");
	EXPECT_TRUE(m::extractCForFunction(cPath, -1, -1, "").empty());
}

TEST(ArtifactLoader, FunctionViewData)
{
	TempDir tmp;
	const fs::path cPath = tmp.path / "v.gui-decompiled.c";
	writeFile(cPath, "int main(void) {\n  return 0;\n}\n");
	writeFile(tmp.path / "v.gui-decompiled.config.json", R"({"functions": [{
        "name": "main", "startAddr": "0x401000", "endAddr": "0x401008", "startLine": "1", "endLine": "3",
        "basicBlocks": [{"startAddr": "0x401000", "endAddr": "0x401008", "succs": ["0x401004"]}]}]})");
	writeFile(
		tmp.path / "v.gui-decompiled.dsm",
		"; function: main at 0x401000 -- 0x401008\n0x401000: push ebp\n0x401004: ret\n");
	writeFile(tmp.path / "v.gui-decompiled.ll", "define i32 @main() {\nentry:\n  ret i32 0\n}\n");

	const auto art = loadOrFail(cPath);
	const auto v = m::buildFunctionViewData(art, 0x401000, "main");
	EXPECT_EQ(v.address, 0x401000u);
	EXPECT_EQ(v.endAddress, 0x401008u);
	EXPECT_EQ(v.startLine, 1);
	EXPECT_EQ(v.endLine, 3);
	EXPECT_TRUE(contains(v.assemblyText, "push ebp"));
	EXPECT_TRUE(v.irText.starts_with("define i32 @main"));
	EXPECT_EQ(v.cText, "int main(void) {\n  return 0;\n}");
	ASSERT_EQ(v.cfgBlocks.size(), 1u);
	EXPECT_EQ(v.cfgBlocks[0].instrs.size(), 2u);
	ASSERT_EQ(v.cfgEdges.size(), 1u);
	ASSERT_EQ(v.lineMapping.size(), 3u); // min(3 asm, 4 ir, 3 c)
	EXPECT_EQ(v.lineMapping[2].asmLine, 3);
	EXPECT_EQ(v.lineMapping[2].cLine, 3);

	const auto noC = m::buildFunctionViewData(art, 0x401000, "main", false);
	EXPECT_TRUE(noC.cText.empty());

	const auto unknown = m::buildFunctionViewData(art, 0x999, "nothing");
	EXPECT_EQ(unknown.endAddress, 0u);
	EXPECT_TRUE(unknown.cfgBlocks.empty());
	EXPECT_TRUE(unknown.irText.empty());
}

TEST(ArtifactLoader, IdentityLineMapping)
{
	EXPECT_TRUE(m::buildIdentityLineMapping("", "a", "b").empty());
	const auto map = m::buildIdentityLineMapping("a\nb", "a\nb\nc", "x\ny");
	ASSERT_EQ(map.size(), 2u);
	EXPECT_EQ(map[1].irLine, 2);
}
