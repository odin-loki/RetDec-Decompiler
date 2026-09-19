/**
 * @file include/retdec/sass_decode/sass_decoder.h
 * @brief SASS opcode decoder for a documented SM_70/SM_80 subset.
 *
 * Opcode bytes are matched against instruction words NVIDIA printed in
 * CUDA Binary Utilities (Volta+ 16-byte listings and Fermi 8-byte EXIT).
 * Unknown words stay `SassOpcode::Unknown`; they are never guessed.
 */

#ifndef RETDEC_SASS_DECODE_SASS_DECODER_H
#define RETDEC_SASS_DECODE_SASS_DECODER_H

#include "retdec/sass_decode/sass_types.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace sass_decode {

class SassDecoder
{
public:
	explicit SassDecoder(uint32_t sm, uint32_t pointerBits = 64);

	uint32_t sm() const { return sm_; }
	uint32_t pointerBits() const { return pointerBits_; }
	uint32_t instrSize() const { return sassInstrSize(sm_); }

	SassInstr decodeWord(uint64_t word0, uint64_t word1, uint64_t pc) const;
	SassInstr decodeBytes(const uint8_t* bytes, size_t n, uint64_t pc) const;

	/// Walk `bytes` in instruction-sized steps. Trailing partial words are dropped.
	std::vector<SassInstr> decodeStream(const uint8_t* bytes, size_t n) const;
	std::vector<SassInstr> decodeStream(const std::vector<uint8_t>& bytes) const;

	void decodeKernel(SassKernel& kernel) const;
	void decodeModule(SassModule& mod) const;

private:
	uint32_t sm_ = 80;
	uint32_t pointerBits_ = 64;
};

} // namespace sass_decode
} // namespace retdec

#endif
