/**
 * @file tests/gui/cli_tool_paths_test.cpp
 */

#include "retdec/gui/model/cli_tool_paths.h"

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
	TempDir() { path = fs::canonical(fs::path(forge::os::fs::make_temp_dir("retdec-gui-tools-").value)); }
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

void touchExecutable(const fs::path& p, std::string_view content = "#!/bin/sh\nexit 0\n")
{
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary) << content;
	fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
}

/// Points applicationDirPath() at a fake install for the test.
class CliToolPaths : public ::testing::Test {
protected:
	void TearDown() override { m::setApplicationDirPathOverride({}); }
	TempDir tmp;
};

fs::path exe(const char* name)
{
#if defined(_WIN32)
	return fs::path(std::string(name) + ".exe");
#else
	return fs::path(name);
#endif
}

} // namespace

TEST_F(CliToolPaths, ApplicationDirIsTheTestBinaryDir)
{
	const fs::path dir = m::applicationDirPath();
	EXPECT_FALSE(dir.empty());
	EXPECT_TRUE(fs::is_directory(dir));
	m::setApplicationDirPathOverride(tmp.path / "bin");
	EXPECT_EQ(m::applicationDirPath(), tmp.path / "bin");
	m::setApplicationDirPathOverride({});
	EXPECT_EQ(m::applicationDirPath(), dir);
}

TEST_F(CliToolPaths, FindsToolsBesideTheGui)
{
	const fs::path bin = tmp.path / "prefix" / "bin";
	touchExecutable(bin / exe("retdec-decompiler"));
	touchExecutable(bin / exe("fileinfo")); // legacy name fallback
	touchExecutable(bin / exe("retdec-unpacker"));
	fs::create_directories(tmp.path / "prefix" / "share" / "retdec");
	std::ofstream(tmp.path / "prefix" / "share" / "retdec" / "decompiler-config.json") << "{}";
	m::setApplicationDirPathOverride(bin);

	EXPECT_EQ(m::resolveRetdecDecompilerExecutablePath(), bin / exe("retdec-decompiler"));
	EXPECT_EQ(m::resolveRetdecUnpackerExecutable(), bin / exe("retdec-unpacker"));
	if (m::resolveCliTool("retdec-fileinfo").empty()) // not installed system-wide
		EXPECT_EQ(m::resolveRetdecFileinfoExecutable(), bin / exe("fileinfo"));
	EXPECT_EQ(
		m::resolveBundledDecompilerConfigPath(), tmp.path / "prefix" / "share" / "retdec" / "decompiler-config.json");
	EXPECT_TRUE(m::resolveCliTool("retdec-no-such-tool-xyz").empty());
	EXPECT_TRUE(m::resolveCliTool("").empty());
}

TEST_F(CliToolPaths, MacBundleLayout)
{
	// <prefix>/RetDec.app/Contents/MacOS/retdec-gui, tools in <prefix>/bin, resources in Contents/share.
	const fs::path macos = tmp.path / "prefix" / "RetDec.app" / "Contents" / "MacOS";
	fs::create_directories(macos);
	touchExecutable(tmp.path / "prefix" / "bin" / exe("retdec-bin2pat"));
	fs::create_directories(tmp.path / "prefix" / "RetDec.app" / "Contents" / "share" / "retdec" / "gui");
	std::ofstream(tmp.path / "prefix" / "RetDec.app" / "Contents" / "share" / "retdec" / "gui" / "bundle.txt") << "x";
	fs::create_directories(tmp.path / "prefix" / "share" / "retdec" / "gui");
	std::ofstream(tmp.path / "prefix" / "share" / "retdec" / "gui" / "prefix-only.txt") << "y";
	m::setApplicationDirPathOverride(macos);

	const auto prefixes = m::installPrefixCandidates();
	ASSERT_EQ(prefixes.size(), 2u);
	EXPECT_EQ(prefixes[0], tmp.path / "prefix" / "RetDec.app" / "Contents");
	EXPECT_EQ(prefixes[1], tmp.path / "prefix");

	EXPECT_EQ(m::resolveRetdecBin2patExecutable(), tmp.path / "prefix" / "bin" / exe("retdec-bin2pat"));
	EXPECT_EQ(m::resolveGuiResourceDir(), tmp.path / "prefix" / "RetDec.app" / "Contents" / "share" / "retdec" / "gui");
	EXPECT_EQ(
		m::resolveGuiResource("bundle.txt"),
		tmp.path / "prefix" / "RetDec.app" / "Contents" / "share" / "retdec" / "gui" / "bundle.txt");
	EXPECT_EQ(m::resolveGuiResource("prefix-only.txt"), tmp.path / "prefix" / "share" / "retdec" / "gui" / "prefix-only.txt");
}

TEST_F(CliToolPaths, GuiResourcesFallBackToTheSourceTree)
{
	m::setApplicationDirPathOverride(tmp.path / "empty" / "bin");
	const fs::path passes = m::resolveGuiResource("llvm_passes_default.json");
	ASSERT_FALSE(passes.empty());
	EXPECT_EQ(passes.filename(), "llvm_passes_default.json");
	EXPECT_FALSE(m::resolveGuiResourceDir().empty());
	EXPECT_TRUE(m::resolveGuiResource("no-such-resource.json").empty());
}

TEST_F(CliToolPaths, InstalledScripts)
{
	const fs::path bin = tmp.path / "p" / "bin";
	fs::create_directories(bin);
	fs::create_directories(tmp.path / "p" / "share" / "retdec" / "scripts");
	std::ofstream(tmp.path / "p" / "share" / "retdec" / "scripts" / "retdec_export_intel.py") << "#";
	m::setApplicationDirPathOverride(bin);
	EXPECT_EQ(m::resolveRetdecExportIntelScript(), tmp.path / "p" / "share" / "retdec" / "scripts" / "retdec_export_intel.py");
	EXPECT_TRUE(m::resolveSignatureFromLibraryCreatorScript().empty());
}
