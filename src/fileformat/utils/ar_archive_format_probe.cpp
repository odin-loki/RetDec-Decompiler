/**
 * @file src/fileformat/utils/ar_archive_format_probe.cpp
 * @brief AR member format probing (lattice hints per member).
 * @copyright (c) 2026, MIT license
 */

#include <memory>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include <llvm/Object/Archive.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include "retdec/fileformat/utils/ar_archive_format_probe.h"
#include "retdec/fileformat/utils/format_detection.h"

using namespace llvm;
using namespace llvm::object;

namespace retdec {
namespace fileformat {

namespace
{

/// Matches @c LATTICE_BUFFER_SIZE in @c format_detection.cpp — only this prefix is needed for hints.
constexpr std::size_t kLatticePrefixBytes = 512;

bool arProbeDiagEnabled()
{
	const char *e = std::getenv("RETDEC_FORMAT_AR_PROBE_DIAG");
	return e != nullptr && e[0] != '\0' && e[0] != '0';
}

/**
 * @c RETDEC_FORMAT_AR_PROBE_THREADS: unset / empty / @c 0 / @c 1 → single-threaded lattice.
 * @c auto → @c std::thread::hardware_concurrency() (min 1). Integer ≥2 → cap [2, 64].
 */
std::size_t arProbeLatticeWorkerBudget()
{
	const char *e = std::getenv("RETDEC_FORMAT_AR_PROBE_THREADS");
	if (e == nullptr || e[0] == '\0')
	{
		return 1;
	}
	if (std::strcmp(e, "0") == 0 || std::strcmp(e, "1") == 0)
	{
		return 1;
	}
	if (std::strcmp(e, "auto") == 0)
	{
		unsigned hc = std::thread::hardware_concurrency();
		return hc != 0u ? static_cast<std::size_t>(hc) : std::size_t{1};
	}
	char *end = nullptr;
	long n = std::strtol(e, &end, 10);
	if (end == e || n < 2)
	{
		return 1;
	}
	if (n > 64)
	{
		n = 64;
	}
	return static_cast<std::size_t>(n);
}

/**
 * Pick a coarse Format from lattice hints (not a full @c detectFileFormat — no stream PE validate).
 * Tie-break order: ELF, PE, COFF, Mach-O slice, Mach-O fat lattice, Intel HEX, Java class lattice.
 */
Format dominantFormatFromHints(const FormatLatticeHints &h)
{
	struct Cand
	{
		Format f;
		unsigned w;
	};
	Cand cands[] = {
			{Format::ELF, h.elfStrength},
			{Format::PE, h.peStrength},
			{Format::COFF, h.coffStrength},
			{Format::MACHO, std::max(h.machoSliceStrength, h.machoFatLatticeStrength)},
			{Format::INTEL_HEX, h.ihexStrength},
			// Java .class is not in Format enum — folded into UNKNOWN when java lattice wins
	};

	unsigned best = 0;
	Format pick = Format::UNKNOWN;
	for (const auto &c : cands)
	{
		if (c.w > best)
		{
			best = c.w;
			pick = c.f;
		}
	}
	if (best == 0 && h.javaClassLatticeStrength >= 80)
	{
		return Format::UNKNOWN;
	}
	if (best == 0 && h.arArchiveStrength >= 80)
	{
		return Format::UNKNOWN;
	}
	return pick;
}

struct ScratchRow
{
	std::string name;
	std::size_t rawSize = 0;
	bool bufferOk = false;
	std::vector<std::uint8_t> latticePrefix;
};

void computeLatticeForIndices(
		const std::vector<ScratchRow> &scratch,
		const std::vector<std::size_t> &probeIdx,
		std::vector<FormatLatticeHints> &hintsOut,
		std::vector<Format> &domOut,
		std::size_t beg,
		std::size_t end)
{
	for (std::size_t j = beg; j < end; ++j)
	{
		const std::size_t idx = probeIdx[j];
		const auto &pre = scratch[idx].latticePrefix;
		FormatLatticeHints h = computeFormatLatticeHints(pre.data(), pre.size());
		hintsOut[idx] = h;
		domOut[idx] = dominantFormatFromHints(h);
	}
}

/**
 * Run lattice on @a probeIdx rows in parallel (chunked @c std::async). Output vectors must be
 * sized to @c scratch.size(); only @a probeIdx slots are written.
 */
std::size_t runLatticeParallel(
		const std::vector<ScratchRow> &scratch,
		const std::vector<std::size_t> &probeIdx,
		std::vector<FormatLatticeHints> &hintsOut,
		std::vector<Format> &domOut,
		std::size_t maxWorkers)
{
	const std::size_t nJobs = probeIdx.size();
	if (nJobs == 0)
	{
		return 1;
	}
	maxWorkers = std::max<std::size_t>(1, std::min(maxWorkers, nJobs));
	const std::size_t chunk = (nJobs + maxWorkers - 1) / maxWorkers;
	std::vector<std::future<void>> futures;
	futures.reserve(maxWorkers);

	for (std::size_t w = 0, launched = 0; w < maxWorkers && launched < nJobs; ++w)
	{
		const std::size_t beg = launched;
		const std::size_t end = std::min(beg + chunk, nJobs);
		launched = end;
		futures.push_back(std::async(
				std::launch::async,
				computeLatticeForIndices,
				std::cref(scratch),
				std::cref(probeIdx),
				std::ref(hintsOut),
				std::ref(domOut),
				beg,
				end));
	}

	for (auto &f : futures)
	{
		f.get();
	}
	return futures.size();
}

} // namespace

bool probeArArchiveMemberFormats(
		const std::uint8_t *data,
		std::size_t size,
		ArArchiveFormatProbeSummary &out,
		std::size_t maxMembersToProbe)
{
	out = ArArchiveFormatProbeSummary{};
	out.clearHistogram();
	out.latticeProbeWorkerCount = 1;

	if (data == nullptr || size < 8)
	{
		out.errorMessage = "empty or too small buffer";
		return false;
	}

	if (std::memcmp(data, "!<arch>\n", 8) != 0
			&& std::memcmp(data, "!<thin>\n", 8) != 0)
	{
		out.errorMessage = "not an ar archive magic";
		return false;
	}

	out.isArchive = true;
	out.isThinArchive = (std::memcmp(data, "!<thin>\n", 8) == 0);

	const std::size_t cap = maxMembersToProbe == 0 ? 4096 : maxMembersToProbe;

	std::unique_ptr<MemoryBuffer> buf = MemoryBuffer::getMemBufferCopy(
			StringRef(reinterpret_cast<const char *>(data), size),
			"ar_probe");
	if (!buf)
	{
		out.errorMessage = "MemoryBuffer::getMemBufferCopy failed";
		if (arProbeDiagEnabled())
		{
			llvm::errs() << "RETDEC_FORMAT_AR_PROBE_DIAG: parse_failed reason=membuffer_copy "
					<< "archive_magic_ok=1 thin_archive="
					<< (out.isThinArchive ? "yes" : "no") << "\n";
		}
		return false;
	}

	Error llvmErr = Error::success();
	std::unique_ptr<Archive> arch = std::make_unique<Archive>(buf->getMemBufferRef(), llvmErr);
	if (llvmErr)
	{
		out.errorMessage = toString(std::move(llvmErr));
		if (arProbeDiagEnabled())
		{
			llvm::errs() << "RETDEC_FORMAT_AR_PROBE_DIAG: parse_failed reason=llvm_archive "
					<< "archive_magic_ok=1 thin_archive="
					<< (out.isThinArchive ? "yes" : "no") << " msg=" << out.errorMessage
					<< "\n";
		}
		return false;
	}

	out.parseOk = true;
	out.isThinArchive = arch->isThin();

	std::vector<ScratchRow> scratch;
	scratch.reserve(32);

	Error childErr = Error::success();
	for (const auto &child : arch->children(childErr))
	{
		if (childErr)
		{
			out.errorMessage = toString(std::move(childErr));
			break;
		}

		if (out.membersSeen >= cap)
		{
			out.truncatedAtMemberCap = true;
			break;
		}

		++out.membersSeen;

		ScratchRow row;
		auto nameOrErr = child.getName();
		row.name = nameOrErr ? nameOrErr->str() : std::string("invalid_name");

		auto bufferOrErr = child.getBuffer();
		if (!bufferOrErr)
		{
			++out.membersSkippedNoBuffer;
			row.bufferOk = false;
			scratch.push_back(std::move(row));
			continue;
		}

		StringRef payload = *bufferOrErr;
		row.rawSize = static_cast<std::size_t>(payload.size());
		row.bufferOk = true;
		++out.membersProbed;

		const std::size_t take = std::min(row.rawSize, kLatticePrefixBytes);
		row.latticePrefix.resize(take);
		if (take != 0)
		{
			std::memcpy(
					row.latticePrefix.data(),
					payload.data(),
					take);
		}

		scratch.push_back(std::move(row));
	}

	if (childErr)
	{
		out.errorMessage = toString(std::move(childErr));
	}

	std::vector<std::size_t> probeIdx;
	probeIdx.reserve(scratch.size());
	for (std::size_t i = 0; i < scratch.size(); ++i)
	{
		if (scratch[i].bufferOk)
		{
			probeIdx.push_back(i);
		}
	}

	const std::size_t budget = arProbeLatticeWorkerBudget();
	const bool useParallel = (budget >= 2 && probeIdx.size() >= 2);

	std::vector<FormatLatticeHints> hintsVec(scratch.size());
	std::vector<Format> domVec(scratch.size(), Format::UNKNOWN);

	if (useParallel)
	{
		out.latticeProbeWorkerCount = runLatticeParallel(
				scratch,
				probeIdx,
				hintsVec,
				domVec,
				budget);
	}
	else
	{
		computeLatticeForIndices(scratch, probeIdx, hintsVec, domVec, 0, probeIdx.size());
	}

	out.members.reserve(scratch.size());
	for (std::size_t i = 0; i < scratch.size(); ++i)
	{
		ArArchiveMemberFormatProbe row;
		row.name = std::move(scratch[i].name);
		row.rawSize = scratch[i].rawSize;
		row.bufferOk = scratch[i].bufferOk;

		if (row.bufferOk)
		{
			row.hints = hintsVec[i];
			row.dominantFormat = domVec[i];
			const std::size_t idx = static_cast<std::size_t>(row.dominantFormat);
			if (idx < out.dominantFormatHistogram.size())
			{
				++out.dominantFormatHistogram[idx];
			}
		}

		if (arProbeDiagEnabled())
		{
			llvm::errs() << "RETDEC_FORMAT_AR_PROBE_DIAG: member=" << row.name
					<< " size=" << row.rawSize
					<< " dominant_format=" << static_cast<int>(row.dominantFormat)
					<< " elf=" << row.hints.elfStrength << " pe=" << row.hints.peStrength
					<< " coff=" << row.hints.coffStrength << "\n";
		}

		out.members.push_back(std::move(row));
	}

	if (arProbeDiagEnabled() && out.truncatedAtMemberCap)
	{
		llvm::errs() << "RETDEC_FORMAT_AR_PROBE_DIAG: note=truncated_at_member_cap cap="
				<< cap << "\n";
	}

	return true;
}

} // namespace fileformat
} // namespace retdec
