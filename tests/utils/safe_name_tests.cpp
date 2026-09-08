/**
 * @file tests/utils/safe_name_tests.cpp
 * @brief Tests for the @c safe_name module.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * src/ar-extractor/archive_wrapper.cpp joins an archive member name to an
 * output directory and opens the result for writing. The sanitiser it used to
 * carry lived in an anonymous namespace in that file, which needs LLVM, so no
 * test in this tree could reach it -- and it kept BACKSLASH in its allow-list
 * for as long as that was true.
 *
 * tests/verification/safe_name_proof.cpp proves the general statements over
 * symbolic input. What is here is the thing the proofs cannot be: the actual
 * names, spelled the way an archive would spell them, joined to a directory
 * the way the extractor joins them, so the assertion is about the path that
 * would be opened rather than about a predicate.
 */

#include <string>

#include <gtest/gtest.h>

#include "retdec/utils/safe_name.h"

using namespace ::testing;

namespace retdec {
namespace utils {
namespace tests {

namespace {

/**
 * The extractor's own composition, in one place: sanitise in place, substitute
 * the placeholder when nothing usable is left. ArchiveWrapper::fixName is
 * these five lines and cannot be linked here.
 */
std::string fixName(std::string name)
{
	if (!safename::sanitizeLeafName(name.data(), name.size()))
	{
		return safename::kPlaceholder;
	}
	return name;
}

/// What ArchiveWrapper::extract opens, for an output directory of @a dir.
std::string joined(const std::string& dir, const std::string& member)
{
	return dir + '/' + fixName(member);
}

/// True when @a s contains a byte that could separate, anchor or truncate a
/// path component -- checked on the leaf only, never on the directory.
bool holdsPathish(const std::string& s)
{
	for (const char c: s)
	{
		if (safename::isPathish(c))
		{
			return true;
		}
	}
	return false;
}

} // namespace

/**
 * @brief Tests for the @c safe_name module.
 */
class SafeNameTests : public Test {};

// The measured escape. Before the fix this came back byte for byte unchanged,
// so on Windows `retdec-ar-extractor archive.a` wrote to the Startup folder of
// whatever user ran it.
TEST_F(SafeNameTests, BackslashTraversalDoesNotLeaveTheOutputDirectory)
{
	const std::string member = "..\\..\\..\\Users\\Public\\Startup\\evil.exe";

	EXPECT_FALSE(holdsPathish(fixName(member)));
	EXPECT_EQ("out/.._.._.._Users_Public_Startup_evil.exe", joined("out", member));
}

// The spelling the old function did neutralise, kept so that fixing the
// backslash case cannot regress it.
TEST_F(SafeNameTests, ForwardSlashTraversalDoesNotLeaveTheOutputDirectory)
{
	const std::string member = "../../../etc/passwd";

	EXPECT_FALSE(holdsPathish(fixName(member)));
	EXPECT_EQ("out/.._.._.._etc_passwd", joined("out", member));
}

// Neutralised only incidentally before, by not being alphanumeric. On Windows
// "C:evil.exe" is a path relative to the current directory of drive C:, and
// "a:b" names an alternate data stream on "a".
TEST_F(SafeNameTests, DriveAndStreamQualifiersAreNeutralised)
{
	EXPECT_EQ("C_evil.exe", fixName("C:evil.exe"));
	EXPECT_EQ("obj.o_hidden", fixName("obj.o:hidden"));
	EXPECT_FALSE(holdsPathish(fixName("\\\\server\\share\\x")));
}

// strchr matches the terminating NUL of its allow-list, so '\0' tested as
// allowed and survived. Everything downstream that takes a const char* then
// saw a shorter name than the one that was bounds-checked.
TEST_F(SafeNameTests, EmbeddedNulIsReplaced)
{
	std::string member("a\0b", 3);
	const std::string fixed = fixName(member);

	ASSERT_EQ(3u, fixed.size());
	EXPECT_EQ(std::string("a_b"), fixed);
	EXPECT_FALSE(holdsPathish(fixed));
}

// A name that reduces to the directory itself, or to its parent, is not a leaf
// name. Windows strips trailing dots and spaces before resolving, so "..." and
// ".. " reach ".." too.
TEST_F(SafeNameTests, NamesThatResolveToADirectoryBecomeThePlaceholder)
{
	EXPECT_EQ("invalid_name", fixName(""));
	EXPECT_EQ("invalid_name", fixName("."));
	EXPECT_EQ("invalid_name", fixName(".."));
	EXPECT_EQ("invalid_name", fixName("..."));
	EXPECT_EQ("invalid_name", fixName(".. "));
	EXPECT_EQ("invalid_name", fixName("  "));
}

// The placeholder is its own sanitised form, so substituting it cannot need a
// second pass.
TEST_F(SafeNameTests, ThePlaceholderIsAFixedPoint)
{
	EXPECT_EQ(safename::kPlaceholder, fixName(safename::kPlaceholder));
}

// The other half of a sanitiser: ordinary names have to survive, or the fix
// gets reverted the first time someone extracts a real archive.
TEST_F(SafeNameTests, OrdinaryMemberNamesAreUnchanged)
{
	EXPECT_EQ("libfoo-1.2 x86.o", fixName("libfoo-1.2 x86.o"));
	// Parentheses are not on the allow-list and never were; asserted so the
	// mangling an extractor user sees is recorded rather than discovered.
	EXPECT_EQ("libfoo _x86_.o", fixName("libfoo (x86).o"));
	EXPECT_EQ("crtbegin.o", fixName("crtbegin.o"));
	EXPECT_EQ("hello_world.obj", fixName("hello_world.obj"));
	EXPECT_EQ("a.out", fixName("a.out"));
}

// std::isalnum reads the global locale, so the old function's output depended
// on process-wide state a parser does not control. Nothing outside ASCII
// survives now, under any locale.
TEST_F(SafeNameTests, BytesOutsideAsciiAreReplacedRegardlessOfLocale)
{
	const std::string member = "caf\xC3\xA9.o";
	EXPECT_EQ("caf__.o", fixName(member));

	std::string highBytes;
	for (int b = 0x80; b <= 0xFF; ++b)
	{
		highBytes.push_back(static_cast<char>(b));
	}
	const std::string fixed = fixName(highBytes);
	ASSERT_EQ(highBytes.size(), fixed.size());
	for (const char c: fixed)
	{
		EXPECT_EQ(safename::kSubstitute, c);
	}
}

// Sanitising is idempotent, which is what lets the extractor sanitise once at
// the boundary. `extract` appends ".2" to a duplicate name after fixing it;
// that suffix has to survive a second pass unchanged.
TEST_F(SafeNameTests, SanitisingIsIdempotent)
{
	const char* const members[] = {"..\\evil", "../evil", "a\\b/c:d", "...", "", "normal.o", "_", "-. "};
	for (const char* const m: members)
	{
		const std::string once = fixName(m);
		EXPECT_EQ(once, fixName(once)) << "input: " << m;
	}

	EXPECT_EQ("obj.o.2", fixName(fixName("obj.o") + ".2"));
}

// The whole 8-bit domain, at the granularity the extractor works in: every byte
// that can appear in a member name, one name each.
TEST_F(SafeNameTests, NoSingleByteMemberNameCanEscape)
{
	for (int b = 0; b <= 0xFF; ++b)
	{
		const std::string member(1, static_cast<char>(b));
		const std::string fixed = fixName(member);

		EXPECT_FALSE(holdsPathish(fixed)) << "byte: " << b;
		EXPECT_EQ(std::string::npos, fixed.find('/')) << "byte: " << b;
		EXPECT_EQ(std::string::npos, fixed.find('\\')) << "byte: " << b;
	}
}

} // namespace tests
} // namespace utils
} // namespace retdec
