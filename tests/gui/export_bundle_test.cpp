/**
 * @file tests/gui/export_bundle_test.cpp
 */

#include "retdec/gui/model/export_bundle.h"
#include "retdec/gui/model/zip_writer.h"

#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

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
		path = fs::path(forge::os::fs::make_temp_dir("retdec-gui-bundle-").value);
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

std::string readAll(const fs::path& p)
{
	auto r = forge::os::read_file(p.string());
	return r ? r.value : std::string{};
}

bool zipContainsEntryName(const std::string& zipBytes, std::string_view name)
{
	return zipBytes.find(name) != std::string::npos;
}

m::DecompileBundleInput sampleInput(const fs::path& cPath)
{
	m::DecompileBundleInput in;
	in.cPath = cPath;
	in.decompilerExe = "C:/tools/retdec-decompiler.exe";
	in.decompilerCwd = "C:/tools";
	in.decompilerArgs = {"--input-file", "sample.exe"};
	in.logInline = "stage: done\n";
	return in;
}

/// A directory with sample.gui-decompiled.c and its config.
struct Sample
{
	TempDir tmp;
	fs::path cPath = tmp.path / "sample.gui-decompiled.c";
	fs::path zipPath = tmp.path / "out.zip";
	Sample()
	{
		writeFile(cPath, "int main(void) { return 0; }\n");
		writeFile(tmp.path / "sample.gui-decompiled.config.json", "{}");
	}
};

} // namespace

TEST(ZipWriter, RoundTripStoreEntry)
{
	TempDir tmp;
	const fs::path zipPath = tmp.path / "test.zip";
	{
		m::ZipWriter zip(zipPath);
		ASSERT_TRUE(zip.isOpen());
		ASSERT_TRUE(zip.addEntry("hello.txt", "hello bundle").has_value());
		ASSERT_TRUE(zip.close().has_value());
	}
	const std::string bytes = readAll(zipPath);
	EXPECT_GE(bytes.size(), 22u);
	EXPECT_EQ(bytes.substr(0, 4), std::string("PK\x03\x04", 4));
	EXPECT_TRUE(zipContainsEntryName(bytes, "hello.txt"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "hello bundle"));
}

TEST(ExportBundle, IncludesArtifactsAndCommand)
{
	Sample s;
	auto in = sampleInput(s.cPath);
	auto r = m::exportDecompileBundle(in, s.zipPath);
	ASSERT_TRUE(r.has_value()) << r.error();
	ASSERT_TRUE(fs::exists(s.zipPath));

	const std::string bytes = readAll(s.zipPath);
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.c"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.config.json"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "decompiler-command.txt"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "decompiler.log"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "--input-file"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "stage: done"));
	EXPECT_FALSE(zipContainsEntryName(bytes, "sample.gui-decompiled.dsm"));
}

TEST(ExportBundle, ExportBundleIncludesOptionalSidecars)
{
	Sample s;
	writeFile(s.tmp.path / "sample.gui-decompiled.c.refined.c", "int refined(void) { return 1; }\n");
	writeFile(s.tmp.path / "sample.gui-decompiled.c.refinement-manifest.json", "{\"accepted\":true}");
	writeFile(s.tmp.path / "sample.gui-decompiled.c.type-inference.json", "{\"ok\":true}");
	writeFile(s.tmp.path / "sample.gui-decompiled.dsm", "0x1: nop\n");
	writeFile(s.tmp.path / "sample.gui-decompiled.ll", "; ll\n");

	auto r = m::exportDecompileBundle(sampleInput(s.cPath), s.zipPath);
	ASSERT_TRUE(r.has_value()) << r.error();
	const std::string bytes = readAll(s.zipPath);
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.c.refined.c"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.c.refinement-manifest.json"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.c.type-inference.json"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.dsm"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.ll"));
}

TEST(ExportBundle, ExportBundleIncludesDotSidecars)
{
	Sample s;
	writeFile(s.tmp.path / "sample.gui-decompiled.cg.dot", "digraph {}");
	writeFile(s.tmp.path / "sample.gui-decompiled.cfg.main.dot", "digraph {}");
	writeFile(s.tmp.path / "other.cfg.main.dot", "digraph {}");

	auto r = m::exportDecompileBundle(sampleInput(s.cPath), s.zipPath);
	ASSERT_TRUE(r.has_value()) << r.error();
	const std::string bytes = readAll(s.zipPath);
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.cg.dot"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.cfg.main.dot"));
	EXPECT_FALSE(zipContainsEntryName(bytes, "other.cfg.main.dot"));
}

TEST(ExportBundle, LogFileWinsOverInlineLog)
{
	Sample s;
	const fs::path log = s.tmp.path / "run.log";
	writeFile(log, "from the log file\n");
	auto in = sampleInput(s.cPath);
	in.logFilePath = log;
	ASSERT_TRUE(m::exportDecompileBundle(in, s.zipPath).has_value());
	const std::string bytes = readAll(s.zipPath);
	EXPECT_TRUE(zipContainsEntryName(bytes, "from the log file"));
	EXPECT_FALSE(zipContainsEntryName(bytes, "stage: done"));

	// An empty / missing log file falls back to the inline log.
	in.logFilePath = s.tmp.path / "missing.log";
	ASSERT_TRUE(m::exportDecompileBundle(in, s.zipPath).has_value());
	EXPECT_TRUE(zipContainsEntryName(readAll(s.zipPath), "stage: done"));

	// No log at all: no decompiler.log entry.
	in.logInline.clear();
	ASSERT_TRUE(m::exportDecompileBundle(in, s.zipPath).has_value());
	EXPECT_FALSE(zipContainsEntryName(readAll(s.zipPath), "decompiler.log"));
}

TEST(ExportBundle, CommandText)
{
	m::DecompileBundleInput in;
	in.decompilerExe = "/opt/retdec/bin/retdec-decompiler";
	in.decompilerArgs = {"a.out", "-o", "a.c"};
	EXPECT_EQ(
		m::formatDecompilerCommandText(in), "executable: /opt/retdec/bin/retdec-decompiler\ncommand: a.out -o a.c\n");
	in.decompilerCwd = "/opt";
	EXPECT_EQ(
		m::formatDecompilerCommandText(in),
		"executable: /opt/retdec/bin/retdec-decompiler\ncwd: /opt\ncommand: a.out -o a.c\n");
}

TEST(ExportBundle, MissingCFileFails)
{
	m::DecompileBundleInput in;
	in.cPath = "C:/missing/sample.gui-decompiled.c";
	auto r = m::exportDecompileBundle(in, "C:/out.zip");
	ASSERT_FALSE(r.has_value());
	EXPECT_FALSE(r.error().empty());
}

TEST(ExportBundle, UnwritableZipFails)
{
	Sample s;
	auto r = m::exportDecompileBundle(sampleInput(s.cPath), s.tmp.path / "no-such-dir" / "out.zip");
	ASSERT_FALSE(r.has_value());
	EXPECT_FALSE(r.error().empty());
}
