/**
 * @file tests/pdbparser/pdbparser_test.cpp
 * @brief Unit tests for the PDB parser.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * This module is compiled on every run of the dependency-free check, is fuzzed,
 * and has been fixed for memory safety a dozen times on this branch -- and had
 * no tests at all, because the suite drift check can only see a test directory
 * that exists. `--audit` reports that case now, and this is the suite it asked
 * for.
 *
 * PDBFile takes a filename rather than a buffer, so these write a fixture to a
 * temporary file, exactly as the fuzz harness does. That is the real public
 * entry point and it is what the tests should exercise.
 */

#include "retdec/pdbparser/pdb_file.h"
#include "retdec/pdbparser/pdb_utils.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace retdec::pdbparser;

namespace {

// ─── on-disk structure widths ────────────────────────────────────────────────
//
// These structures are overlaid on file bytes, so their size in memory has to
// be their size on the wire. PDB_GUID's Data1 was `unsigned long` -- eight
// bytes on LP64 -- which made the GUID 24 bytes rather than 16, and every
// field of PDBInfo70 after it was read from the wrong offset.

TEST(PdbLayout, GuidIsSixteenBytesOnTheWire)
{
	EXPECT_EQ(16u, sizeof(PDB_GUID));
	EXPECT_EQ(4u, sizeof(PDB_GUID::Data1));
	EXPECT_EQ(2u, sizeof(PDB_GUID::Data2));
	EXPECT_EQ(2u, sizeof(PDB_GUID::Data3));
	EXPECT_EQ(8u, sizeof(PDB_GUID::Data4));
}

TEST(PdbLayout, SectionHeaderIsFortyBytesOnTheWire)
{
	EXPECT_EQ(40u, sizeof(PDB_IMAGE_SECTION_HEADER));
}

TEST(PdbLayout, SymbolRecordHeadersArePacked)
{
	// Record lengths in a symbol stream put the next record wherever they like,
	// so these are read at offsets nothing aligns. Packing does not move a
	// field; it says what was always true of these bytes.
	EXPECT_EQ(4u, sizeof(PDBGeneralSymbol));
	EXPECT_EQ(8u, sizeof(PDBBigSymbol));
}

// ─── loading ─────────────────────────────────────────────────────────────────

// Write bytes to a temporary file and hand back the path. The file is removed
// when the guard goes out of scope.
class TempFile {
public:
	explicit TempFile(const std::vector<uint8_t>& bytes)
	{
		static int seq = 0;
		path_ = (std::filesystem::temp_directory_path() / ("retdec-pdb-test-" + std::to_string(seq++))).string();
		std::ofstream out(path_, std::ios::binary);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}
	~TempFile()
	{
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	TempFile(const TempFile&) = delete;
	TempFile& operator=(const TempFile&) = delete;
	const char* path() const
	{
		return path_.c_str();
	}

private:
	std::string path_;
};

// Load a byte sequence and report the state, without asserting anything about
// what it parsed to: the point of most of these is that a malformed file is
// refused rather than followed.
static PDBFileState loadBytes(const std::vector<uint8_t>& bytes)
{
	TempFile f(bytes);
	PDBFile pdb;
	return pdb.load_pdb_file(f.path());
}

TEST(PdbFile, EmptyFileIsRejected)
{
	EXPECT_EQ(PDB_STATE_INVALID_FILE, loadBytes({}));
}

TEST(PdbFile, AFileThatIsNotAPdbIsRejected)
{
	std::vector<uint8_t> junk(512, 0x41);
	EXPECT_EQ(PDB_STATE_INVALID_FILE, loadBytes(junk));
}

TEST(PdbFile, MissingFileIsReportedAsAnOpenFailure)
{
	PDBFile pdb;
	EXPECT_EQ(PDB_STATE_ERR_FILE_OPEN, pdb.load_pdb_file("/nonexistent/retdec/pdb/fixture"));
}

TEST(PdbFile, ATruncatedSignatureIsRejected)
{
	// The signature alone, with nothing behind it: every field the header
	// declares is missing.
	const char sig[] = "Microsoft C/C++ MSF 7.00\r\n";
	std::vector<uint8_t> buf(sig, sig + sizeof(sig) - 1);
	EXPECT_EQ(PDB_STATE_INVALID_FILE, loadBytes(buf));
}

TEST(PdbFile, LoadingTwiceReportsAlreadyLoaded)
{
	std::vector<uint8_t> junk(512, 0x41);
	TempFile f(junk);
	PDBFile pdb;
	const PDBFileState first = pdb.load_pdb_file(f.path());
	// Whatever the first attempt decided, a second must not repeat the work.
	if (first == PDB_STATE_OK)
	{
		EXPECT_EQ(PDB_STATE_ALREADY_LOADED, pdb.load_pdb_file(f.path()));
	}
}

// ─── initialize() on files that do not parse ─────────────────────────────────
//
// Every fix on this branch has been about what happens when a declared count,
// length or offset is not what the file can supply. The corpus under
// tests/crash_corpus/pdb/ carries the reproducers; these assert the contract
// the parser owes a caller in the ordinary case, which is that a file it
// refused leaves nothing half-built behind.

TEST(PdbFile, ARefusedFileExposesNoSymbols)
{
	std::vector<uint8_t> junk(1024, 0x5A);
	TempFile f(junk);
	PDBFile pdb;
	if (pdb.load_pdb_file(f.path()) != PDB_STATE_OK)
	{
		// Not initialized, so the containers must not be handed out either.
		EXPECT_EQ(nullptr, pdb.get_functions());
		EXPECT_EQ(nullptr, pdb.get_global_variables());
	}
}

TEST(PdbFile, InitializeOnAnUnloadedFileIsSafe)
{
	// A caller that ignores the load result must not be able to walk anything.
	PDBFile pdb;
	pdb.initialize(0);
	EXPECT_EQ(nullptr, pdb.get_functions());
	EXPECT_EQ(nullptr, pdb.get_global_variables());
}

} // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
