/**
 * @file include/retdec/sass_decode/cubin_loader.h
 * @brief cubin (ELF EM_CUDA) and fatbin container loader.
 *
 * Does not call or bundle nvdisasm. Compressed fatbin members are reported,
 * not inflated (NVIDIA compression is not a public ISA).
 */

#ifndef RETDEC_SASS_DECODE_CUBIN_LOADER_H
#define RETDEC_SASS_DECODE_CUBIN_LOADER_H

#include "retdec/sass_decode/sass_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace sass_decode {

bool looksLikeCubin(const uint8_t* data, size_t size);
bool looksLikeFatbin(const uint8_t* data, size_t size);

/**
 * Parse a standalone cubin ELF (e_machine == EM_CUDA / 190).
 * Fills kernels from SHF_EXECINSTR / `.text*` sections. Does not decode SASS.
 */
SassModule loadCubin(const uint8_t* data, size_t size, const SassConfig& cfg = {});

/**
 * Parse a NVIDIA fatbin (`FATBIN_MAGIC`). Locates uncompressed ELF cubin
 * and PTX text in the payload. Entry records after the public header are
 * NVIDIA-internal (`fatbinary.h` says so); this loader scans rather than
 * inventing that layout.
 */
SassModule loadFatbin(const uint8_t* data, size_t size, const SassConfig& cfg = {});

/// Dispatch: fatbin, else cubin, else error.
SassModule loadCudaBinary(const uint8_t* data, size_t size, const SassConfig& cfg = {});

SassModule loadCudaBinary(const std::vector<uint8_t>& bytes, const SassConfig& cfg = {});

} // namespace sass_decode
} // namespace retdec

#endif
