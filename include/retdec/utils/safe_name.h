/**
 * @file include/retdec/utils/safe_name.h
 * @brief Reducing an attacker-supplied name to a leaf file name.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * An archive member name, a resource name, a PDB module name: these are bytes
 * out of an untrusted file, and this tree concatenates several of them with an
 * output directory and opens the result for writing. The only safe answer is a
 * name with no path in it at all -- not a name that is checked for "..", which
 * is a game of spellings, but one whose every surviving byte is drawn from a
 * set that cannot separate path components on any host.
 *
 * The version this replaces lived in an anonymous namespace in
 * src/ar-extractor/archive_wrapper.cpp, needed LLVM to compile, and was
 * therefore reachable by no test in this tree. Its allow-list read `"-. \\"`:
 * hyphen, dot, space and BACKSLASH. Forward slash was neutralised, backslash
 * was explicitly kept, and '.' being on the list let ".." through. Measured
 * against the function as it was, with the output directory `C:\out`:
 *
 *   ../../../etc/passwd                     -> C:\out\.._.._.._etc_passwd
 *   ..\..\..\Users\Public\Startup\evil.exe  -> C:\out\..\..\..\Users\Public\...
 *
 * -- the second one unchanged, so on Windows `retdec-ar-extractor`'s default
 * action wrote wherever the archive said. extractByName and extractByIndex
 * take the same name relative to the working directory when no output path is
 * given.
 *
 * Three further holes in the same three lines, each one a property below:
 *
 *   - `strchr(set, c)` matches the terminating NUL of `set`, so c == '\0'
 *     tested as allowed and an embedded NUL was preserved. A NUL truncates the
 *     name at every C-string API downstream of the std::string that holds it.
 *
 *   - ':' was neutralised only incidentally, by not being alphanumeric. On
 *     Windows `C:evil.exe` names a file relative to the current directory of
 *     drive C:, and `a:b` names an alternate data stream. isPathish() states
 *     that as a rule rather than leaving it to arithmetic.
 *
 *   - std::isalnum is locale-dependent: under a locale where byte 0xE9 is
 *     alphabetic it was preserved, under "C" it was not. A sanitizer whose
 *     output depends on setlocale is a sanitizer with two behaviours. The
 *     classification here is ASCII, spelled out, and constexpr.
 *
 * A name that reduces to nothing usable -- empty, ".", "..", or any run of
 * dots and spaces, since Windows strips trailing dots and spaces before
 * resolving a path -- is not a leaf name at all, and the caller substitutes
 * @ref kPlaceholder rather than opening something that resolves to a
 * directory.
 *
 * Header-only, constexpr, no allocation, no locale, raw pointers and sizes
 * rather than containers -- so the proofs in tests/verification/safe_name_proof.cpp
 * reason about the code the extractor calls, not about a re-typed copy of it.
 * Do not re-derive any of this at a call site; call it.
 */

#ifndef RETDEC_UTILS_SAFE_NAME_H
#define RETDEC_UTILS_SAFE_NAME_H

#include <cstddef>

namespace retdec {
namespace utils {
namespace safename {

/// What a byte outside the allowed set becomes. It is itself outside the
/// allowed set, which is what makes sanitising idempotent -- see
/// proof_sanitize_char_is_idempotent.
inline constexpr char kSubstitute = '_';

/// Substituted for a name that reduces to nothing usable. Chosen so that it is
/// its own sanitised form (proof_the_placeholder_is_a_fixed_point), because a
/// placeholder that needed sanitising would be a second code path.
inline constexpr char kPlaceholder[] = "invalid_name";

/**
 * ASCII alphanumeric, spelled out.
 *
 * Not std::isalnum: that reads the global locale, so the same input gave
 * different output depending on process-wide state a parser does not control,
 * and it is undefined for a negative char. Every byte >= 0x80 is replaced here
 * under every locale.
 */
constexpr bool isAsciiAlnum(char c) noexcept
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/**
 * The punctuation kept verbatim, so that ordinary member names such as
 * `libfoo-1.2.o` and `Some Object.o` survive unchanged.
 *
 * '.' is on this list and '..' is therefore spellable; that is why
 * @ref isUsableLeafName exists rather than a rule about what dots may appear
 * where.
 */
constexpr bool isAllowedPunct(char c) noexcept
{
	return c == '-' || c == '.' || c == ' ';
}

/**
 * Bytes that separate, anchor or qualify a path component on some host this
 * output may be written on.
 *
 * '/' and '\\' are separators on POSIX and Windows respectively -- Windows
 * accepts both. ':' is the drive qualifier and the alternate-data-stream
 * separator on Windows. '\0' terminates the name for every C-string API that
 * sees it, which silently discards whatever a bounds check was applied to.
 *
 * Nothing in the allowed set is one of these, and that is a property rather
 * than an observation: see proof_a_surviving_byte_is_never_pathish.
 */
constexpr bool isPathish(char c) noexcept
{
	return c == '/' || c == '\\' || c == ':' || c == '\0';
}

/// A byte survives sanitising unchanged exactly when this holds.
constexpr bool isKept(char c) noexcept
{
	return isAsciiAlnum(c) || isAllowedPunct(c);
}

/// Map one byte to its sanitised form.
constexpr char sanitizeChar(char c) noexcept
{
	return isKept(c) ? c : kSubstitute;
}

/**
 * True when every byte in [name, name + size) is '.' or ' ', including the
 * empty range.
 *
 * Such a name is not a leaf name: "" and "." are the directory itself, ".." is
 * its parent, and Windows strips trailing dots and spaces before resolving a
 * path, so "..." and ".. " resolve to ".." as well. Rather than enumerate
 * those spellings, this rejects the whole class.
 */
constexpr bool isDotsAndSpaces(const char* name, std::size_t size) noexcept
{
	for (std::size_t i = 0; i < size; ++i)
	{
		if (name[i] != '.' && name[i] != ' ')
		{
			return false;
		}
	}
	return true;
}

/// A sanitised name is usable as a leaf when it is not the whole dots-and-spaces
/// class. Call it only on already-sanitised bytes.
constexpr bool isUsableLeafName(const char* name, std::size_t size) noexcept
{
	return !isDotsAndSpaces(name, size);
}

/**
 * Sanitise @a size bytes at @a name in place.
 *
 * @return @c true when the result may be used as a leaf file name, @c false
 *         when the caller must substitute @ref kPlaceholder.
 *
 * A @c false return does not mean the buffer was left alone: the bytes are
 * sanitised either way, so a caller that ignores the result still cannot be
 * made to write outside its output directory. The return distinguishes
 * "sanitised to something" from "sanitised to nothing".
 *
 * @a name may be null only when @a size is zero, which returns @c false.
 */
constexpr bool sanitizeLeafName(char* name, std::size_t size) noexcept
{
	for (std::size_t i = 0; i < size; ++i)
	{
		name[i] = sanitizeChar(name[i]);
	}
	return isUsableLeafName(name, size);
}

} // namespace safename
} // namespace utils
} // namespace retdec

#endif
