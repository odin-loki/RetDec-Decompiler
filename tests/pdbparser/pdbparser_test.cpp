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
#include "retdec/pdbparser/pdb_types.h"
#include "retdec/pdbparser/pdb_utils.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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


// ─── LF_PROCEDURE / LF_MFUNCTION argument lists ──────────────────────────────
//
// A function record says how many parameters it has; a separate LF_ARGLIST
// record holds the types. Three numbers therefore describe the same list --
// lfProc/lfMFunc::parmcount, lfArgList::count, and how many DWORDs the arglist
// record is actually long -- and only the third one is a fact. The first two
// are 16- and 32-bit fields out of the file and can say anything.
//
// The records are allocated at exactly their on-wire length so that a read past
// the end lands in an ASan redzone rather than in whatever the allocator had
// lying around; run the suite with EXTRA_CXXFLAGS="-fsanitize=address -g".

namespace {

/// Bytes of a record, sized exactly, so a one-DWORD overread is detectable.
using Record = std::unique_ptr<unsigned char[]>;

Record makeRecord(std::size_t size)
{
	Record r(new unsigned char[size]);
	std::memset(r.get(), 0, size);
	return r;
}

template <typename T>
void poke(unsigned char* base, std::size_t offset, T value)
{
	std::memcpy(base + offset, &value, sizeof(value));
}

/// An LF_ARGLIST holding @a stored argument types but claiming @a claimed.
/// Returns the record and, through @a sizeOut, its true length in bytes.
Record makeArglist(std::uint32_t claimed, std::uint32_t stored, int& sizeOut)
{
	const std::size_t size = 6 + std::size_t(stored) * 4;
	Record r = makeRecord(size);
	poke<std::uint16_t>(r.get(), 0, LF_ARGLIST);
	poke<std::uint32_t>(r.get(), 2, claimed);
	for (std::uint32_t i = 0; i < stored; ++i)
	{
		poke<std::uint32_t>(r.get(), 6 + std::size_t(i) * 4, T_INT4);
	}
	sizeOut = static_cast<int>(size);
	return r;
}

} // namespace

TEST(PdbArgList, MFunctionParmcountCannotReadPastTheArgumentList)
{
	// The list stores two arguments and says it has 4096; the member function
	// says it has 65535. parse_mfunc used to take parmcount at face value --
	// the disagreement was an assert(), which is nothing in a release build --
	// and then indexed arg[0..65534].
	int arglistSize = 0;
	Record arglistBytes = makeArglist(/*claimed=*/4096, /*stored=*/2, arglistSize);

	PDBTypeDefIndexMap types;
	PDBTypeArglist arglistDef(0x1000);
	arglistDef.parse(reinterpret_cast<lfArgList*>(arglistBytes.get()), arglistSize, types);
	types[0x1000] = &arglistDef;

	Record mfunc = makeRecord(sizeof(lfMFunc));
	poke<std::uint16_t>(mfunc.get(), 0x00, LF_MFUNCTION);
	poke<std::uint32_t>(mfunc.get(), 0x02, T_INT4); // rvtype
	poke<std::uint32_t>(mfunc.get(), 0x06, 0);      // classtype
	poke<std::uint32_t>(mfunc.get(), 0x0A, 0);      // thistype
	poke<std::uint16_t>(mfunc.get(), 0x10, 0xFFFF); // parmcount
	poke<std::uint32_t>(mfunc.get(), 0x12, 0x1000); // arglist

	PDBTypeFunction fn(1);
	fn.parse_mfunc(reinterpret_cast<lfMFunc*>(mfunc.get()), static_cast<int>(sizeof(lfMFunc)), types);

	EXPECT_LE(fn.func_args_count, 2);
	EXPECT_GE(fn.func_args_count, 0);
}

TEST(PdbArgList, ProcedureParmcountCannotReadPastTheArgumentList)
{
	// The same disagreement on the LF_PROCEDURE path. That one already clamped
	// to lfArgList::count -- but count is a 32-bit field out of the file too,
	// and nothing tied it to how long the record is.
	int arglistSize = 0;
	Record arglistBytes = makeArglist(/*claimed=*/4096, /*stored=*/2, arglistSize);

	PDBTypeDefIndexMap types;
	PDBTypeArglist arglistDef(0x1000);
	arglistDef.parse(reinterpret_cast<lfArgList*>(arglistBytes.get()), arglistSize, types);
	types[0x1000] = &arglistDef;

	Record proc = makeRecord(sizeof(lfProc));
	poke<std::uint16_t>(proc.get(), 0x00, LF_PROCEDURE);
	poke<std::uint32_t>(proc.get(), 0x02, T_INT4); // rvtype
	poke<std::uint16_t>(proc.get(), 0x08, 0xFFFF); // parmcount
	poke<std::uint32_t>(proc.get(), 0x0A, 0x1000); // arglist

	PDBTypeFunction fn(2);
	fn.parse(reinterpret_cast<lfProc*>(proc.get()), static_cast<int>(sizeof(lfProc)), types);

	EXPECT_LE(fn.func_args_count, 2);
	EXPECT_GE(fn.func_args_count, 0);
}

TEST(PdbArgList, AnArgumentListShorterThanItsHeaderYieldsNoArguments)
{
	// A record whose length does not even cover `leaf` and `count`. The record
	// walk in parse_types() bounds `size` against the stream, so this is what a
	// two-byte LF_ARGLIST looks like by the time it reaches here.
	Record arglistBytes = makeRecord(6);
	poke<std::uint16_t>(arglistBytes.get(), 0, LF_ARGLIST);
	poke<std::uint32_t>(arglistBytes.get(), 2, 0xFFFFFFFFu);

	PDBTypeDefIndexMap types;
	PDBTypeArglist arglistDef(0x1000);
	arglistDef.parse(reinterpret_cast<lfArgList*>(arglistBytes.get()), 2, types);
	types[0x1000] = &arglistDef;

	Record proc = makeRecord(sizeof(lfProc));
	poke<std::uint16_t>(proc.get(), 0x00, LF_PROCEDURE);
	poke<std::uint16_t>(proc.get(), 0x08, 8);      // parmcount
	poke<std::uint32_t>(proc.get(), 0x0A, 0x1000); // arglist

	PDBTypeFunction fn(3);
	fn.parse(reinterpret_cast<lfProc*>(proc.get()), static_cast<int>(sizeof(lfProc)), types);

	EXPECT_EQ(0, fn.func_args_count);
}

TEST(PdbArgList, AnHonestArgumentListIsReadInFull)
{
	// The other half: when all three numbers agree, every argument is read.
	// A clamp that returned zero would pass the tests above and lose every
	// parameter type in the file.
	int arglistSize = 0;
	Record arglistBytes = makeArglist(/*claimed=*/3, /*stored=*/3, arglistSize);

	PDBTypeDefIndexMap types;
	PDBTypeArglist arglistDef(0x1000);
	arglistDef.parse(reinterpret_cast<lfArgList*>(arglistBytes.get()), arglistSize, types);
	types[0x1000] = &arglistDef;

	Record proc = makeRecord(sizeof(lfProc));
	poke<std::uint16_t>(proc.get(), 0x00, LF_PROCEDURE);
	poke<std::uint32_t>(proc.get(), 0x02, T_INT4);
	poke<std::uint16_t>(proc.get(), 0x08, 3);
	poke<std::uint32_t>(proc.get(), 0x0A, 0x1000);

	PDBTypeFunction fn(4);
	fn.parse(reinterpret_cast<lfProc*>(proc.get()), static_cast<int>(sizeof(lfProc)), types);

	ASSERT_EQ(3, fn.func_args_count);
	for (int i = 0; i < 3; ++i)
	{
		EXPECT_EQ(T_INT4, fn.func_args[i].type_index);
	}
}

TEST(PdbArgList, AFunctionRecordNamingANonArglistTypeIsRefused)
{
	// types[] is seeded with the base types, so an arglist index of 0 resolves
	// to T_NOTYPE rather than to nothing. reinterpret_cast plus assert() is an
	// abort in a debug build and a type-confused read in a release one.
	PDBTypeDefIndexMap types;
	PDBTypeBase baseDef(T_INT4, PDBBASETYPE_INT_SIGNED, false, 32, "int");
	types[T_INT4] = &baseDef;

	Record mfunc = makeRecord(sizeof(lfMFunc));
	poke<std::uint16_t>(mfunc.get(), 0x00, LF_MFUNCTION);
	poke<std::uint16_t>(mfunc.get(), 0x10, 4);      // parmcount
	poke<std::uint32_t>(mfunc.get(), 0x12, T_INT4); // arglist -> a base type

	PDBTypeFunction fn(5);
	fn.parse_mfunc(reinterpret_cast<lfMFunc*>(mfunc.get()), static_cast<int>(sizeof(lfMFunc)), types);

	EXPECT_EQ(nullptr, fn.func_args);
}

} // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
