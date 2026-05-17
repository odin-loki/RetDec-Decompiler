/**
 * @file src/packer/entropy_profiler.cpp
 * @brief Shannon entropy profiler implementation.
 */

#include "retdec/packer/entropy_profiler.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace retdec {
namespace packer {

// ─── Block entropy ────────────────────────────────────────────────────────────

double EntropyProfiler::blockEntropy(const uint8_t *data, size_t size)
{
    if (!data || size == 0) return 0.0;

    // Count byte frequencies
    uint32_t freq[256] = {};
    for (size_t i = 0; i < size; ++i) ++freq[data[i]];

    double entropy = 0.0;
    for (int i = 0; i < 256; ++i) {
        if (freq[i] == 0) continue;
        double p = static_cast<double>(freq[i]) / static_cast<double>(size);
        entropy -= p * std::log2(p);
    }
    return entropy;
}

// ─── Profile ─────────────────────────────────────────────────────────────────

EntropyProfile EntropyProfiler::profile(const uint8_t *data, size_t size) const
{
    EntropyProfile prof;
    prof.fileSize = size;

    if (!data || size == 0) return prof;

    size_t numBlocks = (size + kEntropyBlockSize - 1) / kEntropyBlockSize;
    prof.blocks.reserve(numBlocks);

    double totalEntropy    = 0.0;
    size_t highEntropyBytes = 0;
    size_t lowEntropyBytes  = 0;

    for (size_t bi = 0; bi < numBlocks; ++bi) {
        size_t off    = bi * kEntropyBlockSize;
        size_t bsz    = std::min(kEntropyBlockSize, size - off);
        double ent    = blockEntropy(data + off, bsz);

        EntropyBlock blk;
        blk.fileOffset = off;
        blk.entropy    = ent;
        blk.isHigh     = ent > kHighEntropyThreshold;
        blk.isLow      = ent < kLowEntropyThreshold;
        prof.blocks.push_back(blk);

        totalEntropy += ent * static_cast<double>(bsz);
        if (blk.isHigh) highEntropyBytes += bsz;
        if (blk.isLow)  lowEntropyBytes  += bsz;
    }

    prof.averageEntropy      = totalEntropy / static_cast<double>(size);
    prof.highEntropyFraction = static_cast<double>(highEntropyBytes) / static_cast<double>(size);
    prof.lowEntropyFraction  = static_cast<double>(lowEntropyBytes)  / static_cast<double>(size);

    // ── Build EntropySection runs ─────────────────────────────────────────
    if (!prof.blocks.empty()) {
        EntropySection cur;
        cur.startOffset = prof.blocks[0].fileOffset;
        cur.isHigh      = prof.blocks[0].isHigh;
        cur.isLow       = prof.blocks[0].isLow;
        double sumE     = prof.blocks[0].entropy;
        size_t cnt      = 1;

        auto flush = [&]() {
            cur.endOffset  = cur.startOffset + cnt * kEntropyBlockSize;
            cur.endOffset  = std::min(cur.endOffset, size); // clamp to file size
            cur.avgEntropy = sumE / static_cast<double>(cnt);
            prof.sections.push_back(cur);
        };

        for (size_t bi = 1; bi < prof.blocks.size(); ++bi) {
            const auto &blk = prof.blocks[bi];
            if (blk.isHigh == cur.isHigh && blk.isLow == cur.isLow) {
                sumE += blk.entropy;
                ++cnt;
            } else {
                flush();
                cur.startOffset = blk.fileOffset;
                cur.isHigh      = blk.isHigh;
                cur.isLow       = blk.isLow;
                sumE            = blk.entropy;
                cnt             = 1;
            }
        }
        flush();
    }

    return prof;
}

} // namespace packer
} // namespace retdec
