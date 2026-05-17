/**
 * @file src/crypto_detect/crypto_detector.cpp
 * @brief CryptoDetector orchestrator and CryptoResult utilities.
 */

#include <memory>
#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace crypto_detect {

// ─── CryptoResult utilities ───────────────────────────────────────────────────

std::string CryptoResult::algorithmName() const noexcept {
    switch (algorithm) {
    case CryptoAlgorithm::AES:       return "AES";
    case CryptoAlgorithm::SHA256:    return "SHA-256";
    case CryptoAlgorithm::SHA1:      return "SHA-1";
    case CryptoAlgorithm::ChaCha20:  return "ChaCha20";
    case CryptoAlgorithm::HMAC:      return "HMAC";
    case CryptoAlgorithm::RSA:       return "RSA";
    case CryptoAlgorithm::ECC:       return "ECC";
    case CryptoAlgorithm::RC4:       return "RC4";
    case CryptoAlgorithm::DH:        return "DH";
    case CryptoAlgorithm::BLAKE2:    return "BLAKE2";
    case CryptoAlgorithm::MD5:       return "MD5";
    case CryptoAlgorithm::Poly1305:  return "Poly1305";
    case CryptoAlgorithm::Salsa20:   return "Salsa20";
    default:                         return "Unknown";
    }
}

std::string CryptoResult::variantName() const noexcept {
    switch (variant) {
    case CryptoVariant::AES128:       return "AES-128";
    case CryptoVariant::AES192:       return "AES-192";
    case CryptoVariant::AES256:       return "AES-256";
    case CryptoVariant::SHA1_160:     return "SHA-1";
    case CryptoVariant::SHA256_256:   return "SHA-256";
    case CryptoVariant::SHA512_512:   return "SHA-512";
    case CryptoVariant::RSA1024:      return "RSA-1024";
    case CryptoVariant::RSA2048:      return "RSA-2048";
    case CryptoVariant::RSA4096:      return "RSA-4096";
    case CryptoVariant::P256:         return "P-256";
    case CryptoVariant::P384:         return "P-384";
    case CryptoVariant::P521:         return "P-521";
    case CryptoVariant::Curve25519:   return "Curve25519";
    default:                          return "";
    }
}

std::string CryptoResult::modeName() const noexcept {
    switch (mode) {
    case CryptoMode::ECB: return "ECB";
    case CryptoMode::CBC: return "CBC";
    case CryptoMode::CTR: return "CTR";
    case CryptoMode::GCM: return "GCM";
    case CryptoMode::CCM: return "CCM";
    case CryptoMode::XTS: return "XTS";
    case CryptoMode::OCB: return "OCB";
    default:              return "";
    }
}

std::string CryptoResult::toString() const {
    std::ostringstream oss;
    oss << algorithmName();
    if (!variantName().empty()) oss << " [" << variantName() << "]";
    if (!modeName().empty())    oss << " " << modeName();
    if (hasAESNI)               oss << " (AES-NI)";
    oss << " confidence=" << confidence;
    return oss.str();
}

// ─── CryptoDetector ───────────────────────────────────────────────────────────

CryptoDetector::CryptoDetector(Config cfg) : cfg_(cfg) {
    // Register all detectors in priority order (highest specificity first).
    // HMAC is checked first because it wraps other algorithms; RC4 is last
    // as it is purely structural with no external constant dependency.
    detectors_.push_back(std::make_unique<HMACDetector>());
    detectors_.push_back(std::make_unique<AESDetector>());
    detectors_.push_back(std::make_unique<SHADetector>());
    detectors_.push_back(std::make_unique<ChaCha20Detector>());
    detectors_.push_back(std::make_unique<RSADetector>());
    detectors_.push_back(std::make_unique<RC4Detector>());
}

bool CryptoDetector::passesPreflight(const ssa::SSAFunction& fn) const {
    if (static_cast<int>(fn.blockCount()) < cfg_.minBlocks) return false;
    int instrCount = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        instrCount += static_cast<int>(blk->instrs.size());
    }
    return instrCount >= cfg_.minInstrs;
}

CryptoDetector::ResultList CryptoDetector::detect(const ssa::SSAFunction& fn) const {
    ++stats_.functionsAnalysed;
    ResultList results;
    if (!passesPreflight(fn)) return results;

    for (const auto& det : detectors_) {
        auto r = det->detect(fn);
        if (r.confidence >= cfg_.minConfidence) {
            ++stats_.detections;
            ++stats_.byAlgorithm[r.algorithm];
            results.push_back(std::move(r));
        }
    }
    // Sort by confidence descending.
    std::sort(results.begin(), results.end(),
              [](const CryptoResult& a, const CryptoResult& b) {
                  return a.confidence > b.confidence;
              });
    return results;
}

CryptoDetector::ResultList CryptoDetector::detectModule(
    const std::vector<const ssa::SSAFunction*>& fns) const {
    ResultList all;
    for (const auto* fn : fns) {
        if (!fn) continue;
        auto res = detect(*fn);
        all.insert(all.end(), res.begin(), res.end());
    }
    // Sort by confidence descending.
    std::sort(all.begin(), all.end(),
              [](const CryptoResult& a, const CryptoResult& b) {
                  return a.confidence > b.confidence;
              });
    return all;
}

} // namespace crypto_detect
} // namespace retdec
