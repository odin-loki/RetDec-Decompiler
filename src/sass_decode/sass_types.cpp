/**
 * @file src/sass_decode/sass_types.cpp
 * @brief SASS type helpers (SM names, ELF e_flags).
 */

#include "retdec/sass_decode/sass_types.h"

#include <sstream>

namespace retdec {
namespace sass_decode {

const char* sassOpcodeName(SassOpcode op)
{
	switch (op)
	{
	case SassOpcode::Nop: return "NOP";
	case SassOpcode::Exit: return "EXIT";
	case SassOpcode::Bra: return "BRA";
	case SassOpcode::Imad: return "IMAD";
	case SassOpcode::Mov: return "MOV";
	case SassOpcode::Ldg: return "LDG";
	case SassOpcode::Stg: return "STG";
	case SassOpcode::Iadd3: return "IADD3";
	case SassOpcode::Fadd: return "FADD";
	case SassOpcode::S2r: return "S2R";
	case SassOpcode::Ldc: return "LDC";
	case SassOpcode::Unknown:
	default: return "UNKNOWN";
	}
}

bool isKnownSm(uint32_t sm)
{
	switch (sm)
	{
	case 20:
	case 21:
	case 30:
	case 32:
	case 35:
	case 37:
	case 50:
	case 52:
	case 53:
	case 60:
	case 61:
	case 62:
	case 70:
	case 72:
	case 75:
	case 80:
	case 86:
	case 87:
	case 88:
	case 89:
	case 90:
	case 100:
	case 101:
	case 103:
	case 110:
	case 120:
	case 121:
		return true;
	default:
		return false;
	}
}

uint32_t smFromEflags(uint32_t eFlags, uint8_t abiVersion)
{
	const uint32_t low = eFlags & 0xffu;
	const uint32_t mid = (eFlags >> 8) & 0xffu;
	// NVIDIA cuobjdump prints `EF_CUDA_SMxx`. ABI v1 stores the SM number in
	// e_flags[7:0] (0x50 = sm_80). CUDA 13 ABI v2 shifts it to bits [15:8].
	if (abiVersion >= 8 && isKnownSm(mid))
	{
		return mid;
	}
	if (isKnownSm(low))
	{
		return low;
	}
	if (isKnownSm(mid))
	{
		return mid;
	}
	return low;
}

std::string smName(uint32_t sm)
{
	if (sm == 0)
	{
		return "sm_unknown";
	}
	std::ostringstream os;
	os << "sm_" << sm;
	return os.str();
}

} // namespace sass_decode
} // namespace retdec
