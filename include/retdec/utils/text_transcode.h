/**
 * @file include/retdec/utils/text_transcode.h
 * @brief Sizing and performing every byte-run-to-text conversion in this tree.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * bounded_string.h owns "where does this string end". Nothing owned "how many
 * bytes does rendering it take, and did I have that many" -- so every converter
 * in the tree sized its own output, and each one sized it with an unchecked
 * multiplication:
 *
 *   include/retdec/utils/conversion.h:59  `size * 3 - 1` / `size * 2`, handed
 *       straight to result.resize(), then 2*size characters written into it;
 *   src/utils/gpu_scanner_cpu.cpp         `h_fileNibs.resize(size * 2)` in
 *       GpuScanner::uploadFile, then
 *       writes at i*2 and i*2+1 for every i < size;
 *   src/utils/string.cpp:399              toWide reserves `str.length()*length`.
 *
 * A product that wraps does not fail at the multiplication. It fails later, in
 * the loop, writing past a container that was made short -- which is why the
 * capacity and the conversion belong in one function, and the conversion
 * refuses to start when the capacity was not supplied. That is the shape of
 * every entry point here: `f(in, n, out, outCap, ...)` returns 0 and writes
 * nothing unless outCap covers the matching `...Capacity(n)`.
 *
 * The second half is UTF-16LE to UTF-8, which existed three times:
 *
 *   src/cli_parser/cli_heaps.cpp:83   utf16leToUtf8(const uint8_t*, size_t
 *       chars) -- no buffer size at all, the bound lives in the caller;
 *   CLIReader::fieldConstantString  the same decoder inlined with a
 *       `i + 1 < blob.size()` convention instead of a unit count;
 *   src/utils/string.cpp:421          unicodeToAscii, a third convention.
 *
 * The two .NET copies agree with each other byte for byte (proved:
 * proof_the_two_dotnet_entry_points_agree) and both are wrong in the same way:
 * a UTF-16 surrogate is re-emitted as a three-byte sequence ED A0 80..ED BF BF,
 * which is not UTF-8 -- the code point it denotes is not a scalar value. A
 * surrogate PAIR, the whole reason UTF-16 has surrogates, comes out as two such
 * sequences rather than as the character. This kernel combines a well-formed
 * pair and replaces a lone surrogate with U+FFFD, so every byte it writes is
 * well-formed UTF-8 (proved: proof_encode_utf8_is_well_formed,
 * proof_utf16_never_emits_a_surrogate).
 *
 * Header-only, constexpr where it can be, no allocation, no exceptions, raw
 * pointers and sizes rather than containers -- so the proofs in
 * tests/verification/text_transcode_proof.cpp reason about the code the
 * parsers call, not about a re-typed copy of it. Do not re-derive any of this
 * at a call site; call it.
 */

#ifndef RETDEC_UTILS_TEXT_TRANSCODE_H
#define RETDEC_UTILS_TEXT_TRANSCODE_H

#include "retdec/utils/bounds.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace txt {

// ─── constants, every one of them from a format ──────────────────────────────

/// Characters one byte renders to in hex: two nibbles.
inline constexpr std::size_t kHexCharsPerByte = 2;

/// Characters one byte renders to in the spaced form: two nibbles and the
/// separator that precedes them. The very first byte has no separator, which is
/// the `- 1` in @ref hexCapacity.
inline constexpr std::size_t kSpacedHexCharsPerByte = 3;

/// Bits in a byte, as bytesToBits renders them.
inline constexpr std::size_t kBitsPerByte = 8;

/// Bytes one UTF-16 code unit can expand to in UTF-8, worst case.
///
/// A BMP scalar value above U+07FF is three bytes; U+FFFD, which is what a lone
/// surrogate becomes, is also three. A surrogate PAIR is four bytes but costs
/// two units, so it is under this rate rather than over it. Three is therefore
/// exact and not merely safe.
inline constexpr std::size_t kUtf8BytesPerUtf16Unit = 3;

/// Bytes one MUTF-8 character can expand to in UTF-8, worst case.
///
/// A JVM/DEX supplementary character arrives as a surrogate pair -- two units
/// by the declared count -- and leaves as one four-byte sequence, so four per
/// unit is loose. It is the bound the format's own `utf16_size` field justifies
/// without a case analysis, so it is the one stated.
inline constexpr std::size_t kUtf8BytesPerMutf8Unit = 4;

/// Highest Unicode code point. Everything above is not a scalar value.
inline constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;

/// The UTF-16 surrogate range, which UTF-8 may not encode.
inline constexpr std::uint32_t kHighSurrogateFirst = 0xD800;
inline constexpr std::uint32_t kHighSurrogateLast = 0xDBFF;
inline constexpr std::uint32_t kLowSurrogateFirst = 0xDC00;
inline constexpr std::uint32_t kLowSurrogateLast = 0xDFFF;

/// U+FFFD REPLACEMENT CHARACTER, what an ill-formed unit becomes.
inline constexpr std::uint32_t kReplacement = 0xFFFD;

/// Bits a low surrogate contributes to a combined code point.
inline constexpr unsigned kSurrogateBits = 10;

/// First code point that needs a surrogate pair in UTF-16.
inline constexpr std::uint32_t kSupplementaryBase = 0x10000;

// ─── hex ─────────────────────────────────────────────────────────────────────

/// Byte-pair table, the one src/utils/conversion.cpp:20 uses.
///
/// It is here as data rather than as `"0123456789ABCDEF"[b >> 4]` because a
/// 512-entry table is 512 chances to mistype a digit, and only a proof over the
/// data catches that. proof_byte_to_hex_matches_the_arithmetic checks all 256
/// bytes of both tables against @ref hexDigit, which computes the answer
/// instead of looking it up.
inline constexpr char kHexPairsUpper[513] =
	"000102030405060708090A0B0C0D0E0F"
	"101112131415161718191A1B1C1D1E1F"
	"202122232425262728292A2B2C2D2E2F"
	"303132333435363738393A3B3C3D3E3F"
	"404142434445464748494A4B4C4D4E4F"
	"505152535455565758595A5B5C5D5E5F"
	"606162636465666768696A6B6C6D6E6F"
	"707172737475767778797A7B7C7D7E7F"
	"808182838485868788898A8B8C8D8E8F"
	"909192939495969798999A9B9C9D9E9F"
	"A0A1A2A3A4A5A6A7A8A9AAABACADAEAF"
	"B0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF"
	"C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF"
	"D0D1D2D3D4D5D6D7D8D9DADBDCDDDEDF"
	"E0E1E2E3E4E5E6E7E8E9EAEBECEDEEEF"
	"F0F1F2F3F4F5F6F7F8F9FAFBFCFDFEFF";

inline constexpr char kHexPairsLower[513] =
	"000102030405060708090a0b0c0d0e0f"
	"101112131415161718191a1b1c1d1e1f"
	"202122232425262728292a2b2c2d2e2f"
	"303132333435363738393a3b3c3d3e3f"
	"404142434445464748494a4b4c4d4e4f"
	"505152535455565758595a5b5c5d5e5f"
	"606162636465666768696a6b6c6d6e6f"
	"707172737475767778797a7b7c7d7e7f"
	"808182838485868788898a8b8c8d8e8f"
	"909192939495969798999a9b9c9d9e9f"
	"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
	"b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
	"c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
	"d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
	"e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
	"f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

/// The hex digit for the low four bits of @p v.
///
/// Computed, not looked up. This is the independent statement the table is
/// checked against; if the two ever disagree the proof says which byte.
constexpr char hexDigit(unsigned v, bool upper) noexcept
{
	const unsigned d = v & 0xFu;
	return d < 10 ? static_cast<char>('0' + d) : static_cast<char>((upper ? 'A' : 'a') + (d - 10));
}

/// Value of the hex digit @p c, or -1 when @p c is not one.
///
/// The -1 is the whole point. src/utils/conversion.cpp:174 uses
/// `strtol(byteString.c_str(), nullptr, 16)`, which returns 0 for "zz" exactly
/// as it does for "00", so a corrupt hex dump silently becomes a run of NUL
/// bytes and every caller believes it parsed.
constexpr int hexValue(char c) noexcept
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/// Characters needed to render @p n bytes as hex, or 0 if that does not fit.
///
/// Returns 0 for n == 0 as well, because zero bytes render to zero characters
/// and `3 * 0 - 1` is not a number. Callers must treat 0 as "nothing to write",
/// which is what the empty input means anyway.
///
/// The product is never formed before it is known to be representable. This is
/// the check conversion.h:59 does not do: for `size = 0x8000000000000001` the
/// spaced form computes 3*size - 1 = 0x8000000000000002, resize() makes a
/// string far shorter than the 2*size characters the loop then writes, and the
/// overflow is discovered as heap corruption rather than as a bad length.
constexpr std::size_t hexCapacity(std::size_t n, bool spaced) noexcept
{
	if (n == 0) return 0;
	const std::size_t perByte = spaced ? kSpacedHexCharsPerByte : kHexCharsPerByte;
	// bounds::mulFits(perByte, n) rather than (n, perByte): the two are the
	// same predicate, and this order divides SIZE_MAX by a literal so the
	// query stays linear. Measured: both discharge under boolector in under a
	// second, so this is tidiness rather than necessity.
	if (!bounds::mulFits(perByte, n)) return 0;
	return spaced ? (n * perByte - 1) : (n * perByte);
}

/// The two hex characters of @p b, high nibble first.
///
/// Written through references. src/utils/conversion.cpp:20 returns a pointer to
/// a function-local `static char result[3]`, so two threads of this tree's
/// ThreadPool rendering two different bytes get each other's digits -- and a
/// caller that holds the pointer across a second call gets the second byte.
constexpr void byteToHex(std::uint8_t b, bool upper, char& hi, char& lo) noexcept
{
	const char* lut = upper ? kHexPairsUpper : kHexPairsLower;
	const std::size_t pos = static_cast<std::size_t>(b) * kHexCharsPerByte;
	hi = lut[pos];
	lo = lut[pos + 1];
}

/// Render @p n bytes at @p in as hex into @p out, returning characters written.
///
/// Returns 0 and writes nothing unless `outCap >= hexCapacity(n, spaced)`, so
/// the caller cannot state the length and forget the capacity: there is only
/// one number and this function computes it.
inline std::size_t
bytesToHex(const std::uint8_t* in, std::size_t n, char* out, std::size_t outCap, bool upper, bool spaced) noexcept
{
	if (in == nullptr || out == nullptr) return 0;
	const std::size_t need = hexCapacity(n, spaced);
	if (need == 0) return 0;
	if (outCap < need) return 0;

	std::size_t w = 0;
	for (std::size_t i = 0; i < n; ++i)
	{
		if (spaced && i > 0) out[w++] = ' ';
		byteToHex(in[i], upper, out[w], out[w + 1]);
		w += kHexCharsPerByte;
	}
	return w;
}

/// Parse @p n hex characters at @p in into bytes, returning success.
///
/// An odd @p n is refused outright. src/utils/conversion.cpp:171 steps by two
/// and takes `hex.substr(i, 2)`, so a final lone nibble becomes a whole byte
/// with its value in the HIGH half -- "abc" parses as ab 0c, not ab c0 and not
/// an error. Half a byte is not a byte, and which half it was is a guess.
///
/// A non-hex character anywhere refuses the whole run and leaves @p written 0,
/// rather than contributing a zero byte the way strtol does.
inline bool
hexToBytes(const char* in, std::size_t n, std::uint8_t* out, std::size_t outCap, std::size_t& written) noexcept
{
	written = 0;
	if (in == nullptr || out == nullptr) return false;
	if ((n & 1u) != 0) return false;

	// n >> 1, not n / 2: identical for unsigned, and it keeps the query free of
	// a division whose spurious overflow check the bitvector backends discharge
	// as SAT (see the solver note in the harness).
	const std::size_t need = n >> 1;
	if (outCap < need) return false;

	for (std::size_t i = 0; i < need; ++i)
	{
		// i < n >> 1, so 2*i + 1 <= n - 1 and neither index nor the doubling
		// can leave the buffer or the type. conversion.cpp:171 counts with an
		// `unsigned int` against a `std::string::size_type`: at length 2^32 the
		// counter wraps from 0xFFFFFFFE back to 0 and the loop never ends.
		const std::size_t at = i * kHexCharsPerByte;
		const int hi = hexValue(in[at]);
		const int lo = hexValue(in[at + 1]);
		if (hi < 0 || lo < 0)
		{
			written = 0;
			return false;
		}
		out[i] = static_cast<std::uint8_t>((static_cast<unsigned>(hi) << 4) | static_cast<unsigned>(lo));
	}

	written = need;
	return true;
}

// ─── bits ────────────────────────────────────────────────────────────────────

/// Characters needed to render @p n bytes as bits, or 0 if that does not fit.
constexpr std::size_t bitsCapacity(std::size_t n) noexcept
{
	if (n == 0) return 0;
	if (!bounds::mulFits(kBitsPerByte, n)) return 0;
	return n * kBitsPerByte;
}

/// Render @p n bytes at @p in as '0'/'1', most significant bit first.
///
/// The input is `std::uint8_t`, and that is the fix rather than an incidental
/// choice. include/retdec/utils/conversion.h:204 is a template over N and is
/// instantiated for `int8_t`; its body is `(item << j) & 0x80`, so for any
/// element with the top bit set -- every byte from 0x80 up -- `item` promotes
/// to a negative int and `item << j` left-shifts a negative value, which is
/// undefined behaviour in C++17. Shifting an unsigned value right cannot be.
inline std::size_t bytesToBits(const std::uint8_t* in, std::size_t n, char* out, std::size_t outCap) noexcept
{
	if (in == nullptr || out == nullptr) return 0;
	const std::size_t need = bitsCapacity(n);
	if (need == 0) return 0;
	if (outCap < need) return 0;

	for (std::size_t i = 0; i < n; ++i)
	{
		const unsigned v = in[i];
		for (unsigned j = 0; j < kBitsPerByte; ++j)
		{
			// Shift count is 7 - j with j < 8, so it is 0..7: never negative,
			// never the operand width, and the operand is unsigned.
			const unsigned bit = (v >> (kBitsPerByte - 1 - j)) & 1u;
			out[i * kBitsPerByte + j] = bit ? '1' : '0';
		}
	}
	return need;
}

/// bytesToBits for a run that arrived as `int8_t`.
///
/// The two-line overload exists so the int8_t call sites stop instantiating the
/// undefined-behaviour template. The reinterpret is defined: int8_t and uint8_t
/// have the same size and alignment, and reading either through a narrow
/// character type is explicitly permitted.
inline std::size_t bytesToBits(const std::int8_t* in, std::size_t n, char* out, std::size_t outCap) noexcept
{
	return bytesToBits(reinterpret_cast<const std::uint8_t*>(in), n, out, outCap);
}

// ─── UTF-8 output ────────────────────────────────────────────────────────────

/// Bytes the UTF-8 encoding of @p cp occupies, after replacement.
constexpr std::size_t utf8Length(std::uint32_t cp) noexcept
{
	if (cp > kMaxCodePoint || (cp >= kHighSurrogateFirst && cp <= kLowSurrogateLast)) cp = kReplacement;
	if (cp < 0x80) return 1;
	if (cp < 0x800) return 2;
	if (cp < kSupplementaryBase) return 3;
	return 4;
}

/// Encode @p cp as UTF-8 at @p out, returning the bytes written (1..4).
///
/// Total: a surrogate or an out-of-range value is replaced by U+FFFD rather
/// than encoded, so there is no input for which this writes an ill-formed
/// sequence. Both .NET decoders encode a surrogate directly and produce
/// ED A0 80 .. ED BF BF, which no UTF-8 decoder will accept.
///
/// The caller must have `utf8Length(cp)` bytes at @p out; every caller in this
/// header checks the whole output capacity before the loop starts.
constexpr std::size_t encodeUtf8(std::uint32_t cp, char* out) noexcept
{
	if (cp > kMaxCodePoint || (cp >= kHighSurrogateFirst && cp <= kLowSurrogateLast)) cp = kReplacement;

	if (cp < 0x80)
	{
		out[0] = static_cast<char>(cp);
		return 1;
	}
	if (cp < 0x800)
	{
		out[0] = static_cast<char>(0xC0u | (cp >> 6));
		out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
		return 2;
	}
	if (cp < kSupplementaryBase)
	{
		out[0] = static_cast<char>(0xE0u | (cp >> 12));
		out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
		out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
		return 3;
	}
	out[0] = static_cast<char>(0xF0u | (cp >> 18));
	out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
	out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
	out[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
	return 4;
}

// ─── UTF-16LE ────────────────────────────────────────────────────────────────

/// True when @p u is a UTF-16 high surrogate.
constexpr bool isHighSurrogate(std::uint32_t u) noexcept
{
	return u >= kHighSurrogateFirst && u <= kHighSurrogateLast;
}

/// True when @p u is a UTF-16 low surrogate.
constexpr bool isLowSurrogate(std::uint32_t u) noexcept
{
	return u >= kLowSurrogateFirst && u <= kLowSurrogateLast;
}

/// The scalar value a well-formed surrogate pair denotes.
constexpr std::uint32_t combineSurrogates(std::uint32_t hi, std::uint32_t lo) noexcept
{
	return kSupplementaryBase + (((hi - kHighSurrogateFirst) << kSurrogateBits) | (lo - kLowSurrogateFirst));
}

/// Bytes needed to transcode @p units UTF-16 units to UTF-8, or 0 if that does
/// not fit.
constexpr std::size_t utf8CapacityForUtf16(std::size_t units) noexcept
{
	if (units == 0) return 0;
	if (!bounds::mulFits(kUtf8BytesPerUtf16Unit, units)) return 0;
	return units * kUtf8BytesPerUtf16Unit;
}

/// Transcode @p inBytes of UTF-16LE at @p in to UTF-8 at @p out.
///
/// @return bytes written, or 0 if @p outCap does not cover
///         `utf8CapacityForUtf16(inBytes / 2)`.
///
/// A trailing odd byte is DROPPED, not read: the unit count is `inBytes >> 1`
/// and the highest index touched is `2 * units - 1 <= inBytes - 1`. Half a code
/// unit carries no information about what the other half was, and reading past
/// it is what cli_heaps.cpp:83 risks by taking a unit count with no buffer size
/// beside it -- `src[i * 2 + 1]` for i < chars, where `chars` came from a
/// compressed integer in the file and the length check lives in the caller.
inline std::size_t utf16leToUtf8(const std::uint8_t* in, std::size_t inBytes, char* out, std::size_t outCap) noexcept
{
	if (in == nullptr || out == nullptr) return 0;
	const std::size_t units = inBytes >> 1;
	const std::size_t need = utf8CapacityForUtf16(units);
	if (need == 0) return 0;
	if (outCap < need) return 0;

	std::size_t w = 0;
	std::size_t i = 0;
	while (i < units)
	{
		const std::size_t at = i * 2;
		std::uint32_t u = static_cast<std::uint32_t>(in[at]) | (static_cast<std::uint32_t>(in[at + 1]) << 8);
		std::uint32_t cp = u;

		if (isHighSurrogate(u))
		{
			// The lookahead is guarded by the unit count, not by the byte
			// count: i + 1 < units is what keeps in[at + 3] inside the buffer.
			if (i + 1 < units)
			{
				const std::uint32_t lo =
					static_cast<std::uint32_t>(in[at + 2]) | (static_cast<std::uint32_t>(in[at + 3]) << 8);
				if (isLowSurrogate(lo))
				{
					cp = combineSurrogates(u, lo);
					w += encodeUtf8(cp, out + w);
					i += 2;
					continue;
				}
			}
			cp = kReplacement;
		}
		else if (isLowSurrogate(u))
		{
			// A low surrogate with no high before it. Both .NET decoders emit
			// ED B0 80..ED BF BF here.
			cp = kReplacement;
		}

		w += encodeUtf8(cp, out + w);
		++i;
	}
	return w;
}

// ─── Modified UTF-8 ──────────────────────────────────────────────────────────

/// Bytes needed to transcode @p units MUTF-8 characters, or 0 if that does not
/// fit.
constexpr std::size_t utf8CapacityForMutf8(std::size_t units) noexcept
{
	if (units == 0) return 0;
	if (!bounds::mulFits(kUtf8BytesPerMutf8Unit, units)) return 0;
	return units * kUtf8BytesPerMutf8Unit;
}

/// What @ref mutf8ToUtf8Ex produced and how much of the input it used.
struct Mutf8Result
{
	std::size_t written = 0;  ///< bytes written to out
	std::size_t consumed = 0; ///< bytes read from in; always <= inBytes
};

/// Transcode MUTF-8 to UTF-8, reporting the cursor as well as the length.
///
/// MUTF-8 (JVM 4.4.7, DEX) differs from UTF-8 in two ways, and this is the only
/// place in the tree that handles both: U+0000 is written C0 80, and a
/// supplementary character is written as its two surrogates, each as a
/// three-byte sequence. Both are folded back here.
///
/// Stops at whichever of @p units characters or @p inBytes bytes comes first.
/// The cursor advances by at least one byte on every iteration -- including for
/// a lead byte in F8..FF, which is in no MUTF-8 sequence at all -- so the loop
/// terminates for every input. src/jvm_parser/jvm_const_pool.cpp:85 does not
/// fold anything but C0 80 and leaves the surrogates in place; the C0 80 fold
/// produces an embedded NUL, which is why src/jvm_parser/jvm_class_parser.cpp:253
/// must split the Exceptions attribute with bstr::terminatorAt rather than by
/// hand.
inline Mutf8Result
mutf8ToUtf8Ex(const std::uint8_t* in, std::size_t inBytes, std::size_t units, char* out, std::size_t outCap) noexcept
{
	Mutf8Result r;
	if (in == nullptr || out == nullptr) return r;
	const std::size_t need = utf8CapacityForMutf8(units);
	if (need == 0) return r;
	if (outCap < need) return r;

	std::size_t pos = 0;
	std::size_t done = 0;
	while (done < units && pos < inBytes)
	{
		const std::uint8_t c = in[pos];
		const std::size_t left = inBytes - pos;

		if (c == 0xC0 && left >= 2 && in[pos + 1] == 0x80)
		{
			// The MUTF-8 NUL. Folded to a real 0x00 byte, which is why the
			// result is not a C string.
			out[r.written++] = '\0';
			pos += 2;
			++done;
		}
		else if (c < 0x80)
		{
			// Includes a raw 0x00, which DEX files contain in practice even
			// though the format says they should not.
			out[r.written++] = static_cast<char>(c);
			pos += 1;
			++done;
		}
		else if ((c & 0xE0) == 0xC0 && left >= 2 && (in[pos + 1] & 0xC0) == 0x80)
		{
			const std::uint32_t cp =
				(static_cast<std::uint32_t>(c & 0x1Fu) << 6) | static_cast<std::uint32_t>(in[pos + 1] & 0x3Fu);
			// An overlong two-byte form -- C0 or C1 as the lead -- encodes a
			// value below 0x80, which has a one-byte form. This used to decode
			// it and then re-encode it short, so C0 AF came out as '/' and
			// C1 BF as 0x7F: a filter that inspected the bytes saw neither.
			// That is the oldest UTF-8 attack there is, and MUTF-8 admits
			// exactly one overlong, C0 80 for the NUL, which the branch above
			// has already taken. Everything else here is malformed and gets the
			// replacement character, like every other malformed sequence in
			// this decoder.
			r.written += encodeUtf8(cp < 0x80u ? kReplacement : cp, out + r.written);
			pos += 2;
			++done;
		}
		else if ((c & 0xF0) == 0xE0 && left >= 3 && (in[pos + 1] & 0xC0) == 0x80 && (in[pos + 2] & 0xC0) == 0x80)
		{
			const std::uint32_t cp = (static_cast<std::uint32_t>(c & 0x0Fu) << 12)
								   | (static_cast<std::uint32_t>(in[pos + 1] & 0x3Fu) << 6)
								   | static_cast<std::uint32_t>(in[pos + 2] & 0x3Fu);

			// A supplementary character: two three-byte sequences, six bytes,
			// two units by the declared count, one scalar value out.
			if (isHighSurrogate(cp) && left >= 6 && done + 1 < units && (in[pos + 3] & 0xF0) == 0xE0
				&& (in[pos + 4] & 0xC0) == 0x80 && (in[pos + 5] & 0xC0) == 0x80)
			{
				const std::uint32_t lo = (static_cast<std::uint32_t>(in[pos + 3] & 0x0Fu) << 12)
									   | (static_cast<std::uint32_t>(in[pos + 4] & 0x3Fu) << 6)
									   | static_cast<std::uint32_t>(in[pos + 5] & 0x3Fu);
				if (isLowSurrogate(lo))
				{
					r.written += encodeUtf8(combineSurrogates(cp, lo), out + r.written);
					pos += 6;
					done += 2;
					continue;
				}
			}
			// encodeUtf8 turns an unpaired surrogate into U+FFFD; nothing here
			// has to test for it a second time. An overlong three-byte form --
			// anything below 0x800, which E0 80..9F leads produce -- is the
			// same attack as the two-byte case one branch up: E0 80 AF decoded
			// and re-encoded as '/'.
			r.written += encodeUtf8(cp < 0x800u ? kReplacement : cp, out + r.written);
			pos += 3;
			++done;
		}
		else
		{
			// A truncated sequence, a stray continuation byte, or a lead in
			// F8..FF, which MUTF-8 has no sequence for. One byte is consumed so
			// the cursor cannot stall on it.
			r.written += encodeUtf8(kReplacement, out + r.written);
			pos += 1;
			++done;
		}
	}

	r.consumed = pos;
	return r;
}

/// mutf8ToUtf8Ex, for callers that only need the output length.
inline std::size_t
mutf8ToUtf8(const std::uint8_t* in, std::size_t inBytes, std::size_t units, char* out, std::size_t outCap) noexcept
{
	return mutf8ToUtf8Ex(in, inBytes, units, out, outCap).written;
}

// ─── decimal runs ────────────────────────────────────────────────────────────

/// UINT64_MAX split for the accumulate-without-wrapping test below.
inline constexpr std::uint64_t kMaxU64Div10 = UINT64_MAX / 10;
inline constexpr std::uint64_t kMaxU64Mod10 = UINT64_MAX % 10;

/// Read the run of decimal digits beginning at @p start.
///
/// @return true when at least one digit was read and the value did not wrap.
///         @p value is the value and @p end the index one past the last digit.
///         On false both are left at 0 and @p start respectively.
///
/// The empty run is a failure, not zero. src/jvm_parser/jvm_jar_reader.cpp:152
/// needs this and has neither half: it tests `path.substr(0, 19) !=
/// "META-INF/versions/"` against a literal 18 characters long, so the
/// comparison is false for every path in every JAR, multiReleaseVersion returns
/// 0 unconditionally, and multi-release selection is dead code -- every
/// versioned class is added to the module a second time under a junk package
/// name derived from the unstripped path.
inline bool
decimalRun(const char* in, std::size_t n, std::size_t start, std::uint64_t& value, std::size_t& end) noexcept
{
	value = 0;
	end = start;
	if (in == nullptr || start >= n) return false;

	std::uint64_t acc = 0;
	std::size_t i = start;
	while (i < n && in[i] >= '0' && in[i] <= '9')
	{
		const std::uint64_t d = static_cast<std::uint64_t>(in[i] - '0');
		// acc * 10 + d, tested before it is formed. Compared against constants
		// so no division appears in the query at all.
		if (acc > kMaxU64Div10) return false;
		if (acc == kMaxU64Div10 && d > kMaxU64Mod10) return false;
		acc = acc * 10 + d;
		++i;
	}

	if (i == start) return false;
	value = acc;
	end = i;
	return true;
}

} // namespace txt
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_TEXT_TRANSCODE_H
