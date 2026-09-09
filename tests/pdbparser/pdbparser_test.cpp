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

// The smallest MSF 7.00 file PDBFile::load_pdb_file accepts.
//
// LoadingTwiceReportsAlreadyLoaded used to feed the loader 512 bytes of 0x41 --
// not a PDB -- and put its only assertion behind `if (first == PDB_STATE_OK)`,
// so the branch was never entered and the second load was never tested at all.
//
// Five 512-byte pages: superblock, free page map, one page of stream data, the
// stream directory, and the page listing the directory's pages. Two streams:
// the old-directory stream, which is empty, and the PDB info stream, which the
// loader dereferences after a successful load.
static std::vector<uint8_t> minimalPdb700()
{
	constexpr uint32_t kPage = 512;
	constexpr uint32_t kPages = 5;
	constexpr uint32_t kInfoPage = 2;
	constexpr uint32_t kDirPage = 3;
	constexpr uint32_t kDirIndexPage = 4;

	std::vector<uint8_t> f(static_cast<std::size_t>(kPage) * kPages, 0);
	auto put32 = [&f](std::size_t off, uint32_t v) {
		f[off + 0] = static_cast<uint8_t>(v);
		f[off + 1] = static_cast<uint8_t>(v >> 8);
		f[off + 2] = static_cast<uint8_t>(v >> 16);
		f[off + 3] = static_cast<uint8_t>(v >> 24);
	};

	// Superblock: the signature PDB_SIGNATURE_700 then the six header dwords.
	static const char kSig[] = "Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\x00\x00\x00";
	std::memcpy(f.data(), kSig, 32);
	put32(32, kPage);         // dBytesPerPage
	put32(36, 1);             // dFlagPage
	put32(40, kPages);        // dNumPages -- must match the file size exactly
	put32(44, 4 * 4);         // dRootSize: numStreams + two sizes + one page
	put32(48, 0);             // dReserved
	put32(52, kDirIndexPage); // dRootIndexesPage

	// The page listing the directory's own pages.
	put32(static_cast<std::size_t>(kDirIndexPage) * kPage, kDirPage);

	// The directory: [numStreams][size0][size1][page of stream 1].
	const std::size_t dir = static_cast<std::size_t>(kDirPage) * kPage;
	put32(dir + 0, 2);  // two streams
	put32(dir + 4, 0);  // stream 0 (old directory) is empty
	put32(dir + 8, 28); // stream 1 (PDB info) occupies one page
	put32(dir + 12, kInfoPage);

	// PDB info stream: version, signature, age, and a GUID.
	const std::size_t info = static_cast<std::size_t>(kInfoPage) * kPage;
	put32(info + 0, 20000404); // VC70
	put32(info + 4, 0);        // signature
	put32(info + 8, 1);        // age
	for (int i = 0; i < 16; ++i)
		f[info + 12 + static_cast<std::size_t>(i)] = static_cast<uint8_t>(i);

	return f;
}

TEST(PdbFile, MinimalFileLoads)
{
	// Pins the fixture the test below depends on: if this stops loading, that
	// test must fail rather than quietly stop testing anything.
	TempFile f(minimalPdb700());
	PDBFile pdb;
	EXPECT_EQ(PDB_STATE_OK, pdb.load_pdb_file(f.path()));
}

TEST(PdbFile, LoadingTwiceReportsAlreadyLoaded)
{
	TempFile f(minimalPdb700());
	PDBFile pdb;
	ASSERT_EQ(PDB_STATE_OK, pdb.load_pdb_file(f.path()));
	EXPECT_EQ(PDB_STATE_ALREADY_LOADED, pdb.load_pdb_file(f.path()));
}

TEST(PdbFile, ReloadingARefusedFileRetriesRatherThanReportingAlreadyLoaded)
{
	// The other half of the state machine: a load that failed must not leave
	// the object looking loaded.
	std::vector<uint8_t> junk(512, 0x41);
	TempFile f(junk);
	PDBFile pdb;
	const PDBFileState first = pdb.load_pdb_file(f.path());
	ASSERT_NE(PDB_STATE_OK, first);
	EXPECT_EQ(first, pdb.load_pdb_file(f.path()));
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

/// The name field of a type record, whichever way it is stored.
inline std::string nameOf(const char* n)
{
	return n ? std::string(n) : std::string();
}
inline std::string nameOf(const std::string& n)
{
	return n;
}

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

// LF_STRUCTURE, LF_UNION and LF_ENUM cast `record->field` -- a type index
// straight out of the file -- to a PDBTypeFieldList with no type_class check.
// types[] is pre-seeded with PDBTypeBase objects at the CodeView base-type
// indices, so a non-field index is non-null and passed the guard that was
// there. Reading a PDBTypeBase as a PDBTypeFieldList takes its
// is_pointer/size_bits/description bytes as the `fields` vector: size() came
// back huge and fields[i] dereferenced a fabricated pointer.
TEST(PdbFieldList, AStructNamingANonFieldListTypeCopiesNoMembers)
{
	PDBTypeDefIndexMap types;
	PDBTypeBase baseDef(T_INT4, PDBBASETYPE_INT_SIGNED, false, 32, "int");
	types[T_INT4] = &baseDef;

	Record rec = makeRecord(sizeof(lfStructure) + 8);
	poke<std::uint16_t>(rec.get(), 0x00, LF_STRUCTURE);
	poke<std::uint16_t>(rec.get(), 0x02, 2);      // count
	poke<std::uint32_t>(rec.get(), 0x08, T_INT4); // field -> a base type

	PDBTypeStruct st(10);
	st.parse(reinterpret_cast<lfStructure*>(rec.get()), static_cast<int>(sizeof(lfStructure) + 8), types);

	EXPECT_TRUE(st.struct_members.empty());
}

TEST(PdbFieldList, AUnionNamingANonFieldListTypeCopiesNoMembers)
{
	PDBTypeDefIndexMap types;
	PDBTypeBase baseDef(T_INT4, PDBBASETYPE_INT_SIGNED, false, 32, "int");
	types[T_INT4] = &baseDef;

	Record rec = makeRecord(sizeof(lfUnion) + 8);
	poke<std::uint16_t>(rec.get(), 0x00, LF_UNION);
	poke<std::uint16_t>(rec.get(), 0x02, 2);      // count
	poke<std::uint32_t>(rec.get(), 0x08, T_INT4); // field -> a base type

	PDBTypeUnion un(11);
	un.parse(reinterpret_cast<lfUnion*>(rec.get()), static_cast<int>(sizeof(lfUnion) + 8), types);

	EXPECT_TRUE(un.union_members.empty());
}

// The name a union or class carries is a bare char* into the TPI stream:
// RecordValue returns nullptr for any leaf word >= LF_NUMERIC it does not
// decode, and what it does return has no terminator guaranteed. Assigning it
// to a std::string -- which pdb_types.cpp does, as a types_byname key -- was
// std::logic_error and terminate on the null, and a strlen past the end of the
// stream buffer on the unterminated one. The record's declared size is the
// bound.
TEST(PdbFieldList, AnUnterminatedUnionNameStopsAtTheRecordEnd)
{
	PDBTypeDefIndexMap types;

	// A record whose name field runs to the very last byte with no NUL.
	const std::size_t size = sizeof(lfUnion) + 4;
	Record rec = makeRecord(size);
	poke<std::uint16_t>(rec.get(), 0x00, LF_UNION);
	poke<std::uint16_t>(rec.get(), 0x02, 0); // count
	poke<std::uint32_t>(rec.get(), 0x08, 0); // field = none
	// data[] holds a size leaf then the name; a small leaf value is its own
	// size, so the four bytes after it are all name.
	poke<std::uint16_t>(rec.get(), sizeof(lfUnion) - 4, 0);
	std::memset(rec.get() + size - 4, 'A', 4);

	PDBTypeUnion un(12);
	un.parse(reinterpret_cast<lfUnion*>(rec.get()), static_cast<int>(size), types);

	// Whatever it read, it stopped inside the record. nameOf() reads the field
	// the way the code under test leaves it: a bare char* before the fix, a
	// std::string after -- so this test compiles against both, and the char*
	// form is the strlen past the end of the record that the fix removes.
	EXPECT_LE(nameOf(un.union_name).size(), size);
}

} // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
