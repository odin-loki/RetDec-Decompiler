/**
 * @file tests/verification/safe_name_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/safe_name.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * The header exists because a name out of an archive was concatenated with an
 * output directory and opened for writing, and the function that was supposed
 * to make that safe kept backslash in its allow-list. A test that tries the
 * spellings an attacker might use proves nothing about the spellings it did not
 * try, so the central property here is stated the other way round and over a
 * fully symbolic buffer: no input at all leaves a byte that could separate,
 * anchor or truncate a path.
 *
 * proof_a_byte_survives_only_if_it_is_kept_or_is_the_substitute is the shape
 * the obvious property should have had. `sanitizeChar(c) == c iff isKept(c)` is
 * false, because the substitute '_' is not itself in the kept set and maps to
 * itself; ESBMC returns c = 95 for it. What is true, and what the buffer proofs
 * rest on, is the weaker statement with kSubstitute admitted.
 *
 * The concrete cases at the end are the measured counterexamples from the
 * function this replaces, kept as properties so the allow-list cannot quietly
 * grow a separator again.
 */

// The symbolic buffer is kLen bytes and the longest concrete name below is 19,
// so 24 unwinds every loop in the file to completion. ESBMC generates unwinding
// assertions by default, so that bound is proved rather than assumed.
// ESBMC-OPTIONS: --unwind 24

#include "retdec/utils/safe_name.h"

#include <cassert>
#include <cstddef>

using namespace retdec::utils::safename;

extern "C" {
char nondet_char();
std::size_t nondet_size();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

// Long enough to spell "..", a separator and a byte on each side of both.
// Short enough for every loop over it to unwind fully.
static constexpr std::size_t kLen = 8;

namespace {

void fillNondet(char (&buf)[kLen])
{
	for (std::size_t i = 0; i < kLen; ++i)
		buf[i] = nondet_char();
}

} // namespace

// ─── the character kernel, over every char ───────────────────────────────────

// The property the whole header is for. Whatever the byte was -- separator,
// drive colon, NUL, a byte above 0x7F -- what comes out cannot take a name out
// of the directory it is joined to.
extern "C" void proof_a_surviving_byte_is_never_pathish()
{
	const char c = nondet_char();
	assert(!isPathish(sanitizeChar(c)));
}

// Sanitising twice is sanitising once, so a name that has been through this
// cannot be changed by going through it again -- which is what lets a caller
// sanitise at the boundary and not think about it afterwards. It holds because
// kSubstitute is a fixed point.
extern "C" void proof_sanitize_char_is_idempotent()
{
	const char c = nondet_char();
	assert(sanitizeChar(sanitizeChar(c)) == sanitizeChar(c));
}

// The allow-list, stated as a property rather than as the implementation. Note
// the disjunct: '_' is preserved and is NOT in the kept set, and the biconditional
// without it is refuted at c = 95.
extern "C" void proof_a_byte_survives_only_if_it_is_kept_or_is_the_substitute()
{
	const char c = nondet_char();
	if (sanitizeChar(c) == c)
	{
		assert(isKept(c) || c == kSubstitute);
	}
	if (isKept(c))
	{
		assert(sanitizeChar(c) == c);
	}
}

// std::isalnum, which this replaces, reads the global locale: under a locale
// where 0xE9 is alphabetic the byte was preserved, under "C" it was not. Here
// no byte outside ASCII survives, whatever setlocale was called with.
extern "C" void proof_no_byte_outside_ascii_survives()
{
	const char c = nondet_char();
	// char is signed on the platforms this builds for; a negative value is a
	// byte >= 0x80. The condition is written so the proof still holds, rather
	// than becoming vacuous, where char is unsigned.
	if (c < 0 || static_cast<unsigned char>(c) >= 0x80u)
	{
		assert(sanitizeChar(c) == kSubstitute);
	}
}

// ─── the buffer entry point, over every buffer ───────────────────────────────

// The same property as the first one, but over the function callers actually
// use, and for a symbolic length as well as symbolic contents.
extern "C" void proof_a_sanitised_name_holds_no_pathish_byte()
{
	char buf[kLen];
	fillNondet(buf);
	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kLen);

	sanitizeLeafName(buf, n);

	for (std::size_t i = 0; i < n; ++i)
	{
		assert(!isPathish(buf[i]));
		assert(isKept(buf[i]) || buf[i] == kSubstitute);
	}
}

// A true return is a promise that the name is not the directory, its parent, or
// any spelling Windows resolves to one of those by stripping trailing dots and
// spaces. Stated as: something survived that is neither a dot nor a space.
extern "C" void proof_a_usable_name_has_a_byte_that_is_not_a_dot_or_a_space()
{
	char buf[kLen];
	fillNondet(buf);
	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kLen);

	if (sanitizeLeafName(buf, n))
	{
		bool found = false;
		for (std::size_t i = 0; i < n; ++i)
		{
			if (buf[i] != '.' && buf[i] != ' ')
			{
				found = true;
			}
		}
		assert(found);
	}
}

// The converse, and the one that matters at the call site: an empty name, "."
// and ".." are rejected for every input that reduces to them, not just for the
// three literals.
extern "C" void proof_a_name_of_dots_and_spaces_is_rejected()
{
	char buf[kLen];
	fillNondet(buf);
	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kLen);

	bool onlyDotsAndSpaces = true;
	for (std::size_t i = 0; i < n; ++i)
	{
		if (buf[i] != '.' && buf[i] != ' ')
		{
			onlyDotsAndSpaces = false;
		}
	}

	if (onlyDotsAndSpaces)
	{
		assert(!sanitizeLeafName(buf, n));
	}
}

// Sanitising cannot turn a rejected name into an accepted one by accident: the
// only bytes it produces are kept bytes and kSubstitute, and kSubstitute is
// neither a dot nor a space, so a name that gains a substitute becomes usable.
// This is the direction that keeps the placeholder from being reachable for a
// name that had real content in it.
extern "C" void proof_a_name_with_a_substituted_byte_is_usable()
{
	char buf[kLen];
	fillNondet(buf);
	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kLen);
	__ESBMC_assume(n > 0);

	bool anySubstituted = false;
	for (std::size_t i = 0; i < n; ++i)
	{
		if (!isKept(buf[i]))
		{
			anySubstituted = true;
		}
	}

	if (anySubstituted)
	{
		assert(sanitizeLeafName(buf, n));
	}
}

// ─── the measured counterexamples, kept as properties ────────────────────────

namespace {

// Sanitise a literal and assert the two things the call site depends on: it is
// usable, and nothing in it can leave the output directory.
void checkNeutralised(const char* literal, std::size_t n)
{
	char buf[24];
	assert(n <= sizeof(buf));
	for (std::size_t i = 0; i < n; ++i)
		buf[i] = literal[i];

	const bool usable = sanitizeLeafName(buf, n);
	assert(usable);
	for (std::size_t i = 0; i < n; ++i)
		assert(!isPathish(buf[i]));
}

} // namespace

// The two names measured against the old function. The first came out as
// ".._.._.._etc_passwd"; the second came out UNCHANGED, because backslash was
// on the allow-list, and on Windows that is an absolute escape from the output
// directory.
extern "C" void proof_the_measured_traversals_are_neutralised()
{
	static const char posix[] = "../../../etc/passwd";
	checkNeutralised(posix, sizeof(posix) - 1);

	static const char windows[] = "..\\..\\evil.exe";
	checkNeutralised(windows, sizeof(windows) - 1);

	// Not a separator, and not alphanumeric either, so the old function
	// replaced it by accident rather than by rule: on Windows this names a file
	// relative to the current directory of drive C:.
	static const char drive[] = "C:evil.exe";
	checkNeutralised(drive, sizeof(drive) - 1);
}

// strchr matches the terminating NUL of its first argument, so the old
// allow-list test said yes to '\0' and an embedded NUL was preserved. Every
// C-string API downstream then saw "a".
extern "C" void proof_an_embedded_nul_does_not_survive()
{
	char buf[3] = {'a', '\0', 'b'};
	const bool usable = sanitizeLeafName(buf, 3);
	assert(usable);
	assert(buf[0] == 'a');
	assert(buf[1] == kSubstitute);
	assert(buf[2] == 'b');
}

// An ordinary member name is not touched. A sanitiser that mangled these would
// be replaced at the first complaint, and the replacement would have this bug
// again.
extern "C" void proof_an_ordinary_member_name_is_unchanged()
{
	static const char name[] = "libfoo-1.2 x86.o";
	char buf[sizeof(name) - 1];
	for (std::size_t i = 0; i < sizeof(buf); ++i)
		buf[i] = name[i];

	assert(sanitizeLeafName(buf, sizeof(buf)));
	for (std::size_t i = 0; i < sizeof(buf); ++i)
		assert(buf[i] == name[i]);
}

// The placeholder the caller substitutes has to be its own sanitised form;
// otherwise the substitution would need sanitising in turn, which is the kind
// of second path a fix gets forgotten in.
extern "C" void proof_the_placeholder_is_a_fixed_point()
{
	static constexpr std::size_t n = sizeof(kPlaceholder) - 1;
	char buf[n];
	for (std::size_t i = 0; i < n; ++i)
		buf[i] = kPlaceholder[i];

	assert(sanitizeLeafName(buf, n));
	for (std::size_t i = 0; i < n; ++i)
		assert(buf[i] == kPlaceholder[i]);
}

// The three names the old function returned as-is, all of which resolve to a
// directory rather than to a file in it.
extern "C" void proof_the_directory_names_are_rejected()
{
	char dot[1] = {'.'};
	assert(!sanitizeLeafName(dot, 1));

	char dotdot[2] = {'.', '.'};
	assert(!sanitizeLeafName(dotdot, 2));

	// Windows strips trailing dots and spaces before resolving, so both of
	// these reach "..".
	char tripleDot[3] = {'.', '.', '.'};
	assert(!sanitizeLeafName(tripleDot, 3));

	char dotdotSpace[3] = {'.', '.', ' '};
	assert(!sanitizeLeafName(dotdotSpace, 3));

	char empty[1] = {'x'};
	assert(!sanitizeLeafName(empty, 0));
}
