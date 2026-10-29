/**
 * @file src/bin2llvmir/providers/calling_convention/riscv/riscv_conv.cpp
 * @brief Calling convention of RISC-V (RV32/RV64 psABI).
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <memory>
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/calling_convention/riscv/riscv_conv.h"
#include "retdec/capstone2llvmir/riscv/riscv_defs.h"
#include "capstone2llvmir/capstone6_compat.h"

namespace retdec {
namespace bin2llvmir {

RiscvCallingConvention::RiscvCallingConvention(const Abi* a) :
	CallingConvention(a)
{
	// Integer arguments in a0-a7, floating point in fa0-fa7; results in
	// a0/a1 and fa0/fa1.  RV32 passes 64-bit scalars in aligned-free pairs.
	_paramRegs = {
		RISCV_REG_A0, RISCV_REG_A1, RISCV_REG_A2, RISCV_REG_A3,
		RISCV_REG_A4, RISCV_REG_A5, RISCV_REG_A6, RISCV_REG_A7
	};
	_paramFPRegs = {
		RISCV_REG_F10_64, RISCV_REG_F11_64, RISCV_REG_F12_64, RISCV_REG_F13_64,
		RISCV_REG_F14_64, RISCV_REG_F15_64, RISCV_REG_F16_64, RISCV_REG_F17_64
	};
	_returnRegs = {RISCV_REG_A0, RISCV_REG_A1};
	_returnFPRegs = {RISCV_REG_F10_64, RISCV_REG_F11_64};
	_largeObjectsPassedByReference = true;
	if (!a->isRiscv64())
	{
		_numOfRegsPerParam = 2;
	}
}

CallingConvention::Ptr RiscvCallingConvention::create(const Abi* a)
{
	if (!a->isRiscv() && !a->isRiscv64())
	{
		return nullptr;
	}
	return std::make_unique<RiscvCallingConvention>(a);
}

}
}
