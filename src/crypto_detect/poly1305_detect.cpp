/**
 * @file src/crypto_detect/poly1305_detect.cpp
 * @brief Poly1305 detector — RFC 7539 clamp / donna limb fingerprint.
 *
 * ## Constant fingerprints
 *
 * RFC 7539 clamp(r) as two little-endian uint64 words:
 *   0x0ffffffc0fffffff
 *   0x0ffffffc0ffffffc
 *
 * poly1305-donna 32-bit 26-bit limbs (not the common 0x3ffffff mask):
 *   0x3ffff03, 0x3ffc0ff, 0x3f03fff
 *
 * ## Confidence scoring
 *
 *   64-bit clamp lo                +0.50
 *   64-bit clamp hi                +0.50
 *   each unique donna limb         +0.25
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

Poly1305Evidence Poly1305Detector::analyse(const ssa::SSAFunction& fn) const {
    Poly1305Evidence ev;
    ev.hasClampLo = hasImmediate(fn, 0x0ffffffc0fffffffULL);
    ev.hasClampHi = hasImmediate(fn, 0x0ffffffc0ffffffcULL);
    ev.hasDonnaR1 = hasImmediate(fn, 0x3ffff03ULL);
    ev.hasDonnaR2 = hasImmediate(fn, 0x3ffc0ffULL);
    ev.hasDonnaR3 = hasImmediate(fn, 0x3f03fffULL);
    ev.found = ev.hasClampLo || ev.hasClampHi ||
               ev.hasDonnaR1 || ev.hasDonnaR2 || ev.hasDonnaR3;
    ev.confidence = score(ev);
    return ev;
}

float Poly1305Detector::score(const Poly1305Evidence& ev) const {
    float s = 0.0f;
    if (ev.hasClampLo) s += 0.50f;
    if (ev.hasClampHi) s += 0.50f;
    if (ev.hasDonnaR1) s += 0.25f;
    if (ev.hasDonnaR2) s += 0.25f;
    if (ev.hasDonnaR3) s += 0.25f;
    return s > 1.0f ? 1.0f : s;
}

CryptoResult Poly1305Detector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::Poly1305;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.50f) {
        r.emittedAnnotation =
            "// Cryptographic primitive: Poly1305\n"
            "// RFC 7539 clamp(r) masks 0x0ffffffc0fffffff / 0x0ffffffc0ffffffc\n"
            "// Usage: poly1305_auth(mac, msg, mlen, key);";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
