/**
 * @file src/crypto_detect/aes_detect.cpp
 * @brief AES detector — S-box, key schedule, round structure, AES-NI, CBC/CTR/GCM.
 *
 * ## Constant fingerprints
 *
 * AES constants that appear as Immediate operands in compiled code:
 *
 *   S-box first bytes:   0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5
 *   Inverse S-box bytes: 0x52, 0x09, 0x6a, 0xd5
 *   Rcon values:         0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
 *   MixColumns modulus:  0x1b  (the GF(2^8) irreducible polynomial x^8+x^4+x^3+x+1)
 *   GCM polynomial:      0xe1  (the GHASH field polynomial)
 *
 * ## Structural fingerprints
 *
 * Key schedule XOR chain:
 *   - A sequence of Xor + Shl + Shr + And instructions processing 4-byte words.
 *
 * Round function:
 *   - SubBytes: table-lookup (Load from a 256-entry array base + index).
 *   - ShiftRows: byte-level permutation (And + Shl + Shr + Or).
 *   - MixColumns: GF multiply (Shl + Xor + And(0x1b)).
 *   - AddRoundKey: Xor with round key.
 *
 * ## AES-NI
 *
 * AES-NI instructions appear as Call to intrinsic wrappers:
 *   _mm_aesenc_si128, _mm_aesenclast_si128, _mm_aesdec_si128, aesenc, aesenclast
 *
 * ## Mode detection
 *
 *   CBC: XOR immediately before or after the block cipher call (IV).
 *   CTR: an Add on a counter variable within the encryption loop.
 *   GCM: GHASH multiply: Xor + Shl + And(0xe1) pattern.
 *
 * ## Confidence scoring
 *
 *   S-box constant               +0.30
 *   Rcon / MixColumns constant   +0.25
 *   round structure (XOR chain)  +0.25
 *   AES-NI call                  +0.20 (can reach 1.0 alone with Sbox)
 *
 * None of that is scored at all without discriminating evidence: an ordered
 * run of S-box table entries, or an AES-NI call.  Every constant above is an
 * ordinary small integer on its own, so scoring them unconditionally reported
 * AES on any function that happened to contain a 0x09 and a 1.  The cost of
 * the gate is that a purely table-driven AES, whose S-box lives in .rodata and
 * never appears as an Immediate, is no longer claimed on constants alone; the
 * AES-NI path and unrolled key schedules still are.
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

#include <cstddef>
#include <set>
#include <vector>

namespace retdec {
namespace crypto_detect {

namespace {

// AES S-box first-byte fingerprint constants.
static const std::set<uint64_t> kSBoxConstants = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
    0x52, 0x09, 0x6a, 0xd5,  // inverse S-box
};

// Rcon + MixColumns constants.
static const std::set<uint64_t> kRconConstants = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36,
};

// The first row of the S-box and of the inverse S-box, in table order.
// Membership in kSBoxConstants says almost nothing — 0x09, 0x63 and 0x77 are
// ordinary small integers that turn up in any byte-shuffling code — but the
// *order* is a fingerprint: these are the bytes of a specific permutation.
static const uint64_t kSBoxTable[] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
    0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
};
static const uint64_t kInvSBoxTable[] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38,
    0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
};

// How many consecutive table entries must appear, in table order, before a run
// of small byte constants counts as an S-box.  Four is the shortest run that is
// not plausibly coincidental: each byte on its own has probability ~1/256 of
// matching, so four in the right order is ~2^-32 for unrelated code, while a
// single byte (the old test) matches roughly one function in five.
static constexpr std::size_t kMinSBoxRun = 4;

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
    for (uint64_t v : vals)
        if (hasImmediate(fn, v)) return true;
    return false;
}

// Every Immediate operand in the function, in program order.
static std::vector<uint64_t> immediateStream(const ssa::SSAFunction& fn) {
    std::vector<uint64_t> out;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i) continue;
            for (const auto& u : i->uses) {
                const auto* v = fn.value(u.valueId);
                if (v && v->kind == ssa::ValueKind::Immediate) out.push_back(v->imm);
            }
        }
    }
    return out;
}

// True when >= kMinSBoxRun consecutive entries of `table` appear in the
// immediate stream in table order.  Unrelated constants may sit between them —
// the compiler interleaves indices and addresses — but the relative order of
// the table entries is preserved, and that is what carries the information.
static bool hasOrderedRun(const std::vector<uint64_t>& stream,
                          const uint64_t* table, std::size_t tableLen) {
    for (std::size_t start = 0; start + kMinSBoxRun <= tableLen; ++start) {
        std::size_t want = start;
        for (uint64_t imm : stream) {
            if (imm != table[want]) continue;
            ++want;
            if (want - start >= kMinSBoxRun) return true;
        }
    }
    return false;
}

static bool hasSBoxTable(const ssa::SSAFunction& fn) {
    const auto stream = immediateStream(fn);
    if (stream.size() < kMinSBoxRun) return false;
    return hasOrderedRun(stream, kSBoxTable, sizeof(kSBoxTable) / sizeof(kSBoxTable[0]))
        || hasOrderedRun(stream, kInvSBoxTable, sizeof(kInvSBoxTable) / sizeof(kInvSBoxTable[0]));
}

} // anonymous namespace

bool AESDetector::hasAESNI(const ssa::SSAFunction& fn) const {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn.find("aesenc")           != std::string::npos ||
                cn.find("aesenclast")       != std::string::npos ||
                cn.find("aesdec")           != std::string::npos ||
                cn.find("aeskeygenassist")  != std::string::npos ||
                cn.find("_mm_aesenc")       != std::string::npos ||
                cn.find("_mm_aesdec")       != std::string::npos)
                return true;
        }
    }
    return false;
}

CryptoMode AESDetector::detectMode(const ssa::SSAFunction& fn) const {
    // GCM: And with 0xe1 (GHASH polynomial) + Xor + Shl.
    if (hasImmediate(fn, 0xe1) &&
        countOp(fn, ssa::IrInstr::Op::Xor) >= 2 &&
        countOp(fn, ssa::IrInstr::Op::Shl) >= 1)
        return CryptoMode::GCM;

    // CTR: a counter increment (Add in encryption loop with back-edge).
    bool hasBackEdge = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (uint32_t s : blk->succs) if (s <= b) { hasBackEdge = true; break; }
    }
    if (hasBackEdge && countOp(fn, ssa::IrInstr::Op::Add) >= 2 &&
        countOp(fn, ssa::IrInstr::Op::Xor) >= 1)
        return CryptoMode::CTR;

    // CBC: XOR present (IV XOR before encryption).
    if (countOp(fn, ssa::IrInstr::Op::Xor) >= 1)
        return CryptoMode::CBC;

    return CryptoMode::ECB;
}

AESEvidence AESDetector::analyse(const ssa::SSAFunction& fn) const {
    AESEvidence ev;
    ev.hasSBox      = hasAnyImmediate(fn, kSBoxConstants);
    ev.hasRcon      = hasAnyImmediate(fn, kRconConstants) || hasImmediate(fn, 0x1b);
    ev.hasMixCols   = hasImmediate(fn, 0x1b);
    ev.hasAESNI     = hasAESNI(fn);
    ev.hasRoundLoop = countOp(fn, ssa::IrInstr::Op::Xor) >= 4 &&
                      countOp(fn, ssa::IrInstr::Op::And) >= 1 &&
                      (countOp(fn, ssa::IrInstr::Op::Shl) >= 1 ||
                       countOp(fn, ssa::IrInstr::Op::Shr) >= 1);
    ev.hasSBoxTable = hasSBoxTable(fn);
    ev.mode = detectMode(fn);
    // Discriminating evidence only: an ordered run of S-box entries, or an
    // AES-NI intrinsic.  A lone S-box byte used to be enough, and since the
    // Rcon set is {1,2,4,8,...} and the "round structure" is four Xors with a
    // shift, a plain string hash scored 0.75 and was annotated AES — at the
    // same time as ChaCha20 and RC4 claimed it.
    ev.found = ev.hasSBoxTable || ev.hasAESNI;
    ev.confidence = score(ev);
    return ev;
}

float AESDetector::score(const AESEvidence& ev) const {
    // Supporting evidence stays supporting: without a discriminating signal it
    // sums to nothing, the way MD5Detector::score requires its K[] table.
    //
    // `found` *is* that disjunction -- analyse() sets it two lines above the
    // call to this function -- so asking it here keeps one rule in one place.
    // Re-deriving it was two copies of the gate that would drift apart the
    // first time either changed.
    if (!ev.found) return 0.0f;
    float s = 0.0f;
    if (ev.hasSBox)      s += 0.30f;
    if (ev.hasRcon)      s += 0.20f;
    if (ev.hasMixCols)   s += 0.05f;
    if (ev.hasRoundLoop) s += 0.25f;
    if (ev.hasAESNI)     s += 0.20f;
    return s > 1.0f ? 1.0f : s;
}

CryptoResult AESDetector::detect(const ssa::SSAFunction& fn) const {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::AES;
    auto ev = analyse(fn);
    if (!ev.found) return r;
    r.confidence = ev.confidence;
    r.hasAESNI   = ev.hasAESNI;
    r.mode       = ev.mode;
    if (ev.confidence >= 0.50f) {
        std::string modeStr = (ev.mode == CryptoMode::CBC) ? "-CBC" :
                              (ev.mode == CryptoMode::CTR) ? "-CTR" :
                              (ev.mode == CryptoMode::GCM) ? "-GCM" : "-ECB";
        r.emittedAnnotation =
            "// Cryptographic primitive: AES" + modeStr +
            (ev.hasAESNI ? " (AES-NI hardware acceleration)\n" : "\n") +
            "// Usage: EVP_EncryptInit_ex(ctx, EVP_aes_128_" +
            std::string(ev.mode == CryptoMode::GCM ? "gcm" :
                        ev.mode == CryptoMode::CTR ? "ctr" : "cbc") +
            "(), nullptr, key, iv);";
    }
    return r;
}

} // namespace crypto_detect
} // namespace retdec
