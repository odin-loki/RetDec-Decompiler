/**
 * @file tests/fileformat/ar_archive_format_probe_tests.cpp
 * @brief Tests for @c probeArArchiveMemberFormats (AR member lattice probe).
 * @copyright (c) 2026, MIT license
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retdec/fileformat/utils/ar_archive_format_probe.h"
#include "retdec/utils/os.h"
#include "fileformat/fileformat_tests.h"

using namespace ::testing;

namespace retdec {
namespace fileformat {
namespace tests {

namespace
{

std::vector<std::uint8_t> makeGnuArWithMember(
		const std::vector<std::uint8_t> &body,
		const char *shortName)
{
	std::vector<std::uint8_t> out;
	static const char mag[] = "!<arch>\n";
	out.insert(out.end(), mag, mag + sizeof(mag) - 1);

	std::string n = shortName ? shortName : "a.o";
	if (n.size() > 16)
	{
		n.resize(16);
	}
	n.resize(16, ' ');

	char hdr[60];
	std::memset(hdr, ' ', sizeof(hdr));
	std::memcpy(hdr, n.data(), 16);
	std::memcpy(hdr + 28, "0     ", 6);
	std::memcpy(hdr + 34, "0     ", 6);
	std::memcpy(hdr + 40, "100644  ", 8);
	{
		char szb[32];
		std::snprintf(
				szb,
				sizeof(szb),
				"%-10zu",
				static_cast<std::size_t>(body.size()));
		std::memcpy(hdr + 48, szb, 10);
	}
	hdr[58] = '`';
	hdr[59] = '\n';

	out.insert(out.end(), hdr, hdr + sizeof(hdr));
	out.insert(out.end(), body.begin(), body.end());
	if ((body.size() % 2) == 1)
	{
		out.push_back('\n');
	}
	return out;
}

void appendGnuArMember(
		std::vector<std::uint8_t> &ar,
		const std::vector<std::uint8_t> &body,
		const char *shortName)
{
	std::string n = shortName ? shortName : "a.o";
	if (n.size() > 16)
	{
		n.resize(16);
	}
	n.resize(16, ' ');

	char hdr[60];
	std::memset(hdr, ' ', sizeof(hdr));
	std::memcpy(hdr, n.data(), 16);
	std::memcpy(hdr + 28, "0     ", 6);
	std::memcpy(hdr + 34, "0     ", 6);
	std::memcpy(hdr + 40, "100644  ", 8);
	{
		char szb[32];
		std::snprintf(
				szb,
				sizeof(szb),
				"%-10zu",
				static_cast<std::size_t>(body.size()));
		std::memcpy(hdr + 48, szb, 10);
	}
	hdr[58] = '`';
	hdr[59] = '\n';

	ar.insert(ar.end(), hdr, hdr + sizeof(hdr));
	ar.insert(ar.end(), body.begin(), body.end());
	if ((body.size() % 2) == 1)
	{
		ar.push_back('\n');
	}
}

void expectLatticeHintsEqual(const FormatLatticeHints &a, const FormatLatticeHints &b)
{
	EXPECT_EQ(a.elfStrength, b.elfStrength);
	EXPECT_EQ(a.peStrength, b.peStrength);
	EXPECT_EQ(a.ihexStrength, b.ihexStrength);
	EXPECT_EQ(a.machoSliceStrength, b.machoSliceStrength);
	EXPECT_EQ(a.coffStrength, b.coffStrength);
	EXPECT_EQ(a.arArchiveStrength, b.arArchiveStrength);
	EXPECT_EQ(a.cafeBabeSecondWord, b.cafeBabeSecondWord);
	EXPECT_EQ(a.javaClassLatticeStrength, b.javaClassLatticeStrength);
	EXPECT_EQ(a.machoFatLatticeStrength, b.machoFatLatticeStrength);
}

void expectProbeSummariesEqual(const ArArchiveFormatProbeSummary &a, const ArArchiveFormatProbeSummary &b)
{
	ASSERT_EQ(a.members.size(), b.members.size());
	EXPECT_EQ(a.dominantFormatHistogram, b.dominantFormatHistogram);
	for (std::size_t i = 0; i < a.members.size(); ++i)
	{
		EXPECT_EQ(a.members[i].name, b.members[i].name);
		EXPECT_EQ(a.members[i].rawSize, b.members[i].rawSize);
		EXPECT_EQ(a.members[i].bufferOk, b.members[i].bufferOk);
		EXPECT_EQ(a.members[i].dominantFormat, b.members[i].dominantFormat);
		expectLatticeHintsEqual(a.members[i].hints, b.members[i].hints);
	}
}

/**
 * RAII for @c RETDEC_FORMAT_AR_PROBE_THREADS (parallel lattice workers).
 */
struct ScopedArProbeThreadsEnv
{
	std::string prev;
	bool had = false;

	explicit ScopedArProbeThreadsEnv(const char *value)
	{
		const char *e = std::getenv("RETDEC_FORMAT_AR_PROBE_THREADS");
		if (e != nullptr)
		{
			had = true;
			prev = e;
		}
#ifdef OS_WINDOWS
		const std::string assign = std::string("RETDEC_FORMAT_AR_PROBE_THREADS=") + value;
		_putenv(assign.c_str());
#else
		setenv("RETDEC_FORMAT_AR_PROBE_THREADS", value, 1);
#endif
	}

	~ScopedArProbeThreadsEnv()
	{
#ifdef OS_WINDOWS
		if (had)
		{
			_putenv((std::string("RETDEC_FORMAT_AR_PROBE_THREADS=") + prev).c_str());
		}
		else
		{
			_putenv("RETDEC_FORMAT_AR_PROBE_THREADS=");
		}
#else
		if (had)
		{
			setenv("RETDEC_FORMAT_AR_PROBE_THREADS", prev.c_str(), 1);
		}
		else
		{
			unsetenv("RETDEC_FORMAT_AR_PROBE_THREADS");
		}
#endif
	}
};

} // namespace

TEST(ArArchiveFormatProbe, RejectsNonArchive)
{
	ArArchiveFormatProbeSummary s;
	const std::vector<std::uint8_t> junk = {0, 1, 2, 3};
	EXPECT_FALSE(probeArArchiveMemberFormats(junk.data(), junk.size(), s));
	EXPECT_FALSE(s.parseOk);
}

TEST(ArArchiveFormatProbe, TruncatedArchiveKeepsMagicFlagsForDiagnostics)
{
	std::vector<std::uint8_t> ar = makeGnuArWithMember(elfBytes, "trunc.o");
	ASSERT_GT(ar.size(), 32u);
	ar.resize(ar.size() - 24); // break member layout; magic still valid at 0

	ArArchiveFormatProbeSummary s;
	(void)probeArArchiveMemberFormats(ar.data(), ar.size(), s);
	EXPECT_TRUE(s.isArchive);
	EXPECT_FALSE(s.isThinArchive);
	// LLVM may or may not accept a partial archive; either way magic was recognized.
}

TEST(ArArchiveFormatProbe, MagicOnlyParsesAsEmptyArchive)
{
	ArArchiveFormatProbeSummary s;
	const std::vector<std::uint8_t> mag = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
	// LLVM accepts magic-only as a valid (empty) archive.
	ASSERT_TRUE(probeArArchiveMemberFormats(mag.data(), mag.size(), s));
	EXPECT_TRUE(s.parseOk);
	EXPECT_EQ(0u, s.membersSeen);
	EXPECT_TRUE(s.members.empty());
}

TEST(ArArchiveFormatProbe, SingleMemberElfMagic)
{
	const std::vector<std::uint8_t> body = {0x7F, 'E', 'L', 'F'};
	std::vector<std::uint8_t> ar = makeGnuArWithMember(body, "tiny.o");

	ArArchiveFormatProbeSummary s;
	ASSERT_TRUE(probeArArchiveMemberFormats(ar.data(), ar.size(), s));
	EXPECT_TRUE(s.parseOk);
	EXPECT_TRUE(s.isArchive);
	EXPECT_FALSE(s.isThinArchive);
	ASSERT_EQ(1u, s.membersSeen);
	ASSERT_EQ(1u, s.members.size());
	EXPECT_TRUE(s.members[0].bufferOk);
	EXPECT_EQ(Format::ELF, s.members[0].dominantFormat);
	EXPECT_EQ(100u, s.members[0].hints.elfStrength);
	EXPECT_GE(s.dominantFormatHistogram[static_cast<std::size_t>(Format::ELF)], 1u);
}

TEST(ArArchiveFormatProbe, FullElfFixtureMember)
{
	std::vector<std::uint8_t> ar = makeGnuArWithMember(elfBytes, "real_elf.o");

	ArArchiveFormatProbeSummary s;
	ASSERT_TRUE(probeArArchiveMemberFormats(ar.data(), ar.size(), s));
	EXPECT_TRUE(s.parseOk);
	ASSERT_EQ(1u, s.membersSeen);
	ASSERT_EQ(1u, s.membersProbed);
	ASSERT_FALSE(s.members.empty());
	EXPECT_EQ(Format::ELF, s.members[0].dominantFormat);
	EXPECT_EQ(100u, s.members[0].hints.elfStrength);
}

TEST(ArArchiveFormatProbe, LatticeProbeWorkerCountOneWhenEnvIsSequential)
{
	ScopedArProbeThreadsEnv guard("1");
	const std::vector<std::uint8_t> body = {0x7F, 'E', 'L', 'F'};
	std::vector<std::uint8_t> ar = makeGnuArWithMember(body, "tiny.o");

	ArArchiveFormatProbeSummary s;
	ASSERT_TRUE(probeArArchiveMemberFormats(ar.data(), ar.size(), s));
	EXPECT_EQ(1u, s.latticeProbeWorkerCount);
}

TEST(ArArchiveFormatProbe, TruncatedAtMemberCapWhenMoreMembersExist)
{
	const std::vector<std::uint8_t> elfBody = {0x7F, 'E', 'L', 'F'};
	std::vector<std::uint8_t> ar = makeGnuArWithMember(elfBody, "a.o");
	appendGnuArMember(ar, elfBody, "b.o");

	ArArchiveFormatProbeSummary s;
	ASSERT_TRUE(probeArArchiveMemberFormats(ar.data(), ar.size(), s, 1));
	EXPECT_TRUE(s.truncatedAtMemberCap);
	EXPECT_EQ(1u, s.membersSeen);
	ASSERT_EQ(1u, s.members.size());
}

TEST(ArArchiveFormatProbe, ParallelLatticeMatchesSequentialMultiMember)
{
	const std::vector<std::uint8_t> elfBody = {0x7F, 'E', 'L', 'F', 1, 2, 3, 4};
	std::vector<std::uint8_t> ar = makeGnuArWithMember(elfBody, "m0.o");
	appendGnuArMember(ar, elfBody, "m1.o");
	appendGnuArMember(ar, elfBody, "m2.o");
	appendGnuArMember(ar, elfBody, "m3.o");

	ArArchiveFormatProbeSummary seq;
	{
		ScopedArProbeThreadsEnv guard("1");
		ASSERT_TRUE(probeArArchiveMemberFormats(ar.data(), ar.size(), seq));
	}
	EXPECT_EQ(1u, seq.latticeProbeWorkerCount);
	ASSERT_EQ(4u, seq.members.size());

	ArArchiveFormatProbeSummary par;
	{
		ScopedArProbeThreadsEnv guard("8");
		ASSERT_TRUE(probeArArchiveMemberFormats(ar.data(), ar.size(), par));
	}
	EXPECT_GE(par.latticeProbeWorkerCount, 2u);
	expectProbeSummariesEqual(seq, par);
}

} // namespace tests
} // namespace fileformat
} // namespace retdec
