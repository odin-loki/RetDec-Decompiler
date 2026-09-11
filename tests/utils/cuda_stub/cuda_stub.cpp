/**
 * @file tests/utils/cuda_stub/cuda_stub.cpp
 * @brief Definitions for the launch variables the stub declares.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "cuda_runtime.h"

thread_local dim3 threadIdx;
thread_local dim3 blockIdx;
dim3 blockDim;
dim3 gridDim;

namespace retdec {
namespace tests {
namespace cudastub {

namespace {

/// Not thread_local: ensureGpu() runs on whichever thread constructs the
/// scanner, and the kernel lanes are threads of their own.
bool g_devicePresented = false;

} // anonymous namespace

void presentOneDevice(bool present)
{
	g_devicePresented = present;
}

bool deviceIsPresented()
{
	return g_devicePresented;
}

} // namespace cudastub
} // namespace tests
} // namespace retdec
