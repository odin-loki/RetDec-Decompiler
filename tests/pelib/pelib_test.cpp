/**
 * @file tests/pelib/pelib_test.cpp
 * @brief Unit tests for the vendored PeLib parsers.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * pelib is 9,791 lines that read an untrusted PE. It is compiled on every run
 * of the dependency-free check and fuzzed by tests/managed_integration/fuzz/,
 * and until now nothing asserted anything about what it produces -- only that
 * it does not crash. UNTESTED_MODULES in scripts/standalone_check.sh recorded
 * that as a decision.
 *
 * It stopped being one when the warning gate reported
 *
 *     src/pelib/SecurityDirectory.cpp:117:14: warning: assigning field to
 *     itself [-Wself-assign-field]
 *
 * on `this->size = size;`, which meant SecurityDirectory::getSize() returned
 * zero for every file ever parsed. A fuzzer cannot see that; an assertion can.
 *
 * The directories here take an std::istream and an offset and size, so they can
 * be driven from a stringstream without constructing a whole PE.
 */

#include "retdec/pelib/PeLibAux.h"
#include "retdec/pelib/SecurityDirectory.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace PeLib;

namespace {

void put16(std::string& s, std::uint16_t v)
{
	s.push_back(static_cast<char>(v & 0xFF));
	s.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put32(std::string& s, std::uint32_t v)
{
	put16(s, static_cast<std::uint16_t>(v & 0xFFFF));
	put16(s, static_cast<std::uint16_t>((v >> 16) & 0xFFFF));
}

/// One WIN_CERTIFICATE: Length, Revision, CertificateType, then Length-8 bytes.
/// @a declaredLength overrides the Length field so a lying one can be built.
std::string certificate(std::uint32_t payloadBytes, std::uint32_t declaredLength = 0)
{
	std::string s;
	put32(s, declaredLength ? declaredLength : 8 + payloadBytes);
	put16(s, PELIB_WIN_CERT_REVISION_2_0);
	put16(s, PELIB_WIN_CERT_TYPE_PKCS_SIGNED_DATA);
	for (std::uint32_t i = 0; i < payloadBytes; ++i)
		s.push_back(static_cast<char>('A' + (i % 26)));
	return s;
}

/// A stream with @a lead filler bytes and then @a body, so the directory sits
/// at a non-zero offset exactly as it does in a real file.
std::stringstream streamAt(std::size_t lead, const std::string& body)
{
	std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
	ss << std::string(lead, '\xCC') << body;
	return ss;
}

} // namespace

TEST(SecurityDirectoryTests, TheDirectorySizeIsTheSizeItWasRead)
{
	// `this->size = size;` assigned the member to itself, so this was zero for
	// every signed file RetDec has ever parsed. PeFormat::loadCertificates uses
	// it to decide whether the security directory overlaps a section, which is
	// how Windows decides whether to ignore the certificates.
	const std::string dir = certificate(/*payloadBytes=*/24);
	ASSERT_EQ(32u, dir.size());
	std::stringstream ss = streamAt(0x200, dir);

	SecurityDirectory sec;
	ASSERT_EQ(ERROR_NONE, sec.read(ss, 0x200, static_cast<unsigned int>(dir.size())));

	EXPECT_EQ(0x200u, sec.getOffset());
	EXPECT_EQ(32u, sec.getSize());
	ASSERT_EQ(1u, sec.calcNumberOfCertificates());
	EXPECT_EQ(24u, sec.getCertificate(0).size());
}

TEST(SecurityDirectoryTests, TwoCertificatesAreBothRead)
{
	const std::string dir = certificate(8) + certificate(16);
	std::stringstream ss = streamAt(0, dir);

	SecurityDirectory sec;
	ASSERT_EQ(ERROR_NONE, sec.read(ss, 0, static_cast<unsigned int>(dir.size())));

	EXPECT_EQ(dir.size(), sec.getSize());
	ASSERT_EQ(2u, sec.calcNumberOfCertificates());
	EXPECT_EQ(8u, sec.getCertificate(0).size());
	EXPECT_EQ(16u, sec.getCertificate(1).size());
}

TEST(SecurityDirectoryTests, ACertificateLongerThanTheDirectoryIsRefused)
{
	// The WIN_CERTIFICATE Length field is a uint32 straight out of the file and
	// the resize below it trusted it: a 1,040-byte PE declaring Length
	// 0x80000000 took the process from 4.2 MB to 4.2 GB of RSS.
	const std::string dir = certificate(/*payloadBytes=*/8, /*declaredLength=*/0x80000000u);
	std::stringstream ss = streamAt(0, dir);

	SecurityDirectory sec;
	EXPECT_EQ(ERROR_INVALID_FILE, sec.read(ss, 0, static_cast<unsigned int>(dir.size())));
	EXPECT_EQ(LDR_ERROR_DIGITAL_SIGNATURE_CUT, sec.loaderError());
	EXPECT_EQ(0u, sec.calcNumberOfCertificates());
}

TEST(SecurityDirectoryTests, ADirectoryRangeThatWrapsIsRefused)
{
	// uiOffset and uiSize are 32-bit and come from a data directory, so
	// `uiOffset + uiSize` wraps: 0xFFFFFF00 + 0x200 is 0x100, which passes a
	// naive "does it fit" test while naming a range far outside the file.
	std::stringstream ss = streamAt(0, certificate(8));

	SecurityDirectory sec;
	EXPECT_EQ(ERROR_INVALID_FILE, sec.read(ss, 0xFFFFFF00u, 0x200u));
	EXPECT_EQ(LDR_ERROR_DIGITAL_SIGNATURE_CUT, sec.loaderError());
}

TEST(SecurityDirectoryTests, AnAllZeroDirectoryIsRefused)
{
	std::stringstream ss = streamAt(0, std::string(64, '\0'));

	SecurityDirectory sec;
	EXPECT_EQ(ERROR_INVALID_FILE, sec.read(ss, 0, 64));
	EXPECT_EQ(LDR_ERROR_DIGITAL_SIGNATURE_ZEROED, sec.loaderError());
}

TEST(SecurityDirectoryTests, AnUnknownRevisionOrTypeIsRefused)
{
	std::string dir;
	put32(dir, 16);
	put16(dir, 0x0300); // not WIN_CERT_REVISION_1_0 or _2_0
	put16(dir, PELIB_WIN_CERT_TYPE_PKCS_SIGNED_DATA);
	dir.append(8, 'x');
	std::stringstream ss = streamAt(0, dir);

	SecurityDirectory sec;
	EXPECT_EQ(ERROR_INVALID_FILE, sec.read(ss, 0, static_cast<unsigned int>(dir.size())));
}
