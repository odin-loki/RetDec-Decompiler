/**
 * @file tests/fileformat/character_iterator_test.cpp
 * @brief Boundary tests for CharacterIterator::pointsToValidCharacter.
 *
 * The class documents itself as one that "performs boundary checks", and
 * FileFormat::loadStrings walks a section with it looking for wide strings, so
 * its bounds are the bounds of an attacker-supplied section.
 *
 * src/fileformat links LLVM and is out of reach of the fast gate, but this
 * header is not: it includes <cctype> and <iterator> and nothing else, so the
 * whole class can be driven here directly. The buffers below are exactly-sized
 * heap allocations rather than std::vector or arrays, because a sanitizer only
 * sees a read past the end of the allocation -- past the end of a vector's
 * SIZE, inside its capacity, is invisible.
 */

#include "retdec/fileformat/types/strings/character_iterator.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

using namespace retdec::fileformat;

namespace {

/// Answers pointsToValidCharacter for @p data placed in an exactly-sized heap
/// buffer, with the iterator at @p offset and a character width of @p charStep.
struct Answer
{
	bool little = false;
	bool big = false;
};

Answer ask(const char* data, std::size_t size, std::size_t offset, std::size_t charStep)
{
	char* buf = static_cast<char*>(std::malloc(size));
	std::memcpy(buf, data, size);

	const CharacterIterator<const char*> it(buf + offset, buf, buf + size, charStep);
	Answer a;
	a.little = it.pointsToValidCharacter(CharacterEndianness::Little);
	a.big = it.pointsToValidCharacter(CharacterEndianness::Big);

	std::free(buf);
	return a;
}

} // namespace

// The defect. The guard was `if (itr == last) return false;`, which proves
// only that ONE byte is left, and both branches read charStep of them: the
// little-endian padding range ends at itr + charStep, and the big-endian
// character byte IS itr + charStep - 1.
//
// Watched failing under AddressSanitizer against the code as it stood, on this
// exact shape -- a 3-byte buffer, charStep 2, iterator on the final byte:
//
//     READ of size 1 at ... 0 bytes after 3-byte region
//     #0 ... character_iterator.h:65  pointsToValidCharacter(const It&, ...)
//     #1 ... character_iterator.h:257 pointsToValidCharacter(CharacterEndianness)
//
// This test is load-bearing under a sanitizer and not without one, and says so
// rather than implying otherwise: revert the guard and it aborts here with that
// heap-buffer-overflow, but on a plain build the byte after the allocation is
// usually not printable, so the EXPECT_FALSE below holds by luck. That is the
// defect, not an argument against the test -- the answer depended on a byte
// outside the section. The repository runs this suite under
// -fsanitize=address,undefined in .github/workflows/sanitizers.yml, which is
// where it bites.
TEST(CharacterIteratorBounds, APartialCharacterAtTheEndIsNotValid)
{
	// "AB\0" with the iterator on the '\0': one byte left, two needed.
	const Answer a = ask("AB\0", 3, 2, 2);
	EXPECT_FALSE(a.little);
	EXPECT_FALSE(a.big);

	// Three bytes left of a four-byte character.
	const Answer b = ask("A\0\0", 3, 0, 4);
	EXPECT_FALSE(b.little);
	EXPECT_FALSE(b.big);
}

// A charStep of 0 would make `distance < charStep` true for every position, so
// the guard rejects it rather than letting the loop below run to an iterator it
// never reaches.
TEST(CharacterIteratorBounds, AZeroWidthCharacterIsNotValid)
{
	const Answer a = ask("A", 1, 0, 0);
	EXPECT_FALSE(a.little);
	EXPECT_FALSE(a.big);
}

// The end position answered false before and must still.
TEST(CharacterIteratorBounds, TheEndIsNotValid)
{
	const Answer a = ask("AB", 2, 2, 2);
	EXPECT_FALSE(a.little);
	EXPECT_FALSE(a.big);
}

// What the guard must NOT change: every position with a whole character in
// front of it keeps the answer it had.
TEST(CharacterIteratorBounds, AWholeCharacterKeepsItsAnswer)
{
	const Answer oneByte = ask("A", 1, 0, 1);
	EXPECT_TRUE(oneByte.little);
	EXPECT_TRUE(oneByte.big);

	const Answer unprintable = ask("\x01", 1, 0, 1);
	EXPECT_FALSE(unprintable.little);
	EXPECT_FALSE(unprintable.big);

	// UTF-16LE 'A' is 41 00: printable byte first, then the zero padding.
	const Answer le = ask("A\0", 2, 0, 2);
	EXPECT_TRUE(le.little);
	EXPECT_FALSE(le.big);

	// UTF-16BE 'A' is 00 41.
	const Answer be = ask("\0A", 2, 0, 2);
	EXPECT_FALSE(be.little);
	EXPECT_TRUE(be.big);

	// UTF-32LE 'A'.
	const Answer wide = ask("A\0\0\0", 4, 0, 4);
	EXPECT_TRUE(wide.little);
	EXPECT_FALSE(wide.big);
}
