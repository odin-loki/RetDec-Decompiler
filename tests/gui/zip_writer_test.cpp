/**
 * @file tests/gui/zip_writer_test.cpp
 * @brief ZipWriter output checked against an independent reader (Python's zipfile).
 */

#include "retdec/gui/model/zip_writer.h"

#include <forge/os/fs.hpp>
#include <forge/os/process.hpp>

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
		path = fs::path(forge::os::fs::make_temp_dir("retdec-gui-zip-").value);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

std::string python()
{
	for (const char* name: {"python3", "python"})
	{
		std::string p = forge::os::find_executable(name);
		if (!p.empty()) return p;
	}
	return {};
}

/// Run Python's zipfile on @p zip: CRC check, then one "name<TAB>content\n" line
/// per entry, as UTF-8 bytes.
std::optional<std::string> pythonListZip(const fs::path& zip)
{
	const std::string py = python();
	if (py.empty()) return std::nullopt;
	forge::os::spawn_spec sp;
	sp.argv = {
		py,
		"-I",
		"-c",
		"import sys, zipfile\n"
		"z = zipfile.ZipFile(sys.argv[1])\n"
		"bad = z.testzip()\n"
		"if bad is not None: sys.exit('bad crc: ' + bad)\n"
		"for i in z.infolist():\n"
		"    out = i.filename + '\\t' + z.read(i).decode('utf-8') + '\\n'\n"
		// Raw UTF-8 bytes: text-mode stdout would turn '\n' into CRLF and
		// re-encode names in the console code page on Windows.
		"    sys.stdout.buffer.write(out.encode('utf-8'))\n",
		zip.string()};
	sp.timeout_ms = 60000;
	auto r = forge::os::spawn_wait(sp);
	if (!r || r.value.exit_code != 0)
	{
		ADD_FAILURE() << "python zipfile failed: " << (r ? r.value.err : r.err.what);
		return std::string{};
	}
	return r.value.out;
}

} // namespace

TEST(ZipWriter, Crc32KnownValues)
{
	EXPECT_EQ(m::ZipWriter::crc32(""), 0u);
	EXPECT_EQ(m::ZipWriter::crc32("123456789"), 0xCBF43926u);
	EXPECT_EQ(m::ZipWriter::crc32("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(ZipWriter, PythonReadsTheArchive)
{
	TempDir tmp;
	const fs::path disk = tmp.path / "on-disk.txt";
	{
		std::ofstream(disk, std::ios::binary) << "from disk";
	}
	const fs::path zipPath = tmp.path / "a.zip";
	{
		m::ZipWriter zip(zipPath);
		ASSERT_TRUE(zip.isOpen()) << zip.errorString();
		ASSERT_TRUE(zip.addEntry("hello.txt", "hello bundle").has_value());
		ASSERT_TRUE(zip.addEntry("dir\\nested.txt", "nested").has_value());
		ASSERT_TRUE(zip.addEntry("empty.txt", "").has_value());
		ASSERT_TRUE(zip.addEntry("ünïcode.txt", "utf8 name").has_value());
		ASSERT_TRUE(zip.addFile(disk, "copied.txt").has_value());
		const auto names = zip.entryNames();
		ASSERT_EQ(names.size(), 5u);
		EXPECT_EQ(names[1], "dir/nested.txt");
		ASSERT_TRUE(zip.close().has_value());
	}
	const auto listing = pythonListZip(zipPath);
	if (!listing) GTEST_SKIP() << "no Python interpreter on PATH";
	EXPECT_EQ(
		*listing,
		"hello.txt\thello bundle\n"
		"dir/nested.txt\tnested\n"
		"empty.txt\t\n"
		"ünïcode.txt\tutf8 name\n"
		"copied.txt\tfrom disk\n");
}

TEST(ZipWriter, EmptyArchiveIsValid)
{
	TempDir tmp;
	const fs::path zipPath = tmp.path / "empty.zip";
	{
		m::ZipWriter zip(zipPath);
		ASSERT_TRUE(zip.close().has_value());
	}
	EXPECT_EQ(fs::file_size(zipPath), 22u);
	const auto listing = pythonListZip(zipPath);
	if (!listing) GTEST_SKIP() << "no Python interpreter on PATH";
	EXPECT_EQ(*listing, "");
}

TEST(ZipWriter, DestructorFinalises)
{
	TempDir tmp;
	const fs::path zipPath = tmp.path / "auto.zip";
	{
		m::ZipWriter zip(zipPath);
		ASSERT_TRUE(zip.addEntry("x", "y").has_value());
	}
	const auto listing = pythonListZip(zipPath);
	if (!listing) GTEST_SKIP() << "no Python interpreter on PATH";
	EXPECT_EQ(*listing, "x\ty\n");
}

TEST(ZipWriter, RejectsBadNamesAndUseAfterClose)
{
	TempDir tmp;
	m::ZipWriter zip(tmp.path / "b.zip");
	ASSERT_TRUE(zip.isOpen());
	EXPECT_FALSE(zip.addEntry("", "x").has_value());
	EXPECT_FALSE(zip.addEntry("/abs", "x").has_value());
	EXPECT_FALSE(zip.addEntry("\\abs", "x").has_value());
	EXPECT_FALSE(zip.errorString().empty());
	EXPECT_FALSE(zip.addFile(tmp.path / "missing", "m").has_value());
	ASSERT_TRUE(zip.close().has_value());
	EXPECT_FALSE(zip.isOpen());
	EXPECT_FALSE(zip.addEntry("late", "x").has_value());
}

TEST(ZipWriter, OpenFailureIsReported)
{
	m::ZipWriter zip("/nonexistent-dir/x/y.zip");
	EXPECT_FALSE(zip.isOpen());
	EXPECT_FALSE(zip.errorString().empty());
	EXPECT_FALSE(zip.addEntry("a", "b").has_value());
	EXPECT_FALSE(zip.close().has_value());
}
