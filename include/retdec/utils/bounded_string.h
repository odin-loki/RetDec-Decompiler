/**
 * @file include/retdec/utils/bounded_string.h
 * @brief Measuring a string that a file was under no obligation to terminate.
 *
 * Every parser in this tree eventually reaches a name: a section name, a
 * metadata version, a symbol, a type. The bytes come out of the file and so
 * does the length that is supposed to bound them, and nothing says the
 * terminator is there. `strlen` on such a pointer reads until it finds a zero
 * byte, which may be well past the end of the mapping; taking `std::min` with
 * the declared length afterwards does not help, because the read has already
 * happened.
 *
 * This bug appeared four separate times in one review pass -- in the .NET
 * metadata root, twice in the PDB symbol walker, and in the PDB type field list
 * -- and was fixed four separate ways. It is one rule, so it is stated once
 * here and proved once in tests/verification/bounded_string_proof.cpp.
 *
 * Both functions read at most @p len bytes from @p data and never dereference a
 * null pointer. Neither allocates, throws, or looks past the bound, so either
 * is safe on a pointer into a read-only mapping.
 */

#ifndef RETDEC_UTILS_BOUNDED_STRING_H
#define RETDEC_UTILS_BOUNDED_STRING_H

#include <cstddef>

namespace retdec {
namespace utils {
namespace bstr {

/// No terminator was found within the bound.
inline constexpr std::size_t npos = static_cast<std::size_t>(-1);

/**
 * Offset of the first NUL in the @p len bytes at @p data, or npos.
 *
 * Use this where the terminator is required: a name the format says is
 * NUL-terminated inside its record is malformed without one, and npos is how
 * that is reported. `data == nullptr` is npos rather than a dereference.
 */
inline std::size_t terminatorAt(const char* data, std::size_t len) noexcept
{
	if (data == nullptr) return npos;
	for (std::size_t i = 0; i < len; ++i)
		if (data[i] == '\0') return i;
	return npos;
}

/**
 * Length of the string at @p data, measured to the first NUL or to @p len,
 * whichever comes first.
 *
 * Use this where the terminator is optional: a fixed-width field padded with
 * NULs, or one that fills its space exactly. The result is always <= @p len, so
 * a caller can copy it without a second bound.
 */
inline std::size_t boundedLength(const char* data, std::size_t len) noexcept
{
	const std::size_t at = terminatorAt(data, len);
	return at == npos ? (data == nullptr ? 0 : len) : at;
}

/// True when the @p len bytes at @p data contain a terminator.
inline bool isTerminated(const char* data, std::size_t len) noexcept
{
	return terminatorAt(data, len) != npos;
}

} // namespace bstr
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_BOUNDED_STRING_H
