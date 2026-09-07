/**
 * @file src/utils/gpu_scanner_cpu.cpp
 * @brief CPU-only implementation of GpuScanner — compiled when CUDA is absent.
 * @copyright (c) 2024 RetDec contributors, MIT license
 *
 * When RETDEC_ENABLE_CUDA=OFF (or no CUDA compiler is present),
 * src/utils/CMakeLists.txt compiles this file instead of gpu_scanner.cu. The
 * interface is identical; every method uses CPU algorithms so the rest of the
 * codebase compiles unchanged.
 *
 * The host-side byte, nibble and window arithmetic is NOT written again here.
 * It is compiled out of gpu_scanner.cu, whose leading section is plain C++ and
 * whose CUDA half is behind RETDEC_GPU_SCANNER_HOST_ONLY. That include is the
 * whole point: the two files are alternatives, only one is ever in the library,
 * and while each kept its own copy of the arithmetic a fix applied to this file
 * left the CUDA build with the original wrong answers -- and untested, because
 * a machine without nvcc cannot build gpu_scanner.cu at all. Now the CPU build
 * compiles that file's text, so the suite covers the arithmetic both builds
 * use.
 */

#include <algorithm>
#include <string>
#include <vector>

#include "retdec/utils/float_predicate.h"
#include "retdec/utils/gpu_scanner.h"

#define RETDEC_GPU_SCANNER_HOST_ONLY 1
#include "gpu_scanner.cu"

namespace retdec {
namespace utils {

struct GpuScanner::Impl
{
	std::vector<uint8_t> h_fileBytes;
	std::string h_fileNibs;
};

GpuScanner::GpuScanner(): impl_(new Impl()) {}
GpuScanner::~GpuScanner()
{
	delete impl_;
}

bool GpuScanner::isAvailable() const
{
	return false;
}
std::string GpuScanner::deviceName() const
{
	return "CPU fallback (CUDA disabled)";
}

void GpuScanner::uploadFile(const uint8_t* data, std::size_t size)
{
	// Nothing is touched until both the pointer and the sizing are known good,
	// because every statement in the old body was already past the point of no
	// return by the time it could have noticed: assign() had copied from the
	// pointer and resize() had taken the wrapped length. gpuscan::nibblesFor
	// asks first and builds into a temporary, so a refusal cannot leave a
	// half-performed upload behind.
	std::string nibs;
	if (!gpuscan::nibblesFor(data, size, nibs))
	{
		// An empty scanner makes batchMatch return unmatched results and
		// fileEntropy return 0.0, which is what every caller already handles
		// for a file it could not read. Clearing matters as much as refusing:
		// a caller reading the results of a refused upload must not be shown
		// the file before it.
		impl_->h_fileBytes.clear();
		impl_->h_fileNibs.clear();
		return;
	}

	impl_->h_fileBytes.assign(data, data + size);
	impl_->h_fileNibs = std::move(nibs);
}

std::vector<SigMatchResult>
GpuScanner::batchMatch(const std::vector<std::string>& patterns, std::size_t startOffset, std::size_t stopOffset) const
{
	const std::string& nibs = impl_->h_fileNibs;
	const std::size_t n = patterns.size();
	std::vector<SigMatchResult> results(n);

	// `startOffset * 2` and `std::min(stopOffset * 2 + 1, nibs.size() - 1)`
	// used to be computed here, in the same file as -- and fifty-two lines below --
	// the NIBBLES_PER_BYTE constant that had just been introduced to stop
	// exactly that. Both wraps produce a wrong answer rather than a crash, in
	// opposite directions, and both are demonstrated in gpuscan::nibbleWindow's
	// comment and pinned by
	// GpuScannerTests.AStartOffsetPastTheEndOfTheFileDoesNotMatchAtItsStart
	// and ...AStopOffsetPastTheEndOfTheFileStillScansTheWholeFile.
	std::size_t startNib = 0;
	std::size_t endNib = 0;
	if (!gpuscan::nibbleWindow(startOffset, stopOffset, nibs.size(), startNib, endNib))
	{
		return results;
	}

	for (std::size_t i = 0; i < n; ++i)
	{
		// The match loop itself lived here AND in gpu_scanner.cu, and the two
		// disagreed: this one treated '/' as a don't-care and the other did
		// not, so "de/d" over DE AD BE EF gave bestRatio 1.000000 / totalNibs 3
		// here and 0.750000 / 4 there. One implementation now, in the prologue
		// both halves of this class share.
		results[i] = gpuscan::matchOne(nibs, patterns[i], startNib, endNib);
	}

	return results;
}

double GpuScanner::fileEntropy(std::size_t startOffset, std::size_t stopOffset) const
{
	const auto& bytes = impl_->h_fileBytes;

	// `sz = hi - lo + 1` used to be formed before anything checked that lo was
	// at or below hi. gpuscan::byteWindow answers that question instead, and
	// refusing an empty window is the whole fix: for a startOffset past the end
	// of the file the subtraction underflowed to a number near SIZE_MAX, the
	// histogram loop then did not run at all, every `hist[b] / sz` was 0, and
	// the function returned 0.0 -- which is also exactly what it returns for a
	// genuinely uniform region.
	std::size_t lo = 0;
	std::size_t hi = 0;
	if (!gpuscan::byteWindow(startOffset, stopOffset, bytes.size(), lo, hi)) return 0.0;
	const std::size_t sz = hi - lo + 1;

	uint32_t hist[fpred::kByteValues] = {};
	for (std::size_t i = lo; i <= hi; ++i)
		hist[bytes[i]]++;

	// fpred::entropyBits refuses a histogram that does not sum to the total it
	// was given, so the underflow above cannot be re-introduced without the
	// measurement failing rather than answering 0.0. It is proved over every
	// behaviour of the logarithm in tests/verification/float_predicate_proof.cpp:
	// no input makes it overflow, produce a NaN, divide by zero, or return a
	// value outside [0, 8].
	double e = 0.0;
	if (!fpred::entropyBits(hist, sz, e)) return 0.0;
	return e;
}

std::vector<std::size_t> GpuScanner::findAll(const std::vector<uint8_t>& needle) const
{
	std::vector<std::size_t> offsets;
	const auto& bytes = impl_->h_fileBytes;
	if (bytes.empty() || needle.empty() || needle.size() > bytes.size()) return offsets;
	const uint8_t* b = bytes.data();
	const uint8_t* e = b + bytes.size();
	const uint8_t* n = needle.data();
	for (auto it = b; (it = std::search(it, e, n, n + needle.size())) != e; ++it)
		offsets.push_back(static_cast<std::size_t>(it - b));
	return offsets;
}

} // namespace utils
} // namespace retdec
