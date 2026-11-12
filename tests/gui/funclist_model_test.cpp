/**
 * @file tests/gui/funclist_model_test.cpp
 * @brief Function list model: wildcard rules, hex fields, sorting, editing and
 *        the exact CSV / JSON export text.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/funclist_export.h"
#include "retdec/gui/model/funclist_filter.h"
#include "retdec/gui/model/funclist_model.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

using namespace retdec::gui::model;

namespace {

FunctionEntry fn(uint64_t addr, std::string name, float conf = 1.f)
{
	FunctionEntry e;
	e.address = addr;
	e.rawName = "_Z" + name;
	e.name = std::move(name);
	e.sizeBytes = 100;
	e.instrCount = 25;
	e.cc = "cdecl";
	e.confidence = {conf, conf, conf, conf};
	return e;
}

std::vector<const FunctionEntry*> ptrs(const std::vector<FunctionEntry>& v)
{
	std::vector<const FunctionEntry*> out;
	for (const auto& e: v)
		out.push_back(&e);
	return out;
}

} // namespace

TEST(FunclistModelTest, WildcardRules)
{
	EXPECT_TRUE(funclistWildcardMatch("*pha*", "alpha"));
	EXPECT_TRUE(funclistWildcardMatch("ALPHA", "alpha"));
	EXPECT_FALSE(funclistWildcardMatch("alp", "alpha")); // whole name
	EXPECT_TRUE(funclistWildcardMatch("a?c", "abc"));
	EXPECT_TRUE(funclistWildcardMatch("a*c", "abbbc"));
	EXPECT_FALSE(funclistWildcardMatch("a*c", "a/c")); // * stops at '/'
	EXPECT_FALSE(funclistWildcardMatch("a?c", "a/c"));
	EXPECT_TRUE(funclistWildcardMatch("[a-c]x", "Bx"));
	EXPECT_TRUE(funclistWildcardMatch("[!a]x", "bx"));
	EXPECT_FALSE(funclistWildcardMatch("[!a]x", "ax"));
	EXPECT_TRUE(funclistWildcardMatch("[ab", "[ab")); // unterminated class is literal
	EXPECT_TRUE(funclistWildcardMatch("*", ""));
	EXPECT_TRUE(funclistWildcardMatch("MyClass::*", "MyClass::run"));
}

TEST(FunclistModelTest, HexFields)
{
	EXPECT_EQ(funclistParseHex(" 0x1F "), 0x1Fu);
	EXPECT_EQ(funclistParseHex("1f"), 0x1Fu);
	EXPECT_EQ(funclistParseHex("0XABC"), 0xABCu);
	EXPECT_EQ(funclistParseHex("ffffffffffffffff"), 0xffffffffffffffffull);
	EXPECT_FALSE(funclistParseHex("").has_value());
	EXPECT_FALSE(funclistParseHex("0x").has_value());
	EXPECT_FALSE(funclistParseHex("xyz").has_value());
	EXPECT_FALSE(funclistParseHex("1ffffffffffffffff").has_value());
}

TEST(FunclistModelTest, SortingIsStableAndPerColumn)
{
	FunctionList list;
	auto a = fn(0x30, "beta", 0.5f);
	auto b = fn(0x10, "Alpha", 0.9f);
	auto c = fn(0x20, "gamma", 0.1f);
	b.patterns.stl = b.patterns.crypto = true;
	c.patterns.algo = true;
	a.notes = "zzz";
	c.sizeBytes = 0;
	list.setFunctions({a, b, c});
	FunctionListProxy p(&list);
	auto names = [&] {
		std::string s;
		for (std::size_t r = 0; r < p.rowCount(); ++r)
			s += p.entry(r).name + ",";
		return s;
	};
	EXPECT_EQ(names(), "beta,Alpha,gamma,");
	p.sortBy(FnColAddress, true);
	EXPECT_EQ(names(), "Alpha,gamma,beta,");
	p.sortBy(FnColName, true);
	EXPECT_EQ(names(), "Alpha,beta,gamma,");
	p.sortBy(FnColConfidence, false);
	EXPECT_EQ(names(), "Alpha,beta,gamma,");
	p.sortBy(FnColPatterns, false);
	EXPECT_EQ(names(), "Alpha,gamma,beta,");
	p.sortBy(FnColSize, true);
	EXPECT_EQ(names().substr(0, 6), "gamma,");
	p.sortBy(FnColNotes, false);
	EXPECT_EQ(names().substr(0, 5), "beta,");
	EXPECT_EQ(p.rowForAddress(0x30), 0);
	EXPECT_EQ(p.rowForAddress(0x99), -1);
	// The proxy follows the list.
	list.renameFunction(0x10, "zeta");
	p.sortBy(FnColName, true);
	EXPECT_EQ(names(), "beta,gamma,zeta,");
	p.setDemangleNames(false);
	// Raw names now: _ZAlpha < _Zbeta < _Zgamma (the renamed "zeta" kept its raw name).
	EXPECT_EQ(names(), "zeta,beta,gamma,");
}

TEST(FunclistModelTest, RenameRulesAndAnnotations)
{
	FunctionList list;
	auto e = fn(1, "f");
	e.rawName.clear();
	list.setFunctions({e, fn(2, "g")});
	int renames = 0;
	list.onRenamed = [&](uint64_t, const std::string&, const std::string&) { ++renames; };
	EXPECT_FALSE(list.renameFunction(1, "   "));
	EXPECT_FALSE(list.renameFunction(1, "f"));
	EXPECT_TRUE(list.renameFunction(1, "  h  "));
	EXPECT_EQ(list.entry(0).name, "h");
	EXPECT_EQ(renames, 1);
	EXPECT_TRUE(list.setNotes(1, "note"));
	EXPECT_FALSE(list.setSignature(1, "  "));
	EXPECT_TRUE(list.setSignature(1, " void h() "));
	EXPECT_EQ(list.entry(0).signature, "void h()");
	list.applyTag({1, 2}, "  t ");
	EXPECT_EQ(list.entry(1).tags, (std::vector<std::string>{"t"}));
	list.applyTag({1}, " ");
	EXPECT_EQ(list.entry(0).tags.size(), 1u);
	EXPECT_TRUE(list.clearAnnotation(1)); // no raw name: the name is cleared
	EXPECT_TRUE(list.entry(0).name.empty());
	EXPECT_TRUE(list.entry(0).notes.empty());
	EXPECT_TRUE(list.entry(0).tags.empty());
	EXPECT_TRUE(list.entry(0).signature.empty());
	EXPECT_FALSE(list.clearAnnotation(99));
	EXPECT_EQ(functionCellText(list.entry(0), FnColName), "");
}

TEST(FunclistModelTest, CellTextAndTooltips)
{
	auto e = fn(0x401000, "main", 0.8f);
	e.sizeBytes = 0;
	e.instrCount = 0;
	e.signature = "int main(int, char**)";
	e.patterns.stl = e.patterns.library = true;
	EXPECT_EQ(functionCellText(e, FnColSize), "\xE2\x80\x94");
	EXPECT_EQ(functionCellText(e, FnColInstrs), "\xE2\x80\x94");
	EXPECT_EQ(
		functionCellText(e, FnColConfidence), std::format("{} %", static_cast<int>(e.confidence.composite() * 100)));
	EXPECT_EQ(functionToolTip(e, FnColName), "main\n_Zmain\nint main(int, char**)");
	EXPECT_EQ(functionToolTip(e, FnColPatterns), "STL container / algorithm\nStatically linked library code");
	EXPECT_EQ(functionToolTip(fn(1, "x"), FnColPatterns), "No patterns detected");
	EXPECT_EQ(functionToolTip(e, FnColCC), "Calling convention: cdecl");
	EXPECT_EQ(confidenceTier(0.75f), ConfidenceTier::High);
	EXPECT_EQ(confidenceTier(0.4f), ConfidenceTier::Medium);
	EXPECT_EQ(confidenceTier(0.39f), ConfidenceTier::Low);
	EXPECT_EQ(patternBadges()[1].label, "Cr");
	EXPECT_EQ(patternBadges()[1].rgb, 0xf38ba8u);
}

TEST(FunclistModelTest, CsvTextMatchesQt)
{
	auto a = fn(0x401000, "main", 1.f);
	a.patterns.stl = true;
	a.notes = "entry";
	a.tags = {"x", "y"};
	auto b = fn(0x10, "f", 0.5f);
	b.cc.clear();
	const std::vector<FunctionEntry> v{a, b};
	const std::string expected = std::format(
		"Address,Name,SizeBytes,Instrs,Confidence,CC,STL,Crypto,Algo,Design,Lib,Notes,Tags\n"
		"0x401000,main,100,25,{:.3f},cdecl,1,0,0,0,0,entry,x;y\n"
		"0x10,f,100,25,{:.3f},,0,0,0,0,0,,\n",
		static_cast<double>(a.confidence.composite()),
		static_cast<double>(b.confidence.composite()));
	EXPECT_EQ(functionListCsv(ptrs(v)), expected);
	EXPECT_EQ(
		functionListCsv({}), "Address,Name,SizeBytes,Instrs,Confidence,CC,STL,Crypto,Algo,Design,Lib,Notes,Tags\n");
}

TEST(FunclistModelTest, JsonTextMatchesQtIndentedLayout)
{
	auto e = fn(0x10, "f", 0.5f);
	e.patterns.stl = true;
	e.notes = "n\"q";
	e.tags = {"a", "b"};
	const std::vector<FunctionEntry> v{e};
	const std::string conf = std::format("{}", static_cast<double>(e.confidence.composite()));
	const std::string expected =
		"[\n"
		"    {\n"
		"        \"address\": \"0x10\",\n"
		"        \"cc\": \"cdecl\",\n"
		"        \"confidence\": " + conf + ",\n"
		"        \"confidenceDetail\": {\n"
		"            \"algorithm\": 0.5,\n"
		"            \"structure\": 0.5,\n"
		"            \"typeInference\": 0.5,\n"
		"            \"variable\": 0.5\n"
		"        },\n"
		"        \"instrCount\": 25,\n"
		"        \"name\": \"f\",\n"
		"        \"notes\": \"n\\\"q\",\n"
		"        \"patterns\": {\n"
		"            \"algo\": false,\n"
		"            \"crypto\": false,\n"
		"            \"design\": false,\n"
		"            \"library\": false,\n"
		"            \"stl\": true\n"
		"        },\n"
		"        \"rawName\": \"_Zf\",\n"
		"        \"signature\": \"\",\n"
		"        \"sizeBytes\": 100,\n"
		"        \"tags\": [\n"
		"            \"a\",\n"
		"            \"b\"\n"
		"        ]\n"
		"    }\n"
		"]\n";
	EXPECT_EQ(functionListJson(ptrs(v)), expected);
	EXPECT_EQ(functionListJson({}), "[\n]\n");
	// Non-integral numbers in their shortest round-trip form.
	EXPECT_EQ(funclistIndentedJson(forge::json::value(0.1f)), "0.10000000149011612\n");
	EXPECT_EQ(funclistIndentedJson(forge::json::object{{"e", forge::json::array{}}}), "{\n    \"e\": [\n    ]\n}\n");
	// And it parses back.
	const auto parsed = forge::json::parse(functionListJson(ptrs(v)));
	EXPECT_EQ(parsed[std::size_t{0}]["notes"].str(), "n\"q");
}

TEST(FunclistModelTest, WriteTextFile)
{
	const auto path = std::filesystem::temp_directory_path() / "retdec-gui-funclist-model.txt";
	ASSERT_TRUE(funclistWriteTextFile(path.string(), "abc\n"));
	std::ifstream in(path);
	std::stringstream ss;
	ss << in.rdbuf();
	EXPECT_EQ(ss.str(), "abc\n");
	std::filesystem::remove(path);
	const auto bad = funclistWriteTextFile("/nonexistent-dir-xyz/file.txt", "x");
	ASSERT_FALSE(bad);
	EXPECT_NE(bad.error().find("/nonexistent-dir-xyz/file.txt"), std::string::npos);
}
