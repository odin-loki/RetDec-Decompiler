/**
 * @file src/crypto_detect/rc4_detect.cpp
 * @brief RC4 detector — KSA 256-byte swap loop + PRGA XOR output.
 *
 * ## Key Scheduling Algorithm (KSA) fingerprints
 *
 * Phase 1 — state array initialisation:
 *   ```c
 *   for (i = 0; i < 256; i++) S[i] = i;
 *   ```
 * IR signals:
 *   - Loop with trip-count 256 (immediate 256 or Compare <= 255).
 *   - Store of the loop induction variable into a base+offset address.
 *
 * Phase 2 — key mixing swap loop:
 *   ```c
 *   j = 0;
 *   for (i = 0; i < 256; i++) {
 *       j = (j + S[i] + key[i % key_len]) % 256;
 *       swap(S[i], S[j]);
 *   }
 *   ```
 * IR signals:
 *   - Load + Add + Store sequence (swap) within a 256-iteration loop.
 *   - Modulo operation (And(255) or a Div/Rem) for the j update.
 *
 * ## Pseudo-Random Generation Algorithm (PRGA) fingerprints
 *
 *   ```c
 *   i = j = 0;
 *   while (len--) {
 *       i = (i + 1) % 256;
 *       j = (j + S[i]) % 256;
 *       swap(S[i], S[j]);
 *       *out++ = *in++ ^ S[(S[i] + S[j]) % 256];
 *   }
 *   ```
 * IR signals:
 *   - Xor of plaintext byte with a Load from the state array.
 *   - Add for index computation + And(255) modulo.
 *
 * ## Confidence scoring
 *
 *   256 loop constant              +0.30
 *   swap (Load+Add+Store) pattern  +0.35
 *   PRGA XOR with S[] lookup       +0.35
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

// KSA swap pattern: Load + Add + Store within a loop block.
static bool hasSwapPattern(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        // Check for a back-edge (loop).
        bool isLoop = false;
        for (uint32_t s : blk->succs) if (s <= b) { isLoop = true; break; }
        if (!isLoop) continue;

        int loads = 0, stores = 0, adds = 0;
        for (const auto* i : blk->instrs) {
            if (!i) continue;
            if (i->op == ssa::IrInstr::Op::Load)  ++loads;
            if (i->op == ssa::IrInstr::Op::Store)  ++stores;
            if (i->op == ssa::IrInstr::Op::Add)    ++adds;
        }
        if (loads >= 2 && stores >= 2 && adds >= 1) return true;
    }
    return false;
}

// PRGA: Xor of Load from state array — heuristic: Xor + Load in a loop.
static bool hasPRGA(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Xor) >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Load) >= 2 &&
           (hasImmediate(fn, 255) || hasImmediate(fn, 256));
}

} // anonymous namespace

RC4Evidence RC4Detector::analyse(const ssa::SSAFunction& fn) const {
    RC4Evidence ev;
    ev.has256Constant = hasImmediate(fn, 256) || hasImmediate(fn, 255);
    ev.hasKSA  = ev.has256Constant && hasSwapPattern(fn);
    ev.hasPRGA = hasPRGA(fn);
    ev.found   = ev.hasKSA || ev.hasPRGA;
    ev.confidence = score(ev);
    return ev;
}

float RC4Detector::score(const RC4Evidence& ev) const {
    float s = 0.0f;
    if (ev.has256Constant) s += 0.30f;
    if (ev.hasKSA)         s += 0.35f;
    if (ev.hasPRGA)        s += 0.35f;
    return s > 1.0f ? 1.0f : s;
}

CryptoResult RC4Detector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::RC4;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.50f) {
        r.emittedAnnotation =
            "// Cryptographic primitive: RC4\n"
            "// WARNING: RC4 is cryptographically broken — do not use in new code\n"
            "// KSA 256-byte swap loop + PRGA XOR output detected";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
