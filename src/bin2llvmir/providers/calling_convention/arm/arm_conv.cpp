/**
 * @file src/bin2llvmir/providers/calling_convention/arm/arm_conv.cpp
 * @brief Calling convention of ARM architecture.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <memory>
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/calling_convention/arm/arm_conv.h"
#include "retdec/capstone2llvmir/arm/arm.h"
#include "capstone2llvmir/capstone6_compat.h"

namespace retdec {
namespace bin2llvmir {

ArmCallingConvention::ArmCallingConvention(const Abi* a) :
	CallingConvention(a)
{
	_paramRegs = {
		ARM_REG_R0,
		ARM_REG_R1,
		ARM_REG_R2,
		ARM_REG_R3};

	// AAPCS-VFP passes floating-point arguments in s0-s15 for single
	// precision and d0-d7 for double, and returns in s0 or d0. This was the
	// only one of the eleven calling conventions that declared none of them,
	// while still setting _numOfFPRegsPerParam below -- so on hard-float ARM,
	// which is what the distribution toolchain and this project's corpus
	// build, every floating-point parameter and return value was invisible to
	// parameter recovery.
	//
	// Both lists, for the same reason MIPS declares both: arm_init.cpp gives
	// the S and D registers separate globals rather than modelling them as one
	// bank, so a float argument lands in an S global and a double in a D one,
	// and only naming both catches either. The aliasing that is not modelled
	// would matter if a value were written as S and read as D; compiler output
	// for a single argument does not do that.
	//
	// Safe on soft-float ARM too, where floats go in r0-r3: this analysis is
	// driven by which registers a callee actually READS before writing, so a
	// soft-float function simply never reads s0 and nothing is reported. The
	// lists add candidates, they do not assert an ABI. Reading
	// EF_ARM_ABI_FLOAT_HARD to choose between them would be the faithful
	// thing; nothing reads it today, and it is not needed to make this
	// correct.
	_paramFPRegs = {
		ARM_REG_S0,
		ARM_REG_S1,
		ARM_REG_S2,
		ARM_REG_S3,
		ARM_REG_S4,
		ARM_REG_S5,
		ARM_REG_S6,
		ARM_REG_S7,
		ARM_REG_S8,
		ARM_REG_S9,
		ARM_REG_S10,
		ARM_REG_S11,
		ARM_REG_S12,
		ARM_REG_S13,
		ARM_REG_S14,
		ARM_REG_S15};

	_paramDoubleRegs = {ARM_REG_D0, ARM_REG_D1, ARM_REG_D2, ARM_REG_D3, ARM_REG_D4, ARM_REG_D5, ARM_REG_D6, ARM_REG_D7};

	_returnRegs = {
		ARM_REG_R0,
		ARM_REG_R1};

	_returnFPRegs = {ARM_REG_S0};

	_returnDoubleRegs = {ARM_REG_D0};

	// And the short-vector arguments AAPCS-VFP passes in q0-q3, which
	// _numOfVectorRegsPerParam below has always claimed a capacity for
	// without naming a single register.
	_paramVectorRegs = {ARM_REG_Q0, ARM_REG_Q1, ARM_REG_Q2, ARM_REG_Q3};

	_returnVectorRegs = {ARM_REG_Q0};

	_largeObjectsPassedByReference = true;
//	_respectsRegCouples = true;
	_numOfRegsPerParam = 2;
	_numOfFPRegsPerParam = 2;
	_numOfVectorRegsPerParam = 4;
}

CallingConvention::Ptr ArmCallingConvention::create(const Abi* a)
{
	if (!a->isArm())
	{
		return nullptr;
	}

	return std::make_unique<ArmCallingConvention>(a);
}

}
}
