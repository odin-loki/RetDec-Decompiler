/**
 * @file include/retdec/utils/section_map.h
 * @brief Translating a virtual address to a file offset, once.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Every format this tree reads is mapped differently from the way it is stored:
 * a PE maps at SectionAlignment (0x1000) and stores at FileAlignment (0x200),
 * and every ELF segment after the first is skewed the same way. So every loader
 * needs the same function -- given an address and a section table, which
 * section contains it and where do that section's bytes actually live -- and
 * three of them had written their own:
 *
 *   src/cli_parser/pe_reader.cpp   PeReader::rvaToOffset
 *   src/loader_sim/loader_sim.cpp  LoaderSim::vaToOffset
 *   src/func_boundary/func_boundary.cpp  FuncBoundaryDetector::vaToOffset
 *
 * Two of the three tested containment as
 *
 *     rva >= s.va && rva < s.va + std::max(s.virtualSize, s.rawSize)
 *
 * with every operand a uint32 out of the file. That sum wraps, and ESBMC
 * returns the witness: va = 2692743171, span = 2675966078, rva = 3221225600.
 * The true end is 5368709249, so the section really does contain the address --
 * but the sum wrapped to 1073741953, the test said no, and the walk went on to
 * ask the next section instead. A file chooses which section an address
 * resolves into, or whether it resolves at all.
 *
 * The third had it right, in 64-bit arithmetic, having been fixed once already.
 * One rule in three places drifts; this is the rule.
 *
 * ## The sentinel
 *
 * `kUnmapped` is the ONLY out-of-range answer. An address that is in no section
 * -- or in a section whose bytes are not in the file, which is what a .bss or a
 * virtual-size overhang is -- is unmapped, and every other return is an offset
 * a caller may read at. The alternative, which two of these had, is returning
 * `rawOffset + delta` unchecked: a file-controlled sum that is neither in range
 * nor equal to the sentinel, so a caller testing `off != kUnmapped` gets an
 * offset outside the buffer that passes the test.
 *
 * Proved in tests/verification/section_map_proof.cpp.
 */

#ifndef RETDEC_UTILS_SECTION_MAP_H
#define RETDEC_UTILS_SECTION_MAP_H

#include "retdec/utils/bounds.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace secmap {

// Every offset here comes off the wire as a 64-bit field and is narrowed to
// std::size_t to reach retdec/utils/bounds.h, whose helpers are std::size_t
// throughout. On a build where size_t is narrower than 64 bits that narrowing
// truncates -- and truncates only the values handed to the guard, while the
// sums beside it stay 64-bit, so the guard would be answering about different
// numbers than the ones used. The proofs in tests/verification/ run against a
// 64-bit model and would not see it either.
//
// No target this project configures is 32-bit -- CMakePresets.json is x64
// throughout -- so rather than widening the kernel and its proofs for a
// configuration that does not exist, this makes the assumption a build error
// instead of a silent truncation.
static_assert(
	sizeof(std::size_t) >= sizeof(std::uint64_t),
	"section_map narrows 64-bit file offsets to std::size_t to call "
	"retdec/utils/bounds.h; on a 32-bit target that truncates and the bounds "
	"checks stop matching the arithmetic they guard");

/// The answer for an address that no section maps into the file.
inline constexpr std::uint64_t kUnmapped = ~static_cast<std::uint64_t>(0);

/// One section, in the terms every caller already has.
///
/// All four fields are 64-bit even where the format stores 32, because the
/// arithmetic below must not be done in the width the file chose.
struct Section
{
	std::uint64_t address = 0;   ///< first address of the section as mapped
	std::uint64_t virtSize = 0;  ///< extent in memory (may exceed rawSize)
	std::uint64_t rawSize = 0;   ///< bytes actually stored in the file
	std::uint64_t rawOffset = 0; ///< offset of those bytes in the file
};

/// True when @p addr lies within the mapped extent of @p s.
///
/// The extent is the larger of the two sizes. That is what both callers this
/// replaces did, so the refactor changes no answers -- but it is worth being
/// exact about what it means, because a proved implementation of the wrong
/// contract is worse than an unproved one: it gets trusted.
///
/// The Windows loader maps VirtualSize when VirtualSize is non-zero, and falls
/// back to SizeOfRawData only when it is zero. SizeOfRawData is rounded up to
/// FileAlignment and is routinely LARGER than VirtualSize, so `max` maps
/// addresses in [VirtualSize, SizeOfRawData) that the real loader does not.
/// This is therefore more permissive than a loader, in the direction of
/// resolving an address the process would fault on.
///
/// It is kept because tightening it changes which addresses resolve, and that
/// is a corpus-measurable behaviour change rather than a bug fix -- and because
/// the permissive form is what handles the linkers that write VirtualSize as 0.
/// src/func_boundary/func_boundary.cpp deliberately uses the stricter test
/// (`va < s.end`, the virtual extent alone) and is NOT routed through here for
/// that reason; it is already 64-bit and already maintains the sentinel.
///
/// The sum is never formed: the delta is compared against the span instead.
constexpr bool contains(const Section& s, std::uint64_t addr) noexcept
{
	const std::uint64_t span = s.virtSize > s.rawSize ? s.virtSize : s.rawSize;
	if (span == 0) return false;
	if (addr < s.address) return false;
	// addr - s.address cannot wrap now, and comparing the delta against the
	// span never forms s.address + span at all.
	return addr - s.address < span;
}

/// File offset of @p addr, or @ref kUnmapped.
///
/// Returns kUnmapped when: no section contains the address; the section
/// contains it only in the virtual tail it does not store; or the resulting
/// offset is not inside a file of @p fileSize bytes. The last of those is what
/// makes the sentinel meaningful -- every other answer is readable.
///
/// The first containing section wins, which is what all three callers did and
/// what a loader does. Overlapping sections are malformed; this does not try to
/// adjudicate between them, it is deterministic about which it picks.
inline std::uint64_t
addressToOffset(std::uint64_t addr, const Section* sections, std::size_t count, std::size_t fileSize) noexcept
{
	if (sections == nullptr) return kUnmapped;
	for (std::size_t i = 0; i < count; ++i)
	{
		const Section& s = sections[i];
		if (!contains(s, addr)) continue;

		const std::uint64_t delta = addr - s.address;
		// Mapped, but not stored: the virtual tail of the section.
		if (delta >= s.rawSize) return kUnmapped;
		// rawOffset and delta are both file-controlled, so the sum is formed
		// only once it is known to be representable and inside the file.
		if (!bounds::addFits(static_cast<std::size_t>(s.rawOffset), static_cast<std::size_t>(delta))) return kUnmapped;
		const std::uint64_t off = s.rawOffset + delta;
		if (!bounds::rangeFits(static_cast<std::size_t>(off), fileSize, 1)) return kUnmapped;
		return off;
	}
	return kUnmapped;
}

/// How many bytes are readable at @p addr, at most @p want.
///
/// Zero when the address is unmapped, so a caller can ask for a span without
/// asking for the offset first and then bounding it themselves -- which is the
/// step each of the three callers had to remember, and one of them did not.
inline std::size_t readableAt(
	std::uint64_t addr, const Section* sections, std::size_t count, std::size_t fileSize, std::size_t want) noexcept
{
	const std::uint64_t off = addressToOffset(addr, sections, count, fileSize);
	if (off == kUnmapped) return 0;
	return bounds::clamp(want, bounds::remaining(static_cast<std::size_t>(off), fileSize));
}

} // namespace secmap
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_SECTION_MAP_H
