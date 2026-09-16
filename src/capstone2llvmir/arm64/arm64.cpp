/**
 * @file src/capstone2llvmir/arm64/arm64.cpp
 * @brief ARM64 implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <iomanip>

#include "retdec/utils/io/log.h"

#include "capstone2llvmir/arm64/arm64_impl.h"

using namespace retdec::utils::io;

namespace retdec {
namespace capstone2llvmir {

Capstone2LlvmIrTranslatorArm64_impl::Capstone2LlvmIrTranslatorArm64_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(CS_ARCH_ARM64, basic, extra, m)
{
	initialize();
}

//
//==============================================================================
// Mode query & modification methods - from Capstone2LlvmIrTranslator.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorArm64_impl::isAllowedBasicMode(cs_mode m)
{
	return m == CS_MODE_ARM;
}

bool Capstone2LlvmIrTranslatorArm64_impl::isAllowedExtraMode(cs_mode m)
{
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN;
}

uint32_t Capstone2LlvmIrTranslatorArm64_impl::getArchByteSize()
{
	return 8;
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorArm64_impl::generateEnvironmentArchSpecific()
{
	initializeRegistersParentMap();
}

void Capstone2LlvmIrTranslatorArm64_impl::generateDataLayout()
{
	// clang -x c /dev/null -emit-llvm -S -o -
	_module->setDataLayout("e-m:e-i64:64-i128:128-n32:64-S128");
}

void Capstone2LlvmIrTranslatorArm64_impl::generateRegisters()
{
	// FP&SIMD registers
	createRegister(ARM64_REG_V0, _regLt);
	createRegister(ARM64_REG_V1, _regLt);
	createRegister(ARM64_REG_V2, _regLt);
	createRegister(ARM64_REG_V3, _regLt);
	createRegister(ARM64_REG_V4, _regLt);
	createRegister(ARM64_REG_V5, _regLt);
	createRegister(ARM64_REG_V6, _regLt);
	createRegister(ARM64_REG_V7, _regLt);
	createRegister(ARM64_REG_V8, _regLt);
	createRegister(ARM64_REG_V9, _regLt);
	createRegister(ARM64_REG_V10, _regLt);
	createRegister(ARM64_REG_V11, _regLt);
	createRegister(ARM64_REG_V12, _regLt);
	createRegister(ARM64_REG_V13, _regLt);
	createRegister(ARM64_REG_V14, _regLt);
	createRegister(ARM64_REG_V15, _regLt);
	createRegister(ARM64_REG_V16, _regLt);
	createRegister(ARM64_REG_V17, _regLt);
	createRegister(ARM64_REG_V18, _regLt);
	createRegister(ARM64_REG_V19, _regLt);
	createRegister(ARM64_REG_V20, _regLt);
	createRegister(ARM64_REG_V21, _regLt);
	createRegister(ARM64_REG_V22, _regLt);
	createRegister(ARM64_REG_V23, _regLt);
	createRegister(ARM64_REG_V24, _regLt);
	createRegister(ARM64_REG_V25, _regLt);
	createRegister(ARM64_REG_V26, _regLt);
	createRegister(ARM64_REG_V27, _regLt);
	createRegister(ARM64_REG_V28, _regLt);
	createRegister(ARM64_REG_V29, _regLt);
	createRegister(ARM64_REG_V30, _regLt);
	createRegister(ARM64_REG_V31, _regLt);

	createRegister(ARM64_REG_Q0, _regLt);
	createRegister(ARM64_REG_Q1, _regLt);
	createRegister(ARM64_REG_Q2, _regLt);
	createRegister(ARM64_REG_Q3, _regLt);
	createRegister(ARM64_REG_Q4, _regLt);
	createRegister(ARM64_REG_Q5, _regLt);
	createRegister(ARM64_REG_Q6, _regLt);
	createRegister(ARM64_REG_Q7, _regLt);
	createRegister(ARM64_REG_Q8, _regLt);
	createRegister(ARM64_REG_Q9, _regLt);
	createRegister(ARM64_REG_Q10, _regLt);
	createRegister(ARM64_REG_Q11, _regLt);
	createRegister(ARM64_REG_Q12, _regLt);
	createRegister(ARM64_REG_Q13, _regLt);
	createRegister(ARM64_REG_Q14, _regLt);
	createRegister(ARM64_REG_Q15, _regLt);
	createRegister(ARM64_REG_Q16, _regLt);
	createRegister(ARM64_REG_Q17, _regLt);
	createRegister(ARM64_REG_Q18, _regLt);
	createRegister(ARM64_REG_Q19, _regLt);
	createRegister(ARM64_REG_Q20, _regLt);
	createRegister(ARM64_REG_Q21, _regLt);
	createRegister(ARM64_REG_Q22, _regLt);
	createRegister(ARM64_REG_Q23, _regLt);
	createRegister(ARM64_REG_Q24, _regLt);
	createRegister(ARM64_REG_Q25, _regLt);
	createRegister(ARM64_REG_Q26, _regLt);
	createRegister(ARM64_REG_Q27, _regLt);
	createRegister(ARM64_REG_Q28, _regLt);
	createRegister(ARM64_REG_Q29, _regLt);
	createRegister(ARM64_REG_Q30, _regLt);
	createRegister(ARM64_REG_Q31, _regLt);

	createRegister(ARM64_REG_D0, _regLt);
	createRegister(ARM64_REG_D1, _regLt);
	createRegister(ARM64_REG_D2, _regLt);
	createRegister(ARM64_REG_D3, _regLt);
	createRegister(ARM64_REG_D4, _regLt);
	createRegister(ARM64_REG_D5, _regLt);
	createRegister(ARM64_REG_D6, _regLt);
	createRegister(ARM64_REG_D7, _regLt);
	createRegister(ARM64_REG_D8, _regLt);
	createRegister(ARM64_REG_D9, _regLt);
	createRegister(ARM64_REG_D10, _regLt);
	createRegister(ARM64_REG_D11, _regLt);
	createRegister(ARM64_REG_D12, _regLt);
	createRegister(ARM64_REG_D13, _regLt);
	createRegister(ARM64_REG_D14, _regLt);
	createRegister(ARM64_REG_D15, _regLt);
	createRegister(ARM64_REG_D16, _regLt);
	createRegister(ARM64_REG_D17, _regLt);
	createRegister(ARM64_REG_D18, _regLt);
	createRegister(ARM64_REG_D19, _regLt);
	createRegister(ARM64_REG_D20, _regLt);
	createRegister(ARM64_REG_D21, _regLt);
	createRegister(ARM64_REG_D22, _regLt);
	createRegister(ARM64_REG_D23, _regLt);
	createRegister(ARM64_REG_D24, _regLt);
	createRegister(ARM64_REG_D25, _regLt);
	createRegister(ARM64_REG_D26, _regLt);
	createRegister(ARM64_REG_D27, _regLt);
	createRegister(ARM64_REG_D28, _regLt);
	createRegister(ARM64_REG_D29, _regLt);
	createRegister(ARM64_REG_D30, _regLt);
	createRegister(ARM64_REG_D31, _regLt);

	createRegister(ARM64_REG_S0, _regLt);
	createRegister(ARM64_REG_S1, _regLt);
	createRegister(ARM64_REG_S2, _regLt);
	createRegister(ARM64_REG_S3, _regLt);
	createRegister(ARM64_REG_S4, _regLt);
	createRegister(ARM64_REG_S5, _regLt);
	createRegister(ARM64_REG_S6, _regLt);
	createRegister(ARM64_REG_S7, _regLt);
	createRegister(ARM64_REG_S8, _regLt);
	createRegister(ARM64_REG_S9, _regLt);
	createRegister(ARM64_REG_S10, _regLt);
	createRegister(ARM64_REG_S11, _regLt);
	createRegister(ARM64_REG_S12, _regLt);
	createRegister(ARM64_REG_S13, _regLt);
	createRegister(ARM64_REG_S14, _regLt);
	createRegister(ARM64_REG_S15, _regLt);
	createRegister(ARM64_REG_S16, _regLt);
	createRegister(ARM64_REG_S17, _regLt);
	createRegister(ARM64_REG_S18, _regLt);
	createRegister(ARM64_REG_S19, _regLt);
	createRegister(ARM64_REG_S20, _regLt);
	createRegister(ARM64_REG_S21, _regLt);
	createRegister(ARM64_REG_S22, _regLt);
	createRegister(ARM64_REG_S23, _regLt);
	createRegister(ARM64_REG_S24, _regLt);
	createRegister(ARM64_REG_S25, _regLt);
	createRegister(ARM64_REG_S26, _regLt);
	createRegister(ARM64_REG_S27, _regLt);
	createRegister(ARM64_REG_S28, _regLt);
	createRegister(ARM64_REG_S29, _regLt);
	createRegister(ARM64_REG_S30, _regLt);
	createRegister(ARM64_REG_S31, _regLt);

	createRegister(ARM64_REG_H0, _regLt);
	createRegister(ARM64_REG_H1, _regLt);
	createRegister(ARM64_REG_H2, _regLt);
	createRegister(ARM64_REG_H3, _regLt);
	createRegister(ARM64_REG_H4, _regLt);
	createRegister(ARM64_REG_H5, _regLt);
	createRegister(ARM64_REG_H6, _regLt);
	createRegister(ARM64_REG_H7, _regLt);
	createRegister(ARM64_REG_H8, _regLt);
	createRegister(ARM64_REG_H9, _regLt);
	createRegister(ARM64_REG_H10, _regLt);
	createRegister(ARM64_REG_H11, _regLt);
	createRegister(ARM64_REG_H12, _regLt);
	createRegister(ARM64_REG_H13, _regLt);
	createRegister(ARM64_REG_H14, _regLt);
	createRegister(ARM64_REG_H15, _regLt);
	createRegister(ARM64_REG_H16, _regLt);
	createRegister(ARM64_REG_H17, _regLt);
	createRegister(ARM64_REG_H18, _regLt);
	createRegister(ARM64_REG_H19, _regLt);
	createRegister(ARM64_REG_H20, _regLt);
	createRegister(ARM64_REG_H21, _regLt);
	createRegister(ARM64_REG_H22, _regLt);
	createRegister(ARM64_REG_H23, _regLt);
	createRegister(ARM64_REG_H24, _regLt);
	createRegister(ARM64_REG_H25, _regLt);
	createRegister(ARM64_REG_H26, _regLt);
	createRegister(ARM64_REG_H27, _regLt);
	createRegister(ARM64_REG_H28, _regLt);
	createRegister(ARM64_REG_H29, _regLt);
	createRegister(ARM64_REG_H30, _regLt);
	createRegister(ARM64_REG_H31, _regLt);

	createRegister(ARM64_REG_B0, _regLt);
	createRegister(ARM64_REG_B1, _regLt);
	createRegister(ARM64_REG_B2, _regLt);
	createRegister(ARM64_REG_B3, _regLt);
	createRegister(ARM64_REG_B4, _regLt);
	createRegister(ARM64_REG_B5, _regLt);
	createRegister(ARM64_REG_B6, _regLt);
	createRegister(ARM64_REG_B7, _regLt);
	createRegister(ARM64_REG_B8, _regLt);
	createRegister(ARM64_REG_B9, _regLt);
	createRegister(ARM64_REG_B10, _regLt);
	createRegister(ARM64_REG_B11, _regLt);
	createRegister(ARM64_REG_B12, _regLt);
	createRegister(ARM64_REG_B13, _regLt);
	createRegister(ARM64_REG_B14, _regLt);
	createRegister(ARM64_REG_B15, _regLt);
	createRegister(ARM64_REG_B16, _regLt);
	createRegister(ARM64_REG_B17, _regLt);
	createRegister(ARM64_REG_B18, _regLt);
	createRegister(ARM64_REG_B19, _regLt);
	createRegister(ARM64_REG_B20, _regLt);
	createRegister(ARM64_REG_B21, _regLt);
	createRegister(ARM64_REG_B22, _regLt);
	createRegister(ARM64_REG_B23, _regLt);
	createRegister(ARM64_REG_B24, _regLt);
	createRegister(ARM64_REG_B25, _regLt);
	createRegister(ARM64_REG_B26, _regLt);
	createRegister(ARM64_REG_B27, _regLt);
	createRegister(ARM64_REG_B28, _regLt);
	createRegister(ARM64_REG_B29, _regLt);
	createRegister(ARM64_REG_B30, _regLt);
	createRegister(ARM64_REG_B31, _regLt);

	// General purpose registers
	createRegister(ARM64_REG_X0, _regLt);
	createRegister(ARM64_REG_X1, _regLt);
	createRegister(ARM64_REG_X2, _regLt);
	createRegister(ARM64_REG_X3, _regLt);
	createRegister(ARM64_REG_X4, _regLt);
	createRegister(ARM64_REG_X5, _regLt);
	createRegister(ARM64_REG_X6, _regLt);
	createRegister(ARM64_REG_X7, _regLt);
	createRegister(ARM64_REG_X8, _regLt);
	createRegister(ARM64_REG_X9, _regLt);
	createRegister(ARM64_REG_X10, _regLt);
	createRegister(ARM64_REG_X11, _regLt);
	createRegister(ARM64_REG_X12, _regLt);
	createRegister(ARM64_REG_X13, _regLt);
	createRegister(ARM64_REG_X14, _regLt);
	createRegister(ARM64_REG_X15, _regLt);
	createRegister(ARM64_REG_X16, _regLt);
	createRegister(ARM64_REG_X17, _regLt);
	createRegister(ARM64_REG_X18, _regLt);
	createRegister(ARM64_REG_X19, _regLt);
	createRegister(ARM64_REG_X20, _regLt);
	createRegister(ARM64_REG_X21, _regLt);
	createRegister(ARM64_REG_X22, _regLt);
	createRegister(ARM64_REG_X23, _regLt);
	createRegister(ARM64_REG_X24, _regLt);
	createRegister(ARM64_REG_X25, _regLt);
	createRegister(ARM64_REG_X26, _regLt);
	createRegister(ARM64_REG_X27, _regLt);
	createRegister(ARM64_REG_X28, _regLt);

	// Special registers.

	// FP Frame pointer.
	createRegister(ARM64_REG_X29, _regLt);

	// LP Link register.
	createRegister(ARM64_REG_X30, _regLt);

	// Stack pointer.
	createRegister(ARM64_REG_SP, _regLt);

	// Create system & flag registers in this loop
	for (const auto& r : _reg2name)
	{
		createRegister(r.first, _regLt);
	}

}

uint32_t Capstone2LlvmIrTranslatorArm64_impl::getCarryRegister()
{
	return ARM64_REG_CPSR_C;
}

void Capstone2LlvmIrTranslatorArm64_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_arm64* ai = &d->arm64;

	//std::cout << i->mnemonic << " " << i->op_str << std::endl;

	auto fIt = _i2fm.find(i->id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;

		(this->*f)(i, ai, irb);
	}
	else
	{
		generatePseudoInstruction(i, ai, irb);
	}
}

uint32_t Capstone2LlvmIrTranslatorArm64_impl::getParentRegister(uint32_t r) const
{
	try {
		return _reg2parentMap.at(r);
	}
	catch (std::out_of_range &e)
	{
		return r;
	}
}

//
//==============================================================================
// ARM64-specific methods.
//==============================================================================
//

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::getCurrentPc(cs_insn* i)
{
	return llvm::ConstantInt::get(
			getDefaultType(),
			i->address + i->size);
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::extractVectorValue(
		llvm::IRBuilder<>& irb,
		cs_arm64_op& op,
		llvm::Value* val)
{
	if (val->getType() != llvm::IntegerType::getInt128Ty(_module->getContext()))
	{
		return val;
	}

	// A negative vector_index means the operand names the whole register, not
	// one lane of it: `add v0.4s, v1.4s, v2.4s` carries an arrangement but no
	// index. Every lane branch below multiplies vector_index by a lane width
	// and shifts by the result, so -1 became a shift by 2^128-32 -- poison --
	// and then a truncation of that poison to a lane type. For the S
	// arrangements that lane type is `float`, which is how ten of ten ARM64
	// binaries died in "Tried to create an integer operation on a non-integer
	// type" inside translateAdd.
	//
	// Returning the register whole is both defined and, for the bitwise
	// vector instructions (eor/and/orr/mov on .16b), exactly right: those are
	// lane-agnostic. It is not right for the arithmetic ones -- a lanewise
	// add is not a 128-bit add -- and translateAdd and translateSub handle
	// that case themselves rather than quietly emitting the wrong arithmetic.
	if (op.vector_index < 0)
	{
		return val;
	}

	// Vector element size specifier
	switch(op.vas)
	{
		case ARM64_VAS_16B:
		case ARM64_VAS_8B :
		case ARM64_VAS_4B :
		case ARM64_VAS_1B :
			val = irb.CreateLShr(val, llvm::ConstantInt::get(val->getType(), 8 * op.vector_index));
			return irb.CreateZExtOrTrunc(val, llvm::IntegerType::getInt8Ty(_module->getContext()));
		case ARM64_VAS_8H:
		case ARM64_VAS_4H:
		case ARM64_VAS_2H:
		case ARM64_VAS_1H:
			val = irb.CreateLShr(val, llvm::ConstantInt::get(val->getType(), 16 * op.vector_index));
			return irb.CreateZExtOrTrunc(val, llvm::IntegerType::getInt16Ty(_module->getContext()));
		case ARM64_VAS_4S:
		case ARM64_VAS_2S:
		case ARM64_VAS_1S:
			val = irb.CreateLShr(val, llvm::ConstantInt::get(val->getType(), 32 * op.vector_index));
			val = irb.CreateZExtOrTrunc(val, llvm::IntegerType::getInt32Ty(_module->getContext()));
			return irb.CreateBitCast(val, llvm::Type::getFloatTy(_module->getContext()));
		// 2D was the one arrangement of capstone's fifteen this switch did
		// not name, so it reached the throw below -- and that throw is caught
		// nowhere, so a single `v0.2d` operand ended the whole decompilation.
		//
		// It does NOT share the 1D path. 1D ends in a bitcast to double, and
		// routing 2D through it made `add v0.2d, ...` an integer add on a
		// double: "Tried to create an integer operation on a non-integer
		// type", which is how all ten ARM64 binaries failed after the first
		// attempt at this. A .2d lane is 64 bits wide and the arrangement
		// alone does not say whether the instruction reads it as an integer
		// or a double, so this returns the integer lane -- the same choice
		// the VAS_INVALID branch below already makes for `vN.d[i]`, and the
		// one the integer instructions need.
		case ARM64_VAS_2D:
			val = irb.CreateLShr(
				val, llvm::ConstantInt::get(val->getType(), 64u * static_cast<unsigned>(op.vector_index)));
			return irb.CreateZExtOrTrunc(val, llvm::IntegerType::getInt64Ty(_module->getContext()));
		case ARM64_VAS_1D:
			val = irb.CreateLShr(val, llvm::ConstantInt::get(val->getType(), 64 * op.vector_index));
			val = irb.CreateZExtOrTrunc(val, llvm::IntegerType::getInt64Ty(_module->getContext()));
			return irb.CreateBitCast(val, llvm::Type::getDoubleTy(_module->getContext()));
		case ARM64_VAS_1Q:
			val = irb.CreateLShr(val, llvm::ConstantInt::get(val->getType(), 128 * op.vector_index));
			val = irb.CreateZExtOrTrunc(val, llvm::IntegerType::getInt128Ty(_module->getContext()));
			return irb.CreateBitCast(val, llvm::Type::getFP128Ty(_module->getContext()));
		case ARM64_VAS_INVALID:
			// Newer Capstone may leave VAS unset on `vN.d[i]`; still extract a D lane.
			if (op.vector_index >= 0)
			{
				val = irb.CreateLShr(val, llvm::ConstantInt::get(
					val->getType(), 64u * static_cast<unsigned>(op.vector_index)));
				return irb.CreateZExtOrTrunc(
					val, llvm::IntegerType::getInt64Ty(_module->getContext()));
			}
			return val;
		default:
			throw GenericError("Arm64: extractVectorValue(): Unknown VESS type");
	}

	return val;
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateOperandExtension(
		llvm::IRBuilder<>& irb,
		arm64_extender ext,
		llvm::Value* val,
		llvm::Type* destType)
{
	auto* i8  = llvm::IntegerType::getInt8Ty(_module->getContext());
	auto* i16 = llvm::IntegerType::getInt16Ty(_module->getContext());
	auto* i32 = llvm::IntegerType::getInt32Ty(_module->getContext());

	auto* ty  = destType ? destType : getDefaultType();

	llvm::Value* trunc = nullptr;
	switch(ext)
	{
		case ARM64_EXT_INVALID:
		{
			return val;
		}
		case ARM64_EXT_UXTB:
		{
			trunc = irb.CreateTrunc(val, i8);
			return irb.CreateZExt(trunc, ty);
		}
		case ARM64_EXT_UXTH:
		{
			trunc = irb.CreateTrunc(val, i16);
			return irb.CreateZExt(trunc, ty);
		}
		case ARM64_EXT_UXTW:
		{
			trunc = irb.CreateTrunc(val, i32);
			return irb.CreateZExt(trunc, ty);
		}
		case ARM64_EXT_UXTX:
		{
			trunc = irb.CreateTrunc(val, i32);
			return irb.CreateZExt(trunc, ty);
		}
		case ARM64_EXT_SXTB:
		{
			trunc = irb.CreateTrunc(val, i8);
			return irb.CreateSExt(trunc, ty);
		}
		case ARM64_EXT_SXTH:
		{
			trunc = irb.CreateTrunc(val, i16);
			return irb.CreateSExt(trunc, ty);
		}
		case ARM64_EXT_SXTW:
		{
			trunc = irb.CreateTrunc(val, i32);
			return irb.CreateSExt(trunc, ty);
		}
		case ARM64_EXT_SXTX:
		{
			trunc = irb.CreateTrunc(val, i32);
			return irb.CreateSExt(trunc, ty);
		}
		default:
			throw GenericError("Arm64: generateOperandExtension(): Unsupported extension type");
	}
	return val;
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateOperandShift(
		llvm::IRBuilder<>& irb,
		cs_arm64_op& op,
		llvm::Value* val,
		bool updateFlags)
{
	llvm::Value* n = nullptr;
	if (op.shift.type == ARM64_SFT_INVALID)
	{
		return val;
	}
	else
	{
		n = llvm::ConstantInt::get(val->getType(), op.shift.value);
	}

	if (n == nullptr)
	{
		throw GenericError("generateOperandShift(): nullptr shift value");
	}

	n = irb.CreateZExtOrTrunc(n, val->getType());

	switch (op.shift.type)
	{
		case ARM64_SFT_ASR:
		{
			return generateShiftAsr(irb, val, n, updateFlags);
		}
		case ARM64_SFT_LSL:
		{
			return generateShiftLsl(irb, val, n, updateFlags);
		}
		case ARM64_SFT_LSR:
		{
			return generateShiftLsr(irb, val, n, updateFlags);
		}
		case ARM64_SFT_ROR:
		{
			return generateShiftRor(irb, val, n, updateFlags);
		}
		case ARM64_SFT_MSL:
		{
			return generateShiftMsl(irb, val, n, updateFlags);
		}
		case ARM64_SFT_INVALID:
		default:
		{
			return val;
		}
	}
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateShiftAsr(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value *n,
		bool updateFlags)
{
	if (updateFlags)
	{
		auto* cfOp1 = irb.CreateSub(n, llvm::ConstantInt::get(n->getType(), 1));
		auto* cfShl = irb.CreateShl(llvm::ConstantInt::get(cfOp1->getType(), 1), cfOp1);
		auto* cfAnd = irb.CreateAnd(cfShl, val);
		auto* cfIcmp = irb.CreateICmpNE(cfAnd, llvm::ConstantInt::get(cfAnd->getType(), 0));
		storeRegister(ARM64_REG_CPSR_C, cfIcmp, irb);
	}
	return irb.CreateAShr(val, n);
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateShiftLsl(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value *n,
		bool updateFlags)
{
	if (updateFlags)
	{
		auto* cfOp1 = irb.CreateSub(n, llvm::ConstantInt::get(n->getType(), 1));
		auto* cfShl = irb.CreateShl(val, cfOp1);
		auto* cfIntT = llvm::cast<llvm::IntegerType>(cfShl->getType());
		auto* cfRightCount = llvm::ConstantInt::get(cfIntT, cfIntT->getBitWidth() - 1);
		auto* cfLow = irb.CreateLShr(cfShl, cfRightCount);
		storeRegister(ARM64_REG_CPSR_C, cfLow, irb);
	}
	return irb.CreateShl(val, n);
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateShiftLsr(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value *n,
		bool updateFlags)
{
	if (updateFlags)
	{
		auto* cfOp1 = irb.CreateSub(n, llvm::ConstantInt::get(n->getType(), 1));
		auto* cfShl = irb.CreateShl(llvm::ConstantInt::get(cfOp1->getType(), 1), cfOp1);
		auto* cfAnd = irb.CreateAnd(cfShl, val);
		auto* cfIcmp = irb.CreateICmpNE(cfAnd, llvm::ConstantInt::get(cfAnd->getType(), 0));
		storeRegister(ARM64_REG_CPSR_C, cfIcmp, irb);
	}

	return irb.CreateLShr(val, n);
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateShiftRor(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value *n,
		bool updateFlags)
{
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(n->getType())->getBitWidth();

	auto* srl = irb.CreateLShr(val, n);
	auto* sub = irb.CreateSub(llvm::ConstantInt::get(n->getType(), op0BitW), n);
	auto* shl = irb.CreateShl(val, sub);
	auto* orr = irb.CreateOr(srl, shl);
	if (updateFlags)
	{

		auto* cfSrl = irb.CreateLShr(orr, llvm::ConstantInt::get(orr->getType(), op0BitW - 1));
		auto* cfIcmp = irb.CreateICmpNE(cfSrl, llvm::ConstantInt::get(cfSrl->getType(), 0));
		storeRegister(ARM64_REG_CPSR_C, cfIcmp, irb);
	}

	return orr;
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateShiftMsl(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value *n,
		bool updateFlags)
{
	return val;
// 	unsigned op0BitW = llvm::cast<llvm::IntegerType>(n->getType())->getBitWidth();
// 	auto* doubleT = llvm::Type::getIntNTy(_module->getContext(), op0BitW*2);

// 	auto* cf = loadRegister(ARM64_REG_CPSR_C, irb);
// 	cf = irb.CreateZExtOrTrunc(cf, n->getType());

// 	auto* srl = irb.CreateLShr(val, n);
// 	auto* srlZext = irb.CreateZExt(srl, doubleT);
// 	auto* op0Zext = irb.CreateZExt(val, doubleT);
// 	auto* sub = irb.CreateSub(llvm::ConstantInt::get(n->getType(), op0BitW + 1), n);
// 	auto* subZext = irb.CreateZExt(sub, doubleT);
// 	auto* shl = irb.CreateShl(op0Zext, subZext);
// 	auto* sub2 = irb.CreateSub(llvm::ConstantInt::get(n->getType(), op0BitW), n);
// 	auto* shl2 = irb.CreateShl(cf, sub2);
// 	auto* shl2Zext = irb.CreateZExt(shl2, doubleT);
// 	auto* or1 = irb.CreateOr(shl, srlZext);
// 	auto* or2 = irb.CreateOr(or1, shl2Zext);
// 	auto* or2Trunc = irb.CreateTrunc(or2, val->getType());

// 	auto* sub3 = irb.CreateSub(n, llvm::ConstantInt::get(n->getType(), 1));
// 	auto* shl3 = irb.CreateShl(llvm::ConstantInt::get(sub3->getType(), 1), sub3);
// 	auto* and1 = irb.CreateAnd(shl3, val);
// 	auto* cfIcmp = irb.CreateICmpNE(and1, llvm::ConstantInt::get(and1->getType(), 0));
// 	storeRegister(ARM64_REG_CPSR_C, cfIcmp, irb);

// 	return or2Trunc;
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateGetOperandMemAddr(
		cs_arm64_op& op,
		llvm::IRBuilder<>& irb)
{
	auto* baseR = loadRegister(op.mem.base, irb);
	auto* t = baseR ? baseR->getType() : getDefaultType();
	llvm::Value* disp = op.mem.disp
			? llvm::ConstantInt::getSigned(t, op.mem.disp)
			: nullptr;

	auto* idxR = loadRegister(op.mem.index, irb);
	if (idxR)
	{
		idxR = generateOperandShift(irb, op, idxR);
	}

	llvm::Value* addr = nullptr;
	if (baseR && disp == nullptr)
	{
		addr = baseR;
	}
	else if (disp && baseR == nullptr)
	{
		addr = disp;
	}
	else if (baseR && disp)
	{
		disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
		addr = irb.CreateAdd(baseR, disp);
	}
	else if (idxR)
	{
		addr = idxR;
	}
	else
	{
		addr = llvm::ConstantInt::get(getDefaultType(), 0);
	}

	if (idxR && addr != idxR)
	{
		idxR = irb.CreateZExtOrTrunc(idxR, addr->getType());
		addr = irb.CreateAdd(addr, idxR);
	}
	return addr;
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == ARM64_REG_INVALID)
	{
		return nullptr;
	}

	// There is no such instruction that can access PC, but in case
	// it happens somewhere in LLVM, we should be able to handle it.
	if (r == ARM64_REG_PC)
	{
		return getCurrentPc(_insn);
	}

	llvm::Type* rt = nullptr;
	try
	{
		rt = getRegisterType(r);
	}
	catch (GenericError &e)
	{
		// If we dont find the register type, try to recover from this returning at
		// least the number of register
		// Maybe solve this better
		Log::error() << e.what() << std::endl;
		return llvm::ConstantInt::get(dstType ? dstType : getDefaultType(), r);
	}

	if (r == ARM64_REG_XZR || r == ARM64_REG_WZR)
	{
		// Loads from XZR registers generate zero
		return llvm::ConstantInt::get(rt, 0);
	}

	auto pr = getParentRegister(r);
	auto* llvmReg = getRegister(pr);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvm::Value* ret = createLoad(irb, llvmReg);
	if (r != pr)
	{
		ret = irb.CreateTrunc(ret, rt);
	}

	ret = generateTypeConversion(irb, ret, dstType, ct);
	return ret;
}

//
//==============================================================================
// ARMv8.1 LSE atomics.
//==============================================================================
//
// 124 instruction ids -- LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN, LDUMAX,
// LDUMIN, SWP and CAS, each with four ordering suffixes and three widths --
// and the ARM64 table had an entry for NONE of them, not even nullptr. Not a
// translator that declined to model them: ids the dispatch could never reach.
// LDAXR and STLXR, the older load/store-exclusive pair, are implemented.
//
// A compiler targeting armv8.1-a or later emits these for every atomic
// operation, so a binary built that way had an opaque __asm_* call where x86
// has real IR: x86 translates LOCK XADD and CMPXCHG to atomicrmw and cmpxchg,
// and llvmir2hll converts both. This is the same path, and nothing about it is
// new -- only ARM64's use of it.
//
// All three operands are uniform across the family, measured with capstone
// 5.0.9 against encodings from aarch64-linux-gnu-as (keystone 0.9.2 predates
// LSE and refuses every one of them):
//
//     op0 = Rs, the value operand        op1 = Rt, the destination
//     op2 = [Xn], the memory operand
//
// CAS is the exception worth naming: it writes the old value back to **Rs**,
// not to Rt. Rt is the desired value.

llvm::AtomicOrdering Capstone2LlvmIrTranslatorArm64_impl::lseOrdering(unsigned id)
{
	switch (id)
	{
	// Acquire.
	case ARM64_INS_LDADDA:
	case ARM64_INS_LDADDAB:
	case ARM64_INS_LDADDAH:
	case ARM64_INS_LDCLRA:
	case ARM64_INS_LDCLRAB:
	case ARM64_INS_LDCLRAH:
	case ARM64_INS_LDEORA:
	case ARM64_INS_LDEORAB:
	case ARM64_INS_LDEORAH:
	case ARM64_INS_LDSETA:
	case ARM64_INS_LDSETAB:
	case ARM64_INS_LDSETAH:
	case ARM64_INS_LDSMAXA:
	case ARM64_INS_LDSMAXAB:
	case ARM64_INS_LDSMAXAH:
	case ARM64_INS_LDSMINA:
	case ARM64_INS_LDSMINAB:
	case ARM64_INS_LDSMINAH:
	case ARM64_INS_LDUMAXA:
	case ARM64_INS_LDUMAXAB:
	case ARM64_INS_LDUMAXAH:
	case ARM64_INS_LDUMINA:
	case ARM64_INS_LDUMINAB:
	case ARM64_INS_LDUMINAH:
	case ARM64_INS_SWPA:
	case ARM64_INS_SWPAB:
	case ARM64_INS_SWPAH:
	case ARM64_INS_CASA:
	case ARM64_INS_CASAB:
	case ARM64_INS_CASAH: return llvm::AtomicOrdering::Acquire;
	// Release.
	case ARM64_INS_LDADDL:
	case ARM64_INS_LDADDLB:
	case ARM64_INS_LDADDLH:
	case ARM64_INS_LDCLRL:
	case ARM64_INS_LDCLRLB:
	case ARM64_INS_LDCLRLH:
	case ARM64_INS_LDEORL:
	case ARM64_INS_LDEORLB:
	case ARM64_INS_LDEORLH:
	case ARM64_INS_LDSETL:
	case ARM64_INS_LDSETLB:
	case ARM64_INS_LDSETLH:
	case ARM64_INS_LDSMAXL:
	case ARM64_INS_LDSMAXLB:
	case ARM64_INS_LDSMAXLH:
	case ARM64_INS_LDSMINL:
	case ARM64_INS_LDSMINLB:
	case ARM64_INS_LDSMINLH:
	case ARM64_INS_LDUMAXL:
	case ARM64_INS_LDUMAXLB:
	case ARM64_INS_LDUMAXLH:
	case ARM64_INS_LDUMINL:
	case ARM64_INS_LDUMINLB:
	case ARM64_INS_LDUMINLH:
	case ARM64_INS_SWPL:
	case ARM64_INS_SWPLB:
	case ARM64_INS_SWPLH:
	case ARM64_INS_CASL:
	case ARM64_INS_CASLB:
	case ARM64_INS_CASLH: return llvm::AtomicOrdering::Release;
	// Acquire-release.
	case ARM64_INS_LDADDAL:
	case ARM64_INS_LDADDALB:
	case ARM64_INS_LDADDALH:
	case ARM64_INS_LDCLRAL:
	case ARM64_INS_LDCLRALB:
	case ARM64_INS_LDCLRALH:
	case ARM64_INS_LDEORAL:
	case ARM64_INS_LDEORALB:
	case ARM64_INS_LDEORALH:
	case ARM64_INS_LDSETAL:
	case ARM64_INS_LDSETALB:
	case ARM64_INS_LDSETALH:
	case ARM64_INS_LDSMAXAL:
	case ARM64_INS_LDSMAXALB:
	case ARM64_INS_LDSMAXALH:
	case ARM64_INS_LDSMINAL:
	case ARM64_INS_LDSMINALB:
	case ARM64_INS_LDSMINALH:
	case ARM64_INS_LDUMAXAL:
	case ARM64_INS_LDUMAXALB:
	case ARM64_INS_LDUMAXALH:
	case ARM64_INS_LDUMINAL:
	case ARM64_INS_LDUMINALB:
	case ARM64_INS_LDUMINALH:
	case ARM64_INS_SWPAL:
	case ARM64_INS_SWPALB:
	case ARM64_INS_SWPALH:
	case ARM64_INS_CASAL:
	case ARM64_INS_CASALB:
	case ARM64_INS_CASALH: return llvm::AtomicOrdering::AcquireRelease;
	default: return llvm::AtomicOrdering::Monotonic;
	}
}

/**
 * The access width, which comes from the mnemonic suffix and NOT from the
 * register: `ldaddb w1, w2, [x3]` names W registers and touches one byte.
 */
llvm::Type* Capstone2LlvmIrTranslatorArm64_impl::lseAccessType(unsigned id, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	switch (id)
	{
	case ARM64_INS_LDADDB:
	case ARM64_INS_LDADDAB:
	case ARM64_INS_LDADDLB:
	case ARM64_INS_LDADDALB:
	case ARM64_INS_LDCLRB:
	case ARM64_INS_LDCLRAB:
	case ARM64_INS_LDCLRLB:
	case ARM64_INS_LDCLRALB:
	case ARM64_INS_LDEORB:
	case ARM64_INS_LDEORAB:
	case ARM64_INS_LDEORLB:
	case ARM64_INS_LDEORALB:
	case ARM64_INS_LDSETB:
	case ARM64_INS_LDSETAB:
	case ARM64_INS_LDSETLB:
	case ARM64_INS_LDSETALB:
	case ARM64_INS_LDSMAXB:
	case ARM64_INS_LDSMAXAB:
	case ARM64_INS_LDSMAXLB:
	case ARM64_INS_LDSMAXALB:
	case ARM64_INS_LDSMINB:
	case ARM64_INS_LDSMINAB:
	case ARM64_INS_LDSMINLB:
	case ARM64_INS_LDSMINALB:
	case ARM64_INS_LDUMAXB:
	case ARM64_INS_LDUMAXAB:
	case ARM64_INS_LDUMAXLB:
	case ARM64_INS_LDUMAXALB:
	case ARM64_INS_LDUMINB:
	case ARM64_INS_LDUMINAB:
	case ARM64_INS_LDUMINLB:
	case ARM64_INS_LDUMINALB:
	case ARM64_INS_SWPB:
	case ARM64_INS_SWPAB:
	case ARM64_INS_SWPLB:
	case ARM64_INS_SWPALB:
	case ARM64_INS_CASB:
	case ARM64_INS_CASAB:
	case ARM64_INS_CASLB:
	case ARM64_INS_CASALB: return irb.getInt8Ty();
	case ARM64_INS_LDADDH:
	case ARM64_INS_LDADDAH:
	case ARM64_INS_LDADDLH:
	case ARM64_INS_LDADDALH:
	case ARM64_INS_LDCLRH:
	case ARM64_INS_LDCLRAH:
	case ARM64_INS_LDCLRLH:
	case ARM64_INS_LDCLRALH:
	case ARM64_INS_LDEORH:
	case ARM64_INS_LDEORAH:
	case ARM64_INS_LDEORLH:
	case ARM64_INS_LDEORALH:
	case ARM64_INS_LDSETH:
	case ARM64_INS_LDSETAH:
	case ARM64_INS_LDSETLH:
	case ARM64_INS_LDSETALH:
	case ARM64_INS_LDSMAXH:
	case ARM64_INS_LDSMAXAH:
	case ARM64_INS_LDSMAXLH:
	case ARM64_INS_LDSMAXALH:
	case ARM64_INS_LDSMINH:
	case ARM64_INS_LDSMINAH:
	case ARM64_INS_LDSMINLH:
	case ARM64_INS_LDSMINALH:
	case ARM64_INS_LDUMAXH:
	case ARM64_INS_LDUMAXAH:
	case ARM64_INS_LDUMAXLH:
	case ARM64_INS_LDUMAXALH:
	case ARM64_INS_LDUMINH:
	case ARM64_INS_LDUMINAH:
	case ARM64_INS_LDUMINLH:
	case ARM64_INS_LDUMINALH:
	case ARM64_INS_SWPH:
	case ARM64_INS_SWPAH:
	case ARM64_INS_SWPLH:
	case ARM64_INS_SWPALH:
	case ARM64_INS_CASH:
	case ARM64_INS_CASAH:
	case ARM64_INS_CASLH:
	case ARM64_INS_CASALH: return irb.getInt16Ty();
	default: break;
	}
	// No suffix: the width is the register's, W or X.
	if (ai->op_count > 0 && ai->operands[0].type == ARM64_OP_REG && ai->operands[0].reg >= ARM64_REG_X0
		&& ai->operands[0].reg <= ARM64_REG_X28)
	{
		return irb.getInt64Ty();
	}
	return irb.getInt32Ty();
}

/**
 * ARM64_INS_LDADD and the rest of the read-modify-write family.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateLse(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ai->operands[2].type != ARM64_OP_MEM)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::AtomicRMWInst::BinOp op;
	bool complementValue = false;
	switch (i->id)
	{
	case ARM64_INS_LDADD:
	case ARM64_INS_LDADDA:
	case ARM64_INS_LDADDL:
	case ARM64_INS_LDADDAL:
	case ARM64_INS_LDADDB:
	case ARM64_INS_LDADDAB:
	case ARM64_INS_LDADDLB:
	case ARM64_INS_LDADDALB:
	case ARM64_INS_LDADDH:
	case ARM64_INS_LDADDAH:
	case ARM64_INS_LDADDLH:
	case ARM64_INS_LDADDALH: op = llvm::AtomicRMWInst::Add; break;
	// LDCLR clears the bits SET in the operand, so the operand is
	// complemented and the operation is an AND. Getting this one wrong by
	// treating it as a plain AND would clear exactly the wrong bits.
	case ARM64_INS_LDCLR:
	case ARM64_INS_LDCLRA:
	case ARM64_INS_LDCLRL:
	case ARM64_INS_LDCLRAL:
	case ARM64_INS_LDCLRB:
	case ARM64_INS_LDCLRAB:
	case ARM64_INS_LDCLRLB:
	case ARM64_INS_LDCLRALB:
	case ARM64_INS_LDCLRH:
	case ARM64_INS_LDCLRAH:
	case ARM64_INS_LDCLRLH:
	case ARM64_INS_LDCLRALH:
		op = llvm::AtomicRMWInst::And;
		complementValue = true;
		break;
	case ARM64_INS_LDEOR:
	case ARM64_INS_LDEORA:
	case ARM64_INS_LDEORL:
	case ARM64_INS_LDEORAL:
	case ARM64_INS_LDEORB:
	case ARM64_INS_LDEORAB:
	case ARM64_INS_LDEORLB:
	case ARM64_INS_LDEORALB:
	case ARM64_INS_LDEORH:
	case ARM64_INS_LDEORAH:
	case ARM64_INS_LDEORLH:
	case ARM64_INS_LDEORALH: op = llvm::AtomicRMWInst::Xor; break;
	case ARM64_INS_LDSET:
	case ARM64_INS_LDSETA:
	case ARM64_INS_LDSETL:
	case ARM64_INS_LDSETAL:
	case ARM64_INS_LDSETB:
	case ARM64_INS_LDSETAB:
	case ARM64_INS_LDSETLB:
	case ARM64_INS_LDSETALB:
	case ARM64_INS_LDSETH:
	case ARM64_INS_LDSETAH:
	case ARM64_INS_LDSETLH:
	case ARM64_INS_LDSETALH: op = llvm::AtomicRMWInst::Or; break;
	case ARM64_INS_LDSMAX:
	case ARM64_INS_LDSMAXA:
	case ARM64_INS_LDSMAXL:
	case ARM64_INS_LDSMAXAL:
	case ARM64_INS_LDSMAXB:
	case ARM64_INS_LDSMAXAB:
	case ARM64_INS_LDSMAXLB:
	case ARM64_INS_LDSMAXALB:
	case ARM64_INS_LDSMAXH:
	case ARM64_INS_LDSMAXAH:
	case ARM64_INS_LDSMAXLH:
	case ARM64_INS_LDSMAXALH: op = llvm::AtomicRMWInst::Max; break;
	case ARM64_INS_LDSMIN:
	case ARM64_INS_LDSMINA:
	case ARM64_INS_LDSMINL:
	case ARM64_INS_LDSMINAL:
	case ARM64_INS_LDSMINB:
	case ARM64_INS_LDSMINAB:
	case ARM64_INS_LDSMINLB:
	case ARM64_INS_LDSMINALB:
	case ARM64_INS_LDSMINH:
	case ARM64_INS_LDSMINAH:
	case ARM64_INS_LDSMINLH:
	case ARM64_INS_LDSMINALH: op = llvm::AtomicRMWInst::Min; break;
	case ARM64_INS_LDUMAX:
	case ARM64_INS_LDUMAXA:
	case ARM64_INS_LDUMAXL:
	case ARM64_INS_LDUMAXAL:
	case ARM64_INS_LDUMAXB:
	case ARM64_INS_LDUMAXAB:
	case ARM64_INS_LDUMAXLB:
	case ARM64_INS_LDUMAXALB:
	case ARM64_INS_LDUMAXH:
	case ARM64_INS_LDUMAXAH:
	case ARM64_INS_LDUMAXLH:
	case ARM64_INS_LDUMAXALH: op = llvm::AtomicRMWInst::UMax; break;
	case ARM64_INS_LDUMIN:
	case ARM64_INS_LDUMINA:
	case ARM64_INS_LDUMINL:
	case ARM64_INS_LDUMINAL:
	case ARM64_INS_LDUMINB:
	case ARM64_INS_LDUMINAB:
	case ARM64_INS_LDUMINLB:
	case ARM64_INS_LDUMINALB:
	case ARM64_INS_LDUMINH:
	case ARM64_INS_LDUMINAH:
	case ARM64_INS_LDUMINLH:
	case ARM64_INS_LDUMINALH: op = llvm::AtomicRMWInst::UMin; break;
	case ARM64_INS_SWP:
	case ARM64_INS_SWPA:
	case ARM64_INS_SWPL:
	case ARM64_INS_SWPAL:
	case ARM64_INS_SWPB:
	case ARM64_INS_SWPAB:
	case ARM64_INS_SWPLB:
	case ARM64_INS_SWPALB:
	case ARM64_INS_SWPH:
	case ARM64_INS_SWPAH:
	case ARM64_INS_SWPLH:
	case ARM64_INS_SWPALH: op = llvm::AtomicRMWInst::Xchg; break;
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}

	auto* elem = lseAccessType(i->id, ai, irb);
	auto* addr = generateGetOperandMemAddr(ai->operands[2], irb);
	auto* ptr = intToPtr(irb, addr, elem);

	auto* val = loadOp(ai->operands[0], irb);
	val = generateTypeConversion(irb, val, elem, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	if (complementValue)
	{
		val = irb.CreateNot(val);
	}

	auto* old = irb.CreateAtomicRMW(op, ptr, val, llvm::MaybeAlign(), lseOrdering(i->id));
	attachPointeeType(old, elem);

	storeOp(ai->operands[1], old, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * ARM64_INS_CAS and its ordering and width variants.
 *
 * `cas Rs, Rt, [Xn]` compares the memory against Rs, stores Rt if they match,
 * and writes the ORIGINAL memory value back to Rs. The destination is the
 * first operand, not the second -- the opposite of every instruction above.
 *
 * CASP, the 128-bit register-pair form, is not here: it needs a pair of
 * registers on each side and LLVM's cmpxchg does not take one.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateCas(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ai->operands[2].type != ARM64_OP_MEM)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* elem = lseAccessType(i->id, ai, irb);
	auto* addr = generateGetOperandMemAddr(ai->operands[2], irb);
	auto* ptr = intToPtr(irb, addr, elem);

	auto* expected = loadOp(ai->operands[0], irb);
	auto* desired = loadOp(ai->operands[1], irb);
	expected = generateTypeConversion(irb, expected, elem, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	desired = generateTypeConversion(irb, desired, elem, eOpConv::ZEXT_TRUNC_OR_BITCAST);

	auto ord = lseOrdering(i->id);
	// cmpxchg's failure ordering may not be stronger than its success
	// ordering, and may not be Release or AcquireRelease at all.
	auto failOrd = (ord == llvm::AtomicOrdering::Release || ord == llvm::AtomicOrdering::AcquireRelease)
					 ? llvm::AtomicOrdering::Monotonic
					 : ord;

	auto* cx = irb.CreateAtomicCmpXchg(ptr, expected, desired, llvm::MaybeAlign(), ord, failOrd);
	attachPointeeType(cx, elem);

	storeOp(ai->operands[0], irb.CreateExtractValue(cx, 0), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * The number of bytes a NEON list operand moves, from its arrangement, or 0
 * for the lane arrangements that name a single element rather than a register.
 */
static unsigned vasByteWidth(arm64_vas vas)
{
	switch (vas)
	{
	case ARM64_VAS_16B:
	case ARM64_VAS_8H:
	case ARM64_VAS_4S:
	case ARM64_VAS_2D:
	case ARM64_VAS_1Q: return 16;
	case ARM64_VAS_8B:
	case ARM64_VAS_4H:
	case ARM64_VAS_2S:
	case ARM64_VAS_1D: return 8;
	default: return 0;
	}
}

/**
 * ARM64_INS_MRS, ARM64_INS_MSR -- move from/to a system register.
 *
 * This translator already creates a global for every AArch64 system register
 * Capstone knows: `arm64_init.cpp` lists all of them by name and by type,
 * `tpidr_el0` among them. What was missing was the instruction that reads one.
 *
 * MRS is 12,400 occurrences in the static parity corpus, and 275 of every 293
 * are `mrs xN, tpidr_el0` -- the thread pointer, how glibc finds thread-local
 * storage. The rest read FPSR, FPCR, DCZID_EL0, CTR_EL0 and MIDR_EL1, and
 * those have registers here too, so all of them translate.
 *
 * The system-register operand cannot go through loadOp(). Capstone's operand
 * union aliases `sys` onto `reg`, so ARM64_OP_SYS values collide numerically
 * with ordinary register ids -- loadOp() refuses them by design for exactly
 * that reason. The id is read out of `.sys` and looked up directly, and an id
 * this translator has no register for falls back to the pseudo-assembly call
 * rather than inventing one.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateSysRegMove(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	bool read = i->id == ARM64_INS_MRS;
	cs_arm64_op& sysOp = read ? ai->operands[1] : ai->operands[0];
	cs_arm64_op& gprOp = read ? ai->operands[0] : ai->operands[1];

	if (sysOp.type != ARM64_OP_SYS || gprOp.type != ARM64_OP_REG)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* sysReg = getRegister(sysOp.sys);
	if (sysReg == nullptr)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	if (read)
	{
		storeOp(gprOp, loadRegister(sysOp.sys, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	else
	{
		storeRegister(sysOp.sys, loadOp(gprOp, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
}

/**
 * Lane width in bits and lane count for a NEON arrangement. {0, 0} means an
 * arrangement this file does not model -- including the ones Capstone reports
 * as ARM64_VAS_INVALID for a scalar or lane-indexed operand, where there is no
 * arrangement to read.
 */
static std::pair<unsigned, unsigned> vasLanes(arm64_vas vas)
{
	switch (vas)
	{
	case ARM64_VAS_16B: return {8, 16};
	case ARM64_VAS_8B: return {8, 8};
	case ARM64_VAS_8H: return {16, 8};
	case ARM64_VAS_4H: return {16, 4};
	case ARM64_VAS_4S: return {32, 4};
	case ARM64_VAS_2S: return {32, 2};
	case ARM64_VAS_2D: return {64, 2};
	case ARM64_VAS_1D: return {64, 1};
	default: return {0, 0};
	}
}

/**
 * True if every operand in [0, n) is a plain V register with a modelled
 * arrangement of the same total width, and none of them is lane-indexed.
 * Writes that width, in bytes, through \p bytes.
 */
bool Capstone2LlvmIrTranslatorArm64_impl::neonSameWidthRegs(cs_arm64* ai, unsigned n, unsigned& bytes)
{
	bytes = 0;
	for (unsigned j = 0; j < n; ++j)
	{
		auto& op = ai->operands[j];
		unsigned b = vasByteWidth(op.vas);
		if (!isVectorRegister(op) || op.vector_index >= 0 || b == 0 || (bytes != 0 && b != bytes))
		{
			return false;
		}
		bytes = b;
	}
	return bytes != 0;
}

/**
 * ARM64_INS_EXT -- `ext vd.<T>, vn.<T>, vm.<T>, #index`
 *
 * Concatenate vm:vn and take <T>'s width of bytes starting `index` in from the
 * bottom. It is x86's PALIGNR with the operands the other way round: there the
 * destination is the high half, here the second source is, so copying that
 * translation across would answer with the two halves swapped for every index
 * except zero.
 *
 * 5,124 occurrences in the static corpus, the most frequent thing on ARM64
 * that is neither a system register read nor a supervisor call. It is what
 * every hand-written NEON `memcpy` tail uses to realign a vector.
 *
 * Done on the arrangement's own width rather than always on 128 bits: for an
 * 8B arrangement the operation is 64-bit and the write zeroes the upper half
 * of the register, which is what every D-form write does. The shifts are split
 * by case in C++ for the same reason PALIGNR's are -- `index == 0` would
 * otherwise be a shift by exactly the operand width, which is poison.
 *
 * `index >= width` is architecturally UNDEFINED rather than zero, so it goes
 * to the pseudo-asm path instead of being given an answer.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateNeonExt(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count == 4));

	unsigned bytes = 0;
	if (!neonSameWidthRegs(ai, 3, bytes) || ai->operands[3].type != ARM64_OP_IMM || ai->operands[3].imm < 0
		|| static_cast<uint64_t>(ai->operands[3].imm) >= bytes)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned n = static_cast<unsigned>(ai->operands[3].imm);
	auto* w = irb.getIntNTy(bytes * 8);
	llvm::Value* vn = irb.CreateZExtOrTrunc(loadRegister(ai->operands[1].reg, irb), w);
	llvm::Value* vm = irb.CreateZExtOrTrunc(loadRegister(ai->operands[2].reg, irb), w);

	llvm::Value* res = n == 0 ? vn
							  : irb.CreateOr(
									irb.CreateLShr(vn, llvm::ConstantInt::get(w, n * 8)),
									irb.CreateShl(vm, llvm::ConstantInt::get(w, bytes * 8 - n * 8)));

	storeRegister(ai->operands[0].reg, res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * ARM64_INS_BSL, ARM64_INS_BIT, ARM64_INS_BIF
 *
 * The three bitwise selects, and the only NEON data-processing instructions
 * here that need no lane model at all: they are bit-for-bit, so the
 * arrangement decides the width and nothing else.
 *
 *     BSL vd, vn, vm    vd = (vn & vd) | (vm & ~vd)   vd is the selector
 *     BIT vd, vn, vm    vd = (vd & ~vm) | (vn & vm)   vm is the mask
 *     BIF vd, vn, vm    vd = (vd & vm) | (vn & ~vm)   the same, inverted
 *
 * All three read the destination, and which of the three registers is the
 * selector is the whole difference between them. BSL uses the destination;
 * BIT and BIF use the second source.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateNeonBitSel(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	unsigned bytes = 0;
	if (!neonSameWidthRegs(ai, 3, bytes))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* w = irb.getIntNTy(bytes * 8);
	llvm::Value* d = irb.CreateZExtOrTrunc(loadRegister(ai->operands[0].reg, irb), w);
	llvm::Value* n = irb.CreateZExtOrTrunc(loadRegister(ai->operands[1].reg, irb), w);
	llvm::Value* m = irb.CreateZExtOrTrunc(loadRegister(ai->operands[2].reg, irb), w);

	llvm::Value* res = nullptr;
	switch (i->id)
	{
	case ARM64_INS_BSL: res = irb.CreateOr(irb.CreateAnd(n, d), irb.CreateAnd(m, irb.CreateNot(d))); break;
	case ARM64_INS_BIT: res = irb.CreateOr(irb.CreateAnd(d, irb.CreateNot(m)), irb.CreateAnd(n, m)); break;
	case ARM64_INS_BIF: res = irb.CreateOr(irb.CreateAnd(d, m), irb.CreateAnd(n, irb.CreateNot(m))); break;
	default: throw GenericError("translateNeonBitSel(): unhandled instruction id");
	}

	storeRegister(ai->operands[0].reg, res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * ARM64_INS_CMEQ, CMGE, CMGT, CMHI, CMHS, CMTST
 *
 * Per-lane compare; a lane of the result is all ones or all zeroes. Two forms:
 * against another register, and against an immediate zero (`cmeq v0.16b,
 * v1.16b, #0`), which is how a NEON string routine asks "which of these bytes
 * is the terminator".
 *
 * Signedness is the whole instruction and the mnemonics are one letter apart:
 * CMGE and CMGT are signed, CMHS and CMHI the unsigned pair. The two readings
 * disagree on every lane with its top bit set, which for the byte lanes coming
 * out of a string search is all the interesting ones.
 *
 * CMTST is the odd one and is not a comparison at all -- `(vn & vm) != 0` per
 * lane -- so it has no immediate form here.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateNeonCmp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	unsigned bytes = 0;
	bool immZero = ai->operands[2].type == ARM64_OP_IMM && ai->operands[2].imm == 0;
	if (!neonSameWidthRegs(ai, immZero ? 2 : 3, bytes))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	auto [laneBits, lanes] = vasLanes(ai->operands[0].vas);
	if (laneBits == 0 || (!immZero && ai->operands[2].type != ARM64_OP_REG))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* w = irb.getIntNTy(bytes * 8);
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), lanes);
	llvm::Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(loadRegister(ai->operands[1].reg, irb), w), vecTy);
	llvm::Value* b = immZero
					   ? llvm::Constant::getNullValue(vecTy)
					   : irb.CreateBitCast(irb.CreateZExtOrTrunc(loadRegister(ai->operands[2].reg, irb), w), vecTy);

	llvm::Value* cmp = nullptr;
	switch (i->id)
	{
	case ARM64_INS_CMEQ: cmp = irb.CreateICmpEQ(a, b); break;
	case ARM64_INS_CMGE: cmp = irb.CreateICmpSGE(a, b); break;
	case ARM64_INS_CMGT: cmp = irb.CreateICmpSGT(a, b); break;
	case ARM64_INS_CMHI: cmp = irb.CreateICmpUGT(a, b); break;
	case ARM64_INS_CMHS: cmp = irb.CreateICmpUGE(a, b); break;
	case ARM64_INS_CMTST: cmp = irb.CreateICmpNE(irb.CreateAnd(a, b), llvm::Constant::getNullValue(vecTy)); break;
	default: throw GenericError("translateNeonCmp(): unhandled instruction id");
	}

	storeRegister(
		ai->operands[0].reg, irb.CreateBitCast(irb.CreateSExt(cmp, vecTy), w), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * ARM64_INS_LD1, ARM64_INS_ST1
 *
 * The straight-copy form of the NEON list load and store, and the last thing
 * COV-01 found untranslated on ARM64 in the parity corpus. `ld1 {v0.16b},
 * [x0]` is a plain 128-bit load; `ld1 {v0.8b}, [x1]` a 64-bit one that zeroes
 * the top half of the register, which is what every D-form write does. A list
 * moves consecutive registers to or from consecutive addresses.
 *
 * None of that needs a lane model, which is why these two can be translated
 * while the rest of NEON stays on the pseudo-asm path: V registers are i128
 * globals here and the only thing the arrangement decides is the total width.
 * `ld1 {v0.4s}, [x0]` and `ld1 {v0.16b}, [x0]` move the same 128 bits.
 *
 * What is deliberately left: writeback forms (`[x0], #16` has to update the
 * base register), lane forms (`ld1 {v0.s}[2], [x0]`, which vasByteWidth
 * rejects), and LD2/LD3/LD4 and their stores, which de-interleave rather than
 * copy.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateNeonLoadStore(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count >= 2));

	auto& memOp = ai->operands[ai->op_count - 1];
	unsigned regs = ai->op_count - 1;
	if (memOp.type != ARM64_OP_MEM || ai->writeback)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned bytes = 0;
	for (unsigned j = 0; j < regs; ++j)
	{
		auto& op = ai->operands[j];
		unsigned b = vasByteWidth(op.vas);
		if (!isVectorRegister(op) || op.vector_index >= 0 || b == 0 || (bytes != 0 && b != bytes))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		bytes = b;
	}

	bool load = i->id == ARM64_INS_LD1;
	auto* accessTy = irb.getIntNTy(bytes * 8);
	auto* base = generateGetOperandMemAddr(memOp, irb);

	for (unsigned j = 0; j < regs; ++j)
	{
		llvm::Value* at = base;
		if (j)
		{
			at = irb.CreateAdd(base, llvm::ConstantInt::get(base->getType(), bytes * j));
		}

		if (load)
		{
			// ZEXT into the i128 register is the upper-half zeroing that a
			// 64-bit arrangement does.
			storeRegister(ai->operands[j].reg, loadIntPtr(irb, at, accessTy), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		}
		else
		{
			auto* val = loadRegister(ai->operands[j].reg, irb);
			storeIntPtr(irb, irb.CreateZExtOrTrunc(val, accessTy), at, accessTy);
		}
	}
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::loadOp(
		cs_arm64_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case ARM64_OP_PSTATE:
		case ARM64_OP_SYS:
		case ARM64_OP_SVCR:
		case ARM64_OP_PREFETCH:
		case ARM64_OP_BARRIER:
			// Operation selectors (AT/IC/DC/TLBI, PSTATE, …), not GP/FP regs.
			// Capstone's union aliases these onto op.reg and can collide with
			// ARM64_REG_S10 etc.
			return nullptr;
		case ARM64_OP_REG_MRS:
		case ARM64_OP_REG_MSR:
		case ARM64_OP_REG:
		{
			auto* val = loadRegister(op.reg, irb);
			if (val == nullptr)
			{
				return llvm::UndefValue::get(ty ? ty : getDefaultType());
			}
			auto* vec = extractVectorValue(irb, op, val);
			auto* ext = generateOperandExtension(irb, op.ext, vec, ty);
			return generateOperandShift(irb, op, ext);
		}
		case ARM64_OP_IMM:
		{
			auto* t = getDefaultType();
			auto* val = llvm::ConstantInt::get(t, llvm::APInt(t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm), false, /*implicitTrunc=*/true));
			return generateOperandShift(irb, op, val);
		}
		case ARM64_OP_MEM:
		{
			auto* addr = generateGetOperandMemAddr(op, irb);

			if (lea)
			{
				return addr;
			}
			else
			{
				auto* lty = ty ? ty : getDefaultType();
				return loadIntPtr(irb, addr, lty);
			}

		}
		case ARM64_OP_FP:
		{
			auto* val = llvm::ConstantFP::get(irb.getDoubleTy(), op.fp);
			return val;
		}
		case ARM64_OP_INVALID:
		case ARM64_OP_CIMM:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::Instruction* Capstone2LlvmIrTranslatorArm64_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == ARM64_REG_INVALID)
	{
		return nullptr;
	}

	// Direct writes to PC are not supported, the intended way to alter control flow is to
	// use a branching instruction or exception, those will call pseudo llvm pseudo functions
	if (r == ARM64_REG_PC)
	{
		return nullptr;
	}

	if (r == ARM64_REG_XZR || r == ARM64_REG_WZR)
	{
		// When written the register discards the result
		return nullptr;
	}

	//auto* rt = getRegisterType(r);
	auto pr = getParentRegister(r);
	auto* llvmReg = getRegister(pr);
	if (llvmReg == nullptr)
	{
		// Maybe return xchg eax, eax?
		Log::error() << "storeRegister() unhandled reg." << std::endl;
		return nullptr;
	}

	if (llvmReg->getValueType()->isFloatingPointTy())
	{
		switch (ct)
		{
			case eOpConv::SITOFP_OR_FPCAST:
			case eOpConv::UITOFP_OR_FPCAST:
				val = generateTypeConversion(irb, val, llvmReg->getValueType(), ct);
				break;
			default:
				val = generateTypeConversion(irb, val, llvmReg->getValueType(), eOpConv::FPCAST_OR_BITCAST);
		}
	}
	else
	{
		switch (ct)
		{
			case eOpConv::SEXT_TRUNC_OR_BITCAST:
			case eOpConv::ZEXT_TRUNC_OR_BITCAST:
				val = generateTypeConversion(irb, val, llvmReg->getValueType(), ct);
				break;
			default:
				val = generateTypeConversion(irb, val, llvmReg->getValueType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
		}
	}

	auto* s = irb.CreateStore(val, llvmReg);
	attachPointeeType(s, llvmReg->getValueType());
	return s;
}

llvm::Instruction* Capstone2LlvmIrTranslatorArm64_impl::storeOp(
		cs_arm64_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case ARM64_OP_PSTATE:
		case ARM64_OP_SYS:
		case ARM64_OP_REG:
		case ARM64_OP_REG_MRS:
		case ARM64_OP_REG_MSR:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case ARM64_OP_MEM:
		{
			auto* addr = generateGetOperandMemAddr(op, irb);

			return storeIntPtr(irb, val, addr, val->getType());
		}
		case ARM64_OP_INVALID:
		case ARM64_OP_IMM:
		{
			// This is here because some operands that are for example in post-index addressing mode
			// will have the write flag set and generic functions try to write to IMM, which is not correct
			// Maybe solve this better?
			return nullptr;
		}
		case ARM64_OP_FP:
		case ARM64_OP_CIMM:
		case ARM64_OP_PREFETCH:
		case ARM64_OP_BARRIER:
		default:
		{
			throw GenericError("storeOp(): unhandled operand type");
			return nullptr;
		}
	}
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateInsnConditionCode(
		llvm::IRBuilder<>& irb,
		cs_arm64* ai)
{
	switch (ai->cc)
	{
		// Equal = Zero set
		case ARM64_CC_EQ:
		{
			auto* z = loadRegister(ARM64_REG_CPSR_Z, irb);
			return z;
		}
		// Not equal = Zero clear
		case ARM64_CC_NE:
		{
			auto* z = loadRegister(ARM64_REG_CPSR_Z, irb);
			return generateValueNegate(irb, z);
		}
		// Unsigned higher or same = Carry set
		case ARM64_CC_HS:
		{
			auto* c = loadRegister(ARM64_REG_CPSR_C, irb);
			return c;
		}
		// Unsigned lower = Carry clear
		case ARM64_CC_LO:
		{
			auto* c = loadRegister(ARM64_REG_CPSR_C, irb);
			return generateValueNegate(irb, c);
		}
		// Negative = N set
		case ARM64_CC_MI:
		{
			auto* n = loadRegister(ARM64_REG_CPSR_N, irb);
			return n;
		}
		// Positive or zero = N clear
		case ARM64_CC_PL:
		{
			auto* n = loadRegister(ARM64_REG_CPSR_N, irb);
			return generateValueNegate(irb, n);
		}
		// Overflow = V set
		case ARM64_CC_VS:
		{
			auto* v = loadRegister(ARM64_REG_CPSR_V, irb);
			return v;
		}
		// No overflow = V clear
		case ARM64_CC_VC:
		{
			auto* v = loadRegister(ARM64_REG_CPSR_V, irb);
			return generateValueNegate(irb, v);
		}
		// Unsigned higher = Carry set & Zero clear
		case ARM64_CC_HI:
		{
			auto* c = loadRegister(ARM64_REG_CPSR_C, irb);
			auto* z = loadRegister(ARM64_REG_CPSR_Z, irb);
			auto* nz = generateValueNegate(irb, z);
			return irb.CreateAnd(c, nz);
		}
		// Unsigned lower or same = Carry clear or Zero set
		case ARM64_CC_LS:
		{
			auto* z = loadRegister(ARM64_REG_CPSR_Z, irb);
			auto* c = loadRegister(ARM64_REG_CPSR_C, irb);
			auto* nc = generateValueNegate(irb, c);
			return irb.CreateOr(z, nc);
		}
		// Greater than or equal = N set and V set || N clear and V clear
		// (N & V) || (!N & !V) == !(N xor V)
		case ARM64_CC_GE:
		{
			auto* n = loadRegister(ARM64_REG_CPSR_N, irb);
			auto* v = loadRegister(ARM64_REG_CPSR_V, irb);
			auto* x = irb.CreateXor(n, v);
			return generateValueNegate(irb, x);
		}
		// Less than = N set and V clear || N clear and V set
		// (N & !V) || (!N & V) == (N xor V)
		case ARM64_CC_LT:
		{
			auto* n = loadRegister(ARM64_REG_CPSR_N, irb);
			auto* v = loadRegister(ARM64_REG_CPSR_V, irb);
			return irb.CreateXor(n, v);
		}
		// Greater than = Z clear, and either N set and V set, or N clear and V set
		case ARM64_CC_GT:
		{
			auto* z = loadRegister(ARM64_REG_CPSR_Z, irb);
			auto* n = loadRegister(ARM64_REG_CPSR_N, irb);
			auto* v = loadRegister(ARM64_REG_CPSR_V, irb);
			auto* xor1 = irb.CreateXor(n, v);
			auto* or1 = irb.CreateOr(z, xor1);
			return generateValueNegate(irb, or1);
		}
		// Less than or equal = Z set, or N set and V clear, or N clear and V set
		case ARM64_CC_LE:
		{
			auto* z = loadRegister(ARM64_REG_CPSR_Z, irb);
			auto* n = loadRegister(ARM64_REG_CPSR_N, irb);
			auto* v = loadRegister(ARM64_REG_CPSR_V, irb);
			auto* xor1 = irb.CreateXor(n, v);
			return irb.CreateOr(z, xor1);
		}
		case ARM64_CC_AL:
			// Allways
		case ARM64_CC_NV:
			// The Condition code NV exists only to provide a valid disassembly of the 0b1111 encoding, otherwise its behavior is identical to AL.
			return llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(_module->getContext()), 1);
		case ARM64_CC_INVALID:
		default:
		{
			throw GenericError("Probably wrong condition code.");
		}
	}
}

bool Capstone2LlvmIrTranslatorArm64_impl::isOperandRegister(cs_arm64_op& op)
{
	return op.type == ARM64_OP_REG;
}

bool Capstone2LlvmIrTranslatorArm64_impl::isFPRegister(cs_arm64_op& op, bool onlySupported) const
{
	if (op.type != ARM64_OP_REG)
	{
	    return false;
	}
	bool is_q_reg = (op.reg >= ARM64_REG_Q0 && op.reg <= ARM64_REG_Q31);
	bool is_d_reg = (op.reg >= ARM64_REG_D0 && op.reg <= ARM64_REG_D31);
	bool is_h_reg = (op.reg >= ARM64_REG_H0 && op.reg <= ARM64_REG_H31);
	bool is_s_reg = (op.reg >= ARM64_REG_S0 && op.reg <= ARM64_REG_S31);
	if (onlySupported)
	{
	    return is_d_reg || is_h_reg;
	}
	else
	{
	    // This is the overall correct behavior but since the support for 16bit floats
	    // or 128 bit floats is not implemented, we want to check only D and H registers
	    return is_q_reg || is_d_reg || is_h_reg || is_s_reg;
	}
}

bool Capstone2LlvmIrTranslatorArm64_impl::isVectorRegister(cs_arm64_op& op) const
{
	return op.type == ARM64_OP_REG && op.reg >= ARM64_REG_V0 && op.reg <= ARM64_REG_V31;
}

uint8_t Capstone2LlvmIrTranslatorArm64_impl::getOperandAccess(cs_arm64_op& op)
{
	switch (op.type)
	{
		case ARM64_OP_PSTATE:
		case ARM64_OP_SYS:
		case ARM64_OP_SVCR:
		case ARM64_OP_PREFETCH:
		case ARM64_OP_BARRIER:
			return 0;
		default:
			return op.access;
	}
}

bool Capstone2LlvmIrTranslatorArm64_impl::isCondIns(cs_arm64 * i) const
{
	return (i->cc == ARM64_CC_INVALID) ? false : true;
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateIntBitCastToFP(llvm::IRBuilder<>& irb, llvm::Value* val) const
{
	if (auto* it = llvm::dyn_cast<llvm::IntegerType>(val->getType()))
	{
		switch(it->getBitWidth())
		{
		case 32:
			return irb.CreateBitCast(val, irb.getFloatTy());
		case 64:
			return irb.CreateBitCast(val, irb.getDoubleTy());
		default:
			throw GenericError("Arm64::generateIntBitCastToFP: unhandled Integer type");
		}
	}
	// Return unchanged value if its not FP type
	return val;
}

llvm::Value* Capstone2LlvmIrTranslatorArm64_impl::generateFPBitCastToIntegerType(llvm::IRBuilder<>& irb, llvm::Value* val) const
{
	auto* ty = val->getType();
	if (ty->isFloatingPointTy())
	{
		if (ty->isDoubleTy())
		{
			return irb.CreateBitCast(val, irb.getInt64Ty());
		}
		else if (ty->isFloatTy())
		{
			return irb.CreateBitCast(val, irb.getInt32Ty());
		}
		else
		{
			throw GenericError("Arm64::generateFPBitCastToIntegerType: unhandled FP type");
		}
	}
	// Return unchanged value if its not FP type
	return val;
}

void Capstone2LlvmIrTranslatorArm64_impl::generatePseudoInstruction(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	throwUnhandledInstructions(i);

	if (!isCondIns(ai))
	{
		_inCondition = false;
		translatePseudoAsmGeneric(i, ai, irb);
	}
	else
	{
		_inCondition = true;

		auto* cond = generateInsnConditionCode(irb, ai);
		llvm::IRBuilder<> bodyIrb(generateIfThen(cond, irb));

		translatePseudoAsmGeneric(i, ai, bodyIrb);
	}
}

bool Capstone2LlvmIrTranslatorArm64_impl::ifVectorGeneratePseudo(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb, _translator_fnc trans)
{
    bool pseudo = false;
    for (std::uint8_t i = 0; i < ai->op_count; ++i)
    {
	    if (isVectorRegister(ai->operands[i]))
	    {
		    pseudo = true;
		    break;
	    }
    }

    if (pseudo)
    {
	    throwUnhandledInstructions(i);
	    if (trans == nullptr)
	    {
		    generatePseudoInstruction(i, ai, irb);
	    }
	    else
	    {
		    (this->*trans)(i, ai, irb);
	    }
    }

    return pseudo;
}

//
//==============================================================================
// ARM64 instruction translation methods.
//==============================================================================
//

/**
 * ARM64_INS_ADC
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateAdc(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* carry = loadRegister(ARM64_REG_CPSR_C, irb);

	auto* val = irb.CreateAdd(op1, op2);
	val       = irb.CreateAdd(val, irb.CreateZExtOrTrunc(carry, val->getType()));

	storeOp(ai->operands[0], val, irb);

	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_C, generateCarryAddC(op1, op2, irb, carry), irb);
		storeRegister(ARM64_REG_CPSR_V, generateOverflowAddC(val, op1, op2, irb, carry), irb);
		storeRegister(ARM64_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM64_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
}

/**
 * ARM64_INS_ADD, ARM64_INS_CMN
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateAdd(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	// `add v0.4s, v1.4s, v2.4s` is four 32-bit adds, not one 128-bit add, and
	// this translator has no vector model to say so. It reached CreateAdd
	// anyway -- isFPRegister() knows Q, D, H and S registers but not V, so the
	// bitcast below did not fire, and a `.4s` operand arrives as `float`:
	// "Tried to create an integer operation on a non-integer type", which is
	// how all ten ARM64 binaries in ARCH-01 dumped core. The guard other
	// translators in this file already use is the right answer, and it is
	// honest about what is not modelled instead of emitting the wrong
	// arithmetic.
	if (ifVectorGeneratePseudo(i, ai, irb))
	{
		return;
	}

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb);
	op2 = generateTypeConversion(irb, op2, op1->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);

	// For some reason it is possible to add two FP registers with integer add?
	// This looks to be also true for sub
	if (isFPRegister(ai->operands[0]) && i->id != ARM64_INS_CMN)
	{
		op1 = generateFPBitCastToIntegerType(irb, op1);
		op2 = generateFPBitCastToIntegerType(irb, op2);
	}

	auto *val = irb.CreateAdd(op1, op2);
	if (i->id != ARM64_INS_CMN)
	{
		storeOp(ai->operands[0], val, irb);
	}

	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_C, generateCarryAdd(val, op1, irb), irb);
		storeRegister(ARM64_REG_CPSR_V, generateOverflowAdd(val, op1, op2, irb), irb);
		storeRegister(ARM64_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM64_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
}

/**
 * ARM64_INS_SUB, ARM64_INS_CMP
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateSub(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	// See translateAdd: `sub v0.2d, v1.2d, v2.2d` is not a 128-bit subtract.
	if (ifVectorGeneratePseudo(i, ai, irb))
	{
		return;
	}

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb);
	op2 = generateTypeConversion(irb, op2, op1->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);

	// For some reason it is possible to sub two FP registers with integer sub?
	// This looks to be also true for add
	if (isFPRegister(ai->operands[0]) && i->id != ARM64_INS_CMP)
	{
		op1 = generateFPBitCastToIntegerType(irb, op1);
		op2 = generateFPBitCastToIntegerType(irb, op2);
	}

	auto* val = irb.CreateSub(op1, op2);
	if (i->id != ARM64_INS_CMP)
	{
		storeOp(ai->operands[0], val, irb);
	}

	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_C, generateValueNegate(irb, generateBorrowSub(op1, op2, irb)), irb);
		storeRegister(ARM64_REG_CPSR_V, generateOverflowSub(val, op1, op2, irb), irb);
		storeRegister(ARM64_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM64_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
}

/**
 * ARM64_INS_NEG
 * ARM64_INS_NEGS for some reason capstone includes this instruction as alias.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateNeg(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	auto* op2 = loadOpBinaryOp1(ai, irb);

	llvm::Value* val = nullptr;
	if (isFPRegister(ai->operands[1]))
	{
		val = irb.CreateFNeg(op2);
	}
	else
	{
		llvm::Value* zero = llvm::ConstantInt::get(op2->getType(), 0);
		val = irb.CreateSub(zero, op2);
	}

	storeOp(ai->operands[0], val, irb);

	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_C, generateValueNegate(irb, generateBorrowSub(zero, op2, irb)), irb);
		storeRegister(ARM64_REG_CPSR_V, generateOverflowSub(val, zero, op2, irb), irb);
		storeRegister(ARM64_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM64_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
}

/**
 * ARM64_INS_SBC
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateSbc(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb);

	auto* carry = loadRegister(ARM64_REG_CPSR_C, irb);

	// NOT(OP2)
	op2 = irb.CreateZExtOrTrunc(op2, op1->getType());
	op2 = generateValueNegate(irb, op2);

	// OP1 + NOT(OP2) + CARRY
	auto* val = irb.CreateAdd(op1, op2);
	val       = irb.CreateAdd(val, irb.CreateZExtOrTrunc(carry, val->getType()));

	storeOp(ai->operands[0], val, irb);

	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_C, generateCarryAddC(op1, op2, irb, carry), irb);
		storeRegister(ARM64_REG_CPSR_V, generateOverflowAddC(val, op1, op2, irb, carry), irb);
		storeRegister(ARM64_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM64_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
}

/**
 * ARM64_INS_NGC, ARM64_INS_NGCS
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateNgc(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	auto* op2 = loadOpBinaryOp1(ai, irb);
	llvm::Value* op1 = llvm::ConstantInt::get(op2->getType(), 0);
	auto* carry = loadRegister(ARM64_REG_CPSR_C, irb);

	// NOT(OP2)
	op2 = irb.CreateZExtOrTrunc(op2, op1->getType());
	op2 = generateValueNegate(irb, op2);

	// OP1 + NOT(OP2) + CARRY
	auto* val = irb.CreateAdd(op1, op2);
	val       = irb.CreateAdd(val, irb.CreateZExtOrTrunc(carry, val->getType()));

	storeOp(ai->operands[0], val, irb);

	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_C, generateCarryAddC(op1, op2, irb, carry), irb);
		storeRegister(ARM64_REG_CPSR_V, generateOverflowAddC(val, op1, op2, irb, carry), irb);
		storeRegister(ARM64_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM64_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
}

/**
 * ARM64_INS_NOP
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateNop(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	// Don't translate anything.
}

/**
 * ARM64_INS_DMB, ARM64_INS_DSB, ARM64_INS_ISB
 * Memory / instruction barriers → LLVM fence. ISB has no I-cache model;
 * it is lowered as seq_cst like DMB/DSB.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFence(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	irb.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
}

/**
 * ARM64_INS_MOV, ARM64_INS_MVN, ARM64_INS_MOVZ, ARM64_INS_MOVN
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateMov(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);
	auto* dstTy = getRegisterType(ai->operands[0].reg);
	if (op1->getType() != dstTy)
	{
		if (op1->getType()->isIntegerTy() && dstTy->isIntegerTy())
			op1 = irb.CreateZExtOrTrunc(op1, dstTy);
		else if (op1->getType()->isIntegerTy() && dstTy->isFloatingPointTy())
		{
			auto* intTy = llvm::Type::getIntNTy(
				_module->getContext(), dstTy->getPrimitiveSizeInBits());
			op1 = irb.CreateZExtOrTrunc(op1, intTy);
			op1 = irb.CreateBitCast(op1, dstTy);
		}
		else if (op1->getType()->isFloatingPointTy() && dstTy->isIntegerTy())
		{
			auto* intTy = llvm::Type::getIntNTy(
				_module->getContext(), op1->getType()->getPrimitiveSizeInBits());
			op1 = irb.CreateBitCast(op1, intTy);
			op1 = irb.CreateZExtOrTrunc(op1, dstTy);
		}
		else
			op1 = irb.CreateBitCast(op1, dstTy);
	}

	if (i->id == ARM64_INS_MVN || i->id == ARM64_INS_MOVN)
	{
		op1 = generateValueNegate(irb, op1);
	}

	storeOp(ai->operands[0], op1, irb);
}

/**
 * ARM64_INS_MOVK
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateMovk(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	// Load the destination register
	op0 = loadOp(ai->operands[0], irb);

	// Create simple imm16 bit inverted mask
	llvm::Value* and_mask = llvm::ConstantInt::get(op0->getType(), 0xffff);

	// Get the operand shift value
	auto shift_val = (ai->operands[1].shift.type == ARM64_SFT_INVALID) ? 0 : ai->operands[1].shift.value;

	// Shift the mask to proper place in case of LSL imm shift (example: movk x0, #123, LSL #32)
	and_mask = irb.CreateShl(and_mask, llvm::ConstantInt::get(op0->getType(), shift_val));

	// Invert the mask
	and_mask = generateValueNegate(irb, and_mask);

	op0 = irb.CreateAnd(op0, and_mask);

	op1 = loadOp(ai->operands[1], irb);
	op1 = irb.CreateZExtOrTrunc(op1, op0->getType());
	// Move the value keeping the original data in register changing only the 16bit imm
	auto *val = irb.CreateOr(op0, op1);

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_STR, ARM64_INS_STRB, ARM64_INS_STRH
 * ARM64_INS_STUR, ARM64_INS_STURB, ARM64_INS_STURH
 * ARM64_INS_STTR, ARM64_INS_STTRB, ARM64_INS_STTRH
 * ARM64_INS_STXR, ARM64_INS_STXRB, ARM64_INS_STXRH -- Maybe those should be pseudo
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateStr(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case ARM64_INS_STR:
		case ARM64_INS_STUR:
		case ARM64_INS_STTR:
		case ARM64_INS_STLR:
		{
			ty = getRegisterType(ai->operands[0].reg);
			if (ty->isFloatTy())
			{
				ty = irb.getInt32Ty();
			}
			else if (ty->isDoubleTy())
			{
				ty = irb.getInt64Ty();
			}
			break;
		}
		case ARM64_INS_STRB:
		case ARM64_INS_STURB:
		case ARM64_INS_STTRB:
		case ARM64_INS_STLRB:
		{
			ty = irb.getInt8Ty();
			break;
		}
		case ARM64_INS_STRH:
		case ARM64_INS_STURH:
		case ARM64_INS_STTRH:
		case ARM64_INS_STLRH:
		{
			ty = irb.getInt16Ty();
			break;
		}
		default:
		{
			throw GenericError("Arm64: unhandled STR id");
		}
	}

	op0 = loadOp(ai->operands[0], irb);

	// If its floating point operand bit cast it to integer type
	// since the ZExt or Trunc doesn't work fp numbers
	//op0 = generateFPBitCastToIntegerType(irb, op0);
	//op0 = irb.CreateBitCast(op0, irb.getInt32Ty());
	if (!op0->getType()->isFloatingPointTy())
	{
		op0 = irb.CreateZExtOrTrunc(op0, ty);
	}
	auto* dest = generateGetOperandMemAddr(ai->operands[1], irb);
	auto* st = storeIntPtr(irb, op0, dest, op0->getType());
	if (i->id == ARM64_INS_STLR || i->id == ARM64_INS_STLRB || i->id == ARM64_INS_STLRH)
	{
		st->setAtomic(llvm::AtomicOrdering::Release);
	}

	uint32_t baseR = ARM64_REG_INVALID;
	if (ai->op_count == 2)
	{
		baseR = ai->operands[1].reg;
	}
	else if (ai->op_count == 3)
	{
		baseR = ai->operands[1].reg;

		auto* disp = llvm::ConstantInt::get(getDefaultType(), ai->operands[2].imm);
		dest = irb.CreateAdd(dest, disp);
		// post-index -> always writeback
	}
	else
	{
		throw GenericError("STR: unsupported STR format");
	}

	if (ai->writeback && baseR != ARM64_REG_INVALID)
	{
		storeRegister(baseR, dest, irb);
	}
}

/**
 * ARM64_INS_STXR, ARM64_INS_STXRB, ARM64_INS_STXRH
 * ARM64_INS_STLXR, ARM64_INS_STLXRB, ARM64_INS_STLXRH
 * Exclusive store: atomic store + status 0 (no exclusive-monitor model).
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateStxr(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count == 3));

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case ARM64_INS_STXR:
		case ARM64_INS_STLXR:
			ty = getRegisterType(ai->operands[1].reg);
			break;
		case ARM64_INS_STXRB:
		case ARM64_INS_STLXRB:
			ty = irb.getInt8Ty();
			break;
		case ARM64_INS_STXRH:
		case ARM64_INS_STLXRH:
			ty = irb.getInt16Ty();
			break;
		default:
			throw GenericError("Arm64: unhandled STXR id");
	}

	auto* val = loadOp(ai->operands[1], irb);
	if (!val->getType()->isFloatingPointTy())
	{
		val = irb.CreateZExtOrTrunc(val, ty);
	}

	llvm::Value* dest = nullptr;
	if (ai->operands[2].type == ARM64_OP_MEM)
	{
		dest = generateGetOperandMemAddr(ai->operands[2], irb);
	}
	else
	{
		dest = loadOp(ai->operands[2], irb, nullptr, true);
	}

	auto* st = storeIntPtr(irb, val, dest, val->getType());
	const bool release = i->id == ARM64_INS_STLXR || i->id == ARM64_INS_STLXRB
			|| i->id == ARM64_INS_STLXRH;
	st->setAtomic(release ? llvm::AtomicOrdering::Release : llvm::AtomicOrdering::Monotonic);
	storeRegister(ai->operands[0].reg, llvm::ConstantInt::get(getRegisterType(ai->operands[0].reg), 0), irb);
}

/**
 * ARM64_INS_STXP, ARM64_INS_STLXP
 * Exclusive pair store: one wide atomic store + status 0.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateStxp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count == 4));

	auto* v0 = loadOp(ai->operands[1], irb);
	auto* v1 = loadOp(ai->operands[2], irb);
	const unsigned halfBits = getRegisterByteSize(ai->operands[1].reg) * 8;
	auto* wide = llvm::Type::getIntNTy(_module->getContext(), halfBits * 2);
	v0 = irb.CreateZExtOrTrunc(v0, wide);
	v1 = irb.CreateZExtOrTrunc(v1, wide);
	auto* pair = irb.CreateOr(v0, irb.CreateShl(v1, llvm::ConstantInt::get(wide, halfBits)));

	llvm::Value* dest = nullptr;
	if (ai->operands[3].type == ARM64_OP_MEM)
	{
		dest = generateGetOperandMemAddr(ai->operands[3], irb);
	}
	else
	{
		dest = loadOp(ai->operands[3], irb, nullptr, true);
	}

	auto* st = storeIntPtr(irb, pair, dest, wide);
	st->setAtomic(
			i->id == ARM64_INS_STLXP ? llvm::AtomicOrdering::Release : llvm::AtomicOrdering::Monotonic);
	storeRegister(ai->operands[0].reg, llvm::ConstantInt::get(getRegisterType(ai->operands[0].reg), 0), irb);
}

/**
 * ARM64_INS_STP, ARM64_INS_STNP
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateStp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (2 <= ai->op_count && ai->op_count <= 4));

	op0 = loadOp(ai->operands[0], irb);
	op1 = loadOp(ai->operands[1], irb);

	uint32_t baseR = ARM64_REG_INVALID;
	llvm::Value* newDest = nullptr;
	auto* dest = generateGetOperandMemAddr(ai->operands[2], irb);
	auto* registerSize = llvm::ConstantInt::get(getDefaultType(), getRegisterByteSize(ai->operands[0].reg));
	storeOp(ai->operands[2], op0, irb);
	if (ai->op_count == 3)
	{
		newDest = irb.CreateAdd(dest, registerSize);
		storeIntPtr(irb, op1, newDest, op1->getType());

		baseR = ai->operands[2].mem.base;
	}
	else if (ai->op_count == 4)
	{
		auto* disp = llvm::ConstantInt::get(getDefaultType(), ai->operands[3].imm);
		newDest    = irb.CreateAdd(dest, registerSize);
		storeIntPtr(irb, op1, newDest, op1->getType());

		baseR = ai->operands[2].mem.base;

		newDest = irb.CreateAdd(dest, disp);
	}
	else
	{
		throw GenericError("STR: unsupported STP format");
	}

	if (ai->writeback && baseR != ARM64_REG_INVALID)
	{
		storeRegister(baseR, newDest, irb);
	}
}

/**
 * ARM64_INS_LDR
 * ARM64_INS_LDURB, ARM64_INS_LDUR, ARM64_INS_LDURH, ARM64_INS_LDURSB, ARM64_INS_LDURSH, ARM64_INS_LDURSW
 * ARM64_INS_LDRB, ARM64_INS_LDRH, ARM64_INS_LDRSB, ARM64_INS_LDRSH, ARM64_INS_LDRSW
 * ARM64_INS_LDTR, ARM64_INS_LDTRB, ARM64_INS_LDTRSB, ARM64_INS_LDTRH, ARM64_INS_LDTRSH, ARM64_INS_LDTRSW
 * ARM64_INS_LDXR, ARM64_INS_LDXRB, ARM64_INS_LDXRH
 * ARM64_INS_LDAXR, ARM64_INS_LDAXRB, ARM64_INS_LDAXRH
 * ARM64_INS_LDAR, ARM64_INS_LDARB, ARM64_INS_LDARH
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateLdr(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	llvm::Type* ty = nullptr;
	bool sext = false;
	switch (i->id)
	{
		case ARM64_INS_LDR:
		case ARM64_INS_LDUR:
		case ARM64_INS_LDTR:
		case ARM64_INS_LDXR:
		case ARM64_INS_LDAXR:
		case ARM64_INS_LDAR:
		{
			ty = getRegisterType(ai->operands[0].reg);
			sext = false;
			break;
		}
		case ARM64_INS_LDRB:
		case ARM64_INS_LDURB:
		case ARM64_INS_LDTRB:
		case ARM64_INS_LDXRB:
		case ARM64_INS_LDAXRB:
		case ARM64_INS_LDARB:
		{
			ty = irb.getInt8Ty();
			sext = false;
			break;
		}
		case ARM64_INS_LDRH:
		case ARM64_INS_LDURH:
		case ARM64_INS_LDTRH:
		case ARM64_INS_LDXRH:
		case ARM64_INS_LDAXRH:
		case ARM64_INS_LDARH:
		{
			ty = irb.getInt16Ty();
			sext = false;
			break;
		}
		// Signed loads
		case ARM64_INS_LDRSB:
		case ARM64_INS_LDURSB:
		case ARM64_INS_LDTRSB:
		{
			ty = irb.getInt8Ty();
			sext = true;
			break;
		}
		case ARM64_INS_LDRSH:
		case ARM64_INS_LDURSH:
		case ARM64_INS_LDTRSH:
		{
			ty = irb.getInt16Ty();
			sext = true;
			break;
		}
		case ARM64_INS_LDRSW:
		case ARM64_INS_LDURSW:
		case ARM64_INS_LDTRSW:
		{
			ty = irb.getInt32Ty();
			sext = true;
			break;
		}
		default:
		{
			throw GenericError("Arm64: unhandled LDR id");
		}
	}

	auto* regType = getRegisterType(ai->operands[0].reg);
	auto* dest = loadOp(ai->operands[1], irb, nullptr, true);
	llvm::Value* loaded_value = loadIntPtr(irb, dest, ty);
	if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(loaded_value))
	{
		switch (i->id)
		{
			case ARM64_INS_LDXR:
			case ARM64_INS_LDXRB:
			case ARM64_INS_LDXRH:
				ld->setAtomic(llvm::AtomicOrdering::Monotonic);
				break;
			case ARM64_INS_LDAXR:
			case ARM64_INS_LDAXRB:
			case ARM64_INS_LDAXRH:
			case ARM64_INS_LDAR:
			case ARM64_INS_LDARB:
			case ARM64_INS_LDARH:
				ld->setAtomic(llvm::AtomicOrdering::Acquire);
				break;
			default:
				break;
		}
	}
	// If the result should be floating point, bit cast it
	if (!regType->isFloatingPointTy())
	{
		loaded_value = sext
			? irb.CreateSExtOrTrunc(loaded_value, regType)
			: irb.CreateZExtOrTrunc(loaded_value, regType);
	}

	storeRegister(ai->operands[0].reg, loaded_value, irb);

	uint32_t baseR = ARM64_REG_INVALID;
	if (ai->op_count == 2)
	{
		baseR = ai->operands[1].reg;
	}
	else if (ai->op_count == 3) // POST-index
	{
		baseR = ai->operands[1].reg;

		auto* disp = llvm::ConstantInt::get(getDefaultType(), ai->operands[2].imm);
		dest = irb.CreateAdd(dest, disp);
	}
	else
	{
		throw GenericError("Arm64: unsupported ldr format");
	}

	if (ai->writeback && baseR != ARM64_REG_INVALID)
	{
		storeRegister(baseR, dest, irb);
	}
}

/**
 * ARM64_INS_LDP, ARM64_INS_LDPSW
 * ARM64_INS_LDNP (Non-temporal)
 * ARM64_INS_LDXP (Exclusive) — atomic pair loads (monotonic)
 * ARM64_INS_LDAXP (Exclusive Acquire) — atomic pair loads (acquire)
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateLdp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (2 <= ai->op_count && ai->op_count <= 4));

	llvm::Value* data_size = nullptr;
	llvm::Type* ty = nullptr;
	eOpConv ct = eOpConv::THROW;
	switch(i->id)
	{
	case ARM64_INS_LDNP:
		// Hints PE that the memory is not going to be used in near future
	case ARM64_INS_LDP:
	case ARM64_INS_LDXP:
	case ARM64_INS_LDAXP:
		data_size = llvm::ConstantInt::get(getDefaultType(), getRegisterByteSize(ai->operands[0].reg));
		ty = getRegisterType(ai->operands[0].reg);
		ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
		break;
	case ARM64_INS_LDPSW:
		data_size = llvm::ConstantInt::get(getDefaultType(), 4);
		ty = irb.getInt32Ty();
		ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
		break;
	default:
		throw GenericError("Arm64 Ldp: Instruction id error");
	}

	auto* dest = loadOp(ai->operands[2], irb, nullptr, true);
	auto* newReg1Value = loadIntPtr(irb, dest, ty);

	llvm::Value* newDest = nullptr;
	llvm::Value* newReg2Value = nullptr;
	uint32_t baseR = ARM64_REG_INVALID;
	if (ai->op_count == 3)
	{
		storeRegister(ai->operands[0].reg, newReg1Value, irb, ct);
		newDest = irb.CreateAdd(dest, data_size);
		newReg2Value = loadIntPtr(irb, newDest, ty);
		storeRegister(ai->operands[1].reg, newReg2Value, irb, ct);

		baseR = ai->operands[2].mem.base;
	}
	else if (ai->op_count == 4)
	{

		storeRegister(ai->operands[0].reg, newReg1Value, irb, ct);
		newDest = irb.CreateAdd(dest, data_size);
		newReg2Value = loadIntPtr(irb, newDest, ty);
		storeRegister(ai->operands[1].reg, newReg2Value, irb, ct);

		auto* disp = llvm::ConstantInt::get(getDefaultType(), ai->operands[3].imm);
		dest = irb.CreateAdd(dest, disp);
		baseR = ai->operands[2].mem.base;
	}
	else
	{
		throw GenericError("ldp, ldpsw: Unsupported instruction format");
	}

	if (i->id == ARM64_INS_LDXP || i->id == ARM64_INS_LDAXP)
	{
		const auto order = i->id == ARM64_INS_LDAXP
				? llvm::AtomicOrdering::Acquire
				: llvm::AtomicOrdering::Monotonic;
		if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(newReg1Value))
		{
			ld->setAtomic(order);
		}
		if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(newReg2Value))
		{
			ld->setAtomic(order);
		}
	}

	if (ai->writeback && baseR != ARM64_REG_INVALID)
	{
		storeRegister(baseR, dest, irb);
	}
}

/**
 * ARM64_INS_ADR, ARM64_INS_ADRP
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateAdr(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	auto* imm  = loadOpBinaryOp1(ai, irb);

	// Even though the semantics for this instruction is
	// base = PC[]
	// X[t] = base + imm
	// It looks like capstone is already doing this work for us and
	// second operand has calculated value already
	/*
	auto* base = loadRegister(ARM64_REG_PC, irb);
	// ADRP loads address to 4KB page
	if (i->id == ARM64_INS_ADRP)
	{
		base = llvm::ConstantInt::get(getDefaultType(), (((i->address + i->size) >> 12) << 12));
	}
	auto* res  = irb.CreateAdd(base, imm);
	*/

	storeRegister(ai->operands[0].reg, imm, irb);
}

/**
 * ARM64_INS_AND, ARM64_INS_BIC, ARM64_INS_TST
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateAnd(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb);
	op2 = irb.CreateZExtOrTrunc(op2, op1->getType());

	if (i->id == ARM64_INS_BIC || i->id == ARM64_INS_BICS)
	{
		op2 = generateValueNegate(irb, op2);
	}
	auto* val = irb.CreateAnd(op1, op2);

	if (i->id != ARM64_INS_TST)
	{
		storeOp(ai->operands[0], val, irb);
	}

	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM64_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
		// According to documentation carry and overflow should be
		// set to zero.
		storeRegister(ARM64_REG_CPSR_C, zero, irb);
		storeRegister(ARM64_REG_CPSR_V, zero, irb);
	}
}

/**
 * ARM64_INS_ASR, ARM64_INS_LSL, ARM64_INS_LSR, ARM64_INS_ROR
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateShifts(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb);
	op2 = irb.CreateZExtOrTrunc(op2, op1->getType());

	llvm::Value* val = nullptr;
	switch(i->id)
	{
		case ARM64_INS_ASR:
		{
			val = irb.CreateAShr(op1, op2);
			break;
		}
		case ARM64_INS_LSL:
		{
			val = irb.CreateShl(op1, op2);
			break;
		}
		case ARM64_INS_LSR:
		{
			val = irb.CreateLShr(op1, op2);
			break;
		}
		case ARM64_INS_ROR:
		{
			val = generateShiftRor(irb, op1, op2);
			break;
		}
		default:
		{
			throw GenericError("Shifts: unhandled insn ID");
		}
	}

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_UBFX, ARM64_INS_UBFIZ, ARM64_INS_SBFX, ARM64_INS_SBFIZ,
 * ARM64_INS_BFI, ARM64_INS_BFXIL
 *
 * The bitfield-move aliases. Capstone reports these rather than the UBFM/SBFM/
 * BFM encodings they alias -- measured over a corpus of 36 statically linked
 * aarch64 programs, UBFM, SBFM and BFM appear zero times and these six appear
 * 7,526 -- so translating the aliases is what reaches real code.
 *
 * Every one is a shift and a mask. They were all `nullptr`, which sent each to
 * a pseudo-call: correct in the sense that it does not claim a wrong value,
 * and opaque to every later pass, because an asm pseudo-call is a barrier that
 * nothing can see a definition through.
 *
 * Operands are (Rd, Rn, #lsb, #width) for all six.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateBitfield(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count == 4));

	op1 = loadOp(ai->operands[1], irb);
	auto* ty = llvm::cast<llvm::IntegerType>(op1->getType());
	const unsigned bits = ty->getBitWidth();

	const uint64_t lsb = static_cast<uint64_t>(ai->operands[2].imm);
	const uint64_t width = static_cast<uint64_t>(ai->operands[3].imm);

	// The encodings cannot express these, but the operands arrive from a
	// disassembler rather than from the encoding, so refuse rather than emit a
	// shift by more than the type's width -- which is poison in LLVM IR.
	if (width == 0 || width > bits || lsb >= bits || lsb + width > bits)
	{
		throwUnhandledInstructions(i, "bitfield operands out of range");
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* maskC = llvm::ConstantInt::get(ty, llvm::APInt::getLowBitsSet(bits, static_cast<unsigned>(width)));
	auto* lsbC = llvm::ConstantInt::get(ty, lsb);

	llvm::Value* val = nullptr;
	switch (i->id)
	{
	case ARM64_INS_UBFX: {
		// Rd = (Rn >> lsb) & mask(width)
		val = irb.CreateAnd(irb.CreateLShr(op1, lsbC), maskC);
		break;
	}
	case ARM64_INS_UBFIZ: {
		// Rd = (Rn & mask(width)) << lsb
		val = irb.CreateShl(irb.CreateAnd(op1, maskC), lsbC);
		break;
	}
	case ARM64_INS_SBFX: {
		// Rd = sign_extend(Rn[lsb+width-1 : lsb], width). Shifting the
		// field up to the sign bit and arithmetic-shifting it back is the
		// sign extension, and needs no separate mask.
		auto* up = llvm::ConstantInt::get(ty, bits - width);
		val = irb.CreateAShr(irb.CreateShl(op1, llvm::ConstantInt::get(ty, bits - width - lsb)), up);
		break;
	}
	case ARM64_INS_SBFIZ: {
		// Rd = sign_extend(Rn[width-1:0], width) << lsb. Same trick, but
		// shifted back by less so the field lands at lsb rather than 0.
		val = irb.CreateAShr(
			irb.CreateShl(op1, llvm::ConstantInt::get(ty, bits - width)),
			llvm::ConstantInt::get(ty, bits - width - lsb));
		break;
	}
	case ARM64_INS_BFI: {
		// Rd = (Rd & ~(mask << lsb)) | ((Rn & mask) << lsb). The only two
		// here that read their destination.
		op0 = loadOp(ai->operands[0], irb);
		auto* placed = irb.CreateShl(irb.CreateAnd(op1, maskC), lsbC);
		auto* hole = llvm::ConstantInt::get(
			ty, ~(llvm::APInt::getLowBitsSet(bits, static_cast<unsigned>(width)) << static_cast<unsigned>(lsb)));
		val = irb.CreateOr(irb.CreateAnd(op0, hole), placed);
		break;
	}
	case ARM64_INS_BFXIL: {
		// Rd = (Rd & ~mask) | ((Rn >> lsb) & mask)
		op0 = loadOp(ai->operands[0], irb);
		auto* taken = irb.CreateAnd(irb.CreateLShr(op1, lsbC), maskC);
		auto* hole = llvm::ConstantInt::get(ty, ~llvm::APInt::getLowBitsSet(bits, static_cast<unsigned>(width)));
		val = irb.CreateOr(irb.CreateAnd(op0, hole), taken);
		break;
	}
	default: {
		throw GenericError("Bitfield: unhandled insn ID");
	}
	}

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_BR, ARM64_INS_BRL
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateBr(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	// Branch with link to register
	if (i->id == ARM64_INS_BLR)
	{
		storeRegister(ARM64_REG_LR, getNextInsnAddress(i), irb);
	}

	op0 = loadOpUnary(ai, irb);
	generateBranchFunctionCall(irb, op0);
}

/**
 * ARM64_INS_B
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateB(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	op0 = loadOpUnary(ai, irb);

	if (isCondIns(ai)) {
		auto* cond = generateInsnConditionCode(irb, ai);
		generateCondBranchFunctionCall(irb, cond, op0);
	}
	else
	{
		generateBranchFunctionCall(irb, op0);
	}
}

/**
 * ARM64_INS_BL
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateBl(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	storeRegister(ARM64_REG_LR, getNextInsnAddress(i), irb);
	op0 = loadOpUnary(ai, irb);
	generateCallFunctionCall(irb, op0);
}

/**
 * ARM64_INS_CLZ
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateClz(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);

	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
	    _module,
	    llvm::Intrinsic::ctlz,
	    op1->getType());

	auto* val = irb.CreateCall(f, {op1, irb.getTrue()});
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_CBNZ, ARM64_INS_CBZ
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateCbnz(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	std::tie(op0, op1) = loadOpBinary(ai, irb);
	llvm::Value* cond = nullptr;
	if (i->id == ARM64_INS_CBNZ)
	{
		cond = irb.CreateICmpNE(op0, llvm::ConstantInt::get(op0->getType(), 0));
	}
	else if (i->id == ARM64_INS_CBZ)
	{
		cond = irb.CreateICmpEQ(op0, llvm::ConstantInt::get(op0->getType(), 0));
	}
	else
	{
		throw GenericError("cbnz, cbz: Instruction id error");
	}
	generateCondBranchFunctionCall(irb, cond, op1);
}

/**
 * ARM64_INS_CCMN, ARM64_INS_CCMP
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateCondCompare(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	op1 = loadOp(ai->operands[0], irb);
	op2 = loadOp(ai->operands[1], irb);
	auto* nzvc = loadOp(ai->operands[2], irb);

	op2 = irb.CreateZExtOrTrunc(op2, op1->getType());

	auto* cond = generateInsnConditionCode(irb, ai);
	auto irbP = generateIfThenElse(cond, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	//IF - condition holds
	llvm::Value* val = nullptr;
	if (i->id == ARM64_INS_CCMP)
	{
		val = bodyIf.CreateSub(op1, op2);
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_C, generateValueNegate(bodyIf, generateBorrowSub(op1, op2, bodyIf)), bodyIf);
		storeRegister(ARM64_REG_CPSR_V, generateOverflowSub(val, op1, op2, bodyIf), bodyIf);
		storeRegister(ARM64_REG_CPSR_N, bodyIf.CreateICmpSLT(val, zero), bodyIf);
		storeRegister(ARM64_REG_CPSR_Z, bodyIf.CreateICmpEQ(val, zero), bodyIf);
	}
	else if (i->id == ARM64_INS_CCMN)
	{
		val = bodyIf.CreateAdd(op1, op2);
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM64_REG_CPSR_C, generateCarryAdd(val, op1, bodyIf), bodyIf);
		storeRegister(ARM64_REG_CPSR_V, generateOverflowAdd(val, op1, op2, bodyIf), bodyIf);
		storeRegister(ARM64_REG_CPSR_N, bodyIf.CreateICmpSLT(val, zero), bodyIf);
		storeRegister(ARM64_REG_CPSR_Z, bodyIf.CreateICmpEQ(val, zero), bodyIf);
	}
	else
	{
		throw GenericError("Arm64 ccmp, ccmn: Instruction id error");
	}

	//ELSE - Set the flags from IMM
	// We only use shifts because the final value to be stored is truncated to i1.
	storeRegister(ARM64_REG_CPSR_N, bodyElse.CreateLShr(nzvc, llvm::ConstantInt::get(nzvc->getType(), 3)), bodyElse);
	storeRegister(ARM64_REG_CPSR_Z, bodyElse.CreateLShr(nzvc, llvm::ConstantInt::get(nzvc->getType(), 2)), bodyElse);
	storeRegister(ARM64_REG_CPSR_C, bodyElse.CreateLShr(nzvc, llvm::ConstantInt::get(nzvc->getType(), 1)), bodyElse);
	storeRegister(ARM64_REG_CPSR_V, nzvc, bodyElse);

}

/**
 * ARM64_INS_CSEL
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateCsel(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb);

	auto* cond = generateInsnConditionCode(irb, ai);
	auto* val  = irb.CreateSelect(cond, op1, op2);

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_CINC, ARM64_INS_CINV, ARM64_INS_CNEG
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateCondOp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);

	auto* cond = generateInsnConditionCode(irb, ai);
	// Invert the condition
	cond = generateValueNegate(irb, cond);
	auto irbP = generateIfThenElse(cond, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	//IF - store first operand
	storeOp(ai->operands[0], op1, bodyIf);

	//ELSE
	llvm::Value *val = nullptr;
	switch(i->id)
	{
	case ARM64_INS_CINC:
		val = bodyElse.CreateAdd(op1, llvm::ConstantInt::get(op1->getType(), 1));
		break;
	case ARM64_INS_CINV:
		val = generateValueNegate(bodyElse, op1);
		break;
	case ARM64_INS_CNEG:
		val = generateValueNegate(bodyElse, op1);
		val = bodyElse.CreateAdd(val, llvm::ConstantInt::get(val->getType(), 1));
		break;
	default:
		throw GenericError("translateCondOp: Instruction id error");
		break;
	}
	storeOp(ai->operands[0], val, bodyElse);
	//ENDIF
}

/**
 * ARM64_INS_CSINC, ARM64_INS_CSINV, ARM64_INS_CSNEG
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateCondSelOp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);

	auto* cond = generateInsnConditionCode(irb, ai);
	auto irbP = generateIfThenElse(cond, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	//IF
	storeOp(ai->operands[0], op1, bodyIf);

	//ELSE
	llvm::Value *val = nullptr;
	switch(i->id)
	{
	case ARM64_INS_CSINC:
		val = bodyElse.CreateAdd(op2, llvm::ConstantInt::get(op2->getType(), 1));
		break;
	case ARM64_INS_CSINV:
		val = generateValueNegate(bodyElse, op2);
		break;
	case ARM64_INS_CSNEG:
		val = generateValueNegate(bodyElse, op2);
		val = bodyElse.CreateAdd(val, llvm::ConstantInt::get(val->getType(), 1));
		break;
	default:
		throw GenericError("translateCondSelOp: Instruction id error");
		break;
	}
	storeOp(ai->operands[0], val, bodyElse);
	//ENDIF
}

/**
 * ARM64_INS_CSET, ARM64_INS_CSETM
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateCset(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	auto* rt = getRegisterType(ai->operands[0].reg);
	auto* zero = llvm::ConstantInt::get(rt, 0);
	llvm::Value* one = nullptr;
	if (i->id == ARM64_INS_CSET)
	{
		one = llvm::ConstantInt::get(rt, 1);
	}
	else if (i->id == ARM64_INS_CSETM)
	{
		one = llvm::ConstantInt::getSigned(rt, -1);
		// 0xffffffffffffffff - one in all bits
	}
	else
	{
		throw GenericError("cset, csetm: Instruction id error");
	}

	auto* cond = generateInsnConditionCode(irb, ai);
	auto* val  = irb.CreateSelect(cond, one, zero);

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_EOR, ARM64_INS_EON
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateEor(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);

	if (i->id == ARM64_INS_EON)
	{
	    op2 = generateValueNegate(irb, op2);
	}

	auto* val = irb.CreateXor(op1, op2);

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_SXTB, ARM64_INS_SXTH, ARM64_INS_SXTW
 * ARM64_INS_UXTB, ARM64_INS_UXTH
*/
void Capstone2LlvmIrTranslatorArm64_impl::translateExtensions(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	auto* val = loadOp(ai->operands[1], irb);

	auto* i8  = llvm::IntegerType::getInt8Ty(_module->getContext());
	auto* i16 = llvm::IntegerType::getInt16Ty(_module->getContext());
	auto* i32 = llvm::IntegerType::getInt32Ty(_module->getContext());

	auto* ty  = getRegisterType(ai->operands[0].reg);

	llvm::Value* trunc = nullptr;
	switch(i->id)
	{
		case ARM64_INS_UXTB:
		{
			trunc = irb.CreateTrunc(val, i8);
			val   = irb.CreateZExt(trunc, ty);
			break;
		}
		case ARM64_INS_UXTH:
		{
			trunc = irb.CreateTrunc(val, i16);
			val   = irb.CreateZExt(trunc, ty);
			break;
		}
		/*
		case ARM64_INS_UXTW:
		{
			trunc = irb.CreateTrunc(val, i32);
			val   = irb.CreateZExt(trunc, ty);
			break;
		}
		*/
		case ARM64_INS_SXTB:
		{
			trunc = irb.CreateTrunc(val, i8);
			val   = irb.CreateSExt(trunc, ty);
			break;
		}
		case ARM64_INS_SXTH:
		{
			trunc = irb.CreateTrunc(val, i16);
			val   = irb.CreateSExt(trunc, ty);
			break;
		}
		case ARM64_INS_SXTW:
		{
			trunc = irb.CreateTrunc(val, i32);
			val   = irb.CreateSExt(trunc, ty);
			break;
		}
		default:
			throw GenericError("Arm64 translateExtension(): Unsupported extension type");
	}

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_EXTR
*/
void Capstone2LlvmIrTranslatorArm64_impl::translateExtr(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);
	auto* lsb1 = loadOp(ai->operands[3], irb);
	lsb1 = irb.CreateZExtOrTrunc(lsb1, op1->getType());
	llvm::Value* lsb2 = llvm::ConstantInt::get(op1->getType(), llvm::cast<llvm::IntegerType>(op2->getType())->getBitWidth());
	lsb2 = irb.CreateSub(lsb2, lsb1);

	auto* left_val  = irb.CreateLShr(op2, lsb1);
	auto* right_val = irb.CreateShl(op1, lsb2);

	auto* val = irb.CreateOr(left_val, right_val);

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_ORR, ARM64_INS_ORN
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateOrr(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);

	if (i->id == ARM64_INS_ORN)
	{
	    op2 = generateValueNegate(irb, op2);
	}

	auto* val = irb.CreateOr(op1, op2);

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_UDIV, ARM64_INS_SDIV
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateDiv(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	llvm::Value *val = nullptr;
	if (i->id == ARM64_INS_UDIV)
	{
		val = irb.CreateUDiv(op1, op2);
	}
	else if (i->id == ARM64_INS_SDIV)
	{
		val = irb.CreateSDiv(op1, op2);
	}

	storeOp(ai->operands[0], val, irb);

	/*
	// Zero division yelds zero as result in this case we
	// don't want undefined behaviour so we
	// check for zero division and manualy set the result, for now.
	llvm::Value* zero = llvm::ConstantInt::get(op1->getType(), 0);
	auto* cond = irb.CreateICmpEQ(op2, zero);
	auto irbP = generateIfThenElse(cond, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	//IF - store zero
	storeOp(ai->operands[0], zero, bodyIf);

	//ELSE - store result of division
	llvm::Value *val = nullptr;
	if (i->id == ARM64_INS_UDIV)
	{
		val = bodyElse.CreateUDiv(op1, op2);
	}
	else if (i->id == ARM64_INS_SDIV)
	{
		val = bodyElse.CreateSDiv(op1, op2);
	}

	storeOp(ai->operands[0], val, bodyElse);
	//ENDIF
	*/
}

/**
 * ARM64_INS_UMULH, ARM64_INS_SMULH
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateMulh(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	bool sext = true;
	if (i->id == ARM64_INS_UMULH)
	{
		sext = false;
	}
	else if (i->id == ARM64_INS_SMULH)
	{
		sext = true;
	}
	else
	{
		throw GenericError("Mulh: Unhandled instruction ID");
	}

	auto* res_type = llvm::IntegerType::getInt128Ty(_module->getContext());
	auto* op1 = loadOp(ai->operands[1], irb);
	auto* op2 = loadOp(ai->operands[2], irb);
	if (sext)
	{
		op1 = irb.CreateSExtOrTrunc(op1, res_type);
		op2 = irb.CreateSExtOrTrunc(op2, res_type);
	}
	else
	{
		op1 = irb.CreateZExtOrTrunc(op1, res_type);
		op2 = irb.CreateZExtOrTrunc(op2, res_type);
	}

	auto *val = irb.CreateMul(op1, op2);

	// Get the high bits of the result
	val = irb.CreateAShr(val, llvm::ConstantInt::get(val->getType(), 64));

	val = irb.CreateSExtOrTrunc(val, getDefaultType());

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_UMULL, ARM64_INS_SMULL
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateMull(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	bool sext = true;
	if (i->id == ARM64_INS_UMULL)
	{
		sext = false;
	}
	else if (i->id == ARM64_INS_SMULL)
	{
		sext = true;
	}
	else
	{
		throw GenericError("Mull: Unhandled instruction ID");
	}

	auto* res_type = getDefaultType();
	auto* op1 = loadOp(ai->operands[1], irb);
	auto* op2 = loadOp(ai->operands[2], irb);
	if (sext)
	{
		op1 = irb.CreateSExtOrTrunc(op1, res_type);
		op2 = irb.CreateSExtOrTrunc(op2, res_type);
	}
	else
	{
		op1 = irb.CreateZExtOrTrunc(op1, res_type);
		op2 = irb.CreateZExtOrTrunc(op2, res_type);
	}

	auto *val = irb.CreateMul(op1, op2);

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_UMADDL, ARM64_INS_SMADDL
 * ARM64_INS_UMSUBL, ARM64_INS_SMSUBL
 * ARM64_INS_UMNEGL, ARM64_INS_SMNEGL
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateMulOpl(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (3 <= ai->op_count && ai->op_count <= 4));

	bool sext = true;
	bool add_operation = true;
	bool op3_zero = false;
	switch(i->id) {
	case ARM64_INS_UMADDL:
		sext = false;
		add_operation = true;
		break;
	case ARM64_INS_SMADDL:
		sext = true;
		add_operation = true;
		break;
	case ARM64_INS_UMSUBL:
		sext = false;
		add_operation = false;
		break;
	case ARM64_INS_SMSUBL:
		sext = true;
		add_operation = false;
		break;
	case ARM64_INS_UMNEGL:
		sext = false;
		add_operation = false;
		op3_zero = true;
		break;
	case ARM64_INS_SMNEGL:
		sext = true;
		add_operation = false;
		op3_zero = true;
		break;
	default:
		throw GenericError("Maddl: Unhandled instruction ID");
	}

	auto* res_type = getDefaultType();

	auto* op1 = loadOp(ai->operands[1], irb);
	auto* op2 = loadOp(ai->operands[2], irb);
	if (sext)
	{
		op1 = irb.CreateSExtOrTrunc(op1, res_type);
		op2 = irb.CreateSExtOrTrunc(op2, res_type);
	}
	else
	{
		op1 = irb.CreateZExtOrTrunc(op1, res_type);
		op2 = irb.CreateZExtOrTrunc(op2, res_type);
	}

	auto *val = irb.CreateMul(op1, op2);

	llvm::Value* op3;
	if (op3_zero)
	{
		op3 = llvm::ConstantInt::get(res_type, 0);
	}
	else
	{
		op3 = loadOp(ai->operands[3], irb);
	}

	if (add_operation)
	{
		val = irb.CreateAdd(op3, val);
	}
	else
	{
		val = irb.CreateSub(op3, val);
	}

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_MUL, ARM64_INS_MADD, ARM64_INS_MSUB, ARM64_INS_MNEG
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateMul(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (3 <= ai->op_count && ai->op_count <= 4));

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	auto* op1 = loadOp(ai->operands[1], irb);
	auto* op2 = loadOp(ai->operands[2], irb);

	auto *val = irb.CreateMul(op1, op2);
	if (i->id == ARM64_INS_MADD)
	{
		auto* op3 = loadOp(ai->operands[3], irb);
		val = irb.CreateAdd(val, op3);
	}
	else if (i->id == ARM64_INS_MSUB)
	{
		auto* op3 = loadOp(ai->operands[3], irb);
		val = irb.CreateSub(op3, val);
	}

	if (i->id == ARM64_INS_MNEG)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		val = irb.CreateSub(zero, val);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_TBNZ, ARM64_INS_TBZ
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateTbnz(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	std::tie(op0, op1, op2) = loadOpTernary(ai, irb);

	// Get the needed bit
	auto* ext_imm = irb.CreateZExtOrTrunc(op1, op0->getType());
	auto* shifted_one = irb.CreateShl(llvm::ConstantInt::get(op0->getType(), 1), ext_imm);
	auto* test_bit = irb.CreateAnd(shifted_one, op0);

	llvm::Value* cond = nullptr;
	if (i->id == ARM64_INS_TBNZ)
	{
		cond = irb.CreateICmpNE(test_bit, llvm::ConstantInt::get(op0->getType(), 0));
	}
	else if (i->id == ARM64_INS_TBZ)
	{
		cond = irb.CreateICmpEQ(test_bit, llvm::ConstantInt::get(op0->getType(), 0));
	}
	else
	{
		throw GenericError("cbnz, cbz: Instruction id error");
	}
	generateCondBranchFunctionCall(irb, cond, op2);
}

/**
 * ARM64_INS_RET
*/
void Capstone2LlvmIrTranslatorArm64_impl::translateRet(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, ai, irb);

	// If the register operand is present
	if (ai->op_count == 1)
	{
		op0 = loadOp(ai->operands[0], irb);
	}
	else
	{
		// Default use x30
		op0 = loadRegister(ARM64_REG_LR, irb);
	}
	generateReturnFunctionCall(irb, op0);
}

/**
 * ARM64_INS_REV, ARM64_INS_RBIT
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateRev(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);

	llvm::Function* f = nullptr;
	if (i->id == ARM64_INS_REV)
	{
		f = llvm::Intrinsic::getOrInsertDeclaration(
				_module,
				llvm::Intrinsic::bswap,
				op1->getType());
	}
	else if (i->id == ARM64_INS_RBIT)
	{
		f = llvm::Intrinsic::getOrInsertDeclaration(
				_module,
				llvm::Intrinsic::bitreverse,
				op1->getType());
	}
	else
	{
		throw GenericError("Arm64 REV, RBIT: Unhandled instruction id");
	}

	auto* val = irb.CreateCall(f, {op1});
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FADD
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFAdd(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);

	auto *val = irb.CreateFAdd(op1, op2);
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FCCMP
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFCCmp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	op0 = loadOp(ai->operands[0], irb);
	op1 = loadOp(ai->operands[1], irb);
	auto* nzvc = loadOp(ai->operands[2], irb);

	auto* cond = generateInsnConditionCode(irb, ai);
	auto irbCond = generateIfThenElse(cond, irb);
	llvm::IRBuilder<> condIf(irbCond.first);
	llvm::IRBuilder<> condElse(irbCond.second);

	// IF condition holds

	// IF op1 == op2
	auto* fcmpOeq = condIf.CreateFCmpOEQ(op0, op1);
	auto irbP = generateIfThenElse(fcmpOeq, condIf);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(ARM64_REG_CPSR_N, bodyIf.getFalse(), bodyIf);
	storeRegister(ARM64_REG_CPSR_Z, bodyIf.getTrue(), bodyIf);
	storeRegister(ARM64_REG_CPSR_C, bodyIf.getTrue(), bodyIf);
	storeRegister(ARM64_REG_CPSR_V, bodyIf.getFalse(), bodyIf);

	// ELSE IF op1 < op2
	auto* fcmpOgt = bodyElse.CreateFCmpOGT(op0, op1);
	auto irbP1 = generateIfThenElse(fcmpOgt, bodyElse);
	llvm::IRBuilder<> bodyIf1(irbP1.first);
	llvm::IRBuilder<> bodyElse1(irbP1.second);

	storeRegister(ARM64_REG_CPSR_N, bodyIf1.getTrue(), bodyIf1);
	storeRegister(ARM64_REG_CPSR_Z, bodyIf1.getFalse(), bodyIf1);
	storeRegister(ARM64_REG_CPSR_C, bodyIf1.getFalse(), bodyIf1);
	storeRegister(ARM64_REG_CPSR_V, bodyIf1.getFalse(), bodyIf1);

	// ELSE IF op1 > op2
	auto* fcmpOlt = bodyElse1.CreateFCmpOLT(op0, op1);
	auto irbP2 = generateIfThenElse(fcmpOlt, bodyElse1);
	llvm::IRBuilder<> bodyIf2(irbP2.first);
	llvm::IRBuilder<> bodyElse2(irbP2.second);

	storeRegister(ARM64_REG_CPSR_N, bodyIf2.getFalse(), bodyIf2);
	storeRegister(ARM64_REG_CPSR_Z, bodyIf2.getFalse(), bodyIf2);
	storeRegister(ARM64_REG_CPSR_C, bodyIf2.getTrue(), bodyIf2);
	storeRegister(ARM64_REG_CPSR_V, bodyIf2.getFalse(), bodyIf2);

	// ELSE - NAN
	storeRegister(ARM64_REG_CPSR_N, bodyElse2.getFalse(), bodyElse2);
	storeRegister(ARM64_REG_CPSR_Z, bodyElse2.getFalse(), bodyElse2);
	storeRegister(ARM64_REG_CPSR_C, bodyElse2.getTrue(), bodyElse2);
	storeRegister(ARM64_REG_CPSR_V, bodyElse2.getTrue(), bodyElse2);

	//ELSE - Set the flags from IMM
	// We only use shifts because the final value to be stored is truncated to i1.
	storeRegister(ARM64_REG_CPSR_N, bodyElse.CreateLShr(nzvc, llvm::ConstantInt::get(nzvc->getType(), 3)), condElse);
	storeRegister(ARM64_REG_CPSR_Z, bodyElse.CreateLShr(nzvc, llvm::ConstantInt::get(nzvc->getType(), 2)), condElse);
	storeRegister(ARM64_REG_CPSR_C, bodyElse.CreateLShr(nzvc, llvm::ConstantInt::get(nzvc->getType(), 1)), condElse);
	storeRegister(ARM64_REG_CPSR_V, nzvc, condElse);

}

/**
 * ARM64_INS_FCMP
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFCmp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op0 = loadOp(ai->operands[0], irb);
	op1 = loadOp(ai->operands[1], irb);

	op1 = generateTypeConversion(irb, op1, op0->getType(), eOpConv::FPCAST_OR_BITCAST);

	// IF op1 == op2
	auto* fcmpOeq = irb.CreateFCmpOEQ(op0, op1);
	auto irbP = generateIfThenElse(fcmpOeq, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(ARM64_REG_CPSR_N, bodyIf.getFalse(), bodyIf);
	storeRegister(ARM64_REG_CPSR_Z, bodyIf.getTrue(), bodyIf);
	storeRegister(ARM64_REG_CPSR_C, bodyIf.getTrue(), bodyIf);
	storeRegister(ARM64_REG_CPSR_V, bodyIf.getFalse(), bodyIf);

	// ELSE IF op1 < op2
	auto* fcmpOgt = bodyElse.CreateFCmpOGT(op0, op1);
	auto irbP1 = generateIfThenElse(fcmpOgt, bodyElse);
	llvm::IRBuilder<> bodyIf1(irbP1.first);
	llvm::IRBuilder<> bodyElse1(irbP1.second);

	storeRegister(ARM64_REG_CPSR_N, bodyIf1.getTrue(), bodyIf1);
	storeRegister(ARM64_REG_CPSR_Z, bodyIf1.getFalse(), bodyIf1);
	storeRegister(ARM64_REG_CPSR_C, bodyIf1.getFalse(), bodyIf1);
	storeRegister(ARM64_REG_CPSR_V, bodyIf1.getFalse(), bodyIf1);

	// ELSE IF op1 > op2
	auto* fcmpOlt = bodyElse1.CreateFCmpOLT(op0, op1);
	auto irbP2 = generateIfThenElse(fcmpOlt, bodyElse1);
	llvm::IRBuilder<> bodyIf2(irbP2.first);
	llvm::IRBuilder<> bodyElse2(irbP2.second);

	storeRegister(ARM64_REG_CPSR_N, bodyIf2.getFalse(), bodyIf2);
	storeRegister(ARM64_REG_CPSR_Z, bodyIf2.getFalse(), bodyIf2);
	storeRegister(ARM64_REG_CPSR_C, bodyIf2.getTrue(), bodyIf2);
	storeRegister(ARM64_REG_CPSR_V, bodyIf2.getFalse(), bodyIf2);

	// ELSE
	storeRegister(ARM64_REG_CPSR_N, bodyElse2.getFalse(), bodyElse2);
	storeRegister(ARM64_REG_CPSR_Z, bodyElse2.getFalse(), bodyElse2);
	storeRegister(ARM64_REG_CPSR_C, bodyElse2.getTrue(), bodyElse2);
	storeRegister(ARM64_REG_CPSR_V, bodyElse2.getTrue(), bodyElse2);
}

/**
 * ARM64_INS_FCSEL
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFCsel(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);

	auto* cond = generateInsnConditionCode(irb, ai);
	auto* val  = irb.CreateSelect(cond, op1, op2);

	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FCVT
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFCvt(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);
	storeOp(ai->operands[0], op1, irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * ARM64_INS_UCVTF, ARM64_INS_SCVTF
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFCvtf(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);

	switch(i->id)
	{
	case ARM64_INS_UCVTF:
		storeOp(ai->operands[0], op1, irb, eOpConv::UITOFP_OR_FPCAST);
		break;
	case ARM64_INS_SCVTF:
		storeOp(ai->operands[0], op1, irb, eOpConv::SITOFP_OR_FPCAST);
		break;
	default:
		throw GenericError("Arm64: translateFCvtf(): Unsupported instruction id");
	}
}

/**
 * ARM64_INS_FCVTZS, ARM64_INS_FCVTZU
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFCvtz(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);

	switch(i->id)
	{
	case ARM64_INS_FCVTZU:
		op1 = irb.CreateFPToSI(op1, getRegisterType(ai->operands[0].reg));
		break;
	case ARM64_INS_FCVTZS:
		op1 = irb.CreateFPToUI(op1, getRegisterType(ai->operands[0].reg));
		break;
	default:
		throw GenericError("Arm64: translateFCvtz(): Unsupported instruction id");
	}
	storeOp(ai->operands[0], op1, irb);
}

/**
 * ARM64_INS_FDIV
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFDiv(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);

	auto *val = irb.CreateFDiv(op1, op2);
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FMADD, ARM64_INS_FNMADD
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFMadd(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);
	op3 = loadOp(ai->operands[3], irb);

	auto *val = irb.CreateFMul(op1, op2);
	val = irb.CreateFAdd(op3, val);
	if (i->id == ARM64_INS_FNMADD)
	{
		val = irb.CreateFNeg(val);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FMAX, ARM64_INS_FMIN
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFMinMax(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);

	llvm::Value* cond;
	switch(i->id)
	{
	case ARM64_INS_FMIN:
		cond = irb.CreateFCmpULE(op1, op2);
		break;
	case ARM64_INS_FMAX:
		cond = irb.CreateFCmpUGE(op1, op2);
		break;
	default:
		throw GenericError("Arm64: translateFMinMax(): Unsupported instruction id");
	}

	auto* val = irb.CreateSelect(cond, op1, op2);
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FMAXNM, ARM64_INS_FMINNM
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFMinMaxNum(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);

	llvm::Value* val = nullptr;
	llvm::Function* intrinsic = nullptr;
	switch(i->id)
	{
	case ARM64_INS_FMINNM:
		intrinsic = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::minnum, op1->getType());
		break;
	case ARM64_INS_FMAXNM:
		intrinsic = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::maxnum, op1->getType());
		break;
	default:
		throw GenericError("Arm64: translateFMinMaxNum(): Unsupported instruction id");
	}

	val = irb.CreateCall(intrinsic, {op1, op2});
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FMOV
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFMov(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (isVectorRegister(ai->operands[0]) || isVectorRegister(ai->operands[1]))
	{
		// We want this behavior in cases when move destination is vector register
		generatePseudoInstruction(i, ai, irb);
		return;
	}

	op1 = loadOp(ai->operands[1], irb);
	if (ai->operands[1].type == ARM64_OP_FP)
	{
		op1 = generateTypeConversion(irb, op1, getRegisterType(ai->operands[0].reg), eOpConv::FPCAST_OR_BITCAST);
	}
	else
	{
		op1 = irb.CreateBitCast(op1, getRegisterType(ai->operands[0].reg));
	}

	storeOp(ai->operands[0], op1, irb);
}

/**
 * ARM64_INS_MOVI
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateMovi(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	if (!isFPRegister(ai->operands[0]))
	{
		// We want this behavior in cases when move destination is vector register
		generatePseudoInstruction(i, ai, irb);
		return;
	}

	op1 = loadOp(ai->operands[1], irb);
	storeOp(ai->operands[0], op1, irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * ARM64_INS_FMUL, ARM64_INS_FNMUL
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFMul(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);

	auto *val = irb.CreateFMul(op1, op2);
	if (i->id == ARM64_INS_FNMUL)
	{
		val = irb.CreateFNeg(val);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FMSUB, ARM64_INS_FNMSUB
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFMsub(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, ai, irb);

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);
	op3 = loadOp(ai->operands[3], irb);

	auto *val = irb.CreateFMul(op1, op2);
	val = irb.CreateFSub(op3, val);
	if (i->id == ARM64_INS_FNMSUB)
	{
		val = irb.CreateFNeg(val);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FSUB
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFSub(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	op1 = loadOp(ai->operands[1], irb);
	op2 = loadOp(ai->operands[2], irb);

	auto *val = irb.CreateFSub(op1, op2);
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM64_INS_FNEG, ARM64_INS_FABS, ARM64_INS_FSQRT
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateFUnaryOp(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (ifVectorGeneratePseudo(i, ai, irb))
	{
	    return;
	}

	op1 = loadOp(ai->operands[1], irb);

	llvm::Value* val = nullptr;
	llvm::Function* intrinsic = nullptr;
	switch(i->id)
	{
	case ARM64_INS_FNEG:
		val = irb.CreateFNeg(op1);
		break;
	case ARM64_INS_FABS:
		intrinsic = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, op1->getType());
		val = irb.CreateCall(intrinsic, {op1});
		break;
	case ARM64_INS_FSQRT:
		intrinsic = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sqrt, op1->getType());
		val = irb.CreateCall(intrinsic, {op1});
		break;
	default:
		throw GenericError("Arm64: translateFUnary(): Unsupported instruction id");
	}

	storeOp(ai->operands[0], val, irb);
}

} // namespace capstone2llvmir
} // namespace retdec
