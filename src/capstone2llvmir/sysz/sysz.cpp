/**
 * @file src/capstone2llvmir/sysz/sysz.cpp
 * @brief SystemZ implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>

#include "capstone2llvmir/sysz/sysz_impl.h"

namespace retdec {
namespace capstone2llvmir {

Capstone2LlvmIrTranslatorSysz_impl::Capstone2LlvmIrTranslatorSysz_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(
				CS_ARCH_SYSZ,
				basic,
				// basic is already CS_MODE_BIG_ENDIAN; adding it again
				// wraps the 1U<<31 flag to 0 (little-endian). Extra 0
				// keeps z/Architecture + all features (Capstone default).
				(extra == CS_MODE_BIG_ENDIAN)
						? CS_MODE_LITTLE_ENDIAN
						: extra,
				m)
{
	initialize();
}

//
//==============================================================================
// Mode query & modification methods - from Capstone2LlvmIrTranslator.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorSysz_impl::isAllowedBasicMode(cs_mode m)
{
	// Capstone 6 has no 31-bit / ESA-390 CS_MODE. Instruction bytes are
	// big-endian; CS_MODE_LITTLE_ENDIAN (0) is only the additive identity
	// for extra. Processor-generation bits live in extra.
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN;
}

bool Capstone2LlvmIrTranslatorSysz_impl::isAllowedExtraMode(cs_mode m)
{
	unsigned u = static_cast<unsigned>(m);
	unsigned arch = CS_MODE_SYSTEMZ_ARCH8 | CS_MODE_SYSTEMZ_ARCH9
			| CS_MODE_SYSTEMZ_ARCH10 | CS_MODE_SYSTEMZ_ARCH11
			| CS_MODE_SYSTEMZ_ARCH12 | CS_MODE_SYSTEMZ_ARCH13
			| CS_MODE_SYSTEMZ_ARCH14 | CS_MODE_SYSTEMZ_Z10
			| CS_MODE_SYSTEMZ_Z196 | CS_MODE_SYSTEMZ_ZEC12
			| CS_MODE_SYSTEMZ_Z13 | CS_MODE_SYSTEMZ_Z14
			| CS_MODE_SYSTEMZ_Z15 | CS_MODE_SYSTEMZ_Z16
			| CS_MODE_SYSTEMZ_GENERIC | CS_MODE_BIG_ENDIAN;
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN
			|| (u & ~arch) == 0;
}

uint32_t Capstone2LlvmIrTranslatorSysz_impl::getArchByteSize()
{
	return 8;
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorSysz_impl::generateEnvironmentArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorSysz_impl::generateDataLayout()
{
	_module->setDataLayout("E-p:64:64:64-i8:8:16-i16:16-i32:32-i64:64-n32:64");
}

void Capstone2LlvmIrTranslatorSysz_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		createRegister(p.first, _regLt);
	}

	// Capstone 6 reports 32-bit RR ops as R*L / R*H. Production is 64-bit
	// z/Architecture: those are overlays of R*D, not separate GPRs.
	auto* i64 = llvm::IntegerType::getInt64Ty(_module->getContext());
	for (uint32_t i = 0; i < 16; ++i)
	{
		auto* g = getRegister(SYSZ_REG_R0D + i);
		_capstone2LlvmRegs[SYSZ_REG_R0L + i] = g;
		_capstone2LlvmRegs[SYSZ_REG_R0H + i] = g;
		_reg2type[SYSZ_REG_R0L + i] = i64;
		_reg2type[SYSZ_REG_R0H + i] = i64;
	}
}

uint32_t Capstone2LlvmIrTranslatorSysz_impl::getCarryRegister()
{
	return SYSZ_REG_INVALID;
}

void Capstone2LlvmIrTranslatorSysz_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_sysz* si = &d->sysz;

	auto fIt = _i2fm.find(i->id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;
		(this->*f)(i, si, irb);
	}
	else
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, si, irb);
	}
}

//
//==============================================================================
// SystemZ-specific methods.
//==============================================================================
//

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == SYSZ_REG_INVALID)
	{
		return nullptr;
	}

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);
	return createLoad(irb, llvmReg);
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::generateMemAddress(
		cs_sysz_op& op,
		llvm::IRBuilder<>& irb)
{
	auto* t = getDefaultType();
	llvm::Value* addr = llvm::ConstantInt::getSigned(t, op.mem.disp);

	auto* baseR = loadAddrReg(op.mem.base, irb);
	if (baseR != nullptr)
	{
		addr = irb.CreateAdd(baseR, irb.CreateSExtOrTrunc(addr, baseR->getType()));
	}

	auto* idxR = loadAddrReg(op.mem.index, irb);
	if (idxR != nullptr)
	{
		addr = irb.CreateAdd(addr, irb.CreateSExtOrTrunc(idxR, addr->getType()));
	}

	return addr;
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadAddrReg(
		uint32_t r,
		llvm::IRBuilder<>& irb)
{
	// z/Architecture: GPR 0 as base/index contributes 0, not the register.
	if (r == SYSZ_REG_INVALID || r == SYSZ_REG_R0D
			|| r == SYSZ_REG_R0L || r == SYSZ_REG_R0H)
	{
		return nullptr;
	}
	return loadRegister(r, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadOp(
		cs_sysz_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case SYSZ_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			if (r == nullptr)
			{
				return llvm::UndefValue::get(ty ? ty : getDefaultType());
			}
			if (ty && r->getType() != ty)
			{
				eOpConv c = ty->isFloatingPointTy()
						? eOpConv::FPCAST_OR_BITCAST
						: eOpConv::SEXT_TRUNC_OR_BITCAST;
				if (!r->getType()->isIntegerTy() && !ty->isFloatingPointTy())
				{
					c = eOpConv::ZEXT_TRUNC_OR_BITCAST;
				}
				r = generateTypeConversion(irb, r, ty, c);
			}
			return r;
		}
		case SYSZ_OP_IMM:
		{
			auto* t = getDefaultType();
			return llvm::ConstantInt::get(t, llvm::APInt(t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm), false, /*implicitTrunc=*/true));
		}
		case SYSZ_OP_MEM:
		{
			auto* addr = generateMemAddress(op, irb);
			if (lea)
			{
				return addr;
			}
			auto* lty = ty ? ty : getDefaultType();
			return loadIntPtr(irb, addr, lty);
		}
		case SYSZ_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::StoreInst* Capstone2LlvmIrTranslatorSysz_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == SYSZ_REG_INVALID)
	{
		return nullptr;
	}

	auto* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("storeRegister() unhandled reg.");
	}
	if (ct == eOpConv::SEXT_TRUNC_OR_BITCAST && llvmReg->getValueType()->isFloatingPointTy())
	{
		ct = eOpConv::FPCAST_OR_BITCAST;
	}
	val = generateTypeConversion(irb, val, llvmReg->getValueType(), ct);

	auto* s = irb.CreateStore(val, llvmReg);
	attachPointeeType(s, llvmReg->getValueType());
	return s;
}

llvm::Instruction* Capstone2LlvmIrTranslatorSysz_impl::storeOp(
		cs_sysz_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case SYSZ_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case SYSZ_OP_MEM:
		{
			auto* addr = generateMemAddress(op, irb);
			if (ct == eOpConv::FPCAST_OR_BITCAST && val->getType()->isFloatingPointTy())
			{
				return storeIntPtr(irb, val, addr, val->getType());
			}
			val = generateTypeConversion(irb, val, val->getType(), ct);
			return storeIntPtr(irb, val, addr, val->getType());
		}
		case SYSZ_OP_IMM:
		case SYSZ_OP_INVALID:
		default:
		{
			throw GenericError("storeOp() unhandled operand type.");
		}
	}
}

bool Capstone2LlvmIrTranslatorSysz_impl::isOperandRegister(cs_sysz_op& op)
{
	return op.type == SYSZ_OP_REG;
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::extractLow32(
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	return irb.CreateTrunc(val, irb.getInt32Ty());
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::depositLow32(
		uint32_t r,
		llvm::Value* lo32,
		llvm::IRBuilder<>& irb)
{
	auto* dst = loadRegister(r, irb);
	if (dst == nullptr || dst->getType()->getIntegerBitWidth() <= 32)
	{
		storeRegister(r, lo32, irb);
		return lo32;
	}
	auto* hiMask = llvm::ConstantInt::get(dst->getType(), 0xFFFFFFFF00000000ull);
	auto* hi = irb.CreateAnd(dst, hiMask);
	auto* lo = irb.CreateZExt(lo32, dst->getType());
	auto* merged = irb.CreateOr(hi, lo);
	storeRegister(r, merged, irb);
	return merged;
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcSigned(
		llvm::Value* result,
		llvm::Value* overflow,
		llvm::IRBuilder<>& irb)
{
	auto* zero = llvm::ConstantInt::get(result->getType(), 0);
	auto* isZero = irb.CreateICmpEQ(result, zero);
	auto* isNeg = irb.CreateICmpSLT(result, zero);
	auto* ccLt = llvm::ConstantInt::get(irb.getInt8Ty(), 1);
	auto* ccGt = llvm::ConstantInt::get(irb.getInt8Ty(), 2);
	auto* ccEq = llvm::ConstantInt::get(irb.getInt8Ty(), 0);
	auto* ccOv = llvm::ConstantInt::get(irb.getInt8Ty(), 3);
	auto* signedCc = irb.CreateSelect(isZero, ccEq, irb.CreateSelect(isNeg, ccLt, ccGt));
	auto* cc = irb.CreateSelect(overflow, ccOv, signedCc);
	storeRegister(SYSZ_REG_CC, cc, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcLogical(
		llvm::Value* result,
		llvm::IRBuilder<>& irb)
{
	auto* zero = llvm::ConstantInt::get(result->getType(), 0);
	auto* isZero = irb.CreateICmpEQ(result, zero);
	auto* cc = irb.CreateSelect(
			isZero,
			llvm::ConstantInt::get(irb.getInt8Ty(), 0),
			llvm::ConstantInt::get(irb.getInt8Ty(), 1));
	storeRegister(SYSZ_REG_CC, cc, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::generateCondition(
		systemz_cc cc,
		llvm::IRBuilder<>& irb)
{
	auto* ccv = loadRegister(SYSZ_REG_CC, irb);
	ccv = irb.CreateZExtOrTrunc(ccv, irb.getInt32Ty());
	auto eq = [&](unsigned v) {
		return irb.CreateICmpEQ(ccv, irb.getInt32(v));
	};
	auto ne = [&](unsigned v) {
		return irb.CreateICmpNE(ccv, irb.getInt32(v));
	};

	switch (cc)
	{
		case SYSTEMZ_CC_INVALID:
			return irb.getTrue();
		case SYSTEMZ_CC_O:
			return eq(3);
		case SYSTEMZ_CC_H:
			return eq(2);
		case SYSTEMZ_CC_NLE:
			return irb.CreateOr(eq(2), eq(3));
		case SYSTEMZ_CC_L:
			return eq(1);
		case SYSTEMZ_CC_NHE:
			return irb.CreateOr(eq(1), eq(3));
		case SYSTEMZ_CC_LH:
			return irb.CreateOr(eq(1), eq(2));
		case SYSTEMZ_CC_NE:
			return ne(0);
		case SYSTEMZ_CC_E:
			return eq(0);
		case SYSTEMZ_CC_NLH:
			return irb.CreateOr(eq(0), eq(3));
		case SYSTEMZ_CC_HE:
			return irb.CreateOr(eq(0), eq(2));
		case SYSTEMZ_CC_NL:
			return ne(1);
		case SYSTEMZ_CC_LE:
			return irb.CreateOr(eq(0), eq(1));
		case SYSTEMZ_CC_NH:
			return ne(2);
		case SYSTEMZ_CC_NO:
			return ne(3);
		default:
			return irb.getTrue();
	}
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadBranchTarget(
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count == 0)
	{
		return llvm::UndefValue::get(getDefaultType());
	}
	unsigned idx = (si->op_count == 2) ? 1u : 0u;
	return loadOp(si->operands[idx], irb);
}

systemz_cc Capstone2LlvmIrTranslatorSysz_impl::conditionFromInsn(
		cs_insn* i,
		cs_sysz* si)
{
	if (si->cc != SYSTEMZ_CC_INVALID)
	{
		return si->cc;
	}

	switch (i->id)
	{
		case SYSZ_INS_JE:
		case SYSZ_INS_JZ:
		case SYSZ_INS_J_G_L_E:
		case SYSZ_INS_J_G_L_Z:
			return SYSTEMZ_CC_E;
		case SYSZ_INS_JH:
		case SYSZ_INS_JP:
		case SYSZ_INS_J_G_L_H:
		case SYSZ_INS_J_G_L_P:
			return SYSTEMZ_CC_H;
		case SYSZ_INS_JL:
		case SYSZ_INS_JM:
		case SYSZ_INS_J_G_L_L:
		case SYSZ_INS_J_G_L_M:
			return SYSTEMZ_CC_L;
		case SYSZ_INS_JO:
		case SYSZ_INS_J_G_L_O:
			return SYSTEMZ_CC_O;
		case SYSZ_INS_JNE:
		case SYSZ_INS_JNZ:
		case SYSZ_INS_J_G_L_NE:
		case SYSZ_INS_J_G_L_NZ:
			return SYSTEMZ_CC_NE;
		case SYSZ_INS_JHE:
		case SYSZ_INS_J_G_L_HE:
			return SYSTEMZ_CC_HE;
		case SYSZ_INS_JLE:
		case SYSZ_INS_J_G_L_LE:
			return SYSTEMZ_CC_LE;
		case SYSZ_INS_JLH:
		case SYSZ_INS_J_G_L_LH:
			return SYSTEMZ_CC_LH;
		case SYSZ_INS_JNL:
		case SYSZ_INS_JNM:
		case SYSZ_INS_J_G_L_NL:
		case SYSZ_INS_J_G_L_NM:
			return SYSTEMZ_CC_NL;
		case SYSZ_INS_JNH:
		case SYSZ_INS_JNP:
		case SYSZ_INS_J_G_L_NH:
		case SYSZ_INS_J_G_L_NP:
			return SYSTEMZ_CC_NH;
		case SYSZ_INS_JNLE:
		case SYSZ_INS_J_G_L_NLE:
			return SYSTEMZ_CC_NLE;
		case SYSZ_INS_JNHE:
		case SYSZ_INS_J_G_L_NHE:
			return SYSTEMZ_CC_NHE;
		case SYSZ_INS_JNLH:
		case SYSZ_INS_J_G_L_NLH:
			return SYSTEMZ_CC_NLH;
		case SYSZ_INS_JNO:
		case SYSZ_INS_J_G_L_NO:
			return SYSTEMZ_CC_NO;
		case SYSZ_INS_LOCE:
		case SYSZ_INS_LOCZ:
		case SYSZ_INS_LOCGE:
		case SYSZ_INS_LOCGZ:
		case SYSZ_INS_LOCGRE:
		case SYSZ_INS_LOCGRZ:
		case SYSZ_INS_LOCRE:
		case SYSZ_INS_LOCRZ:
		case SYSZ_INS_LOCHIE:
		case SYSZ_INS_LOCHIZ:
		case SYSZ_INS_LOCGHIE:
		case SYSZ_INS_LOCGHIZ:
			return SYSTEMZ_CC_E;
		case SYSZ_INS_LOCH:
		case SYSZ_INS_LOCP:
		case SYSZ_INS_LOCGH:
		case SYSZ_INS_LOCGP:
		case SYSZ_INS_LOCGRH:
		case SYSZ_INS_LOCGRP:
		case SYSZ_INS_LOCRH:
		case SYSZ_INS_LOCRP:
		case SYSZ_INS_LOCHIH:
		case SYSZ_INS_LOCHIP:
		case SYSZ_INS_LOCGHIH:
		case SYSZ_INS_LOCGHIP:
			return SYSTEMZ_CC_H;
		case SYSZ_INS_LOCL:
		case SYSZ_INS_LOCM:
		case SYSZ_INS_LOCGL:
		case SYSZ_INS_LOCGM:
		case SYSZ_INS_LOCGRL:
		case SYSZ_INS_LOCGRM:
		case SYSZ_INS_LOCRL:
		case SYSZ_INS_LOCRM:
		case SYSZ_INS_LOCHIL:
		case SYSZ_INS_LOCHIM:
		case SYSZ_INS_LOCGHIL:
		case SYSZ_INS_LOCGHIM:
			return SYSTEMZ_CC_L;
		case SYSZ_INS_LOCO:
		case SYSZ_INS_LOCGO:
		case SYSZ_INS_LOCGRO:
		case SYSZ_INS_LOCRO:
		case SYSZ_INS_LOCHIO:
		case SYSZ_INS_LOCGHIO:
			return SYSTEMZ_CC_O;
		case SYSZ_INS_LOCNE:
		case SYSZ_INS_LOCNZ:
		case SYSZ_INS_LOCGNE:
		case SYSZ_INS_LOCGNZ:
		case SYSZ_INS_LOCGRNE:
		case SYSZ_INS_LOCGRNZ:
		case SYSZ_INS_LOCRNE:
		case SYSZ_INS_LOCRNZ:
		case SYSZ_INS_LOCHINE:
		case SYSZ_INS_LOCHINZ:
		case SYSZ_INS_LOCGHINE:
		case SYSZ_INS_LOCGHINZ:
			return SYSTEMZ_CC_NE;
		case SYSZ_INS_LOCHE:
		case SYSZ_INS_LOCGHE:
		case SYSZ_INS_LOCGRHE:
		case SYSZ_INS_LOCRHE:
		case SYSZ_INS_LOCHIHE:
		case SYSZ_INS_LOCGHIHE:
			return SYSTEMZ_CC_HE;
		case SYSZ_INS_LOCLE:
		case SYSZ_INS_LOCGLE:
		case SYSZ_INS_LOCGRLE:
		case SYSZ_INS_LOCRLE:
		case SYSZ_INS_LOCHILE:
		case SYSZ_INS_LOCGHILE:
			return SYSTEMZ_CC_LE;
		case SYSZ_INS_LOCLH:
		case SYSZ_INS_LOCGLH:
		case SYSZ_INS_LOCGRLH:
		case SYSZ_INS_LOCRLH:
		case SYSZ_INS_LOCHILH:
		case SYSZ_INS_LOCGHILH:
			return SYSTEMZ_CC_LH;
		case SYSZ_INS_LOCNL:
		case SYSZ_INS_LOCNM:
		case SYSZ_INS_LOCGNL:
		case SYSZ_INS_LOCGNM:
		case SYSZ_INS_LOCGRNL:
		case SYSZ_INS_LOCGRNM:
		case SYSZ_INS_LOCRNL:
		case SYSZ_INS_LOCRNM:
		case SYSZ_INS_LOCHINL:
		case SYSZ_INS_LOCHINM:
		case SYSZ_INS_LOCGHINL:
		case SYSZ_INS_LOCGHINM:
			return SYSTEMZ_CC_NL;
		case SYSZ_INS_LOCNH:
		case SYSZ_INS_LOCNP:
		case SYSZ_INS_LOCGNH:
		case SYSZ_INS_LOCGNP:
		case SYSZ_INS_LOCGRNH:
		case SYSZ_INS_LOCGRNP:
		case SYSZ_INS_LOCRNH:
		case SYSZ_INS_LOCRNP:
		case SYSZ_INS_LOCHINH:
		case SYSZ_INS_LOCHINP:
		case SYSZ_INS_LOCGHINH:
		case SYSZ_INS_LOCGHINP:
			return SYSTEMZ_CC_NH;
		case SYSZ_INS_LOCNLE:
		case SYSZ_INS_LOCGNLE:
		case SYSZ_INS_LOCGRNLE:
		case SYSZ_INS_LOCRNLE:
		case SYSZ_INS_LOCHINLE:
		case SYSZ_INS_LOCGHINLE:
			return SYSTEMZ_CC_NLE;
		case SYSZ_INS_LOCNHE:
		case SYSZ_INS_LOCGNHE:
		case SYSZ_INS_LOCGRNHE:
		case SYSZ_INS_LOCRNHE:
		case SYSZ_INS_LOCHINHE:
		case SYSZ_INS_LOCGHINHE:
			return SYSTEMZ_CC_NHE;
		case SYSZ_INS_LOCNLH:
		case SYSZ_INS_LOCGNLH:
		case SYSZ_INS_LOCGRNLH:
		case SYSZ_INS_LOCRNLH:
		case SYSZ_INS_LOCHINLH:
		case SYSZ_INS_LOCGHINLH:
			return SYSTEMZ_CC_NLH;
		case SYSZ_INS_LOCNO:
		case SYSZ_INS_LOCGNO:
		case SYSZ_INS_LOCGRNO:
		case SYSZ_INS_LOCRNO:
		case SYSZ_INS_LOCHINO:
		case SYSZ_INS_LOCGHINO:
			return SYSTEMZ_CC_NO;
		default:
			return SYSTEMZ_CC_INVALID;
	}
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::generateCcMask(
		unsigned mask,
		llvm::IRBuilder<>& irb)
{
	mask &= 15u;
	if (mask == 0)
	{
		return irb.getFalse();
	}
	if (mask == 15)
	{
		return irb.getTrue();
	}
	auto* ccv = loadRegister(SYSZ_REG_CC, irb);
	ccv = irb.CreateZExtOrTrunc(ccv, irb.getInt32Ty());
	auto* bit = irb.CreateLShr(irb.getInt32(8), ccv);
	return irb.CreateICmpNE(
			irb.CreateAnd(bit, irb.getInt32(mask)),
			irb.getInt32(0));
}

bool Capstone2LlvmIrTranslatorSysz_impl::isLoc64(unsigned id) const
{
	switch (id)
	{
		case SYSZ_INS_LOCG:
		case SYSZ_INS_LOCGE:
		case SYSZ_INS_LOCGH:
		case SYSZ_INS_LOCGHE:
		case SYSZ_INS_LOCGL:
		case SYSZ_INS_LOCGLE:
		case SYSZ_INS_LOCGLH:
		case SYSZ_INS_LOCGM:
		case SYSZ_INS_LOCGNE:
		case SYSZ_INS_LOCGNH:
		case SYSZ_INS_LOCGNHE:
		case SYSZ_INS_LOCGNL:
		case SYSZ_INS_LOCGNLE:
		case SYSZ_INS_LOCGNLH:
		case SYSZ_INS_LOCGNM:
		case SYSZ_INS_LOCGNO:
		case SYSZ_INS_LOCGNP:
		case SYSZ_INS_LOCGNZ:
		case SYSZ_INS_LOCGO:
		case SYSZ_INS_LOCGP:
		case SYSZ_INS_LOCGZ:
		case SYSZ_INS_LOCGR:
		case SYSZ_INS_LOCGRE:
		case SYSZ_INS_LOCGRH:
		case SYSZ_INS_LOCGRHE:
		case SYSZ_INS_LOCGRL:
		case SYSZ_INS_LOCGRLE:
		case SYSZ_INS_LOCGRLH:
		case SYSZ_INS_LOCGRM:
		case SYSZ_INS_LOCGRNE:
		case SYSZ_INS_LOCGRNH:
		case SYSZ_INS_LOCGRNHE:
		case SYSZ_INS_LOCGRNL:
		case SYSZ_INS_LOCGRNLE:
		case SYSZ_INS_LOCGRNLH:
		case SYSZ_INS_LOCGRNM:
		case SYSZ_INS_LOCGRNO:
		case SYSZ_INS_LOCGRNP:
		case SYSZ_INS_LOCGRNZ:
		case SYSZ_INS_LOCGRO:
		case SYSZ_INS_LOCGRP:
		case SYSZ_INS_LOCGRZ:
		case SYSZ_INS_LOCGHI:
		case SYSZ_INS_LOCGHIE:
		case SYSZ_INS_LOCGHIH:
		case SYSZ_INS_LOCGHIHE:
		case SYSZ_INS_LOCGHIL:
		case SYSZ_INS_LOCGHILE:
		case SYSZ_INS_LOCGHILH:
		case SYSZ_INS_LOCGHIM:
		case SYSZ_INS_LOCGHINE:
		case SYSZ_INS_LOCGHINH:
		case SYSZ_INS_LOCGHINHE:
		case SYSZ_INS_LOCGHINL:
		case SYSZ_INS_LOCGHINLE:
		case SYSZ_INS_LOCGHINLH:
		case SYSZ_INS_LOCGHINM:
		case SYSZ_INS_LOCGHINO:
		case SYSZ_INS_LOCGHINP:
		case SYSZ_INS_LOCGHINZ:
		case SYSZ_INS_LOCGHIO:
		case SYSZ_INS_LOCGHIP:
		case SYSZ_INS_LOCGHIZ:
			return true;
		default:
			return false;
	}
}

bool Capstone2LlvmIrTranslatorSysz_impl::isAlwaysCondition(systemz_cc cc, cs_insn* i)
{
	return cc == SYSTEMZ_CC_INVALID
			&& (i->id == SYSZ_INS_J
					|| i->id == SYSZ_INS_J_G_LU_
					|| i->id == SYSZ_INS_BR
					|| i->id == SYSZ_INS_BRC
					|| i->id == SYSZ_INS_BRCL
					|| i->id == SYSZ_INS_BCR);
}

//
//==============================================================================
// SystemZ instruction translation methods.
//==============================================================================
//

void Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb);
	depositLow32(si->operands[0].reg, extractLow32(op1, irb), irb);
	if (i->id == SYSZ_INS_LTR)
	{
		storeCcSigned(extractLow32(op1, irb), irb.getFalse(), irb);
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb);
	storeOp(si->operands[0], op1, irb);
	if (i->id == SYSZ_INS_LTGR)
	{
		storeCcSigned(op1, irb.getFalse(), irb);
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateAdd32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = extractLow32(loadOp(si->operands[0], irb), irb);
	op1 = extractLow32(loadOp(si->operands[1], irb), irb);
	auto* add = irb.CreateAdd(op0, op1);
	depositLow32(si->operands[0].reg, add, irb);
	storeCcSigned(add, generateOverflowAdd(add, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateAdd64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	auto* add = irb.CreateAdd(op0, op1);
	storeOp(si->operands[0], add, irb);
	storeCcSigned(add, generateOverflowAdd(add, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateSub32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = extractLow32(loadOp(si->operands[0], irb), irb);
	op1 = extractLow32(loadOp(si->operands[1], irb), irb);
	auto* sub = irb.CreateSub(op0, op1);
	depositLow32(si->operands[0].reg, sub, irb);
	storeCcSigned(sub, generateOverflowSub(sub, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateSub64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	auto* sub = irb.CreateSub(op0, op1);
	storeOp(si->operands[0], sub, irb);
	storeCcSigned(sub, generateOverflowSub(sub, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLogical32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = extractLow32(loadOp(si->operands[0], irb), irb);
	op1 = extractLow32(loadOp(si->operands[1], irb), irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_NR:
			res = irb.CreateAnd(op0, op1);
			break;
		case SYSZ_INS_OR:
			res = irb.CreateOr(op0, op1);
			break;
		case SYSZ_INS_XR:
			res = irb.CreateXor(op0, op1);
			break;
		default:
			throw GenericError("Unhandled logical insn in translateLogical32().");
	}
	depositLow32(si->operands[0].reg, res, irb);
	storeCcLogical(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoad32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, irb.getInt32Ty());
	depositLow32(si->operands[0].reg, op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoad64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, irb.getInt64Ty());
	storeOp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateStore32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = extractLow32(loadOp(si->operands[0], irb), irb);
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, op0, addr, irb.getInt32Ty());
}

void Capstone2LlvmIrTranslatorSysz_impl::translateStore64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb);
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, op0, addr, irb.getInt64Ty());
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLa(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, nullptr, /*lea=*/true);
	// LA writes a 32-bit address into bits 32-63; bit 32 (the high bit of
	// that half) is forced to 0, matching z/Architecture LA.
	auto* addr32 = irb.CreateAnd(
			extractLow32(op1, irb),
			llvm::ConstantInt::get(irb.getInt32Ty(), 0x7FFFFFFF));
	depositLow32(si->operands[0].reg, addr32, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateBr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, si, irb);
	unsigned idx = (si->op_count == 2) ? 1u : 0u;
	auto& top = si->operands[idx];
	if (top.type == SYSZ_OP_REG && top.reg == SYSZ_REG_R0D)
	{
		return;
	}
	op0 = loadOp(top, irb);
	generateBranchFunctionCall(irb, op0);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateBrc(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, si, irb);
	auto cc = conditionFromInsn(i, si);
	op0 = loadBranchTarget(si, irb);
	if (isAlwaysCondition(cc, i))
	{
		generateBranchFunctionCall(irb, op0);
		return;
	}
	auto* cond = generateCondition(cc, irb);
	generateCondBranchFunctionCall(irb, cond, op0);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateBasr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	storeRegister(si->operands[0].reg, getNextInsnAddress(i), irb);
	auto& t = si->operands[1];
	if (t.type == SYSZ_OP_REG && (t.reg == SYSZ_REG_R0D || t.reg == SYSZ_REG_INVALID))
	{
		return;
	}
	op1 = loadOp(t, irb);
	generateCallFunctionCall(irb, op1);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateBrasl(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	storeRegister(si->operands[0].reg, getNextInsnAddress(i), irb);
	op1 = loadOp(si->operands[1], irb);
	generateCallFunctionCall(irb, op1);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcFp(
		llvm::Value* result,
		llvm::IRBuilder<>& irb)
{
	auto* zero = llvm::ConstantFP::get(result->getType(), 0.0);
	auto* isNan = irb.CreateFCmpUNO(result, result);
	auto* isZero = irb.CreateFCmpOEQ(result, zero);
	auto* isNeg = irb.CreateFCmpOLT(result, zero);
	auto* ccLt = llvm::ConstantInt::get(irb.getInt8Ty(), 1);
	auto* ccGt = llvm::ConstantInt::get(irb.getInt8Ty(), 2);
	auto* ccEq = llvm::ConstantInt::get(irb.getInt8Ty(), 0);
	auto* ccNan = llvm::ConstantInt::get(irb.getInt8Ty(), 3);
	auto* signedCc = irb.CreateSelect(isZero, ccEq, irb.CreateSelect(isNeg, ccLt, ccGt));
	storeRegister(SYSZ_REG_CC, irb.CreateSelect(isNan, ccNan, signedCc), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcFpCompare(
		llvm::Value* a,
		llvm::Value* b,
		llvm::IRBuilder<>& irb)
{
	auto* unord = irb.CreateFCmpUNO(a, b);
	auto* eq = irb.CreateFCmpOEQ(a, b);
	auto* lt = irb.CreateFCmpOLT(a, b);
	auto* ccLt = llvm::ConstantInt::get(irb.getInt8Ty(), 1);
	auto* ccGt = llvm::ConstantInt::get(irb.getInt8Ty(), 2);
	auto* ccEq = llvm::ConstantInt::get(irb.getInt8Ty(), 0);
	auto* ccUn = llvm::ConstantInt::get(irb.getInt8Ty(), 3);
	auto* ordered = irb.CreateSelect(eq, ccEq, irb.CreateSelect(lt, ccLt, ccGt));
	storeRegister(SYSZ_REG_CC, irb.CreateSelect(unord, ccUn, ordered), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcCompare(
		llvm::Value* a,
		llvm::Value* b,
		llvm::IRBuilder<>& irb)
{
	auto* eq = irb.CreateICmpEQ(a, b);
	auto* lt = irb.CreateICmpSLT(a, b);
	auto* cc = irb.CreateSelect(
			eq,
			llvm::ConstantInt::get(irb.getInt8Ty(), 0),
			irb.CreateSelect(
					lt,
					llvm::ConstantInt::get(irb.getInt8Ty(), 1),
					llvm::ConstantInt::get(irb.getInt8Ty(), 2)));
	storeRegister(SYSZ_REG_CC, cc, irb);
}

bool Capstone2LlvmIrTranslatorSysz_impl::isFpSingleInsn(unsigned id) const
{
	switch (id)
	{
		case SYSZ_INS_AEBR:
		case SYSZ_INS_AEB:
		case SYSZ_INS_SEBR:
		case SYSZ_INS_SEB:
		case SYSZ_INS_MEEBR:
		case SYSZ_INS_MEEB:
		case SYSZ_INS_DEBR:
		case SYSZ_INS_DEB:
		case SYSZ_INS_CEBR:
		case SYSZ_INS_CEB:
		case SYSZ_INS_LE:
		case SYSZ_INS_LEY:
		case SYSZ_INS_STE:
		case SYSZ_INS_STEY:
		case SYSZ_INS_LER:
		case SYSZ_INS_FIEBR:
		case SYSZ_INS_FIEBRA:
			return true;
		default:
			return false;
	}
}

llvm::Type* Capstone2LlvmIrTranslatorSysz_impl::fpTypeForInsn(
		unsigned id,
		llvm::IRBuilder<>& irb) const
{
	return isFpSingleInsn(id) ? irb.getFloatTy() : irb.getDoubleTy();
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadFp(
		cs_sysz_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty)
{
	auto* v = loadOp(op, irb, ty);
	if (v->getType() == ty)
	{
		return v;
	}
	if (v->getType()->isFloatingPointTy())
	{
		return irb.CreateFPCast(v, ty);
	}
	return generateTypeConversion(irb, v, ty, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeFp(
		cs_sysz_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	storeOp(op, val, irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpArith(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op0 = loadFp(si->operands[0], irb, ty);
	op1 = loadFp(si->operands[1], irb, ty);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_AEBR:
		case SYSZ_INS_AEB:
		case SYSZ_INS_ADBR:
		case SYSZ_INS_ADB:
			res = irb.CreateFAdd(op0, op1);
			break;
		case SYSZ_INS_SEBR:
		case SYSZ_INS_SEB:
		case SYSZ_INS_SDBR:
		case SYSZ_INS_SDB:
			res = irb.CreateFSub(op0, op1);
			break;
		case SYSZ_INS_MEEBR:
		case SYSZ_INS_MEEB:
		case SYSZ_INS_MDBR:
		case SYSZ_INS_MDB:
			res = irb.CreateFMul(op0, op1);
			break;
		case SYSZ_INS_DEBR:
		case SYSZ_INS_DEB:
		case SYSZ_INS_DDBR:
		case SYSZ_INS_DDB:
			res = irb.CreateFDiv(op0, op1);
			break;
		default:
			throw GenericError("Unhandled FP arith insn.");
	}
	storeFp(si->operands[0], res, irb);
	storeCcFp(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpCompare(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op0 = loadFp(si->operands[0], irb, ty);
	op1 = loadFp(si->operands[1], irb, ty);
	storeCcFpCompare(op0, op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpLoad(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op1 = loadFp(si->operands[1], irb, ty);
	storeFp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpStore(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op0 = loadFp(si->operands[0], irb, ty);
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, op0, addr, ty);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpMove(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op1 = loadFp(si->operands[1], irb, ty);
	storeFp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLdeb(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadFp(si->operands[1], irb, irb.getFloatTy());
	storeFp(si->operands[0], irb.CreateFPExt(op1, irb.getDoubleTy()), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLedbr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadFp(si->operands[1], irb, irb.getDoubleTy());
	storeFp(si->operands[0], irb.CreateFPTrunc(op1, irb.getFloatTy()), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLogical64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_NGR:
		case SYSZ_INS_NG:
			res = irb.CreateAnd(op0, op1);
			break;
		case SYSZ_INS_OGR:
		case SYSZ_INS_OG:
			res = irb.CreateOr(op0, op1);
			break;
		case SYSZ_INS_XGR:
		case SYSZ_INS_XG:
			res = irb.CreateXor(op0, op1);
			break;
		default:
			throw GenericError("Unhandled logical insn in translateLogical64().");
	}
	storeOp(si->operands[0], res, irb);
	storeCcLogical(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateImm64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb);
	if (i->id == SYSZ_INS_LHI)
	{
		depositLow32(si->operands[0].reg, extractLow32(op1, irb), irb);
		return;
	}
	storeOp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateAddImm(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	if (i->id == SYSZ_INS_AHI || i->id == SYSZ_INS_AFI)
	{
		op0 = extractLow32(loadOp(si->operands[0], irb), irb);
		op1 = extractLow32(loadOp(si->operands[1], irb), irb);
		auto* add = irb.CreateAdd(op0, op1);
		depositLow32(si->operands[0].reg, add, irb);
		storeCcSigned(add, generateOverflowAdd(add, op0, op1, irb), irb);
		return;
	}
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	auto* add = irb.CreateAdd(op0, op1);
	storeOp(si->operands[0], add, irb);
	storeCcSigned(add, generateOverflowAdd(add, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateCompare(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	bool is32 = (i->id == SYSZ_INS_CR || i->id == SYSZ_INS_CHI);
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	if (is32)
	{
		op0 = extractLow32(op0, irb);
		op1 = extractLow32(op1, irb);
	}
	storeCcCompare(op0, op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateExtend32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	bool isMem = (i->id == SYSZ_INS_LGF || i->id == SYSZ_INS_LLGF);
	bool isUnsigned = (i->id == SYSZ_INS_LLGFR || i->id == SYSZ_INS_LLGF);
	op1 = isMem
			? loadOp(si->operands[1], irb, irb.getInt32Ty())
			: extractLow32(loadOp(si->operands[1], irb), irb);
	auto* ext = isUnsigned
			? irb.CreateZExt(op1, irb.getInt64Ty())
			: irb.CreateSExt(op1, irb.getInt64Ty());
	storeOp(si->operands[0], ext, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateShift64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, si, irb);
	unsigned srcIdx = (si->op_count == 3) ? 1u : 0u;
	unsigned amtIdx = (si->op_count == 3) ? 2u : 1u;
	op0 = loadOp(si->operands[srcIdx], irb);
	auto* amt = loadOp(si->operands[amtIdx], irb, nullptr, /*lea=*/true);
	bool is32 = (i->id == SYSZ_INS_SLL || i->id == SYSZ_INS_SRL
			|| i->id == SYSZ_INS_SRA);
	if (is32)
	{
		op0 = extractLow32(op0, irb);
	}
	amt = irb.CreateZExtOrTrunc(amt, op0->getType());
	amt = irb.CreateAnd(amt, llvm::ConstantInt::get(op0->getType(), 63));
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_SLLG:
		case SYSZ_INS_SLL:
			res = irb.CreateShl(op0, amt);
			break;
		case SYSZ_INS_SRLG:
		case SYSZ_INS_SRL:
			res = irb.CreateLShr(op0, amt);
			break;
		case SYSZ_INS_SRAG:
		case SYSZ_INS_SRA:
			res = irb.CreateAShr(op0, amt);
			break;
		default:
			throw GenericError("Unhandled shift insn in translateShift64().");
	}
	if (is32)
	{
		depositLow32(si->operands[0].reg, res, irb);
		return;
	}
	storeOp(si->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLay(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	bool lea = (si->operands[1].type == SYSZ_OP_MEM);
	op1 = loadOp(si->operands[1], irb, nullptr, lea);
	storeOp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorLoad(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, irb.getInt128Ty());
	storeOp(si->operands[0], op1, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorStore(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb, irb.getInt128Ty());
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, op0, addr, irb.getInt128Ty());
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVlr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, irb.getInt128Ty());
	storeOp(si->operands[0], op1, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVlrep(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, si, irb);
	unsigned bits = 64;
	switch (i->id)
	{
		case SYSZ_INS_VLREPB:
			bits = 8;
			break;
		case SYSZ_INS_VLREPH:
			bits = 16;
			break;
		case SYSZ_INS_VLREPF:
			bits = 32;
			break;
		case SYSZ_INS_VLREPG:
			bits = 64;
			break;
		case SYSZ_INS_VLREP:
			if (si->op_count >= 3 && si->operands[2].type == SYSZ_OP_IMM)
			{
				static const unsigned kM3Bits[4] = {8, 16, 32, 64};
				auto m = static_cast<unsigned>(si->operands[2].imm);
				if (m < 4)
				{
					bits = kM3Bits[m];
				}
			}
			break;
		default:
			break;
	}
	auto* elTy = irb.getIntNTy(bits);
	op1 = loadOp(si->operands[1], irb, elTy);
	auto* z = irb.CreateZExt(op1, irb.getInt128Ty());
	llvm::Value* r = z;
	for (unsigned sh = bits; sh < 128; sh += bits)
	{
		r = irb.CreateOr(r, irb.CreateShl(z, sh));
	}
	storeOp(si->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVleg(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, si, irb);
	bool isF = (i->id == SYSZ_INS_VLEF);
	unsigned elemBits = isF ? 32u : 64u;
	unsigned idx = 0;
	if (si->op_count >= 3 && si->operands[2].type == SYSZ_OP_IMM)
	{
		idx = static_cast<unsigned>(si->operands[2].imm);
	}
	auto* vec = loadOp(si->operands[0], irb, irb.getInt128Ty());
	auto* el = loadOp(si->operands[1], irb, irb.getIntNTy(elemBits));
	el = irb.CreateZExt(el, irb.getInt128Ty());
	// Element 0 is the leftmost (high) bits of the 128-bit vector.
	unsigned nElem = 128 / elemBits;
	if (idx >= nElem)
	{
		idx = nElem - 1;
	}
	unsigned shift = (nElem - 1 - idx) * elemBits;
	llvm::APInt ones = llvm::APInt::getLowBitsSet(128, elemBits).shl(shift);
	auto* mask = llvm::ConstantInt::get(irb.getInt128Ty(), ones);
	auto* cleared = irb.CreateAnd(vec, irb.CreateNot(mask));
	auto* placed = irb.CreateShl(el, shift);
	storeOp(si->operands[0], irb.CreateOr(cleared, placed), irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVsteg(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, si, irb);
	bool isF = (i->id == SYSZ_INS_VSTEF);
	unsigned elemBits = isF ? 32u : 64u;
	unsigned idx = 0;
	if (si->op_count >= 3 && si->operands[2].type == SYSZ_OP_IMM)
	{
		idx = static_cast<unsigned>(si->operands[2].imm);
	}
	auto* vec = loadOp(si->operands[0], irb, irb.getInt128Ty());
	unsigned nElem = 128 / elemBits;
	if (idx >= nElem)
	{
		idx = nElem - 1;
	}
	unsigned shift = (nElem - 1 - idx) * elemBits;
	auto* el = irb.CreateTrunc(irb.CreateLShr(vec, shift), irb.getIntNTy(elemBits));
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, el, addr, irb.getIntNTy(elemBits));
}

unsigned Capstone2LlvmIrTranslatorSysz_impl::gprIndex(uint32_t r) const
{
	if (r >= SYSZ_REG_R0D && r <= SYSZ_REG_R15D)
	{
		return r - SYSZ_REG_R0D;
	}
	if (r >= SYSZ_REG_R0L && r <= SYSZ_REG_R15L)
	{
		return r - SYSZ_REG_R0L;
	}
	if (r >= SYSZ_REG_R0H && r <= SYSZ_REG_R15H)
	{
		return r - SYSZ_REG_R0H;
	}
	if (r >= SYSZ_REG_R0Q && r <= SYSZ_REG_R14Q)
	{
		return 2u * (r - SYSZ_REG_R0Q);
	}
	return 0;
}

uint32_t Capstone2LlvmIrTranslatorSysz_impl::gpr64(unsigned idx) const
{
	return SYSZ_REG_R0D + (idx & 15u);
}

uint64_t Capstone2LlvmIrTranslatorSysz_impl::ssLength(cs_sysz* si) const
{
	for (unsigned i = 0; i < si->op_count; ++i)
	{
		if (si->operands[i].type == SYSZ_OP_MEM && si->operands[i].mem.length != 0)
		{
			return si->operands[i].mem.length;
		}
	}
	return 1;
}

unsigned Capstone2LlvmIrTranslatorSysz_impl::lastImm(cs_sysz* si, unsigned def) const
{
	for (unsigned i = si->op_count; i > 0; --i)
	{
		if (si->operands[i - 1].type == SYSZ_OP_IMM)
		{
			return static_cast<unsigned>(si->operands[i - 1].imm);
		}
	}
	return def;
}

unsigned Capstone2LlvmIrTranslatorSysz_impl::vectorLaneBits(cs_insn* i, cs_sysz* si) const
{
	switch (i->id)
	{
		case SYSZ_INS_VAB:
		case SYSZ_INS_VSB:
		case SYSZ_INS_VCEQB:
		case SYSZ_INS_VMLB:
		case SYSZ_INS_VMHB:
		case SYSZ_INS_VREPB:
		case SYSZ_INS_VREPIB:
		case SYSZ_INS_VAVGB:
		case SYSZ_INS_VAVGLB:
		case SYSZ_INS_VCHB:
		case SYSZ_INS_VESLB:
		case SYSZ_INS_VESLVB:
		case SYSZ_INS_VESRAB:
		case SYSZ_INS_VESRAVB:
		case SYSZ_INS_VESRLB:
		case SYSZ_INS_VESRLVB:
		case SYSZ_INS_VLCB:
		case SYSZ_INS_VLPB:
			return 8;
		case SYSZ_INS_VAH:
		case SYSZ_INS_VSH:
		case SYSZ_INS_VCEQH:
		case SYSZ_INS_VMLH:
		case SYSZ_INS_VMHH:
		case SYSZ_INS_VREPH:
		case SYSZ_INS_VREPIH:
		case SYSZ_INS_VPKH:
		case SYSZ_INS_VPKLSH:
		case SYSZ_INS_VPKSH:
		case SYSZ_INS_VAVGH:
		case SYSZ_INS_VAVGLH:
		case SYSZ_INS_VCHH:
		case SYSZ_INS_VESLH:
		case SYSZ_INS_VESLVH:
		case SYSZ_INS_VESRAH:
		case SYSZ_INS_VESRAVH:
		case SYSZ_INS_VESRLH:
		case SYSZ_INS_VESRLVH:
		case SYSZ_INS_VLCH:
		case SYSZ_INS_VLPH:
			return 16;
		case SYSZ_INS_VAF:
		case SYSZ_INS_VSF:
		case SYSZ_INS_VCEQF:
		case SYSZ_INS_VMLF:
		case SYSZ_INS_VMHF:
		case SYSZ_INS_VREPF:
		case SYSZ_INS_VREPIF:
		case SYSZ_INS_VPKF:
		case SYSZ_INS_VPKLSF:
		case SYSZ_INS_VPKSF:
		case SYSZ_INS_VAVGF:
		case SYSZ_INS_VAVGLF:
		case SYSZ_INS_VCHF:
		case SYSZ_INS_VESLF:
		case SYSZ_INS_VESLVF:
		case SYSZ_INS_VESRAF:
		case SYSZ_INS_VESRAVF:
		case SYSZ_INS_VESRLF:
		case SYSZ_INS_VESRLVF:
		case SYSZ_INS_VLCF:
		case SYSZ_INS_VLPF:
			return 32;
		case SYSZ_INS_VAG:
		case SYSZ_INS_VSG:
		case SYSZ_INS_VCEQG:
		case SYSZ_INS_VREPG:
		case SYSZ_INS_VREPIG:
		case SYSZ_INS_VPKG:
		case SYSZ_INS_VPKLSG:
		case SYSZ_INS_VPKSG:
		case SYSZ_INS_VAVGG:
		case SYSZ_INS_VAVGLG:
		case SYSZ_INS_VCHG:
		case SYSZ_INS_VESLG:
		case SYSZ_INS_VESLVG:
		case SYSZ_INS_VESRAG:
		case SYSZ_INS_VESRAVG:
		case SYSZ_INS_VESRLG:
		case SYSZ_INS_VESRLVG:
		case SYSZ_INS_VLCG:
		case SYSZ_INS_VLPG:
			return 64;
		case SYSZ_INS_VAQ:
		case SYSZ_INS_VSQ:
			return 128;
		case SYSZ_INS_VA:
		case SYSZ_INS_VS:
		case SYSZ_INS_VCEQ:
		case SYSZ_INS_VML:
		case SYSZ_INS_VMH:
		case SYSZ_INS_VREP:
		case SYSZ_INS_VREPI:
		case SYSZ_INS_VPK:
		case SYSZ_INS_VPKLS:
		case SYSZ_INS_VPKS:
		case SYSZ_INS_VAVG:
		case SYSZ_INS_VAVGL:
		case SYSZ_INS_VCH:
		case SYSZ_INS_VESL:
		case SYSZ_INS_VESLV:
		case SYSZ_INS_VESRA:
		case SYSZ_INS_VESRAV:
		case SYSZ_INS_VESRL:
		case SYSZ_INS_VESRLV:
		case SYSZ_INS_VLC:
		case SYSZ_INS_VLP:
		{
			static const unsigned kM4[5] = {8, 16, 32, 64, 128};
			auto m = lastImm(si, 0);
			return m < 5 ? kM4[m] : 8;
		}
		default:
			return 128;
	}
}

unsigned Capstone2LlvmIrTranslatorSysz_impl::vectorFpLaneBits(cs_insn* i, cs_sysz* si) const
{
	switch (i->id)
	{
		case SYSZ_INS_VFASB:
		case SYSZ_INS_VFSSB:
		case SYSZ_INS_VFMSB:
		case SYSZ_INS_VFDSB:
		case SYSZ_INS_VFCESB:
		case SYSZ_INS_VFCHSB:
		case SYSZ_INS_VFMASB:
			return 32;
		case SYSZ_INS_VFADB:
		case SYSZ_INS_VFSDB:
		case SYSZ_INS_VFMDB:
		case SYSZ_INS_VFDDB:
		case SYSZ_INS_VFCEDB:
		case SYSZ_INS_VFCHDB:
		case SYSZ_INS_VFMADB:
			return 64;
		default:
		{
			auto m = lastImm(si, 3);
			return m == 2 ? 32u : 64u;
		}
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcUnsignedCompare(
		llvm::Value* a,
		llvm::Value* b,
		llvm::IRBuilder<>& irb)
{
	auto* eq = irb.CreateICmpEQ(a, b);
	auto* lt = irb.CreateICmpULT(a, b);
	auto* cc = irb.CreateSelect(
			eq,
			llvm::ConstantInt::get(irb.getInt8Ty(), 0),
			irb.CreateSelect(
					lt,
					llvm::ConstantInt::get(irb.getInt8Ty(), 1),
					llvm::ConstantInt::get(irb.getInt8Ty(), 2)));
	storeRegister(SYSZ_REG_CC, cc, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateSs(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	if (si->operands[0].type != SYSZ_OP_MEM || si->operands[1].type != SYSZ_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	auto* dst = generateMemAddress(si->operands[0], irb);
	auto* src = generateMemAddress(si->operands[1], irb);
	uint64_t len = ssLength(si);
	auto* i8 = irb.getInt8Ty();
	bool logical = (i->id == SYSZ_INS_XC || i->id == SYSZ_INS_NC || i->id == SYSZ_INS_OC);
	llvm::Value* acc = logical ? llvm::ConstantInt::get(i8, 0) : nullptr;

	auto apply = [&](llvm::Value* dval, llvm::Value* sval) {
		switch (i->id)
		{
			case SYSZ_INS_NC: return irb.CreateAnd(dval, sval);
			case SYSZ_INS_OC: return irb.CreateOr(dval, sval);
			case SYSZ_INS_XC: return irb.CreateXor(dval, sval);
			default: return sval;
		}
	};

	if (i->id == SYSZ_INS_CLC)
	{
		if (len == 1 || len == 2 || len == 4 || len == 8)
		{
			auto* ty = irb.getIntNTy(static_cast<unsigned>(len * 8));
			storeCcUnsignedCompare(
					loadIntPtr(irb, dst, ty),
					loadIntPtr(irb, src, ty),
					irb);
			return;
		}
		llvm::Value* decided = irb.getFalse();
		llvm::Value* cc = llvm::ConstantInt::get(irb.getInt8Ty(), 0);
		for (uint64_t n = 0; n < len; ++n)
		{
			auto* off = llvm::ConstantInt::get(dst->getType(), n);
			auto* db = loadIntPtr(irb, irb.CreateAdd(dst, off), i8);
			auto* sb = loadIntPtr(irb, irb.CreateAdd(src, off), i8);
			auto* eq = irb.CreateICmpEQ(db, sb);
			auto* lt = irb.CreateICmpULT(db, sb);
			auto* thisCc = irb.CreateSelect(
					eq,
					llvm::ConstantInt::get(irb.getInt8Ty(), 0),
					irb.CreateSelect(
							lt,
							llvm::ConstantInt::get(irb.getInt8Ty(), 1),
							llvm::ConstantInt::get(irb.getInt8Ty(), 2)));
			cc = irb.CreateSelect(decided, cc, thisCc);
			decided = irb.CreateOr(decided, irb.CreateNot(eq));
		}
		storeRegister(SYSZ_REG_CC, cc, irb);
		return;
	}

	if ((i->id == SYSZ_INS_MVC) && (len == 1 || len == 2 || len == 4 || len == 8))
	{
		auto* ty = irb.getIntNTy(static_cast<unsigned>(len * 8));
		storeIntPtr(irb, loadIntPtr(irb, src, ty), dst, ty);
		return;
	}
	if (logical && (len == 1 || len == 2 || len == 4 || len == 8))
	{
		auto* ty = irb.getIntNTy(static_cast<unsigned>(len * 8));
		auto* dval = loadIntPtr(irb, dst, ty);
		auto* sval = loadIntPtr(irb, src, ty);
		auto* res = apply(dval, sval);
		storeIntPtr(irb, res, dst, ty);
		storeCcLogical(res, irb);
		return;
	}

	for (uint64_t n = 0; n < len; ++n)
	{
		auto* off = llvm::ConstantInt::get(dst->getType(), n);
		auto* da = irb.CreateAdd(dst, off);
		auto* sa = irb.CreateAdd(src, off);
		auto* sval = loadIntPtr(irb, sa, i8);
		if (i->id == SYSZ_INS_MVC)
		{
			storeIntPtr(irb, sval, da, i8);
			continue;
		}
		auto* dval = loadIntPtr(irb, da, i8);
		auto* res = apply(dval, sval);
		storeIntPtr(irb, res, da, i8);
		acc = irb.CreateOr(acc, res);
	}
	if (logical)
	{
		storeCcLogical(acc, irb);
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateMvcl(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto dEven = gpr64(gprIndex(si->operands[0].reg));
	auto dOdd = gpr64(gprIndex(si->operands[0].reg) + 1);
	auto sEven = gpr64(gprIndex(si->operands[1].reg));
	auto sOdd = gpr64(gprIndex(si->operands[1].reg) + 1);
	auto* dst = loadRegister(dEven, irb);
	auto* src = loadRegister(sEven, irb);
	auto* dlen = irb.CreateZExt(extractLow32(loadRegister(dOdd, irb), irb), dst->getType());
	auto* sOddV = loadRegister(sOdd, irb);
	auto* slen = irb.CreateZExt(extractLow32(sOddV, irb), dst->getType());
	auto* pad = irb.CreateTrunc(irb.CreateLShr(sOddV, 56), irb.getInt8Ty());
	auto* n = irb.CreateSelect(irb.CreateICmpULT(dlen, slen), dlen, slen);
	auto* dstPtr = intToPtr(irb, dst, irb.getInt8Ty());
	auto* srcPtr = intToPtr(irb, src, irb.getInt8Ty());
	irb.CreateMemCpy(dstPtr, llvm::MaybeAlign(), srcPtr, llvm::MaybeAlign(), n);
	auto* extra = irb.CreateSub(dlen, n);
	auto* padPtr = intToPtr(irb, irb.CreateAdd(dst, n), irb.getInt8Ty());
	irb.CreateMemSet(padPtr, pad, extra, llvm::MaybeAlign());
	storeRegister(dEven, irb.CreateAdd(dst, dlen), irb);
	depositLow32(dOdd, irb.getInt32(0), irb);
	storeRegister(sEven, irb.CreateAdd(src, n), irb);
	depositLow32(sOdd, extractLow32(irb.CreateSub(slen, n), irb), irb);
	auto* eq = irb.CreateICmpEQ(dlen, slen);
	auto* destLonger = irb.CreateICmpUGT(dlen, slen);
	storeRegister(
			SYSZ_REG_CC,
			irb.CreateSelect(
					eq,
					llvm::ConstantInt::get(irb.getInt8Ty(), 0),
					irb.CreateSelect(
							destLonger,
							llvm::ConstantInt::get(irb.getInt8Ty(), 1),
							llvm::ConstantInt::get(irb.getInt8Ty(), 2))),
			irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoadMultiple(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, si, irb);
	bool is64 = (i->id == SYSZ_INS_LMG);
	unsigned first = gprIndex(si->operands[0].reg);
	unsigned last = gprIndex(si->operands[1].reg);
	unsigned n = ((last - first) & 15u) + 1u;
	auto* addr = generateMemAddress(si->operands[2], irb);
	unsigned stride = is64 ? 8u : 4u;
	auto* ty = is64 ? irb.getInt64Ty() : irb.getInt32Ty();
	for (unsigned k = 0; k < n; ++k)
	{
		auto* val = loadIntPtr(irb, addr, ty);
		auto r = gpr64(first + k);
		if (is64)
		{
			storeRegister(r, val, irb);
		}
		else
		{
			depositLow32(r, val, irb);
		}
		addr = irb.CreateAdd(addr, llvm::ConstantInt::get(addr->getType(), stride));
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateStoreMultiple(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, si, irb);
	bool is64 = (i->id == SYSZ_INS_STMG);
	unsigned first = gprIndex(si->operands[0].reg);
	unsigned last = gprIndex(si->operands[1].reg);
	unsigned n = ((last - first) & 15u) + 1u;
	auto* addr = generateMemAddress(si->operands[2], irb);
	unsigned stride = is64 ? 8u : 4u;
	auto* ty = is64 ? irb.getInt64Ty() : irb.getInt32Ty();
	for (unsigned k = 0; k < n; ++k)
	{
		auto* val = loadRegister(gpr64(first + k), irb);
		if (!is64)
		{
			val = extractLow32(val, irb);
		}
		storeIntPtr(irb, val, addr, ty);
		addr = irb.CreateAdd(addr, llvm::ConstantInt::get(addr->getType(), stride));
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateCs(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, si, irb);
	if (si->operands[2].type != SYSZ_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	bool is64 = (i->id == SYSZ_INS_CSG);
	auto* elem = is64 ? irb.getInt64Ty() : irb.getInt32Ty();
	auto* expected = loadOp(si->operands[0], irb);
	auto* desired = loadOp(si->operands[1], irb);
	if (!is64)
	{
		expected = extractLow32(expected, irb);
		desired = extractLow32(desired, irb);
	}
	auto* addr = generateMemAddress(si->operands[2], irb);
	auto* ptr = intToPtr(irb, addr, elem);
	auto* cx = irb.CreateAtomicCmpXchg(
			ptr,
			expected,
			desired,
			llvm::MaybeAlign(),
			llvm::AtomicOrdering::SequentiallyConsistent,
			llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(cx, elem);
	auto* old = irb.CreateExtractValue(cx, 0);
	auto* succ = irb.CreateExtractValue(cx, 1);
	if (is64)
	{
		storeOp(si->operands[0], old, irb);
	}
	else
	{
		depositLow32(si->operands[0].reg, old, irb);
	}
	storeRegister(
			SYSZ_REG_CC,
			irb.CreateSelect(
					succ,
					llvm::ConstantInt::get(irb.getInt8Ty(), 0),
					llvm::ConstantInt::get(irb.getInt8Ty(), 1)),
			irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorArith(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	auto* w = irb.getInt128Ty();
	auto* a = loadOp(si->operands[1], irb, w);
	auto* b = loadOp(si->operands[2], irb, w);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_VN:
			res = irb.CreateAnd(a, b);
			break;
		case SYSZ_INS_VO:
			res = irb.CreateOr(a, b);
			break;
		case SYSZ_INS_VX:
			res = irb.CreateXor(a, b);
			break;
		case SYSZ_INS_VNC:
			res = irb.CreateAnd(a, irb.CreateNot(b));
			break;
		case SYSZ_INS_VOC:
			res = irb.CreateOr(a, irb.CreateNot(b));
			break;
		case SYSZ_INS_VNN:
			res = irb.CreateNot(irb.CreateAnd(a, b));
			break;
		case SYSZ_INS_VNO:
			res = irb.CreateNot(irb.CreateOr(a, b));
			break;
		default:
		{
			unsigned laneBits = vectorLaneBits(i, si);
			unsigned lanes = 128 / laneBits;
			auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), lanes);
			auto* va = irb.CreateBitCast(a, vecTy);
			auto* vb = irb.CreateBitCast(b, vecTy);
			llvm::Value* vr = nullptr;
			switch (i->id)
			{
				case SYSZ_INS_VA:
				case SYSZ_INS_VAB:
				case SYSZ_INS_VAH:
				case SYSZ_INS_VAF:
				case SYSZ_INS_VAG:
				case SYSZ_INS_VAQ:
					vr = irb.CreateAdd(va, vb);
					break;
				case SYSZ_INS_VS:
				case SYSZ_INS_VSB:
				case SYSZ_INS_VSH:
				case SYSZ_INS_VSF:
				case SYSZ_INS_VSG:
				case SYSZ_INS_VSQ:
					vr = irb.CreateSub(va, vb);
					break;
				case SYSZ_INS_VCEQ:
				case SYSZ_INS_VCEQB:
				case SYSZ_INS_VCEQH:
				case SYSZ_INS_VCEQF:
				case SYSZ_INS_VCEQG:
					vr = irb.CreateSExt(irb.CreateICmpEQ(va, vb), vecTy);
					break;
				case SYSZ_INS_VCH:
				case SYSZ_INS_VCHB:
				case SYSZ_INS_VCHH:
				case SYSZ_INS_VCHF:
				case SYSZ_INS_VCHG:
					vr = irb.CreateSExt(irb.CreateICmpSGT(va, vb), vecTy);
					break;
				case SYSZ_INS_VAVG:
				case SYSZ_INS_VAVGB:
				case SYSZ_INS_VAVGH:
				case SYSZ_INS_VAVGF:
				case SYSZ_INS_VAVGG:
				case SYSZ_INS_VAVGL:
				case SYSZ_INS_VAVGLB:
				case SYSZ_INS_VAVGLH:
				case SYSZ_INS_VAVGLF:
				case SYSZ_INS_VAVGLG:
				{
					if (laneBits == 0 || laneBits > 64)
					{
						throwUnexpectedOperands(i);
						translatePseudoAsmGeneric(i, si, irb);
						return;
					}
					bool avgU = (i->id == SYSZ_INS_VAVGL
							|| i->id == SYSZ_INS_VAVGLB
							|| i->id == SYSZ_INS_VAVGLH
							|| i->id == SYSZ_INS_VAVGLF
							|| i->id == SYSZ_INS_VAVGLG);
					auto* wideTy = llvm::FixedVectorType::get(
							irb.getIntNTy(laneBits * 2), lanes);
					auto* wa = avgU ? irb.CreateZExt(va, wideTy)
							: irb.CreateSExt(va, wideTy);
					auto* wb = avgU ? irb.CreateZExt(vb, wideTy)
							: irb.CreateSExt(vb, wideTy);
					auto* sum = irb.CreateAdd(
							irb.CreateAdd(wa, wb),
							llvm::ConstantInt::get(wideTy, 1));
					auto* avg = avgU
							? irb.CreateLShr(sum, 1)
							: irb.CreateAShr(sum, 1);
					vr = irb.CreateTrunc(avg, vecTy);
					break;
				}
				default:
					throwUnexpectedOperands(i);
					translatePseudoAsmGeneric(i, si, irb);
					return;
			}
			res = irb.CreateBitCast(vr, w);
			break;
		}
	}
	storeOp(si->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorFpArith(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned laneBits = vectorFpLaneBits(i, si);
	if (laneBits != 32 && laneBits != 64)
	{
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned lanes = 128 / laneBits;
	auto* w = irb.getInt128Ty();
	llvm::Type* elemTy = laneBits == 32
			? static_cast<llvm::Type*>(irb.getFloatTy())
			: static_cast<llvm::Type*>(irb.getDoubleTy());
	auto* vecTy = llvm::FixedVectorType::get(elemTy, lanes);
	auto* va = irb.CreateBitCast(loadOp(si->operands[1], irb, w), vecTy);
	auto* vb = irb.CreateBitCast(loadOp(si->operands[2], irb, w), vecTy);
	llvm::Value* vr = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_VFA:
		case SYSZ_INS_VFASB:
		case SYSZ_INS_VFADB:
			vr = irb.CreateFAdd(va, vb);
			break;
		case SYSZ_INS_VFS:
		case SYSZ_INS_VFSSB:
		case SYSZ_INS_VFSDB:
			vr = irb.CreateFSub(va, vb);
			break;
		case SYSZ_INS_VFM:
		case SYSZ_INS_VFMSB:
		case SYSZ_INS_VFMDB:
			vr = irb.CreateFMul(va, vb);
			break;
		case SYSZ_INS_VFD:
		case SYSZ_INS_VFDSB:
		case SYSZ_INS_VFDDB:
			vr = irb.CreateFDiv(va, vb);
			break;
		case SYSZ_INS_VFCE:
		case SYSZ_INS_VFCESB:
		case SYSZ_INS_VFCEDB:
		{
			auto* maskTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), lanes);
			vr = irb.CreateBitCast(irb.CreateSExt(irb.CreateFCmpOEQ(va, vb), maskTy), vecTy);
			break;
		}
		case SYSZ_INS_VFCH:
		case SYSZ_INS_VFCHSB:
		case SYSZ_INS_VFCHDB:
		{
			auto* maskTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), lanes);
			vr = irb.CreateBitCast(irb.CreateSExt(irb.CreateFCmpOGT(va, vb), maskTy), vecTy);
			break;
		}
		case SYSZ_INS_VFMA:
		case SYSZ_INS_VFMADB:
		case SYSZ_INS_VFMASB:
		{
			if (si->op_count < 4)
			{
				throwUnexpectedOperands(i);
				translatePseudoAsmGeneric(i, si, irb);
				return;
			}
			auto* acc = irb.CreateBitCast(loadOp(si->operands[3], irb, w), vecTy);
			auto* fma = llvm::Intrinsic::getOrInsertDeclaration(
					_module, llvm::Intrinsic::fma, vecTy);
			vr = irb.CreateCall(fma, {va, vb, acc});
			break;
		}
		default:
			translatePseudoAsmGeneric(i, si, irb);
			return;
	}
	storeOp(si->operands[0], irb.CreateBitCast(vr, w), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpConvert(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned srcIdx = si->op_count - 1;
	auto* srcOp = &si->operands[srcIdx];
	auto* dstOp = &si->operands[0];
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_CGEBR:
		case SYSZ_INS_CGEBRA:
			res = irb.CreateFPToSI(loadFp(*srcOp, irb, irb.getFloatTy()), irb.getInt64Ty());
			storeOp(*dstOp, res, irb);
			storeCcSigned(res, irb.getFalse(), irb);
			return;
		case SYSZ_INS_CFEBR:
		case SYSZ_INS_CFEBRA:
			res = irb.CreateFPToSI(loadFp(*srcOp, irb, irb.getFloatTy()), irb.getInt32Ty());
			depositLow32(dstOp->reg, res, irb);
			storeCcSigned(res, irb.getFalse(), irb);
			return;
		case SYSZ_INS_CGDBR:
		case SYSZ_INS_CGDBRA:
			res = irb.CreateFPToSI(loadFp(*srcOp, irb, irb.getDoubleTy()), irb.getInt64Ty());
			storeOp(*dstOp, res, irb);
			storeCcSigned(res, irb.getFalse(), irb);
			return;
		case SYSZ_INS_CFDBR:
		case SYSZ_INS_CFDBRA:
			res = irb.CreateFPToSI(loadFp(*srcOp, irb, irb.getDoubleTy()), irb.getInt32Ty());
			depositLow32(dstOp->reg, res, irb);
			storeCcSigned(res, irb.getFalse(), irb);
			return;
		case SYSZ_INS_CEGBR:
		case SYSZ_INS_CEGBRA:
			res = irb.CreateSIToFP(loadOp(*srcOp, irb), irb.getFloatTy());
			storeFp(*dstOp, res, irb);
			storeCcFp(res, irb);
			return;
		case SYSZ_INS_CDGBR:
		case SYSZ_INS_CDGBRA:
			res = irb.CreateSIToFP(loadOp(*srcOp, irb), irb.getDoubleTy());
			storeFp(*dstOp, res, irb);
			storeCcFp(res, irb);
			return;
		default:
			translatePseudoAsmGeneric(i, si, irb);
			return;
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpSqrt(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = (i->id == SYSZ_INS_SQEBR) ? irb.getFloatTy() : irb.getDoubleTy();
	op1 = loadFp(si->operands[1], irb, ty);
	auto* res = irb.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, op1);
	storeFp(si->operands[0], res, irb);
	storeCcFp(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpFma(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, si, irb);
	bool isS = (i->id == SYSZ_INS_MAEBR || i->id == SYSZ_INS_MAEB);
	auto* ty = isS ? irb.getFloatTy() : irb.getDoubleTy();
	auto* acc = loadFp(si->operands[0], irb, ty);
	auto* a = loadFp(si->operands[1], irb, ty);
	auto* b = loadFp(si->operands[2], irb, ty);
	auto* fma = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fma, ty);
	auto* res = irb.CreateCall(fma, {a, b, acc});
	storeFp(si->operands[0], res, irb);
	storeCcFp(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateCds(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, si, irb);
	if (si->operands[2].type != SYSZ_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned i1 = gprIndex(si->operands[0].reg);
	unsigned i3 = gprIndex(si->operands[1].reg);
	uint32_t e1 = gpr64(i1);
	uint32_t o1 = gpr64(i1 + 1);
	uint32_t e3 = gpr64(i3);
	uint32_t o3 = gpr64(i3 + 1);
	bool is128 = (i->id == SYSZ_INS_CDSG);
	auto* elem = is128 ? irb.getInt128Ty() : irb.getInt64Ty();
	llvm::Value* expected = nullptr;
	llvm::Value* desired = nullptr;
	if (is128)
	{
		expected = irb.CreateOr(
				irb.CreateShl(irb.CreateZExt(loadRegister(e1, irb), elem), 64),
				irb.CreateZExt(loadRegister(o1, irb), elem));
		desired = irb.CreateOr(
				irb.CreateShl(irb.CreateZExt(loadRegister(e3, irb), elem), 64),
				irb.CreateZExt(loadRegister(o3, irb), elem));
	}
	else
	{
		expected = irb.CreateOr(
				irb.CreateShl(irb.CreateZExt(extractLow32(loadRegister(e1, irb), irb), elem), 32),
				irb.CreateZExt(extractLow32(loadRegister(o1, irb), irb), elem));
		desired = irb.CreateOr(
				irb.CreateShl(irb.CreateZExt(extractLow32(loadRegister(e3, irb), irb), elem), 32),
				irb.CreateZExt(extractLow32(loadRegister(o3, irb), irb), elem));
	}
	auto* addr = generateMemAddress(si->operands[2], irb);
	auto* ptr = intToPtr(irb, addr, elem);
	auto* cx = irb.CreateAtomicCmpXchg(
			ptr,
			expected,
			desired,
			llvm::MaybeAlign(),
			llvm::AtomicOrdering::SequentiallyConsistent,
			llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(cx, elem);
	auto* old = irb.CreateExtractValue(cx, 0);
	auto* succ = irb.CreateExtractValue(cx, 1);
	if (is128)
	{
		storeRegister(e1, irb.CreateTrunc(irb.CreateLShr(old, 64), irb.getInt64Ty()), irb);
		storeRegister(o1, irb.CreateTrunc(old, irb.getInt64Ty()), irb);
	}
	else
	{
		depositLow32(e1, irb.CreateTrunc(irb.CreateLShr(old, 32), irb.getInt32Ty()), irb);
		depositLow32(o1, irb.CreateTrunc(old, irb.getInt32Ty()), irb);
	}
	storeRegister(
			SYSZ_REG_CC,
			irb.CreateSelect(
					succ,
					llvm::ConstantInt::get(irb.getInt8Ty(), 0),
					llvm::ConstantInt::get(irb.getInt8Ty(), 1)),
			irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorMul(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned laneBits = vectorLaneBits(i, si);
	if (laneBits == 0 || laneBits > 64 || (128 % laneBits) != 0)
	{
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned lanes = 128 / laneBits;
	auto* w = irb.getInt128Ty();
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), lanes);
	auto* va = irb.CreateBitCast(loadOp(si->operands[1], irb, w), vecTy);
	auto* vb = irb.CreateBitCast(loadOp(si->operands[2], irb, w), vecTy);
	llvm::Value* vr = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_VMH:
		case SYSZ_INS_VMHB:
		case SYSZ_INS_VMHH:
		case SYSZ_INS_VMHF:
		{
			auto* wideTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits * 2), lanes);
			auto* prod = irb.CreateMul(irb.CreateSExt(va, wideTy), irb.CreateSExt(vb, wideTy));
			vr = irb.CreateTrunc(irb.CreateLShr(prod, laneBits), vecTy);
			break;
		}
		default:
			vr = irb.CreateMul(va, vb);
			break;
	}
	storeOp(si->operands[0], irb.CreateBitCast(vr, w), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVperm(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 4)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	auto* w = irb.getInt128Ty();
	auto* a = loadOp(si->operands[1], irb, w);
	auto* b = loadOp(si->operands[2], irb, w);
	auto* c = loadOp(si->operands[3], irb, w);
	auto* ff = llvm::ConstantInt::get(w, 0xff);
	auto* thirtyOne = llvm::ConstantInt::get(w, 31);
	llvm::Value* res = llvm::ConstantInt::get(w, 0);
	for (unsigned lane = 0; lane < 16; ++lane)
	{
		unsigned shift = (15 - lane) * 8;
		auto* sh = llvm::ConstantInt::get(w, shift);
		auto* idx = irb.CreateAnd(irb.CreateLShr(c, sh), thirtyOne);
		auto* fromB = irb.CreateICmpUGE(idx, llvm::ConstantInt::get(w, 16));
		auto* srcIdx = irb.CreateSelect(
				fromB,
				irb.CreateSub(idx, llvm::ConstantInt::get(w, 16)),
				idx);
		auto* srcSh = irb.CreateMul(
				irb.CreateSub(llvm::ConstantInt::get(w, 15), srcIdx),
				llvm::ConstantInt::get(w, 8));
		auto* pickedA = irb.CreateAnd(irb.CreateLShr(a, srcSh), ff);
		auto* pickedB = irb.CreateAnd(irb.CreateLShr(b, srcSh), ff);
		auto* byte = irb.CreateSelect(fromB, pickedB, pickedA);
		res = irb.CreateOr(res, irb.CreateShl(byte, sh));
	}
	storeOp(si->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVpdi(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	auto* w = irb.getInt128Ty();
	auto* a = loadOp(si->operands[1], irb, w);
	auto* b = loadOp(si->operands[2], irb, w);
	unsigned m4 = lastImm(si, 0);
	auto* mask = llvm::ConstantInt::get(w, llvm::APInt::getHighBitsSet(128, 64));
	// I4 bit 2 (from the left of the 4-bit field) selects V2's second DW.
	llvm::Value* hi = ((m4 >> 1) & 1u)
			? irb.CreateShl(irb.CreateZExt(irb.CreateTrunc(a, irb.getInt64Ty()), w), 64)
			: irb.CreateAnd(a, mask);
	llvm::Value* lo = (m4 & 1u)
			? irb.CreateZExt(irb.CreateTrunc(b, irb.getInt64Ty()), w)
			: irb.CreateLShr(b, 64);
	storeOp(si->operands[0], irb.CreateOr(hi, lo), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVrep(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned bits = vectorLaneBits(i, si);
	if (bits == 0 || bits > 64 || (128 % bits) != 0)
	{
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	bool immSplat = (i->id == SYSZ_INS_VREPI
			|| i->id == SYSZ_INS_VREPIB
			|| i->id == SYSZ_INS_VREPIH
			|| i->id == SYSZ_INS_VREPIF
			|| i->id == SYSZ_INS_VREPIG);
	auto* elTy = irb.getIntNTy(bits);
	llvm::Value* el = nullptr;
	if (immSplat)
	{
		int64_t imm = 0;
		for (unsigned k = 1; k < si->op_count; ++k)
		{
			if (si->operands[k].type == SYSZ_OP_IMM)
			{
				imm = si->operands[k].imm;
				break;
			}
		}
		el = llvm::ConstantInt::getSigned(elTy, imm);
	}
	else
	{
		unsigned idx = 0;
		if (si->op_count >= 3 && si->operands[2].type == SYSZ_OP_IMM)
		{
			idx = static_cast<unsigned>(si->operands[2].imm);
		}
		auto* src = loadOp(si->operands[1], irb, irb.getInt128Ty());
		unsigned nElem = 128 / bits;
		if (idx >= nElem)
		{
			idx = nElem - 1;
		}
		unsigned shift = (nElem - 1 - idx) * bits;
		el = irb.CreateTrunc(irb.CreateLShr(src, shift), elTy);
	}
	auto* z = irb.CreateZExt(el, irb.getInt128Ty());
	llvm::Value* r = z;
	for (unsigned sh = bits; sh < 128; sh += bits)
	{
		r = irb.CreateOr(r, irb.CreateShl(z, sh));
	}
	storeOp(si->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVpk(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned srcBits = vectorLaneBits(i, si);
	if (srcBits < 16 || (srcBits % 2) != 0)
	{
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned dstBits = srcBits / 2;
	unsigned n = 128 / srcBits;
	auto* w = irb.getInt128Ty();
	auto* srcTy = irb.getIntNTy(srcBits);
	auto* dstTy = irb.getIntNTy(dstBits);
	auto* va = loadOp(si->operands[1], irb, w);
	auto* vb = loadOp(si->operands[2], irb, w);
	bool satU = (i->id == SYSZ_INS_VPKLS
			|| i->id == SYSZ_INS_VPKLSH
			|| i->id == SYSZ_INS_VPKLSF
			|| i->id == SYSZ_INS_VPKLSG);
	bool satS = (i->id == SYSZ_INS_VPKS
			|| i->id == SYSZ_INS_VPKSH
			|| i->id == SYSZ_INS_VPKSF
			|| i->id == SYSZ_INS_VPKSG);
	auto packHalf = [&](llvm::Value* src) -> llvm::Value* {
		llvm::Value* half = llvm::ConstantInt::get(w, 0);
		for (unsigned k = 0; k < n; ++k)
		{
			unsigned srcShift = (n - 1 - k) * srcBits;
			auto* el = irb.CreateTrunc(
					irb.CreateLShr(src, llvm::ConstantInt::get(w, srcShift)),
					srcTy);
			if (satU)
			{
				auto* maxv = llvm::ConstantInt::get(srcTy, (1ull << dstBits) - 1ull);
				el = irb.CreateSelect(irb.CreateICmpUGT(el, maxv), maxv, el);
			}
			else if (satS)
			{
				int64_t maxS = (1ll << (dstBits - 1)) - 1;
				int64_t minS = -(1ll << (dstBits - 1));
				auto* hi = llvm::ConstantInt::getSigned(srcTy, maxS);
				auto* lo = llvm::ConstantInt::getSigned(srcTy, minS);
				el = irb.CreateSelect(
						irb.CreateICmpSLT(el, lo),
						lo,
						irb.CreateSelect(irb.CreateICmpSGT(el, hi), hi, el));
			}
			auto* pk = irb.CreateZExt(irb.CreateTrunc(el, dstTy), w);
			unsigned dstShift = 64 + (n - 1 - k) * dstBits;
			half = irb.CreateOr(half, irb.CreateShl(pk, dstShift));
		}
		return half;
	};
	// IBM VPK: V1 = pack(V2) || pack(V3) (left / high half, then right / low).
	auto* vr = irb.CreateOr(packHalf(va), irb.CreateLShr(packHalf(vb), 64));
	storeOp(si->operands[0], vr, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFidbr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned srcIdx = si->op_count - 1;
	auto* ty = isFpSingleInsn(i->id) ? irb.getFloatTy() : irb.getDoubleTy();
	auto* src = loadFp(si->operands[srcIdx], irb, ty);
	unsigned m3 = 0;
	for (unsigned k = 1; k < srcIdx; ++k)
	{
		if (si->operands[k].type == SYSZ_OP_IMM)
		{
			m3 = static_cast<unsigned>(si->operands[k].imm);
			break;
		}
	}
	auto iid = (m3 == 5) ? llvm::Intrinsic::trunc : llvm::Intrinsic::nearbyint;
	auto* res = irb.CreateUnaryIntrinsic(iid, src);
	storeFp(si->operands[0], res, irb);
	storeCcFp(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoc(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	bool is64 = isLoc64(i->id);
	auto* elem = is64 ? irb.getInt64Ty() : irb.getInt32Ty();
	auto* dst = loadOp(si->operands[0], irb, elem);
	auto* src = loadOp(si->operands[1], irb, elem);
	if (src->getType() != elem)
	{
		src = irb.CreateSExtOrTrunc(src, elem);
	}
	if (dst->getType() != elem)
	{
		dst = irb.CreateSExtOrTrunc(dst, elem);
	}
	auto cc = conditionFromInsn(i, si);
	llvm::Value* cond = nullptr;
	if (cc != SYSTEMZ_CC_INVALID)
	{
		cond = generateCondition(cc, irb);
	}
	else
	{
		cond = generateCcMask(lastImm(si, 15), irb);
	}
	auto* res = irb.CreateSelect(cond, src, dst);
	if (is64)
	{
		storeOp(si->operands[0], res, irb);
	}
	else
	{
		depositLow32(si->operands[0].reg, res, irb);
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateRisbg(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 5)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	auto* i64 = irb.getInt64Ty();
	auto* dst = loadOp(si->operands[0], irb, i64);
	auto* src = loadOp(si->operands[1], irb, i64);
	unsigned i3 = static_cast<unsigned>(si->operands[2].imm);
	unsigned i4 = static_cast<unsigned>(si->operands[3].imm);
	unsigned i5 = static_cast<unsigned>(si->operands[4].imm) & 63u;
	llvm::Value* rotated = src;
	if (i5 != 0)
	{
		rotated = irb.CreateOr(
				irb.CreateShl(src, i5),
				irb.CreateLShr(src, 64u - i5));
	}
	unsigned start = i3 & 63u;
	unsigned end = i4 & 63u;
	bool zeroRest = (i4 & 0x80u) != 0;
	uint64_t mask = 0;
	auto setBit = [&](unsigned be) {
		mask |= 1ull << (63u - (be & 63u));
	};
	if (start <= end)
	{
		for (unsigned k = start; k <= end; ++k)
		{
			setBit(k);
		}
	}
	else
	{
		for (unsigned k = start; k < 64; ++k)
		{
			setBit(k);
		}
		for (unsigned k = 0; k <= end; ++k)
		{
			setBit(k);
		}
	}
	auto* m = llvm::ConstantInt::get(i64, mask);
	auto* inserted = irb.CreateAnd(rotated, m);
	llvm::Value* res = zeroRest
			? inserted
			: irb.CreateOr(irb.CreateAnd(dst, irb.CreateNot(m)), inserted);
	storeOp(si->operands[0], res, irb);
	if (i->id == SYSZ_INS_RISBG)
	{
		storeCcSigned(res, irb.getFalse(), irb);
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLaa(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, si, irb);
	if (si->operands[2].type != SYSZ_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	bool is64 = (i->id == SYSZ_INS_LAAG
			|| i->id == SYSZ_INS_LANG
			|| i->id == SYSZ_INS_LAOG
			|| i->id == SYSZ_INS_LAXG);
	auto* elem = is64 ? irb.getInt64Ty() : irb.getInt32Ty();
	auto* rhs = loadOp(si->operands[1], irb, elem);
	auto* addr = generateMemAddress(si->operands[2], irb);
	auto* ptr = intToPtr(irb, addr, elem);
	llvm::AtomicRMWInst::BinOp bop = llvm::AtomicRMWInst::Add;
	switch (i->id)
	{
		case SYSZ_INS_LAN:
		case SYSZ_INS_LANG:
			bop = llvm::AtomicRMWInst::And;
			break;
		case SYSZ_INS_LAO:
		case SYSZ_INS_LAOG:
			bop = llvm::AtomicRMWInst::Or;
			break;
		case SYSZ_INS_LAX:
		case SYSZ_INS_LAXG:
			bop = llvm::AtomicRMWInst::Xor;
			break;
		default:
			break;
	}
	auto* old = irb.CreateAtomicRMW(
			bop,
			ptr,
			rhs,
			llvm::MaybeAlign(),
			llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(old, elem);
	llvm::Value* result = nullptr;
	if (bop == llvm::AtomicRMWInst::And)
	{
		result = irb.CreateAnd(old, rhs);
	}
	else if (bop == llvm::AtomicRMWInst::Or)
	{
		result = irb.CreateOr(old, rhs);
	}
	else if (bop == llvm::AtomicRMWInst::Xor)
	{
		result = irb.CreateXor(old, rhs);
	}
	else
	{
		result = irb.CreateAdd(old, rhs);
	}
	if (is64)
	{
		storeOp(si->operands[0], old, irb);
	}
	else
	{
		depositLow32(si->operands[0].reg, old, irb);
	}
	if (bop == llvm::AtomicRMWInst::Add)
	{
		storeCcSigned(result, generateOverflowAdd(result, old, rhs, irb), irb);
	}
	else
	{
		storeCcLogical(result, irb);
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVsel(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 4)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	auto* w = irb.getInt128Ty();
	auto* a = loadOp(si->operands[1], irb, w);
	auto* b = loadOp(si->operands[2], irb, w);
	auto* m = loadOp(si->operands[3], irb, w);
	auto* res = irb.CreateOr(irb.CreateAnd(a, irb.CreateNot(m)), irb.CreateAnd(b, m));
	storeOp(si->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorShift(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned laneBits = vectorLaneBits(i, si);
	if (laneBits == 0 || laneBits > 64 || (128 % laneBits) != 0)
	{
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned lanes = 128 / laneBits;
	auto* w = irb.getInt128Ty();
	auto* elemTy = irb.getIntNTy(laneBits);
	auto* vecTy = llvm::FixedVectorType::get(elemTy, lanes);
	auto* va = irb.CreateBitCast(loadOp(si->operands[1], irb, w), vecTy);
	bool left = false;
	bool arithmetic = false;
	switch (i->id)
	{
		case SYSZ_INS_VESL:
		case SYSZ_INS_VESLB:
		case SYSZ_INS_VESLH:
		case SYSZ_INS_VESLF:
		case SYSZ_INS_VESLG:
		case SYSZ_INS_VESLV:
		case SYSZ_INS_VESLVB:
		case SYSZ_INS_VESLVH:
		case SYSZ_INS_VESLVF:
		case SYSZ_INS_VESLVG:
			left = true;
			break;
		case SYSZ_INS_VESRA:
		case SYSZ_INS_VESRAB:
		case SYSZ_INS_VESRAH:
		case SYSZ_INS_VESRAF:
		case SYSZ_INS_VESRAG:
		case SYSZ_INS_VESRAV:
		case SYSZ_INS_VESRAVB:
		case SYSZ_INS_VESRAVH:
		case SYSZ_INS_VESRAVF:
		case SYSZ_INS_VESRAVG:
			arithmetic = true;
			break;
		case SYSZ_INS_VESRL:
		case SYSZ_INS_VESRLB:
		case SYSZ_INS_VESRLH:
		case SYSZ_INS_VESRLF:
		case SYSZ_INS_VESRLG:
		case SYSZ_INS_VESRLV:
		case SYSZ_INS_VESRLVB:
		case SYSZ_INS_VESRLVH:
		case SYSZ_INS_VESRLVF:
		case SYSZ_INS_VESRLVG:
			break;
		default:
			translatePseudoAsmGeneric(i, si, irb);
			return;
	}
	auto* width = llvm::ConstantInt::get(elemTy, laneBits);
	auto* widthM1 = llvm::ConstantInt::get(elemTy, laneBits - 1);
	llvm::Value* amt = nullptr;
	llvm::Value* tooBig = nullptr;
	if (si->operands[2].type == SYSZ_OP_REG)
	{
		auto* vb = irb.CreateBitCast(loadOp(si->operands[2], irb, w), vecTy);
		tooBig = irb.CreateICmpUGE(vb, irb.CreateVectorSplat(lanes, width));
		amt = irb.CreateSelect(
				tooBig,
				irb.CreateVectorSplat(lanes, widthM1),
				vb);
	}
	else
	{
		auto* raw = loadOp(si->operands[2], irb, nullptr, /*lea=*/true);
		raw = irb.CreateZExtOrTrunc(raw, irb.getInt64Ty());
		auto* scalarTooBig = irb.CreateICmpUGE(
				raw,
				llvm::ConstantInt::get(irb.getInt64Ty(), laneBits));
		auto* safe = irb.CreateSelect(
				scalarTooBig,
				llvm::ConstantInt::get(irb.getInt64Ty(), laneBits - 1),
				raw);
		amt = irb.CreateVectorSplat(lanes, irb.CreateTrunc(safe, elemTy));
		tooBig = irb.CreateVectorSplat(lanes, scalarTooBig);
	}
	llvm::Value* vr = nullptr;
	if (left)
	{
		vr = irb.CreateShl(va, amt);
	}
	else if (arithmetic)
	{
		vr = irb.CreateAShr(va, amt);
	}
	else
	{
		vr = irb.CreateLShr(va, amt);
	}
	if (!arithmetic)
	{
		vr = irb.CreateSelect(tooBig, llvm::Constant::getNullValue(vecTy), vr);
	}
	storeOp(si->operands[0], irb.CreateBitCast(vr, w), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorUnary(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned laneBits = vectorLaneBits(i, si);
	if (laneBits == 0 || laneBits > 64 || (128 % laneBits) != 0)
	{
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned lanes = 128 / laneBits;
	auto* w = irb.getInt128Ty();
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), lanes);
	auto* va = irb.CreateBitCast(loadOp(si->operands[1], irb, w), vecTy);
	llvm::Value* vr = irb.CreateNeg(va);
	switch (i->id)
	{
		case SYSZ_INS_VLC:
		case SYSZ_INS_VLCB:
		case SYSZ_INS_VLCH:
		case SYSZ_INS_VLCF:
		case SYSZ_INS_VLCG:
			break;
		case SYSZ_INS_VLP:
		case SYSZ_INS_VLPB:
		case SYSZ_INS_VLPH:
		case SYSZ_INS_VLPF:
		case SYSZ_INS_VLPG:
			vr = irb.CreateSelect(
					irb.CreateICmpSLT(va, llvm::Constant::getNullValue(vecTy)),
					vr,
					va);
			break;
		default:
			translatePseudoAsmGeneric(i, si, irb);
			return;
	}
	storeOp(si->operands[0], irb.CreateBitCast(vr, w), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVllez(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2 || si->operands[1].type != SYSZ_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned bits = 8;
	switch (i->id)
	{
		case SYSZ_INS_VLLEZB:
			bits = 8;
			break;
		case SYSZ_INS_VLLEZH:
			bits = 16;
			break;
		case SYSZ_INS_VLLEZF:
		case SYSZ_INS_VLLEZLF:
			bits = 32;
			break;
		case SYSZ_INS_VLLEZG:
			bits = 64;
			break;
		case SYSZ_INS_VLLEZ:
		{
			static const unsigned kM3[7] = {8, 16, 32, 64, 8, 8, 32};
			auto m = lastImm(si, 0);
			bits = m < 7 ? kM3[m] : 8;
			break;
		}
		default:
			break;
	}
	auto* el = loadOp(si->operands[1], irb, irb.getIntNTy(bits));
	auto* z = irb.CreateZExt(el, irb.getInt128Ty());
	auto* placed = irb.CreateShl(z, llvm::ConstantInt::get(irb.getInt128Ty(), 128u - bits));
	storeOp(si->operands[0], placed, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVupk(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned srcBits = 8;
	bool highIndex = true;
	bool logical = false;
	switch (i->id)
	{
		case SYSZ_INS_VUPHB:
			srcBits = 8;
			break;
		case SYSZ_INS_VUPHH:
			srcBits = 16;
			break;
		case SYSZ_INS_VUPHF:
			srcBits = 32;
			break;
		case SYSZ_INS_VUPLB:
			srcBits = 8;
			highIndex = false;
			break;
		case SYSZ_INS_VUPLHW:
			srcBits = 16;
			highIndex = false;
			break;
		case SYSZ_INS_VUPLF:
			srcBits = 32;
			highIndex = false;
			break;
		case SYSZ_INS_VUPLHB:
			srcBits = 8;
			logical = true;
			break;
		case SYSZ_INS_VUPLHH:
			srcBits = 16;
			logical = true;
			break;
		case SYSZ_INS_VUPLHF:
			srcBits = 32;
			logical = true;
			break;
		case SYSZ_INS_VUPLLB:
			srcBits = 8;
			highIndex = false;
			logical = true;
			break;
		case SYSZ_INS_VUPLLH:
			srcBits = 16;
			highIndex = false;
			logical = true;
			break;
		case SYSZ_INS_VUPLLF:
			srcBits = 32;
			highIndex = false;
			logical = true;
			break;
		case SYSZ_INS_VUPL:
			highIndex = false;
			srcBits = 8;
			{
				static const unsigned kM4[3] = {8, 16, 32};
				auto m = lastImm(si, 0);
				srcBits = m < 3 ? kM4[m] : 8;
			}
			break;
		case SYSZ_INS_VUPLH:
			logical = true;
			{
				static const unsigned kM4[3] = {8, 16, 32};
				auto m = lastImm(si, 0);
				srcBits = m < 3 ? kM4[m] : 8;
			}
			break;
		case SYSZ_INS_VUPLL:
			highIndex = false;
			logical = true;
			{
				static const unsigned kM4[3] = {8, 16, 32};
				auto m = lastImm(si, 0);
				srcBits = m < 3 ? kM4[m] : 8;
			}
			break;
		case SYSZ_INS_VUPH:
		default:
		{
			static const unsigned kM4[3] = {8, 16, 32};
			auto m = lastImm(si, 0);
			srcBits = m < 3 ? kM4[m] : 8;
			break;
		}
	}
	unsigned dstBits = srcBits * 2;
	if (srcBits == 0 || dstBits > 64 || (128 % dstBits) != 0)
	{
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned n = 128 / dstBits;
	auto* w = irb.getInt128Ty();
	auto* srcTy = irb.getIntNTy(srcBits);
	auto* dstTy = irb.getIntNTy(dstBits);
	auto* src = loadOp(si->operands[1], irb, w);
	llvm::Value* res = llvm::ConstantInt::get(w, 0);
	for (unsigned k = 0; k < n; ++k)
	{
		unsigned srcIdx = (highIndex ? n : 0) + k;
		unsigned srcShift = ((128 / srcBits) - 1 - srcIdx) * srcBits;
		auto* el = irb.CreateTrunc(
				irb.CreateLShr(src, llvm::ConstantInt::get(w, srcShift)),
				srcTy);
		auto* ext = logical
				? irb.CreateZExt(el, dstTy)
				: irb.CreateSExt(el, dstTy);
		unsigned dstShift = (n - 1 - k) * dstBits;
		res = irb.CreateOr(res, irb.CreateShl(irb.CreateZExt(ext, w), dstShift));
	}
	storeOp(si->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVseg(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned srcBits = 8;
	switch (i->id)
	{
		case SYSZ_INS_VSEGB:
			srcBits = 8;
			break;
		case SYSZ_INS_VSEGH:
			srcBits = 16;
			break;
		case SYSZ_INS_VSEGF:
			srcBits = 32;
			break;
		case SYSZ_INS_VSEG:
		{
			static const unsigned kM3[3] = {8, 16, 32};
			auto m = lastImm(si, 0);
			srcBits = m < 3 ? kM3[m] : 8;
			break;
		}
		default:
			break;
	}
	auto* w = irb.getInt128Ty();
	auto* src = loadOp(si->operands[1], irb, w);
	auto* el = irb.CreateTrunc(src, irb.getIntNTy(srcBits));
	auto* dw = irb.CreateSExt(el, irb.getInt64Ty());
	auto* z = irb.CreateZExt(dw, w);
	auto* res = irb.CreateOr(z, irb.CreateShl(z, 64));
	storeOp(si->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVgef(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2 || si->operands[1].type != SYSZ_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	bool scatter = (i->id == SYSZ_INS_VSCEF || i->id == SYSZ_INS_VSCEG);
	unsigned elemBits = (i->id == SYSZ_INS_VGEG || i->id == SYSZ_INS_VSCEG) ? 64u : 32u;
	unsigned idx = lastImm(si, 0);
	unsigned nElem = 128 / elemBits;
	if (idx >= nElem)
	{
		idx = nElem - 1;
	}
	unsigned shift = (nElem - 1 - idx) * elemBits;
	auto* t = getDefaultType();
	llvm::Value* addr = llvm::ConstantInt::getSigned(t, si->operands[1].mem.disp);
	auto* baseR = loadAddrReg(si->operands[1].mem.base, irb);
	if (baseR != nullptr)
	{
		addr = irb.CreateAdd(baseR, irb.CreateSExtOrTrunc(addr, baseR->getType()));
	}
	uint32_t idxReg = si->operands[1].mem.index;
	if (idxReg != SYSZ_REG_INVALID)
	{
		auto* idxVr = loadRegister(idxReg, irb);
		if (idxVr != nullptr)
		{
			auto* w = irb.getInt128Ty();
			if (idxVr->getType() != w)
			{
				idxVr = irb.CreateZExtOrTrunc(idxVr, w);
			}
			auto* elTy = irb.getIntNTy(elemBits);
			auto* indexEl = irb.CreateTrunc(
					irb.CreateLShr(idxVr, llvm::ConstantInt::get(w, shift)),
					elTy);
			addr = irb.CreateAdd(addr, irb.CreateSExtOrTrunc(indexEl, addr->getType()));
		}
	}
	auto* elTy = irb.getIntNTy(elemBits);
	if (scatter)
	{
		auto* dest = loadOp(si->operands[0], irb, irb.getInt128Ty());
		auto* el = irb.CreateTrunc(
				irb.CreateLShr(dest, llvm::ConstantInt::get(irb.getInt128Ty(), shift)),
				elTy);
		storeIntPtr(irb, el, addr, elTy);
		return;
	}
	auto* loaded = loadIntPtr(irb, addr, elTy);
	auto* dest = loadOp(si->operands[0], irb, irb.getInt128Ty());
	llvm::APInt ones = llvm::APInt::getLowBitsSet(128, elemBits).shl(shift);
	auto* mask = llvm::ConstantInt::get(irb.getInt128Ty(), ones);
	auto* cleared = irb.CreateAnd(dest, irb.CreateNot(mask));
	auto* placed = irb.CreateShl(
			irb.CreateZExt(loaded, irb.getInt128Ty()),
			llvm::ConstantInt::get(irb.getInt128Ty(), shift));
	storeOp(si->operands[0], irb.CreateOr(cleared, placed), irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVgbm(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 1)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	llvm::APInt bits(128, 0);
	if (i->id == SYSZ_INS_VONE)
	{
		bits.setAllBits();
	}
	else if (i->id != SYSZ_INS_VZERO)
	{
		if (si->op_count < 2 || si->operands[1].type != SYSZ_OP_IMM)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, si, irb);
			return;
		}
		unsigned mask = static_cast<unsigned>(si->operands[1].imm) & 0xFFFFu;
		for (unsigned b = 0; b < 16; ++b)
		{
			if (mask & (1u << (15 - b)))
			{
				bits.insertBits(0xFF, (15 - b) * 8, 8);
			}
		}
	}
	storeOp(si->operands[0], llvm::ConstantInt::get(irb.getInt128Ty(), bits), irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVgm(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3
			|| si->operands[1].type != SYSZ_OP_IMM
			|| si->operands[2].type != SYSZ_OP_IMM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned elemBits = 8;
	switch (i->id)
	{
		case SYSZ_INS_VGMH:
			elemBits = 16;
			break;
		case SYSZ_INS_VGMF:
			elemBits = 32;
			break;
		case SYSZ_INS_VGMG:
			elemBits = 64;
			break;
		case SYSZ_INS_VGM:
			if (si->op_count >= 4 && si->operands[3].type == SYSZ_OP_IMM)
			{
				static const unsigned kM4[4] = {8, 16, 32, 64};
				auto m = static_cast<unsigned>(si->operands[3].imm);
				elemBits = m < 4 ? kM4[m] : 8;
			}
			break;
		default:
			break;
	}
	if (elemBits == 0 || elemBits > 64 || (128 % elemBits) != 0)
	{
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned start = static_cast<unsigned>(si->operands[1].imm) % elemBits;
	unsigned end = static_cast<unsigned>(si->operands[2].imm) % elemBits;
	llvm::APInt elem(elemBits, 0);
	for (unsigned bit = 0; bit < elemBits; ++bit)
	{
		bool set = (start <= end)
				? (bit >= start && bit <= end)
				: (bit >= start || bit <= end);
		if (set)
		{
			elem.setBit(elemBits - 1 - bit);
		}
	}
	storeOp(si->operands[0],
			llvm::ConstantInt::get(irb.getInt128Ty(), llvm::APInt::getSplat(128, elem)),
			irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVlei(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3
			|| si->operands[1].type != SYSZ_OP_IMM
			|| si->operands[2].type != SYSZ_OP_IMM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	unsigned elemBits = 8;
	switch (i->id)
	{
		case SYSZ_INS_VLEIH:
			elemBits = 16;
			break;
		case SYSZ_INS_VLEIF:
			elemBits = 32;
			break;
		case SYSZ_INS_VLEIG:
			elemBits = 64;
			break;
		default:
			break;
	}
	unsigned nElem = 128 / elemBits;
	unsigned idx = static_cast<unsigned>(si->operands[2].imm);
	if (idx >= nElem)
	{
		idx = nElem - 1;
	}
	unsigned shift = (nElem - 1 - idx) * elemBits;
	uint16_t i2 = static_cast<uint16_t>(si->operands[1].imm);
	llvm::APInt el(elemBits, 0);
	if (elemBits == 8)
	{
		el = llvm::APInt(8, i2 & 0xFFu);
	}
	else if (elemBits == 16)
	{
		el = llvm::APInt(16, i2);
	}
	else
	{
		el = llvm::APInt(16, i2, true).sext(elemBits);
	}
	auto* vec = loadOp(si->operands[0], irb, irb.getInt128Ty());
	llvm::APInt ones = llvm::APInt::getLowBitsSet(128, elemBits).shl(shift);
	auto* mask = llvm::ConstantInt::get(irb.getInt128Ty(), ones);
	auto* cleared = irb.CreateAnd(vec, irb.CreateNot(mask));
	auto* placed = irb.CreateShl(
			irb.CreateZExt(llvm::ConstantInt::get(irb.getIntNTy(elemBits), el),
					irb.getInt128Ty()),
			llvm::ConstantInt::get(irb.getInt128Ty(), shift));
	storeOp(si->operands[0], irb.CreateOr(cleared, placed), irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVfee(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	bool ne = false;
	bool zs = false;
	bool cs = false;
	unsigned elemBits = 8;
	switch (i->id)
	{
		case SYSZ_INS_VFENE:
		case SYSZ_INS_VFENEB:
		case SYSZ_INS_VFENEBS:
		case SYSZ_INS_VFENEH:
		case SYSZ_INS_VFENEHS:
		case SYSZ_INS_VFENEF:
		case SYSZ_INS_VFENEFS:
		case SYSZ_INS_VFENEZB:
		case SYSZ_INS_VFENEZBS:
		case SYSZ_INS_VFENEZH:
		case SYSZ_INS_VFENEZHS:
		case SYSZ_INS_VFENEZF:
		case SYSZ_INS_VFENEZFS:
			ne = true;
			break;
		default:
			break;
	}
	switch (i->id)
	{
		case SYSZ_INS_VFEEH:
		case SYSZ_INS_VFEEHS:
		case SYSZ_INS_VFEEZH:
		case SYSZ_INS_VFEEZHS:
		case SYSZ_INS_VFENEH:
		case SYSZ_INS_VFENEHS:
		case SYSZ_INS_VFENEZH:
		case SYSZ_INS_VFENEZHS:
			elemBits = 16;
			break;
		case SYSZ_INS_VFEEF:
		case SYSZ_INS_VFEEFS:
		case SYSZ_INS_VFEEZF:
		case SYSZ_INS_VFEEZFS:
		case SYSZ_INS_VFENEF:
		case SYSZ_INS_VFENEFS:
		case SYSZ_INS_VFENEZF:
		case SYSZ_INS_VFENEZFS:
			elemBits = 32;
			break;
		default:
			break;
	}
	switch (i->id)
	{
		case SYSZ_INS_VFEEZB:
		case SYSZ_INS_VFEEZBS:
		case SYSZ_INS_VFEEZH:
		case SYSZ_INS_VFEEZHS:
		case SYSZ_INS_VFEEZF:
		case SYSZ_INS_VFEEZFS:
		case SYSZ_INS_VFENEZB:
		case SYSZ_INS_VFENEZBS:
		case SYSZ_INS_VFENEZH:
		case SYSZ_INS_VFENEZHS:
		case SYSZ_INS_VFENEZF:
		case SYSZ_INS_VFENEZFS:
			zs = true;
			break;
		default:
			break;
	}
	switch (i->id)
	{
		case SYSZ_INS_VFEEBS:
		case SYSZ_INS_VFEEHS:
		case SYSZ_INS_VFEEFS:
		case SYSZ_INS_VFEEZBS:
		case SYSZ_INS_VFEEZHS:
		case SYSZ_INS_VFEEZFS:
		case SYSZ_INS_VFENEBS:
		case SYSZ_INS_VFENEHS:
		case SYSZ_INS_VFENEFS:
		case SYSZ_INS_VFENEZBS:
		case SYSZ_INS_VFENEZHS:
		case SYSZ_INS_VFENEZFS:
			cs = true;
			break;
		default:
			break;
	}
	if (si->op_count >= 4 && si->operands[3].type == SYSZ_OP_IMM)
	{
		unsigned m5 = static_cast<unsigned>(si->operands[3].imm);
		if (m5 & 1u)
		{
			cs = true;
		}
		if (m5 & 2u)
		{
			zs = true;
		}
	}
	unsigned nElem = 128 / elemBits;
	auto* w = irb.getInt128Ty();
	auto* elTy = irb.getIntNTy(elemBits);
	auto* a = loadOp(si->operands[1], irb, w);
	auto* b = loadOp(si->operands[2], irb, w);
	llvm::Value* eqIdx = irb.getInt32(16);
	llvm::Value* zIdx = irb.getInt32(16);
	llvm::Value* lookingEq = irb.getTrue();
	llvm::Value* lookingZ = irb.getTrue();
	auto* zeroEl = llvm::ConstantInt::get(elTy, 0);
	for (unsigned idx = 0; idx < nElem; ++idx)
	{
		unsigned shift = (nElem - 1 - idx) * elemBits;
		auto* ea = irb.CreateTrunc(
				irb.CreateLShr(a, llvm::ConstantInt::get(w, shift)), elTy);
		auto* eb = irb.CreateTrunc(
				irb.CreateLShr(b, llvm::ConstantInt::get(w, shift)), elTy);
		auto* hit = ne ? irb.CreateICmpNE(ea, eb) : irb.CreateICmpEQ(ea, eb);
		auto* thisIdx = irb.getInt32(idx * (elemBits / 8));
		eqIdx = irb.CreateSelect(irb.CreateAnd(lookingEq, hit), thisIdx, eqIdx);
		lookingEq = irb.CreateAnd(lookingEq, irb.CreateNot(hit));
		if (zs)
		{
			auto* isZ = irb.CreateICmpEQ(ea, zeroEl);
			zIdx = irb.CreateSelect(irb.CreateAnd(lookingZ, isZ), thisIdx, zIdx);
			lookingZ = irb.CreateAnd(lookingZ, irb.CreateNot(isZ));
		}
	}
	llvm::Value* resIdx = zs
			? irb.CreateSelect(irb.CreateICmpULT(eqIdx, zIdx), eqIdx, zIdx)
			: eqIdx;
	auto* placed = irb.CreateShl(
			irb.CreateZExt(resIdx, w),
			llvm::ConstantInt::get(w, 64));
	storeOp(si->operands[0], placed, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	if (cs)
	{
		llvm::Value* cc = nullptr;
		if (!zs)
		{
			cc = irb.CreateSelect(
					irb.CreateICmpEQ(eqIdx, irb.getInt32(16)),
					irb.getInt8(0),
					irb.getInt8(1));
		}
		else
		{
			auto* none = irb.CreateAnd(
					irb.CreateICmpEQ(eqIdx, irb.getInt32(16)),
					irb.CreateICmpEQ(zIdx, irb.getInt32(16)));
			auto* eqFirst = irb.CreateICmpULT(eqIdx, zIdx);
			auto* zFirst = irb.CreateICmpULT(zIdx, eqIdx);
			cc = irb.CreateSelect(
					none,
					irb.getInt8(0),
					irb.CreateSelect(
							eqFirst,
							irb.getInt8(1),
							irb.CreateSelect(zFirst, irb.getInt8(2), irb.getInt8(3))));
		}
		storeRegister(SYSZ_REG_CC, cc, irb);
	}
}

} // namespace capstone2llvmir
} // namespace retdec
