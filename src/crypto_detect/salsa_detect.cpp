/**
 * @file src/crypto_detect/salsa_detect.cpp
 * @brief Salsa20 detector — quarter-round structural fingerprint.
 *
 * ## Structural invariant
 *
 * The Salsa20 quarter-round rotates by 7, 9, 13, and 18 (distinct from
 * ChaCha20's 16, 12, 8, 7). Shared sigma words ("expand 32-byte k") are
 * owned by ChaCha20Detector and are not scored here.
 *
 * ## Confidence scoring
 *
 *   rotation constant 7            +0.25
 *   rotation constant 9            +0.25
 *   rotation constant 13           +0.25
 *   rotation constant 18           +0.25
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

Salsa20Evidence Salsa20Detector::analyse(const ssa::SSAFunction& fn) const {
    Salsa20Evidence ev;
    ev.hasRotConst7  = hasImmediate(fn, 7);
    ev.hasRotConst9  = hasImmediate(fn, 9);
    ev.hasRotConst13 = hasImmediate(fn, 13);
    ev.hasRotConst18 = hasImmediate(fn, 18);
    ev.hasAddXorRotSeq = countOp(fn, ssa::IrInstr::Op::Add) >= 1 &&
                         countOp(fn, ssa::IrInstr::Op::Xor) >= 1 &&
                         (countOp(fn, ssa::IrInstr::Op::Shl) >= 1 ||
                          countOp(fn, ssa::IrInstr::Op::Or)  >= 1);
    ev.found = ev.hasAddXorRotSeq &&
               (ev.hasRotConst7 || ev.hasRotConst9 ||
                ev.hasRotConst13 || ev.hasRotConst18);
    ev.confidence = score(ev);
    return ev;
}

float Salsa20Detector::score(const Salsa20Evidence& ev) const {
    if (!ev.hasAddXorRotSeq)
        return 0.0f;
    float s = 0.0f;
    if (ev.hasRotConst7)  s += 0.25f;
    if (ev.hasRotConst9)  s += 0.25f;
    if (ev.hasRotConst13) s += 0.25f;
    if (ev.hasRotConst18) s += 0.25f;
    return s > 1.0f ? 1.0f : s;
}

CryptoResult Salsa20Detector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::Salsa20;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.50f) {
        r.emittedAnnotation =
            "// Cryptographic primitive: Salsa20\n"
            "// Quarter-round rotation constants: 7, 9, 13, 18\n"
            "// Usage: salsa20_encrypt(key, nonce, counter, plaintext, ciphertext, len);";
    }
    return r;
}

CryptoResult Curve25519Detector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::ECC;
    // 121665 == 0x1db41 is (A-2)/4 for Curve25519's Montgomery a=486662.
    if (hasImmediate(fn, 121665) || hasImmediate(fn, 0x1db41)) {
        r.variant = CryptoVariant::Curve25519;
        r.confidence = 1.0f;
        r.emittedAnnotation =
            "// Cryptographic primitive: Curve25519\n"
            "// Montgomery-ladder constant 121665 (0x1db41)\n"
            "// Usage: crypto_scalarmult(q, n, p);";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
