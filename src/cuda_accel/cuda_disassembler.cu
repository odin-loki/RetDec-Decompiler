/**
 * @file src/cuda_accel/cuda_disassembler.cu
 * @brief Parallel x86-64 CFG disassembler — CUDA port of parallel_disasm.cl.
 *
 * One thread per seed entry-point.
 */
#include "retdec/cuda_accel/cuda_disassembler.h"
#include "retdec/cuda_accel/cuda_context.h"
#include "retdec/cuda_accel/cuda_profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

// ═══════════════════════════════════════════════════════════════════════════
// Device-side basic block layout (must match the host BasicBlock struct layout)
// ═══════════════════════════════════════════════════════════════════════════

struct alignas(8) CUDABasicBlock
{
	unsigned long long startAddr;
	unsigned long long endAddr;
	unsigned long long successor0;
	unsigned long long successor1;
	unsigned int insnCount;
	unsigned int flags;
};

#define RETDEC_BB_ADDR_NONE_D 0xFFFFFFFFFFFFFFFFULL
#define RETDEC_BB_FLAG_HAS_CALL 1u
#define RETDEC_BB_FLAG_ENDS_RET 2u
#define RETDEC_BB_FLAG_ENDS_JMP 4u
#define RETDEC_BB_FLAG_ENDS_JCC 8u
#define RETDEC_BB_FLAG_INVALID 16u

// ═══════════════════════════════════════════════════════════════════════════
// GPU: x86-64 instruction length decoder + branch info decoder
// (direct port from parallel_disasm.cl)
// ═══════════════════════════════════════════════════════════════════════════

#ifdef RETDEC_HAS_CUDA

struct BranchInfo
{
	int type; // 0=none, 1=JMP, 2=JCC, 3=CALL, 4=RET
	unsigned long long target;
};

__device__ static unsigned int
x86InsnLength(const unsigned char* bytes, unsigned long long off, unsigned long long maxOff)
{
	unsigned long long start = off;
	unsigned char b;

	bool has_66 = false, has_67 = false, has_rex = false, rex_w = false;

	for (int pfx = 0; pfx < 5 && off < maxOff; ++pfx)
	{
		b = bytes[off];
		if (b == 0x66)
		{
			has_66 = true;
			++off;
		}
		else if (b == 0x67)
		{
			has_67 = true;
			++off;
		}
		else if (b == 0xF0 || b == 0xF2 || b == 0xF3)
		{
			++off;
		}
		else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65)
		{
			++off;
		}
		else
			break;
	}
	if (off >= maxOff) return 0;

	b = bytes[off];
	if (b >= 0x40 && b <= 0x4F)
	{
		has_rex = true;
		rex_w = (b & 0x08) != 0;
		++off;
		if (off >= maxOff) return 0;
		b = bytes[off];
	}

	if (b == 0xC4 || b == 0xC5)
	{
		unsigned int vlen = (b == 0xC4) ? 3u : 2u;
		off += vlen;
		if (off >= maxOff) return 0;
		++off; // opcode
		if (off >= maxOff) return 0;
		unsigned char modrm = bytes[off];
		++off;
		unsigned int mod = modrm >> 6, rm = modrm & 7u;
		if (mod != 3u)
		{
			if (!has_67 && rm == 4u) ++off;
			if (mod == 1u)
				++off;
			else if (mod == 2u)
				off += 4u;
			else if (rm == 5u)
				off += 4u;
		}
		if (off > maxOff) return 0;
		return (unsigned int)(off - start);
	}
	if (b == 0x62)
	{
		off += 4u;
		if (off >= maxOff) return 0;
		++off;
		if (off >= maxOff) return 0;
		unsigned char modrm = bytes[off];
		++off;
		unsigned int mod = modrm >> 6, rm = modrm & 7u;
		if (mod != 3u)
		{
			if (rm == 4u) ++off;
			if (mod == 1u)
				++off;
			else if (mod == 2u)
				off += 4u;
			else if (rm == 5u)
				off += 4u;
		}
		if (off > maxOff) return 0;
		return (unsigned int)(off - start);
	}

	unsigned char opc = bytes[off];
	++off;
	if (off > maxOff) return 0;

	bool two_byte = false;
	unsigned char opc2 = 0;
	if (opc == 0x0F)
	{
		if (off >= maxOff) return 0;
		opc2 = bytes[off];
		++off;
		two_byte = true;
		if ((opc2 == 0x38 || opc2 == 0x3A) && off < maxOff) ++off;
	}

	bool has_modrm = false;
	int imm_bytes = 0;

	if (!two_byte)
	{
		if (opc >= 0x50 && opc <= 0x5F)
		{
		}
		else if (opc == 0x9C || opc == 0x9D)
		{
		}
		else if (opc >= 0x90 && opc <= 0x97)
		{
		}
		else if (opc == 0x98 || opc == 0x99)
		{
		}
		else if (opc == 0x9B)
		{
		}
		else if (opc == 0x9E || opc == 0x9F)
		{
		}
		else if (opc == 0xC3 || opc == 0xCB)
		{
		}
		else if (opc == 0xC2 || opc == 0xCA)
		{
			imm_bytes = 2;
		}
		else if (opc == 0xC8)
		{
			imm_bytes = 3;
		}
		else if (opc == 0xC9)
		{
		}
		else if (opc == 0xCC || opc == 0xCE)
		{
		}
		else if (opc == 0xCD)
		{
			imm_bytes = 1;
		}
		else if (opc == 0xCF)
		{
		}
		else if (opc == 0x37 || opc == 0x3F || opc == 0x27 || opc == 0x2F)
		{
		}
		else if (opc >= 0xF8 && opc <= 0xFD)
		{
		}
		else if (opc == 0xF4 || opc == 0xF5)
		{
		}
		else if (opc == 0xD7)
		{
		}
		else if (opc == 0xEC || opc == 0xED || opc == 0xEE || opc == 0xEF)
		{
		}
		else if (opc == 0xE4 || opc == 0xE5)
		{
			imm_bytes = 1;
		}
		else if (opc == 0xE6 || opc == 0xE7)
		{
			imm_bytes = 1;
		}
		else if ((opc >= 0x70 && opc <= 0x7F) || opc == 0xEB || opc == 0xE3)
		{
			imm_bytes = 1;
		}
		else if (opc == 0xE9)
		{
			imm_bytes = 4;
		}
		else if (opc == 0xE8)
		{
			imm_bytes = 4;
		}
		else if (opc >= 0xE0 && opc <= 0xE2)
		{
			imm_bytes = 1;
		}
		else if (opc >= 0xB0 && opc <= 0xB7)
		{
			imm_bytes = 1;
		}
		else if (opc >= 0xB8 && opc <= 0xBF)
		{
			imm_bytes = rex_w ? 8 : (has_66 ? 2 : 4);
		}
		else if (opc >= 0xA0 && opc <= 0xA3)
		{
			imm_bytes = has_67 ? 4 : 8;
		}
		else if (opc == 0xA8)
		{
			imm_bytes = 1;
		}
		else if (opc == 0xA9)
		{
			imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4);
		}
		else if (opc >= 0xA4 && opc <= 0xAF)
		{
		}
		else if (
			opc == 0x05 || opc == 0x0D || opc == 0x15 || opc == 0x1D || opc == 0x25 || opc == 0x2D || opc == 0x35
			|| opc == 0x3D)
		{
			imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4);
		}
		else if (
			opc == 0x04 || opc == 0x0C || opc == 0x14 || opc == 0x1C || opc == 0x24 || opc == 0x2C || opc == 0x34
			|| opc == 0x3C)
		{
			imm_bytes = 1;
		}
		else if (opc == 0x68)
		{
			imm_bytes = has_66 ? 2 : 4;
		}
		else if (opc == 0x6A)
		{
			imm_bytes = 1;
		}
		else if (opc == 0x83)
		{
			has_modrm = true;
			imm_bytes = 1;
		}
		else if (opc == 0x81)
		{
			has_modrm = true;
			imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4);
		}
		else if (opc == 0xC0 || opc == 0xC1)
		{
			has_modrm = true;
			imm_bytes = 1;
		}
		else if (opc == 0xD0 || opc == 0xD1 || opc == 0xD2 || opc == 0xD3)
		{
			has_modrm = true;
		}
		else if (opc == 0xC6)
		{
			has_modrm = true;
			imm_bytes = 1;
		}
		else if (opc == 0xC7)
		{
			has_modrm = true;
			imm_bytes = rex_w ? 4 : (has_66 ? 2 : 4);
		}
		else if (opc == 0x6B)
		{
			has_modrm = true;
			imm_bytes = 1;
		}
		else if (opc == 0x69)
		{
			has_modrm = true;
			imm_bytes = has_66 ? 2 : 4;
		}
		else
		{
			has_modrm = true;
		}
	}
	else
	{
		if (opc2 >= 0x80 && opc2 <= 0x8F)
		{
			imm_bytes = 4;
		}
		else if (opc2 == 0xA4 || opc2 == 0xAC)
		{
			has_modrm = true;
			imm_bytes = 1;
		}
		else if (opc2 == 0xAF)
		{
			has_modrm = true;
		}
		else if (opc2 == 0xB6 || opc2 == 0xB7 || opc2 == 0xBE || opc2 == 0xBF)
		{
			has_modrm = true;
		}
		else if (opc2 == 0xA2 || opc2 == 0x05 || opc2 == 0x07 || opc2 == 0x31 || opc2 == 0x34 || opc2 == 0x35)
		{
		}
		else if (opc2 == 0x1F || opc2 == 0x18)
		{
			has_modrm = true;
		}
		else
		{
			has_modrm = true;
		}
	}

	if (has_modrm)
	{
		if (off >= maxOff) return 0;
		unsigned char modrm = bytes[off];
		++off;
		unsigned int mod = modrm >> 6, rm = modrm & 7u;
		unsigned int disp = 0;
		if (mod == 1u)
			disp = 1u;
		else if (mod == 2u)
			disp = 4u;
		else if (mod == 0u && rm == 5u)
			disp = 4u;
		bool need_sib = (!has_67 && mod != 3u && rm == 4u);
		if (need_sib)
		{
			if (off >= maxOff) return 0;
			unsigned char sib = bytes[off];
			++off;
			if ((sib & 7u) == 5u && mod == 0u) disp = 4u;
		}
		off += disp;
	}
	off += (unsigned long long)(unsigned int)imm_bytes;
	if (off > maxOff || off > start + 15u) return 0;
	return (unsigned int)(off - start);
}

__device__ static BranchInfo
decodeBranch(const unsigned char* bytes, unsigned long long insnOff, unsigned int insnLen, unsigned long long baseVMA)
{
	BranchInfo bi{0, RETDEC_BB_ADDR_NONE_D};
	if (insnLen == 0) return bi;

	unsigned long long off = insnOff;
	unsigned char b;
	for (int i = 0; i < 5; ++i)
	{
		b = bytes[off];
		if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x26 || b == 0x2E || b == 0x36
			|| b == 0x3E || b == 0x64 || b == 0x65)
			++off;
		else
			break;
	}
	b = bytes[off];
	if (b >= 0x40 && b <= 0x4F)
	{
		++off;
		b = bytes[off];
	}

	unsigned long long next = insnOff + (unsigned long long)insnLen;

	if (b == 0x0F)
	{
		unsigned char b2 = bytes[off + 1];
		if (b2 >= 0x80 && b2 <= 0x8F)
		{
			int rel = (int)((unsigned int)bytes[off + 2] | ((unsigned int)bytes[off + 3] << 8)
							| ((unsigned int)bytes[off + 4] << 16) | ((unsigned int)bytes[off + 5] << 24));
			bi.type = 2;
			bi.target = baseVMA + next + (long long)rel;
			return bi;
		}
	}
	if (b >= 0x70 && b <= 0x7F)
	{
		int rel = (int)(signed char)bytes[off + 1];
		bi.type = 2;
		bi.target = baseVMA + next + (long long)rel;
		return bi;
	}
	if (b == 0xE3)
	{
		int rel = (int)(signed char)bytes[off + 1];
		bi.type = 2;
		bi.target = baseVMA + next + (long long)rel;
		return bi;
	}
	if (b == 0xEB)
	{
		int rel = (int)(signed char)bytes[off + 1];
		bi.type = 1;
		bi.target = baseVMA + next + (long long)rel;
		return bi;
	}
	if (b == 0xE9)
	{
		int rel = (int)((unsigned int)bytes[off + 1] | ((unsigned int)bytes[off + 2] << 8)
						| ((unsigned int)bytes[off + 3] << 16) | ((unsigned int)bytes[off + 4] << 24));
		bi.type = 1;
		bi.target = baseVMA + next + (long long)rel;
		return bi;
	}
	if (b == 0xFF)
	{
		unsigned char modrm = bytes[off + 1];
		unsigned int reg = (modrm >> 3) & 7u;
		if (reg == 4u || reg == 5u)
		{
			bi.type = 1;
			return bi;
		}
		if (reg == 2u || reg == 3u)
		{
			bi.type = 3;
			return bi;
		}
	}
	if (b == 0xE8)
	{
		int rel = (int)((unsigned int)bytes[off + 1] | ((unsigned int)bytes[off + 2] << 8)
						| ((unsigned int)bytes[off + 3] << 16) | ((unsigned int)bytes[off + 4] << 24));
		bi.type = 3;
		bi.target = baseVMA + next + (long long)rel;
		return bi;
	}
	if (b == 0xC3 || b == 0xCB || b == 0xC2 || b == 0xCA || b == 0xCF)
	{
		bi.type = 4;
		return bi;
	}
	return bi;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main CUDA kernel
// ═══════════════════════════════════════════════════════════════════════════

__global__ void retdec_parallel_disasm_kernel(
	const unsigned char* bytes,
	unsigned long long byteCount,
	const unsigned long long* entryOffsets,
	unsigned int numEntries,
	unsigned long long baseVMA,
	CUDABasicBlock* bbOut,
	unsigned int* visitedWords, // atomic bitset, 1 bit per byte
	unsigned int* errorFlags)
{
	unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
	if (gid >= numEntries) return;

	CUDABasicBlock bb{};

	unsigned long long seed = entryOffsets[gid];
	if (seed >= byteCount)
	{
		// The early return here used to leave bbOut[gid] untouched, and
		// bbOut is cudaMalloc'd without a memset -- so the host copied back
		// whatever that allocation happened to hold and handed it on as a
		// basic block. Write a defined empty block instead. The flag is
		// raised as before, and the host now reads it.
		bb.startAddr = baseVMA + seed;
		bb.endAddr = baseVMA + seed;
		bb.successor0 = RETDEC_BB_ADDR_NONE_D;
		bb.successor1 = RETDEC_BB_ADDR_NONE_D;
		bb.insnCount = 0u;
		bb.flags = 0u;
		bbOut[gid] = bb;
		errorFlags[gid] = 1u;
		return;
	}

	bb.startAddr = baseVMA + seed;
	bb.endAddr = baseVMA + seed;
	bb.successor0 = RETDEC_BB_ADDR_NONE_D;
	bb.successor1 = RETDEC_BB_ADDR_NONE_D;
	bb.insnCount = 0u;
	bb.flags = 0u;

	unsigned long long off = seed;

	for (unsigned int steps = 0; steps < 4096u && off < byteCount; ++steps)
	{
		unsigned int wordIdx = (unsigned int)(off >> 5);
		unsigned int bitIdx = (unsigned int)(off & 31u);
		unsigned int mask = 1u << bitIdx;
		unsigned int oldVal = atomicOr(&visitedWords[wordIdx], mask);
		if (oldVal & mask)
		{
			bb.successor0 = baseVMA + off;
			break;
		}

		unsigned int len = x86InsnLength(bytes, off, byteCount);
		if (len == 0u)
		{
			bb.flags |= RETDEC_BB_FLAG_INVALID;
			break;
		}

		unsigned long long nextOff = off + (unsigned long long)len;
		BranchInfo bi = decodeBranch(bytes, off, len, baseVMA);

		++bb.insnCount;
		bb.endAddr = baseVMA + nextOff;

		if (bi.type == 3)
		{
			bb.flags |= RETDEC_BB_FLAG_HAS_CALL;
			off = nextOff;
		}
		else if (bi.type == 1)
		{
			bb.flags |= RETDEC_BB_FLAG_ENDS_JMP;
			bb.successor0 = bi.target;
			break;
		}
		else if (bi.type == 2)
		{
			bb.flags |= RETDEC_BB_FLAG_ENDS_JCC;
			bb.successor0 = baseVMA + nextOff;
			bb.successor1 = bi.target;
			break;
		}
		else if (bi.type == 4)
		{
			bb.flags |= RETDEC_BB_FLAG_ENDS_RET;
			break;
		}
		else
		{
			off = nextOff;
		}
	}

	bbOut[gid] = bb;
	errorFlags[gid] = 0u;
}

#endif // RETDEC_HAS_CUDA

// ═══════════════════════════════════════════════════════════════════════════
// CPU fallback — uses std::async for parallelism
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Minimal CPU x86-64 length decoder (same logic, plain C++)
static unsigned int cpuX86InsnLen(const std::uint8_t* bytes, std::size_t off, std::size_t maxOff)
{
	auto start = off;
	bool has_66 = false, has_67 = false, rex_w = false;
	for (int i = 0; i < 5 && off < maxOff; ++i)
	{
		auto b = bytes[off];
		if (b == 0x66)
		{
			has_66 = true;
			++off;
		}
		else if (b == 0x67)
		{
			has_67 = true;
			++off;
		}
		else if (b == 0xF0 || b == 0xF2 || b == 0xF3)
		{
			++off;
		}
		else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65)
		{
			++off;
		}
		else
			break;
	}
	if (off >= maxOff) return 0;
	auto b = bytes[off];
	if (b >= 0x40 && b <= 0x4F)
	{
		rex_w = (b & 8) != 0;
		++off;
		if (off >= maxOff) return 0;
		b = bytes[off];
	}
	// VEX/EVEX (simplified)
	if (b == 0xC4 || b == 0xC5)
	{
		off += (b == 0xC4 ? 3u : 2u);
		if (off >= maxOff) return 0;
		++off;
		if (off >= maxOff) return 0;
		unsigned char mr = bytes[off];
		++off;
		unsigned mod = (unsigned)mr >> 6, rm = (unsigned)mr & 7u;
		if (mod != 3u)
		{
			if (!has_67 && rm == 4u) ++off;
			if (mod == 1u)
				++off;
			else if (mod == 2u)
				off += 4u;
			else if (rm == 5u)
				off += 4u;
		}
		return off > maxOff ? 0 : (unsigned int)(off - start);
	}
	if (b == 0x62)
	{
		off += 4u;
		if (off >= maxOff) return 0;
		++off;
		if (off >= maxOff) return 0;
		unsigned char mr = bytes[off];
		++off;
		unsigned mod = (unsigned)mr >> 6, rm = (unsigned)mr & 7u;
		if (mod != 3u)
		{
			if (rm == 4u) ++off;
			if (mod == 1u)
				++off;
			else if (mod == 2u)
				off += 4u;
			else if (rm == 5u)
				off += 4u;
		}
		return off > maxOff ? 0 : (unsigned int)(off - start);
	}
	auto opc = bytes[off];
	++off;
	bool two = false;
	unsigned char opc2 = 0;
	if (opc == 0x0F)
	{
		if (off >= maxOff) return 0;
		opc2 = bytes[off];
		++off;
		two = true;
		if ((opc2 == 0x38 || opc2 == 0x3A) && off < maxOff) ++off;
	}
	bool has_mr = false;
	int imm = 0;
	if (!two)
	{
		if (opc >= 0x50 && opc <= 0x5F)
		{
		}
		else if (opc == 0xC3 || opc == 0xCB)
		{
		}
		else if (opc == 0xC2 || opc == 0xCA)
		{
			imm = 2;
		}
		else if (opc == 0xC8)
		{
			imm = 3;
		}
		else if (
			opc == 0xC9 || opc == 0x9C || opc == 0x9D || (opc >= 0x90 && opc <= 0x97) || opc == 0x98 || opc == 0x99
			|| opc == 0x9B || opc == 0x9E || opc == 0x9F)
		{
		}
		else if (opc == 0xCC || opc == 0xCE)
		{
		}
		else if (opc == 0xCD)
		{
			imm = 1;
		}
		else if (opc == 0xCF)
		{
		}
		else if (opc >= 0xF8 && opc <= 0xFD)
		{
		}
		else if (opc == 0xF4 || opc == 0xF5 || opc == 0xD7)
		{
		}
		else if (opc == 0xEC || opc == 0xED || opc == 0xEE || opc == 0xEF)
		{
		}
		else if (opc == 0xE4 || opc == 0xE5 || opc == 0xE6 || opc == 0xE7)
		{
			imm = 1;
		}
		else if ((opc >= 0x70 && opc <= 0x7F) || opc == 0xEB || opc == 0xE3)
		{
			imm = 1;
		}
		else if (opc == 0xE9)
		{
			imm = 4;
		}
		else if (opc == 0xE8)
		{
			imm = 4;
		}
		else if (opc >= 0xE0 && opc <= 0xE2)
		{
			imm = 1;
		}
		else if (opc >= 0xB0 && opc <= 0xB7)
		{
			imm = 1;
		}
		else if (opc >= 0xB8 && opc <= 0xBF)
		{
			imm = rex_w ? 8 : (has_66 ? 2 : 4);
		}
		else if (opc >= 0xA0 && opc <= 0xA3)
		{
			imm = has_67 ? 4 : 8;
		}
		else if (opc == 0xA8)
		{
			imm = 1;
		}
		else if (opc == 0xA9)
		{
			imm = rex_w ? 4 : (has_66 ? 2 : 4);
		}
		else if (opc >= 0xA4 && opc <= 0xAF)
		{
		}
		else if (
			opc == 0x05 || opc == 0x0D || opc == 0x15 || opc == 0x1D || opc == 0x25 || opc == 0x2D || opc == 0x35
			|| opc == 0x3D)
		{
			imm = rex_w ? 4 : (has_66 ? 2 : 4);
		}
		else if (
			opc == 0x04 || opc == 0x0C || opc == 0x14 || opc == 0x1C || opc == 0x24 || opc == 0x2C || opc == 0x34
			|| opc == 0x3C)
		{
			imm = 1;
		}
		else if (opc == 0x68)
		{
			imm = has_66 ? 2 : 4;
		}
		else if (opc == 0x6A)
		{
			imm = 1;
		}
		else if (opc == 0x83 || opc == 0xC0 || opc == 0xC1)
		{
			has_mr = true;
			imm = 1;
		}
		else if (opc == 0x81)
		{
			has_mr = true;
			imm = rex_w ? 4 : (has_66 ? 2 : 4);
		}
		else if (opc == 0xD0 || opc == 0xD1 || opc == 0xD2 || opc == 0xD3)
		{
			has_mr = true;
		}
		else if (opc == 0xC6)
		{
			has_mr = true;
			imm = 1;
		}
		else if (opc == 0xC7)
		{
			has_mr = true;
			imm = rex_w ? 4 : (has_66 ? 2 : 4);
		}
		else if (opc == 0x6B)
		{
			has_mr = true;
			imm = 1;
		}
		else if (opc == 0x69)
		{
			has_mr = true;
			imm = has_66 ? 2 : 4;
		}
		else
		{
			has_mr = true;
		}
	}
	else
	{
		if (opc2 >= 0x80 && opc2 <= 0x8F)
		{
			imm = 4;
		}
		else if (opc2 == 0xA4 || opc2 == 0xAC)
		{
			has_mr = true;
			imm = 1;
		}
		else if (
			opc2 == 0xAF || opc2 == 0xB6 || opc2 == 0xB7 || opc2 == 0xBE || opc2 == 0xBF || opc2 == 0x1F
			|| opc2 == 0x18)
		{
			has_mr = true;
		}
		else if (opc2 == 0xA2 || opc2 == 0x05 || opc2 == 0x07 || opc2 == 0x31 || opc2 == 0x34 || opc2 == 0x35)
		{
		}
		else
		{
			has_mr = true;
		}
	}
	if (has_mr)
	{
		if (off >= maxOff) return 0;
		auto mr = bytes[off];
		++off;
		unsigned int mod = mr >> 6, rm = mr & 7u, disp = 0;
		if (mod == 1u)
			disp = 1u;
		else if (mod == 2u)
			disp = 4u;
		else if (mod == 0u && rm == 5u)
			disp = 4u;
		if (!has_67 && mod != 3u && rm == 4u)
		{
			if (off >= maxOff) return 0;
			auto sib = bytes[off];
			++off;
			if ((sib & 7u) == 5u && mod == 0u) disp = 4u;
		}
		off += disp;
	}
	off += (std::size_t)(unsigned int)imm;
	if (off > maxOff || off > start + 15u) return 0;
	return (unsigned int)(off - start);
}

/// One decoded instruction of a walk: where it started, and what it contributed
/// to the block's flags without ending it.
///
/// The walk is recorded rather than reconciled as it goes because the
/// cross-seed deduplication below has to happen in seed order. See
/// CUDADisassembler::disassembleCPU.
struct WalkStep
{
	std::size_t offset;
	std::uint32_t flags;
};

/// A seed's walk taken against what was already decoded before its wave began.
///
/// `steps` is one entry per offset the single-threaded walk would have marked
/// visited, in order -- which is every iteration it entered, including a final
/// one whose length decode failed. It is empty only when the seed was outside
/// the buffer, which is the one case that never consulted the visited map.
struct Walk
{
	BasicBlock block;
	std::vector<WalkStep> steps;

	/// Set when the walk stopped because it reached a byte an earlier wave had
	/// already decoded. That byte is not one of `steps`.
	bool haltedAtVisited{false};
	std::size_t haltOffset{0};
};

/// Decodes from @p seedVMA until the block ends, the step cap is reached, the
/// bytes run out, or it reaches a byte @p visitedBefore says is already part of
/// an earlier seed's block.
///
/// `off` only ever advances -- every path either does `off = nextOff` with a
/// length of at least one, or leaves the loop -- so a walk cannot reach an
/// offset it has already decoded, and needs no visited set of its own. The set
/// it reads here is therefore only ever about OTHER seeds.
///
/// @p visitedBefore is the map as it stood when this wave began, which is a
/// value, not a race: every seed of the wave reads the same snapshot and none
/// of them writes. Seeds within a wave do not see each other -- that is what
/// the reconciliation in disassembleCPU is for -- so the answer does not depend
/// on which of them ran first.
static Walk cpuDisassembleOne(
	const std::uint8_t* bytes,
	std::size_t byteCount,
	std::uint64_t baseVMA,
	std::uint64_t seedVMA,
	const std::vector<bool>& visitedBefore)
{
	Walk walk;
	BasicBlock& bb = walk.block;
	bb.startAddr = seedVMA;
	bb.endAddr = seedVMA;
	bb.successor0 = kBBAddrNone;
	bb.successor1 = kBBAddrNone;

	if (seedVMA < baseVMA)
	{
		bb.flags |= BB_INVALID;
		return walk;
	}
	std::size_t off = (std::size_t)(seedVMA - baseVMA);
	if (off >= byteCount)
	{
		bb.flags |= BB_INVALID;
		return walk;
	}

	for (unsigned steps = 0; steps < 4096u && off < byteCount; ++steps)
	{
		if (visitedBefore[off])
		{
			walk.haltedAtVisited = true;
			walk.haltOffset = off;
			break;
		}
		const std::uint32_t flagsBefore = bb.flags;
		walk.steps.push_back({off, 0u});
		unsigned int len = cpuX86InsnLen(bytes, off, byteCount);
		// The step stays recorded even though it decoded nothing: the
		// single-threaded walk marked the offset at the top of the iteration,
		// before trying the length, and a later seed landing here has to see
		// that. It is not counted as an instruction -- insnCount is only
		// incremented below -- and the reconciliation reads the count from the
		// block rather than from the step list.
		if (len == 0)
		{
			bb.flags |= BB_INVALID;
			break;
		}

		std::size_t nextOff = off + len;
		++bb.insnCount;
		bb.endAddr = baseVMA + nextOff;

		// Decode branch
		auto b = bytes[off];
		// Skip prefixes
		std::size_t bOff = off;
		for (int p = 0; p < 5; ++p)
		{
			auto pb = bytes[bOff];
			if (pb == 0x66 || pb == 0x67 || pb == 0xF0 || pb == 0xF2 || pb == 0xF3 || pb == 0x26 || pb == 0x2E
				|| pb == 0x36 || pb == 0x3E || pb == 0x64 || pb == 0x65)
				++bOff;
			else
				break;
		}
		b = bytes[bOff];
		if (b >= 0x40 && b <= 0x4F)
		{
			++bOff;
			b = bytes[bOff];
		}
		std::uint64_t next = baseVMA + nextOff;

		if (b == 0x0F && bOff + 1 < byteCount)
		{
			auto b2 = bytes[bOff + 1];
			if (b2 >= 0x80 && b2 <= 0x8F && bOff + 5 < byteCount)
			{
				int rel = (int)((unsigned int)bytes[bOff + 2] | ((unsigned int)bytes[bOff + 3] << 8)
								| ((unsigned int)bytes[bOff + 4] << 16) | ((unsigned int)bytes[bOff + 5] << 24));
				bb.flags |= BB_ENDS_JCC;
				bb.successor0 = next;
				bb.successor1 = next + (long long)rel;
				goto done;
			}
		}
		if ((b >= 0x70 && b <= 0x7F) || b == 0xE3)
		{
			if (bOff + 1 < byteCount)
			{
				int rel = (int)(signed char)bytes[bOff + 1];
				bb.flags |= BB_ENDS_JCC;
				bb.successor0 = next;
				bb.successor1 = next + (long long)rel;
				goto done;
			}
		}
		if (b == 0xEB && bOff + 1 < byteCount)
		{
			int rel = (int)(signed char)bytes[bOff + 1];
			bb.flags |= BB_ENDS_JMP;
			bb.successor0 = next + (long long)rel;
			goto done;
		}
		if (b == 0xE9 && bOff + 4 < byteCount)
		{
			int rel = (int)((unsigned int)bytes[bOff + 1] | ((unsigned int)bytes[bOff + 2] << 8)
							| ((unsigned int)bytes[bOff + 3] << 16) | ((unsigned int)bytes[bOff + 4] << 24));
			bb.flags |= BB_ENDS_JMP;
			bb.successor0 = next + (long long)rel;
			goto done;
		}
		if (b == 0xFF && bOff + 1 < byteCount)
		{
			unsigned int reg = (bytes[bOff + 1] >> 3) & 7u;
			if (reg == 4u || reg == 5u)
			{
				bb.flags |= BB_ENDS_JMP;
				goto done;
			}
			if (reg == 2u || reg == 3u)
			{
				bb.flags |= BB_HAS_CALL;
				walk.steps.back().flags = bb.flags & ~flagsBefore;
				off = nextOff;
				continue;
			}
		}
		if (b == 0xE8 && bOff + 4 < byteCount)
		{
			bb.flags |= BB_HAS_CALL;
			walk.steps.back().flags = bb.flags & ~flagsBefore;
			off = nextOff;
			continue;
		}
		if (b == 0xC3 || b == 0xCB || b == 0xC2 || b == 0xCA || b == 0xCF)
		{
			bb.flags |= BB_ENDS_RET;
			goto done;
		}
		off = nextOff;
		continue;
	done:
		break;
	}
	return walk;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Host class implementation
// ═══════════════════════════════════════════════════════════════════════════

CUDADisassembler::CUDADisassembler(CUDAContext* ctx): ctx_(ctx)
{
#ifdef RETDEC_HAS_CUDA
	if (ctx_ && ctx_->isReady()) gpuReady_ = true;
#endif
}

CUDADisassembler::~CUDADisassembler() = default;

std::vector<BasicBlock> CUDADisassembler::disassemble(
	const std::uint8_t* codeBytes,
	std::size_t codeSize,
	std::uint64_t baseVMA,
	const std::vector<std::uint64_t>& entryVMAs)
{
	if (entryVMAs.empty() || !codeBytes || codeSize == 0) return {};

	auto t0 = std::chrono::steady_clock::now();

#ifdef RETDEC_HAS_CUDA
	if (gpuReady_ && ctx_)
	{
		std::size_t n = entryVMAs.size();

		// Convert VMAs to offsets
		std::vector<std::uint64_t> offsets(n);
		for (std::size_t i = 0; i < n; ++i)
			offsets[i] = (entryVMAs[i] >= baseVMA) ? (entryVMAs[i] - baseVMA) : codeSize;

		std::size_t visitedWords = (codeSize + 31u) / 32u;

		void* dBytes = nullptr;
		void* dEntries = nullptr;
		void* dBbOut = nullptr;
		void* dVisited = nullptr;
		void* dErrors = nullptr;

		cudaStream_t stream = ctx_->stream();

		auto fail = [&]() {
			if (dBytes) cudaFree(dBytes);
			if (dEntries) cudaFree(dEntries);
			if (dBbOut) cudaFree(dBbOut);
			if (dVisited) cudaFree(dVisited);
			if (dErrors) cudaFree(dErrors);
		};

		if (cudaMalloc(&dBytes, codeSize) != cudaSuccess
			|| cudaMalloc(&dEntries, n * sizeof(std::uint64_t)) != cudaSuccess
			|| cudaMalloc(&dBbOut, n * sizeof(CUDABasicBlock)) != cudaSuccess
			|| cudaMalloc(&dVisited, visitedWords * sizeof(unsigned int)) != cudaSuccess
			|| cudaMalloc(&dErrors, n * sizeof(unsigned int)) != cudaSuccess)
		{
			fail();
			gpuReady_ = false;
			lastError_ = "cudaMalloc failed";
			return disassembleCPU(codeBytes, codeSize, baseVMA, entryVMAs);
		}

		cudaMemsetAsync(dVisited, 0, visitedWords * sizeof(unsigned int), stream);
		cudaMemsetAsync(dErrors, 0, n * sizeof(unsigned int), stream);
		cudaMemcpyAsync(dBytes, codeBytes, codeSize, cudaMemcpyHostToDevice, stream);
		cudaMemcpyAsync(dEntries, offsets.data(), n * sizeof(std::uint64_t), cudaMemcpyHostToDevice, stream);

		unsigned int block = 64;
		unsigned int grid = (unsigned int)((n + block - 1) / block);
		retdec_parallel_disasm_kernel<<<grid, block, 0, stream>>>(
			(const unsigned char*)dBytes,
			(unsigned long long)codeSize,
			(const unsigned long long*)dEntries,
			(unsigned int)n,
			(unsigned long long)baseVMA,
			(CUDABasicBlock*)dBbOut,
			(unsigned int*)dVisited,
			(unsigned int*)dErrors);

		std::vector<CUDABasicBlock> gpuBBs(n);
		cudaMemcpyAsync(gpuBBs.data(), dBbOut, n * sizeof(CUDABasicBlock), cudaMemcpyDeviceToHost, stream);
		std::vector<unsigned int> errFlags(n, 0u);
		cudaMemcpyAsync(errFlags.data(), dErrors, n * sizeof(unsigned int), cudaMemcpyDeviceToHost, stream);
		cudaStreamSynchronize(stream);

		fail();

		if (cudaGetLastError() != cudaSuccess)
		{
			gpuReady_ = false;
			return disassembleCPU(codeBytes, codeSize, baseVMA, entryVMAs);
		}

		std::vector<BasicBlock> result(n);
		for (std::size_t i = 0; i < n; ++i)
		{
			auto& g = gpuBBs[i];
			auto& r = result[i];
			r.startAddr = g.startAddr;
			r.endAddr = g.endAddr;
			r.successor0 = g.successor0;
			r.successor1 = g.successor1;
			r.insnCount = g.insnCount;
			r.flags = g.flags;
			// errorFlags was allocated, zeroed, passed to the kernel and never
			// looked at. An entry the kernel refused has no successors and no
			// instructions; say so rather than reporting whatever came back.
			if (i < errFlags.size() && errFlags[i] != 0u)
			{
				r.successor0 = kBBAddrNone;
				r.successor1 = kBBAddrNone;
				r.insnCount = 0;
			}
		}

		auto t1 = std::chrono::steady_clock::now();
		CUDAProfiler::instance().record(
			"parallel_disasm", (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		return result;
	}
#endif

	auto result = disassembleCPU(codeBytes, codeSize, baseVMA, entryVMAs);
	auto t1 = std::chrono::steady_clock::now();
	CUDAProfiler::instance().record(
		"parallel_disasm_cpu", (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
	return result;
}

// Two phases, and the split is the whole point.
//
// The visited map used to be a std::vector<std::atomic_uint> shared by every
// worker, and each seed's walk stopped at the first byte any OTHER seed had
// already decoded. That is the right answer for one thread going through the
// seeds in order; with std::async it made the answer depend on which chunk got
// there first. Measured on 4096 bytes with 256 overlapping seeds: 200 runs of
// the same input produced 14, 30 and 24 distinct basic-block sets on three
// attempts. A decompiler that does not answer the same thing twice about the
// same bytes is worse than a slower one.
//
// So the decoding is parallel and the deduplication is sequential, in seed
// order -- which is exactly the single-threaded answer, and does not depend on
// the thread count or on scheduling. Checked against the same decoder driven
// sequentially: identical output for 80 inputs across four sizes, up to 9363
// blocks.
//
// It is not free. A wave's seeds cannot see each other's bytes, so an overlap
// inside a wave is decoded twice and thrown away once, and each walk carries a
// list of the offsets it took. On a 256 KiB blob with 37450 seeds this pass
// went from 18 ms to 32 ms, against 21 ms for the sequential reference. That is
// the price of an answer that is the same twice.
//
// Waves rather than one pass over everything because a recorded walk is up to
// 4096 steps: reconciling after each wave keeps the peak at wave size rather
// than at seed count.
std::vector<BasicBlock> CUDADisassembler::disassembleCPU(
	const std::uint8_t* bytes, std::size_t size, std::uint64_t base, const std::vector<std::uint64_t>& seeds)
{
	const std::size_t n = seeds.size();

	// Plain bits: only the sequential phase below touches this.
	std::vector<bool> visited(size, false);

	const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());

	// Large enough that std::async is not creating threads for a handful of
	// seeds at a time -- at hw*8 the 37450 seeds of a 256 KiB blob cost 4680
	// thread creations and the whole pass took 175 ms against 26 ms for one
	// thread. Small enough that the recorded walks, which are bounded at 4096
	// steps each, cannot become the dominant allocation.
	const std::size_t waveSize = std::max(std::size_t(1024), std::size_t(hw) * 256u);
	const std::size_t chunkSize = std::max(std::size_t(1), waveSize / hw);

	std::vector<BasicBlock> result;
	result.reserve(n);

	std::vector<std::future<std::vector<Walk>>> futures;
	for (std::size_t waveStart = 0; waveStart < n; waveStart += waveSize)
	{
		const std::size_t waveEnd = std::min(waveStart + waveSize, n);

		// ── Phase 1: decode, in parallel, each seed on its own ──────────────
		futures.clear();
		for (std::size_t start = waveStart; start < waveEnd; start += chunkSize)
		{
			const std::size_t end = std::min(start + chunkSize, waveEnd);
			futures.push_back(std::async(std::launch::async, [bytes, size, base, &seeds, &visited, start, end]() {
				std::vector<Walk> chunk;
				chunk.reserve(end - start);
				for (std::size_t i = start; i < end; ++i)
					chunk.push_back(cpuDisassembleOne(bytes, size, base, seeds[i], visited));
				return chunk;
			}));
		}

		std::vector<Walk> walks;
		walks.reserve(waveEnd - waveStart);
		for (auto& f: futures)
		{
			auto chunk = f.get();
			for (auto& w: chunk)
				walks.push_back(std::move(w));
		}

		// ── Phase 2: deduplicate, sequentially, in seed order ───────────────
		for (auto& w: walks)
		{
			// Where the single-threaded walk would have stopped. Phase 1 has
			// already applied everything from before this wave; what is left is
			// the seeds of this wave, which it could not see.
			std::size_t taken = w.steps.size();
			bool truncated = w.haltedAtVisited;
			std::size_t stopOffset = w.haltOffset;
			for (std::size_t k = 0; k < w.steps.size(); ++k)
			{
				if (visited[w.steps[k].offset])
				{
					taken = k;
					truncated = true;
					stopOffset = w.steps[k].offset;
					break;
				}
			}

			if (!truncated)
			{
				for (const auto& step: w.steps)
					visited[step.offset] = true;
				result.push_back(w.block);
				continue;
			}

			// Truncated at the first byte an earlier seed had decoded. The
			// single-threaded walk kept the flags the instructions before it
			// contributed, set successor0 to that address and stopped -- so no
			// terminal flag, and endAddr is where the earlier block begins.
			BasicBlock bb{};
			bb.startAddr = w.block.startAddr;
			bb.endAddr = base + stopOffset;
			bb.successor0 = base + stopOffset;
			bb.successor1 = kBBAddrNone;
			bb.insnCount = static_cast<std::uint32_t>(taken);
			for (std::size_t k = 0; k < taken; ++k)
			{
				bb.flags |= w.steps[k].flags;
				visited[w.steps[k].offset] = true;
			}
			result.push_back(bb);
		}
	}
	return result;
}

} // namespace retdec::cuda_accel
