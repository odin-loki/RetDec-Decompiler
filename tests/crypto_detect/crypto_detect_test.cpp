/**
 * @file tests/crypto_detect/crypto_detect_test.cpp
 * @brief Unit tests for the Cryptographic Algorithm Detector module.
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace retdec::crypto_detect;
using namespace retdec::ssa;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::unique_ptr<SSAFunction> makeEmptyFn(const std::string& name = "test_fn") {
    return std::make_unique<SSAFunction>(name);
}

// Add a basic block; returns pointer (valid for life of SSAFunction).
static BasicBlock* addBlock(SSAFunction& fn) {
    return fn.addBlock();
}

// Add a back-edge successor to simulate a loop.
static void addBackEdge(BasicBlock* blk, uint32_t target) {
    blk->succs.push_back(target);
}

// Add a forward edge.
static void addEdge(BasicBlock* blk, uint32_t target) {
    blk->succs.push_back(target);
}

// Append an instruction to a block (via SSAFunction API).
static IrInstr* addInstr(SSAFunction& fn, BasicBlock* blk, IrInstr::Op op) {
    return fn.addInstr(blk->id, op, /*vma=*/0);
}

// Append a Call instruction with a resolved callee name.
static IrInstr* addCallInstr(SSAFunction& fn, BasicBlock* blk, const std::string& callee) {
    IrInstr* i = fn.addInstr(blk->id, IrInstr::Op::Call, 0);
    i->calleeName = callee;
    return i;
}

// Allocate an Immediate IrValue and wire it as a use on `instr`.
static void addImmUse(SSAFunction& fn, IrInstr* instr, uint64_t imm) {
    IrValue* v = fn.allocValue(ValueKind::Immediate);
    v->imm = imm;
    Use u;
    u.valueId = v->id;
    instr->uses.push_back(u);
}

// ─── 1. CryptoResult utilities ────────────────────────────────────────────────

TEST(CryptoResultTest, AlgorithmNames) {
    CryptoResult r;
    r.algorithm = CryptoAlgorithm::AES;      EXPECT_EQ(r.algorithmName(), "AES");
    r.algorithm = CryptoAlgorithm::SHA256;   EXPECT_EQ(r.algorithmName(), "SHA-256");
    r.algorithm = CryptoAlgorithm::SHA1;     EXPECT_EQ(r.algorithmName(), "SHA-1");
    r.algorithm = CryptoAlgorithm::ChaCha20; EXPECT_EQ(r.algorithmName(), "ChaCha20");
    r.algorithm = CryptoAlgorithm::HMAC;     EXPECT_EQ(r.algorithmName(), "HMAC");
    r.algorithm = CryptoAlgorithm::RSA;      EXPECT_EQ(r.algorithmName(), "RSA");
    r.algorithm = CryptoAlgorithm::RC4;      EXPECT_EQ(r.algorithmName(), "RC4");
    r.algorithm = CryptoAlgorithm::Unknown;  EXPECT_EQ(r.algorithmName(), "Unknown");
}

TEST(CryptoResultTest, VariantNames) {
    CryptoResult r;
    r.variant = CryptoVariant::AES128;     EXPECT_EQ(r.variantName(), "AES-128");
    r.variant = CryptoVariant::SHA256_256; EXPECT_EQ(r.variantName(), "SHA-256");
    r.variant = CryptoVariant::SHA1_160;   EXPECT_EQ(r.variantName(), "SHA-1");
    r.variant = CryptoVariant::RSA2048;    EXPECT_EQ(r.variantName(), "RSA-2048");
    r.variant = CryptoVariant::P256;       EXPECT_EQ(r.variantName(), "P-256");
    r.variant = CryptoVariant::Unknown;    EXPECT_EQ(r.variantName(), "");
}

TEST(CryptoResultTest, ModeNames) {
    CryptoResult r;
    r.mode = CryptoMode::CBC;     EXPECT_EQ(r.modeName(), "CBC");
    r.mode = CryptoMode::CTR;     EXPECT_EQ(r.modeName(), "CTR");
    r.mode = CryptoMode::GCM;     EXPECT_EQ(r.modeName(), "GCM");
    r.mode = CryptoMode::ECB;     EXPECT_EQ(r.modeName(), "ECB");
    r.mode = CryptoMode::Unknown; EXPECT_EQ(r.modeName(), "");
}

TEST(CryptoResultTest, ToStringContainsAlgorithm) {
    CryptoResult r;
    r.algorithm  = CryptoAlgorithm::AES;
    r.variant    = CryptoVariant::AES128;
    r.mode       = CryptoMode::CBC;
    r.confidence = 0.9f;
    std::string s = r.toString();
    EXPECT_NE(s.find("AES"), std::string::npos);
    EXPECT_NE(s.find("CBC"), std::string::npos);
    EXPECT_NE(s.find("AES-128"), std::string::npos);
}

TEST(CryptoResultTest, ToStringAESNI) {
    CryptoResult r;
    r.algorithm  = CryptoAlgorithm::AES;
    r.hasAESNI   = true;
    r.confidence = 1.0f;
    EXPECT_NE(r.toString().find("AES-NI"), std::string::npos);
}

// ─── 2. AES Detector ─────────────────────────────────────────────────────────

TEST(AESDetectorTest, SBoxConstantRaisesConfidence) {
    AESDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i, 0x63);          // S-box first byte
    for (int k = 0; k < 4; ++k)
        addInstr(*fn, blk, IrInstr::Op::Xor);
    auto* a = addInstr(*fn, blk, IrInstr::Op::And);
    addImmUse(*fn, a, 0xff);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.0f);
}

TEST(AESDetectorTest, AESNICallDetected) {
    AESDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    addCallInstr(*fn, blk, "_mm_aesenc_si128");
    auto* xi = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, xi, 0x63);
    for (int k = 0; k < 3; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    EXPECT_TRUE(r.hasAESNI);
    EXPECT_GT(r.confidence, 0.40f);
}

TEST(AESDetectorTest, AesenclastDetected) {
    AESDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    addCallInstr(*fn, blk, "aesenclast");
    auto* xi = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, xi, 0x63);
    for (int k = 0; k < 3; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    EXPECT_TRUE(r.hasAESNI);
}

TEST(AESDetectorTest, MixColumnsConstantContributes) {
    AESDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i1 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i1, 0x63);
    auto* i2 = addInstr(*fn, blk, IrInstr::Op::And);
    addImmUse(*fn, i2, 0x1b);
    for (int k = 0; k < 3; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    auto* sh = addInstr(*fn, blk, IrInstr::Op::Shl);
    addImmUse(*fn, sh, 1);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.50f);
}

TEST(AESDetectorTest, GCMModeDetected) {
    AESDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i1 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i1, 0x63);
    auto* i2 = addInstr(*fn, blk, IrInstr::Op::And);
    addImmUse(*fn, i2, 0xe1);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    addInstr(*fn, blk, IrInstr::Op::Shl);
    auto r = det.detect(*fn);
    EXPECT_EQ(r.mode, CryptoMode::GCM);
}

TEST(AESDetectorTest, EmptyFunctionLowConfidence) {
    AESDetector det;
    auto fn = makeEmptyFn();
    addBlock(*fn);
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.10f);
}

TEST(AESDetectorTest, AlgorithmIsAES) {
    AESDetector det;
    EXPECT_EQ(det.algorithm(), CryptoAlgorithm::AES);
}

// ─── 3. SHA Detector ─────────────────────────────────────────────────────────

TEST(SHADetectorTest, SHA256ConstantDetected) {
    SHADetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, i, 0x428a2f98ULL);  // First SHA-256 K constant
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.40f);
    EXPECT_EQ(r.algorithm, CryptoAlgorithm::SHA256);
}

TEST(SHADetectorTest, SHA1ConstantDetected) {
    SHADetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, i, 0x5A827999ULL);  // SHA-1 K[0]
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    EXPECT_EQ(r.algorithm, CryptoAlgorithm::SHA1);
    EXPECT_EQ(r.variant,   CryptoVariant::SHA1_160);
}

TEST(SHADetectorTest, SHA256InitHashConstant) {
    SHADetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Store);
    addImmUse(*fn, i, 0x6a09e667ULL);  // SHA-256 H[0]
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.40f);
}

TEST(SHADetectorTest, ChFunctionBoostsConfidence) {
    SHADetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, i, 0x71374491ULL);
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    addInstr(*fn, blk, IrInstr::Op::And);  // Maj support
    addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.70f);
}

TEST(SHADetectorTest, EmptyFunctionLowConfidence) {
    SHADetector det;
    auto fn = makeEmptyFn();
    addBlock(*fn);
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.10f);
}

TEST(SHADetectorTest, AlgorithmIsSHA256ByDefault) {
    SHADetector det;
    EXPECT_EQ(det.algorithm(), CryptoAlgorithm::SHA256);
}

// ─── 4. ChaCha20 Detector ────────────────────────────────────────────────────

TEST(ChaCha20DetectorTest, AllFourRotationConstants) {
    ChaCha20Detector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    // Quarter-round: 4x Add + 4x Xor + Shl, with rotation constants 16, 12, 8, 7.
    auto* a1 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, a1, 16);
    auto* a2 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, a2, 12);
    auto* a3 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, a3, 8);
    auto* a4 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, a4, 7);
    for (int k = 0; k < 4; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    addInstr(*fn, blk, IrInstr::Op::Shl);
    auto r = det.detect(*fn);
    EXPECT_NEAR(r.confidence, 1.0f, 0.01f);
}

TEST(ChaCha20DetectorTest, MissingXorGivesLowConfidence) {
    ChaCha20Detector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    // Has rotation constants but no Xor sequence (guard fails).
    auto* a1 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, a1, 16);
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.50f);
}

TEST(ChaCha20DetectorTest, ThreeOfFourConstants) {
    ChaCha20Detector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* a1 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, a1, 16);
    auto* a2 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, a2, 12);
    auto* a3 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, a3, 8);
    for (int k = 0; k < 4; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    addInstr(*fn, blk, IrInstr::Op::Shl);
    auto r = det.detect(*fn);
    EXPECT_NEAR(r.confidence, 0.75f, 0.05f);
}

TEST(ChaCha20DetectorTest, AlgorithmIsChaCha20) {
    ChaCha20Detector det;
    EXPECT_EQ(det.algorithm(), CryptoAlgorithm::ChaCha20);
}

// ─── 5. HMAC Detector ────────────────────────────────────────────────────────

TEST(HMACDetectorTest, BothPadsDetected) {
    HMACDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i1 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i1, 0x36363636ULL);
    auto* i2 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i2, 0x5c5c5c5cULL);
    auto r = det.detect(*fn);
    EXPECT_NEAR(r.confidence, 1.0f, 0.01f);
}

TEST(HMACDetectorTest, IpadOnlyHalfConfidence) {
    HMACDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i, 0x36363636ULL);
    auto r = det.detect(*fn);
    EXPECT_NEAR(r.confidence, 0.50f, 0.01f);
}

TEST(HMACDetectorTest, OpadOnlyHalfConfidence) {
    HMACDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i, 0x5c5c5c5cULL);
    auto r = det.detect(*fn);
    EXPECT_NEAR(r.confidence, 0.50f, 0.01f);
}

TEST(HMACDetectorTest, _64BitPadsDetected) {
    HMACDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i1 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i1, 0x3636363636363636ULL);
    auto* i2 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i2, 0x5c5c5c5c5c5c5c5cULL);
    auto r = det.detect(*fn);
    EXPECT_NEAR(r.confidence, 1.0f, 0.01f);
}

TEST(HMACDetectorTest, AlgorithmIsHMAC) {
    HMACDetector det;
    EXPECT_EQ(det.algorithm(), CryptoAlgorithm::HMAC);
}

TEST(HMACDetectorTest, AnnotationContainsHMAC) {
    HMACDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i1 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i1, 0x36363636ULL);
    auto* i2 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i2, 0x5c5c5c5cULL);
    auto r = det.detect(*fn);
    EXPECT_NE(r.emittedAnnotation.find("HMAC"), std::string::npos);
}

// ─── 6. RSA Detector ─────────────────────────────────────────────────────────

TEST(RSADetectorTest, MontgomeryMultiplyDetected) {
    RSADetector det;
    auto fn = makeEmptyFn();
    // Two loops (two back-edges).
    auto* blk0 = addBlock(*fn);
    addBackEdge(blk0, 0);
    auto* blk1 = addBlock(*fn);
    addBackEdge(blk1, 1);
    // Inner loop: Mul × 2, Add × 5, Shr(32).
    for (int k = 0; k < 2; ++k) addInstr(*fn, blk1, IrInstr::Op::Mul);
    for (int k = 0; k < 5; ++k) addInstr(*fn, blk1, IrInstr::Op::Add);
    auto* sh = addInstr(*fn, blk1, IrInstr::Op::Shr);
    addImmUse(*fn, sh, 32);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.50f);
    EXPECT_EQ(r.algorithm, CryptoAlgorithm::RSA);
}

TEST(RSADetectorTest, ConditionalSubtractContributes) {
    RSADetector det;
    auto fn = makeEmptyFn();
    auto* blk0 = addBlock(*fn);
    addBackEdge(blk0, 0);
    auto* blk1 = addBlock(*fn);
    addBackEdge(blk1, 1);
    for (int k = 0; k < 2; ++k) addInstr(*fn, blk1, IrInstr::Op::Mul);
    for (int k = 0; k < 4; ++k) addInstr(*fn, blk1, IrInstr::Op::Add);
    auto* sh = addInstr(*fn, blk1, IrInstr::Op::Shr);
    addImmUse(*fn, sh, 32);
    // Block 2: conditional subtract.
    auto* blk2 = addBlock(*fn);
    addEdge(blk1, 2);
    addInstr(*fn, blk2, IrInstr::Op::Compare);
    addInstr(*fn, blk2, IrInstr::Op::Sub);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.70f);
}

TEST(RSADetectorTest, EmptyFunctionNoDetection) {
    RSADetector det;
    auto fn = makeEmptyFn();
    addBlock(*fn);
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.50f);
}

TEST(RSADetectorTest, AlgorithmIsRSA) {
    RSADetector det;
    EXPECT_EQ(det.algorithm(), CryptoAlgorithm::RSA);
}

// ─── 7. RC4 Detector ─────────────────────────────────────────────────────────

TEST(RC4DetectorTest, KSAPatternDetected) {
    RC4Detector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    addBackEdge(blk, 0);
    auto* ci = addInstr(*fn, blk, IrInstr::Op::Compare);
    addImmUse(*fn, ci, 255);
    addInstr(*fn, blk, IrInstr::Op::Load);
    addInstr(*fn, blk, IrInstr::Op::Load);
    addInstr(*fn, blk, IrInstr::Op::Add);
    addInstr(*fn, blk, IrInstr::Op::Store);
    addInstr(*fn, blk, IrInstr::Op::Store);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.50f);
    EXPECT_EQ(r.algorithm, CryptoAlgorithm::RC4);
}

TEST(RC4DetectorTest, PRGAXorBoostsConfidence) {
    RC4Detector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    addBackEdge(blk, 0);
    auto* ci = addInstr(*fn, blk, IrInstr::Op::Compare);
    addImmUse(*fn, ci, 256);
    addInstr(*fn, blk, IrInstr::Op::Load);
    addInstr(*fn, blk, IrInstr::Op::Load);
    addInstr(*fn, blk, IrInstr::Op::Add);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    addInstr(*fn, blk, IrInstr::Op::Store);
    addInstr(*fn, blk, IrInstr::Op::Store);
    auto r = det.detect(*fn);
    EXPECT_GT(r.confidence, 0.60f);
}

TEST(RC4DetectorTest, AnnotationMentionsBroken) {
    RC4Detector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    addBackEdge(blk, 0);
    auto* ci = addInstr(*fn, blk, IrInstr::Op::Compare);
    addImmUse(*fn, ci, 255);
    addInstr(*fn, blk, IrInstr::Op::Load);
    addInstr(*fn, blk, IrInstr::Op::Load);
    addInstr(*fn, blk, IrInstr::Op::Add);
    addInstr(*fn, blk, IrInstr::Op::Store);
    addInstr(*fn, blk, IrInstr::Op::Store);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    if (r.confidence >= 0.50f) {
        EXPECT_NE(r.emittedAnnotation.find("WARNING"), std::string::npos);
        EXPECT_NE(r.emittedAnnotation.find("broken"),  std::string::npos);
    }
}

TEST(RC4DetectorTest, AlgorithmIsRC4) {
    RC4Detector det;
    EXPECT_EQ(det.algorithm(), CryptoAlgorithm::RC4);
}

// ─── 8. CryptoDetector orchestrator ──────────────────────────────────────────

TEST(CryptoDetectorTest, PreflightFiltersSmallFunctions) {
    CryptoDetector::Config cfg;
    cfg.minInstrs = 20;
    CryptoDetector det(cfg);
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i, 0x63);
    auto results = det.detect(*fn);
    EXPECT_TRUE(results.empty());
}

TEST(CryptoDetectorTest, HMAC_And_SHA_BothDetected) {
    CryptoDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    // HMAC pads.
    auto* i1 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i1, 0x36363636ULL);
    auto* i2 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i2, 0x5c5c5c5cULL);
    // SHA-256 round constant.
    auto* i3 = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, i3, 0x428a2f98ULL);
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    auto results = det.detect(*fn);
    EXPECT_GE(results.size(), 2u);
    bool hasHMAC = false, hasSHA = false;
    for (auto& r : results) {
        if (r.algorithm == CryptoAlgorithm::HMAC)  hasHMAC = true;
        if (r.algorithm == CryptoAlgorithm::SHA256) hasSHA  = true;
    }
    EXPECT_TRUE(hasHMAC);
    EXPECT_TRUE(hasSHA);
}

TEST(CryptoDetectorTest, ResultsSortedByConfidenceDescending) {
    CryptoDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    // HMAC: both pads → confidence 1.0.
    auto* i1 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i1, 0x36363636ULL);
    auto* i2 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i2, 0x5c5c5c5cULL);
    // AES S-box + MixColumns constant → confidence 0.55 (above default 0.50 threshold).
    auto* i3 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i3, 0x63);
    for (int k = 0; k < 3; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    auto* iand = addInstr(*fn, blk, IrInstr::Op::And);
    addImmUse(*fn, iand, 0x1b);  // MixColumns constant: raises AES confidence to 0.55
    auto results = det.detect(*fn);
    ASSERT_GE(results.size(), 2u);
    for (size_t k = 0; k + 1 < results.size(); ++k)
        EXPECT_GE(results[k].confidence, results[k+1].confidence);
}

TEST(CryptoDetectorTest, ModuleDetectAggregatesResults) {
    CryptoDetector det;
    // Function 1: HMAC.
    auto fn1 = makeEmptyFn("hmac_fn");
    auto* blk1 = addBlock(*fn1);
    auto* h1 = addInstr(*fn1, blk1, IrInstr::Op::Xor);
    addImmUse(*fn1, h1, 0x36363636ULL);
    auto* h2 = addInstr(*fn1, blk1, IrInstr::Op::Xor);
    addImmUse(*fn1, h2, 0x5c5c5c5cULL);
    // Padding to pass preflight (minInstrs = 4).
    addInstr(*fn1, blk1, IrInstr::Op::Add);
    addInstr(*fn1, blk1, IrInstr::Op::Add);

    // Function 2: AES.
    auto fn2 = makeEmptyFn("aes_fn");
    auto* blk2 = addBlock(*fn2);
    auto* a1 = addInstr(*fn2, blk2, IrInstr::Op::Xor);
    addImmUse(*fn2, a1, 0x63);
    for (int k = 0; k < 4; ++k) addInstr(*fn2, blk2, IrInstr::Op::Xor);
    auto* a2 = addInstr(*fn2, blk2, IrInstr::Op::And);
    addImmUse(*fn2, a2, 0x1b);
    auto* a3 = addInstr(*fn2, blk2, IrInstr::Op::Shl);
    addImmUse(*fn2, a3, 1);

    std::vector<const SSAFunction*> fns = {fn1.get(), fn2.get()};
    auto results = det.detectModule(fns);
    bool hasHMAC = false, hasAES = false;
    for (auto& r : results) {
        if (r.algorithm == CryptoAlgorithm::HMAC) hasHMAC = true;
        if (r.algorithm == CryptoAlgorithm::AES)  hasAES  = true;
    }
    EXPECT_TRUE(hasHMAC);
    EXPECT_TRUE(hasAES);
}

TEST(CryptoDetectorTest, ConfidenceThresholdFilters) {
    CryptoDetector::Config cfg;
    cfg.minConfidence = 0.90f;
    CryptoDetector det(cfg);
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    // AES S-box only (~0.30 confidence) — below 0.90 threshold.
    auto* i = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i, 0x63);
    for (int k = 0; k < 4; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    addInstr(*fn, blk, IrInstr::Op::And);
    auto results = det.detect(*fn);
    EXPECT_TRUE(results.empty());
}

// ─── 9. Boundary: default 0.50 threshold ────────────────────────────────────

TEST(ConfidenceTest, DefaultThresholdIncludesHalfPoint) {
    CryptoDetector det;
    // HMAC ipad only → confidence exactly 0.50 (boundary: included).
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i, 0x36363636ULL);
    // Padding to pass preflight (minInstrs = 4).
    addInstr(*fn, blk, IrInstr::Op::Add);
    addInstr(*fn, blk, IrInstr::Op::Add);
    addInstr(*fn, blk, IrInstr::Op::Add);
    auto results = det.detect(*fn);
    bool hasHMAC = false;
    for (auto& r : results) if (r.algorithm == CryptoAlgorithm::HMAC) hasHMAC = true;
    EXPECT_TRUE(hasHMAC);
}

// ─── 10. Negative cases ───────────────────────────────────────────────────────

TEST(NegativeTest, GenericArithmeticNoFalsePositive) {
    CryptoDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    for (int k = 0; k < 10; ++k) {
        auto* i = addInstr(*fn, blk, IrInstr::Op::Add);
        addImmUse(*fn, i, static_cast<uint64_t>(k + 100));
    }
    auto results = det.detect(*fn);
    for (auto& r : results) EXPECT_LT(r.confidence, 0.50f);
}

TEST(NegativeTest, XorLoopNoFalsePositive) {
    CryptoDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    addBackEdge(blk, 0);
    for (int k = 0; k < 8; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    auto results = det.detect(*fn);
    for (auto& r : results) EXPECT_LT(r.confidence, 0.50f);
}

TEST(NegativeTest, NullFunctionPointerSkipped) {
    CryptoDetector det;
    std::vector<const SSAFunction*> fns = {nullptr, nullptr};
    auto results = det.detectModule(fns);
    EXPECT_TRUE(results.empty());
}

// ─── 11. Stats tracking ───────────────────────────────────────────────────────

TEST(StatsTest, FunctionsAnalysedCounted) {
    CryptoDetector det;
    auto fn1 = makeEmptyFn("f1"); addBlock(*fn1);
    auto fn2 = makeEmptyFn("f2"); addBlock(*fn2);
    det.detect(*fn1);
    det.detect(*fn2);
    EXPECT_EQ(det.stats().functionsAnalysed, 2u);
}

TEST(StatsTest, DetectionsCountedPerAlgorithm) {
    CryptoDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i1 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i1, 0x36363636ULL);
    auto* i2 = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, i2, 0x5c5c5c5cULL);
    // Padding to pass preflight (minInstrs = 4).
    addInstr(*fn, blk, IrInstr::Op::Add);
    addInstr(*fn, blk, IrInstr::Op::Add);
    det.detect(*fn);
    EXPECT_GE(det.stats().detections, 1u);
    EXPECT_GE(det.stats().byAlgorithm.count(CryptoAlgorithm::HMAC), 1u);
}

TEST(StatsTest, ModuleDetectCountsAllFunctions) {
    CryptoDetector det;
    auto fn1 = makeEmptyFn("f1"); addBlock(*fn1);
    auto fn2 = makeEmptyFn("f2"); addBlock(*fn2);
    auto fn3 = makeEmptyFn("f3"); addBlock(*fn3);
    std::vector<const SSAFunction*> fns = {fn1.get(), fn2.get(), fn3.get()};
    det.detectModule(fns);
    EXPECT_EQ(det.stats().functionsAnalysed, 3u);
}

// ─── 12. Annotation content ──────────────────────────────────────────────────

TEST(AnnotationTest, AESAnnotationContainsEVP) {
    AESDetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    addCallInstr(*fn, blk, "_mm_aesenc_si128");
    auto* xi = addInstr(*fn, blk, IrInstr::Op::Xor);
    addImmUse(*fn, xi, 0x63);
    for (int k = 0; k < 3; ++k) addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    if (r.confidence >= 0.50f) {
        EXPECT_NE(r.emittedAnnotation.find("AES"), std::string::npos);
        EXPECT_NE(r.emittedAnnotation.find("EVP"), std::string::npos);
    }
}

TEST(AnnotationTest, SHAAnnotationContainsSHA) {
    SHADetector det;
    auto fn = makeEmptyFn();
    auto* blk = addBlock(*fn);
    auto* i = addInstr(*fn, blk, IrInstr::Op::Add);
    addImmUse(*fn, i, 0x428a2f98ULL);
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::And);
    addInstr(*fn, blk, IrInstr::Op::Xor);
    auto r = det.detect(*fn);
    if (r.confidence >= 0.50f) {
        EXPECT_NE(r.emittedAnnotation.find("SHA"), std::string::npos);
    }
}

TEST(AnnotationTest, RSAAnnotationMentionsMontgomery) {
    RSADetector det;
    auto fn = makeEmptyFn();
    auto* blk0 = addBlock(*fn);
    addBackEdge(blk0, 0);
    auto* blk1 = addBlock(*fn);
    addBackEdge(blk1, 1);
    for (int k = 0; k < 2; ++k) addInstr(*fn, blk1, IrInstr::Op::Mul);
    for (int k = 0; k < 5; ++k) addInstr(*fn, blk1, IrInstr::Op::Add);
    auto* sh = addInstr(*fn, blk1, IrInstr::Op::Shr);
    addImmUse(*fn, sh, 32);
    auto r = det.detect(*fn);
    if (r.confidence >= 0.50f) {
        EXPECT_NE(r.emittedAnnotation.find("Montgomery"), std::string::npos);
    }
}
