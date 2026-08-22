/**
 * @file src/crypto_detect/crc_detect.cpp
 * @brief CRC-32 detector — IEEE / Ethernet generator polynomials.
 *
 * ## Constant fingerprints
 *
 *   0xEDB88320  — CRC-32 IEEE reflected (PKZip, PNG, Ethernet software)
 *   0x04C11DB7  — CRC-32 normal / non-reflected (same polynomial, MSB-first)
 *
 * Either constant is uniquely characteristic of CRC-32 table generation
 * or a bit-wise implementation.
 *
 * ## Confidence scoring
 *
 *   reflected polynomial           +0.70
 *   normal polynomial              +0.70
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace crypto_detect {

namespace {

static bool hasImmediate(const ssa::SSAFunction& fn, uint64_t val) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i) continue;
            for (const auto& u : i->uses) {
                const auto* v = fn.value(u.valueId);
                if (v && v->kind == ssa::ValueKind::Immediate && v->imm == val)
                    return true;
            }
        }
    }
    return false;
}

} // anonymous namespace

CRCEvidence CRCDetector::analyse(const ssa::SSAFunction& fn) const {
    CRCEvidence ev;
    ev.hasReflectedPoly = hasImmediate(fn, 0xEDB88320ULL);
    ev.hasNormalPoly    = hasImmediate(fn, 0x04C11DB7ULL);
    ev.found = ev.hasReflectedPoly || ev.hasNormalPoly;
    ev.confidence = score(ev);
    return ev;
}

float CRCDetector::score(const CRCEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasReflectedPoly) s += 0.70f;
    if (ev.hasNormalPoly)    s += 0.70f;
    return s > 1.0f ? 1.0f : s;
}

CryptoResult CRCDetector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::CRC;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.50f) {
        r.emittedAnnotation =
            "// Checksum primitive: CRC-32\n"
            "// Generator polynomial 0xEDB88320 (reflected) / 0x04C11DB7 (normal)\n"
            "// Usage: crc32(crc, buf, len);";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
