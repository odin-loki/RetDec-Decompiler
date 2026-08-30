/**
 * @file include/retdec/crypto_detect/crypto_detect.h
 * @brief Semantic Recovery — Cryptographic Algorithm Detector (Stage 29).
 *
 * ## Overview
 *
 * This module identifies compiled cryptographic primitives from their
 * structural IR fingerprints and emits annotated C++ with algorithm,
 * key-size, and mode commentary.
 *
 * Detection uses two complementary strategies:
 *
 *   **Constant fingerprinting** — Scan Immediate operands in the IR for
 *   cryptographic magic constants that are uniquely characteristic of each
 *   algorithm (S-box values, round constants, rotation amounts, pad bytes).
 *
 *   **Structural fingerprinting** — Identify the loop / computation patterns
 *   that implement each cryptographic primitive (round function structure,
 *   key schedule shape, Montgomery multiplication loop).
 *
 * ## Algorithms detected
 *
 * ### AES
 *
 * Constant fingerprints (any of):
 *   - S-box first bytes: `0x63, 0x7c, 0x77, 0x7b, 0xf2` (SubBytes table).
 *   - Inverse S-box first bytes: `0x52, 0x09, 0x6a`.
 *   - Round constant (Rcon): `0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80`.
 *   - MixColumns GF(2⁸) multiplier: `0x1b` (the irreducible polynomial modulus).
 *
 * Structural fingerprints:
 *   - XOR + And + Shl/Shr in a tight loop (SubBytes substitution).
 *   - AES-NI: Call to `aesenc` / `aesenclast` / `_mm_aesenc_si128`.
 *
 * Mode detection:
 *   - CBC: XOR before or after the AES block (IV handling).
 *   - CTR: an incrementing counter variable + XOR with keystream.
 *   - GCM: GHASH polynomial multiplication (Xor + Shl + And with 0xe1 polynomial).
 *
 * ### SHA-256 / SHA-1
 *
 * SHA-256 constant fingerprints:
 *   - Round constants K[0..63]: first few are `0x428a2f98, 0x71374491, 0xb5c0fbcf`.
 *   - Ch function: `(e & f) ^ (~e & g)` — And + Xor + Not pattern.
 *   - Maj function: `(a & b) ^ (a & c) ^ (b & c)`.
 *   - Sigma0 rotation amounts: {2, 13, 22}, Sigma1: {6, 11, 25}.
 *
 * SHA-1 constant fingerprints:
 *   - Initial hash values: `0x67452301, 0xEFCDAB89, 0x98BADCFE`.
 *   - Round constant K: `0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6`.
 *   - ROTL(1) in message schedule: Shl by 1 + Shr by 31 + Or.
 *
 * ### ChaCha20
 *
 * Structural fingerprint — quarter-round:
 *   ```
 *   a += b; d ^= a; d <<<= 16;
 *   c += d; b ^= c; b <<<= 12;
 *   a += b; d ^= a; d <<<= 8;
 *   c += d; b ^= c; b <<<= 7;
 *   ```
 * IR signals: Add + Xor + (Shl+Shr+Or for rotate) sequence, with rotation
 * constants 16, 12, 8, 7 appearing as Immediate values.
 *
 * Constant fingerprints — ChaCha/Salsa sigma words for "expand 32-byte k":
 *   `0x61707865, 0x3320646e, 0x79622d32, 0x6b206574` (little-endian ASCII).
 *
 * ### MD5
 *
 * Constant fingerprints:
 *   - Sine-table K[0]: `0xd76aa478` (MD5-only; SHA-1 does not use this).
 *   - Init words `0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476` are shared
 *     with SHA-1 and are supporting evidence only — never sufficient alone.
 *
 * ### CRC-32
 *
 * Constant fingerprints (either polynomial):
 *   - Reflected IEEE: `0xEDB88320`.
 *   - Normal / Ethernet: `0x04C11DB7`.
 *
 * ### Blowfish
 *
 * Constant fingerprints — P-array first words (digits of π; uniquely Blowfish):
 *   `0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344`.
 * Any one P-array word is sufficient. Base64 alphabets are not scanned:
 * SSA exposes immediates only (no string-table / data-section access).
 *
 * ### DES
 *
 * Constant fingerprints — OpenSSL/SSLeay DES_SPtrans packed S-box+P words:
 *   `0x02080800, 0x02080802, 0x00080802, 0x02000802`.
 * Any one distinctive multi-bit word is sufficient. Raw S-box nibbles and
 * IP/FP swap masks (`0x0f0f0f0f`, `0x33333333`, …) are not unique.
 *
 * ### HMAC
 *
 * Constant fingerprints (unmistakable):
 *   - ipad: `0x36` repeated — `0x36363636` as a 32-bit immediate.
 *   - opad: `0x5c` repeated — `0x5c5c5c5c` as a 32-bit immediate.
 *   - These XOR with the key block (64 bytes for SHA-256).
 *
 * ### RSA / Montgomery Multiplication
 *
 * Structural fingerprints:
 *   - Multi-precision multiply loop: nested loops multiplying 32/64-bit limbs
 *     with carry propagation (Add+Mul+carry chain).
 *   - Conditional subtract: Compare followed by a sub that brings the result
 *     back into the field (the Montgomery reduction final step).
 *   - Large modulus constant in the data section (256+ bytes for RSA-2048).
 *
 * ### RC4
 *
 * Structural fingerprints — Key Scheduling Algorithm (KSA):
 *   - A 256-iteration initialisation loop: `for i in 0..256: S[i] = i`.
 *   - A second 256-iteration swap loop involving the key bytes.
 *   - The state array size of exactly 256 bytes.
 *
 * Pseudo-Random Generation Algorithm (PRGA):
 *   - XOR of the output with `S[S[i]+S[j]]`.
 *
 * ## Output
 *
 * Each `CryptoResult` carries:
 *   - `algorithm`     — detected cryptographic algorithm
 *   - `variant`       — e.g. AES-128 vs AES-256, SHA-1 vs SHA-256
 *   - `mode`          — CBC / CTR / GCM / ECB for block ciphers
 *   - `confidence`    — in [0.0, 1.0]
 *   - `emittedAnnotation` — C++ comment + API sketch
 */

#ifndef RETDEC_CRYPTO_DETECT_H
#define RETDEC_CRYPTO_DETECT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace retdec {
namespace ssa { class SSAFunction; }
} // namespace retdec

namespace retdec {
namespace crypto_detect {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class CryptoAlgorithm : uint8_t {
    Unknown,
    AES,
    SHA256,
    SHA1,
    ChaCha20,
    HMAC,        ///< HMAC wrapper (combined with underlying hash)
    RSA,         ///< RSA via Montgomery multiplication
    ECC,         ///< Elliptic curve field arithmetic
    RC4,
    DH,          ///< Diffie-Hellman (same Montgomery multiply pattern as RSA)
    BLAKE2,
    MD5,
    CRC,         ///< CRC-32 / CRC-32C polynomial fingerprint
    Poly1305,
    Salsa20,
    Blowfish,    ///< Blowfish P-array (π digits) fingerprint
    DES,         ///< DES_SPtrans packed S-box+P fingerprint
};

enum class CryptoMode : uint8_t {
    Unknown,
    ECB,
    CBC,
    CTR,
    GCM,
    CCM,
    XTS,
    OCB,
};

enum class CryptoVariant : uint8_t {
    Unknown,
    AES128,
    AES192,
    AES256,
    SHA1_160,
    SHA256_256,
    SHA512_512,
    RSA1024,
    RSA2048,
    RSA4096,
    P256,        ///< NIST P-256 (secp256r1)
    P384,
    P521,
    Curve25519,
};

// ─── Crypto detection result ──────────────────────────────────────────────────

struct CryptoResult {
    CryptoAlgorithm algorithm  = CryptoAlgorithm::Unknown;
    CryptoVariant   variant    = CryptoVariant::Unknown;
    CryptoMode      mode       = CryptoMode::Unknown;
    float           confidence = 0.0f;
    bool            hasAESNI   = false;   ///< hardware AES-NI detected
    std::string     emittedAnnotation;    ///< C++ comment + API sketch

    std::string algorithmName() const noexcept;
    std::string variantName()   const noexcept;
    std::string modeName()      const noexcept;
    std::string toString()      const;
};

// ─── Evidence structs ─────────────────────────────────────────────────────────

struct AESEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasSBox          = false;   ///< 0x63 S-box constant
    bool  hasRcon          = false;   ///< Rcon XOR in key schedule
    bool  hasMixCols       = false;   ///< 0x1b GF multiplier
    bool  hasAESNI         = false;   ///< aesenc/aesenclast call
    bool  hasRoundLoop     = false;   ///< XOR+And+Shl structure
    CryptoMode mode        = CryptoMode::Unknown;
};

struct SHAEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasRoundConst    = false;   ///< 0x428a2f98 or 0x5A827999
    bool  hasChFunction    = false;   ///< And+Xor+Not pattern
    bool  hasMajFunction   = false;   ///< triple And+Xor
    bool  hasRotations     = false;   ///< specific rotation amounts
    bool  isSHA1           = false;   ///< SHA-1 vs SHA-256
};

struct ChaCha20Evidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasRotConst16    = false;
    bool  hasRotConst12    = false;
    bool  hasRotConst8     = false;
    bool  hasRotConst7     = false;
    bool  hasAddXorRotSeq  = false;   ///< Add+Xor+(Shl+Shr+Or) sequence
    bool  hasSigmaConst    = false;   ///< "expand 32-byte k" ASCII words
    int   sigmaWords       = 0;       ///< how many of the four sigma words
};

struct Salsa20Evidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasRotConst7     = false;
    bool  hasRotConst9     = false;
    bool  hasRotConst13    = false;
    bool  hasRotConst18    = false;
    bool  hasAddXorRotSeq  = false;   ///< Add+Xor+(Shl or Or)
};

/** RFC 7539 clamp / poly1305-donna 26-bit limb masks. */
struct Poly1305Evidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasClampLo       = false;   ///< 0x0ffffffc0fffffff
    bool  hasClampHi       = false;   ///< 0x0ffffffc0ffffffc
    bool  hasDonnaR1       = false;   ///< 0x3ffff03
    bool  hasDonnaR2       = false;   ///< 0x3ffc0ff
    bool  hasDonnaR3       = false;   ///< 0x3f03fff
};

struct HMACEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasIpad          = false;   ///< 0x36363636
    bool  hasOpad          = false;   ///< 0x5c5c5c5c
};

struct RSAEvidence {
    bool  found              = false;
    float confidence         = 0.0f;
    bool  hasMultiPrecMul    = false; ///< nested loop with Mul+Add+carry
    bool  hasConditionalSub  = false; ///< Compare + Sub (Montgomery reduction)
    bool  hasLargeConstant   = false; ///< large modulus in data section
};

struct RC4Evidence {
    bool  found              = false;
    float confidence         = 0.0f;
    bool  hasKSA             = false; ///< 256-iteration init+swap loop
    bool  hasPRGA            = false; ///< XOR with S[S[i]+S[j]]
    bool  has256Constant     = false; ///< loop bound 256
};

struct MD5Evidence {
    bool  found              = false;
    float confidence         = 0.0f;
    bool  hasSineK           = false; ///< MD5-only T/K table (e.g. 0xd76aa478)
    bool  hasInitMagic       = false; ///< 0x67452301 / 0xefcdab89 / …
};

struct CRCEvidence {
    bool  found              = false;
    float confidence         = 0.0f;
    bool  hasReflectedPoly   = false; ///< 0xEDB88320 CRC-32 IEEE reflected
    bool  hasNormalPoly      = false; ///< 0x04C11DB7 CRC-32 normal
};

struct BlowfishEvidence {
    bool  found              = false;
    float confidence         = 0.0f;
    bool  hasPArray          = false; ///< P-array first words (e.g. 0x243f6a88)
    int   pArrayWords        = 0;     ///< how many of the first four P words
};

struct DESEvidence {
    bool  found              = false;
    float confidence         = 0.0f;
    bool  hasSPtrans         = false; ///< DES_SPtrans packed words (e.g. 0x02080800)
    int   sptransWords       = 0;     ///< how many distinctive SPtrans words
};

// ─── Detector interface ───────────────────────────────────────────────────────

class ICryptoDetector {
public:
    virtual ~ICryptoDetector() = default;
    virtual CryptoResult    detect(const ssa::SSAFunction& fn) const = 0;
    virtual CryptoAlgorithm algorithm() const noexcept = 0;
};

// ─── Per-algorithm detectors ──────────────────────────────────────────────────

/** AES detector — S-box, key schedule, round structure, AES-NI, mode. */
class AESDetector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::AES; }
private:
    AESEvidence analyse(const ssa::SSAFunction& fn) const;
    float       score(const AESEvidence& ev) const;
    CryptoMode  detectMode(const ssa::SSAFunction& fn) const;
    bool        hasAESNI(const ssa::SSAFunction& fn) const;
};

/** SHA-256 / SHA-1 detector — round constants, Ch, Maj, Sigma rotations. */
class SHADetector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::SHA256; }
private:
    SHAEvidence analyse(const ssa::SSAFunction& fn) const;
    float       score(const SHAEvidence& ev) const;
    bool        hasSHA1Constants(const ssa::SSAFunction& fn) const;
    bool        hasSHA256Constants(const ssa::SSAFunction& fn) const;
};

/** ChaCha20 detector — quarter-round Add+Xor+Rotate with constants 16/12/8/7. */
class ChaCha20Detector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::ChaCha20; }
private:
    ChaCha20Evidence analyse(const ssa::SSAFunction& fn) const;
    float            score(const ChaCha20Evidence& ev) const;
};

/**
 * Salsa20 detector — quarter-round Add+Xor+Rotate with constants 7/9/13/18.
 * Does not score the shared "expand 32-byte k" sigma words (ChaCha20 owns those).
 */
class Salsa20Detector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::Salsa20; }
private:
    Salsa20Evidence analyse(const ssa::SSAFunction& fn) const;
    float           score(const Salsa20Evidence& ev) const;
};

/**
 * Poly1305 detector — RFC 7539 64-bit clamp masks and donna 26-bit r limbs.
 * Does not score 0x3ffffff / 0x0fffffff (too common as bit masks).
 */
class Poly1305Detector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::Poly1305; }
private:
    Poly1305Evidence analyse(const ssa::SSAFunction& fn) const;
    float            score(const Poly1305Evidence& ev) const;
};

/**
 * Curve25519 detector — unique Montgomery-ladder constant 121665 (0x1db41).
 * Does not score generic Montgomery multiplication (RSADetector owns that).
 */
class Curve25519Detector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::ECC; }
};

/** HMAC detector — ipad/opad constant XOR. */
class HMACDetector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::HMAC; }
private:
    HMACEvidence analyse(const ssa::SSAFunction& fn) const;
    float        score(const HMACEvidence& ev) const;
};

/** RSA/DH detector — Montgomery multi-precision multiplication loop. */
class RSADetector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::RSA; }
private:
    RSAEvidence analyse(const ssa::SSAFunction& fn) const;
    float       score(const RSAEvidence& ev) const;
};

/** RC4 detector — KSA 256-iteration swap loop, PRGA XOR. */
class RC4Detector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::RC4; }
private:
    RC4Evidence analyse(const ssa::SSAFunction& fn) const;
    float       score(const RC4Evidence& ev) const;
};

/**
 * MD5 detector — sine-table K[] plus init magic.
 * SHA-1 shares 0x67452301 / 0xefcdab89 / 0x98badcfe / 0x10325476; a hit
 * requires at least one MD5-only T/K constant (e.g. 0xd76aa478).
 */
class MD5Detector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::MD5; }
private:
    MD5Evidence analyse(const ssa::SSAFunction& fn) const;
    float       score(const MD5Evidence& ev) const;
};

/** CRC-32 detector — IEEE reflected (0xEDB88320) / normal (0x04C11DB7) polynomials. */
class CRCDetector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::CRC; }
private:
    CRCEvidence analyse(const ssa::SSAFunction& fn) const;
    float       score(const CRCEvidence& ev) const;
};

/**
 * Blowfish detector — P-array first words (π digits).
 * 0x243f6a88 is uniquely Blowfish; SHA-256 H[0] is the distinct 0x6a09e667.
 */
class BlowfishDetector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::Blowfish; }
private:
    BlowfishEvidence analyse(const ssa::SSAFunction& fn) const;
    float            score(const BlowfishEvidence& ev) const;
};

/**
 * DES detector — OpenSSL/SSLeay DES_SPtrans packed S-box+P words.
 * 0x02080800 is uniquely DES; IP/FP masks (0x0f0f0f0f …) are not used.
 */
class DESDetector : public ICryptoDetector {
public:
    CryptoResult    detect(const ssa::SSAFunction& fn) const override;
    CryptoAlgorithm algorithm() const noexcept override { return CryptoAlgorithm::DES; }
private:
    DESEvidence analyse(const ssa::SSAFunction& fn) const;
    float       score(const DESEvidence& ev) const;
};

// ─── Crypto detector orchestrator ────────────────────────────────────────────

/**
 * Runs all registered crypto detectors on a function and returns every
 * algorithm detected above the confidence threshold.  Multiple algorithms
 * can appear in the same function (e.g. HMAC wraps SHA-256).
 */
class CryptoDetector {
public:
    struct Config {
        float minConfidence = 0.50f;
        int   minBlocks     = 1;
        int   minInstrs     = 4;
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        uint32_t functionsAnalysed = 0;
        uint32_t detections        = 0;
        std::unordered_map<CryptoAlgorithm, uint32_t> byAlgorithm;
    };

    using ResultList = std::vector<CryptoResult>;

    explicit CryptoDetector(Config cfg = defaultConfig());

    ResultList detect(const ssa::SSAFunction& fn) const;
    ResultList detectModule(const std::vector<const ssa::SSAFunction*>& fns) const;

    const Stats& stats() const { return stats_; }

private:
    Config cfg_;
    mutable Stats stats_;
    std::vector<std::unique_ptr<ICryptoDetector>> detectors_;

    bool passesPreflight(const ssa::SSAFunction& fn) const;
};

} // namespace crypto_detect
} // namespace retdec

#endif // RETDEC_CRYPTO_DETECT_H
