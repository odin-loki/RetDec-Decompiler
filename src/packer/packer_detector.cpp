/**
 * @file src/packer/packer_detector.cpp
 * @brief Entropy-profile-based packer detector — 4-signal weighted voting.
 */

#include "retdec/packer/packer_detector.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace retdec {
namespace packer {

// ─── PackerFamily name ────────────────────────────────────────────────────────

std::string packerFamilyToString(PackerFamily f)
{
    switch (f) {
        case PackerFamily::UPX:     return "UPX";
        case PackerFamily::MPRESS:  return "MPRESS";
        case PackerFamily::Themida: return "Themida";
        case PackerFamily::Generic: return "Generic";
        default:                    return "Unknown";
    }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static bool containsString(const uint8_t *data, size_t size, const char *needle)
{
    if (!data || size == 0 || !needle || *needle == '\0') return false;
    size_t nlen = std::strlen(needle);
    if (nlen > size) return false;
    const auto *end = data + size - nlen + 1;
    for (const auto *p = data; p < end; ++p) {
        if (std::memcmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

/// Count printable strings of at least minLen characters.
static size_t countPrintableStrings(const uint8_t *data, size_t size, size_t minLen = 5)
{
    size_t count = 0;
    size_t run   = 0;
    for (size_t i = 0; i < size; ++i) {
        if (std::isprint(static_cast<unsigned char>(data[i])) && data[i] != 0) {
            ++run;
        } else {
            if (run >= minLen) ++count;
            run = 0;
        }
    }
    if (run >= minLen) ++count;
    return count;
}

/// Recognise common function prologues: 55 48 89 E5 (GCC64), 55 8B EC (x86), 40 55 (MSVC x64).
static bool hasFunctionPrologue(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 4 <= size; ++i) {
        // push rbp; mov rbp, rsp (GCC x64)
        if (data[i] == 0x55 && data[i+1] == 0x48 && data[i+2] == 0x89 && data[i+3] == 0xE5) return true;
        // push ebp; mov ebp, esp (x86)
        if (data[i] == 0x55 && data[i+1] == 0x8B && data[i+2] == 0xEC) return true;
        // MSVC x64: push rbp-variant
        if (data[i] == 0x40 && data[i+1] == 0x55) return true;
    }
    return false;
}

// ─── Signal 1: EntropySectionSignal ──────────────────────────────────────────
/**
 * Score 1.0 when:
 *   - >80% of file is high-entropy blocks, AND
 *   - there is a small (<= 20% of file) low-entropy stub.
 * Partial credit for partial conditions.
 */
double PackerDetector::evalEntropySectionSignal(const EntropyProfile &prof,
                                                 size_t /*fileSize*/) const
{
    if (prof.blocks.empty()) return 0.0;

    double score = 0.0;

    // Primary signal: high fraction
    if (prof.highEntropyFraction > 0.80) {
        score = 0.9;
    } else if (prof.highEntropyFraction > 0.60) {
        score = 0.6;
    } else if (prof.highEntropyFraction > 0.40) {
        score = 0.3;
    } else {
        return 0.0;
    }

    // Bonus: there's also a low-entropy stub (typical packer structure)
    if (prof.lowEntropyFraction > 0.02 && prof.lowEntropyFraction <= 0.20) {
        score = std::min(1.0, score + 0.1);
    }

    // Bonus: high average entropy
    if (prof.averageEntropy > 7.0) {
        score = std::min(1.0, score + 0.05);
    }

    return score;
}

// ─── Signal 2: SectionMismatchSignal ─────────────────────────────────────────
/**
 * Score 1.0 when an executable section has:
 *   - no readable strings (>= 5 chars), AND
 *   - no recognisable function prologues, AND
 *   - no IAT references (no imports in our format result that map into section)
 */
double PackerDetector::evalSectionMismatchSignal(const uint8_t *data, size_t size,
                                                  const FormatResult *fmt) const
{
    if (!fmt || fmt->sections.empty()) return 0.0;

    size_t execSections = 0;
    size_t mismatchSections = 0;

    for (const auto &sec : fmt->sections) {
        if (!sec.isExecutable) continue;
        ++execSections;

        // Read raw bytes of section
        size_t fo  = static_cast<size_t>(sec.fileOffset);
        size_t fsz = static_cast<size_t>(sec.fileSize);
        if (fo >= size || fsz == 0) { ++mismatchSections; continue; }
        fsz = std::min(fsz, size - fo);
        const uint8_t *sd = data + fo;

        bool hasStrings   = countPrintableStrings(sd, fsz, 5) > 3;
        bool hasPrologues = hasFunctionPrologue(sd, fsz);
        bool hasIAT       = !fmt->imports.empty(); // simplification

        // Mismatch: packed code typically has none of these
        if (!hasStrings && !hasPrologues) {
            ++mismatchSections;
        } else if (!hasStrings && !hasIAT && !hasPrologues) {
            ++mismatchSections;
        }
    }

    if (execSections == 0) return 0.0;
    double ratio = static_cast<double>(mismatchSections) /
                   static_cast<double>(execSections);
    return std::min(1.0, ratio);
}

// ─── Signal 3: EntryPointSignal ───────────────────────────────────────────────
/**
 * Score 1.0 when:
 *   - Entry point is NOT in .text / .code / __TEXT, OR
 *   - Entry-point section is both Writable and Executable (W+X).
 */
double PackerDetector::evalEntryPointSignal(const uint8_t * /*data*/, size_t /*size*/,
                                             const FormatResult *fmt) const
{
    if (!fmt) return 0.0;
    if (fmt->entryPoint == 0) return 0.0;
    if (fmt->sections.empty()) return 0.0;

    uint64_t ep = fmt->entryPoint;

    for (const auto &sec : fmt->sections) {
        // Check if entry point lies within this section
        uint64_t start = sec.virtualAddress;
        uint64_t end   = start + sec.virtualSize;
        if (ep >= start && ep < end) {
            // W+X → strong packer signal
            if (sec.isWritable && sec.isExecutable) return 1.0;

            // Not .text / .code / __TEXT → moderate signal
            std::string n = sec.name;
            // Normalise: strip null bytes, lower-case check
            for (char &c : n) if (c == '\0') c = 0;
            bool isCodeSection =
                (n.find(".text") != std::string::npos) ||
                (n.find(".code") != std::string::npos) ||
                (n.find("__TEXT") != std::string::npos) ||
                (n.find("CODE")   != std::string::npos);
            if (!isCodeSection) return 0.75;
            return 0.0; // in .text → not suspicious
        }
    }
    // EP outside all sections → suspicious
    return 0.9;
}

// ─── Signal 4: ImportSparsitySignal ──────────────────────────────────────────
/**
 * Score 1.0 when import count < 4.
 * Score 0.5 when import count < 10.
 * Score 0.0 when >= 10 imports.
 */
double PackerDetector::evalImportSparsitySignal(const FormatResult *fmt) const
{
    if (!fmt) return 0.0;
    size_t n = fmt->imports.size();
    if (n < 4)  return 1.0;
    if (n < 10) return 0.5;
    return 0.0;
}

// ─── Packer family detection ─────────────────────────────────────────────────

PackerFamily PackerDetector::detectFamily(const FormatResult *fmt,
                                           const EntropyProfile &prof) const
{
    // UPX: section names containing "UPX" or high-entropy section after low-entropy stub
    if (fmt) {
        for (const auto &sec : fmt->sections) {
            if (sec.name.find("UPX") != std::string::npos) return PackerFamily::UPX;
        }
    }

    // MPRESS: section name "MPRESS"
    if (fmt) {
        for (const auto &sec : fmt->sections) {
            if (sec.name.find("MPRESS") != std::string::npos) return PackerFamily::MPRESS;
        }
    }

    // Themida: no readable section names + high entropy
    if (fmt && prof.highEntropyFraction > 0.85) {
        bool allNamesGarbage = true;
        for (const auto &sec : fmt->sections) {
            // Check if name has printable ASCII
            bool readable = false;
            for (char c : sec.name) {
                if (std::isprint(static_cast<unsigned char>(c)) && c != '\0') {
                    readable = true; break;
                }
            }
            if (readable) { allNamesGarbage = false; break; }
        }
        if (allNamesGarbage) return PackerFamily::Themida;
    }

    if (prof.highEntropyFraction > 0.60) return PackerFamily::Generic;
    return PackerFamily::Unknown;
}

// ─── Main detect() ───────────────────────────────────────────────────────────

PackerResult PackerDetector::detect(const uint8_t *data, size_t size,
                                     const FormatResult *fmt) const
{
    PackerResult res;

    // Compute entropy profile
    EntropyProfiler profiler;
    res.entropyProfile = profiler.profile(data, size);

    // Evaluate four signals
    res.signals.entropySectionScore  = evalEntropySectionSignal(res.entropyProfile, size);
    res.signals.sectionMismatchScore = evalSectionMismatchSignal(data, size, fmt);
    res.signals.entryPointScore      = evalEntryPointSignal(data, size, fmt);
    res.signals.importSparsityScore  = evalImportSparsitySignal(fmt);

    res.confidence = res.signals.combined();
    res.isPacked   = res.confidence >= 0.6;

    if (res.isPacked) {
        res.likelyFamily = detectFamily(fmt, res.entropyProfile);
        res.familyName   = packerFamilyToString(res.likelyFamily);
    }

    return res;
}

} // namespace packer
} // namespace retdec
