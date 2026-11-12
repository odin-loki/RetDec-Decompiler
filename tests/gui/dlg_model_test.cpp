/**
 * @file tests/gui/dlg_model_test.cpp
 * @brief Unit tests of the dialogs' model helpers (no UI): input parsing,
 *        per-run option mapping, Signature Studio argv, settings form lists
 *        and validation, help content / docs lookup, Target apply and the
 *        batch path list.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "dlg_test_support.h"

#include "retdec/gui/model/batch_decompile_queue.h"
#include "retdec/gui/model/cli_tool_paths.h"
#include "retdec/gui/model/dlg_help.h"
#include "retdec/gui/model/dlg_input.h"
#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/model/dlg_settings_form.h"
#include "retdec/gui/model/dlg_signature_studio.h"
#include "retdec/gui/model/dlg_target.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <set>

using namespace retdec::gui::model;
namespace fs = std::filesystem;

// ── dlg_input ────────────────────────────────────────────────────────────────

TEST(DlgInput, UnsignedAutoBase)
{
	EXPECT_EQ(dlgParseUnsignedAuto("0x401000"), 0x401000u);
	EXPECT_EQ(dlgParseUnsignedAuto("  0X1f  "), 0x1fu);
	EXPECT_EQ(dlgParseUnsignedAuto("010"), 8u); // octal, as QString::toULongLong(…, 0)
	EXPECT_EQ(dlgParseUnsignedAuto("0"), 0u);
	EXPECT_EQ(dlgParseUnsignedAuto("4096"), 4096u);
	EXPECT_EQ(dlgParseUnsignedAuto("+5"), 5u);
	EXPECT_FALSE(dlgParseUnsignedAuto(""));
	EXPECT_FALSE(dlgParseUnsignedAuto("0x"));
	EXPECT_FALSE(dlgParseUnsignedAuto("-1"));
	EXPECT_FALSE(dlgParseUnsignedAuto("09"));
	EXPECT_FALSE(dlgParseUnsignedAuto("12 34"));
	EXPECT_FALSE(dlgParseUnsignedAuto("0x1ffffffffffffffff")); // overflow
	EXPECT_EQ(dlgParseUnsignedAuto("18446744073709551615"), UINT64_MAX);
}

TEST(DlgInput, IntAndLines)
{
	EXPECT_EQ(dlgParseInt("-12"), -12);
	EXPECT_EQ(dlgParseInt(" 7 "), 7);
	EXPECT_FALSE(dlgParseInt("--1"));
	EXPECT_FALSE(dlgParseInt("2147483648"));
	EXPECT_EQ(dlgParseInt("-2147483648"), INT32_MIN);
	EXPECT_EQ(dlgParseGotoLine("3"), 3);
	EXPECT_EQ(dlgParseGotoLine("0").error(), "Not a valid line number");
	EXPECT_EQ(dlgParseGotoLine("x").error(), "Not a valid line number");
	EXPECT_EQ(dlgParseGotoAddress("0x10"), 16u);
	EXPECT_EQ(dlgParseGotoAddress("q").error(), "Not a valid address");
}

TEST(DlgInput, EntryPoint)
{
	EXPECT_EQ(dlgParseEntryPoint(""), 0u);
	EXPECT_EQ(dlgParseEntryPoint("   "), 0u);
	EXPECT_EQ(dlgParseEntryPoint("0x401000"), 0x401000u);
	EXPECT_EQ(dlgParseEntryPoint("0XABC"), 0xabcu);
	EXPECT_EQ(dlgParseEntryPoint("010"), 10u); // decimal here, unlike go-to-address
	EXPECT_FALSE(dlgParseEntryPoint("0xg"));
	EXPECT_EQ(dlgParseEntryPoint("abc").error(), "Invalid entry point (use 0x… hex or decimal).");
	EXPECT_EQ(dlgFormatEntryPoint(0), "");
	EXPECT_EQ(dlgFormatEntryPoint(0x401000), "0x401000");
}

TEST(DlgInput, MaxMemory)
{
	EXPECT_EQ(dlgParseMaxMemory(""), 0u);
	EXPECT_EQ(dlgParseMaxMemory("0"), 0u);
	EXPECT_EQ(dlgParseMaxMemory(" 1024 "), 1024u);
	EXPECT_EQ(dlgParseMaxMemory("1k").error(), "Not a valid byte count.");
	EXPECT_EQ(dlgFormatMaxMemory(0), "0");
	EXPECT_EQ(dlgFormatMaxMemory(77), "77");
	EXPECT_EQ(dlgMaxMemoryStatus(5), "Next decompile will pass --max-memory 5");
	EXPECT_EQ(dlgMaxMemoryStatus(0), "Next decompile will omit --max-memory");
}

// ── dlg_run_options ──────────────────────────────────────────────────────────

TEST(DlgRunOptions, RawFormMapping)
{
	RawArchiveForm f;
	f.endianText = "little";
	f.bitSizeText = "16";
	f.vmaText = "0x8000";
	f.arNameText = "  a.o ";
	f.arIndexText = "2";
	RawArchiveOptions o = rawArchiveOptionsFromForm(f);
	EXPECT_EQ(o.endian, "little");
	EXPECT_EQ(o.bitSize, 16);
	EXPECT_EQ(o.rawSectionVma, 0x8000u);
	EXPECT_EQ(o.arName, "a.o");
	EXPECT_EQ(o.arIndex, 2);
	f.endianText = "omit";
	f.bitSizeText = "48";
	f.vmaText = "010";
	f.arIndexText = "x";
	o = rawArchiveOptionsFromForm(f);
	EXPECT_TRUE(o.endian.empty());
	EXPECT_EQ(o.bitSize, 0);
	EXPECT_EQ(o.rawSectionVma, 8u);
	EXPECT_FALSE(o.arIndex);
	// round trip
	RawArchiveOptions r{"big", 64, 0x1000, "m.o", 0};
	EXPECT_EQ(rawArchiveOptionsFromForm(rawArchiveFormFromOptions(r)), r);
	EXPECT_EQ(rawArchiveFormFromOptions({}).endianText, "omit");
	EXPECT_EQ(rawArchiveFormFromOptions({}).bitSizeText, "omit");
}

TEST(DlgRunOptions, NextRunOptionsReachTheCommandLine)
{
	NextRunOptions o;
	o.cStyle.keepAllBrackets = true;
	o.cStyle.callInfoObtainer = "optim";
	o.cStyle.backendDisabledOpts = " A,B ";
	o.raw.endian = "big";
	o.raw.bitSize = 32;
	o.raw.arName = "x.o";
	o.raw.arIndex = 4;
	o.rawMode = true;
	o.maxMemoryBytes = 1 << 20;
	o.keepLibraryFuncs = true;
	o.varRenamer = "simple";
	DecompilerLaunchRequest req;
	req.binaryPath = "/tmp/a.bin";
	applyNextRunOptions(o, req);
	EXPECT_TRUE(req.keepAllBrackets);
	EXPECT_EQ(req.callInfoObtainer, "optim");
	EXPECT_EQ(req.backendDisabledOpts, "A,B");
	EXPECT_EQ(req.endian, "big");
	EXPECT_EQ(req.bitSize, 32);
	EXPECT_EQ(req.arIndex, 4);
	EXPECT_TRUE(req.rawMode);
	EXPECT_EQ(req.maxMemoryBytes, 1u << 20);
	auto args = buildDecompilerArguments(req, false);
	ASSERT_TRUE(args);
	auto has = [&](std::string_view a) {
		return std::find(args->args.begin(), args->args.end(), a) != args->args.end();
	};
	EXPECT_TRUE(has("--backend-keep-all-brackets"));
	EXPECT_TRUE(has("--backend-keep-library-funcs"));
	EXPECT_TRUE(has("--ar-name"));
	EXPECT_TRUE(has("--max-memory"));
}

TEST(DlgRunOptions, CallInfoAndRunStage)
{
	EXPECT_EQ(cStyleCallInfoFromText("omit"), "");
	EXPECT_EQ(cStyleCallInfoFromText("pessim"), "pessim");
	EXPECT_EQ(cStyleCallInfoIndex("optim"), 1);
	EXPECT_EQ(cStyleCallInfoIndex(""), 0);
	ASSERT_EQ(runStageItems().size(), 5u);
	EXPECT_EQ(runStageFromText("Fast decompile"), RunStageChoice::FastDecompile);
	EXPECT_EQ(runStageFromText("Signature Studio"), RunStageChoice::SignatureStudio);
	EXPECT_FALSE(runStageFromText("nope"));
	EXPECT_FALSE(runStageFromIndex(5));
	EXPECT_EQ(pdbPathStatus("/a.pdb"), "Next decompile will pass --pdb /a.pdb");
	EXPECT_EQ(staticSigFileStatus("/r.yar"), "Next decompile will pass --static-code-sigfile /r.yar");
	EXPECT_EQ(fileDialogStartDir("/definitely/not/here"), "");
	EXPECT_EQ(fileDialogStartDir(fs::temp_directory_path().string()), fs::temp_directory_path().string());
}

// ── dlg_signature_studio ─────────────────────────────────────────────────────

TEST(DlgSignatureStudioModel, CommandLines)
{
	auto ar = buildArExtractorCommand("/opt/retdec/bin/retdec-ar-extractor", " /x/lib.a ", "/x/out");
	ASSERT_TRUE(ar);
	EXPECT_EQ(ar->arguments, (std::vector<std::string>{"-e", "-o", "/x/out", "/x/lib.a"}));
	EXPECT_EQ(ar->workingDirectory, fs::path(retdec::gui::test::absPath("/opt/retdec/bin")));
	EXPECT_EQ(ar->toolLabel, "retdec-ar-extractor");

	auto mo = buildMachoExtractorCommand("/b/retdec-macho-extractor", "/x/fat", "/x/o");
	ASSERT_TRUE(mo);
	EXPECT_EQ(mo->arguments, (std::vector<std::string>{"--all", "-o", "/x/o", "/x/fat"}));
	EXPECT_EQ(buildMachoExtractorCommand("/b/m", "", "/x").error(), "Set Mach-O file and output directory.");

	auto b2p = buildBin2patCommand("/b/retdec-bin2pat", "/x/obj.o", "");
	ASSERT_TRUE(b2p);
	EXPECT_EQ(b2p->defaultedOutput, retdec::gui::test::absPath("/x/obj.o.pat"));
	EXPECT_EQ(b2p->arguments, (std::vector<std::string>{"-o", retdec::gui::test::absPath("/x/obj.o.pat"), "/x/obj.o"}));
	auto p2y = buildPat2yaraCommand("/b/retdec-pat2yara", "/x/obj.pat", "/x/r.yar");
	ASSERT_TRUE(p2y);
	EXPECT_TRUE(p2y->defaultedOutput.empty());
	EXPECT_EQ(p2y->arguments, (std::vector<std::string>{"-o", "/x/r.yar", "/x/obj.pat"}));
	EXPECT_EQ(buildPat2yaraCommand("/b/p", " ", "").error(), "Set pat2yara input file.");

	auto ex = buildExpressCommand("/x/lib.a", "/x/lib.yar", "/usr/bin/python3", "/s/scripts/creator.py");
	ASSERT_TRUE(ex);
	EXPECT_EQ(ex->executable, fs::path("/usr/bin/python3"));
	EXPECT_EQ(ex->arguments, (std::vector<std::string>{"/s/scripts/creator.py", "-o", "/x/lib.yar", "/x/lib.a"}));
	EXPECT_EQ(ex->workingDirectory, fs::path(retdec::gui::test::absPath("/s/scripts")));
	EXPECT_EQ(buildExpressCommand("", "/o", "/p", "/s").error(), "Express: set library input and YARA output path.");
	EXPECT_NE(buildExpressCommand("/l", "/o", "", "/s").error().find("Python 3"), std::string::npos);
}

TEST(DlgSignatureStudioModel, DefaultsTailAndStaticPick)
{
	const SigStudioDefaults d = sigStudioDefaultsForBinary("/w/archive.tar.a");
	EXPECT_EQ(d.expressOut, retdec::gui::test::absPath("/w/archive.tar-libsig.yar")); // completeBaseName
	EXPECT_EQ(d.workDir, retdec::gui::test::absPath("/w/retdec-sig-work"));
	EXPECT_TRUE(sigStudioDefaultsForBinary("").expressLib.empty());

	EXPECT_EQ(sigToolOutputTail("abcdef", 3), "def");
	EXPECT_EQ(sigToolOutputTail("ab", 3), "ab");
	EXPECT_EQ(sigToolOutputTail("x\xC3\xA9", 1), ""); // never splits a UTF-8 sequence

	SigToolResult r;
	r.tool = "t";
	r.exitCode = 2;
	r.outputTail = "boom";
	EXPECT_EQ(sigToolLogEntry(r), "--- t exit 2 ---\nboom");
	r.failedToStart = true;
	EXPECT_EQ(sigToolLogEntry(r), "Error: boom");

	const auto dir = retdec::gui::test::makeTempDir("sigpick");
	EXPECT_FALSE(pickStaticSigFile((dir / "a.yar").string(), (dir / "b.yar").string()));
	std::ofstream(dir / "b.yar") << "x";
	EXPECT_EQ(pickStaticSigFile((dir / "a.yar").string(), (dir / "b.yar").string()), (dir / "b.yar").generic_string());
	std::ofstream(dir / "a.yar") << "x";
	EXPECT_EQ(pickStaticSigFile((dir / "a.yar").string(), (dir / "b.yar").string()), (dir / "a.yar").generic_string());
	fs::remove_all(dir);
}

TEST(DlgSignatureStudioModel, RunnerReportsMissingTool)
{
	SigToolRunner r;
	SigToolCommand c;
	c.toolLabel = "retdec-x";
	EXPECT_EQ(r.start(c).error(), "Tool not found: retdec-x");
	c.executable = "/__no_such_dir__/retdec-x";
	auto st = r.start(c);
	if (st)
	{
		auto res = r.waitFor(std::chrono::seconds(10));
		ASSERT_TRUE(res);
		EXPECT_TRUE(res->failedToStart || res->exitCode != 0);
	}
	else
		EXPECT_EQ(st.error(), "Failed to start: retdec-x");
	EXPECT_FALSE(r.running());
}

// ── dlg_settings_form ────────────────────────────────────────────────────────

TEST(DlgSettingsForm, ComboListsAndIndices)
{
	EXPECT_EQ(settingsThemeItems().size(), 3u);
	EXPECT_EQ(settingsLanguageIndex("fr"), 2);
	EXPECT_EQ(settingsLanguageIndex("xx"), 0);
	EXPECT_EQ(settingsLanguageCode(4), "zh");
	EXPECT_EQ(settingsLanguageCode(9), "en");
	EXPECT_EQ(settingsProfileIndex("fast"), 0);
	EXPECT_EQ(settingsProfileIndex("weird"), 1);
	EXPECT_EQ(settingsOutputLangIndex("csharp"), 2);
	EXPECT_EQ(settingsOutputLangIndex("cpp"), 0);
	EXPECT_EQ(settingsQuantItems()[int(MLSettings::QuantLevel::F16)], "F16");
	EXPECT_EQ(settingsVerbosityItems()[int(AdvancedSettings::Verbosity::Debug)], "Debug");
	const auto fams = settingsEditorFontFamilies("My Mono");
	ASSERT_FALSE(fams.empty());
	EXPECT_EQ(fams[0], "My Mono");
	EXPECT_EQ(settingsEditorFontFamilies("JetBrains Mono")[0], "JetBrains Mono");
}

TEST(DlgSettingsForm, PassListAndValidation)
{
	auto names = settingsLlvmPassNames();
	ASSERT_TRUE(names) << names.error();
	std::set<std::string> uniq(names->begin(), names->end());
	EXPECT_EQ(uniq.size(), names->size());
	EXPECT_EQ(settingsDisabledPasses({"a", "b", "c"}, {true, false, false}), (std::vector<std::string>{"b", "c"}));

	const auto dir = retdec::gui::test::makeTempDir("validate");
	DecompilerSettings d;
	EXPECT_TRUE(validateSettingsForApply(d));
	d.decompileOutputDir = (dir / "x" / "y").string();
	EXPECT_TRUE(validateSettingsForApply(d));
	EXPECT_TRUE(fs::is_directory(dir / "x" / "y"));
	d.extraConfigPath = (dir / "none.json").string();
	EXPECT_EQ(
		validateSettingsForApply(d).error(), "Decompiler configuration file does not exist:\n" + d.extraConfigPath);
	fs::remove_all(dir);

	PluginMetadata m;
	m.name = "N";
	m.version = "2";
	m.description = "D";
	m.author = "A";
	EXPECT_EQ(pluginRowLabel(m), "N  v2");
	EXPECT_EQ(pluginDetailText(m, "/p.so"), "N v2 — D\nAuthor: A\nFile: /p.so");
	EXPECT_EQ(settingsNonEmptyTrimmed({" a ", "", "  "}), std::vector<std::string>{"a"});
}

// ── dlg_help ─────────────────────────────────────────────────────────────────

TEST(DlgHelp, AboutAndShortcuts)
{
	const std::string a = aboutMarkdown();
	EXPECT_NE(a.find("RetDec — Retargetable Decompiler"), std::string::npos);
	EXPECT_NE(a.find("Batch Decompile"), std::string::npos);
	const auto& g = keyboardShortcutGroups();
	ASSERT_EQ(g.size(), 4u);
	EXPECT_EQ(g[0].entries.front().keys, "Ctrl+O");
	EXPECT_EQ(g[1].entries.size(), 2u);
}

TEST(DlgHelp, DocsLookup)
{
	const auto dir = retdec::gui::test::makeTempDir("docsdir");
	fs::create_directories(dir / "share" / "retdec" / "docs" / "sub");
	fs::create_directories(dir / "bin");
	std::ofstream(dir / "share" / "retdec" / "docs" / "b.md") << "# B";
	std::ofstream(dir / "share" / "retdec" / "docs" / "README.md") << "# R";
	std::ofstream(dir / "share" / "retdec" / "docs" / "sub" / "A.md") << "# A";
	std::ofstream(dir / "share" / "retdec" / "docs" / "x.txt") << "no";
	setApplicationDirPathOverride(dir / "bin");
	const auto dirs = docsSearchDirs();
	ASSERT_FALSE(dirs.empty());
	EXPECT_EQ(dirs[0], (dir / "bin" / ".." / "share" / "retdec" / "docs").lexically_normal());
	const fs::path found = findDocsDir();
	EXPECT_EQ(fs::weakly_canonical(found), fs::weakly_canonical(dir / "share" / "retdec" / "docs"));
	const auto docs = listDocs(found);
	ASSERT_EQ(docs.size(), 3u);
	EXPECT_EQ(docs[0].title, "README.md");
	EXPECT_EQ(docs[1].title, "b.md");
	EXPECT_EQ(docs[2].title, "sub/A.md");
	EXPECT_EQ(defaultDocIndex(docs), 0);
	EXPECT_EQ(*readDoc(docs[1].path), "# B");
	EXPECT_FALSE(readDoc(dir / "missing.md"));
	EXPECT_EQ(defaultDocIndex({}), -1);
	setApplicationDirPathOverride({});
	fs::remove_all(dir);
}

// ── dlg_target ───────────────────────────────────────────────────────────────

TEST(DlgTargetModel, ApplyFields)
{
	ProjectFile pf("/b.exe");
	pf.setEntryPoint(0x10);
	pf.clearModified();
	EXPECT_EQ(targetFieldsFromProject(pf).entry, "0x10");
	ASSERT_TRUE(applyTargetFields(pf, {" mips ", " linux", "0x20"}));
	EXPECT_EQ(pf.arch(), "mips");
	EXPECT_EQ(pf.os(), "linux");
	EXPECT_EQ(pf.entryPoint(), 0x20u);
	EXPECT_TRUE(pf.isModified());
	auto bad = applyTargetFields(pf, {"x86", "", "bogus"});
	EXPECT_FALSE(bad);
	EXPECT_EQ(pf.arch(), "x86"); // arch / OS written before the entry check, as the Qt window did
	EXPECT_EQ(pf.entryPoint(), 0x20u);
}

TEST(DlgTargetModel, ConfigHelpersAndJson)
{
	const auto dir = retdec::gui::test::makeTempDir("tcfg");
	AppSettings s;
	s.setStoragePath(dir / "s.json");
	int n = 0;
	s.addChangeListener([&] { ++n; });
	ASSERT_TRUE(setDecompilerConfigPath(s, dir / "c.json"));
	EXPECT_EQ(s.decompiler.extraConfigPath, (dir / "c.json").generic_string());
	EXPECT_EQ(n, 1);
	EXPECT_FALSE(decompilerConfigEditable(s.decompiler.extraConfigPath));
	std::ofstream(dir / "c.json") << "{}";
	EXPECT_TRUE(decompilerConfigEditable(s.decompiler.extraConfigPath));
	ASSERT_TRUE(setDecompilerConfigPath(s, {}));
	EXPECT_TRUE(s.decompiler.extraConfigPath.empty());

	EXPECT_EQ(targetConfigBrowseStart("/c/x.json", "/b/bin.exe"), "/c/x.json");
	EXPECT_EQ(targetConfigBrowseStart("", "/b/bin.exe"), retdec::gui::test::absPath("/b"));
	EXPECT_EQ(targetConfigBrowseStart("", "—"), "");

	EXPECT_TRUE(validateJsonText(R"({"a": [1, 2]})"));
	auto err = validateJsonText("{ nope");
	ASSERT_FALSE(err);
	EXPECT_NE(err.error().find("at offset"), std::string::npos);

	ASSERT_TRUE(writeTextFile(dir / "t.txt", "hi"));
	EXPECT_EQ(*readTextFile(dir / "t.txt"), "hi");
	EXPECT_EQ(readTextFile(dir / "zz.txt").error(), "Cannot open:\n" + (dir / "zz.txt").string());
	fs::remove_all(dir);
}

TEST(DlgTargetModel, BatchPathList)
{
	std::vector<std::string> list;
	EXPECT_EQ(batchAddPaths(list, {"/a/x.exe", "/a/y.exe", "/a/x.exe", ""}), 2);
	EXPECT_EQ(batchAddPaths(list, {"/a/../a/x.exe"}), 0); // same absolute path
	ASSERT_EQ(list.size(), 2u);
	batchRemoveIndices(list, {5, 0, 0});
	EXPECT_EQ(list, std::vector<std::string>{retdec::gui::test::absPath("/a/y.exe")});
	// The queue helpers the main window runs the list with.
	EXPECT_EQ(batchDecompileStatusLabel(1, 1, list[0]), "Batch 1/1: y.exe");
}
