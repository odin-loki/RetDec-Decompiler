/**
 * @file src/utils/gpu_scanner.cu
 * @brief CUDA kernels for GPU-accelerated signature matching and entropy.
 * @copyright (c) 2024 RetDec contributors, MIT license
 *
 * Build requirements:
 *   - CUDA toolkit >= 11.0
 *   - Compute capability >= 6.0 (Pascal) recommended; 3.5 minimum
 *   - cmake: find_package(CUDAToolkit REQUIRED)
 *
 * Architecture:
 *   batchMatchKernel  — one CUDA block per signature pattern.
 *                       Each block slides the pattern across the file region
 *                       using shared memory to cache the pattern.
 *                       Threads within a block cooperate to scan positions.
 *
 *   entropyKernel     — standard parallel histogram + reduction.
 *                       256-bucket byte frequency, then log2 reduction.
 *
 *   findAllKernel     — one thread per candidate start position, checks
 *                       needle match using coalesced reads.
 *
 * Why the first section of this file has no CUDA in it
 * ----------------------------------------------------
 * Two files implement GpuScanner: this one, which src/utils/CMakeLists.txt
 * compiles with nvcc when a CUDA compiler is found, and gpu_scanner_cpu.cpp,
 * which it compiles instead when one is not. Exactly one of the two is ever in
 * the library, and each used to carry its own copy of the host-side byte,
 * nibble and window arithmetic -- three copies of the byte-to-nibble
 * expansion between them, and two of the scan-window computation. When that
 * arithmetic was corrected, only gpu_scanner_cpu.cpp was corrected, so a CUDA
 * build kept every original wrong answer and no test could see it: no suite
 * could link this file, because a machine without nvcc cannot build a .cu at
 * all.
 *
 * The arithmetic therefore lives exactly once now, in the gpuscan namespace
 * below. It is plain C++ over sizes and a std::string, with nothing CUDA in
 * it, and everything after it is behind RETDEC_GPU_SCANNER_HOST_ONLY.
 * gpu_scanner_cpu.cpp defines that macro and includes this file, so the
 * fallback build compiles this text instead of a copy of it -- which means
 * tests/utils/gpu_scanner_tests.cpp exercises these three functions on every
 * standalone run, CUDA installed or not, and a wrong answer here fails the
 * suite rather than waiting for someone with a GPU.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "retdec/utils/bounds.h"

namespace retdec {
namespace utils {

/// Host-side arithmetic shared by both GpuScanner implementations.
namespace gpuscan {

/// Nibble characters written per input byte. The scanner's patterns are RetDec
/// nibble strings -- one lowercase hex character per 4 bits -- so the nibble
/// buffer is exactly twice the byte buffer. Every byte-to-nibble step in either
/// implementation goes through the functions below, so the factor is stated
/// here and nowhere else.
inline constexpr std::size_t NIBBLES_PER_BYTE = 2;

/// Expands @p size bytes at @p data into the scanner's nibble string.
///
/// Returns false, leaving @p out empty, when the expansion cannot be done:
///
///  - @p data is null while @p size is not zero. The old body ran
///    `assign(data, data + size)` on whatever it was handed -- unlike
///    conversion.h's bytesToHexString, which checks -- so `uploadFile(nullptr,
///    8)` allocated eight bytes and then copied from address 0. uploadFile is
///    a public entry point taking a raw pointer and a length as two
///    independent arguments, so the two can disagree.
///
///  - `size * NIBBLES_PER_BYTE` is not representable. ESBMC's witness is
///    size = 18446744073709551615, where the product wraps to
///    18446744073709551614 -- one BELOW the byte count, where it should have
///    been twice it. resize() therefore gets fewer nibbles than there are
///    bytes while the loop writes at i*2 and i*2+1 for every i < size, so it
///    runs off the end of the string almost immediately. bounds::mulFits is
///    the proved form of the question and is written as a division, so the
///    product is never formed.
///
/// The nibbles are built in @p out before a caller commits anything else, so a
/// refusal cannot leave an upload half-performed.
inline bool nibblesFor(const std::uint8_t* data, std::size_t size, std::string& out)
{
	static const char hexLut[] = "0123456789abcdef";

	out.clear();
	if (data == nullptr && size != 0)
	{
		return false;
	}
	// Two different questions, and asking only the first was the bug.
	//
	// bounds::mulFits says the product is representable as a std::size_t. It
	// does not say a std::string can hold it, and on this build it cannot:
	// std::string::max_size() is 4611686018427387903, so every size in
	// [2305843009213693952, 9223372036854775807] passed the old guard and then
	// threw std::length_error out of a void GpuScanner::uploadFile. Measured on
	// the real library at each of those three boundaries.
	//
	// A throw out of uploadFile is not a refusal a caller can act on -- the
	// function returns void and every caller in the tree treats an empty
	// scanner as "could not read the file". So the capacity is asked about
	// here, and the answer is the same refusal as every other one.
	if (!bounds::mulFits(size, NIBBLES_PER_BYTE))
	{
		return false;
	}
	const std::size_t need = size * NIBBLES_PER_BYTE;
	if (need > out.max_size())
	{
		return false;
	}

	out.resize(need);
	for (std::size_t i = 0; i < size; ++i)
	{
		out[i * NIBBLES_PER_BYTE] = hexLut[data[i] >> 4];
		out[i * NIBBLES_PER_BYTE + 1] = hexLut[data[i] & 0xF];
	}
	return true;
}

/// The inclusive byte window [@p lo, @p hi] that a caller's byte range
/// [@p startOffset, @p stopOffset] selects in a buffer of @p byteLen bytes.
///
/// Returns false when the range selects nothing, which is a different answer
/// from "this region is uniform". `sz = hi - lo + 1` used to be formed before
/// anything checked that lo was at or below hi, so a startOffset past the end
/// of the file underflowed sz to a number near SIZE_MAX, the histogram loop
/// then did not run at all, every `hist[b] / sz` was 0, and fileEntropy
/// returned 0.0 -- which is also exactly what it returns for a genuinely
/// uniform region. An entropy of 0.0 is the strongest possible statement about
/// a range, and an invalid range was making it.
///
/// A @p stopOffset past the last byte means "to the end of the file", so it
/// saturates. SIZE_MAX is only the largest such value and needs no case of its
/// own.
///
/// gpu_scanner_cpu.cpp's fileEntropy would survive without this check, because
/// fpred::entropyBits refuses a histogram that does not sum to the total it was
/// given and answers 0.0 by that route instead. The CUDA build would not: its
/// fileEntropy divides by the length itself and sizes a kernel launch from it,
/// so an underflowed length there is a grid dimension, not a rejected total.
inline bool byteWindow(
	std::size_t startOffset, std::size_t stopOffset, std::size_t byteLen, std::size_t& lo, std::size_t& hi) noexcept
{
	lo = 0;
	hi = 0;
	if (byteLen == 0) return false;

	const std::size_t stop = bounds::clamp(stopOffset, byteLen - 1);
	if (startOffset > stop) return false;

	lo = startOffset;
	hi = stop;
	return true;
}

/// The inclusive nibble window [@p startNib, @p endNib] that a caller's byte
/// range selects in a nibble string of @p nibLen characters; false when it
/// selects nothing.
///
/// Both ends used to be doubled without asking whether the doubling fit, and
/// each wrap produced a wrong answer in a different direction. Both were
/// measured against the real library on the four-byte file DE AD BE EF with
/// the pattern "dead":
///
///  - `startNib = startOffset * 2` wrapping. batchMatch(pats, 2^63, SIZE_MAX)
///    reported matched=1 at offset=0, where the same call with startOffset 100
///    correctly reported matched=0. A start offset 2^63 bytes past the end of
///    a four-byte file wrapped to 0 and produced a full-confidence signature
///    match at the start of it. Here the multiplication is asked about first,
///    and a start that cannot be expressed in nibbles is past the end of any
///    file that could exist, so the window is empty.
///
///  - `std::min(stopOffset * 2 + 1, nibLen - 1)` wrapping. batchMatch(pats, 0,
///    2^63) reported matched=0, where the same call with stopOffset 1000
///    correctly reported matched=1. The doubling wrapped to 0, the +1 made
///    endNib = 1, and a caller asking to scan a whole file got the window
///    collapsed to a single nibble. Here a stop offset whose nibble index is
///    not representable saturates at the last nibble, which is what "past the
///    end" already meant for every stop offset that did fit.
inline bool nibbleWindow(
	std::size_t startOffset,
	std::size_t stopOffset,
	std::size_t nibLen,
	std::size_t& startNib,
	std::size_t& endNib) noexcept
{
	startNib = 0;
	endNib = 0;
	if (nibLen == 0) return false;

	const std::size_t lastNib = nibLen - 1;

	// A stop offset selects the LAST nibble of its byte, which is the
	// NIBBLES_PER_BYTE - 1 the addition below adds.
	std::size_t stopNib = lastNib;
	if (bounds::mulFits(stopOffset, NIBBLES_PER_BYTE))
	{
		const std::size_t stopFirst = stopOffset * NIBBLES_PER_BYTE;
		if (bounds::addFits(stopFirst, NIBBLES_PER_BYTE - 1))
		{
			stopNib = bounds::clamp(stopFirst + (NIBBLES_PER_BYTE - 1), lastNib);
		}
	}

	if (!bounds::mulFits(startOffset, NIBBLES_PER_BYTE)) return false;
	const std::size_t firstNib = startOffset * NIBBLES_PER_BYTE;
	if (firstNib > stopNib) return false;

	startNib = firstNib;
	endNib = stopNib;
	return true;
}


/// The last nibble position at which a pattern of @p patLen nibbles still fits
/// inside a window that ends at @p endNib; false when it does not fit at all.
///
/// The host side wrote this as `if (patLen == 0 || endNib < patLen) continue;`
/// followed by `maxStart = endNib - patLen + 1`, which is off by one: a pattern
/// that exactly fills the window has patLen == endNib + 1 and belongs at
/// position 0, but `endNib < patLen` throws it away. Measured against the real
/// library: the two-byte file DE AD scanned for "dead" -- an exact whole-file
/// match, four nibbles against a four-nibble file -- reported matched=0,
/// bestRatio=0, totalNibs=0, as though the pattern had never been considered.
/// The device kernel in this file computes `endPos + 1 - patLen` guarded by
/// `endPos + 1 >= patLen`, so the CPU and GPU halves of one class disagreed
/// about the last window that fits.
///
/// Written as `endNib < patLen - 1` with patLen known non-zero, so neither the
/// comparison nor the subtraction can wrap.
inline bool lastStartFor(std::size_t endNib, std::size_t patLen, std::size_t& maxStart) noexcept
{
	maxStart = 0;
	if (patLen == 0) return false;
	if (endNib < patLen - 1) return false;
	maxStart = endNib - (patLen - 1);
	return true;
}

/// Does @p n fit the std::uint32_t the CUDA kernels take their sizes in?
///
/// The kernel parameters are uint32_t; the host side counts in std::size_t. The
/// four conversions between them were bare static_casts, so a file larger than
/// 4 GB -- or a nibble buffer larger than 4 GB, which is a 2 GB file -- reached
/// the device as a small number and the kernel scanned a fraction of it while
/// reporting success. Not reachable through the CPU build, and not a wrong
/// answer this environment can produce, but it is a silent one.
///
/// Asked rather than assumed, at each of the four sites.
inline bool fitsKernelWidth(std::size_t n) noexcept
{
	return n <= static_cast<std::size_t>(UINT32_MAX);
}

/// Nibbles of pattern batchMatchKernel can hold in shared memory.
///
/// The kernel declares `__shared__ char sPat[MAX_PATTERN_NIBS]` and used to
/// fill it with `for (i = threadIdx.x; i < patLen; i += MATCH_BLOCK)` -- no
/// bound but patLen, which is the caller's pattern length. The host side chose
/// the GPU batch on whether a pattern contained '/' and nothing else, so a
/// pattern longer than this wrote past the end of shared memory. The
/// window-length guard below it (`patLen > endPos + 1`) runs after the fill and
/// would not have helped.
///
/// The constant lives here rather than beside the kernel so the host can ask
/// the same question the device answers.
constexpr std::size_t kMaxPatternNibs = 4096;

/// True when a pattern is short enough for the shared-memory path.
///
/// Same shape as fitsKernelWidth: a pattern that does not fit takes the CPU
/// path, which has no such limit -- slower and correct, which is the right way
/// round.
inline bool fitsSharedPattern(std::size_t patternNibs) noexcept
{
	return patternNibs != 0 && patternNibs <= kMaxPatternNibs;
}

/// The best match for one pattern over the nibble window [@p startNib,
/// @p endNib].
///
/// One implementation, called from both halves of this class. There were two,
/// and they DISAGREED: the CPU fallback treated '/' as a don't-care and the
/// copy on the CUDA side did not, so the same pattern over the same bytes gave
/// different answers depending on which build ran it. Measured by the
/// adversarial verify pass on DE AD BE EF with the pattern "de/d":
/// bestRatio 1.000000 / totalNibs 3 through the CPU path, 0.750000 / 4 through
/// the other.
///
/// '/' is the RetDec slashed-jump marker and is a don't-care here, which is the
/// reading the tested path had. The CUDA kernel pre-filters patterns containing
/// one (see the encoding note above batchMatchKernel), so it never sees the
/// character and the two cannot drift apart again -- there is nothing left to
/// drift.
inline SigMatchResult
matchOne(const std::string& nibs, const std::string& pat, std::size_t startNib, std::size_t endNib)
{
	SigMatchResult r;

	const std::size_t patLen = pat.find(';') != std::string::npos ? pat.find(';') : pat.size();
	std::size_t maxStart = 0;
	if (!lastStartFor(endNib, patLen, maxStart)) return r;

	for (std::size_t pos = startNib; pos <= maxStart; ++pos)
	{
		std::uint32_t same = 0, total = 0;
		for (std::size_t si = 0; si < patLen; ++si)
		{
			const char pc = pat[si];
			if (pc == ';' || pc == '\0') break;
			if (pc == '?' || pc == '-' || pc == '/') continue;
			++total;
			if (static_cast<std::uint8_t>(pc) == static_cast<std::uint8_t>(nibs[pos + si]))
			{
				++same;
			}
		}
		if (total > 0)
		{
			const double ratio = static_cast<double>(same) / static_cast<double>(total);
			if (ratio > r.bestRatio || (ratio == r.bestRatio && total > r.totalNibs))
			{
				r.bestRatio = ratio;
				r.sameNibs = same;
				r.totalNibs = total;
				// The nibble position back to a byte offset. NIBBLES_PER_BYTE
				// rather than a bare 2, which this file's own comment claims is
				// stated once and, until this consolidation, was not.
				r.offset = static_cast<std::uint32_t>(pos / NIBBLES_PER_BYTE);
				r.matched = ratio >= 0.5;
			}
		}
	}
	return r;
}

} // namespace gpuscan
} // namespace utils
} // namespace retdec

#ifndef RETDEC_GPU_SCANNER_HOST_ONLY

#include <cuda_runtime.h>
#include <device_launch_parameters.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "retdec/utils/gpu_scanner.h"

// ---------------------------------------------------------------------------
// Helpers / macros
// ---------------------------------------------------------------------------

/// How a kernel is launched.
///
/// `kernel<<<grid, block>>>(args)` is not C++ -- only nvcc parses it -- so
/// under nvcc this is exactly that, and under an ordinary compiler it is a call
/// that runs the grid. That second form is what lets this half of the file be
/// compiled and executed at all: everything below RETDEC_GPU_SCANNER_HOST_ONLY
/// was previously built by nothing in this tree, which an adversarial verify
/// pass demonstrated by reverting four fixes inside it and watching the suite
/// stay green.
///
/// The host implementation lives in tests/utils/cuda_stub/, beside the runtime
/// stubs, because it is test scaffolding rather than product code -- and it is
/// only ever selected when __CUDACC__ is absent AND the stub header has been
/// included, so an ordinary build of this file is unaffected.
#if defined(__CUDACC__)
#define RETDEC_GPU_LAUNCH(kernel, grid, block) kernel<<<(grid), (block)>>>
#else
#define RETDEC_GPU_LAUNCH(kernel, grid, block) ::retdec::tests::cudastub::launch(kernel, (grid), (block))
#endif

#define CUDA_CHECK(call)                                                                                 \
	do                                                                                                   \
	{                                                                                                    \
		cudaError_t _e = (call);                                                                         \
		if (_e != cudaSuccess)                                                                           \
		{                                                                                                \
			throw std::runtime_error(std::string("CUDA error in " #call ": ") + cudaGetErrorString(_e)); \
		}                                                                                                \
	}                                                                                                    \
	while (0)

namespace {

// ---------------------------------------------------------------------------
// CUDA kernels
// ---------------------------------------------------------------------------

/**
 * One block per pattern. Each thread within the block tests one candidate
 * file offset. Threads cooperate via shared memory to cache the pattern.
 *
 * Pattern nibble encoding (same as RetDec CPU path):
 *   hex char ['0'-'f'] = exact match required
 *   '?'  = wildcard (any nibble)
 *   '-'  = don't-care (same as '?')
 *   ';'  = pattern end marker
 *   '/'  = slashed jump (not handled on GPU; pre-filtered by host)
 *
 * Output per pattern: { matched, bestRatio, sameNibs, totalNibs, offset }
 * packed into a struct of arrays for coalescing.
 */

struct GpuMatchResult
{
	uint32_t matched; // 0 or 1
	float bestRatio;
	uint32_t sameNibs;
	uint32_t totalNibs;
	uint32_t offset; // byte offset of best match
};

// Maximum pattern length handled in shared memory.
static constexpr int MAX_PATTERN_NIBS = static_cast<int>(::retdec::utils::gpuscan::kMaxPatternNibs);
// Threads per block for match kernel.
static constexpr int MATCH_BLOCK = 256;

__global__ void batchMatchKernel(
	const uint8_t* __restrict__ fileNibs, // nibbles: '0'-'f', lowercase
	uint32_t fileNibLen,
	const char* __restrict__ patterns,       // all patterns concatenated, '\0'-sep
	const uint32_t* __restrict__ patOffsets, // offset of each pattern in patterns[]
	const uint32_t* __restrict__ patLens,    // nibble length of each pattern
	uint32_t numPatterns,
	uint32_t scanStartNib,
	uint32_t scanEndNib, // inclusive
	GpuMatchResult* __restrict__ results)
{
	const uint32_t pid = blockIdx.x;
	if (pid >= numPatterns) return;

	// Load pattern into shared memory.
	__shared__ char sPat[MAX_PATTERN_NIBS];
	const uint32_t patLen = patLens[pid];
	const char* pat = patterns + patOffsets[pid];

	// Before the fill, not after. patLen is the caller's pattern length and
	// sPat is MAX_PATTERN_NIBS bytes; the loop below had no bound but patLen.
	// The host now routes an over-long pattern to the CPU path, so reaching
	// this is a caller error rather than an input -- and the answer to a caller
	// error is to leave the result as cudaMemset wrote it, which is
	// GpuMatchResult's own "no match", not to write past shared memory.
	//
	// Block-uniform, like the `pid >= numPatterns` return above: every thread
	// in the block reads the same patLen, so the __syncthreads() below are
	// reached by all or none.
	if (patLen > static_cast<uint32_t>(MAX_PATTERN_NIBS)) return;

	for (uint32_t i = threadIdx.x; i < patLen; i += MATCH_BLOCK)
	{
		sPat[i] = pat[i];
	}
	__syncthreads();

	// Each thread tests one starting nibble position.
	const uint32_t endPos = (scanEndNib < fileNibLen) ? scanEndNib : (fileNibLen - 1);
	// A pattern longer than the window fits nowhere. Falling through with
	// maxStart = 0 let thread 0 read fileNibs[0 .. patLen-1] for a patLen that
	// can exceed fileNibLen. Every thread in the block computes this from the
	// same block-uniform values, so the return is uniform and the
	// __syncthreads() below are still reached by all or none -- the same shape
	// as the `pid >= numPatterns` return above. The result stays as cudaMemset
	// left it, which is SigMatchResult's own "no match" state.
	if (patLen == 0 || patLen > endPos + 1) return;
	const uint32_t maxStart = endPos + 1 - patLen;

	uint32_t localBestSame = 0;
	uint32_t localBestTotal = 0;
	float localBestRatio = 0.0f;
	uint32_t localBestOff = 0;
	uint32_t localMatched = 0;

	for (uint32_t pos = scanStartNib + threadIdx.x; pos <= maxStart; pos += MATCH_BLOCK)
	{
		uint32_t same = 0;
		uint32_t total = 0;

		for (uint32_t si = 0; si < patLen; ++si)
		{
			const char pc = sPat[si];
			if (pc == ';' || pc == '\0') break;
			if (pc == '?' || pc == '-') continue;
			// Wildcard / slashed already removed by host.
			++total;
			const uint8_t fc = fileNibs[pos + si];
			// Compare nibble: fileNibs stores the raw nibble char.
			if ((uint8_t)pc == fc) ++same;
		}

		if (total > 0)
		{
			float ratio = (float)same / (float)total;
			if (ratio > localBestRatio || (ratio == localBestRatio && total > localBestTotal))
			{
				localBestRatio = ratio;
				localBestSame = same;
				localBestTotal = total;
				// Convert nibble offset to byte offset. The factor is the
				// one constant, not a literal -- this file's own comment says
				// it is stated once, and this was one of the places it was not.
				localBestOff = pos / ::retdec::utils::gpuscan::NIBBLES_PER_BYTE;
				localMatched = (ratio >= 0.5f) ? 1u : 0u;
			}
		}
	}

	// Block-level reduction: find global best across threads.
	__shared__ float shRatio[MATCH_BLOCK];
	__shared__ uint32_t shSame[MATCH_BLOCK];
	__shared__ uint32_t shTotal[MATCH_BLOCK];
	__shared__ uint32_t shOff[MATCH_BLOCK];
	__shared__ uint32_t shMatched[MATCH_BLOCK];

	shRatio[threadIdx.x] = localBestRatio;
	shSame[threadIdx.x] = localBestSame;
	shTotal[threadIdx.x] = localBestTotal;
	shOff[threadIdx.x] = localBestOff;
	shMatched[threadIdx.x] = localMatched;
	__syncthreads();

	for (uint32_t stride = MATCH_BLOCK / 2; stride > 0; stride >>= 1)
	{
		if (threadIdx.x < stride)
		{
			if (shRatio[threadIdx.x + stride] > shRatio[threadIdx.x]
				|| (shRatio[threadIdx.x + stride] == shRatio[threadIdx.x]
					&& shTotal[threadIdx.x + stride] > shTotal[threadIdx.x]))
			{
				shRatio[threadIdx.x] = shRatio[threadIdx.x + stride];
				shSame[threadIdx.x] = shSame[threadIdx.x + stride];
				shTotal[threadIdx.x] = shTotal[threadIdx.x + stride];
				shOff[threadIdx.x] = shOff[threadIdx.x + stride];
				shMatched[threadIdx.x] = shMatched[threadIdx.x + stride];
			}
		}
		__syncthreads();
	}

	if (threadIdx.x == 0)
	{
		results[pid].matched = shMatched[0];
		results[pid].bestRatio = shRatio[0];
		results[pid].sameNibs = shSame[0];
		results[pid].totalNibs = shTotal[0];
		results[pid].offset = shOff[0];
	}
}

// ---------------------------------------------------------------------------
// Entropy kernel
// ---------------------------------------------------------------------------

static constexpr int ENTROPY_BLOCK = 256;

__global__ void buildHistogramKernel(
	const uint8_t* __restrict__ data,
	uint32_t size,
	uint32_t* __restrict__ histogram) // 256 buckets
{
	__shared__ uint32_t localHist[256];
	if (threadIdx.x < 256) localHist[threadIdx.x] = 0;
	__syncthreads();

	const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
	const uint32_t stride = gridDim.x * blockDim.x;

	for (uint32_t i = idx; i < size; i += stride)
	{
		atomicAdd(&localHist[data[i]], 1u);
	}
	__syncthreads();

	if (threadIdx.x < 256)
	{
		atomicAdd(&histogram[threadIdx.x], localHist[threadIdx.x]);
	}
}

__global__ void computeEntropyKernel(
	const uint32_t* __restrict__ histogram,
	uint32_t totalBytes,
	float* __restrict__ entropy) // single output value
{
	__shared__ float partial[256];
	const uint32_t tid = threadIdx.x;

	float h = 0.0f;
	if (tid < 256 && histogram[tid] > 0)
	{
		float p = (float)histogram[tid] / (float)totalBytes;
		h = -p * log2f(p);
	}
	partial[tid] = h;
	__syncthreads();

	// Parallel reduce.
	for (uint32_t stride = 128; stride > 0; stride >>= 1)
	{
		if (tid < stride) partial[tid] += partial[tid + stride];
		__syncthreads();
	}
	if (tid == 0) *entropy = partial[0];
}

// ---------------------------------------------------------------------------
// findAll kernel
// ---------------------------------------------------------------------------

__global__ void findAllKernel(
	const uint8_t* __restrict__ data,
	uint32_t dataSize,
	const uint8_t* __restrict__ needle,
	uint32_t needleLen,
	uint32_t* __restrict__ matchFlags) // 1 per candidate position
{
	const uint32_t pos = blockIdx.x * blockDim.x + threadIdx.x;
	if (pos + needleLen > dataSize) return;

	bool ok = true;
	for (uint32_t i = 0; i < needleLen && ok; ++i)
	{
		ok = (data[pos + i] == needle[i]);
	}
	matchFlags[pos] = ok ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// Host-side Impl
// ---------------------------------------------------------------------------

} // anonymous namespace

namespace retdec {
namespace utils {

struct GpuScanner::Impl
{
	bool gpuAvailable = false;
	int deviceId = -1;
	char deviceNameStr[256] = "CPU fallback";

	// Device memory for the uploaded file.
	uint8_t* d_fileBytes = nullptr; // raw bytes
	uint8_t* d_fileNibs = nullptr;  // nibble chars ('0'-'f')
	uint32_t fileSize = 0;
	uint32_t fileNibLen = 0;

	// Host-side nibble string (for CPU fallback).
	std::vector<uint8_t> h_fileBytes;
	std::string h_fileNibs;

	void ensureGpu()
	{
		int count = 0;
		if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) return;
		// Pick device 0 - could be extended to pick by P920 topology.
		if (cudaSetDevice(0) != cudaSuccess) return;
		cudaDeviceProp prop{};
		if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) return;
		strncpy(deviceNameStr, prop.name, sizeof(deviceNameStr) - 1);
		deviceId = 0;
		gpuAvailable = true;
	}

	void freeDeviceFile()
	{
		if (d_fileBytes)
		{
			cudaFree(d_fileBytes);
			d_fileBytes = nullptr;
		}
		if (d_fileNibs)
		{
			cudaFree(d_fileNibs);
			d_fileNibs = nullptr;
		}
		fileSize = 0;
		fileNibLen = 0;
	}

	// Upload the bytes and the nibble string the host already built.
	//
	// This used to expand the bytes into nibbles a second time, with its own
	// `nibs.resize(size * 2)` and `nibs[i*2]` / `nibs[i*2+1]` loop, on top of
	// the one uploadFile had already run on the same buffer -- so the same
	// unchecked doubling was written twice in this file and once more in
	// gpu_scanner_cpu.cpp. It takes the finished string instead; the expansion
	// and its refusals are gpuscan::nibblesFor's, and happen before anything
	// reaches the device.
	bool uploadToGpu(const uint8_t* data, std::size_t size, const std::string& nibs)
	{
		// Both counts have to survive the trip to a uint32_t kernel parameter.
		// They were cast without asking, so a file past 4 GB (or a nibble
		// buffer past it, which is a 2 GB file) arrived truncated and the
		// kernel scanned part of it while reporting success.
		if (!gpuscan::fitsKernelWidth(size) || !gpuscan::fitsKernelWidth(nibs.size()))
		{
			return false;
		}

		CUDA_CHECK(cudaMalloc(&d_fileBytes, size));
		CUDA_CHECK(cudaMemcpy(d_fileBytes, data, size, cudaMemcpyHostToDevice));

		const std::size_t nibBytes = nibs.size();
		CUDA_CHECK(cudaMalloc(&d_fileNibs, nibBytes));
		CUDA_CHECK(cudaMemcpy(d_fileNibs, nibs.data(), nibBytes, cudaMemcpyHostToDevice));

		fileSize = static_cast<uint32_t>(size);
		fileNibLen = static_cast<uint32_t>(nibBytes);
		return true;
	}

	~Impl()
	{
		freeDeviceFile();
	}
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

GpuScanner::GpuScanner(): impl_(new Impl())
{
	impl_->ensureGpu();
}

GpuScanner::~GpuScanner()
{
	delete impl_;
}

bool GpuScanner::isAvailable() const
{
	return impl_->gpuAvailable;
}

std::string GpuScanner::deviceName() const
{
	return impl_->deviceNameStr;
}

void GpuScanner::uploadFile(const uint8_t* data, std::size_t size)
{
	// Nothing is touched until both the pointer and the sizing are known good.
	// Every statement in the old body was already past the point of no return
	// by the time it could have noticed: the assign() had copied from the
	// pointer and the resize() had taken the wrapped length.
	std::string nibs;
	if (!gpuscan::nibblesFor(data, size, nibs))
	{
		// Refuse the upload rather than half-perform it, and do not leave the
		// previous file behind for a caller who thinks it is looking at this
		// one. An empty scanner makes batchMatch return unmatched results and
		// fileEntropy return 0.0, which is what every caller already handles
		// for a file it could not read.
		impl_->h_fileBytes.clear();
		impl_->h_fileNibs.clear();
		impl_->freeDeviceFile();
		return;
	}

	// Always keep host copy for CPU fallback paths.
	impl_->h_fileBytes.assign(data, data + size);
	impl_->h_fileNibs = std::move(nibs);

	if (!impl_->gpuAvailable) return;
	impl_->freeDeviceFile();
	if (!impl_->uploadToGpu(data, size, impl_->h_fileNibs))
	{
		// The file is too large for the kernel's uint32 counts. The host copies
		// above are already in place, so batchMatch and fileEntropy take the
		// CPU path -- slower, and correct, which is the right way round. The
		// device buffers stay freed, and fileNibLen stays 0, so nothing can
		// launch against a partial upload.
		impl_->freeDeviceFile();
	}
}

// ---------------------------------------------------------------------------
// batchMatch — GPU path
// ---------------------------------------------------------------------------

static SigMatchResult
cpuMatchOne(const std::string& nibs, const std::string& pat, std::size_t startNib, std::size_t endNib)
{
	// Was a second copy of the match loop that disagreed with the CPU
	// fallback's; see gpuscan::matchOne above for the measurement.
	return gpuscan::matchOne(nibs, pat, startNib, endNib);
}


std::vector<SigMatchResult>
GpuScanner::batchMatch(const std::vector<std::string>& patterns, std::size_t startOffset, std::size_t stopOffset) const
{
	const std::size_t n = patterns.size();
	std::vector<SigMatchResult> results(n);
	if (n == 0 || impl_->h_fileNibs.empty()) return results;

	// The two doublings this used to do inline are the pair demonstrated in
	// gpuscan::nibbleWindow's comment: a start offset past the end wrapped to
	// 0 and matched at the front of the file, and a stop offset past the end
	// wrapped to a one-nibble window and matched nothing.
	std::size_t startNib = 0;
	std::size_t endNib = 0;
	if (!gpuscan::nibbleWindow(startOffset, stopOffset, impl_->h_fileNibs.size(), startNib, endNib))
	{
		return results;
	}

	// Separate GPU-friendly (no '/') and CPU-only (has '/') patterns.
	std::vector<std::size_t> gpuIdx, cpuIdx;
	for (std::size_t i = 0; i < n; ++i)
	{
		// '/' is a don't-care the kernel does not model, and a pattern longer
		// than the kernel's shared-memory buffer does not fit it at all. The
		// second test was missing, so an over-long pattern went to the GPU and
		// overran __shared__ char sPat[kMaxPatternNibs]. The length measured
		// here is the one the kernel is given below -- truncated at ';' -- not
		// the raw string.
		const auto sep = patterns[i].find(';');
		const std::size_t effectiveLen = (sep != std::string::npos) ? sep : patterns[i].size();
		if (patterns[i].find('/') != std::string::npos || !gpuscan::fitsSharedPattern(effectiveLen))
			cpuIdx.push_back(i);
		else
			gpuIdx.push_back(i);
	}

	// CPU path for slashed patterns (small fraction in practice).
	for (std::size_t i: cpuIdx)
	{
		results[i] = cpuMatchOne(impl_->h_fileNibs, patterns[i], startNib, endNib);
	}

	if (!impl_->gpuAvailable || gpuIdx.empty())
	{
		// Full CPU fallback.
		for (std::size_t i: gpuIdx)
		{
			results[i] = cpuMatchOne(impl_->h_fileNibs, patterns[i], startNib, endNib);
		}
		return results;
	}

	// --- GPU path ---
	// Pack patterns into a flat buffer.
	std::string patBuf;
	std::vector<uint32_t> patOffsets(gpuIdx.size());
	std::vector<uint32_t> patLens(gpuIdx.size());

	for (std::size_t gi = 0; gi < gpuIdx.size(); ++gi)
	{
		patOffsets[gi] = static_cast<uint32_t>(patBuf.size());
		const auto& p = patterns[gpuIdx[gi]];
		// Truncate at ';'
		const auto ep = p.find(';');
		const std::string pp = (ep != std::string::npos) ? p.substr(0, ep) : p;
		patLens[gi] = static_cast<uint32_t>(pp.size());
		patBuf += pp;
	}

	// startNib and endNib are std::size_t on the host and uint32_t in the
	// kernel signature. They were cast at the launch without asking. A window
	// past 4 G nibbles would arrive truncated and the kernel would scan the
	// wrong part of the file while reporting success -- so if it does not fit,
	// the whole batch takes the CPU path, which counts in size_t throughout.
	if (!gpuscan::fitsKernelWidth(startNib) || !gpuscan::fitsKernelWidth(endNib)
		|| !gpuscan::fitsKernelWidth(gpuIdx.size()))
	{
		for (std::size_t gi = 0; gi < gpuIdx.size(); ++gi)
		{
			results[gpuIdx[gi]] = gpuscan::matchOne(impl_->h_fileNibs, patterns[gpuIdx[gi]], startNib, endNib);
		}
		return results;
	}

	const uint32_t numGpu = static_cast<uint32_t>(gpuIdx.size());

	char* d_patBuf = nullptr;
	uint32_t* d_offsets = nullptr;
	uint32_t* d_lens = nullptr;
	GpuMatchResult* d_res = nullptr;

	try
	{
		CUDA_CHECK(cudaMalloc(&d_patBuf, patBuf.size()));
		CUDA_CHECK(cudaMalloc(&d_offsets, numGpu * sizeof(uint32_t)));
		CUDA_CHECK(cudaMalloc(&d_lens, numGpu * sizeof(uint32_t)));
		CUDA_CHECK(cudaMalloc(&d_res, numGpu * sizeof(GpuMatchResult)));

		CUDA_CHECK(cudaMemcpy(d_patBuf, patBuf.data(), patBuf.size(), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_offsets, patOffsets.data(), numGpu * sizeof(uint32_t), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_lens, patLens.data(), numGpu * sizeof(uint32_t), cudaMemcpyHostToDevice));

		// Zero results.
		CUDA_CHECK(cudaMemset(d_res, 0, numGpu * sizeof(GpuMatchResult)));

		RETDEC_GPU_LAUNCH(batchMatchKernel, numGpu, MATCH_BLOCK)
		(impl_->d_fileNibs,
		 impl_->fileNibLen,
		 d_patBuf,
		 d_offsets,
		 d_lens,
		 numGpu,
		 // Checked immediately above the launch; see the guard there.
		 static_cast<uint32_t>(startNib),
		 static_cast<uint32_t>(endNib),
		 d_res);
		CUDA_CHECK(cudaGetLastError());
		CUDA_CHECK(cudaDeviceSynchronize());

		// Copy results back.
		std::vector<GpuMatchResult> h_res(numGpu);
		CUDA_CHECK(cudaMemcpy(h_res.data(), d_res, numGpu * sizeof(GpuMatchResult), cudaMemcpyDeviceToHost));

		for (std::size_t gi = 0; gi < gpuIdx.size(); ++gi)
		{
			auto& r = results[gpuIdx[gi]];
			r.matched = h_res[gi].matched != 0;
			r.bestRatio = h_res[gi].bestRatio;
			r.sameNibs = h_res[gi].sameNibs;
			r.totalNibs = h_res[gi].totalNibs;
			r.offset = h_res[gi].offset;
		}
	}
	catch (...)
	{
		// GPU error — fall back to CPU for remaining patterns.
		for (std::size_t gi = 0; gi < gpuIdx.size(); ++gi)
		{
			results[gpuIdx[gi]] = cpuMatchOne(impl_->h_fileNibs, patterns[gpuIdx[gi]], startNib, endNib);
		}
	}

	if (d_patBuf) cudaFree(d_patBuf);
	if (d_offsets) cudaFree(d_offsets);
	if (d_lens) cudaFree(d_lens);
	if (d_res) cudaFree(d_res);

	return results;
}

// ---------------------------------------------------------------------------
// fileEntropy
// ---------------------------------------------------------------------------

double GpuScanner::fileEntropy(std::size_t startOffset, std::size_t stopOffset) const
{
	const auto& bytes = impl_->h_fileBytes;
	if (bytes.empty()) return 0.0;

	std::size_t lo = 0;
	std::size_t hi = 0;
	if (!gpuscan::byteWindow(startOffset, stopOffset, bytes.size(), lo, hi)) return 0.0;
	const std::size_t sz = hi - lo + 1;

	if (!impl_->gpuAvailable)
	{
		// CPU fallback: plain histogram.
		uint32_t hist[256] = {};
		for (std::size_t i = lo; i <= hi; ++i)
			hist[bytes[i]]++;
		double e = 0.0;
		for (int b = 0; b < 256; ++b)
		{
			if (hist[b] == 0) continue;
			double p = (double)hist[b] / (double)sz;
			e -= p * std::log2(p);
		}
		return e;
	}

	uint32_t* d_hist = nullptr;
	float* d_entr = nullptr;
	float h_entr = 0.0f;

	try
	{
		CUDA_CHECK(cudaMalloc(&d_hist, 256 * sizeof(uint32_t)));
		CUDA_CHECK(cudaMalloc(&d_entr, sizeof(float)));
		CUDA_CHECK(cudaMemset(d_hist, 0, 256 * sizeof(uint32_t)));

		const int blocks = static_cast<int>((sz + ENTROPY_BLOCK - 1) / ENTROPY_BLOCK);
		RETDEC_GPU_LAUNCH(buildHistogramKernel, blocks, ENTROPY_BLOCK)
		(impl_->d_fileBytes + lo, static_cast<uint32_t>(sz), d_hist);
		CUDA_CHECK(cudaGetLastError());

		RETDEC_GPU_LAUNCH(computeEntropyKernel, 1, 256)(d_hist, static_cast<uint32_t>(sz), d_entr);
		CUDA_CHECK(cudaGetLastError());
		CUDA_CHECK(cudaDeviceSynchronize());
		CUDA_CHECK(cudaMemcpy(&h_entr, d_entr, sizeof(float), cudaMemcpyDeviceToHost));
	}
	catch (...)
	{
		// CPU fallback on error.
		uint32_t hist[256] = {};
		for (std::size_t i = lo; i <= hi; ++i)
			hist[bytes[i]]++;
		double e = 0.0;
		for (int b = 0; b < 256; ++b)
		{
			if (!hist[b]) continue;
			double p = (double)hist[b] / (double)sz;
			e -= p * std::log2(p);
		}
		if (d_hist) cudaFree(d_hist);
		if (d_entr) cudaFree(d_entr);
		return e;
	}

	cudaFree(d_hist);
	cudaFree(d_entr);
	return static_cast<double>(h_entr);
}

// ---------------------------------------------------------------------------
// findAll
// ---------------------------------------------------------------------------

std::vector<std::size_t> GpuScanner::findAll(const std::vector<uint8_t>& needle) const
{
	std::vector<std::size_t> offsets;
	const auto& bytes = impl_->h_fileBytes;
	if (bytes.empty() || needle.empty() || needle.size() > bytes.size()) return offsets;

	if (!impl_->gpuAvailable)
	{
		// CPU fallback.
		const uint8_t* b = bytes.data();
		const uint8_t* e = b + bytes.size();
		const uint8_t* n = needle.data();
		for (auto it = b; (it = std::search(it, e, n, n + needle.size())) != e; ++it)
			offsets.push_back(static_cast<std::size_t>(it - b));
		return offsets;
	}

	const uint32_t numPositions = static_cast<uint32_t>(bytes.size() - needle.size() + 1);

	uint8_t* d_needle = nullptr;
	uint32_t* d_flags = nullptr;

	try
	{
		CUDA_CHECK(cudaMalloc(&d_needle, needle.size()));
		CUDA_CHECK(cudaMalloc(&d_flags, numPositions * sizeof(uint32_t)));
		CUDA_CHECK(cudaMemcpy(d_needle, needle.data(), needle.size(), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemset(d_flags, 0, numPositions * sizeof(uint32_t)));

		const int blk = 256;
		const int blocks = (numPositions + blk - 1) / blk;
		RETDEC_GPU_LAUNCH(findAllKernel, blocks, blk)
		(impl_->d_fileBytes, impl_->fileSize, d_needle, static_cast<uint32_t>(needle.size()), d_flags);
		CUDA_CHECK(cudaGetLastError());
		CUDA_CHECK(cudaDeviceSynchronize());

		std::vector<uint32_t> h_flags(numPositions);
		CUDA_CHECK(cudaMemcpy(h_flags.data(), d_flags, numPositions * sizeof(uint32_t), cudaMemcpyDeviceToHost));
		for (uint32_t i = 0; i < numPositions; ++i)
			if (h_flags[i]) offsets.push_back(i);
	}
	catch (...)
	{
		// CPU fallback.
		const uint8_t* b = bytes.data();
		const uint8_t* e = b + bytes.size();
		const uint8_t* n = needle.data();
		for (auto it = b; (it = std::search(it, e, n, n + needle.size())) != e; ++it)
			offsets.push_back(static_cast<std::size_t>(it - b));
	}

	if (d_needle) cudaFree(d_needle);
	if (d_flags) cudaFree(d_flags);
	return offsets;
}

} // namespace utils
} // namespace retdec

#endif // RETDEC_GPU_SCANNER_HOST_ONLY
