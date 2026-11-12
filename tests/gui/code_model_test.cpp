/**
 * @file tests/gui/code_model_test.cpp
 * @brief Pure logic of the code views: tokenizer, text find / locate,
 *        address parsing and jump targets, tri-pane mapping, history.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/model/code_address.h"
#include "retdec/gui/model/code_highlight.h"
#include "retdec/gui/model/code_text.h"
#include "retdec/gui/model/tri_pane_sync.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace retdec::gui::model;

namespace {
std::vector<TokenSpan> tok(CodeLang l, std::string_view line)
{
	int st = kStateNormal;
	return highlightLine(l, line, st);
}
TokenKind at(CodeLang l, std::string_view line, std::size_t col)
{
	return tokenAt(tok(l, line), static_cast<uint32_t>(col));
}
} // namespace

// ── Tokenizer ────────────────────────────────────────────────────────────────

TEST(CodeHighlight, CKeywordsTypesStringsNumbers)
{
	const std::string_view l = R"(static uint32_t f(char c) { return "a\"b" + 0x10 + 42u; } // done)";
	EXPECT_EQ(at(CodeLang::C, l, 0), TokenKind::Keyword);            // static
	EXPECT_EQ(at(CodeLang::C, l, 7), TokenKind::Type);               // uint32_t
	EXPECT_EQ(at(CodeLang::C, l, 16), TokenKind::Text);              // f
	EXPECT_EQ(at(CodeLang::C, l, 18), TokenKind::Type);              // char
	EXPECT_EQ(at(CodeLang::C, l, l.find("\"a")), TokenKind::String); // string with escaped quote
	EXPECT_EQ(at(CodeLang::C, l, l.find("b\"")), TokenKind::String);
	EXPECT_EQ(at(CodeLang::C, l, l.find("0x10")), TokenKind::Number);
	EXPECT_EQ(at(CodeLang::C, l, l.find("42u") + 2), TokenKind::Number);
	EXPECT_EQ(at(CodeLang::C, l, l.find("done")), TokenKind::Comment);
}

TEST(CodeHighlight, CPreprocessorAndCxxKeywords)
{
	EXPECT_EQ(at(CodeLang::C, "  #define X 1 // c", 2), TokenKind::Preprocessor);
	EXPECT_EQ(at(CodeLang::C, "  #define X 1 // c", 16), TokenKind::Comment);
	EXPECT_EQ(at(CodeLang::C, "class A;", 0), TokenKind::Text);
	EXPECT_EQ(at(CodeLang::Cxx, "class A;", 0), TokenKind::Keyword);
	EXPECT_EQ(at(CodeLang::Cxx, "x = nullptr;", 4), TokenKind::Keyword);
}

TEST(CodeHighlight, IdentifiersContainingDigitsAreNotNumbers)
{
	EXPECT_EQ(at(CodeLang::C, "int v1 = a2;", 5), TokenKind::Text);
	EXPECT_EQ(at(CodeLang::C, "int v1 = a2;", 10), TokenKind::Text);
}

TEST(CodeHighlight, BlockCommentsCarryState)
{
	int st = kStateNormal;
	auto a = highlightLine(CodeLang::C, "int a; /* open", st);
	EXPECT_EQ(st, kStateInBlockComment);
	EXPECT_EQ(tokenAt(a, 0), TokenKind::Keyword);
	EXPECT_EQ(tokenAt(a, 9), TokenKind::Comment);
	auto b = highlightLine(CodeLang::C, "still */ int b;", st);
	EXPECT_EQ(st, kStateNormal);
	EXPECT_EQ(tokenAt(b, 0), TokenKind::Comment);
	EXPECT_EQ(tokenAt(b, 9), TokenKind::Keyword);
	auto c = highlightLine(CodeLang::Java, "/* a */ class /* b */ X", st);
	EXPECT_EQ(st, kStateNormal);
	EXPECT_EQ(tokenAt(c, 8), TokenKind::Keyword);
	EXPECT_EQ(tokenAt(c, 15), TokenKind::Comment);
}

TEST(CodeHighlight, Assembly)
{
	const std::string_view l = "0x401000:  55 89  push    rbp ; save frame";
	EXPECT_EQ(at(CodeLang::Asm, l, 0), TokenKind::Address);
	EXPECT_EQ(at(CodeLang::Asm, l, 11), TokenKind::Number); // byte column
	EXPECT_EQ(at(CodeLang::Asm, l, l.find("push")), TokenKind::Mnemonic);
	EXPECT_EQ(at(CodeLang::Asm, l, l.find("rbp")), TokenKind::Register);
	EXPECT_EQ(at(CodeLang::Asm, l, l.find("save")), TokenKind::Comment);
	const std::string_view m = "  mov dword ptr [ebp - 8], 0x10";
	EXPECT_EQ(at(CodeLang::Asm, m, 2), TokenKind::Mnemonic);
	EXPECT_EQ(at(CodeLang::Asm, m, m.find("dword")), TokenKind::Keyword);
	EXPECT_EQ(at(CodeLang::Asm, m, m.find("ebp")), TokenKind::Register);
	EXPECT_EQ(at(CodeLang::Asm, m, m.find("8")), TokenKind::Number);
	EXPECT_EQ(at(CodeLang::Asm, m, m.find("0x10")), TokenKind::Address);
	EXPECT_EQ(at(CodeLang::Asm, "loop_start:", 0), TokenKind::Label);
	EXPECT_EQ(at(CodeLang::Asm, "rep movsb", 4), TokenKind::Mnemonic); // prefix + mnemonic
	EXPECT_EQ(at(CodeLang::Asm, "  call sub_401100", 7), TokenKind::Text);
	EXPECT_EQ(at(CodeLang::Asm, "  movaps xmm0, xmm15", 9), TokenKind::Register);
	EXPECT_EQ(at(CodeLang::Asm, "  add r8d, r15", 6), TokenKind::Register);
}

TEST(CodeHighlight, LlvmIr)
{
	const std::string_view l = "  %5 = load i32, ptr %x.addr, align 4 ; 0x401000";
	EXPECT_EQ(at(CodeLang::IR, l, 2), TokenKind::SsaName);
	EXPECT_EQ(at(CodeLang::IR, l, l.find("load")), TokenKind::Keyword);
	EXPECT_EQ(at(CodeLang::IR, l, l.find("i32")), TokenKind::Type);
	EXPECT_EQ(at(CodeLang::IR, l, l.find("ptr")), TokenKind::Type);
	EXPECT_EQ(at(CodeLang::IR, l, l.find("%x.addr") + 3), TokenKind::SsaName);
	EXPECT_EQ(at(CodeLang::IR, l, l.find("4 ;")), TokenKind::Number);
	EXPECT_EQ(at(CodeLang::IR, l, l.find("0x401000")), TokenKind::Comment);
	EXPECT_EQ(at(CodeLang::IR, "entry:", 0), TokenKind::Label);
	EXPECT_EQ(at(CodeLang::IR, "%bb1:", 1), TokenKind::Label);
	EXPECT_EQ(at(CodeLang::IR, "define i32 @main() {", 11), TokenKind::SsaName);
	EXPECT_EQ(at(CodeLang::IR, "  store i32 -1, ptr %p", 12), TokenKind::Number);
}

TEST(CodeHighlight, OtherOutputLanguages)
{
	EXPECT_EQ(at(CodeLang::Python, "def f(x): return len(x)  # c", 0), TokenKind::Keyword);
	EXPECT_EQ(at(CodeLang::Python, "def f(x): return len(x)  # c", 17), TokenKind::Builtin);
	EXPECT_EQ(at(CodeLang::Python, "def f(x): return len(x)  # c", 27), TokenKind::Comment);
	EXPECT_EQ(at(CodeLang::Python, "s = 'it\\'s'", 5), TokenKind::String);
	EXPECT_EQ(at(CodeLang::Lua, "local x = nil -- c", 0), TokenKind::Keyword);
	EXPECT_EQ(at(CodeLang::Lua, "local x = nil -- c", 14), TokenKind::Comment);
	EXPECT_EQ(at(CodeLang::CSharp, "var s = @\"c:\\x\";", 8), TokenKind::String);
	EXPECT_EQ(at(CodeLang::CSharp, "var s = 1;", 0), TokenKind::Keyword);
	EXPECT_EQ(at(CodeLang::Java, "public final int x;", 7), TokenKind::Keyword);
	EXPECT_TRUE(tok(CodeLang::Plain, "int x; // c").empty());
}

TEST(CodeHighlight, SpansAreSortedAndDisjoint)
{
	for (CodeLang l: {CodeLang::Asm, CodeLang::IR, CodeLang::C, CodeLang::Python})
	{
		const auto s = tok(l, "  0x40 %a = \"str\" mov eax, 5 ; x // y # z");
		for (std::size_t i = 1; i < s.size(); ++i)
			EXPECT_GE(s[i].start, s[i - 1].start + s[i - 1].length);
	}
}

TEST(CodeHighlight, UnterminatedStringStopsAtEol)
{
	const auto s = tok(CodeLang::C, "x = \"abc");
	ASSERT_FALSE(s.empty());
	EXPECT_EQ(s.back().start + s.back().length, 8u);
}

TEST(CodeHighlight, SettingsStrings)
{
	EXPECT_EQ(codeLangFromSettingsString("cpp"), CodeLang::Cxx);
	EXPECT_EQ(codeLangFromSettingsString(" C++ "), CodeLang::Cxx);
	EXPECT_EQ(codeLangFromSettingsString("cs"), CodeLang::CSharp);
	EXPECT_EQ(codeLangFromSettingsString("CSharp"), CodeLang::CSharp);
	EXPECT_EQ(codeLangFromSettingsString("java"), CodeLang::Java);
	EXPECT_EQ(codeLangFromSettingsString("py"), CodeLang::Python);
	EXPECT_EQ(codeLangFromSettingsString("lua"), CodeLang::Lua);
	EXPECT_EQ(codeLangFromSettingsString(""), CodeLang::C);
	EXPECT_EQ(codeLangName(CodeLang::CSharp), "C#");
}

// ── Text ─────────────────────────────────────────────────────────────────────

TEST(CodeText, SplitJoinAsTextDocumentBlocks)
{
	EXPECT_EQ(splitLines("").size(), 1u);
	EXPECT_EQ(splitLines("a\nb\n").size(), 3u);
	EXPECT_EQ(splitLines("a\r\nb").at(0), "a");
	EXPECT_EQ(joinLines(splitLines("a\nb\n")), "a\nb\n");
	const auto lines = splitLines("hello\nworld\n!");
	EXPECT_EQ(textInRange(lines, {{0, 3}, {1, 2}}), "lo\nwo");
	EXPECT_EQ(textInRange(lines, {{1, 2}, {0, 3}}), "lo\nwo"); // reversed
	EXPECT_EQ(textInRange(lines, {{2, 0}, {2, 1}}), "!");
}

TEST(CodeText, FindForwardBackwardWrapsCaseInsensitive)
{
	const auto lines = splitLines("int main\nMAIN x\nmain");
	auto r = findText(lines, "main", {});
	ASSERT_TRUE(r);
	EXPECT_EQ(r->begin, (TextPos{0, 4}));
	r = findText(lines, "main", *r);
	ASSERT_TRUE(r);
	EXPECT_EQ(r->begin, (TextPos{1, 0}));
	r = findText(lines, "main", *r);
	EXPECT_EQ(r->begin, (TextPos{2, 0}));
	r = findText(lines, "main", *r); // wraps
	EXPECT_EQ(r->begin, (TextPos{0, 4}));
	r = findText(lines, "main", *r, {.forward = false}); // wraps backwards
	EXPECT_EQ(r->begin, (TextPos{2, 0}));
	r = findText(lines, "main", *r, {.forward = false});
	EXPECT_EQ(r->begin, (TextPos{1, 0}));
	EXPECT_FALSE(findText(lines, "MAIN", {{1, 1}, {1, 1}}, {.caseSensitive = true, .wrap = false}));
	EXPECT_TRUE(findText(lines, "MAIN", {{1, 1}, {1, 1}}, {.caseSensitive = true, .wrap = true}));
	EXPECT_FALSE(findText(lines, "zzz", {}));
	EXPECT_FALSE(findText(lines, "", {}));
	EXPECT_EQ(countMatches(lines, "main"), 3u);
}

TEST(CodeText, FindFunctionLineAndLocate)
{
	const auto lines = splitLines(
		"// foo is used\nint foobar(void);\nint foo (int a) {\n  return foo(a);\n}\n// 0x401000\nvoid "
		"function_401100(void);");
	EXPECT_EQ(findFunctionLine(lines, "foo"), 2);
	EXPECT_EQ(findFunctionLine(lines, "bar"), std::nullopt);
	EXPECT_EQ(locateFunction(lines, 0x401000, "foobar")->line, 1);
	EXPECT_EQ(locateFunction(lines, 0x401100, "")->line, 6);
	EXPECT_EQ(locateFunction(lines, 0x401000, "")->line, 5);
	EXPECT_FALSE(locateFunction(lines, 0x999, "nope"));
	EXPECT_EQ(findAddressLine(splitLines("x\n  0X401000: nop\n"), 0x401000), 1);
}

TEST(CodeText, SaveAndLoad)
{
	const auto dir = retdec::gui::test::scratchDir("code_text");
	ASSERT_TRUE(saveTextFile(dir / "a.c", "int x;\n"));
	EXPECT_EQ(*loadTextFile(dir / "a.c"), "int x;\n");
	EXPECT_FALSE(saveTextFile(dir / "missing" / "a.c", "x"));
	EXPECT_FALSE(loadTextFile(dir / "missing.c"));
}

// ── Addresses ────────────────────────────────────────────────────────────────

TEST(CodeAddress, ParseFirstAddressFromDsmLine)
{
	const auto addr = parseFirstAddress("0x401000:  push    ebp");
	ASSERT_TRUE(addr.has_value());
	EXPECT_EQ(*addr, 0x401000ull);
	EXPECT_FALSE(parseFirstAddress("nop").has_value());
}

TEST(CodeAddress, UnifiedContextMenuLabels)
{
	EXPECT_STREQ(kCopyAddressLabel, "Copy Address");
	EXPECT_STREQ(kGoToFunctionLabel, "Go to Function");
	EXPECT_EQ(formatAddressHex(0x401000), "0x401000");
}

TEST(CodeAddress, GoToInput)
{
	EXPECT_EQ(parseAddressInput("0x401000"), 0x401000u);
	EXPECT_EQ(parseAddressInput(" 401000 "), 0x401000u);
	EXPECT_EQ(parseAddressInput("0XABC"), 0xabcu);
	EXPECT_FALSE(parseAddressInput("zz"));
	EXPECT_FALSE(parseAddressInput(""));
	EXPECT_EQ(parseHexU64("0x200"), 0x200u);
	EXPECT_EQ(parseHexU64("junk"), 0u);
}

TEST(CodeAddress, JumpTargets)
{
	EXPECT_EQ(jumpTarget("0x401000:  call 0x401100"), 0x401100u);
	EXPECT_EQ(jumpTarget("0x401000:  jne 0x401020"), 0x401020u);
	// Qt rule kept: with several 0x tokens the last one is the target.
	EXPECT_EQ(jumpTarget("0x401000:  jne 0x401020 <main+0x20>"), 0x20u);
	EXPECT_EQ(jumpTarget("  jmp 0x401050"), 0x401050u);          // no own address
	EXPECT_EQ(jumpTarget("0x401000:  JMP 0x401050"), 0x401050u); // case-insensitive
	EXPECT_FALSE(jumpTarget("0x401000:  mov eax, 0x401050"));    // not a branch
	EXPECT_FALSE(jumpTarget("0x401000:  jmp 0x401000"));         // to itself
	EXPECT_FALSE(jumpTarget("0x401000:  call eax"));             // indirect
	EXPECT_FALSE(jumpTarget("0x401000:  jumper 0x401050"));      // whole word only
	EXPECT_TRUE(isBranchLine("  je 0x10"));
	EXPECT_EQ(listingAddress("  0x401000:   55  push ebp"), 0x401000u);
	EXPECT_EQ(listingAddress("401000:\tnop"), 0x401000u);
	EXPECT_FALSE(listingAddress("; function: main at 0x401000"));
	EXPECT_FALSE(listingAddress("main:"));
	EXPECT_EQ(findListingLine(splitLines("call 0x401100\n0x401100: ret"), 0x401100), 1);
	EXPECT_EQ(findListingLine(splitLines("call 0x401100\nnop"), 0x401100), 0);
}

// ── Tri-pane sync ────────────────────────────────────────────────────────────

TEST(TriPaneSync, LineAddresses)
{
	EXPECT_EQ(lineAddress(TriPane::Asm, "401000:\tpush ebp"), 0x401000u);
	EXPECT_EQ(lineAddress(TriPane::Asm, "  0x401003:   ret"), 0x401003u);
	EXPECT_FALSE(lineAddress(TriPane::Asm, "main:"));
	EXPECT_EQ(lineAddress(TriPane::C, "  x = 1; // 0x401005"), 0x401005u);
	EXPECT_EQ(lineAddress(TriPane::C, "// Address range: 0x401000 - 0x401030"), 0x401000u);
	EXPECT_FALSE(lineAddress(TriPane::C, "  x = 0x10;"));
	EXPECT_EQ(lineAddress(TriPane::IR, "  ret i32 0 ; 0x401010"), 0x401010u);
	const auto meta = parseIrAddressMetadata(splitLines("!0 = !{i64 4198400}\n!12 = !{i64 0x401010}"));
	ASSERT_EQ(meta.size(), 2u);
	EXPECT_EQ(meta.at(0), 4198400u);
	EXPECT_EQ(lineAddress(TriPane::IR, "  store i32 1, ptr %a, !insn.addr !12", &meta), 0x401010u);
	EXPECT_FALSE(lineAddress(TriPane::IR, "  store i32 1, ptr %a, !insn.addr !12"));
}

TEST(TriPaneSync, BuildMappingByAddress)
{
	const auto asmL = splitLines("0x401000: push ebp\n0x401001: mov ebp, esp\n0x401003: ret");
	const auto irL = splitLines(
		"define void @f() {\n  %a = alloca i32, !insn.addr !0\n  ret void ; 0x401003\n}\n!0 = !{i64 4198401}");
	const auto cL = splitLines("void f(void) { // 0x401000\n  return; // 0x401003\n}\n// 0x402000 orphan");
	const LineMapping m = buildLineMappingByAddress(asmL, irL, cL);
	ASSERT_NE(m.byAsmLine(1), nullptr);
	EXPECT_EQ(m.byAsmLine(1)->cLine, 1);
	EXPECT_EQ(m.byAsmLine(1)->irLine, -1);
	EXPECT_EQ(m.byAsmLine(2)->irLine, 2);
	EXPECT_EQ(m.byAsmLine(2)->cLine, -1);
	EXPECT_EQ(m.byAsmLine(3)->irLine, 3);
	EXPECT_EQ(m.byAsmLine(3)->cLine, 2);
	EXPECT_EQ(m.byIrLine(3)->asmLine, 3);
	ASSERT_NE(m.byCLine(4), nullptr); // orphan C address still maps (to nothing else)
	EXPECT_EQ(m.byCLine(4)->asmLine, -1);
}

TEST(TriPaneSync, ProportionalScroll)
{
	EXPECT_EQ(proportionalScroll(50, 100, 200), 100);
	EXPECT_EQ(proportionalScroll(10, 0, 200), 0);
	EXPECT_EQ(proportionalScroll(100, 100, 7), 7);
}

TEST(TriPaneSync, NavHistory)
{
	NavHistory h(3);
	EXPECT_FALSE(h.back());
	h.push(1, "a");
	h.push(2, "b");
	h.push(3, "c");
	h.push(4, "d"); // drops 1
	EXPECT_EQ(h.size(), 3u);
	EXPECT_EQ(h.back()->address, 3u);
	EXPECT_EQ(h.back()->address, 2u);
	EXPECT_FALSE(h.back());
	EXPECT_EQ(h.forward()->address, 3u);
	h.push(9, "z"); // truncates forward history
	EXPECT_FALSE(h.canGoForward());
	EXPECT_EQ(h.back()->address, 3u);
	h.clear();
	EXPECT_FALSE(h.canGoBack());
	EXPECT_EQ(h.index(), -1);
}
