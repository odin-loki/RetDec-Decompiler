/**
 * @file src/crypto_detect/hmac_detect.cpp
 * @brief HMAC detector — ipad/opad constant fingerprint.
 *
 * ## Constant fingerprints (unmistakable)
 *
 * HMAC pads the key by XOR-ing it with a 64-byte repeated constant:
 *
 *   ipad: 0x36 repeated → `0x36363636` (32-bit) or `0x3636363636363636` (64-bit)
 *   opad: 0x5c repeated → `0x5c5c5c5c` (32-bit) or `0x5c5c5c5c5c5c5c5c` (64-bit)
 *
 * These appear as Immediate operands XOR-ed with 4-byte or 8-byte chunks
 * of the key block (64 bytes for SHA-256, 128 bytes for SHA-512).
 *
 * ## Confidence scoring
 *
 *   ipad constant (0x36363636)     +0.50
 *   opad constant (0x5c5c5c5c)     +0.50
 *   64-bit wide variants           bonus +0.10 per constant (capped at 1.0)
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace crypto_detect {

namespace {

static bool hasImmediate(const ssa::SSAFunction& fn, uint64_t val)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i) continue;
			for (const auto& u: i->uses)
			{
				const auto* v = fn.value(u.valueId);
				if (v && v->kind == ssa::ValueKind::Immediate && v->imm == val) return true;
			}
		}
	}
	return false;
}

} // anonymous namespace

HMACEvidence HMACDetector::analyse(const ssa::SSAFunction& fn) const
{
	HMACEvidence ev;
	const bool ipad32 = hasImmediate(fn, 0x36363636ULL);
	const bool ipad64 = hasImmediate(fn, 0x3636363636363636ULL);
	const bool opad32 = hasImmediate(fn, 0x5c5c5c5cULL);
	const bool opad64 = hasImmediate(fn, 0x5c5c5c5c5c5c5c5cULL);
	ev.hasIpad = ipad32 || ipad64;
	ev.hasOpad = opad32 || opad64;
	ev.found = ev.hasIpad || ev.hasOpad;
	float s = score(ev);
	if (ipad64) s += 0.10f;
	if (opad64) s += 0.10f;
	ev.confidence = s > 1.0f ? 1.0f : s;
	return ev;
}

float HMACDetector::score(const HMACEvidence& ev) const
{
	float s = 0.0f;
	if (ev.hasIpad) s += 0.50f;
	if (ev.hasOpad) s += 0.50f;
	return s > 1.0f ? 1.0f : s;
}

CryptoResult HMACDetector::detect(const ssa::SSAFunction& fn) const
{
	CryptoResult r;
	r.algorithm = CryptoAlgorithm::HMAC;
	auto ev = analyse(fn);
	r.confidence = ev.confidence;
	if (ev.confidence >= 0.50f)
	{
		r.emittedAnnotation =
			"// Cryptographic primitive: HMAC\n"
			"// ipad=0x36 / opad=0x5c key derivation detected\n"
			"// Usage: HMAC(EVP_sha256(), key, key_len, data, data_len, out, &out_len);";
	}
	return r;
}

} // namespace crypto_detect
} // namespace retdec
