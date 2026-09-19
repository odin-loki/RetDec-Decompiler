/**
 * @file include/retdec/sass_decode/sass_types.h
 * @brief NVIDIA SASS / cubin types.
 *
 * SASS is not a Capstone architecture. Encodings below are taken only from
 * public NVIDIA CUDA Binary Utilities listings (printed 8-byte / 16-byte
 * instruction words) plus in-tree ELF `EM_CUDA`.
 *
 * Citations:
 *   - ELF gABI / in-tree `deps/elfio/include/elfio/elf_types.hpp`: EM_CUDA = 190
 *   - NVIDIA CUDA Binary Utilities:
 *     https://docs.nvidia.com/cuda/cuda-binary-utilities/index.html
 *     (cubin is ELF; 16-byte PC stride on Volta+ listings; mnemonics)
 *   - NVIDIA CUDA Toolkit `fatbinary.h`: FATBIN_MAGIC 0xBA55ED50,
 *     fatBinaryHeader, FATBIN_KIND_PTX/ELF, FATBIN_FLAG_64BIT
 */

#ifndef RETDEC_SASS_DECODE_SASS_TYPES_H
#define RETDEC_SASS_DECODE_SASS_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace sass_decode {

/// ELF `e_machine` for NVIDIA CUDA cubin. In-tree: elfio `EM_CUDA`.
inline constexpr uint16_t kEmCuda = 190;

/// NVIDIA `fatbinary.h` `FATBIN_MAGIC`.
inline constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
/// NVIDIA `fatbinary.h` `OLD_STYLE_FATBIN_MAGIC`.
inline constexpr uint32_t kOldFatbinMagic = 0x1EE55A01u;

/// NVIDIA `fatbinary.h` code kinds.
inline constexpr uint16_t kFatbinKindPtx = 0x0001;
inline constexpr uint16_t kFatbinKindElf = 0x0002;

/// NVIDIA `fatbinary.h` `FATBIN_FLAG_64BIT` — GPU pointer width, not a CPU `-a`.
inline constexpr uint64_t kFatbinFlag64Bit = 0x1ull;
/// NVIDIA `fatbinary.h` `FATBIN_FLAG_COMPRESS`.
inline constexpr uint64_t kFatbinFlagCompress = 0x1000ull;

/// Zero register in NVIDIA listings (`RZ`).
inline constexpr uint8_t kRegZero = 255;
/// Predicate `PT` (always true) in Volta+ listings: bits [12:15] == 7.
inline constexpr uint8_t kPredTrue = 7;

enum class SassOpcode
{
	Unknown = 0,
	Nop,
	Exit,
	Bra,
	Imad,
	Mov,
	Ldg,
	Stg,
	Iadd3,
	Fadd,
	S2r,
	Ldc,
	ImadWide, ///< IMAD.WIDE — CUDA Binary Utilities 13.3 (`…7825`)
	Shfl,     ///< SHFL.IDX — CUDA Binary Utilities 12.8.2 (`…f389`)
	S2ur,     ///< S2UR — CUDA Binary Utilities 13.3 (`…79c3`)
	Ldcu,     ///< LDCU — CUDA Binary Utilities 13.3 (`…77ac`)
};

const char* sassOpcodeName(SassOpcode op);

inline bool isVoltaPlusSm(uint32_t sm)
{
	return sm >= 70;
}

/// Instruction word size: NVIDIA listings use 8-byte PCs before Volta and
/// 16-byte PCs from SM_70 (Volta) onward.
inline uint32_t sassInstrSize(uint32_t sm)
{
	return isVoltaPlusSm(sm) ? 16u : 8u;
}

/// GPU pointer width. Distinct from host `-a` / Capstone bitness.
struct SassConfig
{
	/// 0 = ELF class (ELF32 → 32-bit GPU pointers, ELF64 → 64-bit).
	/// 32 or 64 overrides. This is SM addressing, not a CPU `-a` flag.
	uint32_t pointerBits = 0;
	uint32_t smOverride = 0; ///< 0 = take SM from cubin e_flags
};

struct SassInstr
{
	uint64_t pc = 0;
	uint64_t word0 = 0;
	uint64_t word1 = 0;
	uint32_t size = 16;
	SassOpcode opcode = SassOpcode::Unknown;
	uint8_t pred = kPredTrue;
	uint8_t dest = 0;
	uint8_t src0 = 0;
	uint8_t src1 = 0;
	uint8_t src2 = kRegZero;
	bool decoded = false;
};

struct SassKernel
{
	std::string name;
	uint64_t fileOffset = 0;
	std::vector<uint8_t> bytes;
	std::vector<SassInstr> instrs;
};

struct SassModule
{
	uint32_t sm = 0;
	uint32_t pointerBits = 64;
	bool elf64 = true;
	uint16_t eMachine = 0;
	std::vector<SassKernel> kernels;
	std::string ptxText;
	std::string error;
	std::vector<std::string> warnings;
	bool ok() const { return error.empty(); }
};

bool isKnownSm(uint32_t sm);
uint32_t smFromEflags(uint32_t eFlags, uint8_t abiVersion);
std::string smName(uint32_t sm);

} // namespace sass_decode
} // namespace retdec

#endif
