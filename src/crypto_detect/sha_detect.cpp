/**
 * @file src/crypto_detect/sha_detect.cpp
 * @brief SHA-256 / SHA-1 detector.
 *
 * ## SHA-256 constant fingerprints
 *
 * First few round constants K[]:
 *   0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5
 *
 * Initial hash values H[]:
 *   0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a
 *
 * Rotation amounts for Sigma0: {2, 13, 22}, Sigma1: {6, 11, 25},
 * sigma0 (message schedule): {7, 18, 3}, sigma1: {17, 19, 10}.
 *
 * ## SHA-256 structural fingerprints
 *
 * Ch function: `(e & f) ^ (~e & g)` — And + Xor + Not (or And + Xor with negated mask).
 * Maj function: `(a & b) ^ (a & c) ^ (b & c)` — three And + two Xor.
 * Round: 64 iterations (loop count constant 64 or loop comparison against 64).
 *
 * ## SHA-1 constant fingerprints
 *
 * Initial hash values: 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0
 * Round constants K[]:  0x5A827999 (rounds 0-19), 0x6ED9EBA1 (20-39),
 *                        0x8F1BBCDC (40-59), 0xCA62C1D6 (60-79)
 * ROTL(1) in message schedule: Shl(w,1) | Shr(w,31).
 *
 * ## Confidence scoring
 *
 *   round constants                +0.45
 *   Ch function (And+Xor pattern)  +0.30
 *   Maj function                   +0.15
 *   rotation constants             +0.10
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

#include <set>

namespace retdec {
namespace crypto_detect {

namespace {

// SHA-256 round constants (first 4 of 64).
static const std::set<uint64_t> kSHA256Constants = {
    0x428a2f98ULL, 0x71374491ULL, 0xb5c0fbcfULL, 0xe9b5dba5ULL,
    0x6a09e667ULL, 0xbb67ae85ULL, 0x3c6ef372ULL, 0xa54ff53aULL,
};

// SHA-1 constants.
static const std::set<uint64_t> kSHA1Constants = {
    0x67452301ULL, 0xEFCDAB89ULL, 0x98BADCFEULL, 0x10325476ULL, 0xC3D2E1F0ULL,
    0x5A827999ULL, 0x6ED9EBA1ULL, 0x8F1BBCDCULL, 0xCA62C1D6ULL,
};

// SHA-256 rotation amounts.
static const std::set<uint64_t> kSHA256Rotations = {
    2, 6, 7, 10, 11, 13, 17, 18, 19, 22, 25,
};

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs)
            if (i && i->op == op) ++n;
    }
    return n;
}

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

static bool hasAnyImmediate(const ssa::SSAFunction& fn, const std::set<uint64_t>& vals) {
    for (uint64_t v : vals) if (hasImmediate(fn, v)) return true;
    return false;
}

// Ch function: (e & f) ^ (~e & g) — need at least 2 And + 1 Xor.
static bool hasChFunction(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::And) >= 2 &&
           countOp(fn, ssa::IrInstr::Op::Xor) >= 1;
}

// Maj function: (a & b) ^ (a & c) ^ (b & c) — need ≥3 And + ≥2 Xor.
static bool hasMajFunction(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::And) >= 3 &&
           countOp(fn, ssa::IrInstr::Op::Xor) >= 2;
}

// SHA-256 rotation amounts: check for the specific Shl/Shr constants.
static bool hasRotationConstants(const ssa::SSAFunction& fn) {
    return hasAnyImmediate(fn, kSHA256Rotations);
}

} // anonymous namespace

bool SHADetector::hasSHA1Constants(const ssa::SSAFunction& fn) const {
    return hasAnyImmediate(fn, kSHA1Constants);
}

bool SHADetector::hasSHA256Constants(const ssa::SSAFunction& fn) const {
    return hasAnyImmediate(fn, kSHA256Constants);
}

SHAEvidence SHADetector::analyse(const ssa::SSAFunction& fn) const {
    SHAEvidence ev;
    bool sha256 = hasSHA256Constants(fn);
    bool sha1   = hasSHA1Constants(fn);
    ev.hasRoundConst  = sha256 || sha1;
    ev.hasChFunction  = hasChFunction(fn);
    ev.hasMajFunction = hasMajFunction(fn);
    ev.hasRotations   = hasRotationConstants(fn);
    ev.isSHA1         = sha1 && !sha256;
    ev.found = ev.hasRoundConst;
    ev.confidence = score(ev);
    return ev;
}

float SHADetector::score(const SHAEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasRoundConst)  s += 0.45f;
    if (ev.hasChFunction)  s += 0.30f;
    if (ev.hasMajFunction) s += 0.15f;
    if (ev.hasRotations)   s += 0.10f;
    return s > 1.0f ? 1.0f : s;
}

CryptoResult SHADetector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::SHA256;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.isSHA1) {
        r.algorithm = CryptoAlgorithm::SHA1;
        r.variant   = CryptoVariant::SHA1_160;
    } else {
        r.variant = CryptoVariant::SHA256_256;
    }
    if (ev.confidence >= 0.50f) {
        std::string algo = ev.isSHA1 ? "SHA-1" : "SHA-256";
        r.emittedAnnotation =
            "// Cryptographic primitive: " + algo + "\n"
            "// Usage: SHA256(data, len, hash_out); // or EVP_DigestInit_ex";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
