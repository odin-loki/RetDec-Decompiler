/**
 * @file src/utils/gpu_scanner_cpu.cpp
 * @brief CPU-only stub for GpuScanner — compiled when CUDA is disabled.
 * @copyright (c) 2024 RetDec contributors, MIT license
 *
 * When RETDEC_ENABLE_CUDA=OFF (or no GPU is present), this file is compiled
 * instead of gpu_scanner.cu. The interface is identical; every method
 * uses CPU algorithms so the rest of the codebase compiles unchanged.
 */

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "retdec/utils/bounds.h"
#include "retdec/utils/float_predicate.h"
#include "retdec/utils/gpu_scanner.h"

namespace retdec {
namespace utils {

struct GpuScanner::Impl {
    std::vector<uint8_t> h_fileBytes;
    std::string          h_fileNibs;
};

GpuScanner::GpuScanner()  : impl_(new Impl()) {}
GpuScanner::~GpuScanner() { delete impl_; }

bool        GpuScanner::isAvailable() const { return false; }
std::string GpuScanner::deviceName()  const { return "CPU fallback (CUDA disabled)"; }

/// Nibble characters written per input byte. The scanner's patterns are RetDec
/// nibble strings -- one hex character per 4 bits -- so the nibble buffer is
/// exactly twice the byte buffer, and every index below is derived from this
/// rather than from a bare 2.
static const std::size_t NIBBLES_PER_BYTE = 2;

void GpuScanner::uploadFile(const uint8_t* data, std::size_t size) {
    static const char hex[] = "0123456789abcdef";

    // Nothing is touched until both the pointer and the sizing are known good,
    // because every statement in the old body was already past the point of no
    // return by the time it could have noticed.
    //
    // `data == nullptr` with a non-zero size: `assign(data, data + size)`
    // allocated `size` bytes and then memcpy'd from address 0. Unlike
    // conversion.h's bytesToHexString, which checks, this had no check at all,
    // and uploadFile is a public entry point taking a raw pointer and a length
    // as two separate arguments.
    //
    // `size * NIBBLES_PER_BYTE` overflowing: ESBMC's witness is
    // size = 18446744073709551615, where the product wraps to
    // 18446744073709551614 -- so resize() got a length two below the input
    // while the loop wrote at i*2 and i*2+1 for every i < size, running off the
    // end of the string. In this build order the wrap is reached only if the
    // assign() above survives, and at that size it throws std::length_error
    // out of a void function instead, which is its own defect; refusing here
    // covers both. bounds::mulFits is the proved form of the check and is
    // written as a division so the product is never formed.
    if ((data == nullptr && size != 0)
            || !bounds::mulFits(size, NIBBLES_PER_BYTE)) {
        // Refuse the upload rather than half-perform it: an empty scanner makes
        // batchMatch return unmatched results and fileEntropy return 0.0, which
        // is what every caller already handles for a file it could not read.
        impl_->h_fileBytes.clear();
        impl_->h_fileNibs.clear();
        return;
    }

    impl_->h_fileBytes.assign(data, data + size);
    impl_->h_fileNibs.resize(size * NIBBLES_PER_BYTE);
    for (std::size_t i = 0; i < size; ++i) {
        impl_->h_fileNibs[i*NIBBLES_PER_BYTE]   = hex[data[i] >> 4];
        impl_->h_fileNibs[i*NIBBLES_PER_BYTE+1] = hex[data[i] & 0xF];
    }
}

std::vector<SigMatchResult> GpuScanner::batchMatch(
    const std::vector<std::string>& patterns,
    std::size_t startOffset,
    std::size_t stopOffset) const
{
    const std::string& nibs = impl_->h_fileNibs;
    const std::size_t n = patterns.size();
    std::vector<SigMatchResult> results(n);
    if (nibs.empty()) return results;

    const std::size_t startNib = startOffset * 2;
    const std::size_t endNib   = (stopOffset == SIZE_MAX)
                                 ? nibs.size() - 1
                                 : std::min(stopOffset * 2 + 1, nibs.size() - 1);

    for (std::size_t i = 0; i < n; ++i) {
        const std::string& pat = patterns[i];
        const std::size_t patLen = (pat.find(';') != std::string::npos)
                                   ? pat.find(';') : pat.size();
        if (patLen == 0 || endNib < patLen) continue;
        const std::size_t maxStart = endNib - patLen + 1;

        SigMatchResult& r = results[i];
        for (std::size_t pos = startNib; pos <= maxStart; ++pos) {
            uint32_t same = 0, total = 0;
            for (std::size_t si = 0; si < patLen; ++si) {
                char pc = pat[si];
                if (pc == ';' || pc == '\0') break;
                if (pc == '?' || pc == '-' || pc == '/') continue;
                ++total;
                if ((uint8_t)pc == (uint8_t)nibs[pos+si]) ++same;
            }
            if (total > 0) {
                double ratio = (double)same / (double)total;
                if (ratio > r.bestRatio ||
                    (ratio == r.bestRatio && total > r.totalNibs))
                {
                    r.bestRatio  = ratio;
                    r.sameNibs   = same;
                    r.totalNibs  = total;
                    r.offset     = static_cast<uint32_t>(pos / 2);
                    r.matched    = ratio >= 0.5;
                }
            }
        }
    }
    return results;
}

double GpuScanner::fileEntropy(std::size_t startOffset, std::size_t stopOffset) const {
    const auto& bytes = impl_->h_fileBytes;
    if (bytes.empty()) return 0.0;
    const std::size_t lo = startOffset;
    const std::size_t hi = (stopOffset == SIZE_MAX) ? bytes.size() - 1
                                                    : std::min(stopOffset, bytes.size() - 1);

    // `sz = hi - lo + 1` used to be formed before anything checked that lo was
    // at or below hi. For a startOffset past the end of the file it underflows
    // to a number near SIZE_MAX, the histogram loop then does not run at all,
    // and every `hist[b] / sz` is 0, so the function returned 0.0 -- which is
    // also exactly what it returns for a genuinely uniform region. An entropy
    // of 0.0 is the strongest possible statement about a range, and an invalid
    // range was making it.
    if (lo > hi) return 0.0;
    const std::size_t sz = hi - lo + 1;

    uint32_t hist[fpred::kByteValues] = {};
    for (std::size_t i = lo; i <= hi; ++i) hist[bytes[i]]++;

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

std::vector<std::size_t> GpuScanner::findAll(const std::vector<uint8_t>& needle) const {
    std::vector<std::size_t> offsets;
    const auto& bytes = impl_->h_fileBytes;
    if (bytes.empty() || needle.empty() || needle.size() > bytes.size())
        return offsets;
    const uint8_t* b = bytes.data();
    const uint8_t* e = b + bytes.size();
    const uint8_t* n = needle.data();
    for (auto it = b; (it = std::search(it, e, n, n + needle.size())) != e; ++it)
        offsets.push_back(static_cast<std::size_t>(it - b));
    return offsets;
}

} // namespace utils
} // namespace retdec
