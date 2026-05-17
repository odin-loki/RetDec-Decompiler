/**
 * @file include/retdec/packer/entropy_profiler.h
 * @brief Shannon entropy profiler for packer detection.
 *
 * Computes per-256-byte-block entropy in O(n), identifies contiguous
 * high-entropy and low-entropy runs (EntropySections), and exposes the
 * full entropy profile for downstream voting signals.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace packer {

// ─── Constants ───────────────────────────────────────────────────────────────

/// Size of each entropy block (bytes).
constexpr size_t kEntropyBlockSize = 256;

/// Threshold for "high entropy" classification (bits/byte).
/// 7.0 is reliable for 256-byte blocks; expected entropy for uniform random is ~7.28.
constexpr double kHighEntropyThreshold = 7.0;

/// Threshold for "low entropy" classification (bits/byte).
constexpr double kLowEntropyThreshold  = 4.0;

// ─── Entropy block ───────────────────────────────────────────────────────────

struct EntropyBlock {
    size_t  fileOffset = 0;          ///< byte offset of block start
    double  entropy    = 0.0;        ///< Shannon entropy in bits/byte [0..8]
    bool    isHigh     = false;      ///< entropy > kHighEntropyThreshold
    bool    isLow      = false;      ///< entropy < kLowEntropyThreshold
};

// ─── EntropySection ──────────────────────────────────────────────────────────

/**
 * A contiguous run of blocks that are either all high-entropy or all
 * low-entropy.
 */
struct EntropySection {
    size_t  startOffset = 0;
    size_t  endOffset   = 0;         ///< exclusive
    double  avgEntropy  = 0.0;
    bool    isHigh      = false;
    bool    isLow       = false;

    size_t size() const { return endOffset - startOffset; }
};

// ─── Profile ─────────────────────────────────────────────────────────────────

struct EntropyProfile {
    std::vector<EntropyBlock>   blocks;
    std::vector<EntropySection> sections;

    /// Fraction of file bytes covered by high-entropy blocks [0..1].
    double highEntropyFraction = 0.0;

    /// Fraction of file bytes covered by low-entropy blocks [0..1].
    double lowEntropyFraction  = 0.0;

    /// Overall average entropy.
    double averageEntropy      = 0.0;

    /// File size used during profiling.
    size_t fileSize            = 0;
};

// ─── EntropyProfiler ─────────────────────────────────────────────────────────

class EntropyProfiler {
public:
    EntropyProfiler() = default;

    /**
     * Compute the entropy profile of a binary in memory.
     *
     * @param data  Pointer to file bytes.
     * @param size  Number of bytes.
     * @return      Populated EntropyProfile.
     */
    EntropyProfile profile(const uint8_t *data, size_t size) const;

    /**
     * Compute the Shannon entropy of a single byte block.
     * Returns bits/byte in [0..8].
     */
    static double blockEntropy(const uint8_t *data, size_t size);
};

} // namespace packer
} // namespace retdec
