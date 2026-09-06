/**
 * @file src/crypto_detect/chacha_detect.cpp
 * @brief ChaCha20 detector — quarter-round structural fingerprint.
 *
 * ## Structural invariant
 *
 * The ChaCha20 quarter-round is:
 *   ```
 *   a += b;  d ^= a;  d <<<= 16;
 *   c += d;  b ^= c;  b <<<= 12;
 *   a += b;  d ^= a;  d <<<= 8;
 *   c += d;  b ^= c;  b <<<= 7;
 *   ```
 *
 * In IR this produces:
 *   - 4× Add, 4× Xor, and 4× rotate (each rotate = Shl + Shr + Or).
 *   - The rotation amounts 16, 12, 8, 7 appear as Immediate operands.
 *
 * ## Confidence scoring
 *
 *   rotation constant 16           +0.25
 *   rotation constant 12           +0.25
 *   rotation constant 8            +0.25
 *   rotation constant 7            +0.25
 *   Add + Xor sequence             required (guard) for rotation path
 *   sigma word ("expand 32-byte k") +0.25 each (constant path; no guard)
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

#include "arx_rotate.h"

#include <set>

namespace retdec {
namespace crypto_detect {

namespace {

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

// Little-endian uint32 words of ASCII "expand 32-byte k".
static const std::set<uint64_t> kChaChaSigma32 = {
    0x61707865ULL, // "expa"
    0x3320646eULL, // "nd 3"
    0x79622d32ULL, // "2-by"
    0x6b206574ULL, // "te k"
};

static int countSigmaWords(const ssa::SSAFunction& fn) {
    int n = 0;
    for (uint64_t v : kChaChaSigma32)
        if (hasImmediate(fn, v)) ++n;
    return n;
}

} // anonymous namespace

ChaCha20Evidence ChaCha20Detector::analyse(const ssa::SSAFunction& fn) const {
    ChaCha20Evidence ev;
    // A ChaCha20 quarter-round rotates by 16, 12, 8 and 7. These used to ask
    // only whether those numbers appeared as an immediate anywhere in the
    // function, and the "rotation sequence" asked for `an Add, a Xor, and a
    // Shl or an Or` -- which is not a rotation. An ordinary byte-mixing string
    // hash satisfied all of it and was annotated as ChaCha20 at 0.50. A
    // rotation is a specific shape and arx::hasRotateBy asks for it.
    static constexpr uint64_t kQuarterRoundRotations[] = {16, 12, 8, 7};
    ev.hasRotConst16   = arx::hasRotateBy(fn, 16);
    ev.hasRotConst12   = arx::hasRotateBy(fn, 12);
    ev.hasRotConst8    = arx::hasRotateBy(fn, 8);
    ev.hasRotConst7    = arx::hasRotateBy(fn, 7);
    ev.hasAddXorRotSeq = arx::hasAddRotateXor(fn, kQuarterRoundRotations, 4);
    ev.sigmaWords      = countSigmaWords(fn);
    ev.hasSigmaConst   = ev.sigmaWords > 0;
    // Rotation path still requires the structural sequence; sigma words
    // are unique magic constants and stand alone.
    ev.found = ev.hasSigmaConst ||
               (ev.hasAddXorRotSeq &&
                (ev.hasRotConst16 || ev.hasRotConst12 ||
                 ev.hasRotConst8  || ev.hasRotConst7));
    ev.confidence = score(ev);
    return ev;
}

float ChaCha20Detector::score(const ChaCha20Evidence& ev) const {
    float s = 0.0f;
    if (ev.hasAddXorRotSeq) {
        if (ev.hasRotConst16) s += 0.25f;
        if (ev.hasRotConst12) s += 0.25f;
        if (ev.hasRotConst8)  s += 0.25f;
        if (ev.hasRotConst7)  s += 0.25f;
    }
    // Each "expand 32-byte k" word is a unique constant fingerprint.
    // Two words reach the 0.50 threshold; four words saturate.
    s += 0.25f * static_cast<float>(ev.sigmaWords);
    return s > 1.0f ? 1.0f : s;
}

CryptoResult ChaCha20Detector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::ChaCha20;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.50f) {
        r.emittedAnnotation =
            "// Cryptographic primitive: ChaCha20\n"
            "// Quarter-round rotation constants: 16, 12, 8, 7\n"
            "// Sigma: \"expand 32-byte k\" (0x61707865 …)\n"
            "// Usage: chacha20_encrypt(key, nonce, counter, plaintext, ciphertext, len);";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
