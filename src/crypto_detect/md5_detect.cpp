/**
 * @file src/crypto_detect/md5_detect.cpp
 * @brief MD5 detector — sine-table K[] and init-magic constants.
 *
 * ## Constant fingerprints
 *
 * MD5 T/K sine table (first entries; uniquely MD5):
 *   0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
 *   0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501
 *
 * Init words (shared with SHA-1 — supporting evidence only):
 *   0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
 *
 * SHA-1 also uses those four init words plus 0xC3D2E1F0 and the SHA-1
 * round K[] (0x5A827999 …).  A lone shared init word must not fire MD5.
 *
 * ## Confidence scoring
 *
 *   MD5-only K[] constant          +0.55  (required)
 *   shared init magic              +0.25
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

#include <set>

namespace retdec {
namespace crypto_detect {

namespace {

// MD5 T/K sine-table entries (not used by SHA-1).
static const std::set<uint64_t> kMD5SineK = {
    0xd76aa478ULL, 0xe8c7b756ULL, 0x242070dbULL, 0xc1bdceeeULL,
    0xf57c0fafULL, 0x4787c62aULL, 0xa8304613ULL, 0xfd469501ULL,
};

// Shared with SHA-1 H[0..3]; never sufficient alone.
static const std::set<uint64_t> kMD5InitShared = {
    0x67452301ULL, 0xefcdab89ULL, 0x98badcfeULL, 0x10325476ULL,
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

static bool hasAnyImmediate(const ssa::SSAFunction& fn, const std::set<uint64_t>& vals) {
    for (uint64_t v : vals) if (hasImmediate(fn, v)) return true;
    return false;
}

} // anonymous namespace

MD5Evidence MD5Detector::analyse(const ssa::SSAFunction& fn) const {
    MD5Evidence ev;
    ev.hasSineK     = hasAnyImmediate(fn, kMD5SineK);
    ev.hasInitMagic = hasAnyImmediate(fn, kMD5InitShared);
    // SHA-1 shares the init words; require an MD5-only K[] entry.
    ev.found = ev.hasSineK;
    ev.confidence = score(ev);
    return ev;
}

float MD5Detector::score(const MD5Evidence& ev) const {
    if (!ev.hasSineK) return 0.0f;
    float s = 0.55f;
    if (ev.hasInitMagic) s += 0.25f;
    return s > 1.0f ? 1.0f : s;
}

CryptoResult MD5Detector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::MD5;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.50f) {
        r.emittedAnnotation =
            "// Cryptographic primitive: MD5\n"
            "// Sine-table K[] / init magic detected\n"
            "// Usage: MD5(data, len, digest); // or EVP_DigestInit_ex(..., EVP_md5(), ...)";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
