/**
 * @file src/crypto_detect/rsa_detect.cpp
 * @brief RSA / DH detector — Montgomery multi-precision multiplication.
 *
 * ## Structural fingerprints
 *
 * ### Multi-precision multiply (limb-by-limb)
 *
 * Montgomery multiplication processes an arbitrary-precision integer as
 * an array of 32- or 64-bit "limbs".  The inner loop multiplies two limbs
 * and accumulates the carry:
 *
 *   ```c
 *   for (i = 0; i < n; i++) {
 *       carry = 0;
 *       for (j = 0; j < n; j++) {
 *           t = (uint64_t)a[i] * b[j] + c[i+j] + carry;
 *           c[i+j] = (uint32_t)t;
 *           carry  = (uint32_t)(t >> 32);
 *       }
 *       c[i+n] = carry;
 *   }
 *   ```
 *
 * IR signals:
 *   - Nested loop structure (two back-edges / two induction phi nodes).
 *   - Mul instruction (64-bit multiply of limbs).
 *   - Add instructions for accumulating the carry.
 *   - Shr by 32 (extracting the high limb as carry).
 *
 * ### Conditional subtract (Montgomery reduction final step)
 *
 * After the main multiply, the result is conditionally subtracted to bring
 * it back into [0, N):
 *
 *   ```c
 *   if (t >= N) t -= N;
 *   ```
 *
 * IR signals: Compare + conditional branch + Sub in the target block.
 *
 * ### Large constant modulus in data section
 *
 * RSA-2048 has a 256-byte modulus.  This is often embedded as a byte array
 * adjacent to the Montgomery multiply function.
 *
 * ## Confidence scoring
 *
 *   nested loops (two back-edges)    +0.25
 *   Mul + Add + Shr(32) sequence     +0.35
 *   conditional subtract             +0.25
 *   64-bit width arithmetic          +0.15
 */

#include "retdec/crypto_detect/crypto_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace crypto_detect {

namespace {

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op)
{
	int n = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == op) ++n;
	}
	return n;
}

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

// Count back-edges (loops): edge (pred → succ) where succ.id <= pred.id.
static int countBackEdges(const ssa::SSAFunction& fn)
{
	int n = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (uint32_t s: blk->succs)
			if (s <= b) ++n;
	}
	return n;
}

// Montgomery reduction ends with `if (t >= n) t -= n;` -- a compare, a
// conditional branch, and a subtract on the taken arm.
//
// The branch is the part that makes it *conditional*, and it was not being
// asked for: any Compare with a Sub after it in the same block matched, as did
// any Compare at all with a Sub anywhere in any successor. In a function with a
// compare and a subtract in it -- which is most functions -- that is always
// true.
static bool hasConditionalSub(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;

		bool seenCmp = false, seenBranch = false;
		for (const auto* i: blk->instrs)
		{
			if (!i) continue;
			if (i->op == ssa::IrInstr::Op::Compare)
			{
				seenCmp = true;
				continue;
			}
			if (seenCmp && i->op == ssa::IrInstr::Op::CondBranch) seenBranch = true;
		}
		if (!seenCmp || !seenBranch) continue;

		// The subtract is on an arm of that branch, so it is in a successor of
		// this block -- not "any successor of any block with a compare in it".
		for (uint32_t sid: blk->succs)
		{
			const auto* sb = fn.block(sid);
			if (!sb || sb == blk) continue;
			for (const auto* i: sb->instrs)
				if (i && i->op == ssa::IrInstr::Op::Sub) return true;
		}
	}
	return false;
}

} // anonymous namespace

RSAEvidence RSADetector::analyse(const ssa::SSAFunction& fn) const
{
	RSAEvidence ev;
	int backEdges = countBackEdges(fn);
	ev.hasMultiPrecMul = backEdges >= 2 && countOp(fn, ssa::IrInstr::Op::Mul) >= 2
					  && countOp(fn, ssa::IrInstr::Op::Add) >= 4
					  && hasImmediate(fn, 32); // Shr by 32 for carry extraction
	ev.hasConditionalSub = hasConditionalSub(fn);
	// Large constant: detect Shr by 32 and wide Mul (heuristic for 64-bit limbs).
	// Note this is strictly implied by hasMultiPrecMul above, which requires the
	// same immediate and more Muls -- see score(), which does not add both.
	ev.hasLargeConstant = hasImmediate(fn, 32) && countOp(fn, ssa::IrInstr::Op::Mul) >= 1;
	ev.found = ev.hasMultiPrecMul || (ev.hasConditionalSub && ev.hasLargeConstant);
	ev.confidence = score(ev);
	return ev;
}

float RSADetector::score(const RSAEvidence& ev) const
{
	float s = 0.0f;
	if (ev.hasMultiPrecMul) s += 0.60f;
	if (ev.hasConditionalSub) s += 0.25f;
	// hasLargeConstant is `the immediate 32 and >= 1 Mul`, which hasMultiPrecMul
	// already requires along with a second Mul, four Adds and two back edges --
	// so adding both scored one piece of evidence twice, and it was the 0.15
	// that carried a nested integer matrix multiply containing a 32 from 0.85
	// to 1.00 as "RSA / Montgomery multiplication".
	if (ev.hasLargeConstant && !ev.hasMultiPrecMul) s += 0.15f;
	return s > 1.0f ? 1.0f : s;
}

CryptoResult RSADetector::detect(const ssa::SSAFunction& fn) const
{
	CryptoResult r;
	r.algorithm = CryptoAlgorithm::RSA;
	auto ev = analyse(fn);
	r.confidence = ev.confidence;
	if (ev.confidence >= 0.50f)
	{
		r.emittedAnnotation =
			"// Cryptographic primitive: RSA / Montgomery multiplication\n"
			"// Multi-precision limb-by-limb multiply + conditional subtract detected\n"
			"// Usage: RSA_private_decrypt(len, in, out, rsa_key, RSA_PKCS1_OAEP_PADDING);";
	}
	return r;
}

} // namespace crypto_detect
} // namespace retdec
