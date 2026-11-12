/**
 * @file tests/gui/inspect_model_test.cpp
 * @brief inspect_model: fileinfo JSON, hex dumps, file helpers and running
 *        fileinfo / unpacker children (with stand-in scripts on POSIX).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/model/inspect_model.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace retdec::gui::model;
namespace fs = std::filesystem;

namespace {

const char* kFileinfoJson = R"({
  "inputFile": "/tmp/a.out",
  "fileFormat": "ELF",
  "fileClass": "64-bit",
  "architecture": "x86-64",
  "osOrAbi": "UNIX - System V",
  "packed": "yes",
  "fileSize": 4096,
  "stripped": true,
  "nothing": null,
  "tools": [ {"type": "compiler", "name": "GCC"}, {"type": "packer", "name": "UPX", "version": "3.96"} ],
  "sectionTable": { "numberOfSections": "2", "sections": [
      {"index": "0", "name": ".text", "address": "0x401000", "offset": "0x1000", "sizeInFile": "0x200"},
      {"index": "1", "name": ".data", "address": "0x402000", "offset": "0x1200", "sizeInFile": "0x10"} ] },
  "nested": { "deeper": { "SHA256": "abcdef" } },
  "md5": "0123",
  "crc32": "  deadbeef  "
})";

void writeFile(const fs::path& p, const std::string& s)
{
	std::ofstream o(p, std::ios::binary);
	o << s;
}

#if !defined(_WIN32)
fs::path script(const fs::path& dir, const std::string& name, const std::string& body)
{
	const fs::path p = dir / name;
	writeFile(p, "#!/bin/sh\n" + body);
	fs::permissions(p, fs::perms::owner_all);
	return p;
}
#endif

} // namespace

TEST(InspectModel, ParsesFileinfoJson)
{
	auto r = parseFileinfoJson(kFileinfoJson);
	ASSERT_TRUE(r) << r.error();
	EXPECT_EQ(r->sha256, "abcdef");
	EXPECT_EQ(r->md5, "0123");
	EXPECT_EQ(r->crc32, "deadbeef"); // trimmed
	EXPECT_EQ(r->format, "ELF");
	EXPECT_EQ(r->arch, "x86-64");
	EXPECT_EQ(r->os, "UNIX - System V");
	EXPECT_EQ(r->packer, "UPX");
	ASSERT_EQ(r->sections.size(), 2u);
	EXPECT_EQ(r->sections[0].name, ".text");
	EXPECT_EQ(r->sections[0].addressValue(), 0x401000u);
	EXPECT_EQ(r->sections[0].sizeValue(), 0x200u);
	EXPECT_EQ(r->sections[1].offset, 0x1200u);
	// Hashes first, then scalars sorted by key, objects / arrays skipped.
	ASSERT_GE(r->summaryRows.size(), 4u);
	EXPECT_EQ(r->summaryRows[0].first, "sha256");
	EXPECT_EQ(r->summaryRows[1].first, "md5");
	EXPECT_EQ(r->summaryRows[2].first, "crc32");
	EXPECT_EQ(r->summaryRows[3].first, "architecture");
	bool sawSize = false, sawNull = false, sawBool = false;
	for (const auto& [k, v]: r->summaryRows)
	{
		EXPECT_NE(k, "tools");
		EXPECT_NE(k, "sectionTable");
		if (k == "fileSize") sawSize = v == "4096";
		if (k == "nothing") sawNull = v == "null";
		if (k == "stripped") sawBool = v == "true";
	}
	EXPECT_TRUE(sawSize && sawNull && sawBool);
	EXPECT_NE(r->prettyJson.find("\n  \"fileFormat\": \"ELF\""), std::string::npos);
	EXPECT_EQ(formatHashesForClipboard(*r), "SHA-256: abcdef\nMD5: 0123\nCRC32: deadbeef");
}

TEST(InspectModel, BadJsonAndPeOs)
{
	EXPECT_FALSE(parseFileinfoJson("not json"));
	EXPECT_FALSE(parseFileinfoJson("[1, 2]"));
	auto pe = parseFileinfoJson(R"({"fileFormat": "PE", "architecture": "x86"})");
	ASSERT_TRUE(pe);
	EXPECT_EQ(pe->os, "Windows");
	EXPECT_TRUE(pe->sections.empty());
	EXPECT_EQ(formatHashesForClipboard(*pe), "");
	EXPECT_EQ(firstNestedString(kFileinfoJson, "sha256"), "abcdef");
	EXPECT_EQ(firstNestedString("{oops", "sha256"), "");
	EXPECT_EQ(prettyPrintJson("{oops"), "{oops");
	EXPECT_EQ(prettyPrintJson(R"({"a":[1,{}]})"), "{\n  \"a\": [\n    1,\n    {}\n  ]\n}\n");
}

TEST(InspectModel, HexDumpAndSnippet)
{
	std::vector<uint8_t> d;
	for (int i = 0; i < 20; ++i)
		d.push_back(uint8_t(0x41 + i));
	d[1] = 0x00;
	const std::string dump = formatHexDump(d, 0x1000);
	const auto nl = dump.find('\n');
	ASSERT_NE(nl, std::string::npos);
	EXPECT_EQ(dump.substr(0, nl), "00001000  41 00 43 44 45 46 47 48  49 4a 4b 4c 4d 4e 4f 50  A.CDEFGHIJKLMNOP");
	EXPECT_EQ(dump.substr(nl + 1, 10), "00001010  ");
	EXPECT_NE(dump.find("51 52 53 54"), std::string::npos);
	EXPECT_EQ(formatHexDump({}, 0), "");
	EXPECT_EQ(hexSnippet({0xde, 0xad, 0x0f}), "de ad 0f");
}

TEST(InspectModel, Sizes)
{
	EXPECT_EQ(humanSize(0), "\xE2\x80\x94");
	EXPECT_EQ(humanSize(512), "512 B");
	EXPECT_EQ(humanSize(1536), "1.5 KiB");
	EXPECT_EQ(humanSize(3 * 1024 * 1024), "3.0 MiB");
	EXPECT_EQ(humanSize(int64_t(5) << 30), "5.00 GiB");
	EXPECT_EQ(defaultUnpackOutput("/x/y/a.exe"), "/x/y/a-unpacked");
	EXPECT_EQ(defaultUnpackOutput("/x.d/noext"), "/x.d/noext-unpacked");
	EXPECT_EQ(defaultUnpackOutput(""), "");
}

TEST(InspectModel, FileHelpers)
{
	const auto dir = retdec::gui::test::scratchDir("inspect_files");
	std::string bytes;
	for (int i = 0; i < 256; ++i)
		bytes += char(i);
	writeFile(dir / "bin", bytes);
	EXPECT_EQ(fileSize(dir / "bin"), 256);
	EXPECT_EQ(fileSize(dir / "missing"), -1);
	auto r = readFileRange(dir / "bin", 250, 100);
	ASSERT_TRUE(r);
	ASSERT_EQ(r->size(), 6u);
	EXPECT_EQ((*r)[0], 250);
	EXPECT_FALSE(readFileRange(dir / "missing", 0, 4));
	auto h = blake3File(dir / "bin");
	ASSERT_TRUE(h);
	EXPECT_EQ(h->size(), 64u);
	writeFile(dir / "empty", "");
	EXPECT_EQ(
		*blake3File(dir / "empty"), "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"); // BLAKE3("")
}

TEST(InspectModel, ArgsAndTail)
{
	EXPECT_EQ(fileinfoArgs("/b"), (std::vector<std::string>{"--json", "--verbose", "/b"}));
	EXPECT_EQ(unpackerArgs("/b", "/o", true), (std::vector<std::string>{"-b", "/b", "-o", "/o"}));
	EXPECT_EQ(unpackerArgs("/b", "/o", false), (std::vector<std::string>{"/b", "-o", "/o"}));
	ToolRun r;
	r.kind = InspectTool::Unpacker;
	r.out = "a";
	r.err = "b";
	EXPECT_EQ(r.outputTail(), "ab");
	r.kind = InspectTool::Fileinfo;
	r.out = std::string(5000, 'x');
	EXPECT_EQ(r.outputTail().size(), 4000u + 4u); // "\n…"
}

TEST(InspectModel, RefusesWithoutExecutable)
{
	InspectModel m;
	EXPECT_FALSE(m.startFileinfo("/bin/ls", ""));
	EXPECT_FALSE(m.startFileinfo("", "/bin/true"));
	EXPECT_FALSE(m.startUnpacker("/bin/ls", "", "/tmp/x", false));
	EXPECT_FALSE(m.anyRunning());
}

TEST(InspectModel, SpawnFailureIsReported)
{
	InspectModel m;
	ASSERT_TRUE(m.startFileinfo("/bin/ls", "/definitely/not/a/retdec-fileinfo"));
	ASSERT_TRUE(m.waitIdle(5000));
	auto runs = m.poll();
	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].exitCode, -1);
	EXPECT_FALSE(runs[0].errorText.empty());
}

#if !defined(_WIN32)
TEST(InspectModel, RunsFileinfoAndUnpacker)
{
	const auto dir = retdec::gui::test::scratchDir("inspect_run");
	const auto fi =
		script(dir, "fake-fileinfo", "echo '{\"fileFormat\":\"ELF\",\"sha256\":\"aa\"}'\necho 'note' >&2\n");
	const auto un = script(dir, "fake-unpacker", "echo \"unpacking $@\"\nexit 3\n");
	InspectModel m;
	ASSERT_TRUE(m.startFileinfo("/some/bin", fi.string()));
	EXPECT_TRUE(m.running(InspectTool::Fileinfo));
	ASSERT_TRUE(m.startUnpacker("/some/bin", un.string(), "/some/out", true));
	EXPECT_FALSE(m.startUnpacker("/some/bin", un.string(), "/some/out", true)); // already running
	ASSERT_TRUE(m.waitIdle(10'000));
	auto runs = m.poll();
	ASSERT_EQ(runs.size(), 2u);
	for (const auto& r: runs)
	{
		if (r.kind == InspectTool::Fileinfo)
		{
			EXPECT_EQ(r.exitCode, 0);
			EXPECT_EQ(r.tool, "retdec-fileinfo");
			EXPECT_EQ(r.args, fileinfoArgs("/some/bin"));
			EXPECT_EQ(r.cwd, dir.string());
			EXPECT_TRUE(parseFileinfoJson(r.out));
			EXPECT_EQ(r.err, "note\n");
		}
		else
		{
			EXPECT_EQ(r.exitCode, 3);
			EXPECT_EQ(r.out, "unpacking -b /some/bin -o /some/out\n");
		}
	}
	EXPECT_TRUE(m.poll().empty());
}

TEST(InspectModel, CancelKillsTheChild)
{
	const auto dir = retdec::gui::test::scratchDir("inspect_cancel");
	const auto slow = script(dir, "slow", "sleep 30\n");
	InspectModel m;
	ASSERT_TRUE(m.startFileinfo("/b", slow.string()));
	const auto t0 = std::chrono::steady_clock::now();
	m.cancelAll();
	EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));
	EXPECT_FALSE(m.anyRunning());
	EXPECT_TRUE(m.poll().empty()); // cancelled runs are not reported
}
#endif
