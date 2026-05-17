/**
 * @file include/retdec/opencl/kernel_sources.h
 * @brief OpenCL C source literals (embedded for runtime compilation).
 */

#ifndef RETDEC_OPENCL_KERNEL_SOURCES_H
#define RETDEC_OPENCL_KERNEL_SOURCES_H

namespace retdec {
namespace opencl {

const char *parallelDisasmClSource();
const char *typePropagationClSource();
const char *steensgaardClSource();
const char *semanticHashClSource();
const char *egraphSimplifyClSource();

} // namespace opencl
} // namespace retdec

#endif
