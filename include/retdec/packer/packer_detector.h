/**
 * @file include/retdec/packer/packer_detector.h
 * @brief Entropy-profile-based packer detector (Stage 2).
 *
 * Four-signal weighted voting classifier:
 *   Signal 1 (weight 0.40): EntropySectionSignal
 *   Signal 2 (weight 0.20): SectionMismatchSignal
 *   Signal 3 (weight 0.25): EntryPointSignal
 *   Signal 4 (weight 0.15): ImportSparsitySignal
 *
 * Threshold: combined score >= 0.6 → packed.
 *
 * Packer family heuristics:
 *   UPX:    characteristic stub entropy pattern + "UPX" section name substring
 *   MPRESS: "MPRESS" section name substring
 */

#pragma once

#include "retdec/packer/entropy_profiler.h"
#include "retdec/fileformat/lattice/format_result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace packer {

using FormatResult = retdec::fileformat::lattice::FormatResult;
using SectionInfo  = retdec::fileformat::lattice::SectionInfo;

// ─── Signal scores ────────────────────────────────────────────────────────────

struct SignalScores {
    double entropySectionScore  = 0.0; ///< [0..1]
    double sectionMismatchScore = 0.0; ///< [0..1]
    double entryPointScore      = 0.0; ///< [0..1]
    double importSparsityScore  = 0.0; ///< [0..1]

    double combined() const {
        return 0.40 * entropySectionScore
             + 0.20 * sectionMismatchScore
             + 0.25 * entryPointScore
             + 0.15 * importSparsityScore;
    }
};

// ─── PackerFamily ─────────────────────────────────────────────────────────────

enum class PackerFamily {
    Unknown,
    UPX,
    MPRESS,
    Themida,
    Generic,
};

std::string packerFamilyToString(PackerFamily f);

// ─── PackerResult ─────────────────────────────────────────────────────────────

struct PackerResult {
    bool          isPacked       = false;
    double        confidence     = 0.0;       ///< combined weighted score [0..1]
    PackerFamily  likelyFamily   = PackerFamily::Unknown;
    std::string   familyName;
    SignalScores  signals;
    EntropyProfile entropyProfile;
};

// ─── PackerDetector ──────────────────────────────────────────────────────────

class PackerDetector {
public:
    PackerDetector() = default;

    /**
     * Analyse a binary and return a PackerResult.
     *
     * @param data          Pointer to file bytes.
     * @param size          Number of bytes.
     * @param formatResult  Optional parsed format result for section/import info.
     *                      If nullptr, section and import signals are skipped.
     */
    PackerResult detect(const uint8_t *data, size_t size,
                        const FormatResult *formatResult = nullptr) const;

private:
    // Individual signal evaluators
    double evalEntropySectionSignal(const EntropyProfile &prof,
                                    size_t fileSize) const;
    double evalSectionMismatchSignal(const uint8_t *data, size_t size,
                                     const FormatResult *fmt) const;
    double evalEntryPointSignal(const uint8_t *data, size_t size,
                                const FormatResult *fmt) const;
    double evalImportSparsitySignal(const FormatResult *fmt) const;

    // Packer family heuristics
    PackerFamily detectFamily(const FormatResult *fmt,
                               const EntropyProfile &prof) const;
};

} // namespace packer
} // namespace retdec
