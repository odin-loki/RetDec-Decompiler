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
 *   Add + Xor sequence             required (guard)
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

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

} // anonymous namespace

ChaCha20Evidence ChaCha20Detector::analyse(const ssa::SSAFunction& fn) const {
    ChaCha20Evidence ev;
    ev.hasRotConst16   = hasImmediate(fn, 16);
    ev.hasRotConst12   = hasImmediate(fn, 12);
    ev.hasRotConst8    = hasImmediate(fn, 8);
    ev.hasRotConst7    = hasImmediate(fn, 7);
    ev.hasAddXorRotSeq = countOp(fn, ssa::IrInstr::Op::Add) >= 1 &&
                         countOp(fn, ssa::IrInstr::Op::Xor) >= 1 &&
                         (countOp(fn, ssa::IrInstr::Op::Shl) >= 1 ||
                          countOp(fn, ssa::IrInstr::Op::Or)  >= 1);
    // Guard: require the structural sequence.
    ev.found = ev.hasAddXorRotSeq &&
               (ev.hasRotConst16 || ev.hasRotConst12 ||
                ev.hasRotConst8  || ev.hasRotConst7);
    ev.confidence = score(ev);
    return ev;
}

float ChaCha20Detector::score(const ChaCha20Evidence& ev) const {
    if (!ev.hasAddXorRotSeq) return 0.0f;
    float s = 0.0f;
    if (ev.hasRotConst16) s += 0.25f;
    if (ev.hasRotConst12) s += 0.25f;
    if (ev.hasRotConst8)  s += 0.25f;
    if (ev.hasRotConst7)  s += 0.25f;
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
            "// Usage: chacha20_encrypt(key, nonce, counter, plaintext, ciphertext, len);";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
