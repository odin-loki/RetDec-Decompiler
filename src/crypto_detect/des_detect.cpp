/**
 * @file src/crypto_detect/des_detect.cpp
 * @brief DES detector — OpenSSL/SSLeay DES_SPtrans packed S-box+P words.
 *
 * ## Constant fingerprints
 *
 * DES_SPtrans[0] distinctive packed words (uniquely DES; Eric Young / OpenSSL):
 *   0x02080800, 0x02080802, 0x00080802, 0x02000802
 *
 * These are precomputed S-box 1 + P-permutation entries.  They do not
 * collide with AES / SHA / MD5 / CRC / ChaCha / Blowfish magics.
 * Sparse single-bit SPtrans slots (e.g. 0x00080000) are omitted.
 *
 * Raw DES S-box nibbles and IP/FP swap masks (0x0f0f0f0f, 0x33333333,
 * 0x00ff00ff, 0x55555555) are too common to fingerprint.
 *
 * ## Confidence scoring
 *
 *   first distinctive SPtrans word   +0.70
 *   each additional SPtrans word     +0.10
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

#include <set>

namespace retdec {
namespace crypto_detect {

namespace {

// OpenSSL DES_SPtrans[0] multi-bit packed words; not AES/SHA/MD5/CRC/ChaCha/Blowfish.
static const std::set<uint64_t> kDES_SPtrans = {
    0x02080800ULL,
    0x02080802ULL,
    0x00080802ULL,
    0x02000802ULL,
};

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

static int countSPtransWords(const ssa::SSAFunction& fn) {
    int n = 0;
    for (uint64_t v : kDES_SPtrans)
        if (hasImmediate(fn, v)) ++n;
    return n;
}

} // anonymous namespace

DESEvidence DESDetector::analyse(const ssa::SSAFunction& fn) const {
    DESEvidence ev;
    ev.sptransWords = countSPtransWords(fn);
    ev.hasSPtrans = ev.sptransWords > 0;
    ev.found = ev.hasSPtrans;
    ev.confidence = score(ev);
    return ev;
}

float DESDetector::score(const DESEvidence& ev) const {
    if (ev.sptransWords <= 0) return 0.0f;
    float s = 0.70f + 0.10f * static_cast<float>(ev.sptransWords - 1);
    return s > 1.0f ? 1.0f : s;
}

CryptoResult DESDetector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::DES;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.50f) {
        r.emittedAnnotation =
            "// Cryptographic primitive: DES\n"
            "// DES_SPtrans packed S-box+P words 0x02080800 …\n"
            "// Usage: DES_set_key_unchecked(&key, &ks); DES_ecb_encrypt(...);";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
