/**
 * @file src/crypto_detect/blowfish_detect.cpp
 * @brief Blowfish detector — P-array first words (digits of π).
 *
 * ## Constant fingerprints
 *
 * Blowfish P-array (first four of 18; uniquely Blowfish):
 *   0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344
 *
 * These are the leading π-digit words used to initialise P[].  They do
 * not collide with SHA-256 H[0] (0x6a09e667) or MD5/CRC/ChaCha magics.
 *
 * Base64 alphabets are not scanned here: SSA exposes Immediate operands
 * only, with no string-table / data-section access.  Isolated 0x2b / 0x2f
 * / 0x3d bytes are too common to fingerprint without a table scan.
 *
 * ## Confidence scoring
 *
 *   first P-array word             +0.70
 *   each additional P-array word   +0.10
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

#include <set>

namespace retdec {
namespace crypto_detect {

namespace {

// Blowfish P[0..3] — π digits; not used by AES / SHA / MD5 / CRC / ChaCha.
static const std::set<uint64_t> kBlowfishP = {
	0x243f6a88ULL,
	0x85a308d3ULL,
	0x13198a2eULL,
	0x03707344ULL,
};

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

static int countPArrayWords(const ssa::SSAFunction& fn)
{
	int n = 0;
	for (uint64_t v: kBlowfishP)
		if (hasImmediate(fn, v)) ++n;
	return n;
}

} // anonymous namespace

BlowfishEvidence BlowfishDetector::analyse(const ssa::SSAFunction& fn) const
{
	BlowfishEvidence ev;
	ev.pArrayWords = countPArrayWords(fn);
	ev.hasPArray = ev.pArrayWords > 0;
	ev.found = ev.hasPArray;
	ev.confidence = score(ev);
	return ev;
}

float BlowfishDetector::score(const BlowfishEvidence& ev) const
{
	if (ev.pArrayWords <= 0) return 0.0f;
	float s = 0.70f + 0.10f * static_cast<float>(ev.pArrayWords - 1);
	return s > 1.0f ? 1.0f : s;
}

CryptoResult BlowfishDetector::detect(const ssa::SSAFunction& fn) const
{
	CryptoResult r;
	r.algorithm = CryptoAlgorithm::Blowfish;
	auto ev = analyse(fn);
	r.confidence = ev.confidence;
	if (ev.confidence >= 0.50f)
	{
		r.emittedAnnotation =
			"// Cryptographic primitive: Blowfish\n"
			"// P-array init words (π digits) 0x243f6a88 …\n"
			"// Usage: BF_set_key(&key, len, data); BF_ecb_encrypt(...);";
	}
	return r;
}

} // namespace crypto_detect
} // namespace retdec
