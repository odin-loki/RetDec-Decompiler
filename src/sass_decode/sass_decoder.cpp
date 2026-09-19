/**
 * @file src/sass_decode/sass_decoder.cpp
 * @brief Decode a documented SASS subset from NVIDIA-printed encodings.
 *
 * Volta+ (SM_70 / SM_80 and later listings) use 16-byte instructions. The
 * low 8 bits of the first little-endian 64-bit word are the opcode in every
 * NVIDIA CUDA Binary Utilities sample consulted:
 *
 *   EXIT  0x4d   /* 0x000000000000794d  0x000fea0003800000 *\/
 *   NOP   0x18   /* 0x0000000000007918  0x000fc00000000000 *\/
 *   BRA   0x47   /* 0xfffffff000007947  0x000fc0000383ffff *\/
 *   IMAD  0x24   /* 0x00000a00ff017624  (12.8.2) / 0x...7c24 (13.3) *\/
 *   MOV   0x02   /* 0x0000590000037a02 *\/
 *   LDG   0x81   /* 0x0000000002027381 / 0x...7981 *\/
 *   STG   0x86   /* 0x0000000906007386 / 0x...7986 *\/
 *   IADD3 0x10   /* 0x0000000502097210 *\/
 *   FADD  0x21   /* 0x0000000502097221 *\/
 *   S2R   0x19   /* 0x0000000000097919 *\/
 *   LDC   0x82   /* 0x0000df00ff017b82 *\/
 *
 * Register fields in those same listings: dest [16:23], src0 [24:31],
 * src1 [32:39], pred [12:15] (7 = PT). IADD3/IMAD src2 is word1[7:0].
 *
 * Pre-Volta 8-byte EXIT from CUDA Binary Utilities 9.1:
 *   EXIT  /* 0x8000000000001de7 *\/  — matched on low 16 bits 0x1de7 only.
 */

#include "retdec/sass_decode/sass_decoder.h"

namespace retdec {
namespace sass_decode {
namespace {

uint64_t readU64le(const uint8_t* p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i)
	{
		v |= uint64_t(p[i]) << (8 * i);
	}
	return v;
}

SassOpcode opcodeVolta(uint8_t op)
{
	switch (op)
	{
	case 0x18: return SassOpcode::Nop;
	case 0x4d: return SassOpcode::Exit;
	case 0x47: return SassOpcode::Bra;
	case 0x24: return SassOpcode::Imad;
	case 0x02: return SassOpcode::Mov;
	case 0x81: return SassOpcode::Ldg;
	case 0x86: return SassOpcode::Stg;
	case 0x10: return SassOpcode::Iadd3;
	case 0x21: return SassOpcode::Fadd;
	case 0x19: return SassOpcode::S2r;
	case 0x82: return SassOpcode::Ldc;
	default: return SassOpcode::Unknown;
	}
}

} // namespace

SassDecoder::SassDecoder(uint32_t sm, uint32_t pointerBits): sm_(sm), pointerBits_(pointerBits)
{
	if (pointerBits_ != 32 && pointerBits_ != 64)
	{
		pointerBits_ = 64;
	}
	if (sm_ == 0)
	{
		sm_ = 80;
	}
}

SassInstr SassDecoder::decodeWord(uint64_t word0, uint64_t word1, uint64_t pc) const
{
	SassInstr in;
	in.pc = pc;
	in.word0 = word0;
	in.word1 = word1;
	in.size = sassInstrSize(sm_);

	if (isVoltaPlusSm(sm_))
	{
		const uint8_t op = static_cast<uint8_t>(word0 & 0xffu);
		in.pred = static_cast<uint8_t>((word0 >> 12) & 0xfu);
		in.dest = static_cast<uint8_t>((word0 >> 16) & 0xffu);
		in.src0 = static_cast<uint8_t>((word0 >> 24) & 0xffu);
		in.src1 = static_cast<uint8_t>((word0 >> 32) & 0xffu);
		in.src2 = static_cast<uint8_t>(word1 & 0xffu);
		in.opcode = opcodeVolta(op);
		in.decoded = in.opcode != SassOpcode::Unknown;
		return in;
	}

	// Pre-Volta 8-byte: only EXIT from CUDA Binary Utilities 9.1 is claimed.
	in.word1 = 0;
	if ((word0 & 0xffffull) == 0x1de7ull)
	{
		in.opcode = SassOpcode::Exit;
		in.decoded = true;
	}
	return in;
}

SassInstr SassDecoder::decodeBytes(const uint8_t* bytes, size_t n, uint64_t pc) const
{
	SassInstr in;
	in.pc = pc;
	in.size = sassInstrSize(sm_);
	if (!bytes || n < in.size)
	{
		return in;
	}
	const uint64_t w0 = readU64le(bytes);
	const uint64_t w1 = (in.size >= 16 && n >= 16) ? readU64le(bytes + 8) : 0;
	return decodeWord(w0, w1, pc);
}

std::vector<SassInstr> SassDecoder::decodeStream(const uint8_t* bytes, size_t n) const
{
	std::vector<SassInstr> out;
	if (!bytes || n == 0)
	{
		return out;
	}
	const uint32_t step = sassInstrSize(sm_);
	out.reserve(n / step);
	for (size_t off = 0; off + step <= n; off += step)
	{
		out.push_back(decodeBytes(bytes + off, n - off, off));
	}
	return out;
}

std::vector<SassInstr> SassDecoder::decodeStream(const std::vector<uint8_t>& bytes) const
{
	return decodeStream(bytes.data(), bytes.size());
}

void SassDecoder::decodeKernel(SassKernel& kernel) const
{
	kernel.instrs = decodeStream(kernel.bytes);
}

void SassDecoder::decodeModule(SassModule& mod) const
{
	SassDecoder d(mod.sm ? mod.sm : sm_, mod.pointerBits ? mod.pointerBits : pointerBits_);
	for (auto& k : mod.kernels)
	{
		d.decodeKernel(k);
	}
}

} // namespace sass_decode
} // namespace retdec
