/**
 * @file include/retdec/bin2llvmir/providers/calling_convention/riscv/riscv_conv.h
 * @brief Calling convention of RISC-V (RV32/RV64 psABI).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_CALL_CONV_RISCV_RISCV_CONV_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_CALL_CONV_RISCV_RISCV_CONV_H

#include "retdec/bin2llvmir/providers/calling_convention/calling_convention.h"

namespace retdec {
namespace bin2llvmir {

class RiscvCallingConvention: public CallingConvention
{
	public:
		RiscvCallingConvention(const Abi* a);
		static CallingConvention::Ptr create(const Abi* a);
};

}
}

#endif
