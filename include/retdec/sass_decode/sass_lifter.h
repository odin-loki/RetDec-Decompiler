/**
 * @file include/retdec/sass_decode/sass_lifter.h
 * @brief Structured CUDA-C lift from decoded SASS (subset) and optional PTX.
 *
 * This is not a Production decompile() path. Unknown opcodes become comments
 * with the raw NVIDIA-format hex words. Embedded fatbin PTX, when present,
 * is lifted with the existing PTX text lifter and labelled as PTX, not SASS.
 */

#ifndef RETDEC_SASS_DECODE_SASS_LIFTER_H
#define RETDEC_SASS_DECODE_SASS_LIFTER_H

#include "retdec/sass_decode/sass_types.h"

#include <string>

namespace retdec {
namespace sass_decode {

class SassLifter
{
public:
	std::string liftKernel(const SassKernel& kernel, const SassModule& mod) const;
	std::string liftModule(const SassModule& mod) const;
};

/// Load cubin/fatbin, decode the documented opcode subset, emit CUDA-C.
std::string decompileCudaBinary(const uint8_t* data, size_t size, const SassConfig& cfg = {});

} // namespace sass_decode
} // namespace retdec

#endif
