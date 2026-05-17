/**
 * @file include/retdec/fileformat/utils/ar_archive_format_probe.h
 * @brief Probe each member of a Unix / GNU `ar` archive with the format lattice (P1.1 slice).
 * @copyright (c) 2026, MIT license
 *
 * Uses LLVM's archive parser (same family as @c ar_extractor). Intended for tooling /
 * diagnostics: classify nested objects (ELF / COFF / Mach-O / …) from the first bytes of
 * each member without extracting to disk. Work is bounded by @a maxMembersToProbe.
 */

#ifndef RETDEC_FILEFORMAT_UTILS_AR_ARCHIVE_FORMAT_PROBE_H
#define RETDEC_FILEFORMAT_UTILS_AR_ARCHIVE_FORMAT_PROBE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "retdec/fileformat/utils/format_detection.h"

namespace retdec {
namespace fileformat {

/// One archive member: name, raw size, lattice hints, and a coarse @c Format guess.
struct ArArchiveMemberFormatProbe
{
	std::string name;
	std::size_t rawSize = 0;
	bool bufferOk = false;
	FormatLatticeHints hints{};
	/// Best-effort dominant format from @c hints (highest strength; ties: ELF, PE, COFF, Mach-O, IHEX).
	Format dominantFormat = Format::UNKNOWN;
};

struct ArArchiveFormatProbeSummary
{
	bool isArchive = false;
	bool isThinArchive = false;
	bool parseOk = false;
	std::string errorMessage;
	std::size_t membersSeen = 0;
	std::size_t membersProbed = 0;
	std::size_t membersSkippedNoBuffer = 0;
	/// @c true if iteration stopped because @a maxMembersToProbe was reached while more members may exist.
	bool truncatedAtMemberCap = false;
	/// Lattice phase: @c 1 = single-threaded; @c >1 = that many worker tasks (chunked @c std::async).
	std::size_t latticeProbeWorkerCount = 1;
	/// Counts per @c Format enum value (index = static_cast<std::size_t>(Format)).
	std::array<std::size_t, 8> dominantFormatHistogram{};
	std::vector<ArArchiveMemberFormatProbe> members;

	void clearHistogram()
	{
		dominantFormatHistogram.fill(0);
	}
};

/**
 * Parse @a data as an `ar` / thin archive and run @c computeFormatLatticeHints on the start
 * of each member's payload (up to the usual lattice window inside @c computeFormatLatticeHints).
 *
 * @param data archive bytes (must include magic @c "!<arch>\\n" or @c "!<thin>\\n" for success).
 * @param size byte length
 * @param out filled on success or partial failure (check @c parseOk)
 * @param maxMembersToProbe cap iterations (0 = default 4096)
 * @return @c true if LLVM accepted the archive container; @c false if not an archive or parse error
 *
 * Parallelism: after a sequential LLVM parse, per-member @c computeFormatLatticeHints may run in
 * parallel when @c RETDEC_FORMAT_AR_PROBE_THREADS is set to @c auto or an integer @c >= 2
 * (capped). Unset / @c 0 / @c 1 keeps a single-threaded lattice pass (default).
 */
bool probeArArchiveMemberFormats(
		const std::uint8_t *data,
		std::size_t size,
		ArArchiveFormatProbeSummary &out,
		std::size_t maxMembersToProbe = 4096);

} // namespace fileformat
} // namespace retdec

#endif
