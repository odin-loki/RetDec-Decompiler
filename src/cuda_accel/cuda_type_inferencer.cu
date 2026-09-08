/**
 * @file src/cuda_accel/cuda_type_inferencer.cu
 * @brief CUDA type propagation — port of type_propagation.cl.
 */
#include "retdec/cuda_accel/cuda_type_inferencer.h"
#include "retdec/cuda_accel/cuda_context.h"
#include "retdec/cuda_accel/cuda_profiler.h"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <vector>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

// ═══════════════════════════════════════════════════════════════════════════
// CUDA kernels
// ═══════════════════════════════════════════════════════════════════════════
#ifdef RETDEC_HAS_CUDA

__device__ static unsigned char merge_width_d(unsigned char a, unsigned char b)
{
	if (a == 0) return b;
	if (b == 0) return a;
	return a > b ? a : b;
}
__device__ static unsigned char merge_sign_d(unsigned char a, unsigned char b)
{
	if (a == 0) return b;
	if (b == 0) return a;
	if (a == b) return a;
	return 1u;
}

__device__ static unsigned int dsu_find_d(unsigned int* parent, unsigned int x)
{
	unsigned int root = x;
	for (unsigned d = 0; d < 64u; ++d)
	{
		unsigned int p = parent[root];
		if (p == root) break;
		root = p;
	}
	unsigned int cur = x;
	for (unsigned d = 0; d < 64u; ++d)
	{
		unsigned int p = parent[cur];
		if (p == cur) break;
		parent[cur] = root;
		cur = p;
	}
	return root;
}

__global__ void retdec_type_propagation(
	unsigned int* parent,
	unsigned int* rnk,
	unsigned char* width,
	unsigned char* signedness,
	unsigned char* is_pointer,
	const unsigned int* con_offsets,
	const unsigned int* con_a,
	const unsigned int* con_b,
	unsigned int* dirty_flags,
	unsigned int* global_done,
	unsigned int num_funcs)
{
	unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
	if (gid >= num_funcs) return;

	unsigned int c_start = con_offsets[gid];
	unsigned int c_end = con_offsets[gid + 1u];
	unsigned int local_dirty = 0u;

	for (unsigned int ci = c_start; ci < c_end; ++ci)
	{
		unsigned int a = con_a[ci], b = con_b[ci];
		unsigned int ra = dsu_find_d(parent, a), rb = dsu_find_d(parent, b);
		if (ra == rb) continue;

		unsigned char mw = merge_width_d(width[ra], width[rb]);
		unsigned char ms = merge_sign_d(signedness[ra], signedness[rb]);
		unsigned char mp = is_pointer[ra] | is_pointer[rb];

		unsigned int winner, loser;
		if (rnk[ra] > rnk[rb])
		{
			winner = ra;
			loser = rb;
		}
		else if (rnk[rb] > rnk[ra])
		{
			winner = rb;
			loser = ra;
		}
		else
		{
			winner = ra;
			loser = rb;
			rnk[ra] += 1u;
		}

		parent[loser] = winner;
		width[winner] = mw;
		signedness[winner] = ms;
		is_pointer[winner] = mp;
		local_dirty = 1u;
	}

	if (local_dirty)
	{
		dirty_flags[gid] = 1u;
		*global_done = 0u;
	}
}

// The per-slot type facts are one byte each, but the narrowest CUDA atomic
// operates on a naturally aligned 32-bit word (CUDA C Programming Guide, B.14
// "Atomic Functions"). These two helpers do the read-modify-write on the aligned
// word that contains the byte and leave the other three bytes of that word
// alone. NVIDIA GPUs are little-endian, so byte idx sits at bit offset
// 8*(idx&3). The caller has to round the byte arrays up to a multiple of four,
// because the enclosing word of the last slot reaches up to three bytes past a
// totalSlots-sized allocation.
__device__ static void atomic_or_u8_d(unsigned char* base, unsigned int idx, unsigned char val)
{
	unsigned int* word = (unsigned int*)(base + (idx & ~3u));
	atomicOr(word, (unsigned int)val << (8u * (idx & 3u)));
}

// atomicMax has no masked form, so the largest-wins merge needs a CAS loop. It
// terminates: a losing iteration means another thread raised the byte, the byte
// only ever grows under max, and it saturates at 255 -- so at most 255 retries,
// and in practice zero or one. The early return also skips the write entirely
// once the byte already holds a value >= val, which is the common case.
__device__ static void atomic_max_u8_d(unsigned char* base, unsigned int idx, unsigned char val)
{
	unsigned int* word = (unsigned int*)(base + (idx & ~3u));
	unsigned int shift = 8u * (idx & 3u);
	unsigned int old = *word;
	unsigned int assumed;
	do
	{
		if ((unsigned char)((old >> shift) & 0xffu) >= val) return;
		assumed = old;
		old = atomicCAS(word, assumed, (assumed & ~(0xffu << shift)) | ((unsigned int)val << shift));
	}
	while (old != assumed);
}

__global__ void retdec_type_seed(
	unsigned char* width,
	unsigned char* signedness,
	unsigned char* is_pointer,
	const unsigned int* operand_slot,
	const unsigned char* operand_width,
	const unsigned char* operand_sign,
	const unsigned char* operand_ptr,
	unsigned int num_operands)
{
	unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
	if (gid >= num_operands) return;

	unsigned int slot = operand_slot[gid];
	unsigned char ow = operand_width[gid], os = operand_sign[gid], op = operand_ptr[gid];
	// Several operands may target the same slot, so the seeds do have to be merged
	// atomically -- but width/signedness/is_pointer are ONE BYTE per slot, and a
	// 32-bit atomic aimed at &width[slot] is misaligned for the three slots in four
	// where slot%4 != 0, which faults the kernel with cudaErrorMisalignedAddress
	// (CUDA C Programming Guide, B.14). For the remaining quarter it
	// read-modify-writes the three neighbouring slots as well, and atomicMax
	// compares the whole word: on little-endian hardware a nonzero neighbour byte
	// lands in the upper 24 bits, so the word compares greater than any 8-bit width
	// and this slot's seed is dropped without a trace. Modelled on the host with
	// the same arithmetic: a 32-bit atomicMax of 4 over slot 0, with the three
	// neighbour bytes at 0xCC, leaves byte 0 at 0 instead of 4.
	// Go through the enclosing aligned word instead, touching one byte.
	atomic_max_u8_d(width, slot, ow);
	if (os) atomic_or_u8_d(signedness, slot, os);
	if (op) atomic_or_u8_d(is_pointer, slot, op);
}

#endif // RETDEC_HAS_CUDA

// ═══════════════════════════════════════════════════════════════════════════
// Host implementation
// ═══════════════════════════════════════════════════════════════════════════

CUDATypeInferencer::CUDATypeInferencer(CUDAContext* ctx): ctx_(ctx)
{
#ifdef RETDEC_HAS_CUDA
	if (ctx_ && ctx_->isReady()) gpuReady_ = true;
#endif
}

CUDATypeInferencer::~CUDATypeInferencer() = default;

std::vector<TypeSlot> CUDATypeInferencer::infer(const std::vector<FunctionTypeData>& funcs)
{
	if (funcs.empty()) return {};
	auto t0 = std::chrono::steady_clock::now();

#ifdef RETDEC_HAS_CUDA
	if (gpuReady_ && ctx_)
	{
		// Flatten per-function data into SOA
		unsigned int F = (unsigned int)funcs.size();

		// Count total slots
		unsigned int totalSlots = 0;
		for (auto& f: funcs)
			totalSlots += f.numSlots;
		if (totalSlots == 0) return {};

		// Build constraint SOA
		std::vector<unsigned int> conOffsets(F + 1u, 0u);
		unsigned int totalCons = 0;
		for (unsigned int i = 0; i < F; ++i)
		{
			conOffsets[i] = totalCons;
			totalCons += (unsigned int)funcs[i].constraints.size();
		}
		conOffsets[F] = totalCons;

		std::vector<unsigned int> conA(totalCons), conB(totalCons);
		unsigned int ci = 0;
		for (auto& f: funcs)
			for (auto& c: f.constraints)
			{
				conA[ci] = c.slotA;
				conB[ci] = c.slotB;
				++ci;
			}

		// Operand hints
		unsigned int totalHints = 0;
		for (auto& f: funcs)
			totalHints += (unsigned int)f.operandHints.size();

		std::vector<unsigned int> hSlot(totalHints);
		std::vector<unsigned char> hWidth(totalHints), hSign(totalHints), hPtr(totalHints);
		unsigned int hi = 0;
		for (auto& f: funcs)
			for (auto& h: f.operandHints)
			{
				hSlot[hi] = (unsigned int)h.slot;
				hWidth[hi] = (unsigned char)h.widthBytes;
				hSign[hi] = (unsigned char)h.sign;
				hPtr[hi] = (unsigned char)(h.isPointer ? 1 : 0);
				++hi;
			}

		// DSU arrays
		std::vector<unsigned int> parent(totalSlots), rnk(totalSlots, 0u);
		std::vector<unsigned char> width(totalSlots, 0u), sign(totalSlots, 0u), isPtr(totalSlots, 0u);
		std::iota(parent.begin(), parent.end(), 0u);

		cudaStream_t stream = ctx_->stream();

		void *dParent = nullptr, *dRnk = nullptr, *dWidth = nullptr, *dSign = nullptr, *dIsPtr = nullptr;
		void *dConOff = nullptr, *dConA = nullptr, *dConB = nullptr;
		void *dDirty = nullptr, *dDone = nullptr;
		void *dHSlot = nullptr, *dHWidth = nullptr, *dHSign = nullptr, *dHPtr = nullptr;

		auto cleanup = [&]() {
			for (void* p:
				 {dParent,
				  dRnk,
				  dWidth,
				  dSign,
				  dIsPtr,
				  dConOff,
				  dConA,
				  dConB,
				  dDirty,
				  dDone,
				  dHSlot,
				  dHWidth,
				  dHSign,
				  dHPtr})
				if (p) cudaFree(p);
		};

		// retdec_type_seed merges each byte through the aligned 32-bit word that
		// contains it, so the three byte arrays are rounded up to a multiple of four:
		// without the padding, that word would read and write up to three bytes past
		// the allocation for the last slot. Nine bytes at most.
		const unsigned int slotBytes = (totalSlots + 3u) & ~3u;

		bool ok =
			(cudaMalloc(&dParent, totalSlots * 4) == cudaSuccess && cudaMalloc(&dRnk, totalSlots * 4) == cudaSuccess
			 && cudaMalloc(&dWidth, slotBytes) == cudaSuccess && cudaMalloc(&dSign, slotBytes) == cudaSuccess
			 && cudaMalloc(&dIsPtr, slotBytes) == cudaSuccess && cudaMalloc(&dConOff, (F + 1) * 4) == cudaSuccess
			 && (totalCons == 0 || cudaMalloc(&dConA, totalCons * 4) == cudaSuccess)
			 && (totalCons == 0 || cudaMalloc(&dConB, totalCons * 4) == cudaSuccess)
			 && cudaMalloc(&dDirty, F * 4) == cudaSuccess && cudaMalloc(&dDone, 4) == cudaSuccess
			 && (totalHints == 0 || cudaMalloc(&dHSlot, totalHints * 4) == cudaSuccess)
			 && (totalHints == 0 || cudaMalloc(&dHWidth, totalHints) == cudaSuccess)
			 && (totalHints == 0 || cudaMalloc(&dHSign, totalHints) == cudaSuccess)
			 && (totalHints == 0 || cudaMalloc(&dHPtr, totalHints) == cudaSuccess));

		if (!ok)
		{
			cleanup();
			gpuReady_ = false;
			goto cpu_fallback;
		}

		cudaMemcpyAsync(dParent, parent.data(), totalSlots * 4, cudaMemcpyHostToDevice, stream);
		cudaMemsetAsync(dRnk, 0, totalSlots * 4, stream);
		// The padding is zeroed with the rest, so the CAS loop never writes back
		// uninitialised device memory. The three device-to-host copies further down
		// still take totalSlots bytes: the padding is device-side only, and the host
		// vectors are sized totalSlots.
		cudaMemsetAsync(dWidth, 0, slotBytes, stream);
		cudaMemsetAsync(dSign, 0, slotBytes, stream);
		cudaMemsetAsync(dIsPtr, 0, slotBytes, stream);
		cudaMemcpyAsync(dConOff, conOffsets.data(), (F + 1) * 4, cudaMemcpyHostToDevice, stream);
		if (totalCons > 0)
		{
			cudaMemcpyAsync(dConA, conA.data(), totalCons * 4, cudaMemcpyHostToDevice, stream);
			cudaMemcpyAsync(dConB, conB.data(), totalCons * 4, cudaMemcpyHostToDevice, stream);
		}
		if (totalHints > 0)
		{
			cudaMemcpyAsync(dHSlot, hSlot.data(), totalHints * 4, cudaMemcpyHostToDevice, stream);
			cudaMemcpyAsync(dHWidth, hWidth.data(), totalHints, cudaMemcpyHostToDevice, stream);
			cudaMemcpyAsync(dHSign, hSign.data(), totalHints, cudaMemcpyHostToDevice, stream);
			cudaMemcpyAsync(dHPtr, hPtr.data(), totalHints, cudaMemcpyHostToDevice, stream);

			unsigned int block = 256, gridH = (totalHints + block - 1) / block;
			retdec_type_seed<<<gridH, block, 0, stream>>>(
				(unsigned char*)dWidth,
				(unsigned char*)dSign,
				(unsigned char*)dIsPtr,
				(const unsigned int*)dHSlot,
				(const unsigned char*)dHWidth,
				(const unsigned char*)dHSign,
				(const unsigned char*)dHPtr,
				totalHints);
		}

		{
			unsigned int block = 256, gridF = (F + block - 1) / block;
			for (lastIter_ = 0; lastIter_ < kMaxIterations; ++lastIter_)
			{
				unsigned int one = 1;
				cudaMemcpyAsync(dDone, &one, 4, cudaMemcpyHostToDevice, stream);
				cudaMemsetAsync(dDirty, 0, F * 4, stream);
				retdec_type_propagation<<<gridF, block, 0, stream>>>(
					(unsigned int*)dParent,
					(unsigned int*)dRnk,
					(unsigned char*)dWidth,
					(unsigned char*)dSign,
					(unsigned char*)dIsPtr,
					(const unsigned int*)dConOff,
					(const unsigned int*)dConA,
					(const unsigned int*)dConB,
					(unsigned int*)dDirty,
					(unsigned int*)dDone,
					F);
				unsigned int done = 0;
				cudaMemcpyAsync(&done, dDone, 4, cudaMemcpyDeviceToHost, stream);
				cudaStreamSynchronize(stream);
				if (done) break;
			}
		}

		cudaMemcpyAsync(parent.data(), dParent, totalSlots * 4, cudaMemcpyDeviceToHost, stream);
		cudaMemcpyAsync(width.data(), dWidth, totalSlots, cudaMemcpyDeviceToHost, stream);
		cudaMemcpyAsync(sign.data(), dSign, totalSlots, cudaMemcpyDeviceToHost, stream);
		cudaMemcpyAsync(isPtr.data(), dIsPtr, totalSlots, cudaMemcpyDeviceToHost, stream);
		cudaStreamSynchronize(stream);
		cleanup();

		std::function<unsigned int(unsigned int)> find = [&](unsigned int x) -> unsigned int {
			if (parent[x] == x) return x;
			parent[x] = find(parent[x]);
			return parent[x];
		};

		std::vector<TypeSlot> result(totalSlots);
		for (unsigned int i = 0; i < totalSlots; ++i)
		{
			unsigned int r = find(i);
			result[i].widthBytes = width[r];
			result[i].sign = static_cast<TypeSign>(sign[r]);
			result[i].isPointer = isPtr[r] != 0;
		}

		auto t1 = std::chrono::steady_clock::now();
		CUDAProfiler::instance().record(
			"type_propagation", (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		return result;
	}
cpu_fallback:
#endif

	return inferCPU(funcs);
}

std::vector<TypeSlot> CUDATypeInferencer::inferCPU(const std::vector<FunctionTypeData>& funcs)
{
	unsigned int totalSlots = 0;
	for (auto& f: funcs)
		totalSlots += f.numSlots;
	if (totalSlots == 0) return {};

	std::vector<unsigned int> parent(totalSlots), rnk(totalSlots, 0u);
	std::vector<unsigned char> width(totalSlots, 0u), sign(totalSlots, 0u), isPtr(totalSlots, 0u);
	std::iota(parent.begin(), parent.end(), 0u);

	// Seed from operand hints
	unsigned int slotBase = 0;
	for (auto& f: funcs)
	{
		for (auto& h: f.operandHints)
		{
			unsigned int s = slotBase + h.slot;
			if (s >= totalSlots) continue;
			width[s] = std::max(width[s], (unsigned char)h.widthBytes);
			if ((unsigned char)h.sign) sign[s] = (unsigned char)h.sign;
			if (h.isPointer) isPtr[s] = 1;
		}
		slotBase += f.numSlots;
	}

	std::function<unsigned int(unsigned int)> find = [&](unsigned int x) -> unsigned int {
		if (parent[x] == x) return x;
		parent[x] = find(parent[x]);
		return parent[x];
	};
	auto mergeW = [](unsigned char a, unsigned char b) -> unsigned char {
		if (!a) return b;
		if (!b) return a;
		return a > b ? a : b;
	};
	auto mergeS = [](unsigned char a, unsigned char b) -> unsigned char {
		if (!a) return b;
		if (!b) return a;
		if (a == b) return a;
		return 1u;
	};

	for (lastIter_ = 0; lastIter_ < kMaxIterations; ++lastIter_)
	{
		bool changed = false;
		slotBase = 0;
		for (auto& f: funcs)
		{
			for (auto& c: f.constraints)
			{
				unsigned int a = slotBase + c.slotA, b = slotBase + c.slotB;
				if (a >= totalSlots || b >= totalSlots) continue;
				unsigned int ra = find(a), rb = find(b);
				if (ra == rb) continue;
				unsigned char mw = mergeW(width[ra], width[rb]);
				unsigned char ms = mergeS(sign[ra], sign[rb]);
				unsigned char mp = isPtr[ra] | isPtr[rb];
				if (rnk[ra] >= rnk[rb])
				{
					parent[rb] = ra;
					if (rnk[ra] == rnk[rb]) ++rnk[ra];
				}
				else
				{
					parent[ra] = rb;
				}
				unsigned int winner = find(a);
				width[winner] = mw;
				sign[winner] = ms;
				isPtr[winner] = mp;
				changed = true;
			}
			slotBase += f.numSlots;
		}
		if (!changed) break;
	}

	std::vector<TypeSlot> result(totalSlots);
	for (unsigned int i = 0; i < totalSlots; ++i)
	{
		unsigned int r = find(i);
		result[i].widthBytes = width[r];
		result[i].sign = static_cast<TypeSign>(sign[r]);
		result[i].isPointer = isPtr[r] != 0;
	}
	return result;
}

} // namespace retdec::cuda_accel
