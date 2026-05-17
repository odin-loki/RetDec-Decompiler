/**
 * @file include/retdec/utils/gpu_scanner.h
 * @brief GPU-accelerated signature and entropy scanner for RetDec.
 * @copyright (c) 2024 RetDec contributors, MIT license
 *
 * When built with RETDEC_ENABLE_CUDA=ON, the heavy loops run on the GPU.
 * When CUDA is unavailable, every function falls back to a CPU implementation
 * transparently — callers see the same interface either way.
 *
 * Typical usage:
 *
 *   retdec::utils::GpuScanner scanner;
 *   if (scanner.isAvailable()) {
 *       scanner.uploadFile(bytes.data(), bytes.size());
 *       auto results = scanner.batchMatch(patterns);
 *       float entropy = scanner.fileEntropy();
 *   } else {
 *       // fall back to existing CPU path
 *   }
 */

#ifndef RETDEC_UTILS_GPU_SCANNER_H
#define RETDEC_UTILS_GPU_SCANNER_H

#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace utils {

/**
 * @brief Result of matching a single signature pattern against the file.
 */
struct SigMatchResult {
    bool    matched    = false;  ///< true if at least one position matched
    double  bestRatio  = 0.0;   ///< best similarity ratio (0.0 – 1.0)
    uint32_t sameNibs  = 0;     ///< nibbles agreed at best match
    uint32_t totalNibs = 0;     ///< total meaningful nibbles in pattern
    uint32_t offset    = 0;     ///< file offset (in bytes) of best match
};

/**
 * @brief GPU (or CPU-fallback) scanner.
 *
 * Not thread-safe — create one instance per thread or protect with a mutex.
 * Uploading the file once and then calling batchMatch() many times is
 * efficient: the file bytes stay on the GPU between calls.
 */
class GpuScanner {
public:
    GpuScanner();
    ~GpuScanner();

    GpuScanner(const GpuScanner&) = delete;
    GpuScanner& operator=(const GpuScanner&) = delete;

    /**
     * @brief Returns true if a CUDA-capable GPU is available and the CUDA
     *        build was enabled.
     *
     * If this returns false, every method silently uses the CPU fallback.
     */
    bool isAvailable() const;

    /**
     * @brief Returns the name string of the GPU being used, or "CPU fallback"
     *        when no GPU is available.
     */
    std::string deviceName() const;

    /**
     * @brief Upload the raw binary bytes to the GPU (or store for CPU path).
     *
     * Must be called before batchMatch() or fileEntropy().
     * Calling again with new data replaces the previous upload.
     *
     * @param data  Pointer to raw file bytes.
     * @param size  Number of bytes.
     */
    void uploadFile(const uint8_t* data, std::size_t size);

    /**
     * @brief Match all patterns in @a patterns against the uploaded file.
     *
     * Each pattern is a RetDec nibble-string (hex chars, '?' for wildcard,
     * '-' for don't-care, ';' as terminator). Slashed jump patterns ('/')
     * are not supported on GPU and will be handled by CPU fallback per pattern.
     *
     * @param patterns    Vector of nibble-string patterns.
     * @param startOffset Start offset in the file (bytes) to scan from.
     * @param stopOffset  End offset in the file (bytes, inclusive). Pass
     *                    std::string::npos to scan to end of file.
     * @return            One SigMatchResult per pattern, in the same order.
     */
    std::vector<SigMatchResult> batchMatch(
        const std::vector<std::string>& patterns,
        std::size_t startOffset = 0,
        std::size_t stopOffset  = SIZE_MAX) const;

    /**
     * @brief Compute Shannon entropy of the uploaded file (or a sub-range).
     *
     * @param startOffset  Start byte offset (inclusive).
     * @param stopOffset   End byte offset (inclusive), SIZE_MAX = end of file.
     * @return Shannon entropy in bits per byte (0.0 – 8.0).
     */
    double fileEntropy(
        std::size_t startOffset = 0,
        std::size_t stopOffset  = SIZE_MAX) const;

    /**
     * @brief Find all offsets where @a needle appears in the file.
     *
     * Implemented as GPU parallel search (or std::search loop on CPU).
     *
     * @param needle  Raw byte sequence to search for.
     * @return        Vector of byte offsets where needle starts.
     */
    std::vector<std::size_t> findAll(const std::vector<uint8_t>& needle) const;

private:
    struct Impl;
    Impl* impl_;   // PIMPL to hide CUDA types from non-.cu translation units
};

} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_GPU_SCANNER_H
