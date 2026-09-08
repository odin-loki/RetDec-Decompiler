/**
 * @file tests/utils/cuda_stub/cuda_runtime.h
 * @brief Enough of the CUDA runtime to compile and RUN src/utils/gpu_scanner.cu
 *        on the host.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Why this exists
 * ---------------
 * src/utils/CMakeLists.txt builds retdec-gpu-scanner from gpu_scanner.cu when
 * CUDA is found and from gpu_scanner_cpu.cpp otherwise. Only the second is ever
 * compiled in this tree, and gpu_scanner_cpu.cpp includes the first with
 * RETDEC_GPU_SCANNER_HOST_ONLY defined -- so the ~660 lines behind that guard,
 * which is all of the device code and all of the CUDA-side plumbing, were
 * compiled by nothing at all. An adversarial verify pass demonstrated the
 * consequence: it reverted four fixes inside that region and the suite stayed
 * green at 13/13.
 *
 * These stubs let the region be compiled by an ordinary C++17 compiler, and the
 * kernel actually executed, so a test can observe what it computes.
 *
 * How faithful this is, and where it is not
 * ----------------------------------------
 * Faithful:
 *   - one block at a time, its threads running concurrently as std::thread;
 *   - __syncthreads() is a real barrier across those threads, so a kernel that
 *     depends on the barrier gives the same answer it gives on a device;
 *   - __shared__ is one object per block, shared by its threads and rebuilt for
 *     the next block -- correct because blocks are serialised here;
 *   - cudaMalloc/cudaMemcpy/cudaFree over host memory, so a read past an
 *     allocation is a real out-of-bounds access that ASan reports.
 *
 * Not faithful, and not pretended to be:
 *   - no warp semantics: no implicit lock-step, no shuffle, no divergence
 *     modelling. A kernel relying on warp-synchronous behaviour would be wrong
 *     here and right on a device. batchMatchKernel does not: it synchronises
 *     with __syncthreads() explicitly, which is why it can be run this way.
 *   - no memory-coalescing, occupancy or timing behaviour. This is for
 *     correctness, not performance.
 *   - blocks do not run concurrently, so a kernel with inter-block races would
 *     pass here and fail on a device.
 */

#ifndef RETDEC_TESTS_CUDA_STUB_CUDA_RUNTIME_H
#define RETDEC_TESTS_CUDA_STUB_CUDA_RUNTIME_H

#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ─── attributes ──────────────────────────────────────────────────────────────
//
// The qualifiers carry no meaning on the host. __shared__ becomes `static`,
// which is one object per block: correct only because blocks are serialised
// below, and the reason they are.
#define __global__
#define __device__
#define __host__
#define __restrict__
#define __shared__ static
#define __forceinline__ inline

// ─── the launch geometry, per thread ─────────────────────────────────────────

struct dim3
{
	unsigned x = 1, y = 1, z = 1;
	dim3() = default;
	dim3(unsigned x_, unsigned y_ = 1, unsigned z_ = 1): x(x_), y(y_), z(z_) {}
};

namespace retdec {
namespace tests {
namespace cudastub {

/// One barrier per block, counting the block's threads.
///
/// Written out rather than using std::barrier so this stays C++17, which is
/// what the rest of the tree builds at.
class BlockBarrier {
public:
	explicit BlockBarrier(unsigned threads): threads_(threads) {}

	void arriveAndWait()
	{
		std::unique_lock<std::mutex> lock(m_);
		const unsigned myGeneration = generation_;
		if (++waiting_ == threads_)
		{
			waiting_ = 0;
			++generation_;
			cv_.notify_all();
			return;
		}
		cv_.wait(lock, [&] { return generation_ != myGeneration; });
	}

private:
	std::mutex m_;
	std::condition_variable cv_;
	unsigned threads_;
	unsigned waiting_ = 0;
	unsigned generation_ = 0;
};

/// The block currently running. One at a time, so a plain pointer is enough.
inline BlockBarrier*& currentBarrier()
{
	static BlockBarrier* b = nullptr;
	return b;
}

} // namespace cudastub
} // namespace tests
} // namespace retdec

// threadIdx/blockIdx are per-thread; blockDim/gridDim are per-launch.
extern thread_local dim3 threadIdx;
extern thread_local dim3 blockIdx;
extern dim3 blockDim;
extern dim3 gridDim;

inline void __syncthreads()
{
	auto* b = ::retdec::tests::cudastub::currentBarrier();
	if (b != nullptr)
	{
		b->arriveAndWait();
	}
}

// ─── the runtime API this file uses ──────────────────────────────────────────

using cudaError_t = int;
enum : cudaError_t
{
	cudaSuccess = 0,
	cudaErrorMemoryAllocation = 2,
	/// What a 32-bit atomic on a byte address actually returns on a device, and
	/// therefore the error src/cuda_accel/cuda_type_inferencer.cu had to stop
	/// producing.
	cudaErrorMisalignedAddress = 716
};

enum cudaMemcpyKind
{
	cudaMemcpyHostToDevice,
	cudaMemcpyDeviceToHost,
	cudaMemcpyDeviceToDevice,
	cudaMemcpyHostToHost,
};

inline const char* cudaGetErrorString(cudaError_t e)
{
	return e == cudaSuccess ? "no error" : "stubbed CUDA error";
}

/// Host memory, so an over-read past the allocation is a real one and ASan
/// reports it. That is most of the point of running the kernel here.
template <typename T>
inline cudaError_t cudaMalloc(T** p, std::size_t bytes)
{
	// A zero-byte allocation still yields a distinct pointer on the device;
	// malloc(0) may return nullptr, which the caller would read as failure.
	void* mem = std::malloc(bytes == 0 ? 1 : bytes);
	if (mem == nullptr)
	{
		*p = nullptr;
		return cudaErrorMemoryAllocation;
	}
	*p = static_cast<T*>(mem);
	return cudaSuccess;
}

inline cudaError_t cudaFree(void* p)
{
	std::free(p);
	return cudaSuccess;
}

inline cudaError_t cudaMemcpy(void* dst, const void* src, std::size_t bytes, cudaMemcpyKind)
{
	if (bytes != 0)
	{
		std::memcpy(dst, src, bytes);
	}
	return cudaSuccess;
}

inline cudaError_t cudaMemset(void* p, int v, std::size_t bytes)
{
	if (bytes != 0)
	{
		std::memset(p, v, bytes);
	}
	return cudaSuccess;
}

/// A device-side atomic add. Mutex-guarded rather than std::atomic, because the
/// kernel takes the address of an ordinary uint32_t; correctness matters here
/// and speed does not.
template <typename T>
inline T atomicAdd(T* address, T value)
{
	static std::mutex m;
	std::lock_guard<std::mutex> lock(m);
	const T old = *address;
	*address = old + value;
	return old;
}

/// The remaining device-side atomics src/cuda_accel/ uses, on the same footing
/// as atomicAdd above: mutex-guarded because the kernels take the address of an
/// ordinary object, and correctness is the only thing being modelled.
template <typename T>
inline T atomicOr(T* address, T value)
{
	static std::mutex m;
	std::lock_guard<std::mutex> lock(m);
	const T old = *address;
	*address = old | value;
	return old;
}

template <typename T>
inline T atomicMax(T* address, T value)
{
	static std::mutex m;
	std::lock_guard<std::mutex> lock(m);
	const T old = *address;
	if (value > old) *address = value;
	return old;
}

/// Returns the value that was there, whether or not the swap happened, which is
/// what a CAS loop tests against.
template <typename T>
inline T atomicCAS(T* address, T expected, T desired)
{
	static std::mutex m;
	std::lock_guard<std::mutex> lock(m);
	const T old = *address;
	if (old == expected) *address = desired;
	return old;
}

// ─── streams ─────────────────────────────────────────────────────────────────
//
// src/cuda_accel/ drives its work through a stream. Nothing here is
// asynchronous -- the "async" calls do the work immediately and the
// synchronisation is a no-op -- which is a faithful model of a stream with one
// consumer and no overlap, and is what these kernels use it as.
using cudaStream_t = struct CUstream_st*;

inline cudaError_t
cudaMemcpyAsync(void* dst, const void* src, std::size_t bytes, cudaMemcpyKind kind, cudaStream_t = nullptr)
{
	return cudaMemcpy(dst, src, bytes, kind);
}

inline cudaError_t cudaMemsetAsync(void* p, int v, std::size_t bytes, cudaStream_t = nullptr)
{
	return cudaMemset(p, v, bytes);
}

inline cudaError_t cudaStreamSynchronize(cudaStream_t)
{
	return cudaSuccess;
}

struct cudaDeviceProp
{
	char name[256] = "stub (no CUDA device)";
	int major = 0;
	int minor = 0;
	std::size_t totalGlobalMem = 0;
	int multiProcessorCount = 0;
};

/// No device: ensureGpu() must take its "CUDA absent" path, so the CPU
/// fallback is what a test observes unless it drives the kernel itself.
inline cudaError_t cudaSetDevice(int)
{
	return cudaErrorMemoryAllocation;
}
inline cudaError_t cudaGetDeviceProperties(cudaDeviceProp*, int)
{
	return cudaErrorMemoryAllocation;
}

inline cudaError_t cudaGetLastError()
{
	return cudaSuccess;
}
inline cudaError_t cudaDeviceSynchronize()
{
	return cudaSuccess;
}
inline cudaError_t cudaGetDeviceCount(int* n)
{
	*n = 0;
	return cudaSuccess;
}

// ─── the launch ──────────────────────────────────────────────────────────────

namespace retdec {
namespace tests {
namespace cudastub {

/// Run one block: `block` threads, concurrently, sharing one barrier.
///
/// Real threads rather than a serial loop, because __syncthreads() has to mean
/// something. A kernel that fills a __shared__ array before the barrier and
/// reads its neighbours' entries after it -- which batchMatchKernel does twice,
/// for the pattern cache and for the reduction -- gives the wrong answer under
/// any simulation that runs threads to completion one at a time.
template <typename Kernel, typename... Args>
void runBlock(Kernel kernel, unsigned blockIndex, unsigned threads, Args... args)
{
	BlockBarrier barrier(threads);
	currentBarrier() = &barrier;

	std::vector<std::thread> lanes;
	lanes.reserve(threads);
	for (unsigned t = 0; t < threads; ++t)
	{
		lanes.emplace_back([&, t] {
			threadIdx = dim3(t);
			blockIdx = dim3(blockIndex);
			kernel(args...);
		});
	}
	for (auto& lane: lanes)
	{
		lane.join();
	}

	currentBarrier() = nullptr;
}

/// `RETDEC_GPU_LAUNCH(k, grid, block)(args...)` -- the call this returns takes
/// the kernel arguments, so the launch reads the same way it does under nvcc.
///
/// Blocks run one after another. On a device they do not, so a kernel with a
/// race between blocks would pass here; batchMatchKernel gives each block its
/// own pattern and its own results slot, so it has none.
template <typename Kernel>
auto launch(Kernel kernel, unsigned grid, unsigned block)
{
	return [kernel, grid, block](auto&&... args) {
		blockDim = dim3(block);
		gridDim = dim3(grid);
		for (unsigned b = 0; b < grid; ++b)
		{
			runBlock(kernel, b, block, args...);
		}
	};
}

} // namespace cudastub
} // namespace tests
} // namespace retdec

#endif // RETDEC_TESTS_CUDA_STUB_CUDA_RUNTIME_H
