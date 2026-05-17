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

void GpuScanner::uploadFile(const uint8_t* data, std::size_t size) {
    static const char hex[] = "0123456789abcdef";
    impl_->h_fileBytes.assign(data, data + size);
    impl_->h_fileNibs.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        impl_->h_fileNibs[i*2]   = hex[data[i] >> 4];
        impl_->h_fileNibs[i*2+1] = hex[data[i] & 0xF];
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
    const std::size_t sz = hi - lo + 1;
    uint32_t hist[256] = {};
    for (std::size_t i = lo; i <= hi; ++i) hist[bytes[i]]++;
    double e = 0.0;
    for (int b = 0; b < 256; ++b) {
        if (!hist[b]) continue;
        double p = (double)hist[b] / (double)sz;
        e -= p * std::log2(p);
    }
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
