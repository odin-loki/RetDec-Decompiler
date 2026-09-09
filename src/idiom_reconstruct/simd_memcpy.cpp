/**
 * @file src/idiom_reconstruct/simd_memcpy.cpp
 * @brief SIMD-vectorised memset / memcpy / memmove recognition.
 *
 * ## Background
 *
 * For small or medium-sized memory operations with a compile-time-known count,
 * compilers inline and vectorise:
 *   - `memset(dst, c, N)` → repeated SIMD stores of a broadcast value
 *   - `memcpy(dst, src, N)` → SIMD load-store pairs
 *
 * GCC/Clang with -O2 and MSVC /O2 typically unroll the vector loop entirely
 * for N ≤ 256 bytes.  For larger N they emit a counted loop.  We handle both.
 *
 * ## Recognition strategy
 *
 * This matcher operates on a LINEAR window (one basic block or one loop body).
 * It classifies the window by scanning for:
 *
 * ### memset detection
 *   1. One `VecSet` instruction broadcasting a scalar (or zero/immediate) to a
 *      SIMD register → the fill value.
 *   2. One or more `VecStore` instructions all storing the same SIMD register
 *      to consecutive addresses (dst, dst+vecWidth, dst+2*vecWidth, …).
 *   3. Optional scalar epilogue: `Store` instructions for the remaining bytes.
 *   4. A `Mov` loading a count into a register (for the counted-loop variant).
 *
 * ### memcpy detection
 *   1. One or more `VecLoad` + `VecStore` pairs where:
 *      - Load source addresses are consecutive (src, src+vecWidth, …)
 *      - Store destination addresses are consecutive (dst, dst+vecWidth, …)
 *      - Load and store widths match.
 *   2. No aliasing between src and dst ranges (we assume no_alias since the
 *      compiler emitted memcpy semantics).
 *
 * ### memmove detection
 *   Same as memcpy but with a direction check (backward copy loop → memmove).
 *   We check for decreasing address sequence (src+N-vecW, …, src downward).
 *
 * ## Heuristics for "is this a memory function?"
 *
 * We require:
 *   - At least 2 vector stores (otherwise it might just be a normal store).
 *   - All stores use the same SIMD register (memset) or paired load registers
 *     (memcpy).
 *   - The stride between consecutive stores equals the vector register width.
 *
 * ## IdiomInstr fields used
 *
 *   VecLoad/VecStore: dst=vecReg, src0=baseReg, src1=offsetImm (if any)
 *   VecSet: dst=vecReg, src0=scalarReg or src0=Imm(0) for vxorps/vpxor zeroing
 *   vecWidth: width in bytes of the vector register (16=SSE, 32=AVX, 64=AVX-512)
 *
 * ## Output
 *
 *   ReplacementKind::Memset with dstReg, fillValue (or fillReg), countImm or countReg.
 *   ReplacementKind::Memcpy with dstReg, srcReg, countImm or countReg.
 *   ReplacementKind::Memmove for backward-copy variant.
 *
 * ## Limitations
 *
 * This matcher does NOT handle cross-block loop recognition — that requires
 * CFG analysis (which is done in the CFG structuring stage, Task 28).
 * Within a single basic block (unrolled inlined copy), it works fully.
 */

#include <memory>
#include "retdec/idiom_reconstruct/idiom_reconstruct.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace idiom_reconstruct {

namespace {

struct MemAccess
{
	uint32_t baseReg;
	int64_t offsetBytes; ///< immediate offset from base (if any)
	uint32_t vecReg;     ///< SIMD register used
	uint32_t vecWidth;   ///< width in bytes
	bool isLoad;
	uint64_t vma;
};

class SimdMemMatcher : public IIdiomMatcher {
public:
	const char* name() const noexcept override
	{
		return "SimdMemset_Memcpy";
	}
	std::size_t minWindowSize() const noexcept override
	{
		return 2;
	}

	std::optional<ReplacementNode> match(const InstrWindow& W, std::size_t off, CompilerProfile /*prof*/) const override
	{
		const std::size_t n = W.size();

		// ── Step 1: Scan for vector accesses starting at `off` ────────────────
		std::vector<MemAccess> accesses;
		uint32_t fillVecReg = UINT32_MAX;
		int64_t fillValue = 0;
		bool hasFill = false;
		std::size_t lastIdx = off;
		int64_t epilogueBytes = 0;

		for (std::size_t i = off; i < n; ++i)
		{
			const IdiomInstr& ins = W[i];

			if (ins.op == IdiomOp::VecSet)
			{
				// Broadcast scalar (or zero) into SIMD register
				fillVecReg = ins.dst.reg;
				if (ins.src0.kind == OperandKind::Imm)
					fillValue = ins.src0.imm;
				else if (ins.src0.kind == OperandKind::Reg && ins.src0.reg == ins.src1.reg && ins.op == IdiomOp::Xor)
					fillValue = 0; // vpxor self = zero
				else
					fillValue = 0;
				hasFill = true;
				lastIdx = i;
				continue;
			}

			if (ins.op == IdiomOp::VecStore || ins.op == IdiomOp::VecLoad)
			{
				MemAccess acc;
				acc.isLoad = (ins.op == IdiomOp::VecLoad);
				acc.vecWidth = ins.vecWidth > 0 ? ins.vecWidth : ins.dst.width / 8;
				acc.vma = ins.vma;
				acc.offsetBytes = ins.src1.kind == OperandKind::Imm ? ins.src1.imm : 0;
				if (ins.op == IdiomOp::VecStore)
				{
					// Tests encode store as dst=mem base, src0=vecReg, src1=offset.
					acc.vecReg = ins.src0.reg;
					acc.baseReg = ins.dst.reg;
				}
				else
				{
					acc.vecReg = ins.dst.reg;
					acc.baseReg = ins.src0.reg;
				}
				accesses.push_back(acc);
				lastIdx = i;
				continue;
			}

			// Scalar epilogue stores (Store) after the SIMD block. These are
			// absorbed into the replacement -- lastIdx moves past them, so
			// instrCount counts them -- and their bytes were not added to the
			// count, so the emitted memcpy covered fewer bytes than the
			// instructions it replaced.
			if (ins.op == IdiomOp::Store && !accesses.empty())
			{
				uint32_t w = ins.src0.width ? ins.src0.width / 8u : 0u;
				if (w == 0) w = ins.dst.width ? ins.dst.width / 8u : 0u;

				// ...but only when it continues the destination run. The only
				// condition used to be "some vector access has been seen", so
				// a scalar store to an unrelated base was absorbed: its bytes
				// went into the count and its instruction into instrCount, and
				// the caller skips instrCount instructions -- so the emitted
				// memcpy claimed bytes it does not write and swallowed a store
				// that writes somewhere else.
				uint32_t dstBase = 0;
				int64_t dstEnd = 0;
				bool haveDst = false;
				for (const auto& a: accesses)
				{
					if (a.isLoad) continue;
					if (!haveDst)
					{
						dstBase = a.baseReg;
						dstEnd = a.offsetBytes + (int64_t)a.vecWidth;
						haveDst = true;
					}
					else
					{
						dstEnd = std::max(dstEnd, a.offsetBytes + (int64_t)a.vecWidth);
					}
				}
				const int64_t here = ins.src1.kind == OperandKind::Imm ? ins.src1.imm : 0;
				if (!haveDst || ins.dst.reg != dstBase || here != dstEnd + epilogueBytes)
				{
					break;
				}

				epilogueBytes += w;
				lastIdx = i;
				continue;
			}

			// Any non-memory non-setup instruction breaks the sequence.
			//
			// This guard used to be `if (!accesses.empty())`, so before the
			// first vector access anything at all was skipped over: the match
			// was not anchored at `off`. The matcher would walk forward past
			// unrelated instructions until it found a SIMD block somewhere in
			// the window and then report a replacement spanning from `off`,
			// swallowing everything in between -- including idioms another
			// matcher would have recovered. The sequence has to start where
			// the caller says it starts.
			if (ins.op != IdiomOp::Add && ins.op != IdiomOp::Lea && ins.op != IdiomOp::Mov && ins.op != IdiomOp::Sub)
				break;
		}

		if (accesses.size() < 2) return std::nullopt;

		// ── Step 2: Classify as memset or memcpy ──────────────────────────────

		// Separate loads and stores
		std::vector<MemAccess> loads, stores;
		for (auto& a: accesses)
		{
			if (a.isLoad)
				loads.push_back(a);
			else
				stores.push_back(a);
		}

		if (stores.empty()) return std::nullopt;

		// Check that all stores use the same base register
		uint32_t dstBase = stores[0].baseReg;
		bool allSameDst =
			std::all_of(stores.begin(), stores.end(), [dstBase](const MemAccess& a) { return a.baseReg == dstBase; });

		if (!allSameDst) return std::nullopt;

		// `loads` and `stores` are still in program order here, which is what
		// says which load feeds which store. The stride check needs them by
		// offset, so it works on a copy -- sorting the real ones (and sorting
		// `loads` separately, further down) threw the pairing away, and the
		// backward-copy test then compared two ascending lists, which agree
		// only for a palindrome: the memmove branch could not be reached.
		{
			std::vector<MemAccess> byOffset = stores;
			std::sort(byOffset.begin(), byOffset.end(), [](const MemAccess& a, const MemAccess& b) {
				return a.offsetBytes < b.offsetBytes;
			});
			uint32_t vecW = byOffset[0].vecWidth ? byOffset[0].vecWidth : 16;
			bool strideOk = true;
			for (std::size_t i = 1; i < byOffset.size(); ++i)
			{
				int64_t expectedOff = byOffset[i - 1].offsetBytes + vecW;
				if (byOffset[i].offsetBytes != expectedOff)
				{
					strideOk = false;
					break;
				}
			}
			if (!strideOk) return std::nullopt;
		}

		// Total bytes covered, including any scalar epilogue the span absorbed.
		uint32_t vecW = stores[0].vecWidth ? stores[0].vecWidth : 16;
		int64_t count = (int64_t)(stores.size() * vecW) + epilogueBytes;

		// ── memset: no loads, fill value from VecSet ──────────────────────────
		if (loads.empty())
		{
			// Every store has to write the same register, and when a broadcast
			// set one up, that one. This used to short-circuit on `hasFill`,
			// which any VecSet anywhere in the span sets -- so the check was
			// skipped exactly when there was a fill register to check against,
			// and stores of an unrelated register were reported as a memset of
			// the broadcast value.
			const uint32_t vr = hasFill ? fillVecReg : stores[0].vecReg;
			const bool isFill =
				std::all_of(stores.begin(), stores.end(), [vr](const MemAccess& a) { return a.vecReg == vr; });
			if (!isFill) return std::nullopt;

			ReplacementNode r;
			r.kind = ReplacementKind::Memset;
			r.dstReg = dstBase;
			r.fillValue = fillValue;
			r.countImm = count;
			r.firstVma = W[off].vma;
			r.lastVma = W[lastIdx].vma;
			r.instrCount = lastIdx - off + 1;
			return r;
		}

		// ── memcpy: loads from one base, stores to another ────────────────────
		uint32_t srcBase = loads[0].baseReg;
		bool allSameSrc =
			std::all_of(loads.begin(), loads.end(), [srcBase](const MemAccess& a) { return a.baseReg == srcBase; });
		if (!allSameSrc) return std::nullopt;
		if (srcBase == dstBase) return std::nullopt; // trivially aliased

		// Check load offsets match store offsets (same count in order)
		if (loads.size() != stores.size()) return std::nullopt;

		// A store copies what a load put in its register, so that is the pairing:
		// the register, not the position in the list. Pairing by position never
		// looked at vecReg at all, so a store of a register nothing loaded read
		// as a copy of bytes it does not copy.
		std::unordered_map<uint32_t, int64_t> srcOffOfReg;
		bool offsetsMatch = true;
		std::vector<int64_t> dstSeq;
		for (const auto& a: accesses)
		{
			if (a.isLoad)
			{
				srcOffOfReg[a.vecReg] = a.offsetBytes;
				continue;
			}
			auto it = srcOffOfReg.find(a.vecReg);
			if (it == srcOffOfReg.end() || it->second != a.offsetBytes)
			{
				offsetsMatch = false;
				break;
			}
			dstSeq.push_back(a.offsetBytes);
		}

		if (!offsetsMatch) return std::nullopt;

		// Descending destination offsets are the overlap-safe direction, which
		// is what distinguishes memmove from memcpy.
		bool backward = dstSeq.size() > 1 && dstSeq.front() > dstSeq.back();

		ReplacementNode r;
		r.kind = backward ? ReplacementKind::Memmove : ReplacementKind::Memcpy;
		r.dstReg = dstBase;
		r.srcReg = srcBase;
		r.countImm = count;
		r.firstVma = W[off].vma;
		r.lastVma = W[lastIdx].vma;
		r.instrCount = lastIdx - off + 1;
		return r;
	}
};

} // namespace

std::unique_ptr<IIdiomMatcher> makeSimdMemMatcher()
{
	return std::make_unique<SimdMemMatcher>();
}

} // namespace idiom_reconstruct
} // namespace retdec
