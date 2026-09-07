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
