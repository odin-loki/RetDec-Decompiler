/**
 * @file src/capstone2llvmir/x86/x86.cpp
 * @brief X86 implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cmath>
#include <iomanip>
#include <memory>

#include "capstone2llvmir/x86/x86_impl.h"

namespace retdec {
namespace capstone2llvmir {

Capstone2LlvmIrTranslatorX86_impl::Capstone2LlvmIrTranslatorX86_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(CS_ARCH_X86, basic, extra, m),
		_reg2parentMap(X86_REG_ENDING, X86_REG_INVALID)
{
	// This needs to be called from concrete's class ctor, not abstract's
	// class ctor, so that virtual table is properly initialized.
	initialize();
}

//
//==============================================================================
// Mode query & modification methods - from Capstone2LlvmIrTranslator.
//==============================================================================
//

/**
 * x86 is special.
 *
 * If the original basic mode was not set yet (CS_MODE_LITTLE_ENDIAN), this
 * returns all the modes that can be used to initialize x86 translator.
 *
 * If it was set, x86 allows to change basic mode only to modes lower than the
 * original initialization mode an back to original mode
 * (CS_MODE_16 < CS_MODE_32 < CS_MODE_64). This is because the original mode is
 * used to initialize module's environment with registers and other specific
 * features. It is possible to simulate lower modes in environments created for
 * higher modes (e.g. get ax register from eax), but not the other way around
 * (e.g. get rax from eax).
 */
bool Capstone2LlvmIrTranslatorX86_impl::isAllowedBasicMode(cs_mode m)
{
	if (_origBasicMode == CS_MODE_LITTLE_ENDIAN)
	{
		return m == CS_MODE_16 || m == CS_MODE_32 || m == CS_MODE_64;
	}
	else if (_origBasicMode == CS_MODE_16)
	{
		return m == CS_MODE_16;
	}
	else if (_origBasicMode == CS_MODE_32)
	{
		return m == CS_MODE_16 || m == CS_MODE_32;
	}
	else if (_origBasicMode == CS_MODE_64)
	{
		return m == CS_MODE_16 || m == CS_MODE_32 || m == CS_MODE_64;
	}
	else
	{
		return false;
	}
}

bool Capstone2LlvmIrTranslatorX86_impl::isAllowedExtraMode(cs_mode m)
{
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN;
}

uint32_t Capstone2LlvmIrTranslatorX86_impl::getArchByteSize()
{
	switch (_origBasicMode)
	{
		case CS_MODE_16: return 2;
		case CS_MODE_32: return 4;
		case CS_MODE_64: return 8;
		default:
		{
			throw GenericError("Unhandled mode in getArchByteSize().");
			break;
		}
	}
}

//
//==============================================================================
// LLVM related getters and query methods - from Capstone2LlvmIrTranslator.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorX86_impl::isAnyPseudoFunction(llvm::Function* f) const
{
	return Capstone2LlvmIrTranslator_impl::isAnyPseudoFunction(f)
			|| isX87DataStoreFunction(f)
			|| isX87DataLoadFunction(f);
}

bool Capstone2LlvmIrTranslatorX86_impl::isAnyPseudoFunctionCall(
		llvm::CallInst* c) const
{
	return Capstone2LlvmIrTranslator_impl::isAnyPseudoFunctionCall(c)
			|| isX87DataStoreFunctionCall(c)
			|| isX87DataLoadFunctionCall(c);
}

//
//==============================================================================
// x86 specialization methods - from Capstone2LlvmIrTranslatorX86
//==============================================================================
//

bool Capstone2LlvmIrTranslatorX86_impl::isX87DataStoreFunction(llvm::Function* f) const
{
	return f == _x87DataStoreFunction;
}

bool Capstone2LlvmIrTranslatorX86_impl::isX87DataStoreFunctionCall(llvm::CallInst* c) const
{
	return c && isX87DataStoreFunction(c->getCalledFunction());
}

llvm::Function* Capstone2LlvmIrTranslatorX86_impl::getX87DataStoreFunction() const
{
	return _x87DataStoreFunction;
}

bool Capstone2LlvmIrTranslatorX86_impl::isX87DataLoadFunction(llvm::Function* f) const
{
	return f == _x87DataLoadFunction;
}

bool Capstone2LlvmIrTranslatorX86_impl::isX87DataLoadFunctionCall(llvm::CallInst* c) const
{
	return c && isX87DataLoadFunction(c->getCalledFunction());
}

llvm::Function* Capstone2LlvmIrTranslatorX86_impl::getX87DataLoadFunction() const
{
	return _x87DataLoadFunction;
}

/**
 * All registers from the original Capstone @c x86_reg should be
 * in @c _reg2parentMap. Our added registers are not there, but all of them
 * should map to themselves, i.e. if register not in map, we return its number.
 */
uint32_t Capstone2LlvmIrTranslatorX86_impl::getParentRegister(uint32_t r) const
{
	return r < _reg2parentMap.size()
			? (_reg2parentMap[r] != X86_REG_INVALID ? _reg2parentMap[r] : r)
			: r;
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorX86_impl::generateEnvironmentArchSpecific()
{
	generateX87RegLoadStoreFunctions();
}

void Capstone2LlvmIrTranslatorX86_impl::generateDataLayout()
{
	switch (_origBasicMode)
	{
		case CS_MODE_16:
		{
			_module->setDataLayout("e-p:32:32-f64:32:64-f80:32-n8:16:32-S128"); // clang -m16
			break;
		}
		case CS_MODE_32:
		{
			_module->setDataLayout("e-p:32:32-f64:32:64-f80:32-n8:16:32-S128"); // clang -m32
			break;
		}
		case CS_MODE_64:
		{
			_module->setDataLayout("e-m:e-p:64:64-i64:64-f80:128-n8:16:32:64-S128"); // clang
			break;
		}
		default:
		{
			throw GenericError("Unhandled mode in getStackPointerRegister().");
			break;
		}
	}
}

void Capstone2LlvmIrTranslatorX86_impl::generateRegisters()
{
	generateRegistersCommon();

	switch (_origBasicMode)
	{
		case CS_MODE_16: generateRegisters16(); break;
		case CS_MODE_32: generateRegisters32(); break;
		case CS_MODE_64: generateRegisters64(); break;
		default:
		{
			throw GenericError("Unhandled mode in generateRegisters().");
			break;
		}
	}
}

uint32_t Capstone2LlvmIrTranslatorX86_impl::getCarryRegister()
{
	return X86_REG_CF;
}

void Capstone2LlvmIrTranslatorX86_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_x86* xi = &d->x86;

	auto fIt = _i2fm.find(i->id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;
		(this->*f)(i, xi, irb);
	}
	else
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, xi, irb);
	}
}

//
//==============================================================================
// x86-specific methods.
//==============================================================================
//

void Capstone2LlvmIrTranslatorX86_impl::generateX87RegLoadStoreFunctions()
{
	std::vector<llvm::Type*> dsp = {
			llvm::Type::getIntNTy(_module->getContext(), 3),
			llvm::Type::getX86_FP80Ty(_module->getContext())};
	auto* dsft = llvm::FunctionType::get(
			llvm::Type::getVoidTy(_module->getContext()),
			dsp,
			false);
	_x87DataStoreFunction = llvm::Function::Create(
			dsft,
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"",
			_module);

	auto* dlft = llvm::FunctionType::get(
			llvm::Type::getX86_FP80Ty(_module->getContext()),
			{llvm::Type::getIntNTy(_module->getContext(), 3)},
			false);
	_x87DataLoadFunction = llvm::Function::Create(
			dlft,
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"",
			_module);
}

uint32_t Capstone2LlvmIrTranslatorX86_impl::getAccumulatorRegister(std::size_t size)
{
	switch (size)
	{
		case 1: return X86_REG_AL;
		case 2: return X86_REG_AX;
		case 4: return X86_REG_EAX;
		case 8: return X86_REG_RAX;
		default: throw GenericError("Unhandled accumulator register.");
	}
}

uint32_t Capstone2LlvmIrTranslatorX86_impl::getStackPointerRegister()
{
	switch (_origBasicMode)
	{
		case CS_MODE_16: return X86_REG_SP;
		case CS_MODE_32: return X86_REG_ESP;
		case CS_MODE_64: return X86_REG_RSP;
		default:
		{
			throw GenericError("Unhandled mode in getStackPointerRegister().");
			break;
		}
	}
}

uint32_t Capstone2LlvmIrTranslatorX86_impl::getBasePointerRegister()
{
	switch (_origBasicMode)
	{
		case CS_MODE_16: return X86_REG_BP;
		case CS_MODE_32: return X86_REG_EBP;
		case CS_MODE_64: return X86_REG_RBP;
		default:
		{
			throw GenericError("Unhandled mode in getBasePointerRegister().");
			break;
		}
	}
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::getCurrentPc(cs_insn* i)
{
	return getNextInsnAddress(i);
}

void Capstone2LlvmIrTranslatorX86_impl::generateRegistersCommon()
{
	// Flag registers (x86_reg_rflags).
	//
	createRegister(X86_REG_CF, _regLt);
	createRegister(X86_REG_PF, _regLt);
	createRegister(X86_REG_AF, _regLt);
	createRegister(X86_REG_ZF, _regLt);
	createRegister(X86_REG_SF, _regLt);
	createRegister(X86_REG_TF, _regLt);
	createRegister(X86_REG_IF, _regLt);
	createRegister(X86_REG_DF, _regLt);
	createRegister(X86_REG_OF, _regLt);
	createRegister(X86_REG_IOPL, _regLt);
	createRegister(X86_REG_NT, _regLt);
	createRegister(X86_REG_RF, _regLt);
	createRegister(X86_REG_VM, _regLt);
	createRegister(X86_REG_AC, _regLt);
	createRegister(X86_REG_VIF, _regLt);
	createRegister(X86_REG_VIP, _regLt);
	createRegister(X86_REG_ID, _regLt);

	createRegister(X86_REG_EFLAGS, _regLt);

	// Segment registers.
	//
	createRegister(X86_REG_SS, _regLt);
	createRegister(X86_REG_CS, _regLt);
	createRegister(X86_REG_DS, _regLt);
	createRegister(X86_REG_ES, _regLt);
	createRegister(X86_REG_FS, _regLt);
	createRegister(X86_REG_GS, _regLt);

	// x87 FPU data registers.
	//
	createRegister(X86_REG_ST0, _regLt);
	createRegister(X86_REG_ST1, _regLt);
	createRegister(X86_REG_ST2, _regLt);
	createRegister(X86_REG_ST3, _regLt);
	createRegister(X86_REG_ST4, _regLt);
	createRegister(X86_REG_ST5, _regLt);
	createRegister(X86_REG_ST6, _regLt);
	createRegister(X86_REG_ST7, _regLt);

	// x87 FPU status registers (x87_reg_status).
	//
	createRegister(X87_REG_IE, _regLt);
	createRegister(X87_REG_DE, _regLt);
	createRegister(X87_REG_ZE, _regLt);
	createRegister(X87_REG_OE, _regLt);
	createRegister(X87_REG_UE, _regLt);
	createRegister(X87_REG_PE, _regLt);
	createRegister(X87_REG_SF, _regLt);
	createRegister(X87_REG_ES, _regLt);
	createRegister(X87_REG_C0, _regLt);
	createRegister(X87_REG_C1, _regLt);
	createRegister(X87_REG_C2, _regLt);
	createRegister(X87_REG_C3, _regLt);
	createRegister(X87_REG_TOP, _regLt);
	createRegister(X87_REG_B, _regLt);

	// x87 FPU control registers (x87_reg_control).
	//
	createRegister(X87_REG_IM, _regLt);
	createRegister(X87_REG_DM, _regLt);
	createRegister(X87_REG_ZM, _regLt);
	createRegister(X87_REG_OM, _regLt);
	createRegister(X87_REG_UM, _regLt);
	createRegister(X87_REG_PM, _regLt);
	createRegister(X87_REG_PC, _regLt);
	createRegister(X87_REG_RC, _regLt);
	createRegister(X87_REG_X, _regLt);

	// 64-bit FP registers.
	//
	createRegister(X86_REG_FP0, _regLt);
	createRegister(X86_REG_FP1, _regLt);
	createRegister(X86_REG_FP2, _regLt);
	createRegister(X86_REG_FP3, _regLt);
	createRegister(X86_REG_FP4, _regLt);
	createRegister(X86_REG_FP5, _regLt);
	createRegister(X86_REG_FP6, _regLt);
	createRegister(X86_REG_FP7, _regLt);

	// Opmask registers (AVX-512).
	//
	createRegister(X86_REG_K0, _regLt);
	createRegister(X86_REG_K1, _regLt);
	createRegister(X86_REG_K2, _regLt);
	createRegister(X86_REG_K3, _regLt);
	createRegister(X86_REG_K4, _regLt);
	createRegister(X86_REG_K5, _regLt);
	createRegister(X86_REG_K6, _regLt);
	createRegister(X86_REG_K7, _regLt);

	// MMX.
	//
	createRegister(X86_REG_MM0, _regLt);
	createRegister(X86_REG_MM1, _regLt);
	createRegister(X86_REG_MM2, _regLt);
	createRegister(X86_REG_MM3, _regLt);
	createRegister(X86_REG_MM4, _regLt);
	createRegister(X86_REG_MM5, _regLt);
	createRegister(X86_REG_MM6, _regLt);
	createRegister(X86_REG_MM7, _regLt);

	// The upper halves of the YMM registers. YMM = YMMH:XMM, and the low
	// half is the XMM register below, which is why an SSE write and an AVX
	// read of the same register now see each other.
	createRegister(X86_REG_YMM0_HI, _regLt);
	createRegister(X86_REG_YMM1_HI, _regLt);
	createRegister(X86_REG_YMM2_HI, _regLt);
	createRegister(X86_REG_YMM3_HI, _regLt);
	createRegister(X86_REG_YMM4_HI, _regLt);
	createRegister(X86_REG_YMM5_HI, _regLt);
	createRegister(X86_REG_YMM6_HI, _regLt);
	createRegister(X86_REG_YMM7_HI, _regLt);
	createRegister(X86_REG_YMM8_HI, _regLt);
	createRegister(X86_REG_YMM9_HI, _regLt);
	createRegister(X86_REG_YMM10_HI, _regLt);
	createRegister(X86_REG_YMM11_HI, _regLt);
	createRegister(X86_REG_YMM12_HI, _regLt);
	createRegister(X86_REG_YMM13_HI, _regLt);
	createRegister(X86_REG_YMM14_HI, _regLt);
	createRegister(X86_REG_YMM15_HI, _regLt);
	createRegister(X86_REG_YMM16_HI, _regLt);
	createRegister(X86_REG_YMM17_HI, _regLt);
	createRegister(X86_REG_YMM18_HI, _regLt);
	createRegister(X86_REG_YMM19_HI, _regLt);
	createRegister(X86_REG_YMM20_HI, _regLt);
	createRegister(X86_REG_YMM21_HI, _regLt);
	createRegister(X86_REG_YMM22_HI, _regLt);
	createRegister(X86_REG_YMM23_HI, _regLt);
	createRegister(X86_REG_YMM24_HI, _regLt);
	createRegister(X86_REG_YMM25_HI, _regLt);
	createRegister(X86_REG_YMM26_HI, _regLt);
	createRegister(X86_REG_YMM27_HI, _regLt);
	createRegister(X86_REG_YMM28_HI, _regLt);
	createRegister(X86_REG_YMM29_HI, _regLt);
	createRegister(X86_REG_YMM30_HI, _regLt);
	createRegister(X86_REG_YMM31_HI, _regLt);

	// The top 256 bits of each ZMM register. ZMM = ZMMH:YMMH:XMM.
	createRegister(X86_REG_ZMM0_HI, _regLt);
	createRegister(X86_REG_ZMM1_HI, _regLt);
	createRegister(X86_REG_ZMM2_HI, _regLt);
	createRegister(X86_REG_ZMM3_HI, _regLt);
	createRegister(X86_REG_ZMM4_HI, _regLt);
	createRegister(X86_REG_ZMM5_HI, _regLt);
	createRegister(X86_REG_ZMM6_HI, _regLt);
	createRegister(X86_REG_ZMM7_HI, _regLt);
	createRegister(X86_REG_ZMM8_HI, _regLt);
	createRegister(X86_REG_ZMM9_HI, _regLt);
	createRegister(X86_REG_ZMM10_HI, _regLt);
	createRegister(X86_REG_ZMM11_HI, _regLt);
	createRegister(X86_REG_ZMM12_HI, _regLt);
	createRegister(X86_REG_ZMM13_HI, _regLt);
	createRegister(X86_REG_ZMM14_HI, _regLt);
	createRegister(X86_REG_ZMM15_HI, _regLt);
	createRegister(X86_REG_ZMM16_HI, _regLt);
	createRegister(X86_REG_ZMM17_HI, _regLt);
	createRegister(X86_REG_ZMM18_HI, _regLt);
	createRegister(X86_REG_ZMM19_HI, _regLt);
	createRegister(X86_REG_ZMM20_HI, _regLt);
	createRegister(X86_REG_ZMM21_HI, _regLt);
	createRegister(X86_REG_ZMM22_HI, _regLt);
	createRegister(X86_REG_ZMM23_HI, _regLt);
	createRegister(X86_REG_ZMM24_HI, _regLt);
	createRegister(X86_REG_ZMM25_HI, _regLt);
	createRegister(X86_REG_ZMM26_HI, _regLt);
	createRegister(X86_REG_ZMM27_HI, _regLt);
	createRegister(X86_REG_ZMM28_HI, _regLt);
	createRegister(X86_REG_ZMM29_HI, _regLt);
	createRegister(X86_REG_ZMM30_HI, _regLt);
	createRegister(X86_REG_ZMM31_HI, _regLt);

	// XMM.
	createRegister(X86_REG_XMM0, _regLt);
	createRegister(X86_REG_XMM1, _regLt);
	createRegister(X86_REG_XMM2, _regLt);
	createRegister(X86_REG_XMM3, _regLt);
	createRegister(X86_REG_XMM4, _regLt);
	createRegister(X86_REG_XMM5, _regLt);
	createRegister(X86_REG_XMM6, _regLt);
	createRegister(X86_REG_XMM7, _regLt);
	createRegister(X86_REG_XMM8, _regLt);
	createRegister(X86_REG_XMM9, _regLt);
	createRegister(X86_REG_XMM10, _regLt);
	createRegister(X86_REG_XMM11, _regLt);
	createRegister(X86_REG_XMM12, _regLt);
	createRegister(X86_REG_XMM13, _regLt);
	createRegister(X86_REG_XMM14, _regLt);
	createRegister(X86_REG_XMM15, _regLt);
	createRegister(X86_REG_XMM16, _regLt);
	createRegister(X86_REG_XMM17, _regLt);
	createRegister(X86_REG_XMM18, _regLt);
	createRegister(X86_REG_XMM19, _regLt);
	createRegister(X86_REG_XMM20, _regLt);
	createRegister(X86_REG_XMM21, _regLt);
	createRegister(X86_REG_XMM22, _regLt);
	createRegister(X86_REG_XMM23, _regLt);
	createRegister(X86_REG_XMM24, _regLt);
	createRegister(X86_REG_XMM25, _regLt);
	createRegister(X86_REG_XMM26, _regLt);
	createRegister(X86_REG_XMM27, _regLt);
	createRegister(X86_REG_XMM28, _regLt);
	createRegister(X86_REG_XMM29, _regLt);
	createRegister(X86_REG_XMM30, _regLt);
	createRegister(X86_REG_XMM31, _regLt);

	// YMM.
	createRegister(X86_REG_YMM0, _regLt);
	createRegister(X86_REG_YMM1, _regLt);
	createRegister(X86_REG_YMM2, _regLt);
	createRegister(X86_REG_YMM3, _regLt);
	createRegister(X86_REG_YMM4, _regLt);
	createRegister(X86_REG_YMM5, _regLt);
	createRegister(X86_REG_YMM6, _regLt);
	createRegister(X86_REG_YMM7, _regLt);
	createRegister(X86_REG_YMM8, _regLt);
	createRegister(X86_REG_YMM9, _regLt);
	createRegister(X86_REG_YMM10, _regLt);
	createRegister(X86_REG_YMM11, _regLt);
	createRegister(X86_REG_YMM12, _regLt);
	createRegister(X86_REG_YMM13, _regLt);
	createRegister(X86_REG_YMM14, _regLt);
	createRegister(X86_REG_YMM15, _regLt);
	createRegister(X86_REG_YMM16, _regLt);
	createRegister(X86_REG_YMM17, _regLt);
	createRegister(X86_REG_YMM18, _regLt);
	createRegister(X86_REG_YMM19, _regLt);
	createRegister(X86_REG_YMM20, _regLt);
	createRegister(X86_REG_YMM21, _regLt);
	createRegister(X86_REG_YMM22, _regLt);
	createRegister(X86_REG_YMM23, _regLt);
	createRegister(X86_REG_YMM24, _regLt);
	createRegister(X86_REG_YMM25, _regLt);
	createRegister(X86_REG_YMM26, _regLt);
	createRegister(X86_REG_YMM27, _regLt);
	createRegister(X86_REG_YMM28, _regLt);
	createRegister(X86_REG_YMM29, _regLt);
	createRegister(X86_REG_YMM30, _regLt);
	createRegister(X86_REG_YMM31, _regLt);

	// ZMM.
	createRegister(X86_REG_ZMM0, _regLt);
	createRegister(X86_REG_ZMM1, _regLt);
	createRegister(X86_REG_ZMM2, _regLt);
	createRegister(X86_REG_ZMM3, _regLt);
	createRegister(X86_REG_ZMM4, _regLt);
	createRegister(X86_REG_ZMM5, _regLt);
	createRegister(X86_REG_ZMM6, _regLt);
	createRegister(X86_REG_ZMM7, _regLt);
	createRegister(X86_REG_ZMM8, _regLt);
	createRegister(X86_REG_ZMM9, _regLt);
	createRegister(X86_REG_ZMM10, _regLt);
	createRegister(X86_REG_ZMM11, _regLt);
	createRegister(X86_REG_ZMM12, _regLt);
	createRegister(X86_REG_ZMM13, _regLt);
	createRegister(X86_REG_ZMM14, _regLt);
	createRegister(X86_REG_ZMM15, _regLt);
	createRegister(X86_REG_ZMM16, _regLt);
	createRegister(X86_REG_ZMM17, _regLt);
	createRegister(X86_REG_ZMM18, _regLt);
	createRegister(X86_REG_ZMM19, _regLt);
	createRegister(X86_REG_ZMM20, _regLt);
	createRegister(X86_REG_ZMM21, _regLt);
	createRegister(X86_REG_ZMM22, _regLt);
	createRegister(X86_REG_ZMM23, _regLt);
	createRegister(X86_REG_ZMM24, _regLt);
	createRegister(X86_REG_ZMM25, _regLt);
	createRegister(X86_REG_ZMM26, _regLt);
	createRegister(X86_REG_ZMM27, _regLt);
	createRegister(X86_REG_ZMM28, _regLt);
	createRegister(X86_REG_ZMM29, _regLt);
	createRegister(X86_REG_ZMM30, _regLt);
	createRegister(X86_REG_ZMM31, _regLt);

	// BND
	createRegister(X86_REG_BND0, _regLt);
	createRegister(X86_REG_BND1, _regLt);
	createRegister(X86_REG_BND2, _regLt);
	createRegister(X86_REG_BND3, _regLt);

	// Debug registers.
	//
	createRegister(X86_REG_DR0, _regLt);
	createRegister(X86_REG_DR1, _regLt);
	createRegister(X86_REG_DR2, _regLt);
	createRegister(X86_REG_DR3, _regLt);
	createRegister(X86_REG_DR4, _regLt);
	createRegister(X86_REG_DR5, _regLt);
	createRegister(X86_REG_DR6, _regLt);
	createRegister(X86_REG_DR7, _regLt);
	createRegister(X86_REG_DR8, _regLt);
	createRegister(X86_REG_DR9, _regLt);
	createRegister(X86_REG_DR10, _regLt);
	createRegister(X86_REG_DR11, _regLt);
	createRegister(X86_REG_DR12, _regLt);
	createRegister(X86_REG_DR13, _regLt);
	createRegister(X86_REG_DR14, _regLt);
	createRegister(X86_REG_DR15, _regLt);

	// Control registers.
	//
	createRegister(X86_REG_CR0, _regLt);
	createRegister(X86_REG_CR1, _regLt);
	createRegister(X86_REG_CR2, _regLt);
	createRegister(X86_REG_CR3, _regLt);
	createRegister(X86_REG_CR4, _regLt);
	createRegister(X86_REG_CR5, _regLt);
	createRegister(X86_REG_CR6, _regLt);
	createRegister(X86_REG_CR7, _regLt);
	createRegister(X86_REG_CR8, _regLt);
	createRegister(X86_REG_CR9, _regLt);
	createRegister(X86_REG_CR10, _regLt);
	createRegister(X86_REG_CR11, _regLt);
	createRegister(X86_REG_CR12, _regLt);
	createRegister(X86_REG_CR13, _regLt);
	createRegister(X86_REG_CR14, _regLt);
	createRegister(X86_REG_CR15, _regLt);

	createRegister(X86_REG_FPSW, _regLt);
}

void Capstone2LlvmIrTranslatorX86_impl::generateRegisters16()
{
	// General-purpose registers.
	//
	createRegister(X86_REG_AX, _regLt);
	createRegister(X86_REG_CX, _regLt);
	createRegister(X86_REG_DX, _regLt);
	createRegister(X86_REG_BX, _regLt);
	createRegister(X86_REG_SP, _regLt);
	createRegister(X86_REG_BP, _regLt);
	createRegister(X86_REG_SI, _regLt);
	createRegister(X86_REG_DI, _regLt);

	// Instruction pointer register.
	//
	createRegister(X86_REG_IP, _regLt);
}

void Capstone2LlvmIrTranslatorX86_impl::generateRegisters32()
{
	auto* i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
	auto* i32Zero = llvm::ConstantInt::get(i32, 0);

	// General-purpose registers.
	//
	createRegister(X86_REG_EAX, _regLt);
	createRegister(X86_REG_ECX, _regLt);
	createRegister(X86_REG_EDX, _regLt);
	createRegister(X86_REG_EBX, _regLt);
	createRegister(X86_REG_ESP, _regLt);
	createRegister(X86_REG_EBP, _regLt);
	createRegister(X86_REG_ESI, _regLt);
	createRegister(X86_REG_EDI, _regLt);

	// Instruction pointer register.
	//
	createRegister(X86_REG_EIP, _regLt);

	// Other.
	//
	// Pseudo register eval to 0.
	createRegister(X86_REG_EIZ, _regLt, i32Zero);
}

void Capstone2LlvmIrTranslatorX86_impl::generateRegisters64()
{
	auto* i64 = llvm::IntegerType::getInt64Ty(_module->getContext());
	auto* i64Zero = llvm::ConstantInt::get(i64, 0);

	// General-purpose registers.
	//
	// bits               64                  8,    8,   16,   32
	createRegister(X86_REG_RAX, _regLt); //   ah,   al,   ax,  eax
	createRegister(X86_REG_RCX, _regLt); //   ch,   cl,   cx,  ecx
	createRegister(X86_REG_RDX, _regLt); //   dh,   dl,   dx,  edx
	createRegister(X86_REG_RBX, _regLt); //   bh,   bl,   bx,  ebx
	createRegister(X86_REG_RSP, _regLt); // ----,  spl,   sp,  esp
	createRegister(X86_REG_RBP, _regLt); // ----,  bpl,   bp,  ebp
	createRegister(X86_REG_RSI, _regLt); // ----,  sil,   si,  esi
	createRegister(X86_REG_RDI, _regLt); // ----,  dil,   di,  edi
	createRegister(X86_REG_R8, _regLt);  // ----,  r8b,  r8w,  r8d
	createRegister(X86_REG_R9, _regLt);  // ----,  r9b,  r9w,  r9d
	createRegister(X86_REG_R10, _regLt); // ----, r10b, r10w, r10d
	createRegister(X86_REG_R11, _regLt); // ----, r11b, r11w, r11d
	createRegister(X86_REG_R12, _regLt); // ----, r12b, r12w, r12d
	createRegister(X86_REG_R13, _regLt); // ----, r13b, r13w, r13d
	createRegister(X86_REG_R14, _regLt); // ----, r14b, r14w, r14d
	createRegister(X86_REG_R15, _regLt); // ----, r15b, r15w, r15d

	// Instruction pointer register.
	//
	createRegister(X86_REG_RIP, _regLt); // ----, ----,   ip,  eip

	// Other.
	//
	// Pseudo register eval to 0.
	createRegister(X86_REG_RIZ, _regLt, i64Zero); // ----, ----, ----,  eiz
}

//
//==============================================================================
// Translation helper methods.
//==============================================================================
//

/**
 * True for a YMM or ZMM register NAME. The storage behind such a name is three
 * other globals -- ZMM = ZMMH:YMMH:XMM -- and the standalone i256 YMMn and
 * i512 ZMMn globals in the register file are not it.
 *
 * Without this, the two paths disagree: a translated `vmovdqu64 zmm1, zmm2`
 * writes the slices while the pseudo-assembly fallback for an instruction
 * this does not model reads the i512 global, which nothing ever wrote. The
 * fallback would then be reading a register that is permanently zero and
 * looking, in the output, exactly like one that had been read correctly.
 */
bool Capstone2LlvmIrTranslatorX86_impl::isWideVectorRegister(uint32_t r) const
{
	return (X86_REG_YMM0 <= r && r <= X86_REG_YMM31) || (X86_REG_ZMM0 <= r && r <= X86_REG_ZMM31);
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadWideVectorRegister(uint32_t r, llvm::IRBuilder<>& irb)
{
	bool zmm = X86_REG_ZMM0 <= r && r <= X86_REG_ZMM31;
	unsigned n = zmm ? r - X86_REG_ZMM0 : r - X86_REG_YMM0;
	unsigned bits = zmm ? 512 : 256;
	auto* ty = irb.getIntNTy(bits);

	llvm::Value* ret = irb.CreateZExt(loadRegister(X86_REG_XMM0 + n, irb), ty);
	ret = irb.CreateOr(
		ret,
		irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_YMM0_HI + n, irb), ty), llvm::ConstantInt::get(ty, 128)));
	if (zmm)
	{
		ret = irb.CreateOr(
			ret,
			irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_ZMM0_HI + n, irb), ty), llvm::ConstantInt::get(ty, 256)));
	}

	return ret;
}

llvm::StoreInst*
Capstone2LlvmIrTranslatorX86_impl::storeWideVectorRegister(uint32_t r, llvm::Value* val, llvm::IRBuilder<>& irb)
{
	bool zmm = X86_REG_ZMM0 <= r && r <= X86_REG_ZMM31;
	unsigned n = zmm ? r - X86_REG_ZMM0 : r - X86_REG_YMM0;
	unsigned bits = zmm ? 512 : 256;
	auto* ty = irb.getIntNTy(bits);
	auto* i128 = irb.getInt128Ty();

	val = irb.CreateZExtOrTrunc(val, ty);
	llvm::StoreInst* ret = storeRegister(X86_REG_XMM0 + n, irb.CreateTrunc(val, i128), irb);
	ret = storeRegister(
		X86_REG_YMM0_HI + n, irb.CreateTrunc(irb.CreateLShr(val, llvm::ConstantInt::get(ty, 128)), i128), irb);
	if (zmm)
	{
		ret = storeRegister(
			X86_REG_ZMM0_HI + n,
			irb.CreateTrunc(irb.CreateLShr(val, llvm::ConstantInt::get(ty, 256)), irb.getIntNTy(256)),
			irb);
	}
	else
	{
		// A 256-bit write zeroes bits 511:256. Nothing but a VEX or EVEX
		// instruction can write a YMM register at all -- a legacy SSE write
		// goes to the XMM name -- so every value arriving here carries that
		// rule. Measured: `vmovaps ymm0, ymm1` leaves zmm0[511:256] zero,
		// and this left whatever was there. storeVectorOp's YMM branch had
		// the rule; this path, which is where storeRegister sends a YMM
		// destination, did not.
		ret = storeRegister(X86_REG_ZMM0_HI + n, llvm::ConstantInt::get(irb.getIntNTy(256), 0), irb);
	}

	return ret;
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == X86_REG_INVALID)
	{
		return nullptr;
	}

	if (isWideVectorRegister(r))
	{
		return loadWideVectorRegister(r, irb);
	}

	auto* rt = getRegisterType(r);
	auto pr = getParentRegister(r);
	auto* reg = getRegister(pr);
	if (reg == nullptr)
	{
		throw GenericError("Capstone2LlvmIrTranslatorX86_impl() unhandled reg.");
	}

	llvm::Value* ret = nullptr;
	if (pr == X86_REG_RIP
			|| pr == X86_REG_EIP
			|| pr == X86_REG_IP)
	{
		ret = getCurrentPc(_insn);
	}
	else
	{
		ret = createLoad(irb, reg);

		if (r != pr)
// TODO: We want to do this for register storing, but probably not here?
//				&& getRegisterBitSize(pr) != 64) // Do not trunc for 64-bit target regs.
		{
			// Special handling - we need a right shift to get bits 8..15 to 0..7.
			//
			if (r == X86_REG_AH
					|| r == X86_REG_CH
					|| r == X86_REG_DH
					|| r == X86_REG_BH)
			{
				ret = irb.CreateLShr(ret, 8);
			}

			// Only truncate when the source is actually wider than the target;
			// a no-op trunc (same type) is invalid LLVM IR.
			if (ret->getType() != rt)
			{
				auto* retIntTy = llvm::dyn_cast<llvm::IntegerType>(ret->getType());
				auto* rtIntTy  = llvm::dyn_cast<llvm::IntegerType>(rt);
				if (retIntTy && rtIntTy
						&& retIntTy->getBitWidth() > rtIntTy->getBitWidth())
				{
					ret = irb.CreateTrunc(ret, rt);
				}
				else if (ret->getType() != rt)
				{
					ret = irb.CreateBitCast(ret, rt);
				}
			}
		}
	}

	ret = generateTypeConversion(irb, ret, dstType, ct);
	return ret;
}

llvm::StoreInst* Capstone2LlvmIrTranslatorX86_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (isWideVectorRegister(r))
	{
		return storeWideVectorRegister(r, val, irb);
	}

	auto* rt = getRegisterType(r);
	auto pr = getParentRegister(r);
	auto* reg = getRegister(pr);
	if (reg == nullptr)
	{
		throw GenericError("Capstone2LlvmIrTranslatorX86_impl() unhandled reg.");
	}

	llvm::StoreInst* ret = nullptr;

	// We probably want to do this for all conversion variants.
	//
	if (rt->isIntegerTy() && val->getType()->isIntegerTy())
	{
		auto* valT = llvm::cast<llvm::IntegerType>(val->getType());
		if (valT->getBitWidth() > getRegisterBitSize(r))
		{
			val = irb.CreateTrunc(val, rt);
		}
	}

	val = generateTypeConversion(irb, val, reg->getValueType(), ct);

	// generateTypeConversion widens to the PARENT's width, which is one step
	// too far for a sign-extending store. x86 says the extension fills the
	// DESTINATION operand and stops: `movsx ecx, ax` with AX = 0xff00 leaves
	// RCX = 0x00000000_ffffff00 on this machine, not 0xffffffff_ffffff00,
	// because a 32-bit write zeroes 63:32 whatever produced the value.
	// Worse at 8 and 16 bits, where the merge below is `l | val` with no mask
	// on val: `movsx ax, bl` with BL = 0xff measured 0x11223344_5566_ffff,
	// and the all-ones i64 sext would have set every bit of RAX.
	//
	// Masking the converted value down to the destination's own width says
	// exactly that. For every ZEXT caller -- which is every other caller --
	// the bits it clears are already zero, so this changes nothing but MOVSX.
	if (auto* valTy = llvm::dyn_cast<llvm::IntegerType>(val->getType()))
	{
		unsigned childBits = getRegisterBitSize(r);
		if (childBits && childBits < valTy->getBitWidth())
		{
			val = irb.CreateAnd(
				val, llvm::ConstantInt::get(valTy, llvm::APInt::getLowBitsSet(valTy->getBitWidth(), childBits)));
		}
	}

	if (r == pr
			// Zext for 64-bit target regs & 32-bit source regs.
			|| (getRegisterBitSize(pr) == 64 && getRegisterBitSize(r) == 32))
	{
		ret = irb.CreateStore(val, reg);
	}
	else
	{
		llvm::Value* l = createLoad(irb, reg);
		auto* parentTy = llvm::dyn_cast<llvm::IntegerType>(l->getType());
		if (parentTy == nullptr)
		{
			throw GenericError("Unexpected parent type.");
		}

		// The mask a sub-register write needs is `~((2^childBits - 1) <<
		// offset)` at the parent's width, and offset is 8 for exactly the
		// four high-byte registers and 0 for everything else.
		//
		// This was a hard-coded three-by-four table of those values -- i8,
		// i16 and i32 children against i16, i32 and i64 parents, plus the
		// AH/CH/DH/BH row -- and any pair outside it threw "Mask not
		// initialized in storeRegister()". That covered every general-purpose
		// register and nothing else, which is why XMM, YMM and ZMM are three
		// independent globals in this register file rather than one register
		// seen at three widths: mapping XMM's parent to YMM would have made
		// every SSE store throw.
		unsigned parentBits = parentTy->getBitWidth();
		unsigned childBits = getRegisterBitSize(r);
		unsigned offset = 0;
		if (r == X86_REG_AH
				|| r == X86_REG_CH
				|| r == X86_REG_DH
				|| r == X86_REG_BH)
		{
			offset = 8;
			val = irb.CreateShl(val, 8);
		}

		if (childBits == 0 || childBits + offset > parentBits)
		{
			throw GenericError("Sub-register does not fit its parent.");
		}

		llvm::APInt keep = llvm::APInt::getAllOnes(childBits).zext(parentBits).shl(offset);
		auto* andC = llvm::ConstantInt::get(parentTy, ~keep);
		l = irb.CreateAnd(l, andC);

		auto* o = irb.CreateOr(l, val);
		ret = irb.CreateStore(o, reg);
	}

	if (ret)
		attachPointeeType(ret, reg->getValueType());
	return ret;
}

void Capstone2LlvmIrTranslatorX86_impl::storeRegisters(
		llvm::IRBuilder<>& irb,
		const std::vector<std::pair<uint32_t, llvm::Value*>>& regs)
{
	for (auto& p : regs)
	{
		storeRegister(p.first, p.second, irb);
	}
}

void Capstone2LlvmIrTranslatorX86_impl::storeRegistersPlusSflags(
		llvm::IRBuilder<>& irb,
		llvm::Value* sflagsVal,
		const std::vector<std::pair<uint32_t, llvm::Value*>>& regs)
{
	storeRegisters(irb, regs);
	storeRegister(X86_REG_ZF, generateZeroFlag(sflagsVal, irb), irb);
	storeRegister(X86_REG_SF, generateSignFlag(sflagsVal, irb), irb);
	storeRegister(X86_REG_PF, generateParityFlag(sflagsVal, irb), irb);
}

unsigned Capstone2LlvmIrTranslatorX86_impl::getAddrSpace(x86_reg segment)
{
	switch (segment)
	{
		case X86_REG_FS:
		{
			return static_cast<unsigned>(x86_addr_space::FS);
		}
		case X86_REG_GS:
		{
			return static_cast<unsigned>(x86_addr_space::GS);
		}
		case X86_REG_SS:
		{
			return static_cast<unsigned>(x86_addr_space::SS);
		}
		default:
		{
			return static_cast<unsigned>(x86_addr_space::DEFAULT);
		}
	}
}

bool Capstone2LlvmIrTranslatorX86_impl::isX87DataRegister(uint32_t r)
{
	return X86_REG_ST0 <= r && r <= X86_REG_ST7;
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadX87Top(llvm::IRBuilder<>& irb)
{
	return loadRegister(X87_REG_TOP, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadX87TopDec(llvm::IRBuilder<>& irb)
{
	auto* top = loadX87Top(irb);
	return irb.CreateSub(top, llvm::ConstantInt::get(top->getType(), 1));
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadX87TopInc(llvm::IRBuilder<>& irb)
{
	auto* top = loadX87Top(irb);
	return irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), 1));
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadX87TopDecStore(
		llvm::IRBuilder<>& irb)
{
	auto* top = loadX87TopDec(irb);
	storeRegister(X87_REG_TOP, top, irb);
	return top;
}

/**
 * This returns TOP value before the incrementation.
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadX87TopIncStore(
		llvm::IRBuilder<>& irb)
{
	auto* top = loadX87Top(irb);
	auto* inc = irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), 1));
	storeRegister(X87_REG_TOP, inc, irb);
	return top;
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::x87IncTop(
		llvm::IRBuilder<>& irb,
		llvm::Value* top)
{
	top = top == nullptr ? loadX87Top(irb) : top;
	auto* inc = irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), 1));
	storeRegister(X87_REG_TOP, inc, irb);
	return inc;
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::x87DecTop(
		llvm::IRBuilder<>& irb,
		llvm::Value* top)
{
	top = top == nullptr ? loadX87Top(irb) : top;
	auto* dec = irb.CreateSub(top, llvm::ConstantInt::get(top->getType(), 1));
	storeRegister(X87_REG_TOP, dec, irb);
	return dec;
}

llvm::CallInst* Capstone2LlvmIrTranslatorX86_impl::storeX87DataReg(
		llvm::IRBuilder<>& irb,
		llvm::Value* rNum,
		llvm::Value* val)
{
	if (!rNum->getType()->isIntegerTy(3) || !val->getType()->isX86_FP80Ty())
	{
		throw GenericError("Bad operands of storeX87DataReg().");
	}

	std::vector<llvm::Value*> ps = {rNum, val};
	return irb.CreateCall(getX87DataStoreFunction(), ps);
}

llvm::CallInst* Capstone2LlvmIrTranslatorX86_impl::loadX87DataReg(
		llvm::IRBuilder<>& irb,
		llvm::Value* rNum)
{
	if (!rNum->getType()->isIntegerTy(3))
	{
		throw GenericError("Bad operands of loadX87DataReg().");
	}

	std::vector<llvm::Value*> ps = {rNum};
	return irb.CreateCall(getX87DataLoadFunction(), ps);
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadOp(
		cs_x86_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case X86_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case X86_OP_IMM:
		{
			auto* t = getIntegerTypeFromByteSize(_module, op.size);
			return llvm::ConstantInt::get(t, llvm::APInt(t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm), false, /*implicitTrunc=*/true));
		}
		case X86_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
			auto* t = baseR ? baseR->getType() : getDefaultType();
			llvm::Value* disp = op.mem.disp
					? llvm::ConstantInt::getSigned(t, op.mem.disp)
					: nullptr;

			auto* idxR = loadRegister(op.mem.index, irb);
			if (idxR)
			{
				auto* scale = llvm::ConstantInt::get(
						idxR->getType(),
						op.mem.scale);
				idxR = irb.CreateMul(idxR, scale);
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
			// Possible, e.g. lea edi, dword ptr [ecx*4] (8d 3c 8d 00 00 00 00).
			//
			else
			{
				addr = llvm::ConstantInt::get(getDefaultType(), 0);
			}

			if (idxR && addr != idxR)
			{
				idxR = irb.CreateZExtOrTrunc(idxR, addr->getType());
				addr = irb.CreateAdd(addr, idxR);
			}

			if (lea)
			{
				return addr;
			}
			else
			{
				llvm::Type* t = ty && ty->isFloatingPointTy()
						? getFloatTypeFromByteSize(_module, op.size)
						: getIntegerTypeFromByteSize(_module, op.size);
				return loadIntPtr(irb, addr, t, getAddrSpace(op.mem.segment));
			}
		}
		case X86_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::Instruction* Capstone2LlvmIrTranslatorX86_impl::storeOp(
		cs_x86_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case X86_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case X86_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
			auto* t = baseR ? baseR->getType() : getDefaultType();
			llvm::Value* disp = op.mem.disp
					? llvm::ConstantInt::getSigned(t, op.mem.disp)
					: nullptr;

			auto* idxR = loadRegister(op.mem.index, irb);
			if (idxR)
			{
				auto* scale = llvm::ConstantInt::get(idxR->getType(), op.mem.scale);
				idxR = irb.CreateMul(idxR, scale);
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
			// Possible, e.g. lea edi, dword ptr [ecx*4] (8d 3c 8d 00 00 00 00).
			//
			else
			{
				addr = llvm::ConstantInt::get(getDefaultType(), 0);
			}

			if (idxR && addr != idxR)
			{
				idxR = irb.CreateZExtOrTrunc(idxR, addr->getType());
				addr = irb.CreateAdd(addr, idxR);
			}

			auto* tt = val->getType()->isFloatingPointTy()
					? getFloatTypeFromByteSize(_module, op.size)
					: getIntegerTypeFromByteSize(_module, op.size);

			val = generateTypeConversion(irb, val, tt, ct);

			return storeIntPtr(irb, val, addr, tt, getAddrSpace(op.mem.segment));
		}
		case X86_OP_IMM:
		case X86_OP_INVALID:
		default:
		{
			throw GenericError("Should not be possible.");
		}
	}
}

/**
 * @return (op0, top)
 */
std::tuple<llvm::Value*, llvm::Value*>
Capstone2LlvmIrTranslatorX86_impl::loadOpFloatingNullaryOrUnaryTop(
		cs_insn* i,
		cs_x86* xi,
		llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 0 && xi->op_count != 1)
	{
		throw GenericError("This is not an unary instruction.");
	}

	llvm::Value* top = loadX87Top(irb);
	llvm::Value* op0 = nullptr;

	if (xi->op_count == 0)
	{
		op0 = loadX87DataReg(irb, top);
	}
	else if (xi->op_count == 1
			&& xi->operands[0].type == X86_OP_REG
			&& isX87DataRegister(xi->operands[0].reg))
	{
		auto reg1 = xi->operands[0].reg;
		unsigned regOff1 = reg1 - X86_REG_ST0;
		auto* idx = regOff1
				? irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), regOff1))
				: top;

		op0 = loadX87DataReg(irb, idx);
	}
	else if (xi->op_count == 1 && xi->operands[0].type == X86_OP_MEM)
	{
		if (i->id == X86_INS_FILD)
		{
			op0 = loadOpUnary(
					xi,
					irb,
					nullptr,
					llvm::Type::getX86_FP80Ty(_module->getContext()),
					eOpConv::SITOFP_OR_FPCAST);
		}
		// X86_INS_FLD
		else
		{
			op0 = loadOpUnary(
					xi,
					irb,
					llvm::Type::getFloatTy(_module->getContext()),
					llvm::Type::getX86_FP80Ty(_module->getContext()),
					eOpConv::FPCAST_OR_BITCAST);
		}
	}
	else
	{
		throw GenericError("loadOpFloatingUnaryTop(): unhandled.");
	}

	return std::make_tuple(op0, top);
}

/**
 * @return (op0, op1, top, idx)
 */
std::tuple<llvm::Value*, llvm::Value*, llvm::Value*, llvm::Value*>
Capstone2LlvmIrTranslatorX86_impl::loadOpFloatingBinaryTop(
		cs_insn* i,
		cs_x86* xi,
		llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 0 && xi->op_count != 1 && xi->op_count != 2)
	{
		throw GenericError("This is not an binary instruction.");
	}

	llvm::Value* top = loadX87Top(irb);
	llvm::Value* op0 = nullptr;
	llvm::Value* op1 = nullptr;
	llvm::Value* idx = nullptr;
	llvm::Value* idx2= nullptr;

	if (xi->op_count == 0)
	{
		idx = irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), 1));

		op0 = loadX87DataReg(irb, top);
		op1 = loadX87DataReg(irb, idx);
	}
	else if (xi->op_count == 2
			&& isX87DataRegister(xi->operands[0].reg)
			&& isX87DataRegister(xi->operands[1].reg))
	{
		auto reg1 = xi->operands[0].reg;
		unsigned regOff1 = reg1 - X86_REG_ST0;
		idx = regOff1
				? irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), regOff1))
				: top;

		auto reg2 = xi->operands[1].reg;
		unsigned regOff2 = reg2 - X86_REG_ST0;
		idx2 = regOff2
				? irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), regOff2))
				: top;

		op0 = loadX87DataReg(irb, idx);
		op1 = loadX87DataReg(irb, idx2);
	}
	else if (xi->op_count == 1
			&& xi->operands[0].type == X86_OP_REG
			&& isX87DataRegister(xi->operands[0].reg))
	{
		auto reg = xi->operands[0].reg;
		unsigned regOff = reg - X86_REG_ST0;
		idx = regOff
				? irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), regOff))
				: top;

		op0 = loadX87DataReg(irb, top);
		op1 = loadX87DataReg(irb, idx);
	}
	else if (xi->op_count == 1 && xi->operands[0].type == X86_OP_MEM)
	{
		if (i->id == X86_INS_FIADD
				|| i->id == X86_INS_FIMUL
				|| i->id == X86_INS_FIDIV
				|| i->id == X86_INS_FIDIVR
				|| i->id == X86_INS_FISUB
				|| i->id == X86_INS_FISUBR)
		{
			op0 = loadX87DataReg(irb, top);
			op1 = loadOpUnary(
					xi,
					irb,
					nullptr,
					llvm::Type::getX86_FP80Ty(_module->getContext()),
					eOpConv::SITOFP_OR_FPCAST);
		}
		else if ( i->id == X86_INS_FICOM
				|| i->id == X86_INS_FICOMP)
		{
			op0 = loadX87DataReg(irb, top);
			// SIGNED. FICOM's memory operand is a signed word or dword
			// integer -- the same encoding FILD reads, and FILD, FIADD,
			// FIMUL, FIDIV and FISUB all use SITOFP. This alone said UITOFP,
			// so `ficoms` on a word holding 0xffff compared against 65535.0
			// instead of -1.0. Measured: ST(0) = 0.0 against that word sets
			// C0 = 0, because 0.0 is the greater; this produced C0 = 1 and
			// the fnstsw/sahf/jb that follows branched the wrong way.
			op1 = loadOpUnary(
				xi, irb, nullptr, llvm::Type::getX86_FP80Ty(_module->getContext()), eOpConv::SITOFP_OR_FPCAST);
		}
		else
		{
			op0 = loadX87DataReg(irb, top);
			op1 = loadOpUnary(
					xi,
					irb,
					llvm::Type::getFloatTy(_module->getContext()),
					llvm::Type::getX86_FP80Ty(_module->getContext()),
					eOpConv::FPCAST_OR_BITCAST);
		}

		idx = top;
	}
	else
	{
		throw GenericError("loadOpFloatingBinaryTop(): unhandled.");
	}

	if (i->id == X86_INS_FSUBP
			|| i->id == X86_INS_FADD
			|| i->id == X86_INS_FDIVP
			|| i->id == X86_INS_FDIVRP
			|| i->id == X86_INS_FMULP
			|| i->id == X86_INS_FSUBRP)
	{
		auto* tmp = op0;
		op0 = op1;
		op1 = tmp;
	}

	if (i->id == X86_INS_FXCH
			&& top == idx)
	{
		idx = idx2;
	}

	return std::make_tuple(op0, op1, top, idx);
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateZeroFlag(
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	auto* zero = llvm::ConstantInt::get(val->getType(), 0);
	return irb.CreateICmpEQ(val, zero);
}

llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateSignFlag(
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	auto* zero = llvm::ConstantInt::get(val->getType(), 0);
	return irb.CreateICmpSLT(val, zero);
}

/**
 * The parity flag reflects the parity only of the least significant byte of
 * the result, and is set if the number of set bits of ones is even.
 *
 * (val & 1) (== 1) -> odd
 * (val & 1) == 0   -> even
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateParityFlag(
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	auto* i8t = irb.getInt8Ty();
	auto* trunc = irb.CreateTrunc(val, i8t);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctpop, i8t);
	auto* c = irb.CreateCall(f, {trunc});
	auto* a = irb.CreateAnd(c, llvm::ConstantInt::get(c->getType(), 1));
	return irb.CreateICmpEQ(a, llvm::ConstantInt::get(a->getType(), 0));
}

/**
 * SET_SFLAGS()
 */
void Capstone2LlvmIrTranslatorX86_impl::generateSetSflags(
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	storeRegister(X86_REG_ZF, generateZeroFlag(val, irb), irb);
	storeRegister(X86_REG_SF, generateSignFlag(val, irb), irb);
	storeRegister(X86_REG_PF, generateParityFlag(val, irb), irb);
}

/**
 * CF == 0
 * AE - above or equal
 * NB - not below
 * NC - not carry
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcAE(llvm::IRBuilder<>& irb)
{
	auto* cf = loadRegister(X86_REG_CF, irb);
	return irb.CreateICmpEQ(cf, irb.getInt1(false));
}

/**
 * CF == 0 && ZF == 0
 * A - above
 * NBE - not below or equal
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcA(llvm::IRBuilder<>& irb)
{
	auto* cf = loadRegister(X86_REG_CF, irb);
	auto* zf = loadRegister(X86_REG_ZF, irb);
	auto* orr = irb.CreateOr(cf, zf);
	return irb.CreateXor(orr, irb.getInt1(true));
}

/**
 * CF == 1 or ZF == 1
 * BE - below or equal
 * NA - not above
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcBE(llvm::IRBuilder<>& irb)
{
	auto* cf = loadRegister(X86_REG_CF, irb);
	auto* zf = loadRegister(X86_REG_ZF, irb);
	return irb.CreateOr(cf, zf);
}

/**
 * CF == 1
 * B - below
 * C - carry
 * NAE - not above or equal
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcB(llvm::IRBuilder<>& irb)
{
	return loadRegister(X86_REG_CF, irb);
}

/**
 * ZF == 1
 * E - equal
 * Z - zero
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcE(llvm::IRBuilder<>& irb)
{
	return loadRegister(X86_REG_ZF, irb);
}

/**
 * SF == OF
 * GE - greater or equal
 * NL - not less
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcGE(llvm::IRBuilder<>& irb)
{
	auto* sf = loadRegister(X86_REG_SF, irb);
	auto* of = loadRegister(X86_REG_OF, irb);
	return irb.CreateICmpEQ(sf, of);
}

/**
 * ZF == 0 and SF == OF
 * G - greater
 * NLE - not less or equal
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcG(llvm::IRBuilder<>& irb)
{
	auto* zf = loadRegister(X86_REG_ZF, irb);
	auto* sf = loadRegister(X86_REG_SF, irb);
	auto* of = loadRegister(X86_REG_OF, irb);
	auto* sfOfEq = irb.CreateICmpEQ(sf, of);
	auto* zfZero = irb.CreateICmpEQ(zf, irb.getInt1(false));
	return irb.CreateAnd(sfOfEq, zfZero);
}

/**
 * ZF == 1 or SF != OF
 * LE - less or equal
 * NG - not greater
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcLE(llvm::IRBuilder<>& irb)
{
	auto* zf = loadRegister(X86_REG_ZF, irb);
	auto* sf = loadRegister(X86_REG_SF, irb);
	auto* of = loadRegister(X86_REG_OF, irb);
	auto* sfOfNe = irb.CreateICmpNE(sf, of);
	return irb.CreateOr(zf, sfOfNe);
}

/**
 * SF != OF
 * L - less
 * NGE - not greater or equal
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcL(llvm::IRBuilder<>& irb)
{
	auto* sf = loadRegister(X86_REG_SF, irb);
	auto* of = loadRegister(X86_REG_OF, irb);
	return irb.CreateICmpNE(sf, of);
}

/**
 * ZF == 0
 * NE - not equal
 * NZ - not zero
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcNE(llvm::IRBuilder<>& irb)
{
	auto* zf = loadRegister(X86_REG_ZF, irb);
	return irb.CreateICmpEQ(zf, irb.getInt1(false));
}

/**
 * OF == 0
 * NO - not overflow
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcNO(llvm::IRBuilder<>& irb)
{
	auto* of = loadRegister(X86_REG_OF, irb);
	return irb.CreateICmpEQ(of, irb.getInt1(false));
}

/**
 * PF == 0
 * NP - not parity
 * PO - parity odd
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcNP(llvm::IRBuilder<>& irb)
{
	auto* pf = loadRegister(X86_REG_PF, irb);
	return irb.CreateICmpEQ(pf, irb.getInt1(false));
}

/**
 * SF == 0
 * NS - not sign
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcNS(llvm::IRBuilder<>& irb)
{
	auto* sf = loadRegister(X86_REG_SF, irb);
	return irb.CreateICmpEQ(sf, irb.getInt1(false));
}

/**
 * OF == 1
 * O - overflow
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcO(llvm::IRBuilder<>& irb)
{
	return loadRegister(X86_REG_OF, irb);
}

/**
 * PF == 1
 * P - parity
 * PE - parity even
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcP(llvm::IRBuilder<>& irb)
{
	return loadRegister(X86_REG_PF, irb);
}

/**
 * SF == 1
 * S - sign
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateCcS(llvm::IRBuilder<>& irb)
{
	return loadRegister(X86_REG_SF, irb);
}

bool Capstone2LlvmIrTranslatorX86_impl::isOperandRegister(cs_x86_op& op)
{
	return op.type == X86_OP_REG;
}

uint8_t Capstone2LlvmIrTranslatorX86_impl::getOperandAccess(cs_x86_op& op)
{
	return op.access;
}

//
//==============================================================================
// x86 instruction translation methods.
//==============================================================================
//

/**
 * X86_INS_AAA, X86_INS_AAS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAaa(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* al = loadRegister(X86_REG_AL, irb);
	auto* ah = loadRegister(X86_REG_AH, irb);
	auto* af = loadRegister(X86_REG_AF, irb);

	auto* ala = irb.CreateAnd(al, irb.getInt8(0xf));
	auto* alUgt = irb.CreateICmpUGT(ala, irb.getInt8(9));
	auto* cond = irb.CreateOr(alUgt, af);

	auto* aladd = i->id == X86_INS_AAA
			? irb.CreateAdd(al, irb.getInt8(6))  // X86_INS_AAA
			: irb.CreateSub(al, irb.getInt8(6)); // X86_INS_AAS
	auto* ahadd = i->id == X86_INS_AAA
			? irb.CreateAdd(ah, irb.getInt8(1))  // X86_INS_AAA
			: irb.CreateSub(ah, irb.getInt8(1)); // X86_INS_AAS

	auto* alv = irb.CreateSelect(cond, aladd, al);
	auto* ahv = irb.CreateSelect(cond, ahadd, ah);
	auto* afv = irb.CreateSelect(cond, irb.getInt1(true), irb.getInt1(false));
	auto* cfv = irb.CreateSelect(cond, irb.getInt1(true), irb.getInt1(false));

	alv = irb.CreateAnd(alv, irb.getInt8(0xf));

	storeRegisters(irb, {
			{X86_REG_AL, alv},
			{X86_REG_AH, ahv},
			{X86_REG_AF, afv},
			{X86_REG_CF, cfv}});
}

/**
 * X86_INS_DAA, X86_INS_DAS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateDaaDas(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* al = loadRegister(X86_REG_AL, irb);
	auto* af = loadRegister(X86_REG_AF, irb);
	auto* cf = loadRegister(X86_REG_CF, irb);

	auto* alAnd = irb.CreateAnd(al, llvm::ConstantInt::get(al->getType(), 0xf));
	auto* alIcmp = irb.CreateICmpUGT(alAnd, llvm::ConstantInt::get(alAnd->getType(), 9));
	auto* cnd = irb.CreateOr(alIcmp, af);

	auto irbP = generateIfThenElse(cnd, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	{
		auto* alAdd = i->id == X86_INS_DAA
				? bodyIf.CreateAdd(al, llvm::ConstantInt::get(al->getType(), 6))    // X86_INS_DAA
				: bodyIf.CreateAdd(al, llvm::ConstantInt::get(al->getType(), 250)); // X86_INS_DAS, => +250 == -6
		auto* alUgt = bodyIf.CreateICmpUGT(al, llvm::ConstantInt::get(al->getType(), 153));
		auto* cfOr = bodyIf.CreateOr(alUgt, cf);
		auto* alAdd96 = i->id == X86_INS_DAA
				? bodyIf.CreateAdd(alAdd, llvm::ConstantInt::get(al->getType(), 96))  // X86_INS_DAA
				: bodyIf.CreateSub(alAdd, llvm::ConstantInt::get(al->getType(), 96)); // X86_INS_DAS
		auto* alSel = bodyIf.CreateSelect(cfOr, alAdd96, alAdd);
		storeRegistersPlusSflags(bodyIf, alSel, {
				{X86_REG_CF, cfOr},
				{X86_REG_AF, bodyIf.getInt1(true)},
				{X86_REG_AL, alSel}});
	}

	{
		auto* alUgt = bodyElse.CreateICmpUGT(al, llvm::ConstantInt::get(al->getType(), 153));
		auto* cfOr = bodyElse.CreateOr(alUgt, cf);
		auto* alAdd96 = i->id == X86_INS_DAA
				? bodyElse.CreateAdd(al, llvm::ConstantInt::get(al->getType(), 96))  // X86_INS_DAA
				: bodyElse.CreateSub(al, llvm::ConstantInt::get(al->getType(), 96)); // X86_INS_DAS
		auto* alSel = bodyElse.CreateSelect(cfOr, alAdd96, al);

		storeRegistersPlusSflags(bodyElse, alSel, {
				{X86_REG_CF, cfOr},
				{X86_REG_AF, bodyElse.getInt1(false)},
				{X86_REG_AL, alSel}});
	}
}

/**
 * X86_INS_AAD
 * According to Ollydbg, CF, OF, and possibly AF are also set (undef in specs).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAad(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, xi, irb);

	auto* al = loadRegister(X86_REG_AL, irb);
	auto* ah = loadRegister(X86_REG_AH, irb);
	if (xi->op_count == 0)
	{
		op0 = llvm::ConstantInt::get(ah->getType(), 10);
	}
	else
	{
		op0 = loadOpUnary(xi, irb, nullptr, ah->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}

	auto* mul = irb.CreateMul(ah, op0);
	auto* add = irb.CreateAdd(al, mul);
	// There is & 0xFF in specification, but I think LLVM's arithmetic on i8
	// will take care of this.

	storeRegistersPlusSflags(irb, add, {
			{X86_REG_AL, add},
			{X86_REG_AH, llvm::ConstantInt::get(ah->getType(), 0)}});
}

/**
 * X86_INS_AAM
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAam(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, xi, irb);

	auto* al = loadRegister(X86_REG_AL, irb);
	if (xi->op_count == 0)
	{
		op0 = llvm::ConstantInt::get(al->getType(), 10);
	}
	else
	{
		op0 = loadOpUnary(xi, irb, nullptr, al->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}

	// `aam 0`, the encoding D4 00, divides by the literal zero -- which
	// constant-folds to immediate undefined behaviour, not to poison. No
	// compiler emits it, but a decompiler does not get to choose its input:
	// D4 00 is a classic anti-disassembly pair and is what junk bytes decode
	// to when data is disassembled as code. Falling over on exactly that
	// input is the wrong failure mode.
	//
	// As with DIV and IDIV, AAM with a zero immediate RAISES #DE, so no
	// continuing execution observes a value here; the divisor is made defined
	// and no answer is invented. For every other immediate the select folds
	// away and this costs nothing.
	op0 = irb.CreateBinaryIntrinsic(llvm::Intrinsic::umax, op0, llvm::ConstantInt::get(op0->getType(), 1));

	auto* div = irb.CreateUDiv(al, op0);
	auto* rem = irb.CreateURem(al, op0);

	storeRegistersPlusSflags(irb, rem, {
			{X86_REG_AL, rem},
			{X86_REG_AH, div}});
}

/**
 * X86_INS_ADC, X86_INS_ADCX, X86_INS_ADOX
 * http://stackoverflow.com/questions/29747508/what-is-the-difference-between-the-adc-and-adcx-instructions-on-ia32-ia64
 * X86_INS_ADC == X86_INS_ADCX : carry-in/out == CF
 * X86_INS_ADOX : carry-in/out == OF
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAdc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	uint32_t cfReg = X86_REG_CF; // X86_INS_ADC || X86_INS_ADCX
	if (i->id == X86_INS_ADOX)
	{
		cfReg = X86_REG_OF;
	}
	auto* cf = loadRegister(cfReg, irb, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);

	auto* add1 = irb.CreateAdd(op0, op1);
	auto* add = irb.CreateAdd(add1, cf);

	if (i->id == X86_INS_ADC)
	{
		storeRegistersPlusSflags(irb, add, {
				{X86_REG_AF, generateCarryAddCInt4(op0, op1, irb, cf)},
				{X86_REG_OF, generateOverflowAddC(add, op0, op1, irb, cf)}});
	}
	storeRegister(cfReg, generateCarryAddC(op0, op1, irb, cf), irb);
	storeOp(xi->operands[0], add, irb);
}

namespace {

bool hasLockPrefix(const cs_x86* xi)
{
	for (int p = 0; p < 4; ++p)
	{
		if (xi->prefix[p] == X86_PREFIX_LOCK)
		{
			return true;
		}
	}
	return false;
}

} // namespace

bool Capstone2LlvmIrTranslatorX86_impl::tryTranslateLockedRmw(
		cs_insn* i,
		cs_x86* xi,
		llvm::IRBuilder<>& irb,
		llvm::AtomicRMWInst::BinOp aop)
{
	if (!hasLockPrefix(xi) || xi->op_count < 2)
	{
		return false;
	}
	if (xi->operands[0].type != X86_OP_MEM || i->id == X86_INS_TEST)
	{
		return false;
	}

	auto* addr = loadOp(xi->operands[0], irb, nullptr, true);
	auto* rhs = loadOp(xi->operands[1], irb);
	auto* elem = getIntegerTypeFromByteSize(_module, xi->operands[0].size);
	rhs = generateTypeConversion(irb, rhs, elem, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ptr = intToPtr(irb, addr, elem, getAddrSpace(xi->operands[0].mem.segment));
	auto* old = irb.CreateAtomicRMW(aop, ptr, rhs, llvm::MaybeAlign(), llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(old, elem);

	llvm::Value* result = nullptr;
	switch (aop)
	{
		case llvm::AtomicRMWInst::Add:
			result = irb.CreateAdd(old, rhs);
			storeRegistersPlusSflags(irb, result, {
					{X86_REG_AF, generateCarryAddInt4(old, rhs, irb)},
					{X86_REG_CF, generateCarryAdd(result, old, irb)},
					{X86_REG_OF, generateOverflowAdd(result, old, rhs, irb)}});
			break;
		case llvm::AtomicRMWInst::And:
			result = irb.CreateAnd(old, rhs);
			storeRegistersPlusSflags(irb, result, {
					{X86_REG_AF, irb.getInt1(false)},
					{X86_REG_CF, irb.getInt1(false)},
					{X86_REG_OF, irb.getInt1(false)}});
			break;
		case llvm::AtomicRMWInst::Or:
			result = irb.CreateOr(old, rhs);
			storeRegistersPlusSflags(irb, result, {
					{X86_REG_AF, irb.getInt1(false)},
					{X86_REG_CF, irb.getInt1(false)},
					{X86_REG_OF, irb.getInt1(false)}});
			break;
		case llvm::AtomicRMWInst::Xor:
			result = irb.CreateXor(old, rhs);
			storeRegistersPlusSflags(irb, result, {
					{X86_REG_AF, irb.getInt1(false)},
					{X86_REG_CF, irb.getInt1(false)},
					{X86_REG_OF, irb.getInt1(false)}});
			break;
		case llvm::AtomicRMWInst::Sub:
			result = irb.CreateSub(old, rhs);
			storeRegistersPlusSflags(irb, result, {
					{X86_REG_AF, generateBorrowSubInt4(old, rhs, irb)},
					{X86_REG_CF, generateBorrowSub(old, rhs, irb)},
					{X86_REG_OF, generateOverflowSub(result, old, rhs, irb)}});
			break;
		default:
			return false;
	}

	if (i->id == X86_INS_XADD)
	{
		storeOp(xi->operands[1], old, irb);
	}
	return true;
}

bool Capstone2LlvmIrTranslatorX86_impl::tryTranslateLockedBit(
		cs_insn* i,
		cs_x86* xi,
		llvm::IRBuilder<>& irb,
		llvm::AtomicRMWInst::BinOp aop)
{
	if (!hasLockPrefix(xi) || xi->op_count < 2)
	{
		return false;
	}
	if (xi->operands[0].type != X86_OP_MEM)
	{
		return false;
	}

	auto* addr = loadOp(xi->operands[0], irb, nullptr, true);
	auto* bit = loadOp(xi->operands[1], irb);
	if (!addr || !bit)
	{
		return false;
	}
	auto* elem = getIntegerTypeFromByteSize(_module, xi->operands[0].size);
	bit = generateTypeConversion(irb, bit, elem, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = elem->getBitWidth();
	bit = irb.CreateAnd(bit, llvm::ConstantInt::get(elem, op0BitW - 1));
	auto* mask = irb.CreateShl(llvm::ConstantInt::get(elem, 1), bit);
	llvm::Value* rhs = mask;
	if (aop == llvm::AtomicRMWInst::And)
	{
		rhs = irb.CreateXor(mask, llvm::ConstantInt::getSigned(elem, -1));
	}
	auto* ptr = intToPtr(irb, addr, elem, getAddrSpace(xi->operands[0].mem.segment));
	auto* old = irb.CreateAtomicRMW(aop, ptr, rhs, llvm::MaybeAlign(), llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(old, elem);
	auto* andd = irb.CreateAnd(old, mask);
	auto* icmp = irb.CreateICmpNE(andd, llvm::ConstantInt::get(elem, 0));
	storeRegister(X86_REG_CF, icmp, irb);
	return true;
}

bool Capstone2LlvmIrTranslatorX86_impl::tryTranslateLockedCmpxchg(
		cs_insn* i,
		cs_x86* xi,
		llvm::IRBuilder<>& irb)
{
	if (!hasLockPrefix(xi) || xi->op_count < 2)
	{
		return false;
	}
	if (xi->operands[0].type != X86_OP_MEM)
	{
		return false;
	}

	auto* addr = loadOp(xi->operands[0], irb, nullptr, true);
	auto* desired = loadOp(xi->operands[1], irb);
	auto* elem = getIntegerTypeFromByteSize(_module, xi->operands[0].size);
	auto* accum = loadRegister(getAccumulatorRegister(xi->operands[0].size), irb);
	desired = generateTypeConversion(irb, desired, elem, eOpConv::SEXT_TRUNC_OR_BITCAST);
	accum = generateTypeConversion(irb, accum, elem, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ptr = intToPtr(irb, addr, elem, getAddrSpace(xi->operands[0].mem.segment));
	auto* cx = irb.CreateAtomicCmpXchg(
			ptr,
			accum,
			desired,
			llvm::MaybeAlign(),
			llvm::AtomicOrdering::SequentiallyConsistent,
			llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(cx, elem);
	auto* old = irb.CreateExtractValue(cx, 0);
	auto* sub = irb.CreateSub(accum, old);
	storeRegistersPlusSflags(irb, sub, {
			{X86_REG_AF, generateBorrowSubInt4(accum, old, irb)},
			{X86_REG_CF, generateBorrowSub(accum, old, irb)},
			{X86_REG_OF, generateOverflowSub(sub, accum, desired, irb)}});
	storeRegister(getAccumulatorRegister(xi->operands[0].size), old, irb);
	return true;
}

bool Capstone2LlvmIrTranslatorX86_impl::tryTranslateLockedCmpxchgWide(
		cs_insn* i,
		cs_x86* xi,
		llvm::IRBuilder<>& irb,
		unsigned bits)
{
	if (!hasLockPrefix(xi) || xi->op_count < 1)
	{
		return false;
	}
	if (xi->operands[0].type != X86_OP_MEM || (bits != 64 && bits != 128))
	{
		return false;
	}

	const uint32_t loExp = bits == 64 ? X86_REG_EAX : X86_REG_RAX;
	const uint32_t hiExp = bits == 64 ? X86_REG_EDX : X86_REG_RDX;
	const uint32_t loDes = bits == 64 ? X86_REG_EBX : X86_REG_RBX;
	const uint32_t hiDes = bits == 64 ? X86_REG_ECX : X86_REG_RCX;
	auto* wide = llvm::Type::getIntNTy(_module->getContext(), bits);
	auto* half = llvm::Type::getIntNTy(_module->getContext(), bits / 2);
	auto* shamt = llvm::ConstantInt::get(wide, bits / 2);

	auto* addr = loadOp(xi->operands[0], irb, nullptr, true);
	auto* elo = generateTypeConversion(irb, loadRegister(loExp, irb), wide, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* ehi = generateTypeConversion(irb, loadRegister(hiExp, irb), wide, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* expected = irb.CreateOr(elo, irb.CreateShl(ehi, shamt));
	auto* dlo = generateTypeConversion(irb, loadRegister(loDes, irb), wide, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* dhi = generateTypeConversion(irb, loadRegister(hiDes, irb), wide, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* desired = irb.CreateOr(dlo, irb.CreateShl(dhi, shamt));
	auto* ptr = intToPtr(irb, addr, wide, getAddrSpace(xi->operands[0].mem.segment));
	auto* cx = irb.CreateAtomicCmpXchg(
			ptr,
			expected,
			desired,
			llvm::MaybeAlign(),
			llvm::AtomicOrdering::SequentiallyConsistent,
			llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(cx, wide);
	auto* old = irb.CreateExtractValue(cx, 0);
	auto* succ = irb.CreateExtractValue(cx, 1);
	storeRegister(X86_REG_ZF, succ, irb);
	storeRegister(loExp, irb.CreateTrunc(old, half), irb);
	storeRegister(hiExp, irb.CreateTrunc(irb.CreateLShr(old, shamt), half), irb);
	return true;
}

bool Capstone2LlvmIrTranslatorX86_impl::tryTranslateLockedIncDec(
		cs_insn* i,
		cs_x86* xi,
		llvm::IRBuilder<>& irb,
		bool isDec)
{
	if (!hasLockPrefix(xi) || xi->op_count < 1)
	{
		return false;
	}
	if (xi->operands[0].type != X86_OP_MEM)
	{
		return false;
	}

	auto* addr = loadOp(xi->operands[0], irb, nullptr, true);
	auto* elem = getIntegerTypeFromByteSize(_module, xi->operands[0].size);
	auto* one = llvm::ConstantInt::get(elem, 1);
	auto* ptr = intToPtr(irb, addr, elem, getAddrSpace(xi->operands[0].mem.segment));
	auto aop = isDec ? llvm::AtomicRMWInst::Sub : llvm::AtomicRMWInst::Add;
	auto* old = irb.CreateAtomicRMW(aop, ptr, one, llvm::MaybeAlign(), llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(old, elem);
	llvm::Value* result = isDec ? irb.CreateSub(old, one) : irb.CreateAdd(old, one);
	if (isDec)
	{
		storeRegistersPlusSflags(irb, result, {
				{X86_REG_AF, generateBorrowSubInt4(old, one, irb)},
				{X86_REG_OF, generateOverflowSub(result, old, one, irb)}});
	}
	else
	{
		storeRegistersPlusSflags(irb, result, {
				{X86_REG_AF, generateCarryAddInt4(old, one, irb)},
				{X86_REG_OF, generateOverflowAdd(result, old, one, irb)}});
	}
	return true;
}

/**
 * X86_INS_ADD, X86_INS_XADD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAdd(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (tryTranslateLockedRmw(i, xi, irb, llvm::AtomicRMWInst::Add))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	auto* add = irb.CreateAdd(op0, op1);

	storeRegistersPlusSflags(irb, add, {
			{X86_REG_AF, generateCarryAddInt4(op0, op1, irb)},
			{X86_REG_CF, generateCarryAdd(add, op0, irb)},
			{X86_REG_OF, generateOverflowAdd(add, op0, op1, irb)}});
	storeOp(xi->operands[0], add, irb);
	if (i->id == X86_INS_XADD)
	{
		storeOp(xi->operands[1], op0, irb);
	}
}

/**
 * X86_INS_TEST, X86_INS_AND
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAnd(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (tryTranslateLockedRmw(i, xi, irb, llvm::AtomicRMWInst::And))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	auto* andOp = irb.CreateAnd(op0, op1);

	storeRegistersPlusSflags(irb, andOp, {
			{X86_REG_AF, irb.getInt1(false)},   // undef
			{X86_REG_CF, irb.getInt1(false)},   // cleared
			{X86_REG_OF, irb.getInt1(false)}}); // cleared
	if (i->id == X86_INS_AND)
	{
		storeOp(xi->operands[0], andOp, irb);
	}
}

/**
 * X86_INS_BSF, X86_INS_BSR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBsf(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::THROW);
	auto fnc = i->id == X86_INS_BSF ? llvm::Intrinsic::cttz : llvm::Intrinsic::ctlz;
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			fnc,
			op1->getType());

	llvm::Value* c = irb.CreateCall(f, {op1, irb.getTrue()});
	if (i->id == X86_INS_BSR)
	{
		auto* op1T = llvm::cast<llvm::IntegerType>(op1->getType());
		auto* w = llvm::ConstantInt::get(c->getType(), op1T->getBitWidth() - 1);
		c = irb.CreateSub(w, c);
	}
	auto* eqz = irb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 0));
	auto* zf = irb.CreateSelect(eqz, irb.getInt1(true), irb.getInt1(false));
	auto* rv = irb.CreateSelect(eqz, op0, c); // true => undef

	storeRegister(X86_REG_ZF, zf, irb);
	storeOp(xi->operands[0], rv, irb);
}

/**
 * X86_INS_TZCNT, X86_INS_LZCNT, X86_INS_POPCNT
 *
 * The three BMI1/ABM counting instructions. They look like BSF/BSR and are not
 * the same instruction: BSF leaves the destination *unmodified* when the source
 * is zero and reports that through ZF, whereas TZCNT and LZCNT define the
 * zero-source case -- the answer is the operand width -- and report it through
 * CF. A compiler emits TZCNT precisely so it does not have to branch around
 * that case, so translating it as BSF would drop the branch-free property and,
 * for a zero source, produce a different number.
 *
 * llvm.cttz/ctlz with is_zero_poison=false have exactly the defined semantics:
 * width for a zero input. That is the reason the second argument is false here
 * and true in translateBsf().
 *
 * Flags. TZCNT and LZCNT: CF = (src == 0), ZF = (dst == 0); the other four are
 * architecturally undefined and are left alone rather than being given a value
 * this translator would then be asserting. POPCNT is the odd one: ZF = (src ==
 * 0) and CF, OF, SF, AF and PF are *cleared*, not undefined, so they are
 * written.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBitCount(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	op1 = loadOpBinaryOp1(xi, irb);
	auto* srcTy = llvm::cast<llvm::IntegerType>(op1->getType());
	auto* zero = llvm::ConstantInt::get(srcTy, 0);

	llvm::Intrinsic::ID id = i->id == X86_INS_TZCNT
							   ? llvm::Intrinsic::cttz
							   : (i->id == X86_INS_LZCNT ? llvm::Intrinsic::ctlz : llvm::Intrinsic::ctpop);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, id, srcTy);

	llvm::Value* cnt = i->id == X86_INS_POPCNT ? irb.CreateCall(f, {op1}) : irb.CreateCall(f, {op1, irb.getFalse()});

	auto* srcIsZero = irb.CreateICmpEQ(op1, zero);
	if (i->id == X86_INS_POPCNT)
	{
		// ZF is set from the source, not the result: POPCNT of a non-zero
		// source is never zero, so reading the result would be the same
		// answer by accident and a different one if that ever changed.
		storeRegisters(
			irb,
			{{X86_REG_ZF, srcIsZero},
			 {X86_REG_CF, irb.getInt1(false)},
			 {X86_REG_OF, irb.getInt1(false)},
			 {X86_REG_SF, irb.getInt1(false)},
			 {X86_REG_AF, irb.getInt1(false)},
			 {X86_REG_PF, irb.getInt1(false)}});
	}
	else
	{
		storeRegisters(irb, {{X86_REG_CF, srcIsZero}, {X86_REG_ZF, irb.CreateICmpEQ(cnt, zero)}});
	}

	storeOp(xi->operands[0], cnt, irb);
}

/**
 * X86_INS_ANDN
 *
 * dst = ~src1 & src2. Note the order: the operand that is complemented is the
 * FIRST source, not the second, which is the opposite of PANDN two files over
 * (`~dst & src`). Getting it backwards gives a different answer for every
 * input pair that is not symmetric, and both spellings look right.
 *
 * Flags: SF and ZF from the result, OF and CF cleared. AF and PF are
 * architecturally undefined and are left alone rather than being given a
 * value this translator would then be asserting -- which is why
 * generateSetSflags() is not used here: it writes PF.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAndn(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	op1 = loadOp(xi->operands[1], irb);
	op2 = loadOp(xi->operands[2], irb);
	auto* res = irb.CreateAnd(irb.CreateNot(op1), op2);

	storeRegisters(
		irb,
		{{X86_REG_CF, irb.getInt1(false)},
		 {X86_REG_OF, irb.getInt1(false)},
		 {X86_REG_ZF, generateZeroFlag(res, irb)},
		 {X86_REG_SF, generateSignFlag(res, irb)}});
	storeOp(xi->operands[0], res, irb);
}

/**
 * X86_INS_BLSI, X86_INS_BLSMSK, X86_INS_BLSR
 *
 * The three lowest-set-bit instructions, which differ by one operator each:
 *
 *     BLSI    dst = -src & src        isolate the lowest set bit
 *     BLSMSK  dst = (src - 1) ^ src   mask up to and including it
 *     BLSR    dst = (src - 1) & src   clear it
 *
 * CF is the odd one and is not the same question for all three. BLSI sets CF
 * when the source is **not** zero; BLSMSK and BLSR set it when the source
 * **is** zero. Copying one to the others inverts a flag that the branch after
 * it reads.
 *
 * For BLSMSK the manual clears ZF outright rather than computing it, because
 * the result cannot be zero: for a source of zero it is all ones, and
 * otherwise it is at least 1. Computing it from the result gives the same
 * answer for every input and does not need that argument to stay true.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBls(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	op1 = loadOpBinaryOp1(xi, irb);
	auto* ty = llvm::cast<llvm::IntegerType>(op1->getType());
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* one = llvm::ConstantInt::get(ty, 1);

	llvm::Value* res = nullptr;
	llvm::Value* cf = nullptr;
	switch (i->id)
	{
	case X86_INS_BLSI:
		res = irb.CreateAnd(irb.CreateNeg(op1), op1);
		cf = irb.CreateICmpNE(op1, zero);
		break;
	case X86_INS_BLSMSK:
		res = irb.CreateXor(irb.CreateSub(op1, one), op1);
		cf = irb.CreateICmpEQ(op1, zero);
		break;
	case X86_INS_BLSR:
		res = irb.CreateAnd(irb.CreateSub(op1, one), op1);
		cf = irb.CreateICmpEQ(op1, zero);
		break;
	default: throw GenericError("translateBls(): unhandled instruction id");
	}

	storeRegisters(
		irb,
		{{X86_REG_CF, cf},
		 {X86_REG_OF, irb.getInt1(false)},
		 {X86_REG_ZF, generateZeroFlag(res, irb)},
		 {X86_REG_SF, generateSignFlag(res, irb)}});
	storeOp(xi->operands[0], res, irb);
}

/**
 * X86_INS_BEXTR — extract `len` bits starting at bit `start`.
 *
 *     start = ctrl[7:0], len = ctrl[15:8]
 *     dst   = (src >> start) & ((1 << len) - 1)
 *
 * Both shift amounts come from a register and can exceed the operand width,
 * where an LLVM shift is poison rather than zero. Each is therefore guarded by
 * a select against the width: past the top the extracted value is zero, and a
 * length at or beyond the width is the full mask. The guarded shift is still
 * emitted -- a select only propagates poison from the arm it chooses -- and
 * the alternative, clamping the amount, would answer for a different
 * instruction.
 *
 * Flags: ZF from the result, CF and OF cleared; SF, AF and PF undefined.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBextr(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	llvm::Value* src = loadOp(xi->operands[1], irb);
	llvm::Value* ctrl = loadOp(xi->operands[2], irb);
	auto* ty = llvm::cast<llvm::IntegerType>(src->getType());
	ctrl = irb.CreateZExtOrTrunc(ctrl, ty);

	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* one = llvm::ConstantInt::get(ty, 1);
	auto* width = llvm::ConstantInt::get(ty, ty->getBitWidth());
	auto* byteMask = llvm::ConstantInt::get(ty, 0xff);

	llvm::Value* start = irb.CreateAnd(ctrl, byteMask);
	llvm::Value* len = irb.CreateAnd(irb.CreateLShr(ctrl, llvm::ConstantInt::get(ty, 8)), byteMask);

	llvm::Value* shifted = irb.CreateSelect(irb.CreateICmpULT(start, width), irb.CreateLShr(src, start), zero);
	llvm::Value* mask = irb.CreateSelect(
		irb.CreateICmpULT(len, width),
		irb.CreateSub(irb.CreateShl(one, len), one),
		llvm::Constant::getAllOnesValue(ty));
	llvm::Value* res = irb.CreateAnd(shifted, mask);

	storeRegisters(
		irb,
		{{X86_REG_CF, irb.getInt1(false)}, {X86_REG_OF, irb.getInt1(false)}, {X86_REG_ZF, generateZeroFlag(res, irb)}});
	storeOp(xi->operands[0], res, irb);
}

/**
 * X86_INS_BZHI — zero the bits of the source from bit `index` upwards.
 *
 * CF here is not "the source was zero" as it is for BLSR: it reports that the
 * index was at or past the operand width, in which case nothing is zeroed and
 * the source passes through unchanged.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBzhi(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	llvm::Value* src = loadOp(xi->operands[1], irb);
	llvm::Value* ctrl = loadOp(xi->operands[2], irb);
	auto* ty = llvm::cast<llvm::IntegerType>(src->getType());
	ctrl = irb.CreateZExtOrTrunc(ctrl, ty);

	auto* one = llvm::ConstantInt::get(ty, 1);
	auto* width = llvm::ConstantInt::get(ty, ty->getBitWidth());
	llvm::Value* n = irb.CreateAnd(ctrl, llvm::ConstantInt::get(ty, 0xff));
	llvm::Value* inRange = irb.CreateICmpULT(n, width);

	llvm::Value* masked = irb.CreateAnd(src, irb.CreateSub(irb.CreateShl(one, n), one));
	llvm::Value* res = irb.CreateSelect(inRange, masked, src);

	storeRegisters(
		irb,
		{{X86_REG_CF, irb.CreateNot(inRange)},
		 {X86_REG_OF, irb.getInt1(false)},
		 {X86_REG_ZF, generateZeroFlag(res, irb)},
		 {X86_REG_SF, generateSignFlag(res, irb)}});
	storeOp(xi->operands[0], res, irb);
}

/**
 * X86_INS_SHLX, X86_INS_SHRX, X86_INS_SARX, X86_INS_RORX
 *
 * The BMI2 shifts. What separates them from SHL, SHR and SAR is not the shift
 * -- it is that they **do not touch the flags at all**, which is the entire
 * reason a compiler emits them: the shift can then be scheduled across a
 * comparison. Writing SF/ZF/CF here would be a wrong answer of the kind that
 * only shows up several instructions later, at the Jcc that reads a flag this
 * instruction was chosen for not disturbing.
 *
 * The count is masked to the operand width by the hardware, so the masked
 * amount is always in range and no poison guard is needed. RORX's count is an
 * immediate, so its one out-of-range case -- a rotate by zero, which would
 * otherwise be a shift by the full width -- is decided here rather than in
 * the IR.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateShiftX(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	llvm::Value* src = loadOp(xi->operands[1], irb);
	auto* ty = llvm::cast<llvm::IntegerType>(src->getType());
	unsigned bits = ty->getBitWidth();

	llvm::Value* res = nullptr;
	if (i->id == X86_INS_RORX)
	{
		unsigned n = static_cast<unsigned>(xi->operands[2].imm) & (bits - 1);
		res = n == 0 ? src
					 : irb.CreateOr(
						   irb.CreateLShr(src, llvm::ConstantInt::get(ty, n)),
						   irb.CreateShl(src, llvm::ConstantInt::get(ty, bits - n)));
	}
	else
	{
		llvm::Value* cnt = irb.CreateZExtOrTrunc(loadOp(xi->operands[2], irb), ty);
		cnt = irb.CreateAnd(cnt, llvm::ConstantInt::get(ty, bits - 1));
		switch (i->id)
		{
		case X86_INS_SHLX: res = irb.CreateShl(src, cnt); break;
		case X86_INS_SHRX: res = irb.CreateLShr(src, cnt); break;
		case X86_INS_SARX: res = irb.CreateAShr(src, cnt); break;
		default: throw GenericError("translateShiftX(): unhandled instruction id");
		}
	}

	storeOp(xi->operands[0], res, irb);
}

/**
 * X86_INS_PDEP, X86_INS_PEXT — BMI2 parallel deposit / extract.
 *
 * PEXT packs the bits of SRC that sit under a 1 in MASK into the low end of
 * the destination, in order. PDEP is the inverse: it scatters the low bits of
 * SRC into the positions where MASK is 1 and writes 0 everywhere else. The
 * Intel loop is the definition, not an implementation choice -- a popcount of
 * the mask is not enough, because the bits have to stay in relative order.
 *
 * There is no portable `llvm.pdep` / `llvm.pext` to call on the LLVM 23 pin
 * without a declaration this translator cannot prove exists, so the SDM
 * walk is emitted as an unrolled shift/mask loop. Flags are not written:
 * later manuals list them as unaffected, and inventing ZF here would be a
 * different instruction.
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePdepPext(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	llvm::Value* src = loadOp(xi->operands[1], irb);
	llvm::Value* mask = loadOp(xi->operands[2], irb);
	auto* ty = llvm::cast<llvm::IntegerType>(src->getType());
	mask = irb.CreateZExtOrTrunc(mask, ty);

	unsigned bits = ty->getBitWidth();
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* one = llvm::ConstantInt::get(ty, 1);

	llvm::Value* res = zero;
	llvm::Value* k = zero;
	for (unsigned m = 0; m < bits; ++m)
	{
		llvm::Value* take = irb.CreateICmpNE(
			irb.CreateAnd(irb.CreateLShr(mask, llvm::ConstantInt::get(ty, m)), one), zero);
		if (i->id == X86_INS_PDEP)
		{
			llvm::Value* bit = irb.CreateAnd(irb.CreateLShr(src, k), one);
			llvm::Value* placed = irb.CreateShl(bit, llvm::ConstantInt::get(ty, m));
			res = irb.CreateSelect(take, irb.CreateOr(res, placed), res);
			k = irb.CreateSelect(take, irb.CreateAdd(k, one), k);
		}
		else
		{
			llvm::Value* bit = irb.CreateAnd(irb.CreateLShr(src, llvm::ConstantInt::get(ty, m)), one);
			llvm::Value* placed = irb.CreateShl(bit, k);
			res = irb.CreateSelect(take, irb.CreateOr(res, placed), res);
			k = irb.CreateSelect(take, irb.CreateAdd(k, one), k);
		}
	}

	storeOp(xi->operands[0], res, irb);
}

/**
 * X86_INS_MOVBE — move with the bytes reversed.
 *
 * One of the two operands is always memory, and which one decides the
 * direction; neither direction changes what happens to the value, so both are
 * the same byte swap. No flags.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMovbe(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	op1 = loadOpBinaryOp1(xi, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::bswap, op1->getType());
	storeOp(xi->operands[0], irb.CreateCall(f, {op1}), irb);
}

/**
 * X86_INS_BSWAP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBswap(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	op0 = loadOpUnary(xi, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::bswap,
			op0->getType());

	auto* c = irb.CreateCall(f, {op0});

	storeOp(xi->operands[0], c, irb);
}

/**
 * X86_INS_BT
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBt(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	op1 = irb.CreateAnd(op1, llvm::ConstantInt::get(op1->getType(), op0BitW - 1));
	auto* shl = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op1);
	auto* andd = irb.CreateAnd(shl, op0);
	auto* icmp = irb.CreateICmpNE(andd, llvm::ConstantInt::get(andd->getType(), 0));
	storeRegister(X86_REG_CF, icmp, irb);
}

/**
 * X86_INS_BTC
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBtc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (tryTranslateLockedBit(i, xi, irb, llvm::AtomicRMWInst::Xor))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	op1 = irb.CreateAnd(op1, llvm::ConstantInt::get(op1->getType(), op0BitW - 1));

	auto* srl = irb.CreateLShr(op0, op1);
	auto* and1 = irb.CreateAnd(srl, llvm::ConstantInt::get(srl->getType(), 1));
	auto* icmp = irb.CreateICmpNE(and1, llvm::ConstantInt::get(and1->getType(), 0));
	storeRegister(X86_REG_CF, icmp, irb);

	auto* shl = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op1);
	auto* xor1 = irb.CreateXor(shl, llvm::ConstantInt::get(shl->getType(), -1, true));
	auto* and2 = irb.CreateAnd(op0, xor1);
	auto* xor2 = irb.CreateXor(and1, llvm::ConstantInt::get(and1->getType(), 1));
	auto* shl2 = irb.CreateShl(xor2, op1);
	auto* or1 = irb.CreateOr(shl2, and2);
	storeOp(xi->operands[0], or1, irb);
}

/**
 * X86_INS_BTR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBtr(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (tryTranslateLockedBit(i, xi, irb, llvm::AtomicRMWInst::And))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	op1 = irb.CreateAnd(op1, llvm::ConstantInt::get(op1->getType(), op0BitW - 1));
	auto* shl = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op1);
	auto* andd = irb.CreateAnd(shl, op0);
	auto* icmp = irb.CreateICmpNE(andd, llvm::ConstantInt::get(andd->getType(), 0));
	storeRegister(X86_REG_CF, icmp, irb);

	auto* xor1 = irb.CreateXor(shl, llvm::ConstantInt::get(shl->getType(), -1, true));
	auto* and2 = irb.CreateAnd(op0, xor1);
	storeOp(xi->operands[0], and2, irb);
}

/**
 * X86_INS_BTS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateBts(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (tryTranslateLockedBit(i, xi, irb, llvm::AtomicRMWInst::Or))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	op1 = irb.CreateAnd(op1, llvm::ConstantInt::get(op1->getType(), op0BitW - 1));
	auto* shl = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op1);
	auto* andd = irb.CreateAnd(shl, op0);
	auto* icmp = irb.CreateICmpNE(andd, llvm::ConstantInt::get(andd->getType(), 0));
	storeRegister(X86_REG_CF, icmp, irb);

	auto* or1 = irb.CreateOr(shl, op0);
	storeOp(xi->operands[0], or1, irb);
}

/**
 * X86_INS_CBW
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCbw(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	op0 = loadRegister(X86_REG_AL, irb);
	auto* e = irb.CreateSExt(op0, getRegisterType(X86_REG_AX));
	storeRegister(X86_REG_AX, e, irb);
}

/**
 * X86_INS_CWDE
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCwde(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	op0 = loadRegister(X86_REG_AX, irb);
	auto* e = irb.CreateSExt(op0, getRegisterType(X86_REG_EAX));
	storeRegister(X86_REG_EAX, e, irb);
}

/**
 * X86_INS_CDQE
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCdqe(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	op0 = loadRegister(X86_REG_EAX, irb);
	auto* e = irb.CreateSExt(op0, getRegisterType(X86_REG_RAX));
	storeRegister(X86_REG_RAX, e, irb);
}

/**
 * X86_INS_CWD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCwd(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	op0 = loadRegister(X86_REG_AX, irb);
	auto* e = irb.CreateAShr(op0, getRegisterBitSize(X86_REG_AX) - 1);
	storeRegister(X86_REG_DX, e, irb);
}

/**
 * X86_INS_CDQ
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCdq(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	op0 = loadRegister(X86_REG_EAX, irb);
	auto* e = irb.CreateAShr(op0, getRegisterBitSize(X86_REG_EAX) - 1);
	storeRegister(X86_REG_EDX, e, irb);
}

/**
 * X86_INS_CQO
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCqo(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	op0 = loadRegister(X86_REG_RAX, irb);
	auto* e = irb.CreateAShr(op0, getRegisterBitSize(X86_REG_RAX) - 1);
	storeRegister(X86_REG_RDX, e, irb);
}

/**
 * X86_INS_CLC
 */
void Capstone2LlvmIrTranslatorX86_impl::translateClc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	storeRegister(X86_REG_CF, irb.getInt1(false), irb);
}

/**
 * X86_INS_CLD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCld(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	storeRegister(X86_REG_DF, irb.getInt1(false), irb);
}

/**
 * X86_INS_CLI
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCli(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	storeRegister(X86_REG_IF, irb.getInt1(false), irb);
}

/**
 * X86_INS_CMC
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCmc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* cf = loadRegister(X86_REG_CF, irb);
	auto* xorOp = irb.CreateXor(cf, llvm::ConstantInt::get(cf->getType(), 1));
	storeRegister(X86_REG_CF, xorOp, irb);
}

/**
 * X86_INS_CMPXCHG
 * cmpxchg accum={al, ax, eax}, op0, op1
 * if (accum == op0) then op0 <- op1 else accum <- op0
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCmpxchg(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (tryTranslateLockedCmpxchg(i, xi, irb))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* accum = loadRegister(getAccumulatorRegister(xi->operands[0].size), irb);

	auto* sub = irb.CreateSub(accum, op0);

	storeRegistersPlusSflags(irb, sub, {
			{X86_REG_AF, generateBorrowSubInt4(accum, op0, irb)},
			{X86_REG_CF, generateBorrowSub(accum, op0, irb)},
			{X86_REG_OF, generateOverflowSub(sub, accum, op1, irb)}});
	// If-then-else construction could be used here for more straightforward
	// code, but that would create BBs inside ASM instruction, which should be
	// avoided whenever possible.
	// if (accum == op1) then op0 <- op1, accum <- accum
	//                   else op0 <- op0, accum <- op0
	// (accum == op1) <=> (zf == 1)
	auto* zf = loadRegister(X86_REG_ZF, irb);
	auto* op0Val = irb.CreateSelect(zf, op1, op0);
	auto* accumVal = irb.CreateSelect(zf, accum, op0);
	storeOp(xi->operands[0], op0Val, irb);
	storeRegister(getAccumulatorRegister(xi->operands[0].size), accumVal, irb);
}

/**
 * X86_INS_CMPXCHG8B
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCmpxchg8b(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	if (tryTranslateLockedCmpxchgWide(i, xi, irb, 64))
	{
		return;
	}

	op0 = loadOpUnary(xi, irb);
	auto* eax = loadRegister(X86_REG_EAX, irb, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* edx = loadRegister(X86_REG_EDX, irb, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	edx = irb.CreateShl(edx, 32);
	auto* rval = irb.CreateOr(edx, eax);
	auto* cnd = irb.CreateICmpEQ(op0, rval);

	auto irbP = generateIfThenElse(cnd, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(X86_REG_ZF, bodyIf.getInt1(true), bodyIf);
	auto* ecx = loadRegister(X86_REG_ECX, bodyIf, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* ebx = loadRegister(X86_REG_EBX, bodyIf, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	ecx = bodyIf.CreateShl(ecx, 32);
	auto* res = bodyIf.CreateOr(ecx, ebx);
	storeOp(xi->operands[0], res, bodyIf);

	storeRegister(X86_REG_ZF, bodyElse.getInt1(false), bodyElse);
	auto* low = bodyElse.CreateTrunc(op0, eax->getType());
	auto* high = bodyElse.CreateTrunc(bodyElse.CreateLShr(op0, 32), edx->getType());
	storeRegister(X86_REG_EAX, low, bodyElse);
	storeRegister(X86_REG_EDX, high, bodyElse);
}

/**
 * X86_INS_CMPXCHG16B
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCmpxchg16b(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	if (tryTranslateLockedCmpxchgWide(i, xi, irb, 128))
	{
		return;
	}

	op0 = loadOpUnary(xi, irb);
	auto* rax = loadRegister(X86_REG_RAX, irb, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* rdx = loadRegister(X86_REG_RDX, irb, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	rdx = irb.CreateShl(rdx, 64);
	auto* rval = irb.CreateOr(rdx, rax);
	auto* cnd = irb.CreateICmpEQ(op0, rval);

	auto irbP = generateIfThenElse(cnd, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(X86_REG_ZF, bodyIf.getInt1(true), bodyIf);
	auto* rcx = loadRegister(X86_REG_RCX, bodyIf, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* rbx = loadRegister(X86_REG_RBX, bodyIf, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	rcx = bodyIf.CreateShl(rcx, 64);
	auto* res = bodyIf.CreateOr(rcx, rbx);
	storeOp(xi->operands[0], res, bodyIf);

	storeRegister(X86_REG_ZF, bodyElse.getInt1(false), bodyElse);
	auto* low = bodyElse.CreateTrunc(op0, rax->getType());
	auto* high = bodyElse.CreateTrunc(bodyElse.CreateLShr(op0, 64), rdx->getType());
	storeRegister(X86_REG_RAX, low, bodyElse);
	storeRegister(X86_REG_RDX, high, bodyElse);
}

/**
 * X86_INS_DEC
 */
void Capstone2LlvmIrTranslatorX86_impl::translateDec(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	if (tryTranslateLockedIncDec(i, xi, irb, true))
	{
		return;
	}

	op0 = loadOpUnary(xi, irb);
	op1 = llvm::ConstantInt::get(op0->getType(), 1);

	auto* sub = irb.CreateSub(op0, op1);

	storeRegistersPlusSflags(irb, sub, {
			{X86_REG_AF, generateBorrowSubInt4(op0, op1, irb)},
			// CF not changed.
			{X86_REG_OF, generateOverflowSub(sub, op0, op1, irb)}});
	storeOp(xi->operands[0], sub, irb);
}

/**
 * X86_INS_IMUL
 */
void Capstone2LlvmIrTranslatorX86_impl::translateImul(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (1 <= xi->op_count && xi->op_count <= 3));

	if (xi->op_count == 1)
	{
		translateMul(i, xi, irb);
	}
	else if (xi->op_count == 2)
	{
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

		auto* origType = op0->getType();
		unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
		auto* doubleT = llvm::Type::getIntNTy(_module->getContext(), op0BitW*2);

		op0 = irb.CreateSExt(op0, doubleT);
		op1 = irb.CreateSExt(op1, doubleT);
		auto* mul = irb.CreateMul(op0, op1);
		storeOp(xi->operands[0], mul, irb);

		auto* trunc = irb.CreateTrunc(mul, origType);
		auto* sext = irb.CreateSExt(trunc, doubleT);
		auto* f = irb.CreateICmpNE(mul, sext);
		storeRegister(X86_REG_OF, f, irb);
		storeRegister(X86_REG_CF, f, irb);
	}
	else if (xi->op_count == 3)
	{
		std::tie(op0, op1, op2) = loadOpTernary(xi, irb);

		unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
		auto* doubleT = llvm::Type::getIntNTy(_module->getContext(), op0BitW*2);

		op1 = irb.CreateSExt(op1, doubleT);
		op2 = irb.CreateSExt(op2, doubleT);
		auto* mul = irb.CreateMul(op1, op2);
		storeOp(xi->operands[0], mul, irb);

		auto* trunc = irb.CreateTrunc(mul, op0->getType());
		auto* sext = irb.CreateSExt(trunc, doubleT);
		auto* f = irb.CreateICmpNE(mul, sext);
		storeRegister(X86_REG_OF, f, irb);
		storeRegister(X86_REG_CF, f, irb);
	}
}

/**
 * X86_INS_INC
 */
void Capstone2LlvmIrTranslatorX86_impl::translateInc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	if (tryTranslateLockedIncDec(i, xi, irb, false))
	{
		return;
	}

	op0 = loadOpUnary(xi, irb);
	op1 = llvm::ConstantInt::get(op0->getType(), 1);

	auto* add = irb.CreateAdd(op0, op1);

	storeRegistersPlusSflags(irb, add, {
			{X86_REG_AF, generateCarryAddInt4(op0, op1, irb)},
			// CF not changed.
			{X86_REG_OF, generateOverflowAdd(add, op0, op1, irb)}});
	storeOp(xi->operands[0], add, irb);
}

/**
 * X86_INS_DIV, X86_INS_IDIV
 */
void Capstone2LlvmIrTranslatorX86_impl::translateDiv(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	op1 = loadOpUnary(xi, irb);
	uint32_t op0l = X86_REG_INVALID;
	uint32_t op0h = X86_REG_INVALID;
	uint32_t divR = X86_REG_INVALID;
	uint32_t remR = X86_REG_INVALID;
	llvm::IntegerType* divT = nullptr;
	llvm::IntegerType* resT = nullptr;

	switch (xi->operands[0].size)
	{
		case 1:
		{
			op0l = X86_REG_AX;
			op0h = X86_REG_INVALID;
			divR = X86_REG_AL;
			remR = X86_REG_AH;
			divT = irb.getInt16Ty();
			resT = irb.getInt8Ty();
			break;
		}
		case 2:
		{
			op0l = X86_REG_AX;
			op0h = X86_REG_DX;
			divR = X86_REG_AX;
			remR = X86_REG_DX;
			divT = irb.getInt32Ty();
			resT = irb.getInt16Ty();
			break;
		}
		case 4:
		{
			op0l = X86_REG_EAX;
			op0h = X86_REG_EDX;
			divR = X86_REG_EAX;
			remR = X86_REG_EDX;
			divT = irb.getInt64Ty();
			resT = irb.getInt32Ty();
			break;
		}
		case 8:
		{
			op0l = X86_REG_RAX;
			op0h = X86_REG_RDX;
			divR = X86_REG_RAX;
			remR = X86_REG_RDX;
			divT = irb.getInt128Ty();
			resT = irb.getInt64Ty();
			break;
		}
		default:
		{
			throw GenericError("Unhandled op size in translateDiv().");
		}
	}

	if (op0h == X86_REG_INVALID)
	{
		op0 = loadRegister(op0l, irb); // already i16
	}
	else
	{
		auto* op0ll = irb.CreateZExt(loadRegister(op0l, irb), divT);
		auto* op0hl = irb.CreateZExt(loadRegister(op0h, irb), divT);
		op0hl = irb.CreateShl(op0hl, resT->getBitWidth());
		op0 = irb.CreateOr(op0hl, op0ll);
	}
	// The divisor is widened to the dividend's type, and for IDIV that has to
	// be a SIGN extension. Zero-extending it turned every negative divisor
	// into a huge positive one: `idiv rcx` with rax=10 and rcx=-3 gave a
	// quotient of 0 and a remainder of 10, where the hardware gives -3
	// remainder 1. The dividend needs no such care -- it is the exact
	// two's-complement bit pattern of RDX:RAX either way.
	op1 = i->id == X86_INS_IDIV ? irb.CreateSExt(op1, op0->getType())  // X86_INS_IDIV - signed.
								: irb.CreateZExt(op1, op0->getType()); // X86_INS_DIV  - unsigned.

	// A divisor LLVM cannot prove non-zero makes this IMMEDIATE undefined
	// behaviour -- not poison. Poison is a bad value that spreads; immediate
	// UB lets the optimiser delete the surrounding code, which for a
	// decompiler means deleting the code path the reverser is reading. The
	// signed form has a second such case, INT_MIN / -1, at the DOUBLE width
	// this dividend is assembled at.
	//
	// C makes division by zero undefined, so compilers emit a bare div with
	// no check at all -- the #DE trap IS the check. Every x86 binary with a
	// runtime division reaches this.
	//
	// The divisor is made safe and the result is NOT selected afterwards,
	// which is the difference between this and the ARM64 fix. ARM64 DEFINES
	// the answer for a zero divisor, so there it is produced. x86 defines no
	// answer at all: DIV and IDIV RAISE #DE and the instruction does not
	// complete, so no execution that continues past it can observe any value
	// here. Inventing a specific one would put a claim in the decompiled
	// output that the hardware never makes. Making it merely DEFINED is what
	// this needs; which defined value it is does not matter.
	//
	// Modelling the trap as control flow is a separate and larger question --
	// INT, INT3, BOUND, HLT, UD2, PowerPC tw/twi and MIPS teq are all
	// modelled as nothing or as opaque calls today, and #DE belongs with them
	// in whatever trap model this decompiler grows, designed once.
	{
		auto* dty = op0->getType();
		unsigned dbits = dty->getIntegerBitWidth();
		if (i->id == X86_INS_IDIV)
		{
			// The overflow pair is a relation between the two operands, so it
			// needs a select; only the zero case can be stated as a bound.
			auto* overflow = irb.CreateAnd(
				irb.CreateICmpEQ(op0, llvm::ConstantInt::get(dty, llvm::APInt::getSignedMinValue(dbits))),
				irb.CreateICmpEQ(op1, llvm::ConstantInt::getSigned(dty, -1)));
			op1 = irb.CreateSelect(overflow, llvm::ConstantInt::get(dty, 1), op1);
		}
		// umax rather than a select on "is it zero": the two compute the same number
		// -- umax(x, 1) is x for every non-zero x, since x as an UNSIGNED value is
		// then at least 1, and is 1 when x is zero -- but only umax states the bound
		// LOCALLY. A reader of `select(y == 0, 1, y)`, human or analysis, has to
		// correlate the condition with the arms to see the result is non-zero, and
		// DIV-01 below cannot. This is the same lesson SHIFT-01 taught on the shift
		// side, where the clamp-and-select idiom made the checker fire on the fix.
		op1 = irb.CreateBinaryIntrinsic(llvm::Intrinsic::umax, op1, llvm::ConstantInt::get(dty, 1));
	}

	auto* div = i->id == X86_INS_IDIV
			? irb.CreateSDiv(op0, op1)  // X86_INS_IDIV - signed.
			: irb.CreateUDiv(op0, op1); // X86_INS_DIV  - unsigned.
	div = irb.CreateTrunc(div, resT);
	storeRegister(divR, div, irb);

	auto* rem = i->id == X86_INS_IDIV
			? irb.CreateSRem(op0, op1)  // X86_INS_IDIV - signed.
			: irb.CreateURem(op0, op1); // X86_INS_DIV  - unsigned.
	rem = irb.CreateTrunc(rem, resT);
	storeRegister(remR, rem, irb);
}

/**
 * X86_INS_JMP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateJmp(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	op0 = loadOpUnary(xi, irb);
	generateBranchFunctionCall(irb, op0);
}

/**
 * X86_INS_LJMP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateLjmp(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, xi, irb);

	if (xi->op_count == 1)
	{
		// Same/similar to translateLoadFarPtr().
		op0 = loadOp(xi->operands[0], irb, nullptr, true);

		auto* it1 = getIntegerTypeFromByteSize(_module, xi->operands[0].size);
		auto* l1 = loadIntPtr(irb, op0, it1);

		auto* it2 = irb.getInt16Ty();
		auto* addC = llvm::ConstantInt::get(op0->getType(), xi->operands[0].size);
		auto* addr2 = irb.CreateAdd(op0, addC);
		auto* l2 = loadIntPtr(irb, addr2, it2);

		op0 = l2; // segment selector
		op1 = l1; // address
	}
	else if (xi->op_count == 2)
	{
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	}

	// What to do with segment selector (op0)?
	// Store it to segment register (used now)?
	// Create a different kind of brach function call that also takes segment
	// selector value as parameter?
	storeRegister(X86_REG_CS, op0, irb);
	generateBranchFunctionCall(irb, op1);
}

/**
 * X86_INS_CALL
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCall(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	auto* pc = getCurrentPc(i);
	auto* sp = loadRegister(getStackPointerRegister(), irb);
	auto* ci = llvm::ConstantInt::get(sp->getType(), getArchByteSize());
	auto* sub = irb.CreateSub(sp, ci);
	storeIntPtr(irb, pc, sub, pc->getType());
	storeRegister(getStackPointerRegister(), sub, irb);

	op0 = loadOpUnary(xi, irb);
	generateCallFunctionCall(irb, op0);
}

/**
 * X86_INS_LCALL
 * e.g. lcall ptr [ecx + 0x78563412]
 */
void Capstone2LlvmIrTranslatorX86_impl::translateLcall(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, xi, irb);

	auto* pc = getCurrentPc(i);
	auto* cs = loadRegister(X86_REG_CS, irb);
	auto* sp = loadRegister(getStackPointerRegister(), irb);

	auto* ci1 = llvm::ConstantInt::get(sp->getType(), getArchByteSize());
	auto* sub1 = irb.CreateSub(sp, ci1);
	storeIntPtr(irb, cs, sub1, cs->getType());

	auto* ci2 = llvm::ConstantInt::get(sp->getType(), getArchByteSize()*2);
	auto* sub2 = irb.CreateSub(sp, ci2);
	storeIntPtr(irb, pc, sub2, pc->getType());

	storeRegister(getStackPointerRegister(), sub2, irb);

	if (xi->op_count == 1)
	{
		op0 = loadOpUnary(xi, irb);
	}
	// binary e.g.: lcall 7:0
	else
	{
		std::tie(op1, op0) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	}

	generateCallFunctionCall(irb, op0);
}

/**
 * X86_INS_LAHF
 */
void Capstone2LlvmIrTranslatorX86_impl::translateLahf(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* i8t = irb.getInt8Ty();
	auto* cf = irb.CreateZExt(loadRegister(X86_REG_CF, irb), i8t);
	auto* pf = irb.CreateZExt(loadRegister(X86_REG_PF, irb), i8t);
	auto* af = irb.CreateZExt(loadRegister(X86_REG_AF, irb), i8t);
	auto* zf = irb.CreateZExt(loadRegister(X86_REG_ZF, irb), i8t);
	auto* sf = irb.CreateZExt(loadRegister(X86_REG_SF, irb), i8t);
	auto* zero = irb.getInt8(0);
	auto* one = irb.getInt8(1);

	llvm::Value* val = zero;
	val = irb.CreateOr(val, cf);
	val = irb.CreateOr(val, irb.CreateShl(one, 1));
	val = irb.CreateOr(val, irb.CreateShl(pf, 2));
	val = irb.CreateOr(val, irb.CreateShl(af, 4));
	val = irb.CreateOr(val, irb.CreateShl(zf, 6));
	val = irb.CreateOr(val, irb.CreateShl(sf, 7));
	storeRegister(X86_REG_AH, val, irb);
}

/**
 * X86_INS_LEA
 */
void Capstone2LlvmIrTranslatorX86_impl::translateLea(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	op1 = loadOp(xi->operands[1], irb, nullptr, true);
	// In specification, there are op size/addr size tables of actions based on
	// different bit sizes -- zero extends, truncates.
	// I think storeOp() -> storeRegister() will take of it automatically.
	storeOp(xi->operands[0], op1, irb);
}

/**
 * X86_INS_ENTER
 */
void Capstone2LlvmIrTranslatorX86_impl::translateEnter(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	auto* sp = loadRegister(getStackPointerRegister(), irb);
	auto* bp = loadRegister(getBasePointerRegister(), irb);

//	auto* nestingLevel = irb.CreateURem(op1, llvm::ConstantInt::get(op1->getType(), 32));

	auto* ci = llvm::ConstantInt::get(sp->getType(), xi->addr_size);
	auto* sub = irb.CreateSub(sp, ci);
	storeIntPtr(irb, bp, sub, bp->getType());  // push BP
	storeRegister(getStackPointerRegister(), sub, irb);

	auto* frameTemp = sub; // SP

	// TODO: nestingLevel != 0

	// Continue:
	//
	storeRegister(getBasePointerRegister(), frameTemp, irb);
	op0 = irb.CreateZExtOrTrunc(op0, frameTemp->getType());
	auto* spSub = irb.CreateSub(frameTemp, op0);
	storeRegister(getStackPointerRegister(), spSub, irb);
}

/**
 * X86_INS_LEAVE
 */
void Capstone2LlvmIrTranslatorX86_impl::translateLeave(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* bp = loadRegister(getBasePointerRegister(), irb);
	auto* l = loadIntPtr(irb, bp, bp->getType());
	auto* c = llvm::ConstantInt::get(bp->getType(), getArchByteSize());
	auto* add = irb.CreateAdd(bp, c);

	storeRegister(getBasePointerRegister(), l, irb);
	storeRegister(getStackPointerRegister(), add, irb);
}

/**
 * X86_INS_LDS, X86_INS_LES, X86_INS_LFS, X86_INS_LGS, X86_INS_LSS
 * There is some more shit going on when instruction executed in protected mode.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateLoadFarPtr(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	op1 = loadOp(xi->operands[1], irb, nullptr, true);

	auto* it1 = getIntegerTypeFromByteSize(_module, xi->operands[1].size);
	auto* l1 = loadIntPtr(irb, op1, it1);

	auto* it2 = irb.getInt16Ty();
	auto* addC = llvm::ConstantInt::get(op1->getType(), xi->operands[1].size);
	auto* addr2 = irb.CreateAdd(op1, addC);
	auto* l2 = loadIntPtr(irb, addr2, it2);

	uint32_t segR = X86_REG_INVALID;
	switch (i->id)
	{
		case X86_INS_LDS: segR = X86_REG_DS; break;
		case X86_INS_LES: segR = X86_REG_ES; break;
		case X86_INS_LFS: segR = X86_REG_FS; break;
		case X86_INS_LGS: segR = X86_REG_GS; break;
		case X86_INS_LSS: segR = X86_REG_SS; break;
		default: throw GenericError("Unhandled insn ID in translateLoadFarPtr().");
	}

	storeRegister(segR, l2, irb);
	storeOp(xi->operands[0], l1, irb);
}

/**
 * X86_INS_MOV, X86_INS_MOVSX, X86_INS_MOVSXD, X86_INS_MOVZX, X86_INS_MOVABS,
 * X86_INS_MOVNTI, X86_INS_MOVNTQ
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMov(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	op1 = loadOp(xi->operands[1], irb);
	switch (i->id)
	{
		case X86_INS_MOV:
		case X86_INS_MOVABS:
		// Non-temporal stores. The hint is about cache allocation policy and
		// has no architecturally visible effect on the value stored, so at IR
		// level MOVNTI and MOVNTQ are the move they spell.
		case X86_INS_MOVNTI:
		case X86_INS_MOVNTQ: storeOp(xi->operands[0], op1, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST); break;
		case X86_INS_MOVSX:
		case X86_INS_MOVSXD:
			storeOp(xi->operands[0], op1, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
			break;
		case X86_INS_MOVZX:
			storeOp(xi->operands[0], op1, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			break;
		default:
			throw GenericError("Unhandle instr ID in translateMov().");
	}
}

/**
 * X86_INS_MUL, X86_INS_IMUL (only unary form)
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMul(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	op0 = loadOpUnary(xi, irb);
	op1 = loadRegister(getAccumulatorRegister(xi->operands[0].size), irb);

	llvm::IntegerType* halfT = nullptr;
	llvm::IntegerType* mulT = nullptr;
	uint32_t lowR = X86_REG_INVALID;
	uint32_t highR = X86_REG_INVALID;
	switch (xi->operands[0].size)
	{
		case 1:
		{
			halfT = irb.getInt8Ty();
			mulT = irb.getInt16Ty();
			lowR = X86_REG_AX;
			highR = X86_REG_INVALID;
			break;
		}
		case 2:
		{
			halfT = irb.getInt16Ty();
			mulT = irb.getInt32Ty();
			lowR = X86_REG_AX;
			highR = X86_REG_DX;
			break;
		}
		case 4:
		{
			halfT = irb.getInt32Ty();
			mulT = irb.getInt64Ty();
			lowR = X86_REG_EAX;
			highR = X86_REG_EDX;
			break;
		}
		case 8:
		{
			halfT = irb.getInt64Ty();
			mulT = irb.getInt128Ty();
			lowR = X86_REG_RAX;
			highR = X86_REG_RDX;
			break;
		}
		default:
		{
			throw GenericError("Unhandled op size in translateMul().");
		}
	}

	op0 = i->id == X86_INS_MUL ? irb.CreateZExt(op0, mulT) : irb.CreateSExt(op0, mulT);
	op1 = i->id == X86_INS_MUL ? irb.CreateZExt(op1, mulT) : irb.CreateSExt(op1, mulT);
	auto* mul = irb.CreateMul(op0, op1);
	auto* l = irb.CreateTrunc(mul, halfT);
	auto* h = irb.CreateTrunc(irb.CreateLShr(mul, halfT->getBitWidth()), halfT);

	// CF and OF say the same thing for both forms: the full product does not
	// fit in the low half. For MUL that is "the high half is not zero". For
	// IMUL it is "the high half is not the SIGN EXTENSION of the low half",
	// which is not the same as "not zero and not all ones" -- the test this
	// replaces. `imul cl` with al=16 and cl=8 gives 128: the high half is
	// zero, but 128 does not fit in a signed byte, so the hardware sets both
	// flags and the old test set neither.
	llvm::Value* f = nullptr;
	if (i->id == X86_INS_IMUL)
	{
		auto* signOfLow = irb.CreateAShr(l, llvm::ConstantInt::get(l->getType(), halfT->getBitWidth() - 1));
		f = irb.CreateICmpNE(h, signOfLow);
	}
	else
	{
		f = irb.CreateICmpNE(h, llvm::ConstantInt::get(h->getType(), 0));
	}

	if (highR == X86_REG_INVALID)
	{
		storeRegister(lowR, mul, irb);
	}
	else
	{
		storeRegister(lowR, l, irb);
		storeRegister(highR, h, irb);
	}
	storeRegister(X86_REG_OF, f, irb);
	storeRegister(X86_REG_CF, f, irb);
}

/**
 * X86_INS_NEG
 */
void Capstone2LlvmIrTranslatorX86_impl::translateNeg(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	op0 = loadOpUnary(xi, irb);
	auto* zero = llvm::ConstantInt::get(op0->getType(), 0);

	auto* sub = irb.CreateSub(zero, op0);

	// OF was hardcoded to zero. NEG is `0 - op0`, and that overflows for
	// exactly one input: the minimum signed value, whose negation is itself.
	// `neg rax` with rax = 0x8000000000000000 sets OF on the hardware.
	auto* intMin =
		llvm::ConstantInt::get(op0->getType(), llvm::APInt::getSignedMinValue(op0->getType()->getIntegerBitWidth()));

	storeRegistersPlusSflags(
		irb,
		sub,
		{{X86_REG_AF, generateBorrowSubInt4(zero, op0, irb)},
		 {X86_REG_CF, irb.CreateICmpNE(op0, zero)},
		 {X86_REG_OF, irb.CreateICmpEQ(op0, intMin)}});
	storeOp(xi->operands[0], sub, irb);
}

/**
 * X86_INS_NOP, X86_INS_UD2, X86_INS_UD2B, X86_INS_FNOP, X86_INS_FDISI8087_NOP,
 * X86_INS_FENI8087_NOP
 *
 * X86_INS_FNSTCW - ignore FPU control word store.
 * X86_INS_FLDCW - ignore FPU control word load.
 *
 * Complete list from the old semantics:
 * IRETD, IRET, STI, CLI, VERR, VERW, LMSW, LTR,
 * SMSW, CLTS, INVD, LOCK, RSM, RDMSR, WRMSR, RDPMC, SYSENTER,
 * SYSEXIT, XGETBV, LAR, LSL, INVPCID, SLDT, LLDT, SGDT, SIDT, LGDT, LIDT,
 * XSAVE, XRSTOR, XSAVEOPT, INVLPG, FLDENV, ARPL,
 * STR,
 * FWAIT, FNOP, WAIT,
 * PAUSE,
 * PREFETCH, PREFETCHNTA, PREFETCHT0, PREFETCHT1, PREFETCHT2, PREFETCHW
 */
void Capstone2LlvmIrTranslatorX86_impl::translateNop(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	// nothing

	// X86_INS_NOP -> true nop
	// X86_INS_UD2 -> undefined
	// X86_INS_UD2B -> undefined
	// X86_INS_FNOP -> FPU nop
}

/**
 * X86_INS_LFENCE, X86_INS_SFENCE, X86_INS_MFENCE
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFence(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	llvm::AtomicOrdering ord = llvm::AtomicOrdering::SequentiallyConsistent;
	if (i->id == X86_INS_LFENCE)
	{
		ord = llvm::AtomicOrdering::Acquire;
	}
	else if (i->id == X86_INS_SFENCE)
	{
		ord = llvm::AtomicOrdering::Release;
	}
	irb.CreateFence(ord);
}

/**
 * X86_INS_FNINIT
 * This was modeled as empty (nop) instruction in an old semantics, but it
 * does set some values. Not all of the set objects are represented in our
 * current environment, and therefore we are not able to set them all.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFninit(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* zero = irb.getFalse();
	auto* one = irb.getTrue();
	auto* i2Set = llvm::ConstantInt::get(irb.getIntNTy(2), 3); // 0b11

	// FPUControlWord = 0x37F; (0x37F = 00000011 01111111)
	storeRegister(X87_REG_IM, one, irb);
	storeRegister(X87_REG_DM, one, irb);
	storeRegister(X87_REG_ZM, one, irb);
	storeRegister(X87_REG_OM, one, irb);
	storeRegister(X87_REG_UM, one, irb);
	storeRegister(X87_REG_PM, one, irb);
	storeRegister(X87_REG_PC, i2Set, irb);
	storeRegister(X87_REG_RC, zero, irb);
	storeRegister(X87_REG_X, zero, irb);
	// FPUStatusWord = 0;
	storeRegister(X87_REG_IE, zero, irb);
	storeRegister(X87_REG_DE, zero, irb);
	storeRegister(X87_REG_ZE, zero, irb);
	storeRegister(X87_REG_OE, zero, irb);
	storeRegister(X87_REG_UE, zero, irb);
	storeRegister(X87_REG_PE, zero, irb);
	storeRegister(X87_REG_SF, zero, irb);
	storeRegister(X87_REG_ES, zero, irb);
	storeRegister(X87_REG_C0, zero, irb);
	storeRegister(X87_REG_C1, zero, irb);
	storeRegister(X87_REG_C2, zero, irb);
	storeRegister(X87_REG_C3, zero, irb);
	storeRegister(X87_REG_TOP, zero, irb);
	storeRegister(X87_REG_B, zero, irb);
	// FPUTagWord = 0xFFFF;
	// FPUDataPointer = 0;
	// FPUInstructionPointer = 0;
	// FPULastInstructionOpcode = 0;
}

/**
 * X86_INS_NOT
 */
void Capstone2LlvmIrTranslatorX86_impl::translateNot(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	if (hasLockPrefix(xi) && xi->operands[0].type == X86_OP_MEM)
	{
		auto* addr = loadOp(xi->operands[0], irb, nullptr, true);
		if (!addr)
		{
			return;
		}
		auto* elem = getIntegerTypeFromByteSize(_module, xi->operands[0].size);
		auto* ones = llvm::ConstantInt::getSigned(elem, -1);
		auto* ptr = intToPtr(irb, addr, elem, getAddrSpace(xi->operands[0].mem.segment));
		auto* old = irb.CreateAtomicRMW(
				llvm::AtomicRMWInst::Xor,
				ptr,
				ones,
				llvm::MaybeAlign(),
				llvm::AtomicOrdering::SequentiallyConsistent);
		attachPointeeType(old, elem);
		return;
	}

	op0 = loadOpUnary(xi, irb);
	auto* negativeOne = llvm::ConstantInt::getSigned(op0->getType(), -1);

	auto* xorOp = irb.CreateXor(op0, negativeOne);

	storeOp(xi->operands[0], xorOp, irb);
}

/**
 * X86_INS_OR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateOr(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (tryTranslateLockedRmw(i, xi, irb, llvm::AtomicRMWInst::Or))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	auto* orOp = irb.CreateOr(op0, op1);

	storeRegistersPlusSflags(irb, orOp, {
			{X86_REG_AF, irb.getInt1(false)},   // undef
			{X86_REG_CF, irb.getInt1(false)},   // cleared
			{X86_REG_OF, irb.getInt1(false)}}); // cleared
	storeOp(xi->operands[0], orOp, irb);
}

/**
 * X86_INS_POP
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePop(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	auto* sp = loadRegister(getStackPointerRegister(), irb);

	auto* it = getIntegerTypeFromByteSize(_module, xi->operands[0].size);
	auto* l = loadIntPtr(irb, sp, it);
	storeOp(xi->operands[0], l, irb);

	auto* ci = llvm::ConstantInt::get(sp->getType(), xi->operands[0].size);
	auto* add = irb.CreateAdd(sp, ci);
	storeRegister(getStackPointerRegister(), add, irb);
}

/**
 * X86_INS_POPAL == POPAD (32-bit), X86_INS_POPAW == POPA (16-bit)
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePopa(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* sp = loadRegister(getStackPointerRegister(), irb);
	auto* t = getIntegerTypeFromByteSize(_module, xi->addr_size);
	auto* c = llvm::ConstantInt::get(sp->getType(), xi->addr_size);

	auto* a1 = sp;
	auto* a2 = irb.CreateAdd(a1, c);
	auto* a3 = irb.CreateAdd(a2, c);
	auto* a4 = irb.CreateAdd(a3, c); // unused
	auto* a5 = irb.CreateAdd(a4, c);
	auto* a6 = irb.CreateAdd(a5, c);
	auto* a7 = irb.CreateAdd(a6, c);
	auto* a8 = irb.CreateAdd(a7, c);
	auto* a9 = irb.CreateAdd(a8, c);

	// 67 61 = popal with addr size == 2 -> probbaly behaves like popaw
	//
	if (i->id == X86_INS_POPAL && xi->addr_size == 4)
	{
		storeRegisters(irb, {
			{X86_REG_EDI, loadIntPtr(irb, a1, t)},
			{X86_REG_ESI, loadIntPtr(irb, a2, t)},
			{X86_REG_EBP, loadIntPtr(irb, a3, t)},
			{X86_REG_EBX, loadIntPtr(irb, a5, t)},
			{X86_REG_EDX, loadIntPtr(irb, a6, t)},
			{X86_REG_ECX, loadIntPtr(irb, a7, t)},
			{X86_REG_EAX, loadIntPtr(irb, a8, t)},
			{getStackPointerRegister(), a9}});
	}
	else if ((i->id == X86_INS_POPAW
					&& (xi->addr_size == 2 || xi->addr_size == 4))
			|| (i->id == X86_INS_POPAL && xi->addr_size == 2))
	{
		storeRegisters(irb, {
			{X86_REG_DI, loadIntPtr(irb, a1, t)},
			{X86_REG_SI, loadIntPtr(irb, a2, t)},
			{X86_REG_BP, loadIntPtr(irb, a3, t)},
			{X86_REG_BX, loadIntPtr(irb, a5, t)},
			{X86_REG_DX, loadIntPtr(irb, a6, t)},
			{X86_REG_CX, loadIntPtr(irb, a7, t)},
			{X86_REG_AX, loadIntPtr(irb, a8, t)},
			{getStackPointerRegister(), a9}});
	}
	else
	{
		throw GenericError("unhandled combination");
	}
}

/**
 * X86_INS_POPF, X86_INS_POPFD, X86_INS_POPFQ
 * This currently does only what original model did.
 * The operations are more complicated, setting of some flags is conditoned by
 * some runtime CPU modes. I don't know if we can/need to solve this.
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePopEflags(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* sp = loadRegister(getStackPointerRegister(), irb);
	auto* it = getIntegerTypeFromByteSize(_module, xi->addr_size);
	auto* l = loadIntPtr(irb, sp, it);

	auto* ci = llvm::ConstantInt::get(sp->getType(), xi->addr_size);
	auto* add = irb.CreateAdd(sp, ci);
	storeRegister(getStackPointerRegister(), add, irb);

	auto* zero = llvm::ConstantInt::get(l->getType(), 0);
	auto* cf = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 0));
	// reserved
	auto* pf = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 2));
	// reserved
	auto* af = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 4));
	// reserved
	auto* zf = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 6));
	auto* sf = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 7));
	auto* tf = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 8));
	auto* iff = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 9));
	auto* df = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 10));
	auto* of = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 11));
//	auto* iopl = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), (1 << 12) + (1 << 13)));
	auto* nt = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 14));

	storeRegisters(irb, {
		{X86_REG_CF, irb.CreateICmpNE(cf, zero)},
		{X86_REG_PF, irb.CreateICmpNE(pf, zero)},
		{X86_REG_AF, irb.CreateICmpNE(af, zero)},
		{X86_REG_ZF, irb.CreateICmpNE(zf, zero)},
		{X86_REG_SF, irb.CreateICmpNE(sf, zero)},
		{X86_REG_TF, irb.CreateICmpNE(tf, zero)},
		{X86_REG_IF, irb.CreateICmpNE(iff, zero)},
		{X86_REG_DF, irb.CreateICmpNE(df, zero)},
		{X86_REG_OF, irb.CreateICmpNE(of, zero)},
//		{X86_REG_IOPL, irb.CreateICmpNE(iopl, zero)},
		{X86_REG_NT, irb.CreateICmpNE(nt, zero)}});

	if (i->id == X86_INS_POPFD || i->id == X86_INS_POPFQ)
	{
		// reserved
		auto* rf = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 16));
//		auto* vm = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 17));
		auto* ac = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 18));
//		auto* vif = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 19));
//		auto* vip = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 20));
		auto* id = irb.CreateAnd(l, llvm::ConstantInt::get(l->getType(), 1 << 21));

		storeRegisters(irb, {
			{X86_REG_RF, irb.CreateICmpNE(rf, zero)},
//			{X86_REG_VM, irb.CreateICmpNE(vm, zero)},
			{X86_REG_AC, irb.CreateICmpNE(ac, zero)},
//			{X86_REG_VIF, irb.CreateICmpNE(vif, zero)},
//			{X86_REG_VIP, irb.CreateICmpNE(vip, zero)},
			{X86_REG_ID, irb.CreateICmpNE(id, zero)}});
	}
}

/**
 * X86_INS_PUSH
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePush(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	op0 = loadOpUnary(xi, irb);
	auto* sp = loadRegister(getStackPointerRegister(), irb);

	auto* ci = llvm::ConstantInt::get(sp->getType(), xi->operands[0].size);
	auto* sub = irb.CreateSub(sp, ci);
	storeIntPtr(irb, op0, sub, op0->getType());
	storeRegister(getStackPointerRegister(), sub, irb);
}

/**
 * X86_INS_PUSHAL = PUSHAD (32-bit), X86_INS_PUSHAW = PUSHA (16-bit)
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePusha(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* sp = loadRegister(getStackPointerRegister(), irb);
	llvm::Type* t = nullptr; // getIntegerTypeFromByteSize(_module, xi->addr_size);
	std::size_t bsz = 0;
	if (i->id == X86_INS_PUSHAL)
	{
		t = irb.getInt32Ty();
		bsz = 4;
	}
	else if (i->id == X86_INS_PUSHAW)
	{
		t = irb.getInt16Ty();
		bsz = 2;
	}
	auto* c = llvm::ConstantInt::get(sp->getType(), bsz);

	auto* a1 = irb.CreateSub(sp, c);
	auto* a2 = irb.CreateSub(a1, c);
	auto* a3 = irb.CreateSub(a2, c);
	auto* a4 = irb.CreateSub(a3, c);
	auto* a5 = irb.CreateSub(a4, c);
	auto* a6 = irb.CreateSub(a5, c);
	auto* a7 = irb.CreateSub(a6, c);
	auto* a8 = irb.CreateSub(a7, c);

	if (i->id == X86_INS_PUSHAL)
	{
		storeIntPtr(irb, loadRegister(X86_REG_EAX, irb), a1, t);
		storeIntPtr(irb, loadRegister(X86_REG_ECX, irb), a2, t);
		storeIntPtr(irb, loadRegister(X86_REG_EDX, irb), a3, t);
		storeIntPtr(irb, loadRegister(X86_REG_EBX, irb), a4, t);
		storeIntPtr(irb, irb.CreateZExtOrTrunc(sp, t), a5, t);
		storeIntPtr(irb, loadRegister(X86_REG_EBP, irb), a6, t);
		storeIntPtr(irb, loadRegister(X86_REG_ESI, irb), a7, t);
		storeIntPtr(irb, loadRegister(X86_REG_EDI, irb), a8, t);
		storeRegister(getStackPointerRegister(), a8, irb);
	}
	else if (i->id == X86_INS_PUSHAW)
	{
		storeIntPtr(irb, loadRegister(X86_REG_AX, irb), a1, t);
		storeIntPtr(irb, loadRegister(X86_REG_CX, irb), a2, t);
		storeIntPtr(irb, loadRegister(X86_REG_DX, irb), a3, t);
		storeIntPtr(irb, loadRegister(X86_REG_BX, irb), a4, t);
		storeIntPtr(irb, irb.CreateZExtOrTrunc(sp, t), a5, t);
		storeIntPtr(irb, loadRegister(X86_REG_BP, irb), a6, t);
		storeIntPtr(irb, loadRegister(X86_REG_SI, irb), a7, t);
		storeIntPtr(irb, loadRegister(X86_REG_DI, irb), a8, t);
		storeRegister(getStackPointerRegister(), a8, irb);
	}
}

/**
 * X86_INS_PUSHF, X86_INS_PUSHFD, X86_INS_PUSHFQ
 * See @c translatePopEflags() comment.
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePushEflags(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* it = getIntegerTypeFromByteSize(_module, xi->addr_size);

	auto* cf = irb.CreateZExt(loadRegister(X86_REG_CF, irb), it);
	// reserved
	auto* pf = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_PF, irb), it), 2);
	// reserved
	auto* af = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_AF, irb), it), 4);
	// reserved
	auto* zf = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_ZF, irb), it), 6);
	auto* sf = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_SF, irb), it), 7);
	auto* tf = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_TF, irb), it), 8);
	auto* iff = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_IF, irb), it), 9);
	auto* df = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_DF, irb), it), 10);
	auto* of = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_OF, irb), it), 11);
//	auto* iopl = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_IOPL, irb), it), 13);
	auto* nt = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_NT, irb), it), 14);
	// reserved

	auto* val = cf;
	// This was in original model, but I did not find a reason for it.
	val = irb.CreateOr(val, llvm::ConstantInt::get(val->getType(), 2));
	val = irb.CreateOr(val, pf);
	val = irb.CreateOr(val, af);
	val = irb.CreateOr(val, zf);
	val = irb.CreateOr(val, sf);
	val = irb.CreateOr(val, tf);
	val = irb.CreateOr(val, iff);
	val = irb.CreateOr(val, df);
	val = irb.CreateOr(val, of);
	val = irb.CreateOr(val, nt);

	// PUSHFD and PUSHFQ, not POPFD and POPFQ -- this is translatePushEflags,
	// and the only ids dispatched here are PUSHF, PUSHFD and PUSHFQ, so the
	// test named two instructions that never arrive and the branch was dead.
	// A copy of the identical (and correct) test in translatePopEflags.
	//
	// The consequence is that AC and ID could be written by POPFD and never
	// read back, which breaks the canonical CPUID probe: set EFLAGS.ID with
	// popfd, push the flags again and compare. Measured on this machine the
	// round-trip returns bit 21 set; this returned zero, so the probe always
	// concluded CPUID was unsupported.
	if (i->id == X86_INS_PUSHFD || i->id == X86_INS_PUSHFQ)
	{
//		auto* rf = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_RF, irb), it), 16);
//		auto* vm = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_VM, irb), it), 17);
		auto* ac = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_AC, irb), it), 18);
//		auto* vif = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_VIF, irb), it), 19);
//		auto* vip = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_VIP, irb), it), 20);
		auto* id = irb.CreateShl(irb.CreateZExt(loadRegister(X86_REG_ID, irb), it), 21);

		val = irb.CreateOr(val, ac);
		val = irb.CreateOr(val, id);
	}

	auto* sp = loadRegister(getStackPointerRegister(), irb);
	auto* ci = llvm::ConstantInt::get(sp->getType(), xi->addr_size);
	auto* sub = irb.CreateSub(sp, ci);
	storeIntPtr(irb, val, sub, val->getType());
	storeRegister(getStackPointerRegister(), sub, irb);
}

/**
 * X86_INS_RET, X86_INS_RETF, X86_INS_RETFQ
 */
void Capstone2LlvmIrTranslatorX86_impl::translateRet(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, xi, irb);

	bool far = i->id != X86_INS_RET;
	auto* sp = loadRegister(getStackPointerRegister(), irb);
	auto sz = 0;
	switch (_origBasicMode)
	{
		case CS_MODE_16:
			sz = xi->prefix[2] == X86_PREFIX_OPSIZE ? 4 : 2;
			break;
		case CS_MODE_32:
			sz = xi->prefix[2] == X86_PREFIX_OPSIZE ? 2 : 4;
			break;
		case CS_MODE_64:
			sz = xi->prefix[2] == X86_PREFIX_OPSIZE ? 4 : 8;
			break;
		default:
			throw GenericError("Unhandled mode in translateRet().");
	}
	if (!sz)
	{
		throw GenericError("Uninitialized size in translateRet().");
	}
	op0 = nullptr;
	if (xi->op_count == 1)
	{
		op0 = loadOpUnary(xi, irb);
	}

	auto* it = getIntegerTypeFromByteSize(_module, sz);
	auto* l = loadIntPtr(irb, sp, it);

	auto* ci = llvm::ConstantInt::get(sp->getType(), sz);
	auto* add = irb.CreateAdd(sp, ci);

	if (far)
	{
		auto* l2 = loadIntPtr(irb, add, it);
		storeRegister(X86_REG_CS, l2, irb);
		add = irb.CreateAdd(add, ci);
	}
	if (op0)
	{
		op0 = irb.CreateZExtOrTrunc(op0, add->getType());
		add = irb.CreateAdd(add, op0);
	}

	storeRegister(getStackPointerRegister(), add, irb);

	generateReturnFunctionCall(irb, l);
}

/**
 * X86_INS_SAHF
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSahf(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* ah = loadRegister(X86_REG_AH, irb);
	auto* t = ah->getType();
	auto* zero = irb.getInt8(0);

	storeRegister(X86_REG_CF, irb.CreateAnd(ah, llvm::ConstantInt::get(t, 1 << 0)), irb);
	// Bit 1 of RFLAGS is set to 1, but we have no way of setting it.
	storeRegister(X86_REG_PF, irb.CreateICmpNE(
			irb.CreateAnd(ah, llvm::ConstantInt::get(t, 1 << 2)), zero), irb);
	// Bit 3 of RFLAGS is set to 0, but we have no way of setting it.
	storeRegister(X86_REG_AF, irb.CreateICmpNE(
			irb.CreateAnd(ah, llvm::ConstantInt::get(t, 1 << 4)), zero), irb);
	// Bit 5 of RFLAGS is set to 0, but we have no way of setting it.
	storeRegister(X86_REG_ZF, irb.CreateICmpNE(
			irb.CreateAnd(ah, llvm::ConstantInt::get(t, 1 << 6)), zero), irb);
	storeRegister(X86_REG_SF, irb.CreateICmpNE(
			irb.CreateAnd(ah, llvm::ConstantInt::get(t, 1 << 7)), zero), irb);
}

/**
 * X86_INS_SALC
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSalc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* cf = loadRegister(X86_REG_CF, irb);
	auto* icmp = irb.CreateICmpEQ(cf, irb.getInt1(false));
	auto* v = irb.CreateSelect(icmp, irb.getInt8(0), irb.getInt8(0xff));

	storeRegister(X86_REG_AL, v, irb);
}

/**
 * X86_INS_SBB
 * op0 = op0 - (op1 + CF)
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSbb(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* cf = loadRegister(X86_REG_CF, irb, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);

	// The carry is applied ONCE, here. This used to fold it into op1 and then
	// hand the modified op1 to generateBorrowSubC() and friends, which load
	// CF themselves and apply it a second time -- so with an incoming carry
	// of 1 the borrow came out wrong for almost every operand pair. ADC next
	// door has always done it this way, keeping its operands and passing the
	// carry explicitly; SBB did not.
	//
	// Found by executing 400 random operand pairs per instruction on this
	// machine's CPU and comparing the flags; SBB was 126 of the 127
	// disagreements, and ADC had none.
	auto* sub = irb.CreateSub(irb.CreateSub(op0, op1), cf);
	auto* cfBit = irb.CreateTrunc(cf, irb.getInt1Ty());
	auto* zero = llvm::ConstantInt::get(op0->getType(), 0);
	auto* fifteen = llvm::ConstantInt::get(op0->getType(), 15);

	// CF is `op0 < op1 + carry` without wrapping. The carry is 0 or 1, so
	// that is `op0 < op1`, or `op0 == op1` with a carry in -- which avoids
	// the `op1 + 1` that wraps when op1 is all ones.
	auto* borrow = irb.CreateOr(irb.CreateICmpULT(op0, op1), irb.CreateAnd(irb.CreateICmpEQ(op0, op1), cfBit));

	// AF is the same question asked of the low nibble.
	auto* lo0 = irb.CreateAnd(op0, fifteen);
	auto* lo1 = irb.CreateAnd(op1, fifteen);
	auto* borrow4 = irb.CreateOr(irb.CreateICmpULT(lo0, lo1), irb.CreateAnd(irb.CreateICmpEQ(lo0, lo1), cfBit));

	// OF: the sign of the result disagrees with op0's, and op0's disagreed
	// with op1's. Folding the borrow into `sub` first makes this exact.
	auto* of = irb.CreateICmpSLT(irb.CreateAnd(irb.CreateXor(op0, op1), irb.CreateXor(op0, sub)), zero);

	storeRegistersPlusSflags(irb, sub, {{X86_REG_AF, borrow4}, {X86_REG_CF, borrow}, {X86_REG_OF, of}});

	storeOp(xi->operands[0], sub, irb);
}

/**
 * The shift and rotate instructions write their destination even when the
 * masked count is zero. That is invisible for a memory destination or a
 * full-width register, but a write to a 32-bit register zeroes bits 63:32, so
 * `shl eax, cl` with cl=0 clears the top half of RAX, and `shld eax, ecx, cl`
 * with cl=0x60 -- a count that masks to zero -- does the same.
 *
 * Measured on the hardware rather than read off the manual, which says only
 * that the FLAGS are unaffected and is silent about the write: all nine of
 * shl, shr, sar, rol, ror, rcl, rcr, shld and shrd zero the upper half, at a
 * literal count of zero and at counts that mask to zero.
 *
 * Every one of those translators wraps its body in a "count is not zero"
 * branch, so this has to run before the branch. Storing the loaded value back
 * is a no-op in value terms and gets the destination's width right for free.
 * It is skipped for a memory destination, where there is nothing to widen and
 * a redundant store would be a new memory write for later analyses to explain.
 */
void Capstone2LlvmIrTranslatorX86_impl::generateShiftDestinationWrite(
	cs_x86_op& dst, llvm::Value* val, llvm::IRBuilder<>& irb)
{
	if (dst.type == X86_OP_REG)
	{
		storeOp(dst, val, irb);
	}
}

namespace {

/**
 * What an 8- or 16-bit shift or rotate does with a count that does not fit.
 *
 * All of these first mask the count to five bits, or six for a 64-bit
 * operand. That much was always here, and at 32 and 64 bits it is the whole
 * rule -- five bits reach 31, six reach 63, and both are inside the operand.
 * At 8 and 16 bits they are not, and what happens next is NOT the same
 * instruction to instruction. Measured on this machine:
 *
 *   ROL/ROR  reduce MOD the width. A rotate by more than its width is just a
 *            rotate; `rol al, cl` with cl = 20 rotates by 4.
 *   RCL/RCR  reduce MOD the width PLUS ONE, because the carry is a genuine
 *            extra bit of the value being rotated. `rcl al, cl` with cl = 9
 *            is the identity and cl = 8 is not, so the modulus is 9.
 *   SHL/SHR  do NOT reduce at all. The SDM's decrementing loop runs the whole
 *   SAR      masked count, so the answer is 0 -- or all-sign for SAR.
 *            `shl al, cl` with cl = 20 measured 0x00, NOT the modulo-8 0x20;
 *            `sar al, cl` with cl >= 8 measured 0xff.
 *
 * LLVM calls a shift by at least the operand's width poison whichever of
 * those three the architecture means, so each has to be said out loud. That
 * the emulator reduces modulo the width made the first two look right and the
 * third look plausible, which is why this survived: it agreed with the
 * hardware for the rotates by accident, and disagreed for the shifts in a way
 * only a count of 8 or more could show.
 *
 * The oracle this should have been caught by declined to draw such counts at
 * 8 and 16 bits, on a comment claiming the destination was undefined there.
 * The destination is defined; only CF is not.
 */
enum class eShiftCount
{
	/// ROL, ROR -- reduce MOD width.
	Rotate,
	/// RCL, RCR -- reduce MOD (width + 1).
	RotateThroughCarry,
	/// SHL, SHR, SAR -- do not reduce; the caller saturates instead.
	Linear
};

llvm::Value* maskShiftCount(llvm::Value* cnt, unsigned op0BitW, eShiftCount kind, llvm::IRBuilder<>& irb)
{
	unsigned maskC = op0BitW == 64 ? 0x3f : 0x1f;
	cnt = irb.CreateAnd(cnt, llvm::ConstantInt::get(cnt->getType(), maskC));

	// At 32 and 64 bits the five- or six-bit mask has already done every
	// reduction below: 31 < 32 and < 33, 63 < 64 and < 65.
	if (op0BitW >= 32)
	{
		return cnt;
	}

	switch (kind)
	{
	case eShiftCount::RotateThroughCarry:
		return irb.CreateURem(cnt, llvm::ConstantInt::get(cnt->getType(), op0BitW + 1));
	case eShiftCount::Rotate:
	case eShiftCount::Linear: break;
	}
	return cnt;
}

/**
 * How far ROL and ROR actually rotate, given the five- or six-bit count.
 *
 * The reduction MOD the width belongs HERE and not in the count itself,
 * because the two are read for different questions. `rol al, cl` with cl = 8
 * rotates by nothing -- and still writes CF. Measured: AL = 0x5a with CF = 1
 * beforehand comes back AL = 0x5a and CF = 0, which is LSB(result). The SDM
 * says as much in two separate lines: the rotate loop runs `(count & mask)
 * MOD size` times, and the CF update is guarded by `(count & mask) != 0`.
 *
 * Reducing the count itself made the whole body conditional on the reduced
 * value, so a count of 8, 16 or 24 on a byte skipped the CF write. RCL and
 * RCR are the other way round -- there the carry is a bit of the rotated
 * value, so a count that reduces to zero really does leave CF alone, which is
 * why they reduce the count and these do not.
 *
 * Since every width here is a power of two, MOD is an AND, and at 32 and 64
 * bits it is the same mask that has already been applied.
 */
llvm::Value* rotateAmount(llvm::Value* cnt, unsigned op0BitW, llvm::IRBuilder<>& irb)
{
	return irb.CreateAnd(cnt, llvm::ConstantInt::get(cnt->getType(), op0BitW - 1));
}

/**
 * The largest count SHL/SHR/SAR can be given without producing poison, plus
 * the flag saying the real count was past it.
 *
 * For SAR the clamp alone IS the answer -- shifting right by width-1 leaves
 * the sign bit in every position, which is what the hardware settles on -- so
 * that caller ignores \p tooBig. SHL and SHR have to select a zero back in.
 * \p tooBig is null when no clamp was needed, which is every 32- and 64-bit
 * operand and therefore almost all real code.
 */
llvm::Value* clampLinearShiftCount(llvm::Value* cnt, unsigned op0BitW, llvm::Value*& tooBig, llvm::IRBuilder<>& irb)
{
	tooBig = nullptr;
	if (op0BitW >= 32)
	{
		return cnt;
	}
	auto* widthC = llvm::ConstantInt::get(cnt->getType(), op0BitW);
	tooBig = irb.CreateICmpUGE(cnt, widthC);
	// umin rather than a select on `tooBig`. The two compute the same number,
	// but only umin states the bound LOCALLY: a reader of `select(n >= 8, 7,
	// n)` -- human or analysis -- has to correlate the condition with the arms
	// to see that the result is below 8, and LLVM's value tracking does not.
	// SHIFT-01 fired on this exact idiom, which is to say it fired on the fix
	// rather than on the bug.
	return irb.CreateBinaryIntrinsic(llvm::Intrinsic::umin, cnt, llvm::ConstantInt::get(cnt->getType(), op0BitW - 1));
}

} // anonymous namespace

/**
 * X86_INS_SHL == X86_INS_SAL
 */
void Capstone2LlvmIrTranslatorX86_impl::translateShiftLeft(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, xi, irb);

	op0 = loadOp(xi->operands[0], irb);
	if (xi->op_count == 2)
	{
		op1 = loadOp(xi->operands[1], irb);
		op1 = irb.CreateZExtOrTrunc(op1, op0->getType());
	}
	else
	{
		op1 = llvm::ConstantInt::get(op0->getType(), 1);
	}
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	op1 = maskShiftCount(op1, op0BitW, eShiftCount::Linear, irb);
	llvm::Value* tooBig = nullptr;
	llvm::Value* shAmt = clampLinearShiftCount(op1, op0BitW, tooBig, irb);
	auto* of = llvm::cast<llvm::Instruction>(loadRegister(X86_REG_OF, irb));
	auto* op1Zero = irb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 0));

	generateShiftDestinationWrite(xi->operands[0], op0, irb);

	// Sometimes (most of the times, not for op1 = CL) LLVM can eval cond brach
	// cond on-the-fly. Then this pattern creates stuff like:
	// br i1 false, x, y
	// It is not a big deal, because it will be optimized, but with a bit better
	// code here, we could generate much simpler customized translations.
	//
	llvm::IRBuilder<> bodyIrb(generateIfNotThen(op1Zero, irb));

	llvm::Value* shl = bodyIrb.CreateShl(op0, shAmt);
	if (tooBig)
	{
		// Everything shifted out. Measured, not assumed: `shl al, cl` with
		// cl = 20 gives 0x00.
		shl = bodyIrb.CreateSelect(tooBig, llvm::ConstantInt::get(shl->getType(), 0), shl);
	}
	generateSetSflags(shl, bodyIrb);
	storeOp(xi->operands[0], shl, bodyIrb);

	// CF is architecturally undefined once the count reaches the width, so the
	// clamped amount is used here only to keep the IR free of poison. The
	// subtraction can still wrap when the count is zero -- which this block is
	// guarded against, but by a BRANCH, and a branch is not something a local
	// analysis can use. Clamping again says it in the value itself, and costs
	// nothing: for every count this block really runs with, umin is identity.
	auto* cfOp1 = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(shAmt, llvm::ConstantInt::get(shAmt->getType(), 1)),
		llvm::ConstantInt::get(shAmt->getType(), op0BitW - 1));
	auto* cfShl = bodyIrb.CreateShl(op0, cfOp1);
	auto* cfIntT = llvm::cast<llvm::IntegerType>(cfShl->getType());
	auto* cfRightCount = llvm::ConstantInt::get(cfIntT, cfIntT->getBitWidth() - 1);
	auto* cfLow = bodyIrb.CreateLShr(cfShl, cfRightCount);
	storeRegister(X86_REG_CF, cfLow, bodyIrb);

	auto* ofLow = bodyIrb.CreateLShr(shl, cfRightCount);
	auto* ofIcmp = bodyIrb.CreateICmpNE(ofLow, cfLow);
	auto* ofIs1 = bodyIrb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 1));
	auto* ofV = bodyIrb.CreateSelect(ofIs1, ofIcmp, of);
	storeRegister(X86_REG_OF, ofV, bodyIrb);
}

/**
 * X86_INS_SHR, X86_INS_SAR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateShiftRight(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, xi, irb);

	op0 = loadOp(xi->operands[0], irb);
	if (xi->op_count == 2)
	{
		op1 = loadOp(xi->operands[1], irb);
		op1 = irb.CreateZExtOrTrunc(op1, op0->getType());
	}
	else
	{
		op1 = llvm::ConstantInt::get(op0->getType(), 1);
	}
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	op1 = maskShiftCount(op1, op0BitW, eShiftCount::Linear, irb);
	llvm::Value* tooBig = nullptr;
	llvm::Value* shAmt = clampLinearShiftCount(op1, op0BitW, tooBig, irb);
	auto* of = llvm::cast<llvm::Instruction>(loadRegister(X86_REG_OF, irb));
	auto* op1Zero = irb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 0));
	generateShiftDestinationWrite(xi->operands[0], op0, irb);
	llvm::IRBuilder<> bodyIrb(generateIfNotThen(op1Zero, irb));

	llvm::Value* shift = i->id == X86_INS_SHR ? bodyIrb.CreateLShr(op0, shAmt)  // X86_INS_SHR
											  : bodyIrb.CreateAShr(op0, shAmt); // X86_INS_SAR
	if (tooBig && i->id == X86_INS_SHR)
	{
		// SHR empties the operand; SAR does not, because the clamp to
		// width-1 has already left the sign bit in every position, which is
		// the answer `sar al, cl` gives for every cl of 8 or more.
		shift = bodyIrb.CreateSelect(tooBig, llvm::ConstantInt::get(shift->getType(), 0), shift);
	}
	generateSetSflags(shift, bodyIrb);
	storeOp(xi->operands[0], shift, bodyIrb);
	// CF is undefined at a count of at least the width; the clamped amount
	// keeps this out of poison territory. Clamped once more for the same
	// reason as in translateShiftLeft: the count-is-zero guarantee is a
	// branch, and the subtraction below would wrap without it.
	auto* cfOp1 = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(shAmt, llvm::ConstantInt::get(shAmt->getType(), 1)),
		llvm::ConstantInt::get(shAmt->getType(), op0BitW - 1));
	auto* cfShl = bodyIrb.CreateShl(llvm::ConstantInt::get(cfOp1->getType(), 1), cfOp1);
	auto* cfAnd = bodyIrb.CreateAnd(cfShl, op0);
	auto* cfIcmp = bodyIrb.CreateICmpNE(cfAnd, llvm::ConstantInt::get(cfAnd->getType(), 0));
	storeRegister(X86_REG_CF, cfIcmp, bodyIrb);
	auto* ofIs1 = bodyIrb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 1));
	llvm::Value* ofVal = nullptr;
	if (i->id == X86_INS_SHR)
	{
		ofVal = bodyIrb.CreateICmpSLT(op0, llvm::ConstantInt::get(op0->getType(), 0));
	}
	else if (i->id == X86_INS_SAR)
	{
		ofVal = bodyIrb.getInt1(false);
	}
	storeRegister(X86_REG_OF, bodyIrb.CreateSelect(ofIs1, ofVal, of), bodyIrb);
}

/**
 * X86_INS_SHLD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateShld(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(xi, irb);
	op2 = irb.CreateZExtOrTrunc(op2, op0->getType());
	auto* of = loadRegister(X86_REG_OF, irb);

	// The count is masked by the OPERAND size, not by the processor mode:
	// five bits for a 16- or 32-bit operand, six for a 64-bit one. This read
	// the mode instead, so in 64-bit mode `shld eax, ecx, cl` with cl=0xe0
	// reduced 224 to 32 rather than to 0 and then shifted an i32 by 32 --
	// poison, which the emulator evaluated to the source operand. The plain
	// shifts, fifty lines up, already mask on op0's width; this is the same
	// rule. These two were the only calls to getBasicMode() in this file.
	unsigned op0BitW = op0->getType()->getIntegerBitWidth();
	op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), op0BitW == 64 ? 0x3f : 0x1f));
	if (op0BitW < 32)
	{
		// A 16-bit SHLD/SHRD with a count above 15 is architecturally
		// undefined. Reducing it keeps the IR free of poison, which a shift
		// of an i16 by 20 would be -- and poison does not stay where it is
		// put once the optimiser sees it.
		op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), op0BitW - 1));
	}

	auto* op2Zero = irb.CreateICmpEQ(op2, llvm::ConstantInt::get(op2->getType(), 0));
	generateShiftDestinationWrite(xi->operands[0], op0, irb);
	llvm::IRBuilder<> bodyIrb(generateIfNotThen(op2Zero, irb));

	auto* shl = bodyIrb.CreateShl(op0, op2);
	auto* it = llvm::cast<llvm::IntegerType>(shl->getType());
	// The complement is the full width when the count is zero -- which this
	// block is guarded against, but by a BRANCH, and a branch is not a bound
	// a local analysis can use. Clamping states it in the value, and is the
	// identity for every count this block actually runs on.
	auto* sub = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(llvm::ConstantInt::get(it, it->getBitWidth()), op2),
		llvm::ConstantInt::get(it, it->getBitWidth() - 1));
	auto* srl = bodyIrb.CreateLShr(op1, sub);
	auto* orr = bodyIrb.CreateOr(srl, shl);
	generateSetSflags(orr, bodyIrb);
	storeOp(xi->operands[0], orr, bodyIrb);

	// Same again: count - 1 wraps at a count of zero.
	auto* subCf = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(op2, llvm::ConstantInt::get(op2->getType(), 1)),
		llvm::ConstantInt::get(op2->getType(), op0BitW - 1));
	auto* shlCf = bodyIrb.CreateShl(op0, subCf);
	auto* icmpCf = bodyIrb.CreateICmpSLT(shlCf, llvm::ConstantInt::getSigned(shlCf->getType(), 0));
	storeRegister(X86_REG_CF, icmpCf, bodyIrb);

	auto* icmpOf = bodyIrb.CreateICmpEQ(op2, llvm::ConstantInt::get(op2->getType(), 1));
	auto* xorOf = bodyIrb.CreateXor(orr, shlCf);
	auto* icmpOfV = bodyIrb.CreateICmpSLT(xorOf, llvm::ConstantInt::getSigned(xorOf->getType(), 0));
	auto* ofV = bodyIrb.CreateSelect(icmpOf, icmpOfV, of);
	storeRegister(X86_REG_OF, ofV, bodyIrb);
}

/**
 * X86_INS_SHRD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateShrd(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(xi, irb);
	op2 = irb.CreateZExtOrTrunc(op2, op0->getType());
	auto* of = loadRegister(X86_REG_OF, irb);

	// The count is masked by the OPERAND size, not by the processor mode:
	// five bits for a 16- or 32-bit operand, six for a 64-bit one. This read
	// the mode instead, so in 64-bit mode `shld eax, ecx, cl` with cl=0xe0
	// reduced 224 to 32 rather than to 0 and then shifted an i32 by 32 --
	// poison, which the emulator evaluated to the source operand. The plain
	// shifts, fifty lines up, already mask on op0's width; this is the same
	// rule. These two were the only calls to getBasicMode() in this file.
	unsigned op0BitW = op0->getType()->getIntegerBitWidth();
	op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), op0BitW == 64 ? 0x3f : 0x1f));
	if (op0BitW < 32)
	{
		// A 16-bit SHLD/SHRD with a count above 15 is architecturally
		// undefined. Reducing it keeps the IR free of poison, which a shift
		// of an i16 by 20 would be -- and poison does not stay where it is
		// put once the optimiser sees it.
		op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), op0BitW - 1));
	}

	auto* op2Zero = irb.CreateICmpEQ(op2, llvm::ConstantInt::get(op2->getType(), 0));
	generateShiftDestinationWrite(xi->operands[0], op0, irb);
	llvm::IRBuilder<> bodyIrb(generateIfNotThen(op2Zero, irb));

	auto* lshr = bodyIrb.CreateLShr(op0, op2);
	auto* it = llvm::cast<llvm::IntegerType>(op2->getType());
	// See translateShld: the complement is the full width at a count of zero,
	// which only the branch excludes.
	auto* sub = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(llvm::ConstantInt::get(it, it->getBitWidth()), op2),
		llvm::ConstantInt::get(it, it->getBitWidth() - 1));
	auto* shl = bodyIrb.CreateShl(op1, sub);
	auto* orr = bodyIrb.CreateOr(shl, lshr);
	generateSetSflags(orr, bodyIrb);
	storeOp(xi->operands[0], orr, bodyIrb);

	auto* subCf = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(op2, llvm::ConstantInt::get(op2->getType(), 1)),
		llvm::ConstantInt::get(op2->getType(), op0BitW - 1));
	auto* shlCf = bodyIrb.CreateShl(llvm::ConstantInt::get(subCf->getType(), 1), subCf);
	auto* andCf = bodyIrb.CreateAnd(shlCf, op0);
	auto* icmpCf = bodyIrb.CreateICmpNE(andCf, llvm::ConstantInt::get(andCf->getType(), 0));
	storeRegister(X86_REG_CF, icmpCf, bodyIrb);

	auto* icmpOf = bodyIrb.CreateICmpEQ(op2, llvm::ConstantInt::get(op2->getType(), 1));
	auto* xorOf = bodyIrb.CreateXor(orr, op0);
	auto* icmpOfV = bodyIrb.CreateICmpSLT(xorOf, llvm::ConstantInt::getSigned(xorOf->getType(), 0));
	auto* ofV = bodyIrb.CreateSelect(icmpOf, icmpOfV, of);
	storeRegister(X86_REG_OF, ofV, bodyIrb);
}

/**
 * X86_INS_RCR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateRcr(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	auto* doubleT = llvm::Type::getIntNTy(_module->getContext(), op0BitW*2);
	op1 = maskShiftCount(op1, op0BitW, eShiftCount::RotateThroughCarry, irb);
	auto* op1NotZero = irb.CreateICmpNE(op1, llvm::ConstantInt::get(op1->getType(), 0));

	generateShiftDestinationWrite(xi->operands[0], op0, irb);
	llvm::IRBuilder<> bodyIrb(generateIfThen(op1NotZero, irb));

	auto* cf = loadRegister(X86_REG_CF, bodyIrb, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);

	// See translateRcl: a count of exactly the width is in range after MOD
	// (width+1) and would make `lshr i8 %x, 8` poison, so the shift is done
	// at the doubled width where it is not.
	auto* op0Zext = bodyIrb.CreateZExt(op0, doubleT);
	auto* srlZext = bodyIrb.CreateLShr(op0Zext, bodyIrb.CreateZExt(op1, doubleT));
	auto* sub = bodyIrb.CreateSub(llvm::ConstantInt::get(op1->getType(), op0BitW + 1), op1);
	auto* subZext = bodyIrb.CreateZExt(sub, doubleT);
	auto* shl = bodyIrb.CreateShl(op0Zext, subZext);
	// w - n is w when n is zero, which the branch excludes and a local
	// analysis cannot know. SHIFT-01 found this one -- it is the same shape as
	// the two umins above and I had not spotted it by reading.
	auto* sub2 = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(llvm::ConstantInt::get(op1->getType(), op0BitW), op1),
		llvm::ConstantInt::get(op1->getType(), op0BitW - 1));
	auto* shl2 = bodyIrb.CreateShl(cf, sub2);
	auto* shl2Zext = bodyIrb.CreateZExt(shl2, doubleT);
	auto* or1 = bodyIrb.CreateOr(shl, srlZext);
	auto* or2 = bodyIrb.CreateOr(or1, shl2Zext);
	auto* or2Trunc = bodyIrb.CreateTrunc(or2, op0->getType());
	storeOp(xi->operands[0], or2Trunc, bodyIrb);

	// See the umin above: n - 1 wraps at n == 0, which the branch excludes and
	// a local analysis cannot know.
	auto* sub3 = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(op1, llvm::ConstantInt::get(op1->getType(), 1)),
		llvm::ConstantInt::get(op1->getType(), op0BitW - 1));
	auto* shl3 = bodyIrb.CreateShl(llvm::ConstantInt::get(sub3->getType(), 1), sub3);
	auto* and1 = bodyIrb.CreateAnd(shl3, op0);
	auto* cfIcmp = bodyIrb.CreateICmpNE(and1, llvm::ConstantInt::get(and1->getType(), 0));
	storeRegister(X86_REG_CF, cfIcmp, bodyIrb);

	auto* of = loadRegister(X86_REG_OF, bodyIrb);
	auto* ofSrl = bodyIrb.CreateLShr(op0, llvm::ConstantInt::get(op0->getType(), op0BitW - 1));
	auto* ofIcmp = bodyIrb.CreateICmpNE(ofSrl, cf);
	auto* op1Eq1 = bodyIrb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 1));
	auto* ofVal = bodyIrb.CreateSelect(op1Eq1, ofIcmp, of);
	storeRegister(X86_REG_OF, ofVal, bodyIrb);
}

/**
 * X86_INS_RCL
 */
void Capstone2LlvmIrTranslatorX86_impl::translateRcl(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	auto* doubleT = llvm::Type::getIntNTy(_module->getContext(), op0BitW*2);
	op1 = maskShiftCount(op1, op0BitW, eShiftCount::RotateThroughCarry, irb);
	auto* op1NotZero = irb.CreateICmpNE(op1, llvm::ConstantInt::get(op1->getType(), 0));

	generateShiftDestinationWrite(xi->operands[0], op0, irb);
	llvm::IRBuilder<> bodyIrb(generateIfThen(op1NotZero, irb));

	auto* cf = loadRegister(X86_REG_CF, bodyIrb, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);

	// MOD (width+1) leaves a count of exactly the width in range -- `rcl al,
	// cl` with cl = 8 is a real rotation of the nine-bit {CF, AL} quantity --
	// and `shl i8 %x, 8` is poison. Widening the shift keeps every bit that
	// the OR below needs and lets the final truncate drop the rest, which is
	// what the narrow shift was already relying on for smaller counts.
	auto* op0Zext = bodyIrb.CreateZExt(op0, doubleT);
	auto* shlZext = bodyIrb.CreateShl(op0Zext, bodyIrb.CreateZExt(op1, doubleT));
	auto* sub = bodyIrb.CreateSub(llvm::ConstantInt::get(op1->getType(), op0BitW + 1), op1);
	auto* subZext = bodyIrb.CreateZExt(sub, doubleT);
	auto* srl = bodyIrb.CreateLShr(op0Zext, subZext);
	// n - 1 wraps when n is zero. This block only runs for a non-zero n, but
	// that is a branch, and a branch is not a bound a local analysis can use;
	// clamping says it in the value. Identity for every n this block runs on.
	auto* sub2 = bodyIrb.CreateBinaryIntrinsic(
		llvm::Intrinsic::umin,
		bodyIrb.CreateSub(op1, llvm::ConstantInt::get(op1->getType(), 1)),
		llvm::ConstantInt::get(op1->getType(), op0BitW - 1));
	auto* shl2 = bodyIrb.CreateShl(cf, sub2);
	auto* shl2Zext = bodyIrb.CreateZExt(shl2, doubleT);
	auto* or1 = bodyIrb.CreateOr(srl, shlZext);
	auto* or2 = bodyIrb.CreateOr(or1, shl2Zext);
	auto* or2Trunc = bodyIrb.CreateTrunc(or2, op0->getType());
	storeOp(xi->operands[0], or2Trunc, bodyIrb);

	auto* shl3 = bodyIrb.CreateShl(op0, sub2);
	auto* srl2 = bodyIrb.CreateLShr(shl3, llvm::ConstantInt::get(shl3->getType(), op0BitW - 1));
	auto* cfIcmp = bodyIrb.CreateICmpNE(srl2, llvm::ConstantInt::get(srl2->getType(), 0));
	storeRegister(X86_REG_CF, cfIcmp, bodyIrb);

	auto* of = loadRegister(X86_REG_OF, bodyIrb);
	auto* ofSrl = bodyIrb.CreateLShr(or2Trunc, llvm::ConstantInt::get(or2Trunc->getType(), op0BitW - 1));
	auto* ofIcmp = bodyIrb.CreateICmpNE(ofSrl, srl2);
	auto* op1Eq1 = bodyIrb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 1));
	auto* ofVal = bodyIrb.CreateSelect(op1Eq1, ofIcmp, of);
	storeRegister(X86_REG_OF, ofVal, bodyIrb);
}

/**
 * X86_INS_ROL
 */
void Capstone2LlvmIrTranslatorX86_impl::translateRol(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	op1 = maskShiftCount(op1, op0BitW, eShiftCount::Rotate, irb);
	auto* op1NotZero = irb.CreateICmpNE(op1, llvm::ConstantInt::get(op1->getType(), 0));

	generateShiftDestinationWrite(xi->operands[0], op0, irb);
	llvm::IRBuilder<> bodyIrb(generateIfThen(op1NotZero, irb));

	// The body runs whenever the MASKED count is non-zero, but rotates by the
	// count reduced MOD the width -- which may be zero, and then this has to
	// come out as the identity rather than as poison. Masking the complement
	// with width-1 does that: at a rotation of zero both halves are the whole
	// operand and the OR of them is the operand itself.
	auto* rot = rotateAmount(op1, op0BitW, bodyIrb);
	auto* shl = bodyIrb.CreateShl(op0, rot);
	auto* sub = bodyIrb.CreateAnd(
		bodyIrb.CreateSub(llvm::ConstantInt::get(rot->getType(), op0BitW), rot),
		llvm::ConstantInt::get(rot->getType(), op0BitW - 1));
	auto* srl = bodyIrb.CreateLShr(op0, sub);
	auto* orr = bodyIrb.CreateOr(srl, shl);

	storeOp(xi->operands[0], orr, bodyIrb);

	auto* and1 = bodyIrb.CreateAnd(orr, llvm::ConstantInt::get(orr->getType(), 1));
	auto* cfIcmp = bodyIrb.CreateICmpNE(and1, llvm::ConstantInt::get(orr->getType(), 0));
	storeRegister(X86_REG_CF, cfIcmp, bodyIrb);

	auto* of = loadRegister(X86_REG_OF, bodyIrb);
	auto* ofSrl = bodyIrb.CreateLShr(orr, llvm::ConstantInt::get(orr->getType(), op0BitW - 1));
	auto* ofIcmp = bodyIrb.CreateICmpNE(ofSrl, and1);
	auto* op1Eq1 = bodyIrb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 1));
	auto* ofVal = bodyIrb.CreateSelect(op1Eq1, ofIcmp, of);
	storeRegister(X86_REG_OF, ofVal, bodyIrb);
}

/**
 * X86_INS_ROR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateRor(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(op0->getType())->getBitWidth();
	op1 = maskShiftCount(op1, op0BitW, eShiftCount::Rotate, irb);
	auto* op1NotZero = irb.CreateICmpNE(op1, llvm::ConstantInt::get(op1->getType(), 0));

	generateShiftDestinationWrite(xi->operands[0], op0, irb);
	llvm::IRBuilder<> bodyIrb(generateIfThen(op1NotZero, irb));

	// See translateRol: the body is gated on the masked count, the rotation
	// uses the count reduced MOD the width, and the complement is masked so a
	// rotation of zero is the identity instead of a shift by the full width.
	auto* rot = rotateAmount(op1, op0BitW, bodyIrb);
	auto* srl = bodyIrb.CreateLShr(op0, rot);
	auto* sub = bodyIrb.CreateAnd(
		bodyIrb.CreateSub(llvm::ConstantInt::get(rot->getType(), op0BitW), rot),
		llvm::ConstantInt::get(rot->getType(), op0BitW - 1));
	auto* shl = bodyIrb.CreateShl(op0, sub);
	auto* orr = bodyIrb.CreateOr(srl, shl);
	storeOp(xi->operands[0], orr, bodyIrb);

	auto* cfSrl = bodyIrb.CreateLShr(orr, llvm::ConstantInt::get(orr->getType(), op0BitW - 1));
	auto* cfIcmp = bodyIrb.CreateICmpNE(cfSrl, llvm::ConstantInt::get(cfSrl->getType(), 0));
	storeRegister(X86_REG_CF, cfIcmp, bodyIrb);

	auto* of = loadRegister(X86_REG_OF, bodyIrb);
	auto* ofSrl = bodyIrb.CreateLShr(orr, llvm::ConstantInt::get(orr->getType(), op0BitW - 2));
	auto* ofAnd = bodyIrb.CreateAnd(ofSrl, llvm::ConstantInt::get(ofSrl->getType(), 1));
	auto* ofIcmp = bodyIrb.CreateICmpNE(cfSrl, ofAnd);
	auto* op1Eq1 = bodyIrb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 1));
	auto* ofVal = bodyIrb.CreateSelect(op1Eq1, ofIcmp, of);
	storeRegister(X86_REG_OF, ofVal, bodyIrb);
}

/**
 * X86_INS_STC
 */
void Capstone2LlvmIrTranslatorX86_impl::translateStc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	storeRegister(X86_REG_CF, irb.getInt1(true), irb);
}

/**
 * X86_INS_STD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateStd(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	storeRegister(X86_REG_DF, irb.getInt1(true), irb);
}

/**
 * X86_INS_SUB, X86_INS_CMP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSub(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (i->id == X86_INS_SUB
			&& tryTranslateLockedRmw(i, xi, irb, llvm::AtomicRMWInst::Sub))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	auto* sub = irb.CreateSub(op0, op1);

	storeRegistersPlusSflags(irb, sub, {
			{X86_REG_AF, generateBorrowSubInt4(op0, op1, irb)},
			{X86_REG_CF, generateBorrowSub(op0, op1, irb)},
			{X86_REG_OF, generateOverflowSub(sub, op0, op1, irb)}});
	if (i->id == X86_INS_SUB)
	{
		storeOp(xi->operands[0], sub, irb);
	}
}

/**
 * X86_INS_XCHG
 */
void Capstone2LlvmIrTranslatorX86_impl::translateXchg(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	// xchg r/m is implicitly locked even without a LOCK prefix.
	int memIdx = -1;
	int otherIdx = -1;
	if (xi->operands[0].type == X86_OP_MEM)
	{
		memIdx = 0;
		otherIdx = 1;
	}
	else if (xi->operands[1].type == X86_OP_MEM)
	{
		memIdx = 1;
		otherIdx = 0;
	}
	if (memIdx >= 0)
	{
		auto* addr = loadOp(xi->operands[memIdx], irb, nullptr, true);
		auto* val = loadOp(xi->operands[otherIdx], irb);
		if (!addr || !val)
		{
			return;
		}
		auto* elem = getIntegerTypeFromByteSize(_module, xi->operands[memIdx].size);
		val = generateTypeConversion(irb, val, elem, eOpConv::SEXT_TRUNC_OR_BITCAST);
		auto* ptr = intToPtr(irb, addr, elem, getAddrSpace(xi->operands[memIdx].mem.segment));
		auto* old = irb.CreateAtomicRMW(
				llvm::AtomicRMWInst::Xchg,
				ptr,
				val,
				llvm::MaybeAlign(),
				llvm::AtomicOrdering::SequentiallyConsistent);
		attachPointeeType(old, elem);
		storeOp(xi->operands[otherIdx], old, irb);
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	// TODO:
	// Capstone may generate something like this:
	// "xchg eax, bp" at 0x1100107b in x86-pe-df4c5b7cdbb714f30fe958236c745d50
	// That should not be a valid instructions. Right now, we skip translation
	// of such case, but we could use this to detect that we are decoding bad
	// data -> instead of ignore or throw, we should behave as if capstone
	// decoding failed (might be implemented as throw catched by captone2llvm).
	//
	// However, that address should not even be translated.
	//
	if (op0->getType() != op1->getType())
	{
		return;
	}

	storeOp(xi->operands[0], op1, irb);
	storeOp(xi->operands[1], op0, irb);
}

/**
 * X86_INS_XLATB
 */
void Capstone2LlvmIrTranslatorX86_impl::translateXlatb(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* al = loadRegister(X86_REG_AL, irb);
	llvm::Value* ebx = nullptr;
	switch (xi->addr_size)
	{
		case 2: ebx = loadRegister(X86_REG_BX, irb); break;  // Maybe DS:BX?
		case 4: ebx = loadRegister(X86_REG_EBX, irb); break; // Maybe DS:EBX?
		case 8: ebx = loadRegister(X86_REG_RBX, irb); break; // Only RBX.
		default: throw GenericError("Unhandled address size in XLATB.");
	}

	al = irb.CreateZExt(al, ebx->getType());
	auto* add = irb.CreateAdd(ebx, al);
	auto* l = loadIntPtr(irb, add, irb.getInt8Ty());

	storeRegister(X86_REG_AL, l, irb);
}

/**
 * X86_INS_XOR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateXor(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (tryTranslateLockedRmw(i, xi, irb, llvm::AtomicRMWInst::Xor))
	{
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	auto* xorOp = irb.CreateXor(op0, op1);

	storeRegistersPlusSflags(irb, xorOp, {
			{X86_REG_AF, irb.getInt1(false)},   // undef
			{X86_REG_CF, irb.getInt1(false)},   // cleared
			{X86_REG_OF, irb.getInt1(false)}}); // cleared

	storeOp(xi->operands[0], xorOp, irb);
}

/**
 * X86_INS_LODSB, X86_INS_LODSW, X86_INS_LODSD, X86_INS_LODSQ
 * + REP prefix variants
 */
void Capstone2LlvmIrTranslatorX86_impl::translateLoadString(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	EXPECT_IS_EXPR(i, xi, irb, (xi->operands[1].type == X86_OP_MEM));

	// REP prefix.
	//
	bool isRepPrefix = xi->prefix[0] == X86_PREFIX_REP;
	llvm::BranchInst* branch = nullptr;
	llvm::Value* cntr = nullptr;
	std::unique_ptr<llvm::IRBuilder<>> beforeStorage;
	std::unique_ptr<llvm::IRBuilder<>> bodyStorage;
	if (isRepPrefix)
	{
		auto pts = generateWhile(branch, irb);
		beforeStorage = std::make_unique<llvm::IRBuilder<>>(pts.first);
		bodyStorage = std::make_unique<llvm::IRBuilder<>>(pts.second);
		llvm::IRBuilder<>& before = *beforeStorage;
		cntr = loadRegister(getParentRegister(X86_REG_CX), before);
		auto* cond = before.CreateICmpNE(cntr, llvm::ConstantInt::get(cntr->getType(), 0));
		branch->setCondition(cond);
	}
	llvm::IRBuilder<>& body = isRepPrefix ? *bodyStorage : irb;

	// Body.
	//
	op1 = loadOp(xi->operands[1], body);
	storeOp(xi->operands[0], op1, body);

	// We need to modify SI/ESI/RSI, it should be base register in memory op1.
	cs_x86_op& o1 = xi->operands[1];
	uint32_t siN = o1.mem.base;
	auto* si = loadRegister(siN, body);
	auto* df = loadRegister(X86_REG_DF, body);

	llvm::Value* v1 = llvm::ConstantInt::getSigned(si->getType(), -o1.size);
	llvm::Value* v2 = llvm::ConstantInt::getSigned(si->getType(), o1.size);
	auto* val = body.CreateSelect(df, v1, v2);
	auto* add = body.CreateAdd(si, val);

	storeRegister(siN, add, body);

	// REP prefix.
	//
	if (isRepPrefix)
	{
		auto* sub = body.CreateSub(cntr, llvm::ConstantInt::get(cntr->getType(), 1));
		storeRegister(getParentRegister(X86_REG_CX), sub, body);
	}
}

/**
 * X86_INS_STOSB, X86_INS_STOSW, X86_INS_STOSD, X86_INS_STOSQ
 * + REP prefix variants
 */
void Capstone2LlvmIrTranslatorX86_impl::translateStoreString(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	if (xi->prefix[0] == X86_PREFIX_REP)
	{
		auto ediId = getParentRegister(X86_REG_DI);
		auto* edi = loadRegister(ediId, irb);
		auto* ediPtr = irb.CreateIntToPtr(edi, llvm::PointerType::get(irb.getInt8Ty(), 0));
		if (auto* ediI = llvm::dyn_cast<llvm::Instruction>(ediPtr))
		{
			attachPointeeType(ediI, irb.getInt8Ty());
		}

		auto* eax = loadOp(xi->operands[1], irb); // al, ax, eax, rax

		auto ecxId = getParentRegister(X86_REG_CX);
		auto* ecx = loadRegister(ecxId, irb);

		std::string name;
		llvm::Type* ty = nullptr;
		switch (i->id)
		{
			case X86_INS_STOSB:
				name = "__asm_rep_stosb_memset";
				ty = irb.getInt8Ty();
				break;
			case X86_INS_STOSW:
				name = "__asm_rep_stosw_memset";
				ty = irb.getInt16Ty();
				break;
			case X86_INS_STOSD:
				name = "__asm_rep_stosd_memset";
				ty = irb.getInt32Ty();
				break;
			case X86_INS_STOSQ:
				name = "__asm_rep_stosq_memset";
				ty = irb.getInt64Ty();
				break;
			default: throw GenericError("Unhandled insn ID.");
		}

		eax = irb.CreateZExtOrTrunc(eax, ty);

		llvm::Function* fnc = getPseudoAsmFunction(
				i,
				irb.getVoidTy(),
				llvm::ArrayRef<llvm::Type*>{
						ediPtr->getType(),
						ty,
						ecx->getType()},
				name);

		irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{ediPtr, eax, ecx});

		// EDI is +/-(size * ecx)
		cs_x86_op& o0 = xi->operands[0];
		auto* df = loadRegister(X86_REG_DF, irb);
		llvm::Value* minus = llvm::ConstantInt::getSigned(edi->getType(), -o0.size);
		llvm::Value* plus = llvm::ConstantInt::getSigned(edi->getType(), o0.size);
		auto* val = irb.CreateSelect(df, minus, plus);
		val = irb.CreateMul(val, ecx);
		auto* add = irb.CreateAdd(edi, val);
		storeRegister(ediId, add, irb);

		// ECX is zero afterwards.
		storeRegister(ecxId, llvm::ConstantInt::get(ecx->getType(), 0), irb);
	}
	else
	{
		op1 = loadOp(xi->operands[1], irb);
		storeOp(xi->operands[0], op1, irb);

		// We need to modify DI/EDI/RDI, it should be base reg in memory op0.
		cs_x86_op& o0 = xi->operands[0];
		if (o0.type != X86_OP_MEM)
		{
			throw GenericError("unexpected operand type");
		}
		uint32_t diN = o0.mem.base;
		auto* di = loadRegister(diN, irb);
		auto* df = loadRegister(X86_REG_DF, irb);

		llvm::Value* v1 = llvm::ConstantInt::getSigned(di->getType(), -o0.size);
		llvm::Value* v2 = llvm::ConstantInt::getSigned(di->getType(), o0.size);
		auto* val = irb.CreateSelect(df, v1, v2);
		auto* add = irb.CreateAdd(di, val);

		storeRegister(diN, add, irb);
	}
}

/**
 * X86_INS_MOVSB, X86_INS_MOVSW, X86_INS_MOVSD, X86_INS_MOVSQ
 * + REP prefix variants
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMoveString(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	// TODO: 10003351 @ movsd xmm0, qword ptr [edi + 8] in x86-pe-00d062fd23f36fbcdda3ae372f3dd975
	// even ida says:
	// .text:10003351                 movsd   xmm0, qword ptr [edi+8]
	// maybe this?
	// https://x86.puri.sm/html/file_module_x86_id_204.html
	//
	if (xi->operands[0].type != X86_OP_MEM
		|| xi->operands[1].type != X86_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	if (xi->prefix[0] == X86_PREFIX_REP)
	{
		std::string name = std::string("__asm_rep_")
				+ cs_insn_name(_handle, i->id) + "_memcpy";

		auto esiId = getParentRegister(X86_REG_SI);
		auto* esi = loadRegister(esiId, irb);
		auto* esiPtr = irb.CreateIntToPtr(esi, llvm::PointerType::get(irb.getInt8Ty(), 0));
		if (auto* esiI = llvm::dyn_cast<llvm::Instruction>(esiPtr))
		{
			attachPointeeType(esiI, irb.getInt8Ty());
		}

		auto ediId = getParentRegister(X86_REG_DI);
		auto* edi = loadRegister(ediId, irb);
		auto* ediPtr = irb.CreateIntToPtr(edi, llvm::PointerType::get(irb.getInt8Ty(), 0));
		if (auto* ediI = llvm::dyn_cast<llvm::Instruction>(ediPtr))
		{
			attachPointeeType(ediI, irb.getInt8Ty());
		}

		auto ecxId = getParentRegister(X86_REG_CX);
		auto* ecx = loadRegister(ecxId, irb);

		llvm::Function* fnc = getPseudoAsmFunction(
				i,
				irb.getVoidTy(),
				llvm::ArrayRef<llvm::Type*>{
						ediPtr->getType(),
						esiPtr->getType(),
						ecx->getType()},
				name);

		irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{ediPtr, esiPtr, ecx});

		// EDI & ESI is +/-(size * ecx)
		cs_x86_op& o0 = xi->operands[0];
		auto* df = loadRegister(X86_REG_DF, irb);
		llvm::Value* minus = llvm::ConstantInt::getSigned(edi->getType(), -o0.size);
		llvm::Value* plus = llvm::ConstantInt::getSigned(edi->getType(), o0.size);
		auto* val = irb.CreateSelect(df, minus, plus);
		val = irb.CreateMul(val, ecx);
		// Both pointers move by the same delta, but from their OWN bases.
		// This stored EDI's result into ESI as well, so `rep movsb` with
		// ESI = 0x1000, EDI = 0x2000, ECX = 16 left ESI = 0x2010 where the
		// hardware leaves 0x1010 -- the source pointer landed on top of the
		// destination. The non-REP path forty lines down always computed the
		// two separately; only this one shared the sum. `rep movsb` is the
		// inlined memcpy in current glibc, so the wrong ESI is carried into
		// whatever reads the source pointer next.
		storeRegister(ediId, irb.CreateAdd(edi, val), irb);
		storeRegister(esiId, irb.CreateAdd(esi, val), irb);

		// ECX is zero afterwards.
		storeRegister(ecxId, llvm::ConstantInt::get(ecx->getType(), 0), irb);
	}
	else
	{
		op1 = loadOp(xi->operands[1], irb);
		storeOp(xi->operands[0], op1, irb);

		// We need to modify DI/EDI/RDI, it should be base register in memory op0.
		cs_x86_op& o0 = xi->operands[0];
		uint32_t diN = o0.mem.base;
		auto* di = loadRegister(diN, irb);
		// We need to modify SI/ESI/RSI, it should be base register in memory op1.
		cs_x86_op& o1 = xi->operands[1];
		uint32_t siN = o1.mem.base;
		auto* si = loadRegister(siN, irb);

		auto* df = loadRegister(X86_REG_DF, irb);
		llvm::Value* v1 = llvm::ConstantInt::getSigned(di->getType(), -o0.size);
		llvm::Value* v2 = llvm::ConstantInt::getSigned(di->getType(), o0.size);
		auto* val = irb.CreateSelect(df, v1, v2);
		auto* addDi = irb.CreateAdd(di, val);
		auto* addSi = irb.CreateAdd(si, val);

		storeRegister(diN, addDi, irb);
		storeRegister(siN, addSi, irb);
	}
}

/**
 * X86_INS_SCASB, X86_INS_SCASW, X86_INS_SCASD, X86_INS_SCASQ
 * TODO: rep variant is a strchr-type operation, maybe we could convert it to
 * such psuedo call. IDA does not do it (do while is generated) so maybe there
 * is some problem.
 * TODO: this is strlen only if (according to IDA):
 * - X86_INS_SCASB
 * - X86_PREFIX_REPNE
 * - eax == 0
 * => searches for terminating '\0' in string and returns its position = length.
 * other constants in eax || X86_PREFIX_REPE || SCASD || ... => do while cycle
 */
void Capstone2LlvmIrTranslatorX86_impl::translateScanString(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	// REPE/REPNE prefix.
	//
	bool isRepePrefix = xi->prefix[0] == X86_PREFIX_REPE;
	bool isRepnePrefix = xi->prefix[0] == X86_PREFIX_REPNE;
	bool isPrefix = isRepePrefix || isRepnePrefix;
	llvm::BranchInst* branch = nullptr;
	llvm::Value* cntr = nullptr;
	std::unique_ptr<llvm::IRBuilder<>> beforeStorage;
	std::unique_ptr<llvm::IRBuilder<>> bodyStorage;
	if (isPrefix)
	{
		auto pts = generateWhile(branch, irb);
		beforeStorage = std::make_unique<llvm::IRBuilder<>>(pts.first);
		bodyStorage = std::make_unique<llvm::IRBuilder<>>(pts.second);
		llvm::IRBuilder<>& before = *beforeStorage;
		cntr = loadRegister(getParentRegister(X86_REG_CX), before);
		auto* cond = before.CreateICmpNE(cntr, llvm::ConstantInt::get(cntr->getType(), 0));
		branch->setCondition(cond);
	}
	llvm::IRBuilder<>& body = isPrefix ? *bodyStorage : irb;

	// Body.
	//
	std::tie(op0, op1) = loadOpBinary(xi, body, eOpConv::THROW);

	auto* sub = body.CreateSub(op0, op1);

	storeRegistersPlusSflags(body, sub, {
			{X86_REG_AF, generateBorrowSubInt4(op0, op1, body)},
			{X86_REG_CF, generateBorrowSub(op0, op1, body)},
			{X86_REG_OF, generateOverflowSub(sub, op0, op1, body)}});

	// We need to modify DI/EDI/RDI, it should be base register in memory op1.
	cs_x86_op& o1 = xi->operands[1];
	uint32_t diN = o1.mem.base;
	auto* di = loadRegister(diN, body);

	auto* df = loadRegister(X86_REG_DF, body);
	llvm::Value* v1 = llvm::ConstantInt::getSigned(di->getType(), -o1.size);
	llvm::Value* v2 = llvm::ConstantInt::getSigned(di->getType(), o1.size);
	auto* val = body.CreateSelect(df, v1, v2);
	auto* add = body.CreateAdd(di, val);

	storeRegister(diN, add, body);

	// REP/REPNE prefix.
	//
	if (isPrefix)
	{
		auto* sub = body.CreateSub(cntr, llvm::ConstantInt::get(cntr->getType(), 1));
		storeRegister(getParentRegister(X86_REG_CX), sub, body);

		auto* zf = loadRegister(X86_REG_ZF, body);
		if (isRepnePrefix)
		{
			llvm::BranchInst::Create(
					irb.GetInsertBlock(),        // zf == true -> break
					beforeStorage->GetInsertBlock(),
					zf,
					body.GetInsertBlock()->getTerminator());
		}
		else if (isRepePrefix)
		{
			llvm::BranchInst::Create(
					beforeStorage->GetInsertBlock(), // zf == true -> continue
					irb.GetInsertBlock(),
					zf,
					body.GetInsertBlock()->getTerminator());
		}
		body.GetInsertBlock()->getTerminator()->eraseFromParent();
	}
}

/**
 * X86_INS_CMPSB, X86_INS_CMPSW, X86_INS_CMPSD, X86_INS_CMPSQ
 * TODO: rep variant is a strncmp-type operation, maybe we could convert it to
 * such psuedo call. IDA does not do it (do while is generated) so maybe there
 * is some problem.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCompareString(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	// TODO: https://x86.puri.sm/html/file_module_x86_id_39.html
	if (xi->operands[0].type != X86_OP_MEM
		|| xi->operands[1].type != X86_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	// REPE/REPNE prefix.
	//
	bool isRepePrefix = xi->prefix[0] == X86_PREFIX_REPE;
	bool isRepnePrefix = xi->prefix[0] == X86_PREFIX_REPNE;
	bool isPrefix = isRepePrefix || isRepnePrefix;
	llvm::BranchInst* branch = nullptr;
	llvm::Value* cntr = nullptr;
	std::unique_ptr<llvm::IRBuilder<>> beforeStorage;
	std::unique_ptr<llvm::IRBuilder<>> bodyStorage;
	if (isPrefix)
	{
		auto pts = generateWhile(branch, irb);
		beforeStorage = std::make_unique<llvm::IRBuilder<>>(pts.first);
		bodyStorage = std::make_unique<llvm::IRBuilder<>>(pts.second);
		llvm::IRBuilder<>& before = *beforeStorage;
		cntr = loadRegister(getParentRegister(X86_REG_CX), before);
		auto* cond = before.CreateICmpNE(cntr, llvm::ConstantInt::get(cntr->getType(), 0));
		branch->setCondition(cond);
	}
	llvm::IRBuilder<>& body = isPrefix ? *bodyStorage : irb;

	// Body.
	//
	std::tie(op0, op1) = loadOpBinary(xi, body, eOpConv::THROW);

	auto* sub = body.CreateSub(op0, op1);

	storeRegistersPlusSflags(body, sub, {
			{X86_REG_AF, generateBorrowSubInt4(op0, op1, body)},
			{X86_REG_CF, generateBorrowSub(op0, op1, body)},
			{X86_REG_OF, generateOverflowSub(sub, op0, op1, body)}});

	// We need to modify SI/ESI/RSI, it should be base register in memory op0.
	cs_x86_op& o0 = xi->operands[0];
	uint32_t siN = o0.mem.base;
	auto* si = loadRegister(siN, body);
	// We need to modify DI/EDI/RDI, it should be base register in memory op1.
	cs_x86_op& o1 = xi->operands[1];
	uint32_t diN = o1.mem.base;
	auto* di = loadRegister(diN, body);

	auto* df = loadRegister(X86_REG_DF, body);
	llvm::Value* v1 = llvm::ConstantInt::getSigned(si->getType(), -o0.size);
	llvm::Value* v2 = llvm::ConstantInt::getSigned(si->getType(), o0.size);
	auto* val = body.CreateSelect(df, v1, v2);
	auto* addDi = body.CreateAdd(di, val);
	auto* addSi = body.CreateAdd(si, val);

	storeRegister(diN, addDi, body);
	storeRegister(siN, addSi, body);

	// REP/REPNE prefix.
	//
	if (isPrefix)
	{
		auto* sub = body.CreateSub(cntr, llvm::ConstantInt::get(cntr->getType(), 1));
		storeRegister(getParentRegister(X86_REG_CX), sub, body);

		auto* zf = loadRegister(X86_REG_ZF, body);
		if (isRepnePrefix)
		{
			llvm::BranchInst::Create(
					irb.GetInsertBlock(),        // zf == true -> break
					beforeStorage->GetInsertBlock(),
					zf,
					body.GetInsertBlock()->getTerminator());
		}
		else if (isRepePrefix)
		{
			llvm::BranchInst::Create(
					beforeStorage->GetInsertBlock(), // zf == true -> continue
					irb.GetInsertBlock(),
					zf,
					body.GetInsertBlock()->getTerminator());
		}
		body.GetInsertBlock()->getTerminator()->eraseFromParent();
	}
}

/**
 * X86_INS_JCXZ, X86_INS_JECXZ, X86_INS_JRCXZ
 */
void Capstone2LlvmIrTranslatorX86_impl::translateJecxz(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	llvm::Value* ecx = nullptr;
	switch (xi->addr_size)
	{
		case 2: ecx = loadRegister(X86_REG_CX, irb); break;
		case 4: ecx = loadRegister(X86_REG_ECX, irb); break;
		case 8: ecx = loadRegister(X86_REG_RCX, irb); break;
		default: throw GenericError("Unhandled addr size in translateJecxz().");
	}

	auto* eqZ = irb.CreateICmpEQ(ecx, llvm::ConstantInt::get(ecx->getType(), 0));
	op0 = loadOpUnary(xi, irb);
	generateCondBranchFunctionCall(irb, eqZ, op0);
}

/**
 * X86_INS_LOOP, X86_INS_LOOPE (LOOPZ), X86_INS_LOOPNE (LOOPNZ)
 */
void Capstone2LlvmIrTranslatorX86_impl::translateLoop(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	uint32_t ecxN = X86_REG_INVALID;
	switch (xi->addr_size)
	{
		case 2: ecxN = X86_REG_CX; break;
		case 4: ecxN = X86_REG_ECX; break;
		case 8: ecxN = X86_REG_RCX; break;
		default: throw GenericError("Unhandled addr size in translateLoop().");
	}
	llvm::Value* ecx = loadRegister(ecxN, irb);

	auto* sub = irb.CreateSub(ecx, llvm::ConstantInt::get(ecx->getType(), 1));
	storeRegister(ecxN, sub, irb);

	llvm::Value* cnd = nullptr;
	switch (i->id)
	{
		case X86_INS_LOOP:
		{
			cnd = irb.CreateICmpNE(sub, llvm::ConstantInt::get(sub->getType(), 0));
			break;
		}
		case X86_INS_LOOPE:
		{
			auto* neZ = irb.CreateICmpNE(sub, llvm::ConstantInt::get(sub->getType(), 0));
			auto* zf = loadRegister(X86_REG_ZF, irb);
			cnd = irb.CreateAnd(neZ, zf);
			break;
		}
		case X86_INS_LOOPNE:
		{
			auto* eqZ = irb.CreateICmpEQ(sub, llvm::ConstantInt::get(sub->getType(), 0));
			auto* zf = loadRegister(X86_REG_ZF, irb);
			auto* orr = irb.CreateOr(eqZ, zf);
			cnd = irb.CreateXor(orr, irb.getInt1(true));
			break;
		}
		default:
		{
			throw GenericError("Unhandled insn ID in translateLoop().");
		}
	}

	op0 = loadOpUnary(xi, irb);
	generateCondBranchFunctionCall(irb, cnd, op0);
}

/**
 * X86_INS_JAE, X86_INS_JA, X86_INS_JBE, X86_INS_JB, X86_INS_JE, X86_INS_JGE,
 * X86_INS_JG, X86_INS_JLE, X86_INS_JL, X86_INS_JNE, X86_INS_JNO,
 * X86_INS_JNP, X86_INS_JNS, X86_INS_JO, X86_INS_JP, X86_INS_JS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateJCc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case X86_INS_JAE: cond = generateCcAE(irb); break;
		case X86_INS_JA:  cond = generateCcA(irb); break;
		case X86_INS_JBE: cond = generateCcBE(irb); break;
		case X86_INS_JB:  cond = generateCcB(irb); break;
		case X86_INS_JE:  cond = generateCcE(irb); break;
		case X86_INS_JGE: cond = generateCcGE(irb); break;
		case X86_INS_JG:  cond = generateCcG(irb); break;
		case X86_INS_JLE: cond = generateCcLE(irb); break;
		case X86_INS_JL:  cond = generateCcL(irb); break;
		case X86_INS_JNE: cond = generateCcNE(irb); break;
		case X86_INS_JNO: cond = generateCcNO(irb); break;
		case X86_INS_JNP: cond = generateCcNP(irb); break;
		case X86_INS_JNS: cond = generateCcNS(irb); break;
		case X86_INS_JO:  cond = generateCcO(irb); break;
		case X86_INS_JP:  cond = generateCcP(irb); break;
		case X86_INS_JS:  cond = generateCcS(irb); break;
		default: throw GenericError("Unhandled insn ID in translateJCc().");
	}

	op0 = loadOpUnary(xi, irb);
	generateCondBranchFunctionCall(irb, cond, op0);
}

/**
 * X86_INS_SETAE, X86_INS_SETA, X86_INS_SETBE, X86_INS_SETB, X86_INS_SETE,
 * X86_INS_SETGE, X86_INS_SETG, X86_INS_SETLE, X86_INS_SETL, X86_INS_SETNE,
 * X86_INS_SETNO, X86_INS_SETNP, X86_INS_SETNS, X86_INS_SETO, X86_INS_SETP,
 * X86_INS_SETS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSetCc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	// This insn should always set byte.
	EXPECT_IS_EXPR(i, xi, irb, (xi->operands[0].size == 1));

	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case X86_INS_SETAE: cond = generateCcAE(irb); break;
		case X86_INS_SETA:  cond = generateCcA(irb); break;
		case X86_INS_SETBE: cond = generateCcBE(irb); break;
		case X86_INS_SETB:  cond = generateCcB(irb); break;
		case X86_INS_SETE:  cond = generateCcE(irb); break;
		case X86_INS_SETGE: cond = generateCcGE(irb); break;
		case X86_INS_SETG:  cond = generateCcG(irb); break;
		case X86_INS_SETLE: cond = generateCcLE(irb); break;
		case X86_INS_SETL:  cond = generateCcL(irb); break;
		case X86_INS_SETNE: cond = generateCcNE(irb); break;
		case X86_INS_SETNO: cond = generateCcNO(irb); break;
		case X86_INS_SETNP: cond = generateCcNP(irb); break;
		case X86_INS_SETNS: cond = generateCcNS(irb); break;
		case X86_INS_SETO:  cond = generateCcO(irb); break;
		case X86_INS_SETP:  cond = generateCcP(irb); break;
		case X86_INS_SETS:  cond = generateCcS(irb); break;
		default: throw GenericError("Unhandled insn ID in translateSetCc().");
	}

	// This should be done by storeOp(), but we make sure here anyway.
	auto* val = irb.CreateZExtOrTrunc(cond, irb.getInt8Ty());

	storeOp(xi->operands[0], val, irb);
}

/**
 * X86_INS_CMOVAE, X86_INS_CMOVA, X86_INS_CMOVBE, X86_INS_CMOVB, X86_INS_CMOVE,
 * X86_INS_CMOVGE, X86_INS_CMOVG, X86_INS_CMOVLE, X86_INS_CMOVL, X86_INS_CMOVNE,
 * X86_INS_CMOVNO, X86_INS_CMOVNP, X86_INS_CMOVNS, X86_INS_CMOVO, X86_INS_CMOVP,
 * X86_INS_CMOVS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCMovCc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case X86_INS_CMOVAE: cond = generateCcAE(irb); break;
		case X86_INS_CMOVA:  cond = generateCcA(irb); break;
		case X86_INS_CMOVBE: cond = generateCcBE(irb); break;
		case X86_INS_CMOVB:  cond = generateCcB(irb); break;
		case X86_INS_CMOVE:  cond = generateCcE(irb); break;
		case X86_INS_CMOVGE: cond = generateCcGE(irb); break;
		case X86_INS_CMOVG:  cond = generateCcG(irb); break;
		case X86_INS_CMOVLE: cond = generateCcLE(irb); break;
		case X86_INS_CMOVL:  cond = generateCcL(irb); break;
		case X86_INS_CMOVNE: cond = generateCcNE(irb); break;
		case X86_INS_CMOVNO: cond = generateCcNO(irb); break;
		case X86_INS_CMOVNP: cond = generateCcNP(irb); break;
		case X86_INS_CMOVNS: cond = generateCcNS(irb); break;
		case X86_INS_CMOVO:  cond = generateCcO(irb); break;
		case X86_INS_CMOVP:  cond = generateCcP(irb); break;
		case X86_INS_CMOVS:  cond = generateCcS(irb); break;
		default: throw GenericError("Unhandled insn ID in translateSetCc().");
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::THROW);
	auto* val = irb.CreateSelect(cond, op1, op0);
	storeOp(xi->operands[0], val, irb);
}
/**
 * X86_INS_FCMOVB, X86_INS_FCMOVE, X86_INS_FCMOVBE, X86_INS_FCMOVU, X86_INS_FCMOVNB, X86_INS_FCMOVNE,
 * X86_INS_FCMOVNBE, X86_INS_FCMOVNU
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFCMovCc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case X86_INS_FCMOVB: 	cond = generateCcB(irb); break;
		case X86_INS_FCMOVE:  	cond = generateCcE(irb); break;
		case X86_INS_FCMOVBE: 	cond = generateCcBE(irb); break;
		case X86_INS_FCMOVU:  	cond = generateCcP(irb); break;
		case X86_INS_FCMOVNB:  	cond = generateCcAE(irb); break;
		case X86_INS_FCMOVNE: 	cond = generateCcNE(irb); break;
		case X86_INS_FCMOVNBE:  cond = generateCcA(irb); break;
		case X86_INS_FCMOVNU: 	cond = generateCcNP(irb); break;
		default: throw GenericError("Unhandled insn ID in translateSetCc().");
	}

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);
	auto* val = irb.CreateSelect(cond, op1, op0);
	storeX87DataReg(irb, top, val);
}

/**
 * X86_INS_FLD, X86_INS_FILD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFld(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, xi, irb);

	std::tie(op0, top) = loadOpFloatingNullaryOrUnaryTop(i, xi, irb);

	top = x87DecTop(irb, top);
	storeX87DataReg(irb, top, op0);
}

/**
 * X86_INS_FBLD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFbld(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	std::tie(op0, top) = loadOpFloatingNullaryOrUnaryTop(i, xi, irb);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			op0->getType(),
			llvm::ArrayRef<llvm::Type*>{op0->getType()});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0});

	storeX87DataReg(irb, top, c);
	x87DecTop(irb, top); //push
}

/**
 * X86_INS_FBSTP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFbstp(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			op0->getType(),
			llvm::ArrayRef<llvm::Type*>{op0->getType()});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0});

	storeOp(xi->operands[0], c, irb);
	x87IncTop(irb, top); //pop
}

/**
 * X86_INS_FLD1, X86_INS_FLDL2T, X86_INS_FLDL2E, X86_INS_FLDPI, X86_INS_FLDLG2,
 * X86_INS_FLDLN2, X86_INS_FLDZ
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFloadConstant(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87TopDec(irb);

	auto* fp80 = llvm::Type::getX86_FP80Ty(_module->getContext());
	llvm::Value* val = nullptr;
	switch (i->id)
	{
		case X86_INS_FLD1:
		{
			val = llvm::ConstantFP::get(fp80, 1.0);
			break;
		}
		case X86_INS_FLDL2T:
		{
			static double l2t = std::log2(10.0L);
			val = llvm::ConstantFP::get(fp80, l2t);
			break;
		}
		case X86_INS_FLDL2E:
		{
			static double l2e = std::log2(std::exp(1.0L));
			val = llvm::ConstantFP::get(fp80, l2e);
			break;
		}
		case X86_INS_FLDPI:
		{
			static double pi = 3.14159265358979323846;
			val = llvm::ConstantFP::get(fp80, pi);
			break;
		}
		case X86_INS_FLDLG2:
		{
			static double lg2 = std::log10(2.0L);
			val = llvm::ConstantFP::get(fp80, lg2);
			break;
		}
		case X86_INS_FLDLN2:
		{
			static double ln2 = std::log(2.0L);
			val = llvm::ConstantFP::get(fp80, ln2);
			break;
		}
		case X86_INS_FLDZ:
		{
			val = llvm::ConstantFP::get(fp80, 0.0);
			break;
		}
		default:
		{
			throw GenericError("unhandled instruction ID");
		}
	}

	storeX87DataReg(irb, top, val);
	storeRegister(X87_REG_TOP, top, irb);
}

/**
 * X86_INS_FST, X86_INS_FSTP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFst(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	llvm::Value* src = loadX87DataReg(irb, top);

	if (xi->op_count == 1 && xi->operands[0].type == X86_OP_REG)
	{
		auto reg = xi->operands[0].reg;
		if (!isX87DataRegister(reg))
		{
			throw GenericError("unexpected register");
		}
		unsigned regOff = reg - X86_REG_ST0;

		idx = regOff
				? irb.CreateAdd(top, llvm::ConstantInt::get(top->getType(), regOff))
				: top;

		storeX87DataReg(irb, idx, src);
	}
	else
	{
		storeOp(xi->operands[0], src, irb, eOpConv::FPCAST_OR_BITCAST);
	}

	if (i->id == X86_INS_FSTP)
	{
		x87IncTop(irb, top);
	}
}

/**
 * X86_INS_FMUL, X86_INS_FMULP, X86_INS_FIMUL
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFmul(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count <= 2));

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	auto* fmul = irb.CreateFMul(op0, op1);

	if (xi->op_count == 2 || i->id == X86_INS_FMULP)
	{
		storeX87DataReg(irb, idx, fmul);
	}
	else
	{
		storeX87DataReg(irb, top, fmul);
	}

	if (i->id == X86_INS_FMULP)
	{
		x87IncTop(irb, top);
	}
}

/**
 * X86_INS_FADD, X86_INS_FADDP, X86_INS_FIADD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFadd(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count <= 2));

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);
	// Capstone reports FADDP as X86_INS_FADD -- it is the ONE x87 arithmetic
	// instruction whose popping form does not get its own id. fmulp is
	// X86_INS_FMULP, fsubp is X86_INS_FSUBP, fdivp is X86_INS_FDIVP, fstp is
	// X86_INS_FSTP; faddp is X86_INS_FADD. So the P form has to be read off
	// the opcode byte, which is what this already did for the destination --
	// and then the pop was gated on the id instead, which is true for every
	// form.
	//
	// The result, until this was fixed: `fadd m32fp`, `fadd m64fp`,
	// `fadd st(0), st(i)` and `fadd st(i), st(0)` all popped the x87 stack,
	// and nothing after them in the function was reading the register it
	// thought it was. Checked on the hardware rather than in the manual --
	// fnstsw before and after each form -- and only DE pops.
	// The id has to be part of the test as well as the opcode byte: FIADD
	// m16int is ALSO 0xDE (DE /0), and it is a different instruction that
	// does not pop. Capstone does give FIADD its own id, so
	// "id is FADD and the opcode is DE" is exactly FADDP and nothing else.
	bool isFADDP = i->id == X86_INS_FADD && xi->opcode[0] == 0xDE && xi->opcode[1] == 0x00 && xi->opcode[2] == 0x00
				&& xi->opcode[3] == 0x00;

	auto* fadd = irb.CreateFAdd(op0, op1);

	if (xi->op_count == 2 || isFADDP)
	{
		storeX87DataReg(irb, idx, fadd);
	}
	else
	{
		storeX87DataReg(irb, top, fadd);
	}

	if (isFADDP)
	{
		x87IncTop(irb, top);
	}
}

/**
 * X86_INS_FDIV, X86_INS_FDIVP, X86_INS_FIDIV
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFdiv(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count <= 2));

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	auto* fdiv = irb.CreateFDiv(op0, op1);

	if (xi->op_count == 2 || i->id == X86_INS_FDIVP)
	{
		storeX87DataReg(irb, idx, fdiv);
	}
	else
	{
		storeX87DataReg(irb, top, fdiv);
	}

	if (i->id == X86_INS_FDIVP)
	{
		x87IncTop(irb, top);
	}
}

/**
 * X86_INS_FDIVR, X86_INS_FDIVRP, X86_INS_FIDIVR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFdivr(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count <= 2));

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	auto* fdiv = irb.CreateFDiv(op1, op0);

	if (xi->op_count == 2 || i->id == X86_INS_FDIVRP)
	{
		storeX87DataReg(irb, idx, fdiv);
	}
	else
	{
		storeX87DataReg(irb, top, fdiv);
	}

	if (i->id == X86_INS_FDIVRP)
	{
		x87IncTop(irb, top);
	}
}

/**
 * X86_INS_FPREM, X86_INS_FPREM1
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFprem(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count == 0));

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	auto* frem = irb.CreateFRem(op0, op1);

	storeX87DataReg(irb, top, frem);
}

/**
 * X86_INS_FSUB, X86_INS_FSUBP, X86_INS_FISUB
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFsub(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count <= 2));

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	auto* fsub = irb.CreateFSub(op0, op1);

	if (xi->op_count == 2 || i->id == X86_INS_FSUBP)
	{
		storeX87DataReg(irb, idx, fsub);
	}
	else
	{
		storeX87DataReg(irb, top, fsub);
	}

	if (i->id == X86_INS_FSUBP)
	{
		x87IncTop(irb, top);
	}
}

/**
 * X86_INS_FSUBR, X86_INS_FSUBRP, X86_INS_FISUBR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFsubr(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count <= 2));

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	auto* fsub = irb.CreateFSub(op1, op0);

	if (xi->op_count == 2 || i->id == X86_INS_FSUBRP)
	{
		storeX87DataReg(irb, idx, fsub);
	}
	else
	{
		storeX87DataReg(irb, top, fsub);
	}

	if (i->id == X86_INS_FSUBRP)
	{
		x87IncTop(irb, top);
	}
}

/**
 * X86_INS_FABS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFabs(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, op0->getType());
	auto* fabs = irb.CreateCall(f, {op0});

	storeX87DataReg(irb, top, fabs);
}

/**
 * X86_INS_FCHS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFchs(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);
	auto* res = irb.CreateFSub(llvm::ConstantFP::getNegativeZero(op0->getType()), op0);

	storeX87DataReg(irb, top, res);
}

/**
 * X86_INS_FSQRT
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFsqrt(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sqrt, op0->getType());
	auto* fabs = irb.CreateCall(f, {op0});

	storeX87DataReg(irb, top, fabs);
}

/**
 * X86_INS_FSCALE
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFscale(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);
	// FSCALE TRUNCATES ST(1) toward zero; it does not round it. Measured:
	// fscale(8.0, 1.9) = 16 (2^1, not 2^2) and fscale(8.0, -0.9) = 8 (2^0,
	// not 2^-1). Intrinsic::round gets both of those wrong, and the variable
	// it was assigned to was already called `roundDown`.
	auto* truncToZero = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::trunc, op1->getType());
	op1 = irb.CreateCall(truncToZero, {op1});
	auto* exp2 = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::exp2, op1->getType());
	op1 = irb.CreateCall(exp2, {op1});
	op0 = irb.CreateFMul(op0, op1);

	storeX87DataReg(irb, top, op0);
}

/**
 * X86_INS_F2XM1
 */
void Capstone2LlvmIrTranslatorX86_impl::translateF2xm1(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);
	op1 = llvm::ConstantFP::get(op0->getType(), 1);
	// 2^x - 1, not 2^(x-1). The subtraction was applied to the EXPONENT.
	// Measured: f2xm1 on 0.5 gives 0.41421356, which is 2^0.5 - 1; this gave
	// 2^-0.5 = 0.70710678. The two agree only at x = 1, and the one test of
	// this instruction could not tell them apart because it asserted the
	// register as ANY -- the emulator could not evaluate an fp80 intrinsic,
	// so the value was never checked. Fixing that is what exposed this.
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::exp2, op0->getType());
	auto* res = irb.CreateFSub(irb.CreateCall(f, {op0}), op1);

	storeX87DataReg(irb, top, res);
}

/**
 * X86_INS_FYL2X, X86_INS_FYL2X1
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFyl2x(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	if (i->id == X86_INS_FYL2XP1)
	{
		op2 = llvm::ConstantFP::get(op0->getType(), 1);
		op0 = irb.CreateFAdd(op0, op2);
	}

	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::log2, op0->getType());
	auto* log2 = irb.CreateCall(f, {op0});
	auto* fmulLog2 = irb.CreateFMul(op1, log2);

	storeX87DataReg(irb, idx, fmulLog2);
	x87IncTop(irb, top);
}

/**
 * X86_INS_FXCH
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFxch(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count <= 2));

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	storeX87DataReg(irb, top, op1);
	storeX87DataReg(irb, idx, op0);
}

/**
 * X86_INS_FCOS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFcos(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);
	auto* fabs = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, op0->getType());
	auto* absCall = irb.CreateCall(fabs, {op0});
	auto* fc = llvm::ConstantFP::get(absCall->getType(), 9223372036854775808.0); // 1 << 63
	auto* olt = irb.CreateFCmpOLT(absCall, fc);

	auto irbP = generateIfThenElse(olt, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(X87_REG_C2, bodyIf.getFalse(), bodyIf);
	auto* cos = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::cos, op0->getType());
	auto* cosCall = bodyIf.CreateCall(cos, {op0});
	storeX87DataReg(bodyIf, top, cosCall);

	storeRegister(X87_REG_C2, bodyElse.getTrue(), bodyElse);
}

/**
 * X86_INS_FSINCOS
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFsincos(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);
	auto* fabs = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, op0->getType());
	auto* absCall = irb.CreateCall(fabs, {op0});
	auto* fc = llvm::ConstantFP::get(absCall->getType(), 9223372036854775808.0); // 1 << 63
	auto* olt = irb.CreateFCmpOLT(absCall, fc);

	auto irbP = generateIfThenElse(olt, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(X87_REG_C2, bodyIf.getFalse(), bodyIf);
	auto* sin = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sin, op0->getType());
	auto* sinCall = bodyIf.CreateCall(sin, {op0});
	storeX87DataReg(bodyIf, top, sinCall);

	auto* cos = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::cos, op0->getType());
	auto* cosCall = bodyIf.CreateCall(cos, {op0});
	auto* nTop = x87DecTop(bodyIf, top);
	storeX87DataReg(bodyIf, nTop, cosCall);

	storeRegister(X87_REG_C2, bodyElse.getTrue(), bodyElse);
}

/**
 * X86_INS_FSIN
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFsin(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);
	auto* fabs = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, op0->getType());
	auto* absCall = irb.CreateCall(fabs, {op0});
	auto* fc = llvm::ConstantFP::get(absCall->getType(), 9223372036854775808.0); // 1 << 63
	auto* olt = irb.CreateFCmpOLT(absCall, fc);

	auto irbP = generateIfThenElse(olt, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(X87_REG_C2, bodyIf.getFalse(), bodyIf);
	auto* sin = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sin, op0->getType());
	auto* sinCall = bodyIf.CreateCall(sin, {op0});
	storeX87DataReg(bodyIf, top, sinCall);

	storeRegister(X87_REG_C2, bodyElse.getTrue(), bodyElse);
}

/**
 * X86_INS_FPTAN
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFtan(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	op0 = loadX87DataReg(irb, top);
	auto* fc = llvm::ConstantFP::get(op0->getType(), 9223372036854775808.0); // 1 << 63
	auto* olt = irb.CreateFCmpOLT(op0, fc);

	auto irbP = generateIfThenElse(olt, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(X87_REG_C2, bodyIf.getFalse(), bodyIf);
	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			op0->getType(),
			llvm::ArrayRef<llvm::Type*>{op0->getType()});

	auto* tan = bodyIf.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0});
	storeX87DataReg(bodyIf, top, tan);

	top = x87DecTop(bodyIf, top); //push
	auto* fp80 = llvm::Type::getX86_FP80Ty(_module->getContext());
	auto one = llvm::ConstantFP::get(fp80, 1.0);
	storeX87DataReg(bodyIf, top, one);

	storeRegister(X87_REG_C2, bodyElse.getTrue(), bodyElse);
}

/**
 * X86_INS_FPATAN
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFatan(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			op0->getType(),
			llvm::ArrayRef<llvm::Type*>{op1->getType(), op0->getType()});
	auto* atan = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op1, op0});

	storeX87DataReg(irb, idx, atan);

	x87IncTop(irb, top);
}

/**
 * X86_INS_FINCSTP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFincstp(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	// top == 7 ? 0 : top + 1
	// x87IncTop() does not do this logic explicitly (i.e. comparison & select),
	// but because TOP is i3 type, adding 1 to 7 gives 0 (i.e. 000b = 0).
	x87IncTop(irb);
	storeRegister(X87_REG_C1, irb.getFalse(), irb);
}

/**
 * X86_INS_FDECSTP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFdecstp(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	// top == 0 ? 7 : top - 1
	// x87DecTop() does not do this logic explicitly (i.e. comparison & select),
	// but because TOP is i3 type, subtracting 1 from 0 gives -1 (i.e. 111b = 7).
	x87DecTop(irb);
	storeRegister(X87_REG_C1, irb.getFalse(), irb);
}

/**
 * X86_INS_FFREE
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFfree(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	// ignore
}

/**
 * X86_INS_FNSTSW
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFnstsw(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	auto* fpsw = loadRegister(X86_REG_FPSW, irb);
	storeOp(xi->operands[0], fpsw, irb);
}

/**
 * X86_INS_FNCLEX
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFnclex(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			getDefaultType(),
			llvm::ArrayRef<llvm::Type*>{});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{});

	storeRegister(X86_REG_FPSW, c, irb);
}

/**
 * X86_INS_FRSTOR
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFrstor(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	uint8_t memorySizeOfFpuStateInBytesFor16BitArch = 94;
	uint8_t memorySizeOfFpuStateInBytesFor32or64BitArch = 108;
	if (_origBasicMode == CS_MODE_16)
		xi->operands[0].size = memorySizeOfFpuStateInBytesFor16BitArch;
	else // CS_MODE_32, CS_MODE_64
		xi->operands[0].size = memorySizeOfFpuStateInBytesFor32or64BitArch;

	op0 = loadOp(xi->operands[0], irb);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			irb.getVoidTy(),
			llvm::ArrayRef<llvm::Type*>{op0->getType()});

	irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0});
}

/**
 * X86_INS_FNSAVE
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFnsave(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	uint8_t memorySizeOfFpuStateInBytesFor16BitArch = 94;
	uint8_t memorySizeOfFpuStateInBytesFor32or64BitArch = 108;
	if (_origBasicMode == CS_MODE_16)
		xi->operands[0].size = memorySizeOfFpuStateInBytesFor16BitArch;
	else // CS_MODE_32, CS_MODE_64
		xi->operands[0].size = memorySizeOfFpuStateInBytesFor32or64BitArch;

	auto type = getIntegerTypeFromByteSize(_module, xi->operands[0].size);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			type,
			llvm::ArrayRef<llvm::Type*>{});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{});
	storeOp(xi->operands[0], c, irb);
}

/**
 * X86_INS_FNSTENV
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFnstenv(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	uint8_t memorySizeOfFpuEnvironmentInBytesFor16BitArch = 14;
	uint8_t memorySizeOfFpuEnvironmentInBytesFor32or64BitArch = 28;
	if (_origBasicMode == CS_MODE_16)
		xi->operands[0].size = memorySizeOfFpuEnvironmentInBytesFor16BitArch;
	else // CS_MODE_32, CS_MODE_64Environment
		xi->operands[0].size = memorySizeOfFpuEnvironmentInBytesFor32or64BitArch;

	auto type = getIntegerTypeFromByteSize(_module, xi->operands[0].size);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			type,
			llvm::ArrayRef<llvm::Type*>{});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{});
	storeOp(xi->operands[0], c, irb);
}

/**
 * X86_INS_FXSAVE, X86_INS_FXSAVE64
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFxsave(cs_insn *i, cs_x86 *xi, llvm::IRBuilder<> &irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	// sizeof(state of the x87 FPU, MMX technology, XMM, and MXCSR registers) = 512
	auto retType = getIntegerTypeFromByteSize(_module, 512);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			retType,
			llvm::ArrayRef<llvm::Type*>{});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{});

	auto* baseR = loadRegister(xi->operands[0].mem.base, irb);
	auto* t = baseR ? baseR->getType() : getDefaultType();
	llvm::Value* disp = xi->operands[0].mem.disp
						? llvm::ConstantInt::getSigned(t, xi->operands[0].mem.disp)
						: nullptr;

	auto* idxR = loadRegister(xi->operands[0].mem.index, irb);
	if (idxR)
	{
		auto* scale = llvm::ConstantInt::get(idxR->getType(), xi->operands[0].mem.scale);
		idxR = irb.CreateMul(idxR, scale);
	}

	llvm::Value* addr = nullptr;
	if (baseR && disp == nullptr)
	{
		addr = baseR; //fxsave [EAX]
	}
	else if (disp && baseR == nullptr)
	{
		addr = disp; //fxsave [0x1234]
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

	auto val = generateTypeConversion(irb, c, retType, eOpConv::ZEXT_TRUNC_OR_BITCAST);

	storeIntPtr(irb, val, addr, retType, getAddrSpace(xi->operands[0].mem.segment));
}

/**
 * X86_INS_FXRSTOR, X86_INS_FXRSTOR64
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFxstor(cs_insn *i, cs_x86 *xi, llvm::IRBuilder<> &irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	// sizeof(state of the x87 FPU, MMX technology, XMM, and MXCSR registers) = 512
	auto retType = getIntegerTypeFromByteSize(_module, 512);

	auto* baseR = loadRegister(xi->operands[0].mem.base, irb);
	auto* t = baseR ? baseR->getType() : getDefaultType();
	llvm::Value* disp = xi->operands[0].mem.disp
						? llvm::ConstantInt::getSigned(t, xi->operands[0].mem.disp)
						: nullptr;

	auto* idxR = loadRegister(xi->operands[0].mem.index, irb);
	if (idxR)
	{
		auto* scale = llvm::ConstantInt::get(idxR->getType(), xi->operands[0].mem.scale);
		idxR = irb.CreateMul(idxR, scale);
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

	op0 = loadIntPtr(irb, addr, retType, getAddrSpace(xi->operands[0].mem.segment));

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			irb.getVoidTy(),
			llvm::ArrayRef<llvm::Type*>{op0->getType()});

	irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0});
}

/**
 * X86_INS_FUCOM, X86_INS_FUCOMP, X86_INS_FUCOMPP
 * X86_INS_FCOM, X86_INS_FCOMP, X86_INS_FCOMPP
 * X86_INS_FUCOMI, X86_INS_FUCOMIP
 * X86_INS_FCOMI, X86_INS_FCOMIP
 * X86_INS_FTST
 * X86_INS_FICOM, X86_INS_FICOMP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFucomPop(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, (xi->op_count <= 2));

	bool doublePop = i->id == X86_INS_FUCOMPP || i->id == X86_INS_FCOMPP;
	bool pop = i->id == X86_INS_FUCOMP || i->id == X86_INS_FCOMP
			|| i->id == X86_INS_FUCOMPI || i->id == X86_INS_FCOMPI
			|| i->id == X86_INS_FICOMP || doublePop;

	uint32_t r1 = X87_REG_C0;
	uint32_t r2 = X87_REG_C2;
	uint32_t r3 = X87_REG_C3;
	if (i->id == X86_INS_FUCOMI
			|| i->id == X86_INS_FUCOMPI
			|| i->id == X86_INS_FCOMI
			|| i->id == X86_INS_FCOMPI)
	{
		r1 = X86_REG_CF;
		r2 = X86_REG_PF;
		r3 = X86_REG_ZF;
	}

	if (i->id == X86_INS_FTST)
	{
		std::tie(op0, top) = loadOpFloatingNullaryOrUnaryTop(i, xi, irb);
		op1 = llvm::ConstantFP::get(op0->getType(), 0.0);
	}
	else
	{
		std::tie(op0, op1, top, idx) = loadOpFloatingBinaryTop(i, xi, irb);
	}

	auto* fcmpOgt = irb.CreateFCmpOGT(op0, op1);
	auto irbP = generateIfThenElse(fcmpOgt, irb);
	llvm::IRBuilder<> bodyIf(irbP.first);
	llvm::IRBuilder<> bodyElse(irbP.second);

	storeRegister(r1, bodyIf.getFalse(), bodyIf);
	storeRegister(r2, bodyIf.getFalse(), bodyIf);
	storeRegister(r3, bodyIf.getFalse(), bodyIf);

	auto* fcmpOlt = bodyElse.CreateFCmpOLT(op0, op1);
	auto irbP1 = generateIfThenElse(fcmpOlt, bodyElse);
	llvm::IRBuilder<> bodyIf1(irbP1.first);
	llvm::IRBuilder<> bodyElse1(irbP1.second);

	storeRegister(r1, bodyIf1.getTrue(), bodyIf1);
	storeRegister(r2, bodyIf1.getFalse(), bodyIf1);
	storeRegister(r3, bodyIf1.getFalse(), bodyIf1);

	auto* fcmpOeq = bodyElse1.CreateFCmpOEQ(op0, op1);
	storeRegister(r3, bodyElse1.getTrue(), bodyElse1);
	auto irbP2 = generateIfThenElse(fcmpOeq, bodyElse1);
	llvm::IRBuilder<> bodyIf2(irbP2.first);
	llvm::IRBuilder<> bodyElse2(irbP2.second);

	storeRegister(r1, bodyIf2.getFalse(), bodyIf2);
	storeRegister(r2, bodyIf2.getFalse(), bodyIf2);

	storeRegister(r1, bodyElse2.getTrue(), bodyElse2);
	storeRegister(r2, bodyElse2.getTrue(), bodyElse2);

	if (pop)
	{
		auto* top1 = x87IncTop(irb, top);
		if (doublePop)
		{
			x87IncTop(irb, top1);
		}
	}
}

/**
 * X86_INS_FXAM
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFxam(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);
	std::tie(op0, top) = loadOpFloatingNullaryOrUnaryTop(i, xi, irb);

	llvm::Function* fnc = getPseudoAsmFunction(
		i,
		getRegisterType(X86_REG_FPSW),
		llvm::ArrayRef<llvm::Type*>{op0->getType()}
	);

	auto* x86RegFpsw = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0});
	storeRegister(X86_REG_FPSW, x86RegFpsw, irb);
}

/**
 * X86_INS_FXTRACT
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFxtract(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);
	std::tie(op0, top) = loadOpFloatingNullaryOrUnaryTop(i, xi, irb);

	// call of pseudo function witch parse mantissa and exponent from st(0) because llvm can not
	// simply represent this operation by native
	llvm::Function* pseudoGetSignificand = getPseudoAsmFunction(
			i,
			op0->getType(),
			llvm::ArrayRef<llvm::Type*>{op0->getType()},
			"__pseudo_get_significand"
	);
	auto* mantissa = irb.CreateCall(pseudoGetSignificand, llvm::ArrayRef<llvm::Value*>{op0});

	llvm::Function* pseudoGetExponent = getPseudoAsmFunction(
			i,
			op0->getType(),
			llvm::ArrayRef<llvm::Type*>{op0->getType()},
			"__pseudo_get_exponent"
	);
	auto* exponent = irb.CreateCall(pseudoGetExponent, llvm::ArrayRef<llvm::Value*>{op0});

	storeX87DataReg(irb, top, exponent);
	top = x87DecTop(irb, top);
	storeX87DataReg(irb, top, mantissa);
}

/**
 * X86_INS_FIST, X86_INS_FISTP, X86_INS_FISTPP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFist(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);

	auto* topNum = loadX87Top(irb);
	// Value*, not the CallInst* loadX87DataReg hands back: the roundeven below
	// reassigns this, and IRBuilder's intrinsic creators return Value*.
	llvm::Value* top = loadX87DataReg(irb, topNum);
	auto* t = getIntegerTypeFromByteSize(_module, xi->operands[0].size);

	// FISTTP truncates toward zero -- that is the whole reason SSE3 added it.
	// FIST and FISTP round with the FPCW rounding-control field, whose reset
	// value is round-to-nearest-even, and a bare CreateFPToSI gave all three
	// FISTTP's operation. Measured: `fistpl` on 2.7 stores 3, on 2.5 stores 2
	// and on 3.5 stores 4 -- ties to even, so roundeven and not round.
	// The SSE side of the same conversion already made this call, with the
	// same reasoning, in translateCvtSs2Si.
	//
	// RetDec models no FPCW rounding-control field, so the reset value is the
	// only one that can be modelled; code that sets RC to truncate first (the
	// fnstcw/or 0x0c00/fldcw idiom) is translated as if it had not. That is a
	// stated approximation rather than a silent one.
	if (i->id != X86_INS_FISTTP)
	{
		top = irb.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, top);
	}
	// An input that does not fit stores the INTEGER INDEFINITE value, not
	// poison: measured, `fistpl` gives 0x80000000 for +inf, -inf, NaN, 1e24
	// and 2147483647.5 alike, and `fistps` gives 0x8000. fptosi calls every
	// one of those poison.
	storeOp(xi->operands[0], generateFpToSiDefined(top, t, irb), irb);

	if (i->id == X86_INS_FISTP || i->id == X86_INS_FISTTP) // pop
	{
		x87IncTop(irb, topNum);
	}
}

/**
 * X86_INS_FRNDINT
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFrndint(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* top = loadX87Top(irb);
	llvm::Value* src = loadX87DataReg(irb, top);
	// roundeven, not round: Intrinsic::round is ties-AWAY-from-zero, and
	// FRNDINT follows the FPCW rounding control, whose reset value is
	// nearest-EVEN. Measured on this machine: 0.5 -> 0, 1.5 -> 2, 2.5 -> 2,
	// 3.5 -> 4. Intrinsic::round answers 1 and 3 for the two that matter.
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::roundeven, src->getType());
	auto* val = irb.CreateCall(f, {src});
	storeX87DataReg(irb, top, val);
}

/**
 * X86_INS_CPUID
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCpuid(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	llvm::Type* i32 = irb.getInt32Ty();
	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			llvm::StructType::create(llvm::ArrayRef<llvm::Type*>{
					i32, i32, i32, i32}),
			llvm::ArrayRef<llvm::Type*>{i32});

	auto* eax = loadRegister(X86_REG_EAX, irb);
	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{eax});
	storeRegister(X86_REG_EAX, irb.CreateExtractValue(c, {0}), irb);
	storeRegister(X86_REG_EBX, irb.CreateExtractValue(c, {1}), irb);
	storeRegister(X86_REG_ECX, irb.CreateExtractValue(c, {2}), irb);
	storeRegister(X86_REG_EDX, irb.CreateExtractValue(c, {3}), irb);
}

/**
 * X86_INS_OUTSB, X86_INS_OUTSD, X86_INS_OUTSW
 */
void Capstone2LlvmIrTranslatorX86_impl::translateOuts(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::string name;
	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case X86_INS_OUTSB: name = "__asm_outsb"; ty = irb.getInt8Ty(); break;
		case X86_INS_OUTSW: name = "__asm_outsw"; ty = irb.getInt16Ty(); break;
		case X86_INS_OUTSD: name = "__asm_outsd"; ty = irb.getInt32Ty(); break;
		default: throw GenericError("Unhandled insn ID.");
	}
	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			irb.getVoidTy(),
			llvm::ArrayRef<llvm::Type*>{irb.getInt16Ty(), ty},
			name);

	// REP prefix.
	//
	bool isRepPrefix = xi->prefix[0] == X86_PREFIX_REP;
	llvm::BranchInst* branch = nullptr;
	llvm::Value* cntr = nullptr;
	std::unique_ptr<llvm::IRBuilder<>> beforeStorage;
	std::unique_ptr<llvm::IRBuilder<>> bodyStorage;
	if (isRepPrefix)
	{
		auto pts = generateWhile(branch, irb);
		beforeStorage = std::make_unique<llvm::IRBuilder<>>(pts.first);
		bodyStorage = std::make_unique<llvm::IRBuilder<>>(pts.second);
		llvm::IRBuilder<>& before = *beforeStorage;
		cntr = loadRegister(getParentRegister(X86_REG_CX), before);
		auto* cond = before.CreateICmpNE(cntr, llvm::ConstantInt::get(cntr->getType(), 0));
		branch->setCondition(cond);
	}
	llvm::IRBuilder<>& body = isRepPrefix ? *bodyStorage : irb;

	// Body.
	//
	std::tie(op0, op1) = loadOpBinary(xi, body, eOpConv::NOTHING);
	auto* dx = body.CreateZExtOrTrunc(op0, body.getInt16Ty());
	auto* val = body.CreateZExtOrTrunc(op1, ty);
	body.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{dx, val});

	// REP prefix.
	//
	if (isRepPrefix)
	{
		auto* sub = body.CreateSub(cntr, llvm::ConstantInt::get(cntr->getType(), 1));
		storeRegister(getParentRegister(X86_REG_CX), sub, body);
	}
}

/**
 * X86_INS_INSB, X86_INS_INSW, X86_INS_INSD
 */
void Capstone2LlvmIrTranslatorX86_impl::translateIns(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	std::string name;
	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case X86_INS_INSB: name = "__asm_insb"; ty = irb.getInt8Ty(); break;
		case X86_INS_INSW: name = "__asm_insw"; ty = irb.getInt16Ty(); break;
		case X86_INS_INSD: name = "__asm_insd"; ty = irb.getInt32Ty(); break;
		default: throw GenericError("Unhandled insn ID.");
	}
	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			ty,
			llvm::ArrayRef<llvm::Type*>{irb.getInt16Ty()},
			name);

	// REP prefix.
	//
	bool isRepPrefix = xi->prefix[0] == X86_PREFIX_REP;
	llvm::BranchInst* branch = nullptr;
	llvm::Value* cntr = nullptr;
	std::unique_ptr<llvm::IRBuilder<>> beforeStorage;
	std::unique_ptr<llvm::IRBuilder<>> bodyStorage;
	if (isRepPrefix)
	{
		auto pts = generateWhile(branch, irb);
		beforeStorage = std::make_unique<llvm::IRBuilder<>>(pts.first);
		bodyStorage = std::make_unique<llvm::IRBuilder<>>(pts.second);
		llvm::IRBuilder<>& before = *beforeStorage;
		cntr = loadRegister(getParentRegister(X86_REG_CX), before);
		auto* cond = before.CreateICmpNE(cntr, llvm::ConstantInt::get(cntr->getType(), 0));
		branch->setCondition(cond);
	}
	llvm::IRBuilder<>& body = isRepPrefix ? *bodyStorage : irb;

	// Body.
	//
	auto* dx = loadRegister(X86_REG_DX, body);
	auto* c = body.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{dx});
	storeOp(xi->operands[0], c, body);

	// REP prefix.
	//
	if (isRepPrefix)
	{
		auto* sub = body.CreateSub(cntr, llvm::ConstantInt::get(cntr->getType(), 1));
		storeRegister(getParentRegister(X86_REG_CX), sub, body);
	}
}

/**
 * X86_INS_RDTSC
 */
void Capstone2LlvmIrTranslatorX86_impl::translateRdtsc(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			llvm::StructType::create(llvm::ArrayRef<llvm::Type*>{
					irb.getInt32Ty(),
					irb.getInt32Ty()}),
			llvm::ArrayRef<llvm::Type*>{});

	auto* c = irb.CreateCall(fnc);
	storeRegister(X86_REG_EDX, irb.CreateExtractValue(c, {0}), irb);
	storeRegister(X86_REG_EAX, irb.CreateExtractValue(c, {1}), irb);
}

/**
 * X86_INS_RDTSCP
 */
void Capstone2LlvmIrTranslatorX86_impl::translateRdtscp(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY(i, xi, irb);

	auto* i32 = irb.getInt32Ty();
	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			llvm::StructType::create(llvm::ArrayRef<llvm::Type*>{i32, i32, i32}),
			llvm::ArrayRef<llvm::Type*>{});

	auto* c = irb.CreateCall(fnc);
	storeRegister(X86_REG_EDX, irb.CreateExtractValue(c, {0}), irb);
	storeRegister(X86_REG_EAX, irb.CreateExtractValue(c, {1}), irb);
	storeRegister(X86_REG_ECX, irb.CreateExtractValue(c, {2}), irb);
}

} // namespace capstone2llvmir
} // namespace retdec
