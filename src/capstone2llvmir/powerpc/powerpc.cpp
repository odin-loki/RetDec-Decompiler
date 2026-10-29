/**
 * @file src/capstone2llvmir/powerpc/powerpc.cpp
 * @brief PowerPC implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <functional>
#include <cstring>
#include <climits>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <set>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsPowerPC.h>

#include "capstone2llvmir/powerpc/powerpc_impl.h"

namespace retdec {
namespace capstone2llvmir {

namespace {

bool ppcIsId(const cs_insn* i, unsigned id)
{
	return i->id == id || (i->is_alias && static_cast<unsigned>(i->alias_id) == id);
}

// Capstone 6 groups CR bit registers by bit name (EQ, then GT, then LT, then
// UN), not by field. A `bf`/`bt` operand is one of those bits; the branch
// condition is on the whole field (CR0..CR7).
uint32_t ppcCrField(uint32_t reg)
{
	if (reg >= PPC_REG_CR0 && reg <= PPC_REG_CR7)
	{
		return reg;
	}
	if (reg >= PPC_REG_CR0EQ && reg <= PPC_REG_CR7EQ)
	{
		return static_cast<uint32_t>(PPC_REG_CR0 + (reg - PPC_REG_CR0EQ));
	}
	if (reg >= PPC_REG_CR0GT && reg <= PPC_REG_CR7GT)
	{
		return static_cast<uint32_t>(PPC_REG_CR0 + (reg - PPC_REG_CR0GT));
	}
	if (reg >= PPC_REG_CR0LT && reg <= PPC_REG_CR7LT)
	{
		return static_cast<uint32_t>(PPC_REG_CR0 + (reg - PPC_REG_CR0LT));
	}
	if (reg >= PPC_REG_CR0UN && reg <= PPC_REG_CR7UN)
	{
		return static_cast<uint32_t>(PPC_REG_CR0 + (reg - PPC_REG_CR0UN));
	}
	return reg;
}

// bdnzf/bdzf name one CR bit (CR4LT). Mapping that bit onto the whole field
// makes the F form load GT|EQ. Keep the bit when the branch also tests CTR.
uint32_t ppcBranchCr(uint32_t reg, bool singleBit)
{
	return singleBit ? reg : ppcCrField(reg);
}

bool ppcIsSldi(const cs_insn* i)
{
	return ppcIsId(i, PPC_INS_SLDI) || ppcIsId(i, PPC_INS_ALIAS_SLDI);
}

bool ppcIsRotldi(const cs_insn* i)
{
	return ppcIsId(i, PPC_INS_ROTLDI) || ppcIsId(i, PPC_INS_ALIAS_ROTLDI) || ppcIsId(i, PPC_INS_ALIAS_ROTLDI_);
}

bool ppcIsClrldi(const cs_insn* i)
{
	return ppcIsId(i, PPC_INS_CLRLDI) || ppcIsId(i, PPC_INS_ALIAS_CLRLDI) || ppcIsId(i, PPC_INS_ALIAS_CLRLDI_);
}

// Capstone 6 prints `mtocrf 8, r3` / `mfocrf r3, 8` but the FXM slot is a CR
// field register (PPC_REG_CR0 + f), not PPC_OP_IMM. Bit 0x80 still selects CR0.
bool ppcFxmMask(const cs_ppc_op& op, uint64_t& mask)
{
	if (op.type == PPC_OP_IMM)
	{
		mask = static_cast<uint64_t>(op.imm);
		return true;
	}
	if (op.type == PPC_OP_REG && op.reg >= PPC_REG_CR0 && op.reg <= PPC_REG_CR7)
	{
		mask = 0x80u >> static_cast<unsigned>(op.reg - PPC_REG_CR0);
		return true;
	}
	return false;
}

unsigned qpxSlotShift(unsigned lane)
{
	return (3u - lane) * 64u;
}

llvm::Value* qpxGetSlot(llvm::Value* bits, unsigned lane, llvm::IRBuilder<>& irb)
{
	return irb.CreateTrunc(irb.CreateLShr(bits, qpxSlotShift(lane)), irb.getInt64Ty());
}

llvm::Value* qpxPutSlot(llvm::Value* acc, llvm::Value* slot, unsigned lane, llvm::IRBuilder<>& irb)
{
	return irb.CreateOr(acc, irb.CreateShl(irb.CreateZExt(slot, acc->getType()), qpxSlotShift(lane)));
}

llvm::Value* qpxCrossI256(llvm::Value* bits, llvm::IRBuilder<>& irb)
{
	auto* sh = llvm::ConstantInt::get(bits->getType(), 128);
	return irb.CreateOr(irb.CreateShl(bits, sh), irb.CreateLShr(bits, sh));
}

llvm::Value* qpxCpnI256(llvm::Value* bits, llvm::IRBuilder<>& irb)
{
	const uint64_t w[4] = {0x8000000000000000ULL, 0, 0x8000000000000000ULL, 0};
	auto* mask = llvm::ConstantInt::get(bits->getType(), llvm::APInt(256, llvm::ArrayRef<uint64_t>(w, 4)));
	return irb.CreateXor(bits, mask);
}

unsigned qpxLogicalTt(const cs_insn* i, const cs_ppc* pi)
{
	if (pi->op_count >= 4 && pi->operands[3].type == PPC_OP_IMM)
	{
		return static_cast<unsigned>(pi->operands[3].imm) & 15u;
	}
	unsigned id = i->is_alias ? static_cast<unsigned>(i->alias_id) : i->id;
	switch (id)
	{
	case PPC_INS_QVFCLR: return 0;
	case PPC_INS_QVFAND: return 1;
	case PPC_INS_QVFANDC: return 4;
	case PPC_INS_QVFCTFB: return 5;
	case PPC_INS_QVFXOR: return 6;
	case PPC_INS_QVFOR: return 7;
	case PPC_INS_QVFNOR: return 8;
	case PPC_INS_QVFEQU: return 9;
	case PPC_INS_QVFNOT: return 10;
	case PPC_INS_QVFORC: return 13;
	case PPC_INS_QVFNAND: return 14;
	case PPC_INS_QVFSET: return 15;
	default: return 0;
	}
}

bool qpxIsUpdateForm(unsigned id)
{
	switch (id)
	{
	case PPC_INS_QVLFCDUX:
	case PPC_INS_QVLFCDUXA:
	case PPC_INS_QVLFCSUX:
	case PPC_INS_QVLFCSUXA:
	case PPC_INS_QVLFDUX:
	case PPC_INS_QVLFDUXA:
	case PPC_INS_QVLFSUX:
	case PPC_INS_QVLFSUXA:
	case PPC_INS_QVSTFCDUX:
	case PPC_INS_QVSTFCDUXA:
	case PPC_INS_QVSTFCDUXI:
	case PPC_INS_QVSTFCDUXIA:
	case PPC_INS_QVSTFCSUX:
	case PPC_INS_QVSTFCSUXA:
	case PPC_INS_QVSTFCSUXI:
	case PPC_INS_QVSTFCSUXIA:
	case PPC_INS_QVSTFDUX:
	case PPC_INS_QVSTFDUXA:
	case PPC_INS_QVSTFDUXI:
	case PPC_INS_QVSTFDUXIA:
	case PPC_INS_QVSTFSUX:
	case PPC_INS_QVSTFSUXA:
	case PPC_INS_QVSTFSUXI:
	case PPC_INS_QVSTFSUXIA:
		return true;
	default:
		return false;
	}
}

} // namespace

Capstone2LlvmIrTranslatorPowerpc_impl::Capstone2LlvmIrTranslatorPowerpc_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(
			CS_ARCH_PPC,
			basic,
			extra,
			m)
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

bool Capstone2LlvmIrTranslatorPowerpc_impl::isAllowedBasicMode(cs_mode m)
{
	return m == CS_MODE_32 || m == CS_MODE_64 || m == CS_MODE_QPX;
}

bool Capstone2LlvmIrTranslatorPowerpc_impl::isAllowedExtraMode(cs_mode m)
{
	// Endian plus optional CS_MODE_SPE (e500). Do not OR CS_MODE_PWR7:
	// Capstone 6.0.0-Alpha10 then fails to disassemble classic fadd/fmul/lfd.
	// VSX and Altivec that gcc -O1 emits already decode in plain PPC32/PPC64.
	unsigned cpu = CS_MODE_PWR7 | CS_MODE_PWR8 | CS_MODE_PWR9 | CS_MODE_PWR10
			| CS_MODE_PPC_ISA_FUTURE;
	unsigned bits = static_cast<unsigned>(m) & ~cpu;
	bits &= ~static_cast<unsigned>(CS_MODE_SPE);
	bits &= ~static_cast<unsigned>(CS_MODE_PS);
	return bits == CS_MODE_LITTLE_ENDIAN || bits == CS_MODE_BIG_ENDIAN;
}

uint32_t Capstone2LlvmIrTranslatorPowerpc_impl::getArchByteSize()
{
	switch (_origBasicMode)
	{
		case CS_MODE_32:
			return 4;
		case CS_MODE_64:
		case CS_MODE_QPX:
			return 8;
		default:
		{
			throw GenericError("Unhandled mode in getArchByteSize().");
			break;
		}
	}
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorPowerpc_impl::generateEnvironmentArchSpecific()
{
	// Paired-single (Gekko/Broadway) slot 1 of each FPR, created up front so
	// every consumer sees it from the start.
	if (static_cast<unsigned>(_extraMode) & CS_MODE_PS)
	{
		for (unsigned r = 0; r < 32; ++r)
		{
			getPs1(PPC_REG_F0 + r);
		}
		for (unsigned q = 0; q < 8; ++q)
		{
			getGqr(q);
		}
	}
}

/**
 * Graphics quantization register GQR@a n (SPR 912 + n), 0 at reset: both
 * directions plain single-precision floats, no scaling.
 */
llvm::GlobalVariable* Capstone2LlvmIrTranslatorPowerpc_impl::getGqr(unsigned n)
{
	std::string name = "gqr" + std::to_string(n);
	if (auto* gv = _module->getGlobalVariable(name))
	{
		return gv;
	}
	auto* i32 = llvm::Type::getInt32Ty(_module->getContext());
	return new llvm::GlobalVariable(*_module, i32, false, llvm::GlobalValue::ExternalLinkage,
			llvm::ConstantInt::get(i32, 0), name);
}

/**
 * The second paired-single slot of FPR @a fr, kept as a double alongside
 * the FPR itself (which holds ps0).
 */
llvm::GlobalVariable* Capstone2LlvmIrTranslatorPowerpc_impl::getPs1(uint32_t fr)
{
	std::string name = "ps1_f" + std::to_string(fr - PPC_REG_F0);
	if (auto* gv = _module->getGlobalVariable(name))
	{
		return gv;
	}
	auto* d = llvm::Type::getDoubleTy(_module->getContext());
	return new llvm::GlobalVariable(*_module, d, false, llvm::GlobalValue::ExternalLinkage,
			llvm::ConstantFP::get(d, 0.0), name);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::generateDataLayout()
{
	switch (_basicMode)
	{
		case CS_MODE_32:
			_module->setDataLayout("E-p:32:32:32-f80:32:32");
			break;
		case CS_MODE_64:
		case CS_MODE_QPX:
			_module->setDataLayout("E-p:64:64:64-f80:32:32");
			break;
		default:
		{
			throw GenericError("Unhandled mode in generateDataLayout().");
			break;
		}
	}
}

void Capstone2LlvmIrTranslatorPowerpc_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		createRegister(p.first, _regLt);
	}
}

uint32_t Capstone2LlvmIrTranslatorPowerpc_impl::getCarryRegister()
{
	return PPC_REG_CARRY;
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;
	cs_detail* d = i->detail;
	cs_ppc* pi = &d->ppc;

	// Capstone 6 keeps the real id (ADDI/OR/BC) and puts li/mr/bdnz in
	// alias_id. Prefer a dedicated alias translator.
	std::size_t id = i->id;
	if (i->is_alias)
	{
		auto aIt = _i2fm.find(static_cast<std::size_t>(i->alias_id));
		if (aIt != _i2fm.end() && aIt->second != nullptr)
		{
			id = static_cast<std::size_t>(i->alias_id);
		}
	}

	auto fIt = _i2fm.find(id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;
		(this->*f)(i, pi, irb);
		if (static_cast<unsigned>(_extraMode) & CS_MODE_PS)
		{
			copySingleResultToPs1(i, pi, irb);
		}
	}
	else
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, pi, irb);
	}
}

//
//==============================================================================
// PowerPC-specific methods.
//==============================================================================
//

uint32_t Capstone2LlvmIrTranslatorPowerpc_impl::canonicalRegister(uint32_t r) const
{
	// Capstone 6 names vs32..vs63 as PPC_REG_VSX32..VSX63; those are the
	// Altivec VRs. vs0..vs31 are PPC_REG_VSL0..VSL31, overlapping F0..F31
	// in the high doubleword.
	if (r >= PPC_REG_VSX32 && r <= PPC_REG_VSX63)
	{
		return PPC_REG_V0 + (r - PPC_REG_VSX32);
	}
	if (r >= PPC_REG_X0 && r <= PPC_REG_X31)
	{
		return PPC_REG_R0 + (r - PPC_REG_X0);
	}
	return r;
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == PPC_REG_INVALID)
	{
		return nullptr;
	}
	if (r == PPC_REG_ZERO)
	{
		return llvm::ConstantInt::get(getDefaultType(), 0);
	}

	r = canonicalRegister(r);
	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	// vs0..vs31 share their high doubleword with f0..f31: the high half
	// comes from the FPR, the low half from the VSL register.
	if (dstType == nullptr && r >= PPC_REG_VSL0 && r <= PPC_REG_VSL31)
	{
		if (auto* f = getRegister(PPC_REG_F0 + (r - PPC_REG_VSL0)))
		{
			auto* i64 = irb.getInt64Ty();
			auto* i128 = irb.getInt128Ty();
			auto* hi = irb.CreateBitCast(createLoad(irb, f), i64);
			auto* lo = irb.CreateTrunc(createLoad(irb, llvmReg), i64);
			return irb.CreateOr(
					irb.CreateShl(irb.CreateZExt(hi, i128), 64),
					irb.CreateZExt(lo, i128));
		}
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);

	return createLoad(irb, llvmReg);
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::loadOp(
		cs_ppc_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case PPC_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case PPC_OP_IMM:
		{
			auto* t = getDefaultType();
			return llvm::ConstantInt::get(t, llvm::APInt(t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm), false, /*implicitTrunc=*/true));
			break;
		}
		case PPC_OP_MEM:
		{
			// Capstone 6 X-form (lwzx/lvx/lfdx) is often 2-op: dest + MEM
			// {base, offset}. ISA RA=0 means the constant 0, not r0.
			auto* baseR = (op.mem.base == PPC_REG_R0 || op.mem.base == PPC_REG_ZERO)
					? nullptr
					: loadRegister(op.mem.base, irb);
			auto* t = getDefaultType();
			llvm::Value* disp = llvm::ConstantInt::getSigned(t, op.mem.disp);

			llvm::Value* addr = nullptr;
			if (baseR == nullptr)
			{
				addr = disp;
			}
			else if (op.mem.disp == 0)
			{
				addr = baseR;
			}
			else
			{
				disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
				addr = irb.CreateAdd(baseR, disp);
			}
			if (op.mem.offset != PPC_REG_INVALID)
			{
				auto* idx = loadRegister(op.mem.offset, irb);
				if (idx != nullptr)
				{
					idx = irb.CreateSExtOrTrunc(idx, addr->getType());
					addr = irb.CreateAdd(addr, idx);
				}
			}

			if (lea)
			{
				return addr;
			}
			else
			{
				auto* lty = ty ? ty : t;
				return loadIntPtr(irb, addr, lty);
			}
		}
		case PPC_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::StoreInst* Capstone2LlvmIrTranslatorPowerpc_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == PPC_REG_INVALID || r == PPC_REG_ZERO)
	{
		return nullptr;
	}

	r = canonicalRegister(r);
	auto* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("storeRegister() unhandled reg.");
	}
	auto* destTy = llvmReg->getValueType();
	llvm::GlobalVariable* fpr = nullptr;
	if (r >= PPC_REG_VSL0 && r <= PPC_REG_VSL31)
	{
		fpr = getRegister(PPC_REG_F0 + (r - PPC_REG_VSL0));
	}
	if (destTy->isIntegerTy(128) && val->getType()->isFloatingPointTy())
	{
		if (fpr != nullptr)
		{
			// Only the shared high doubleword changes.
			auto* d = irb.CreateFPCast(val, irb.getDoubleTy());
			auto* fs = irb.CreateStore(d, fpr);
			attachPointeeType(fs, fpr->getValueType());
			return fs;
		}
		val = insertHighDouble(createLoad(irb, llvmReg), val, irb);
	}
	else
	{
		val = generateTypeConversion(irb, val, destTy, ct);
	}

	if (fpr != nullptr && val->getType()->isIntegerTy(128))
	{
		auto* hi = irb.CreateTrunc(irb.CreateLShr(val, 64), irb.getInt64Ty());
		auto* fs = irb.CreateStore(irb.CreateBitCast(hi, irb.getDoubleTy()), fpr);
		attachPointeeType(fs, fpr->getValueType());
	}

	auto* s = irb.CreateStore(val, llvmReg);
	attachPointeeType(s, destTy);
	return s;
}

llvm::Instruction* Capstone2LlvmIrTranslatorPowerpc_impl::storeOp(
		cs_ppc_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case PPC_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case PPC_OP_MEM:
		{
			// Capstone 6 X-form (lwzx/lvx/lfdx) is often 2-op: dest + MEM
			// {base, offset}. ISA RA=0 means the constant 0, not r0.
			auto* baseR = (op.mem.base == PPC_REG_R0 || op.mem.base == PPC_REG_ZERO)
					? nullptr
					: loadRegister(op.mem.base, irb);
			auto* t = getDefaultType();
			llvm::Value* disp = llvm::ConstantInt::getSigned(t, op.mem.disp);

			llvm::Value* addr = nullptr;
			if (baseR == nullptr)
			{
				addr = disp;
			}
			else if (op.mem.disp == 0)
			{
				addr = baseR;
			}
			else
			{
				disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
				addr = irb.CreateAdd(baseR, disp);
			}
			if (op.mem.offset != PPC_REG_INVALID)
			{
				auto* idx = loadRegister(op.mem.offset, irb);
				if (idx != nullptr)
				{
					idx = irb.CreateSExtOrTrunc(idx, addr->getType());
					addr = irb.CreateAdd(addr, idx);
				}
			}

			return storeIntPtr(irb, val, addr, val->getType());
		}
		case PPC_OP_IMM:
		case PPC_OP_INVALID:
		default:
		{
			throw GenericError("Should not be possible.");
		}
	}
}

void Capstone2LlvmIrTranslatorPowerpc_impl::crFieldRegisters(
	uint32_t crReg, uint32_t& ltR, uint32_t& gtR, uint32_t& eqR, uint32_t& soR)
{
	switch (crReg)
	{
	case PPC_REG_CR0:
		ltR = PPC_REG_CR0LT;
		gtR = PPC_REG_CR0GT;
		eqR = PPC_REG_CR0EQ;
		soR = PPC_REG_CR0UN;
		break;
	case PPC_REG_CR1:
		ltR = PPC_REG_CR1LT;
		gtR = PPC_REG_CR1GT;
		eqR = PPC_REG_CR1EQ;
		soR = PPC_REG_CR1UN;
		break;
	case PPC_REG_CR2:
		ltR = PPC_REG_CR2LT;
		gtR = PPC_REG_CR2GT;
		eqR = PPC_REG_CR2EQ;
		soR = PPC_REG_CR2UN;
		break;
	case PPC_REG_CR3:
		ltR = PPC_REG_CR3LT;
		gtR = PPC_REG_CR3GT;
		eqR = PPC_REG_CR3EQ;
		soR = PPC_REG_CR3UN;
		break;
	case PPC_REG_CR4:
		ltR = PPC_REG_CR4LT;
		gtR = PPC_REG_CR4GT;
		eqR = PPC_REG_CR4EQ;
		soR = PPC_REG_CR4UN;
		break;
	case PPC_REG_CR5:
		ltR = PPC_REG_CR5LT;
		gtR = PPC_REG_CR5GT;
		eqR = PPC_REG_CR5EQ;
		soR = PPC_REG_CR5UN;
		break;
	case PPC_REG_CR6:
		ltR = PPC_REG_CR6LT;
		gtR = PPC_REG_CR6GT;
		eqR = PPC_REG_CR6EQ;
		soR = PPC_REG_CR6UN;
		break;
	case PPC_REG_CR7:
		ltR = PPC_REG_CR7LT;
		gtR = PPC_REG_CR7GT;
		eqR = PPC_REG_CR7EQ;
		soR = PPC_REG_CR7UN;
		break;
	default: throw GenericError("Unhandled CR register.");
	}
}

void Capstone2LlvmIrTranslatorPowerpc_impl::storeCrX(
		llvm::IRBuilder<>& irb,
		uint32_t crReg,
		llvm::Value* op0,
		llvm::Value* op1, // = nullptr
		bool signedCmp) // = true
{
	llvm::Value* zero = llvm::ConstantInt::get(op0->getType(), 0);
	if (op1 == nullptr)
	{
		op1 = zero;
	}

	if (op0->getType() != op1->getType())
	{
		op1 = irb.CreateSExtOrTrunc(op1, op0->getType());
	}

	auto s = signedCmp;
	auto* lt = s ? irb.CreateICmpSLT(op0, op1) : irb.CreateICmpULT(op0, op1);
	auto* gt = s ? irb.CreateICmpSGT(op0, op1) : irb.CreateICmpUGT(op0, op1);
	auto* eq = irb.CreateICmpEQ(op0, op1);
	// The fourth bit of an integer compare is a copy of XER[SO].
	auto* so = loadRegister(kXerSoReg, irb);

	uint32_t ltR = PPC_REG_CR0LT;
	uint32_t gtR = PPC_REG_CR0GT;
	uint32_t eqR = PPC_REG_CR0EQ;
	uint32_t soR = PPC_REG_CR0UN;

	crFieldRegisters(crReg, ltR, gtR, eqR, soR);

	storeRegister(ltR, lt, irb);
	storeRegister(gtR, gt, irb);
	storeRegister(eqR, eq, irb);
	storeRegister(soR, so, irb);
}

std::tuple<llvm::Value*, llvm::Value*, llvm::Value*, llvm::Value*> Capstone2LlvmIrTranslatorPowerpc_impl::loadCrX(
		llvm::IRBuilder<>& irb,
		uint32_t crReg)
{
	uint32_t ltR = PPC_REG_CR0LT;
	uint32_t gtR = PPC_REG_CR0GT;
	uint32_t eqR = PPC_REG_CR0EQ;
	uint32_t soR = PPC_REG_CR0UN;

	crFieldRegisters(crReg, ltR, gtR, eqR, soR);

	llvm::Value* lt = loadRegister(ltR, irb);
	llvm::Value* gt = loadRegister(gtR, irb);
	llvm::Value* eq = loadRegister(eqR, irb);
	llvm::Value* so = loadRegister(soR, irb);

	return std::make_tuple(lt, gt, eq, so);
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::loadCrX(
		llvm::IRBuilder<>& irb,
		uint32_t crReg,
		ppc_pred type)
{
	uint32_t ltR = PPC_REG_CR0LT;
	uint32_t gtR = PPC_REG_CR0GT;
	uint32_t eqR = PPC_REG_CR0EQ;
	uint32_t soR = PPC_REG_CR0UN;

	if (isCrRegister(crReg) && isCrBitRegister(crReg))
	{
		return loadRegister(crReg, irb);
	}

	crFieldRegisters(crReg, ltR, gtR, eqR, soR);

	switch (type)
	{
		case PPC_CR_LT:
			return loadRegister(ltR, irb);;
		case PPC_CR_GT:
			return loadRegister(gtR, irb);
		case PPC_CR_EQ:
			return loadRegister(eqR, irb);
		case PPC_CR_SO:
			return loadRegister(soR, irb);
		default:
			throw GenericError("Should not happen.");
	}
}

/**
 * CR0 for a word operation whose result is ZERO-extended into the register.
 *
 * Power ISA: for Rc=1, "if MSR_SF = 1 then M <- 0 else M <- 32; if (RA)[M:63]
 * < 0 then LT". In 64-bit mode the WHOLE register is compared, and rlwinm,
 * slw, srw and the rest of that family all leave RA[0:31] zero -- so their
 * register value can never be negative, while the 32-bit result can.
 *
 *   rlwinm. r0, r1, 0, 0, 15   with r1 = 0x80000000
 *     RA = 0x0000000080000000 -> LT = 0, GT = 1
 *     comparing the word      -> LT = 1, GT = 0
 *
 * Widening to the register's own type says that. EQ was right either way,
 * which is most of why this survived: compilers know LT is always clear after
 * rlwinm. on ppc64 and branch on beq/bne rather than blt/bgt.
 *
 * The SIGN-extending members of the same family -- cntlzw., divw., mulhw.,
 * sraw. -- must NOT come through here: their word and their register value
 * have the same sign, and zero-extending would invert it.
 */
/**
 * A float-to-integer conversion with the Power ISA's defined answer for the
 * inputs that do not fit.
 *
 * LLVM's fptosi and fptoui call a NaN, an infinity, or an out-of-range
 * magnitude POISON. Power defines all three, and -- this is the part worth
 * being careful about -- defines them differently from both x86 and ARM:
 *
 *   FCTIW/FCTIWZ   above 2^31-1 -> 0x7fffffff
 *                  below -2^31  -> 0x80000000
 *                  NaN          -> 0x80000000     (the MINIMUM, not zero)
 *   FCTIWU/…UZ     above 2^32-1 -> 0xffffffff
 *                  below 0      -> 0
 *                  NaN          -> 0
 *
 * ARM sends a NaN to ZERO; x86 sends every bad input alike to the integer
 * indefinite value. Three architectures, three rules, and the one thing they
 * agree on is that none of them is poison. Copying a neighbour's helper here
 * would have been wrong in a way no test would notice.
 *
 * NOT MEASURED: this container is x86-64 with no PowerPC emulator, so this
 * rests on the Power ISA's stated results rather than an observation.
 */
llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::generateFpToIntBounded(
	llvm::Value* v, llvm::Type* intTy, bool isSigned, llvm::IRBuilder<>& irb)
{
	unsigned bits = intTy->getScalarSizeInBits();
	auto* fpTy = v->getType();

	auto* loF = llvm::ConstantFP::get(fpTy, isSigned ? -std::ldexp(1.0, static_cast<int>(bits - 1)) : 0.0);
	auto* hiF = llvm::ConstantFP::get(fpTy, std::ldexp(1.0, static_cast<int>(isSigned ? bits - 1 : bits)));

	auto* aboveLo = irb.CreateFCmpOGE(v, loF);
	auto* inRange = irb.CreateAnd(aboveLo, irb.CreateFCmpOLT(v, hiF));

	auto* safe = irb.CreateSelect(inRange, v, llvm::ConstantFP::get(fpTy, 0.0));
	auto* conv = isSigned ? irb.CreateFPToSI(safe, intTy) : irb.CreateFPToUI(safe, intTy);

	auto* minV =
		llvm::ConstantInt::get(intTy, isSigned ? llvm::APInt::getSignedMinValue(bits) : llvm::APInt::getZero(bits));
	auto* maxV =
		llvm::ConstantInt::get(intTy, isSigned ? llvm::APInt::getSignedMaxValue(bits) : llvm::APInt::getAllOnes(bits));

	// A NaN fails the ordered comparison above, so it lands on the minimum --
	// which is exactly what Power specifies, for both signednesses.
	return irb.CreateSelect(inRange, conv, irb.CreateSelect(aboveLo, maxV, minV));
}

void Capstone2LlvmIrTranslatorPowerpc_impl::storeCr0Word(llvm::IRBuilder<>& irb, cs_ppc* pi, llvm::Value* val)
{
	if (!pi->update_cr0)
	{
		return;
	}
	storeCr0(irb, pi, irb.CreateZExtOrTrunc(val, getDefaultType()));
}

void Capstone2LlvmIrTranslatorPowerpc_impl::storeCr0(
		llvm::IRBuilder<>& irb,
		cs_ppc* pi,
		llvm::Value* val)
{
	if (!pi->update_cr0)
	{
		return;
	}

	llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);

	auto* ltZero = irb.CreateICmpSLT(val, zero);
	storeRegister(PPC_REG_CR0LT, ltZero, irb);

	auto* gtZero = irb.CreateICmpSGT(val, zero);
	storeRegister(PPC_REG_CR0GT, gtZero, irb);

	auto* eqZero = irb.CreateICmpEQ(val, zero);
	storeRegister(PPC_REG_CR0EQ, eqZero, irb);

	// PPC_REG_CR0UN is a copy of XER, which we do not have.
	storeRegister(PPC_REG_CR0UN, zero, irb);
}

bool Capstone2LlvmIrTranslatorPowerpc_impl::isGeneralPurposeRegister(uint32_t r)
{
	return PPC_REG_R0 <= r && r <= PPC_REG_R31;
}

uint32_t Capstone2LlvmIrTranslatorPowerpc_impl::getGeneralPurposeRegisterIndex(uint32_t r)
{
	return r - PPC_REG_R0;
}

/**
 * 0  -> PPC_REG_CR0LT
 * 1  -> PPC_REG_CR0GT
 * 2  -> PPC_REG_CR0EQ
 * 3  -> PPC_REG_CR0UN
 * 4  -> PPC_REG_CR1LT
 * 5  -> PPC_REG_CR2GT
 * ...
 * 30 -> PPC_REG_CR7EQ
 * 31 -> PPC_REG_CR7UN
 */
uint32_t Capstone2LlvmIrTranslatorPowerpc_impl::crBitIndexToCrRegister(uint32_t idx)
{
	return PPC_REG_CR0LT + idx;
}

bool Capstone2LlvmIrTranslatorPowerpc_impl::isCrRegister(uint32_t r)
{
	return (PPC_REG_CR0 <= r && r <= PPC_REG_CR7)
			|| isCrBitRegister(r);
}

bool Capstone2LlvmIrTranslatorPowerpc_impl::isCrRegister(cs_ppc_op& op)
{
	return op.type == PPC_OP_REG && isCrRegister(op.reg);
}

/**
 * Capstone's CR bit registers are grouped by bit NAME, not by field: EQ is
 * 312..319 for CR0..CR7, GT is 320..327, LT 328..335 and UN 336..343. So a
 * numeric range over them is a range over that layout and not over the fields,
 * and the end of it is CR7UN.
 *
 * This said `r <= PPC_REG_CR5UN`, which is 341 -- correct for thirty of the
 * thirty-two bits and wrong for CR6UN (342) and CR7UN (343). The two it
 * excluded were the summary-overflow bits of the two highest fields, which is
 * exactly the kind of near-miss a range over an enum whose order you have not
 * checked produces: it looks like it says "the CR bit registers" and it says
 * "all but two of them".
 */
bool Capstone2LlvmIrTranslatorPowerpc_impl::isCrBitRegister(uint32_t r)
{
	return PPC_REG_CR0EQ <= r && r <= PPC_REG_CR7UN;
}
bool Capstone2LlvmIrTranslatorPowerpc_impl::isOperandRegister(cs_ppc_op& op)
{
	return op.type == PPC_OP_REG;
}

/**
 * The effective address of an X-form (register-indexed) memory access.
 *
 * The rA slot of every X-form access has an encoding the d(rA) forms share and
 * that the arithmetic forms do not: rA = 0 does not mean r0, it means the
 * literal zero. `lwzx rD, 0, rB` therefore addresses rB alone, and GCC emits
 * exactly that whenever the address is already whole in one register.
 *
 * Capstone reports that slot as PPC_OP_REG with reg PPC_REG_INVALID, and
 * loadOp() turns an invalid register into UndefValue -- so the address of
 * every rA=0 indexed access was `add undef, rB`, an undefined pointer, in five
 * translators (integer and float, load and store, and lhbrx). The d(rA) forms
 * never had this: loadOp()'s PPC_OP_MEM case already reads
 * `mem.base == PPC_REG_INVALID` as "displacement alone". This applies the same
 * rule to the register-indexed forms.
 *
 * The rA slot is always the second-to-last operand -- operands[1] of three for
 * the ternary form, operands[0] of two for the binary one -- which is the same
 * slot loadOpBinaryOrTernaryOp1Op2() loads, because _loadOps() counts back
 * from the end.
 */
llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::generateIndexedAddress(
	cs_ppc* pi, llvm::Value* base, llvm::Value* index, llvm::IRBuilder<>& irb)
{
	if (pi->op_count == 2)
	{
		// Capstone 6 omits rA when it is r0 rather than emitting
		// PPC_REG_INVALID. The remaining index operand is RB.
		return index;
	}
	if (pi->op_count >= 3)
	{
		cs_ppc_op& aOp = pi->operands[pi->op_count - 2];
		if (aOp.type == PPC_OP_REG
			&& (aOp.reg == PPC_REG_INVALID || aOp.reg == PPC_REG_R0
				|| aOp.reg == PPC_REG_ZERO))
		{
			return index;
		}
	}

	return irb.CreateAdd(base, index);
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::loadIndexedEffectiveAddress(
	cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	cs_ppc_op& last = pi->operands[pi->op_count - 1];
	if (last.type == PPC_OP_MEM)
	{
		return loadOp(last, irb, nullptr, true);
	}
	if (pi->op_count == 2)
	{
		auto* idx = loadOp(last, irb);
		return irb.CreateZExtOrTrunc(idx, getDefaultType());
	}
	auto* base = loadOp(pi->operands[pi->op_count - 2], irb);
	auto* index = loadOp(last, irb);
	base = irb.CreateZExtOrTrunc(base, getDefaultType());
	index = irb.CreateZExtOrTrunc(index, getDefaultType());
	return generateIndexedAddress(pi, base, index, irb);
}

//
//==============================================================================
// PowerPC instruction translation methods.
//==============================================================================
//

/**
 * PPC_INS_ADD, PPC_INS_ADDI
 * PPC_INS_LA - 1. and 2. operands are reversed, but it probbaly does not matter.
 *              la 0, 0x4, 1 (reg, imm, reg) == addi 0, 1, 0x4 (reg, reg, imm)
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAdd(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST, eOpConv::FPCAST_OR_BITCAST);
	auto* add = irb.CreateAdd(op1, op2);
	storeOp(pi->operands[0], add, irb);
	storeCr0(irb, pi, add);
}

/**
 * PPC_INS_ADDC, PPC_INS_ADDIC
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAddc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* add = irb.CreateAdd(op1, op2);
	storeOp(pi->operands[0], add, irb);
	storeCr0(irb, pi, add);
	storeRegister(PPC_REG_CARRY, generateCarryAdd(add, op1, irb), irb);
}

/**
 * PPC_INS_ADDE
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAdde(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* add = irb.CreateAdd(op1, op2);
	auto* carry = loadRegister(PPC_REG_CARRY, irb);
	carry = irb.CreateZExtOrTrunc(carry, add->getType());
	add = irb.CreateAdd(add, carry);
	storeOp(pi->operands[0], add, irb);
	// TODO: In the original semantics, LT is set using final add, GT and EQ
	// using first add. Not sure what is ok.
	storeCr0(irb, pi, add);
	storeRegister(PPC_REG_CARRY, generateCarryAddC(op1, op2, irb, carry), irb);
}

/**
 * PPC_INS_ADDIS
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAddis(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	// ELFv2 global entry: `addis r2, r12, hi` runs with r12 holding this
	// very address (the ABI requires it), so the TOC pointer is a constant.
	// Without this r12 is an unknown input and every TOC-relative access
	// (switch tables, globals) reads through garbage.
	if (_origBasicMode == CS_MODE_64 && pi->op_count == 3
			&& pi->operands[0].type == PPC_OP_REG && pi->operands[0].reg == PPC_REG_R2
			&& pi->operands[1].type == PPC_OP_REG && pi->operands[1].reg == PPC_REG_R12)
	{
		op1 = llvm::ConstantInt::get(op1->getType(), i->address);
	}
	op2 = irb.CreateShl(op2, llvm::ConstantInt::get(op2->getType(), 16));
	auto* add = irb.CreateAdd(op1, op2);
	storeOp(pi->operands[0], add, irb);
	storeCr0(irb, pi, add);
}

/**
 * PPC_INS_ADDME
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAddme(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	auto* carry = loadRegister(PPC_REG_CARRY, irb);
	carry = irb.CreateZExtOrTrunc(carry, op1->getType());
	auto* add = irb.CreateAdd(op1, carry);
	auto* one = llvm::ConstantInt::get(add->getType(), 1);
	auto* sub = irb.CreateSub(add, one);
	storeOp(pi->operands[0], sub, irb);

	// TODO: In the original semantics, LT is set using a different value
	// than GT and EQ.
	storeCr0(irb, pi, sub);

	auto* negativeOne = llvm::ConstantInt::getSigned(op1->getType(), -1);
	storeRegister(PPC_REG_CARRY, generateCarryAddC(op1, negativeOne, irb, carry), irb);
}

/**
 * PPC_INS_ADDZE
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAddze(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	auto* carry = loadRegister(PPC_REG_CARRY, irb);
	carry = irb.CreateZExtOrTrunc(carry, op1->getType());
	auto* add = irb.CreateAdd(op1, carry);
	storeOp(pi->operands[0], add, irb);

	// TODO: In the original semantics, LT is set using final add, GT and EQ
	// using op1. Not sure what is ok.
	storeCr0(irb, pi, add);

	auto* zero = llvm::ConstantInt::get(op1->getType(), 0);
	storeRegister(PPC_REG_CARRY, generateCarryAddC(op1, zero, irb, carry), irb);
}

/**
 * PPC_INS_AND, PPC_INS_ANDI
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAnd(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* val = irb.CreateAnd(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_ANDC
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAndc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = generateValueNegate(irb, op2);
	auto* val = irb.CreateAnd(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_ANDIS
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAndis(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = irb.CreateShl(op2, llvm::ConstantInt::get(op2->getType(), 16));
	auto* val = irb.CreateAnd(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_CLRLWI - clrlwi rA, RS, n (n < 32) = rlwinm rA, rS, 0, n, 31
 * PPC_INS_RLWINM - in capstone, rlwinm is equivalent to clrlwi
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateClrlwi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	std::set<int> opCountTemp = {3, 5};
	EXPECT_IS_SET(i, pi, irb, opCountTemp)
	if(pi->op_count == 3)
	{
		// `clrlwi rA, rS, n` is `rlwinm rA, rS, 0, n, 31`: clear the top n bits
		// of the WORD. Shifting left and back at the register's width clears
		// the top n bits of the register, which on 64-bit PowerPC keeps the
		// upper 32 bits the instruction is defined to drop -- `clrlwi r0, r1,
		// 16` with r1 = 0x12345678 answered 0x12345678 instead of 0x5678.
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		auto* i32 = irb.getInt32Ty();
		llvm::Value* w = irb.CreateZExtOrTrunc(op1, i32);
		llvm::Value* n = irb.CreateAnd(irb.CreateZExtOrTrunc(op2, i32), llvm::ConstantInt::get(i32, 31));
		llvm::Value* res = irb.CreateLShr(irb.CreateShl(w, n), n);
		storeOp(pi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		storeCr0Word(irb, pi, res);
	}
	else
	{
		translateRotateWordMask(i, pi, irb);
	}
}

/**
 * PPC_INS_CMPD  = cmp  0, 1, rA, rB
 * But Capstone also allows things like "cmpd cr5, 0, 1"
 *
 * PPC_INS_CMPDI = cmpi 0, 1, rA, value
 * But Capstone also allows things like "cmpdi cr5, 0, 1"
 *
 * PPC_INS_CMPW, PPC_INS_CMPWI
 * PPC_INS_CMPLD, PPC_INS_CMPLDI
 * PPC_INS_CMPLW, PPC_INS_CMPLWI
 *
 * PPC_INS_CMP, PPC_INS_CMPL
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateCmp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	std::set<int> opCountTemp = {2, 3, 4};
	EXPECT_IS_SET(i, pi, irb, opCountTemp)

	uint32_t crReg = PPC_REG_CR0;
	bool word = false;
	bool haveL = false;

	if (pi->op_count == 2)
	{
		std::tie(op0, op1) = loadOpBinary(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	}
	else if (pi->op_count == 3
			&& pi->operands[0].type == PPC_OP_REG
			&& pi->operands[0].reg >= PPC_REG_CR0
			&& pi->operands[0].reg <= PPC_REG_CR7)
	{
		crReg = pi->operands[0].reg;
		std::tie(op0, op1) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	}
	else if (pi->op_count == 3 && pi->operands[0].type == PPC_OP_IMM)
	{
		// Primary form with CR0 implied: cmpi L, rA, SI / cmp L, rA, rB.
		haveL = true;
		word = (pi->operands[0].imm == 0);
		op0 = loadOp(pi->operands[1], irb);
		op1 = loadOp(pi->operands[2], irb);
	}
	else if (pi->op_count == 4
			&& pi->operands[0].type == PPC_OP_REG
			&& pi->operands[0].reg >= PPC_REG_CR0
			&& pi->operands[0].reg <= PPC_REG_CR7)
	{
		crReg = pi->operands[0].reg;
		if (pi->operands[1].type == PPC_OP_IMM)
		{
			haveL = true;
			word = (pi->operands[1].imm == 0);
		}
		op0 = loadOp(pi->operands[2], irb);
		op1 = loadOp(pi->operands[3], irb);
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	if (op0->getType() != op1->getType())
	{
		op1 = irb.CreateSExtOrTrunc(op1, op0->getType());
	}

	if (!haveL
			&& (i->id == PPC_INS_CMPW
				|| i->id == PPC_INS_CMPL
				|| i->id == PPC_INS_CMPWI
				|| i->id == PPC_INS_CMPLW
				|| i->id == PPC_INS_CMPLWI))
	{
		word = true;
	}

	if (word)
	{
		op0 = irb.CreateSExtOrTrunc(op0, irb.getInt32Ty());
		op1 = irb.CreateSExtOrTrunc(op1, irb.getInt32Ty());
	}

	bool signedCmp = true;
	if (i->id == PPC_INS_CMPLD
			|| i->id == PPC_INS_CMPL
			|| i->id == PPC_INS_CMPLDI
			|| i->id == PPC_INS_CMPLI
			|| i->id == PPC_INS_CMPLW
			|| i->id == PPC_INS_CMPLWI)
	{
		signedCmp = false;
	}

	storeCrX(irb, crReg, op0, op1, signedCmp);
}

/**
 * PPC_INS_CNTLZW, PPC_INS_CNTLZD
 */
/**
 * The `w` instructions -- cntlzw, mullw, mulhw, divw, sraw and their
 * relatives -- work on the low WORD of their operands whatever the register
 * width. On PPC64, which createPpc64() enables and the gtest suite
 * instantiates, the operands arrive as i64 and each of these was computing at
 * register width:
 *
 *   cntlzw r0, r1      r1 = 1                  31 on the hardware, 63 here
 *   mullw  r0, r1, r2  r1 = 0x1_00000002, r2=3  6 on the hardware, 0x3_00000006
 *   mulhw  r0, r1, r2  r1 = 0x1_00000002, r2=2  0 on the hardware, 2 here
 *   divw   r0, r1, r2  r1 = 0x1_00000000, r2=1  0 on the hardware, 0x1_00000000
 *
 * All of them are inert in 32-bit mode, which is where every test for them
 * that is not ALL_MODES lives.
 *
 * The doubleword forms (cntlzd, divd, mulld, mulhd, sld, srad) use the same
 * translators at 64-bit width. MIPS solves the word/double split centrally
 * with isWordOperation() and narrowToWord(); here the instruction id picks
 * the width.
 */
llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::narrowToWord(llvm::Value* v, llvm::IRBuilder<>& irb)
{
	return irb.CreateZExtOrTrunc(v, irb.getInt32Ty());
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateCntlzw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	if (i->id == PPC_INS_CNTLZD)
	{
		op1 = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
	}
	else
	{
		op1 = narrowToWord(op1, irb);
	}
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::ctlz,
			op1->getType());
	// isZeroUndef = false: Power defines cntlzw(0)=32 and cntlzd(0)=64.
	auto* val = irb.CreateCall(f, {op1, irb.getFalse()});
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translatePopcnt(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	if (i->id == PPC_INS_POPCNTD)
	{
		op1 = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
	}
	else
	{
		op1 = narrowToWord(op1, irb);
	}
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::ctpop,
			op1->getType());
	auto* val = irb.CreateCall(f, {op1});
	storeOp(pi->operands[0], val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * PPC_INS_DIVW, PPC_INS_DIVWU, PPC_INS_DIVD, PPC_INS_DIVDU
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateDivw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	const bool dbl = (i->id == PPC_INS_DIVD || i->id == PPC_INS_DIVDU);
	if (dbl)
	{
		op1 = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt64Ty());
	}
	else
	{
		op1 = narrowToWord(op1, irb);
		op2 = narrowToWord(op2, irb);
	}
	const bool isSigned = (i->id == PPC_INS_DIVW || i->id == PPC_INS_DIVD);
	// Power says RT is UNDEFINED for a zero divisor -- and, for signed
	// divide, for MIN / -1 -- while the instruction COMPLETES and takes no
	// interrupt. LLVM's sdiv says something much stronger: immediate
	// undefined behaviour, which lets the optimiser delete the surrounding
	// code rather than merely leave a value unspecified.
	//
	// Since any value is a faithful model of "undefined register contents",
	// one select on the divisor is the whole fix; there is no architectural
	// answer to select back in, unlike ARM64 where the manual names one.
	//
	// CR0 needs no special-casing either: the LT/GT/EQ bits are undefined in
	// exactly the cases being guarded, so deriving them from whatever the
	// guard yields is already correct.
	//
	// Not theoretical: GCC guards PowerPC divisions with twi/tw, and every
	// member of that trap family is a nullptr entry here, so the guard
	// becomes an opaque call that constrains nothing and the division that
	// follows is bare.
	{
		auto* dty = op2->getType();
		unsigned dbits = dty->getIntegerBitWidth();
		if (isSigned)
		{
			auto* overflow = irb.CreateAnd(
				irb.CreateICmpEQ(op1, llvm::ConstantInt::get(dty, llvm::APInt::getSignedMinValue(dbits))),
				irb.CreateICmpEQ(op2, llvm::ConstantInt::getSigned(dty, -1)));
			op2 = irb.CreateSelect(overflow, llvm::ConstantInt::get(dty, 1), op2);
		}
		// umax rather than a select on "is it zero": the two compute the same number
		// -- umax(x, 1) is x for every non-zero x, since x as an UNSIGNED value is
		// then at least 1, and is 1 when x is zero -- but only umax states the bound
		// LOCALLY. A reader of `select(y == 0, 1, y)`, human or analysis, has to
		// correlate the condition with the arms to see the result is non-zero, and
		// DIV-01 below cannot. This is the same lesson SHIFT-01 taught on the shift
		// side, where the clamp-and-select idiom made the checker fire on the fix.
		op2 = irb.CreateBinaryIntrinsic(llvm::Intrinsic::umax, op2, llvm::ConstantInt::get(dty, 1));
	}

	auto* val = isSigned ? irb.CreateSDiv(op1, op2) : irb.CreateUDiv(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_EQV
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateEqv(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* val = irb.CreateXor(op1, op2);
	val = generateValueNegate(irb, val);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_EXTSB, PPC_INS_EXTSH, PPC_INS_EXTSW
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateExtendSign(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(op1->getType());
	if (ty == nullptr)
	{
		return;
	}
	unsigned shift = 0;
	switch (i->id)
	{
		case PPC_INS_EXTSB: shift = ty->getBitWidth() - 8; break;
		case PPC_INS_EXTSH: shift = ty->getBitWidth() - 16; break;
		case PPC_INS_EXTSW: shift = ty->getBitWidth() - 32; break;
		default: return;
	}

	auto* val = irb.CreateShl(op1, shift);
	val = irb.CreateAShr(val, shift);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val); // TODO: Orig sem is using op1 here.
}

/**
 * PPC_INS_LBZ, PPC_INS_LHZ, PPC_INS_LWZ,
 * PPC_INS_LBZU, PPC_INS_LHZU, PPC_INS_LWZU,
 * PPC_INS_LHA, PPC_INS_LHAU
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLoad(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case PPC_INS_LBZ:
		case PPC_INS_LBZU:
			ty = irb.getInt8Ty();
			break;
		case PPC_INS_LHZ:
		case PPC_INS_LHZU:
		case PPC_INS_LHA:
		case PPC_INS_LHAU:
			ty = irb.getInt16Ty();
			break;
		case PPC_INS_LWZ:
		case PPC_INS_LWZU:
		case PPC_INS_LWA: ty = irb.getInt32Ty(); break;
		case PPC_INS_LD:
		case PPC_INS_LDU: ty = irb.getInt64Ty(); break;
		default:
			return;
	}

	op1 = loadOpBinaryOp1(pi, irb, ty);

	eOpConv conv = eOpConv::ZEXT_TRUNC_OR_BITCAST;
	if (i->id == PPC_INS_LHA || i->id == PPC_INS_LHAU || i->id == PPC_INS_LWA)
	{
		conv = eOpConv::SEXT_TRUNC_OR_BITCAST;
	}
	storeOp(pi->operands[0], op1, irb, conv);

	// With update.
	//
	auto& ppcOp1 = pi->operands[1];
	if (i->id == PPC_INS_LBZU || i->id == PPC_INS_LHZU || i->id == PPC_INS_LWZU || i->id == PPC_INS_LHAU
		|| i->id == PPC_INS_LDU)
		if (ppcOp1.type == PPC_OP_MEM && ppcOp1.mem.base != PPC_REG_INVALID)
			if (auto* l = llvm::dyn_cast<llvm::LoadInst>(op1))
				if (auto* cast = llvm::dyn_cast<llvm::CastInst>(l->getPointerOperand()))
				{
					storeRegister(ppcOp1.mem.base, cast->getOperand(0), irb);
				}
}

/**
 * PPC_INS_LBZX, PPC_INS_LHZX, PPC_INS_LWZX,
 * PPC_INS_LBZUX, PPC_INS_LHZUX, PPC_INS_LWZUX,
 * PPC_INS_LHAX, PPC_INS_LHAUX
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLoadIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	auto* add = loadIndexedEffectiveAddress(pi, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case PPC_INS_LBZX:
		case PPC_INS_LBZCIX:
		case PPC_INS_LBEPX:
		case PPC_INS_LBZUX:
		case PPC_INS_LBARX:
			ty = irb.getInt8Ty();
			break;
		case PPC_INS_LHZX:
		case PPC_INS_LHZCIX:
		case PPC_INS_LHEPX:
		case PPC_INS_LHZUX:
		case PPC_INS_LHARX:
		case PPC_INS_LHAX:
		case PPC_INS_LHAUX:
			ty = irb.getInt16Ty();
			break;
		case PPC_INS_LWZX:
		case PPC_INS_LWZCIX:
		case PPC_INS_LWEPX:
		case PPC_INS_LWZUX:
		case PPC_INS_LWARX:
		case PPC_INS_LWAX:
		case PPC_INS_LWAUX: ty = irb.getInt32Ty(); break;
		case PPC_INS_LDARX:
		case PPC_INS_LDX:
		case PPC_INS_LDCIX:
		case PPC_INS_LDUX: ty = irb.getInt64Ty(); break;
		default:
			return;
	}

	auto* l = loadIntPtr(irb, add, ty);
	if (i->id == PPC_INS_LWARX || i->id == PPC_INS_LDARX || i->id == PPC_INS_LBARX || i->id == PPC_INS_LHARX)
	{
		l->setAtomic(llvm::AtomicOrdering::Monotonic);
	}

	eOpConv conv = eOpConv::ZEXT_TRUNC_OR_BITCAST;
	if (i->id == PPC_INS_LHAX || i->id == PPC_INS_LHAUX || i->id == PPC_INS_LWAX || i->id == PPC_INS_LWAUX)
	{
		conv = eOpConv::SEXT_TRUNC_OR_BITCAST;
	}
	storeOp(pi->operands[0], l, irb, conv);

	if (i->id == PPC_INS_LBZUX || i->id == PPC_INS_LHZUX || i->id == PPC_INS_LWZUX || i->id == PPC_INS_LHAUX
		|| i->id == PPC_INS_LDUX || i->id == PPC_INS_LWAUX)
	{
		storeIndexedUpdate(pi, add, irb);
	}
}

void Capstone2LlvmIrTranslatorPowerpc_impl::storeIndexedUpdate(cs_ppc* pi, llvm::Value* ea, llvm::IRBuilder<>& irb)
{
	// Capstone 6 X-form is dest + PPC_OP_MEM {base=rA, offset=rB}. The
	// update form writes that effective address back to rA. The legacy
	// three-register form still names rA as operands[1].
	for (unsigned k = 0; k < pi->op_count; ++k)
	{
		cs_ppc_op& op = pi->operands[k];
		if (op.type != PPC_OP_MEM)
		{
			continue;
		}
		uint32_t base = op.mem.base;
		if (base != PPC_REG_INVALID && base != PPC_REG_ZERO && base != PPC_REG_R0)
		{
			storeRegister(base, ea, irb);
		}
		return;
	}
	if (pi->op_count >= 2 && pi->operands[1].type == PPC_OP_REG && pi->operands[1].reg != PPC_REG_INVALID
		&& pi->operands[1].reg != PPC_REG_ZERO)
	{
		storeRegister(pi->operands[1].reg, ea, irb);
	}
}

/**
 * PPC_INS_STB, PPC_INS_STH, PPC_INS_STW,
 * PPC_INS_STBU, PPC_INS_STHU, PPC_INS_STWU,
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateStore(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case PPC_INS_STB:
		case PPC_INS_STBU:
			ty = irb.getInt8Ty();
			break;
		case PPC_INS_STH:
		case PPC_INS_STHU:
			ty = irb.getInt16Ty();
			break;
		case PPC_INS_STW:
		case PPC_INS_STWU:
			ty = irb.getInt32Ty();
			break;
		case PPC_INS_STD:
		case PPC_INS_STDU: ty = irb.getInt64Ty(); break;
		default:
			return;
	}

	op0 = loadOpBinaryOp0(pi, irb);
	if (ty->isIntegerTy())
	{
		op0 = irb.CreateZExtOrTrunc(op0, ty);
	}
	else
	{
		throw GenericError("unhandled type");
	}

	auto* si = storeOp(pi->operands[1], op0, irb);

	// With update.
	//
	auto& ppcOp1 = pi->operands[1];
	if (i->id == PPC_INS_STBU || i->id == PPC_INS_STHU || i->id == PPC_INS_STWU || i->id == PPC_INS_STDU)
		if (ppcOp1.type == PPC_OP_MEM && ppcOp1.mem.base != PPC_REG_INVALID)
			if (auto* s = llvm::dyn_cast<llvm::StoreInst>(si))
				if (auto* cast = llvm::dyn_cast<llvm::CastInst>(s->getPointerOperand()))
				{
					storeRegister(ppcOp1.mem.base, cast->getOperand(0), irb);
				}
}

/**
 * PPC_INS_STBX, PPC_INS_STHX, PPC_INS_STWX,
 * PPC_INS_STBUX, PPC_INS_STHUX, PPC_INS_STWUX,
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateStoreIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	op0 = loadOp(pi->operands[0], irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case PPC_INS_STBX:
		case PPC_INS_STBCIX:
		case PPC_INS_STBEPX:
		case PPC_INS_STBUX:
		case PPC_INS_STBCX:
			ty = irb.getInt8Ty();
			break;
		case PPC_INS_STHX:
		case PPC_INS_STHCIX:
		case PPC_INS_STHEPX:
		case PPC_INS_STHUX:
		case PPC_INS_STHCX:
			ty = irb.getInt16Ty();
			break;
		case PPC_INS_STWX:
		case PPC_INS_STWCIX:
		case PPC_INS_STWEPX:
		case PPC_INS_STWUX:
		case PPC_INS_STWCX:
			ty = irb.getInt32Ty();
			break;
		case PPC_INS_STDCX:
		case PPC_INS_STDX:
		case PPC_INS_STDCIX:
		case PPC_INS_STDUX: ty = irb.getInt64Ty(); break;
		default:
			return;
	}

	op0 = irb.CreateZExtOrTrunc(op0, ty);

	auto* add = loadIndexedEffectiveAddress(pi, irb);
	auto* st = storeIntPtr(irb, op0, add, ty);
	if (i->id == PPC_INS_STWCX || i->id == PPC_INS_STDCX || i->id == PPC_INS_STBCX || i->id == PPC_INS_STHCX)
	{
		st->setAtomic(llvm::AtomicOrdering::Monotonic);
		// Success. No exclusive-monitor model.
		storeRegister(PPC_REG_CR0EQ, irb.getInt1(true), irb);
	}

	if (i->id == PPC_INS_STBUX || i->id == PPC_INS_STHUX || i->id == PPC_INS_STWUX || i->id == PPC_INS_STDUX)
	{
		storeIndexedUpdate(pi, add, irb);
	}
}

/**
 * The FPRs are doubles. A single-precision PowerPC operation computes in
 * double and rounds the result to single before writing it back; the
 * round-trip through float is that rounding.
 */
llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::roundToSingle(llvm::IRBuilder<>& irb, llvm::Value* val)
{
	return irb.CreateFPExt(irb.CreateFPTrunc(val, irb.getFloatTy()), irb.getDoubleTy());
}

/**
 * Load @a op as a double, whatever it arrives as.
 *
 * VSX scalar forms name a 128-bit VSR. The ISA's leftmost doubleword is the
 * high 64 bits of that i128, which is also FPR n for vs0..vs31.
 */
llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::loadOpDouble(cs_ppc_op& op, llvm::IRBuilder<>& irb)
{
	auto* f64 = irb.getDoubleTy();
	auto* v = loadOp(op, irb);
	if (v == nullptr)
	{
		return llvm::UndefValue::get(f64);
	}
	if (v->getType()->isDoubleTy())
	{
		return v;
	}
	if (v->getType()->isFloatTy())
	{
		return irb.CreateFPExt(v, f64);
	}
	if (v->getType()->isIntegerTy(128))
	{
		return i128HighDouble(v, irb);
	}
	if (v->getType()->isIntegerTy(64))
	{
		return irb.CreateBitCast(v, f64);
	}
	return generateTypeConversion(irb, v, f64, eOpConv::FPCAST_OR_BITCAST);
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::i128HighDouble(llvm::Value* v, llvm::IRBuilder<>& irb)
{
	auto* hi = irb.CreateTrunc(
			irb.CreateLShr(v, llvm::ConstantInt::get(v->getType(), 64)),
			irb.getInt64Ty());
	return irb.CreateBitCast(hi, irb.getDoubleTy());
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::insertHighDouble(
		llvm::Value* vec, llvm::Value* d, llvm::IRBuilder<>& irb)
{
	if (!d->getType()->isDoubleTy())
	{
		d = irb.CreateFPExt(d, irb.getDoubleTy());
	}
	auto* bits = irb.CreateBitCast(d, irb.getInt64Ty());
	auto* hi = irb.CreateShl(irb.CreateZExt(bits, irb.getInt128Ty()), 64);
	auto* loMask = llvm::ConstantInt::get(irb.getInt128Ty(), llvm::APInt(128, UINT64_MAX));
	auto* lo = irb.CreateAnd(vec, loMask);
	return irb.CreateOr(hi, lo);
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::loadOpI128(cs_ppc_op& op, llvm::IRBuilder<>& irb)
{
	auto* v = loadOp(op, irb);
	auto* i128 = irb.getInt128Ty();
	if (v == nullptr)
	{
		return llvm::UndefValue::get(i128);
	}
	if (v->getType()->isIntegerTy(128))
	{
		return v;
	}
	if (v->getType()->isDoubleTy())
	{
		auto* bits = irb.CreateBitCast(v, irb.getInt64Ty());
		return irb.CreateShl(irb.CreateZExt(bits, i128), 64);
	}
	if (v->getType()->isFloatTy())
	{
		auto* bits = irb.CreateBitCast(v, irb.getInt32Ty());
		return irb.CreateShl(irb.CreateZExt(bits, i128), 96);
	}
	if (v->getType()->isIntegerTy())
	{
		return irb.CreateZExt(v, i128);
	}
	return irb.CreateBitCast(v, i128);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::storeOpI128(cs_ppc_op& op, llvm::Value* val, llvm::IRBuilder<>& irb)
{
	if (op.type == PPC_OP_REG)
	{
		uint32_t r = canonicalRegister(op.reg);
		auto* llvmReg = getRegister(r);
		if (llvmReg != nullptr && llvmReg->getValueType()->isDoubleTy())
		{
			storeRegister(r, i128HighDouble(val, irb), irb, eOpConv::FPCAST_OR_BITCAST);
			return;
		}
	}
	storeOp(op, val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::asVec128(
		llvm::Value* v, llvm::Type* elemTy, unsigned n, llvm::IRBuilder<>& irb)
{
	if (!v->getType()->isIntegerTy(128))
	{
		v = irb.CreateZExtOrTrunc(v, irb.getInt128Ty());
	}
	return irb.CreateBitCast(v, llvm::FixedVectorType::get(elemTy, n));
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::loadOpI256(cs_ppc_op& op, llvm::IRBuilder<>& irb)
{
	auto* v = loadOp(op, irb);
	auto* i256 = irb.getIntNTy(256);
	if (v == nullptr)
	{
		return llvm::UndefValue::get(i256);
	}
	if (v->getType()->isIntegerTy(256))
	{
		return v;
	}
	if (v->getType()->isDoubleTy())
	{
		// QPX slot 0 overlays the FPR: leftmost doubleword of the 256-bit
		// register, which is bits 192–255 of the integer view.
		auto* bits = irb.CreateBitCast(v, irb.getInt64Ty());
		return irb.CreateShl(irb.CreateZExt(bits, i256), 192);
	}
	if (v->getType()->isIntegerTy())
	{
		return irb.CreateZExt(v, i256);
	}
	return irb.CreateBitCast(v, i256);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::storeOpI256(cs_ppc_op& op, llvm::Value* val, llvm::IRBuilder<>& irb)
{
	if (!val->getType()->isIntegerTy(256))
	{
		val = irb.CreateZExtOrTrunc(val, irb.getIntNTy(256));
	}
	if (op.type == PPC_OP_REG)
	{
		uint32_t r = canonicalRegister(op.reg);
		auto* llvmReg = getRegister(r);
		if (llvmReg != nullptr && llvmReg->getValueType()->isDoubleTy())
		{
			auto* hi = irb.CreateTrunc(irb.CreateLShr(val, 192), irb.getInt64Ty());
			storeRegister(r, irb.CreateBitCast(hi, irb.getDoubleTy()), irb, eOpConv::FPCAST_OR_BITCAST);
			return;
		}
	}
	storeOp(op, val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::asVec256(
		llvm::Value* v, llvm::Type* elemTy, unsigned n, llvm::IRBuilder<>& irb)
{
	auto* i256 = irb.getIntNTy(256);
	if (!v->getType()->isIntegerTy(256))
	{
		v = irb.CreateZExtOrTrunc(v, i256);
	}
	return irb.CreateBitCast(v, llvm::FixedVectorType::get(elemTy, n));
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::qpxAsArithVec(
		llvm::Value* bits, bool sp, llvm::IRBuilder<>& irb)
{
	auto* dp = asVec256(bits, irb.getDoubleTy(), 4, irb);
	if (!sp)
	{
		return dp;
	}
	return irb.CreateFPTrunc(dp, llvm::FixedVectorType::get(irb.getFloatTy(), 4));
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::qpxFromArithVec(
		llvm::Value* vec, llvm::IRBuilder<>& irb)
{
	if (vec->getType()->isFPOrFPVectorTy()
			&& vec->getType()->getScalarType()->isFloatTy())
	{
		vec = irb.CreateFPExt(vec, llvm::FixedVectorType::get(irb.getDoubleTy(), 4));
	}
	return irb.CreateBitCast(vec, irb.getIntNTy(256));
}

bool Capstone2LlvmIrTranslatorPowerpc_impl::isQpxSingle(unsigned id)
{
	switch (id)
	{
	case PPC_INS_QVFADDS:
	case PPC_INS_QVFSUBS:
	case PPC_INS_QVFMULS:
	case PPC_INS_QVFMADDS:
	case PPC_INS_QVFMSUBS:
	case PPC_INS_QVFNMADDS:
	case PPC_INS_QVFNMSUBS:
	case PPC_INS_QVFCFIDS:
	case PPC_INS_QVFCFIDUS:
	case PPC_INS_QVFRES:
	case PPC_INS_QVFRSQRTES:
	case PPC_INS_QVFXMADDS:
	case PPC_INS_QVFXMULS:
	case PPC_INS_QVFXXCPNMADDS:
	case PPC_INS_QVFXXMADDS:
	case PPC_INS_QVFXXNPMADDS:
	case PPC_INS_QVLFSX:
	case PPC_INS_QVLFSUX:
	case PPC_INS_QVLFSXA:
	case PPC_INS_QVLFSUXA:
	case PPC_INS_QVSTFSX:
	case PPC_INS_QVSTFSUX:
	case PPC_INS_QVSTFSXA:
	case PPC_INS_QVSTFSUXA:
	case PPC_INS_QVSTFSXI:
	case PPC_INS_QVSTFSXIA:
	case PPC_INS_QVSTFSUXI:
	case PPC_INS_QVSTFSUXIA:
		return true;
	default:
		return false;
	}
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::fpIntrinsic(
	llvm::IRBuilder<>& irb, llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Value*> args)
{
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, id, irb.getDoubleTy());
	return irb.CreateCall(f, args);
}

/**
 * Is @a id one of the single-precision forms? Those compute in double and
 * round the result to single before writing it back.
 */
bool Capstone2LlvmIrTranslatorPowerpc_impl::isSinglePrecisionForm(unsigned id)
{
	switch (id)
	{
	case PPC_INS_FADDS:
	case PPC_INS_FSUBS:
	case PPC_INS_FMULS:
	case PPC_INS_FDIVS:
	case PPC_INS_FSQRTS:
	case PPC_INS_FRES:
	case PPC_INS_FRSQRTES:
	case PPC_INS_FMADDS:
	case PPC_INS_FMSUBS:
	case PPC_INS_FNMADDS:
	case PPC_INS_FNMSUBS:
	case PPC_INS_FCFIDS:
	case PPC_INS_FCFIDUS:
	case PPC_INS_XSADDSP:
	case PPC_INS_XSSUBSP:
	case PPC_INS_XSMULSP:
	case PPC_INS_XSDIVSP:
	case PPC_INS_XSSQRTSP:
	case PPC_INS_XSRESP:
	case PPC_INS_XSRSQRTESP:
	case PPC_INS_XSRSP:
	case PPC_INS_XSCVSXDSP:
	case PPC_INS_XSCVUXDSP:
	case PPC_INS_XSMADDASP:
	case PPC_INS_XSMADDMSP:
	case PPC_INS_XSMSUBASP:
	case PPC_INS_XSMSUBMSP:
	case PPC_INS_XSNMADDASP:
	case PPC_INS_XSNMADDMSP:
	case PPC_INS_XSNMSUBASP:
	case PPC_INS_XSNMSUBMSP: return true;
	default: return false;
	}
}

/**
 * PPC_INS_FADD, PPC_INS_FADDS, PPC_INS_FSUB, PPC_INS_FSUBS,
 * PPC_INS_FMUL, PPC_INS_FMULS, PPC_INS_FDIV, PPC_INS_FDIVS,
 * PPC_INS_FCPSGN
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateFpArithm(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	op1 = loadOpDouble(pi->operands[1], irb);
	op2 = loadOpDouble(pi->operands[2], irb);

	llvm::Value* val = nullptr;
	switch (i->id)
	{
	case PPC_INS_FADD:
	case PPC_INS_FADDS:
	case PPC_INS_XSADDDP:
	case PPC_INS_XSADDSP: val = irb.CreateFAdd(op1, op2); break;
	case PPC_INS_FSUB:
	case PPC_INS_FSUBS:
	case PPC_INS_XSSUBDP:
	case PPC_INS_XSSUBSP: val = irb.CreateFSub(op1, op2); break;
	case PPC_INS_FMUL:
	case PPC_INS_FMULS:
	case PPC_INS_XSMULDP:
	case PPC_INS_XSMULSP: val = irb.CreateFMul(op1, op2); break;
	case PPC_INS_FDIV:
	case PPC_INS_FDIVS:
	case PPC_INS_XSDIVDP:
	case PPC_INS_XSDIVSP: val = irb.CreateFDiv(op1, op2); break;
	// fcpsgn FRT, FRA, FRB takes the magnitude of FRB and the sign of FRA.
	// llvm.copysign takes (magnitude, sign), so the operands swap.
	case PPC_INS_FCPSGN:
	case PPC_INS_XSCPSGNDP: val = fpIntrinsic(irb, llvm::Intrinsic::copysign, {op2, op1}); break;
	case PPC_INS_XSMAXDP: val = fpIntrinsic(irb, llvm::Intrinsic::maxnum, {op1, op2}); break;
	case PPC_INS_XSMINDP: val = fpIntrinsic(irb, llvm::Intrinsic::minnum, {op1, op2}); break;
	// The C-type forms are the C `a > b ? a : b` reading; the J-type forms
	// follow Java's NaN-propagating max/min.
	case PPC_INS_XSMAXCDP: val = irb.CreateSelect(irb.CreateFCmpOGT(op1, op2), op1, op2); break;
	case PPC_INS_XSMINCDP: val = irb.CreateSelect(irb.CreateFCmpOLT(op1, op2), op1, op2); break;
	case PPC_INS_XSMAXJDP:
	case PPC_INS_XSMINJDP:
	{
		auto id = i->id == PPC_INS_XSMAXJDP ? llvm::Intrinsic::maxnum : llvm::Intrinsic::minnum;
		val = fpIntrinsic(irb, id, {op1, op2});
		val = irb.CreateSelect(irb.CreateFCmpUNO(op2, op2), op2, val);
		val = irb.CreateSelect(irb.CreateFCmpUNO(op1, op1), op1, val);
		break;
	}
	case PPC_INS_XSCMPEQDP:
	case PPC_INS_XSCMPGEDP:
	case PPC_INS_XSCMPGTDP:
	{
		auto* c = i->id == PPC_INS_XSCMPEQDP ? irb.CreateFCmpOEQ(op1, op2)
				: i->id == PPC_INS_XSCMPGEDP ? irb.CreateFCmpOGE(op1, op2)
				: irb.CreateFCmpOGT(op1, op2);
		auto* i128 = irb.getInt128Ty();
		auto* m = irb.CreateShl(irb.CreateZExt(irb.CreateSExt(c, irb.getInt64Ty()), i128), 64);
		storeOpI128(pi->operands[0], m, irb);
		return;
	}
	default: return;
	}

	if (isSinglePrecisionForm(i->id))
	{
		val = roundToSingle(irb, val);
	}
	storeOp(pi->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * The unary floating-point forms, written FRT, FRB.
 *
 * PPC_INS_FMR, PPC_INS_FNEG, PPC_INS_FABS, PPC_INS_FNABS,
 * PPC_INS_FSQRT, PPC_INS_FSQRTS, PPC_INS_FRSP,
 * PPC_INS_FRE, PPC_INS_FRES, PPC_INS_FRSQRTE, PPC_INS_FRSQRTES,
 * PPC_INS_FRIM, PPC_INS_FRIN, PPC_INS_FRIP, PPC_INS_FRIZ,
 * PPC_INS_FCTIW, PPC_INS_FCTIWZ, PPC_INS_FCTIWUZ,
 * PPC_INS_FCTID, PPC_INS_FCTIDZ, PPC_INS_FCTIDUZ,
 * PPC_INS_FCFID, PPC_INS_FCFIDS, PPC_INS_FCFIDU, PPC_INS_FCFIDUS
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateFpUnary(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	auto* f64 = irb.getDoubleTy();

	// The conversion instructions do not read or write their FPR as a number:
	// fcfid reads a 64-bit integer out of one, and the fctiw/fctid family
	// leaves one there. They load and store differently from everything else,
	// so they come first.
	switch (i->id)
	{
	case PPC_INS_FCFID:
	case PPC_INS_FCFIDS:
	case PPC_INS_FCFIDU:
	case PPC_INS_FCFIDUS:
	case PPC_INS_XSCVSXDDP:
	case PPC_INS_XSCVUXDDP:
	case PPC_INS_XSCVSXDSP:
	case PPC_INS_XSCVUXDSP: {
		op1 = loadOp(pi->operands[1], irb);
		op1 = generateTypeConversion(irb, op1, irb.getInt64Ty(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
		bool uns = (i->id == PPC_INS_FCFIDU || i->id == PPC_INS_FCFIDUS || i->id == PPC_INS_XSCVUXDDP
				|| i->id == PPC_INS_XSCVUXDSP);
		llvm::Value* val = uns ? irb.CreateUIToFP(op1, f64) : irb.CreateSIToFP(op1, f64);
		if (isSinglePrecisionForm(i->id))
		{
			val = roundToSingle(irb, val);
		}
		storeOp(pi->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}
	case PPC_INS_FCTIW:
	case PPC_INS_FCTIWZ:
	case PPC_INS_FCTIWUZ:
	case PPC_INS_FCTIWU:
	case PPC_INS_FCTIDU:
	case PPC_INS_FCTID:
	case PPC_INS_FCTIDZ:
	case PPC_INS_FCTIDUZ:
	case PPC_INS_XSCVDPSXDS:
	case PPC_INS_XSCVDPSXWS:
	case PPC_INS_XSCVDPUXDS:
	case PPC_INS_XSCVDPUXWS: {
		op1 = loadOpDouble(pi->operands[1], irb);
		// The Z forms truncate. The other two round by FPSCR[RN], which is
		// not modelled; round-to-nearest-even is its reset value and what
		// every ABI leaves it at.
		if (i->id == PPC_INS_FCTIW || i->id == PPC_INS_FCTID || i->id == PPC_INS_FCTIWU
				|| i->id == PPC_INS_FCTIDU)
		{
			op1 = fpIntrinsic(irb, llvm::Intrinsic::roundeven, {op1});
		}
		bool wide = (i->id == PPC_INS_FCTID || i->id == PPC_INS_FCTIDZ || i->id == PPC_INS_FCTIDUZ
				|| i->id == PPC_INS_FCTIDU
				|| i->id == PPC_INS_XSCVDPSXDS || i->id == PPC_INS_XSCVDPUXDS);
		bool uns = (i->id == PPC_INS_FCTIWUZ || i->id == PPC_INS_FCTIDUZ || i->id == PPC_INS_FCTIWU
				|| i->id == PPC_INS_FCTIDU
				|| i->id == PPC_INS_XSCVDPUXDS || i->id == PPC_INS_XSCVDPUXWS);
		auto* ity = wide ? irb.getInt64Ty() : irb.getInt32Ty();
		llvm::Value* val = generateFpToIntBounded(op1, ity, !uns, irb);
		// The W forms leave the result in the low 32 bits and the ISA
		// leaves the high 32 undefined. Widening by the result's own
		// signedness is a defined choice; for the D forms these are
		// no-ops.
		val = uns ? irb.CreateZExtOrTrunc(val, irb.getInt64Ty()) : irb.CreateSExtOrTrunc(val, irb.getInt64Ty());
		// FPCAST_OR_BITCAST, not ZEXT_TRUNC_OR_BITCAST: the destination is
		// an f64 register and the value is a bit pattern to be reinterpreted,
		// not a number to be converted. ZEXT_TRUNC_OR_BITCAST throws on a
		// non-integer destination; this bitcasts i64 to double, which is
		// what the FPR holds after fctiw.
		storeOp(pi->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}
	default: break;
	}

	if (i->id == PPC_INS_XSCVDPSP)
	{
		// Single-precision format in words 0 and 1 (as POWER hardware does;
		// word 1 is architecturally undefined), doubleword 1 zeroed.
		auto* d = loadOpDouble(pi->operands[1], irb);
		auto* bits = irb.CreateZExt(
				irb.CreateBitCast(irb.CreateFPTrunc(d, irb.getFloatTy()), irb.getInt32Ty()),
				irb.getInt128Ty());
		storeOpI128(pi->operands[0], irb.CreateOr(irb.CreateShl(bits, 96), irb.CreateShl(bits, 64)), irb);
		return;
	}
	if (i->id == PPC_INS_XSCVDPSPN)
	{
		// Single-precision result in word 0 of the target.
		auto* d = loadOpDouble(pi->operands[1], irb);
		auto* bits = irb.CreateBitCast(irb.CreateFPTrunc(d, irb.getFloatTy()), irb.getInt32Ty());
		storeOpI128(pi->operands[0], irb.CreateShl(irb.CreateZExt(bits, irb.getInt128Ty()), 96), irb);
		return;
	}
	if (i->id == PPC_INS_XSCVSPDP || i->id == PPC_INS_XSCVSPDPN)
	{
		auto* raw = loadOp(pi->operands[1], irb);
		llvm::Value* bits64 = nullptr;
		if (raw != nullptr && raw->getType()->isIntegerTy(128))
		{
			bits64 = irb.CreateTrunc(irb.CreateLShr(raw, 64), irb.getInt64Ty());
		}
		else
		{
			auto* d = loadOpDouble(pi->operands[1], irb);
			bits64 = irb.CreateBitCast(d, irb.getInt64Ty());
		}
		auto* sp = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(bits64, 32), irb.getInt32Ty()), irb.getFloatTy());
		storeOp(pi->operands[0], irb.CreateFPExt(sp, f64), irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}

	auto* one = llvm::ConstantFP::get(f64, 1.0);
	op1 = loadOpDouble(pi->operands[1], irb);

	llvm::Value* val = nullptr;
	switch (i->id)
	{
	case PPC_INS_FMR: val = op1; break;
	case PPC_INS_FNEG:
	case PPC_INS_XSNEGDP: val = irb.CreateFNeg(op1); break;
	case PPC_INS_FABS:
	case PPC_INS_XSABSDP: val = fpIntrinsic(irb, llvm::Intrinsic::fabs, {op1}); break;
	case PPC_INS_FNABS:
	case PPC_INS_XSNABSDP: val = irb.CreateFNeg(fpIntrinsic(irb, llvm::Intrinsic::fabs, {op1})); break;
	case PPC_INS_FSQRT:
	case PPC_INS_FSQRTS:
	case PPC_INS_XSSQRTDP:
	case PPC_INS_XSSQRTSP: val = fpIntrinsic(irb, llvm::Intrinsic::sqrt, {op1}); break;
	case PPC_INS_XSRSP: val = op1; break;
	case PPC_INS_FRSP:
	case PPC_INS_XSCVDPSP: val = roundToSingle(irb, op1); break;
	// The estimate instructions are specified only to within a relative
	// error bound, so the exact operation is the honest reading of them.
	case PPC_INS_FRE:
	case PPC_INS_FRES:
	case PPC_INS_XSREDP:
	case PPC_INS_XSRESP: val = irb.CreateFDiv(one, op1); break;
	case PPC_INS_FRSQRTE:
	case PPC_INS_FRSQRTES:
	case PPC_INS_XSRSQRTEDP:
	case PPC_INS_XSRSQRTESP: val = irb.CreateFDiv(one, fpIntrinsic(irb, llvm::Intrinsic::sqrt, {op1})); break;
	case PPC_INS_FRIM:
	case PPC_INS_XSRDPIM: val = fpIntrinsic(irb, llvm::Intrinsic::floor, {op1}); break;
	case PPC_INS_FRIP:
	case PPC_INS_XSRDPIP: val = fpIntrinsic(irb, llvm::Intrinsic::ceil, {op1}); break;
	case PPC_INS_FRIZ:
	case PPC_INS_XSRDPIZ: val = fpIntrinsic(irb, llvm::Intrinsic::trunc, {op1}); break;
	case PPC_INS_FRIN:
	case PPC_INS_XSRDPI: val = fpIntrinsic(irb, llvm::Intrinsic::roundeven, {op1}); break;
	case PPC_INS_XSRDPIC: val = fpIntrinsic(irb, llvm::Intrinsic::nearbyint, {op1}); break;
	default: return;
	}

	if (isSinglePrecisionForm(i->id))
	{
		val = roundToSingle(irb, val);
	}
	storeOp(pi->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * The fused multiply-add family and fsel, all written FRT, FRA, FRC, FRB.
 *
 * PPC_INS_FMADD, PPC_INS_FMADDS, PPC_INS_FMSUB, PPC_INS_FMSUBS,
 * PPC_INS_FNMADD, PPC_INS_FNMADDS, PPC_INS_FNMSUB, PPC_INS_FNMSUBS,
 * PPC_INS_FSEL
 *
 * Operand order is the thing these cannot be written without settling: the
 * assembly names FRA, FRC, FRB in that order, so operands[2] is the
 * multiplicand and operands[3] the addend. The tests pin it, so if capstone
 * ever disagrees they say so instead of the arithmetic being quietly wrong.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateFpTernary(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);

	auto* a = loadOpDouble(pi->operands[1], irb);
	auto* c = loadOpDouble(pi->operands[2], irb);
	auto* b = loadOpDouble(pi->operands[3], irb);

	if (i->id == PPC_INS_FSEL)
	{
		// FRT = FRA >= 0.0 ? FRC : FRB. The comparison is ordered, so a NaN in
		// FRA selects FRB -- which is what the ISA says.
		auto* zero = llvm::ConstantFP::get(irb.getDoubleTy(), 0.0);
		auto* sel = irb.CreateSelect(irb.CreateFCmpOGE(a, zero), c, b);
		storeOp(pi->operands[0], sel, irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}

	bool negateAddend =
		(i->id == PPC_INS_FMSUB || i->id == PPC_INS_FMSUBS || i->id == PPC_INS_FNMSUB || i->id == PPC_INS_FNMSUBS);
	bool negateResult =
		(i->id == PPC_INS_FNMADD || i->id == PPC_INS_FNMADDS || i->id == PPC_INS_FNMSUB || i->id == PPC_INS_FNMSUBS);

	llvm::Value* val = fpIntrinsic(irb, llvm::Intrinsic::fma, {a, c, negateAddend ? irb.CreateFNeg(b) : b});
	if (negateResult)
	{
		val = irb.CreateFNeg(val);
	}
	if (isSinglePrecisionForm(i->id))
	{
		val = roundToSingle(irb, val);
	}
	storeOp(pi->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * PPC_INS_FCMPU
 *
 * Not storeCrX: that builds integer comparisons, and it writes a constant zero
 * into the fourth bit of the field because for the integer compares that bit
 * is a copy of XER, which is not modelled. Here the fourth bit is FU,
 * "unordered", and it is the only thing that distinguishes comparing a NaN
 * from comparing two equal numbers.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateFcmp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	uint32_t crReg = PPC_REG_CR0;
	cs_ppc_op* aOp = &pi->operands[0];
	cs_ppc_op* bOp = &pi->operands[1];
	if (pi->op_count == 3)
	{
		if (pi->operands[0].type == PPC_OP_REG && pi->operands[0].reg >= PPC_REG_CR0
				&& pi->operands[0].reg <= PPC_REG_CR7)
		{
			crReg = pi->operands[0].reg;
		}
		aOp = &pi->operands[1];
		bOp = &pi->operands[2];
	}

	auto* a = loadOpDouble(*aOp, irb);
	auto* b = loadOpDouble(*bOp, irb);

	uint32_t ltR = PPC_REG_CR0LT;
	uint32_t gtR = PPC_REG_CR0GT;
	uint32_t eqR = PPC_REG_CR0EQ;
	uint32_t unR = PPC_REG_CR0UN;
	crFieldRegisters(crReg, ltR, gtR, eqR, unR);

	storeRegister(ltR, irb.CreateFCmpOLT(a, b), irb);
	storeRegister(gtR, irb.CreateFCmpOGT(a, b), irb);
	storeRegister(eqR, irb.CreateFCmpOEQ(a, b), irb);
	storeRegister(unR, irb.CreateFCmpUNO(a, b), irb);
}

/**
 * On Gekko/Broadway every single-precision result -- arithmetic, frsp and
 * the lfs family -- is written to both paired-single slots.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::copySingleResultToPs1(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	switch (i->id)
	{
	case PPC_INS_FADDS: case PPC_INS_FSUBS: case PPC_INS_FMULS: case PPC_INS_FDIVS:
	case PPC_INS_FMADDS: case PPC_INS_FMSUBS: case PPC_INS_FNMADDS: case PPC_INS_FNMSUBS:
	case PPC_INS_FRES: case PPC_INS_FRSP: case PPC_INS_FSQRTS:
	case PPC_INS_LFS: case PPC_INS_LFSU: case PPC_INS_LFSX: case PPC_INS_LFSUX:
		break;
	default:
		return;
	}
	if (pi->op_count == 0 || pi->operands[0].type != PPC_OP_REG
			|| pi->operands[0].reg < PPC_REG_F0 || pi->operands[0].reg > PPC_REG_F31)
	{
		return;
	}
	auto* v = loadRegister(pi->operands[0].reg, irb);
	irb.CreateStore(irb.CreateFPCast(v, irb.getDoubleTy()), getPs1(pi->operands[0].reg));
}

/**
 * PPC_INS_MTSPR / PPC_INS_MFSPR. In paired-single mode SPRs 912-919 are the
 * GQRs that PSQ_L/PSQ_ST read, so they are state rather than opaque calls.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMtspr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if ((static_cast<unsigned>(_extraMode) & CS_MODE_PS) && pi->op_count == 2
			&& pi->operands[0].type == PPC_OP_IMM && pi->operands[0].imm >= 912 && pi->operands[0].imm <= 919
			&& pi->operands[1].type == PPC_OP_REG)
	{
		auto* v = irb.CreateZExtOrTrunc(loadRegister(pi->operands[1].reg, irb), irb.getInt32Ty());
		irb.CreateStore(v, getGqr(unsigned(pi->operands[0].imm - 912)));
		return;
	}
	translatePseudoAsmFncOp0Op1(i, pi, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateMfspr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if ((static_cast<unsigned>(_extraMode) & CS_MODE_PS) && pi->op_count == 2
			&& pi->operands[1].type == PPC_OP_IMM && pi->operands[1].imm >= 912 && pi->operands[1].imm <= 919
			&& pi->operands[0].type == PPC_OP_REG)
	{
		auto* v = irb.CreateLoad(irb.getInt32Ty(), getGqr(unsigned(pi->operands[1].imm - 912)));
		storeRegister(pi->operands[0].reg, v, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	translatePseudoAsmOp0FncOp1(i, pi, irb);
}

/**
 * PPC_INS_PSQ_L(U)(X), PPC_INS_PSQ_ST(U)(X) -- quantized paired-single
 * load/store: psq_l fD, d(rA), W, I  /  psq_lx fD, rA, rB, W, I.
 *
 * GQR[I] gives, per direction, a type (0 float, 4 u8, 5 u16, 6 s8, 7 s16)
 * and a signed 6-bit scale: loads compute int * 2^-scale, stores
 * clamp(round(v * 2^scale)). W=1 transfers ps0 only and a load sets ps1
 * to 1.0. The type is read at run time, so each element is a select over
 * the three widths.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translatePsq(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	bool store = i->id == PPC_INS_PSQ_ST || i->id == PPC_INS_PSQ_STU
			|| i->id == PPC_INS_PSQ_STX || i->id == PPC_INS_PSQ_STUX;
	bool update = i->id == PPC_INS_PSQ_LU || i->id == PPC_INS_PSQ_STU
			|| i->id == PPC_INS_PSQ_LUX || i->id == PPC_INS_PSQ_STUX;
	unsigned n = pi->op_count;
	if (n < 4 || pi->operands[0].type != PPC_OP_REG
			|| pi->operands[0].reg < PPC_REG_F0 || pi->operands[0].reg > PPC_REG_F31
			|| pi->operands[n - 1].type != PPC_OP_IMM || pi->operands[n - 2].type != PPC_OP_IMM)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	bool w = pi->operands[n - 2].imm != 0;
	unsigned gq = unsigned(pi->operands[n - 1].imm) & 7;
	auto* at = getDefaultType();
	llvm::Value* ea = nullptr;
	uint32_t updReg = PPC_REG_INVALID;
	if (pi->operands[1].type == PPC_OP_MEM)
	{
		ea = irb.CreateZExtOrTrunc(loadOp(pi->operands[1], irb, nullptr, true), at);
		updReg = pi->operands[1].mem.base;
	}
	else if (n == 5 && pi->operands[1].type == PPC_OP_REG && pi->operands[2].type == PPC_OP_REG)
	{
		auto* b = irb.CreateZExtOrTrunc(loadRegister(pi->operands[2].reg, irb), at);
		uint32_t a = pi->operands[1].reg;
		ea = (a == PPC_REG_R0 || a == PPC_REG_ZERO || a == PPC_REG_INVALID)
				? b : irb.CreateAdd(irb.CreateZExtOrTrunc(loadRegister(a, irb), at), b);
		updReg = a;
	}
	else if (n == 4 && pi->operands[1].type == PPC_OP_REG)
	{
		ea = irb.CreateZExtOrTrunc(loadRegister(pi->operands[1].reg, irb), at);
	}
	if (ea == nullptr)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	auto* i32 = irb.getInt32Ty();
	auto* f32 = irb.getFloatTy();
	auto* d = irb.getDoubleTy();
	llvm::Value* gqr = irb.CreateLoad(i32, getGqr(gq));
	llvm::Value* type = irb.CreateAnd(irb.CreateLShr(gqr, store ? 0 : 16), 7);
	llvm::Value* scale = irb.CreateAnd(irb.CreateLShr(gqr, store ? 8 : 24), 0x3f);
	scale = irb.CreateAShr(irb.CreateShl(scale, 26), 26); // sign-extend 6 bits
	if (!store) scale = irb.CreateNeg(scale);
	// 2^scale as a double, built from the exponent field.
	llvm::Value* pow2 = irb.CreateBitCast(
			irb.CreateShl(irb.CreateZExt(irb.CreateAdd(scale, irb.getInt32(1023)), irb.getInt64Ty()), 52), d);
	auto isT = [&](llvm::IRBuilder<>& b, unsigned t) { return b.CreateICmpEQ(type, b.getInt32(t)); };
	llvm::Value* isFloat = irb.CreateICmpULT(type, irb.getInt32(4));
	llvm::Value* isByte = irb.CreateOr(isT(irb, 4), isT(irb, 6));
	unsigned elems = w ? 1 : 2;
	uint32_t fr = pi->operands[0].reg;
	llvm::Value* src[2] = {nullptr, nullptr};
	if (store)
	{
		src[0] = irb.CreateFPCast(loadRegister(fr, irb), d);
		src[1] = irb.CreateLoad(d, getPs1(fr));
	}

	// One path per memory width (float / byte / halfword), each with
	// exactly the accesses the hardware makes -- psq_st is how Gekko code
	// feeds the write-gather pipe, so no extra reads or writes.
	// Returns the loaded (ps0, ps1) for loads.
	auto path = [&](llvm::IRBuilder<>& b, unsigned bytes) -> std::pair<llvm::Value*, llvm::Value*> {
		llvm::Value* out[2] = {llvm::ConstantFP::get(d, 1.0), llvm::ConstantFP::get(d, 1.0)};
		llvm::Type* it = b.getIntNTy(bytes * 8);
		for (unsigned k = 0; k < elems; ++k)
		{
			llvm::Value* a = k == 0 ? ea : b.CreateAdd(ea, llvm::ConstantInt::get(at, bytes));
			if (bytes == 4)
			{
				if (store) storeIntPtr(b, b.CreateFPTrunc(src[k], f32), a, f32);
				else out[k] = b.CreateFPExt(loadIntPtr(b, a, f32), d);
				continue;
			}
			llvm::Value* sgn = isT(b, bytes == 1 ? 6 : 7);
			if (store)
			{
				llvm::Value* q = b.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, b.CreateFMul(src[k], pow2));
				double ulo = 0, uhi = bytes == 1 ? 255 : 65535;
				double slo = bytes == 1 ? -128 : -32768, shi = bytes == 1 ? 127 : 32767;
				auto clamp = [&](double lo, double hi) {
					llvm::Value* c = b.CreateIntrinsic(llvm::Intrinsic::maxnum, {d}, {q, llvm::ConstantFP::get(d, lo)});
					return b.CreateIntrinsic(llvm::Intrinsic::minnum, {d}, {c, llvm::ConstantFP::get(d, hi)});
				};
				llvm::Value* sv = b.CreateFPToSI(clamp(slo, shi), it);
				llvm::Value* uv = b.CreateFPToUI(clamp(ulo, uhi), it);
				storeIntPtr(b, b.CreateSelect(sgn, sv, uv), a, it);
			}
			else
			{
				llvm::Value* v = loadIntPtr(b, a, it);
				llvm::Value* f = b.CreateSelect(sgn, b.CreateSIToFP(v, d), b.CreateUIToFP(v, d));
				out[k] = b.CreateFMul(f, pow2);
			}
		}
		return {out[0], out[1]};
	};

	auto [fBody, iBody] = generateIfThenElse(isFloat, irb);
	llvm::IRBuilder<> fb(fBody);
	auto fv = path(fb, 4);
	llvm::IRBuilder<> ib(iBody);
	auto [bBody, hBody] = generateIfThenElse(isByte, ib);
	llvm::IRBuilder<> bb(bBody);
	auto bv = path(bb, 1);
	llvm::IRBuilder<> hb(hBody);
	auto hv = path(hb, 2);
	if (!store)
	{
		auto phi2 = [&](llvm::IRBuilder<>& at2, llvm::Value* x, llvm::BasicBlock* xb, llvm::Value* y, llvm::BasicBlock* yb) {
			auto* p = at2.CreatePHI(d, 2);
			p->addIncoming(x, xb);
			p->addIncoming(y, yb);
			return p;
		};
		// ib now sits after the inner if/else, irb after the outer one.
		llvm::Value* i0 = phi2(ib, bv.first, bBody->getParent(), hv.first, hBody->getParent());
		llvm::Value* i1 = phi2(ib, bv.second, bBody->getParent(), hv.second, hBody->getParent());
		llvm::BasicBlock* intEnd = ib.GetInsertBlock();
		llvm::Value* v0 = phi2(irb, fv.first, fBody->getParent(), i0, intEnd);
		llvm::Value* v1 = phi2(irb, fv.second, fBody->getParent(), i1, intEnd);
		storeRegister(fr, v0, irb, eOpConv::FPCAST_OR_BITCAST);
		irb.CreateStore(w ? llvm::ConstantFP::get(d, 1.0) : v1, getPs1(fr));
	}
	if (update && updReg != PPC_REG_INVALID && updReg != PPC_REG_R0 && updReg != PPC_REG_ZERO)
	{
		storeRegister(updReg, ea, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
}

/**
 * Paired-single (Gekko/Broadway, CS_MODE_PS) arithmetic, moves, selects
 * and compares. ps0 is the FPR itself; ps1 is the ps1_fN global. Every
 * arithmetic result is rounded to single precision, as the hardware does.
 * Operand order is the assembler's: ps_madd fD, fA, fC, fB.
 *
 *   ps_sum0  D = (A0 + B1, C1)        ps_sum1  D = (C0, A0 + B1)
 *   ps_muls0 D = (A0*C0, A1*C0)       ps_muls1 D = (A0*C1, A1*C1)
 *   ps_madds0/1 likewise, plus B      ps_mergeXY D = (A.X, B.Y)
 *   ps_sel   D = A >= 0 ? C : B       ps_cmpXY crD, A, B compares slot X/Y
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translatePairedSingle(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto isF = [&](unsigned j) {
		return j < pi->op_count && pi->operands[j].type == PPC_OP_REG
				&& pi->operands[j].reg >= PPC_REG_F0 && pi->operands[j].reg <= PPC_REG_F31;
	};
	auto* d = irb.getDoubleTy();
	auto ps0 = [&](unsigned j) { return loadOpDouble(pi->operands[j], irb); };
	auto ps1 = [&](unsigned j) { return irb.CreateLoad(d, getPs1(pi->operands[j].reg)); };
	auto single = [&](llvm::Value* v) { return irb.CreateFPExt(irb.CreateFPTrunc(v, irb.getFloatTy()), d); };
	auto put = [&](llvm::Value* a, llvm::Value* b) {
		storeRegister(pi->operands[0].reg, a, irb, eOpConv::FPCAST_OR_BITCAST);
		irb.CreateStore(b, getPs1(pi->operands[0].reg));
	};
	auto putS = [&](llvm::Value* a, llvm::Value* b) { put(single(a), single(b)); };

	switch (i->id)
	{
	case PPC_INS_PS_CMPU0:
	case PPC_INS_PS_CMPO0:
	case PPC_INS_PS_CMPU1:
	case PPC_INS_PS_CMPO1:
	{
		if (pi->op_count != 3 || !isF(1) || !isF(2) || pi->operands[0].type != PPC_OP_REG
				|| pi->operands[0].reg < PPC_REG_CR0 || pi->operands[0].reg > PPC_REG_CR7)
		{
			break;
		}
		bool one = i->id == PPC_INS_PS_CMPU1 || i->id == PPC_INS_PS_CMPO1;
		llvm::Value* a = one ? ps1(1) : ps0(1);
		llvm::Value* b = one ? ps1(2) : ps0(2);
		uint32_t lt = PPC_REG_CR0LT, gt = PPC_REG_CR0GT, eq = PPC_REG_CR0EQ, un = PPC_REG_CR0UN;
		crFieldRegisters(pi->operands[0].reg, lt, gt, eq, un);
		storeRegister(lt, irb.CreateFCmpOLT(a, b), irb);
		storeRegister(gt, irb.CreateFCmpOGT(a, b), irb);
		storeRegister(eq, irb.CreateFCmpOEQ(a, b), irb);
		storeRegister(un, irb.CreateFCmpUNO(a, b), irb);
		return;
	}
	case PPC_INS_PS_MR:
	case PPC_INS_PS_NEG:
	case PPC_INS_PS_ABS:
	case PPC_INS_PS_NABS:
	case PPC_INS_PS_RES:
	case PPC_INS_PS_RSQRTE:
	{
		if (pi->op_count != 2 || !isF(0) || !isF(1)) break;
		auto f = [&](llvm::Value* v) -> llvm::Value* {
			switch (i->id)
			{
			case PPC_INS_PS_NEG: return irb.CreateFNeg(v);
			case PPC_INS_PS_ABS: return irb.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, v);
			case PPC_INS_PS_NABS: return irb.CreateFNeg(irb.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, v));
			case PPC_INS_PS_RES: return single(irb.CreateFDiv(llvm::ConstantFP::get(d, 1.0), v));
			case PPC_INS_PS_RSQRTE:
				return single(irb.CreateFDiv(llvm::ConstantFP::get(d, 1.0),
						irb.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, v)));
			default: return v;
			}
		};
		llvm::Value* a = ps0(1);
		llvm::Value* b = ps1(1);
		put(f(a), f(b));
		return;
	}
	case PPC_INS_PS_ADD:
	case PPC_INS_PS_SUB:
	case PPC_INS_PS_MUL:
	case PPC_INS_PS_DIV:
	case PPC_INS_PS_MERGE00:
	case PPC_INS_PS_MERGE01:
	case PPC_INS_PS_MERGE10:
	case PPC_INS_PS_MERGE11:
	case PPC_INS_PS_MULS0:
	case PPC_INS_PS_MULS1:
	{
		if (pi->op_count != 3 || !isF(0) || !isF(1) || !isF(2)) break;
		llvm::Value* a0 = ps0(1);
		llvm::Value* a1 = ps1(1);
		llvm::Value* b0 = ps0(2);
		llvm::Value* b1 = ps1(2);
		switch (i->id)
		{
		case PPC_INS_PS_ADD: putS(irb.CreateFAdd(a0, b0), irb.CreateFAdd(a1, b1)); break;
		case PPC_INS_PS_SUB: putS(irb.CreateFSub(a0, b0), irb.CreateFSub(a1, b1)); break;
		case PPC_INS_PS_MUL: putS(irb.CreateFMul(a0, b0), irb.CreateFMul(a1, b1)); break;
		case PPC_INS_PS_DIV: putS(irb.CreateFDiv(a0, b0), irb.CreateFDiv(a1, b1)); break;
		case PPC_INS_PS_MERGE00: put(single(a0), single(b0)); break;
		case PPC_INS_PS_MERGE01: put(single(a0), single(b1)); break;
		case PPC_INS_PS_MERGE10: put(single(a1), single(b0)); break;
		case PPC_INS_PS_MERGE11: put(single(a1), single(b1)); break;
		case PPC_INS_PS_MULS0: putS(irb.CreateFMul(a0, b0), irb.CreateFMul(a1, b0)); break;
		case PPC_INS_PS_MULS1: putS(irb.CreateFMul(a0, b1), irb.CreateFMul(a1, b1)); break;
		default: break;
		}
		return;
	}
	case PPC_INS_PS_MADD:
	case PPC_INS_PS_MSUB:
	case PPC_INS_PS_NMADD:
	case PPC_INS_PS_NMSUB:
	case PPC_INS_PS_MADDS0:
	case PPC_INS_PS_MADDS1:
	case PPC_INS_PS_SUM0:
	case PPC_INS_PS_SUM1:
	case PPC_INS_PS_SEL:
	{
		if (pi->op_count != 4 || !isF(0) || !isF(1) || !isF(2) || !isF(3)) break;
		llvm::Value* a0 = ps0(1);
		llvm::Value* a1 = ps1(1);
		llvm::Value* c0 = ps0(2);
		llvm::Value* c1 = ps1(2);
		llvm::Value* b0 = ps0(3);
		llvm::Value* b1 = ps1(3);
		auto fma = [&](llvm::Value* x, llvm::Value* y, llvm::Value* z, bool sub, bool neg) {
			llvm::Value* r = irb.CreateIntrinsic(llvm::Intrinsic::fma, {d}, {x, y, sub ? irb.CreateFNeg(z) : z});
			return neg ? irb.CreateFNeg(r) : r;
		};
		switch (i->id)
		{
		case PPC_INS_PS_MADD: putS(fma(a0, c0, b0, false, false), fma(a1, c1, b1, false, false)); break;
		case PPC_INS_PS_MSUB: putS(fma(a0, c0, b0, true, false), fma(a1, c1, b1, true, false)); break;
		case PPC_INS_PS_NMADD: putS(fma(a0, c0, b0, false, true), fma(a1, c1, b1, false, true)); break;
		case PPC_INS_PS_NMSUB: putS(fma(a0, c0, b0, true, true), fma(a1, c1, b1, true, true)); break;
		case PPC_INS_PS_MADDS0: putS(fma(a0, c0, b0, false, false), fma(a1, c0, b1, false, false)); break;
		case PPC_INS_PS_MADDS1: putS(fma(a0, c1, b0, false, false), fma(a1, c1, b1, false, false)); break;
		case PPC_INS_PS_SUM0: put(single(irb.CreateFAdd(a0, b1)), single(c1)); break;
		case PPC_INS_PS_SUM1: put(single(c0), single(irb.CreateFAdd(a0, b1))); break;
		case PPC_INS_PS_SEL:
		{
			auto* z = llvm::ConstantFP::get(d, 0.0);
			put(irb.CreateSelect(irb.CreateFCmpOGE(a0, z), c0, b0),
					irb.CreateSelect(irb.CreateFCmpOGE(a1, z), c1, b1));
			break;
		}
		default: break;
		}
		return;
	}
	default:
		break;
	}
	translatePseudoAsmGeneric(i, pi, irb);
}

/**
 * PPC_INS_LFS, PPC_INS_LFSU, PPC_INS_LFD, PPC_INS_LFDU
 *
 * The FPRs are modelled as doubles (powerpc_init.cpp maps PPC_REG_F0..F31 to
 * f64), which is what the hardware register file holds: a single-precision
 * load converts the 32-bit datum to double on the way in. So the load reads
 * float or double from memory and the store to the register widens, which is
 * exactly eOpConv::FPCAST_OR_BITCAST.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLoadFloat(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
	case PPC_INS_LFS:
	case PPC_INS_LFSU: ty = irb.getFloatTy(); break;
	case PPC_INS_LFD:
	case PPC_INS_LFDU: ty = irb.getDoubleTy(); break;
	default: return;
	}

	op1 = loadOpBinaryOp1(pi, irb, ty);
	storeOp(pi->operands[0], op1, irb, eOpConv::FPCAST_OR_BITCAST);

	// With update.
	//
	auto& ppcOp1 = pi->operands[1];
	if (i->id == PPC_INS_LFSU || i->id == PPC_INS_LFDU)
		if (ppcOp1.type == PPC_OP_MEM && ppcOp1.mem.base != PPC_REG_INVALID)
			if (auto* l = llvm::dyn_cast<llvm::LoadInst>(op1))
				if (auto* cast = llvm::dyn_cast<llvm::CastInst>(l->getPointerOperand()))
				{
					storeRegister(ppcOp1.mem.base, cast->getOperand(0), irb);
				}
}

/**
 * PPC_INS_LFSX, PPC_INS_LFSUX, PPC_INS_LFDX, PPC_INS_LFDUX
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLoadFloatIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
	case PPC_INS_LFSX:
	case PPC_INS_LFSUX: ty = irb.getFloatTy(); break;
	case PPC_INS_LFDX:
	case PPC_INS_LFDUX:
	case PPC_INS_LXSDX: ty = irb.getDoubleTy(); break;
	default: return;
	}

	auto* add = loadIndexedEffectiveAddress(pi, irb);

	auto* l = loadIntPtr(irb, add, ty);
	storeOp(pi->operands[0], l, irb, eOpConv::FPCAST_OR_BITCAST);

	// With update.
	//
	auto& ppcOp1 = pi->operands[1];
	if (i->id == PPC_INS_LFSUX || i->id == PPC_INS_LFDUX)
		if (ppcOp1.type == PPC_OP_REG && ppcOp1.reg != PPC_REG_INVALID)
		{
			storeRegister(ppcOp1.reg, add, irb);
		}
}

/**
 * PPC_INS_STFS, PPC_INS_STFSU, PPC_INS_STFD, PPC_INS_STFDU
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateStoreFloat(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
	case PPC_INS_STFS:
	case PPC_INS_STFSU: ty = irb.getFloatTy(); break;
	case PPC_INS_STFD:
	case PPC_INS_STFDU: ty = irb.getDoubleTy(); break;
	default: return;
	}

	op0 = loadOpBinaryOp0(pi, irb);
	op0 = generateTypeConversion(irb, op0, ty, eOpConv::FPCAST_OR_BITCAST);

	auto* si = storeOp(pi->operands[1], op0, irb);

	// With update.
	//
	auto& ppcOp1 = pi->operands[1];
	if (i->id == PPC_INS_STFSU || i->id == PPC_INS_STFDU)
		if (ppcOp1.type == PPC_OP_MEM && ppcOp1.mem.base != PPC_REG_INVALID)
			if (auto* s = llvm::dyn_cast<llvm::StoreInst>(si))
				if (auto* cast = llvm::dyn_cast<llvm::CastInst>(s->getPointerOperand()))
				{
					storeRegister(ppcOp1.mem.base, cast->getOperand(0), irb);
				}
}

/**
 * PPC_INS_STFSX, PPC_INS_STFSUX, PPC_INS_STFDX, PPC_INS_STFDUX
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateStoreFloatIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
	case PPC_INS_STFSX:
	case PPC_INS_STFSUX: ty = irb.getFloatTy(); break;
	case PPC_INS_STFDX:
	case PPC_INS_STFDUX:
	case PPC_INS_STXSDX: ty = irb.getDoubleTy(); break;
	default: return;
	}

	op0 = loadOp(pi->operands[0], irb);
	op0 = generateTypeConversion(irb, op0, ty, eOpConv::FPCAST_OR_BITCAST);
	auto* add = loadIndexedEffectiveAddress(pi, irb);
	storeIntPtr(irb, op0, add, ty);

	// With update.
	//
	auto& ppcOp1 = pi->operands[1];
	if (i->id == PPC_INS_STFSUX || i->id == PPC_INS_STFDUX)
		if (ppcOp1.type == PPC_OP_REG && ppcOp1.reg != PPC_REG_INVALID)
		{
			storeRegister(ppcOp1.reg, add, irb);
		}
}

/**
 * PPC_INS_LHBRX, PPC_INS_LWBRX, PPC_INS_LDBRX,
 * PPC_INS_STHBRX, PPC_INS_STWBRX, PPC_INS_STDBRX
 *
 * The byte-reversed accesses: load or store a halfword, word or doubleword
 * with the byte order of the other endianness. A big-endian PowerPC reading a
 * little-endian file uses them, which is why they are what a decompiler meets
 * in ELF parsers, network code and anything that touches a PE header.
 *
 * `lwbrx` was 1,092 occurrences of `__asm_lwbrx` in the static parity corpus,
 * `stwbrx` was a write-only `__asm_stwbrx` call -- the store disappeared
 * entirely, because translatePseudoAsmFncOp0Op1Op2 passes the value and the
 * two address registers to an opaque function and nothing writes memory. The
 * halfword load was modelled, by hand, as two byte loads OR'd together with a
 * `TODO: Maybe model this as ASM pseudo call as PPC_INS_LWBRX` on it: the
 * suggestion ran the wrong way. One `llvm.bswap` covers all six.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLoadStoreByteReverse(
	cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	llvm::Type* ty = nullptr;
	bool store = false;
	switch (i->id)
	{
	case PPC_INS_LHBRX: ty = irb.getInt16Ty(); break;
	case PPC_INS_LWBRX: ty = irb.getInt32Ty(); break;
	case PPC_INS_LDBRX: ty = irb.getInt64Ty(); break;
	case PPC_INS_STHBRX:
		ty = irb.getInt16Ty();
		store = true;
		break;
	case PPC_INS_STWBRX:
		ty = irb.getInt32Ty();
		store = true;
		break;
	case PPC_INS_STDBRX:
		ty = irb.getInt64Ty();
		store = true;
		break;
	default: return;
	}

	auto* addr = loadIndexedEffectiveAddress(pi, irb);

	auto* bswap =
		llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::bswap, llvm::ArrayRef<llvm::Type*>{ty});

	if (store)
	{
		llvm::Value* val = loadOp(pi->operands[0], irb);
		val = irb.CreateZExtOrTrunc(val, ty);
		val = irb.CreateCall(bswap, llvm::ArrayRef<llvm::Value*>{val});
		storeIntPtr(irb, val, addr, ty);
	}
	else
	{
		llvm::Value* val = loadIntPtr(irb, addr, ty);
		val = irb.CreateCall(bswap, llvm::ArrayRef<llvm::Value*>{val});
		storeOp(pi->operands[0], val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
}

/**
 * PPC_INS_LMW, PPC_INS_STMW
 *
 * Load and store multiple word: `stmw rS, d(rA)` writes rS, rS+1, ... r31 to
 * consecutive words starting at d + rA, and `lmw` reads them back. A compiler
 * emits them to save and restore the callee-saved half of the register file in
 * one instruction, so they bracket whole functions -- `stmw` was 668
 * occurrences of a pseudo-assembly call that wrote nothing at all, and `lmw`
 * was dispatched to nullptr, which is the unhandled-instruction path.
 *
 * The count is implied, not encoded: it is 32 - rS, so `stmw r28, 8(r1)` is
 * four stores and `stmw r31, 8(r1)` is one. Getting that backwards (counting
 * up from r0, or storing a fixed number) is what the tests here are shaped to
 * catch -- each register gets a different value and the memory either side of
 * the written range is checked as well.
 *
 * The transfers are 32-bit words on 64-bit PowerPC too, and `lmw`
 * zero-extends. The architecture leaves the case where rA is inside the loaded
 * range invalid; loading through the address computed once, up front, is the
 * reading every implementation takes.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLoadStoreMultiple(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	if (pi->operands[0].type != PPC_OP_REG || !isGeneralPurposeRegister(pi->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	auto* i32 = irb.getInt32Ty();
	llvm::Value* addr = loadOp(pi->operands[1], irb, nullptr, /*lea=*/true);
	unsigned first = getGeneralPurposeRegisterIndex(pi->operands[0].reg);

	for (unsigned r = first; r < 32; ++r)
	{
		llvm::Value* ea = addr;
		if (unsigned off = (r - first) * 4)
		{
			ea = irb.CreateAdd(addr, llvm::ConstantInt::get(addr->getType(), off));
		}

		uint32_t reg = PPC_REG_R0 + r;
		if (i->id == PPC_INS_STMW)
		{
			llvm::Value* val = loadRegister(reg, irb);
			storeIntPtr(irb, irb.CreateZExtOrTrunc(val, i32), ea, i32);
		}
		else
		{
			llvm::Value* val = loadIntPtr(irb, ea, i32);
			storeRegister(reg, val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		}
	}
}

/**
 * PPC_INS_LI = addi rD, 0, value
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	storeOp(pi->operands[0], op1, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeCr0(irb, pi, op1); // ?
}

/**
 * PPC_INS_LIS = addis rD, 0, value
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLis(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	op1 = irb.CreateShl(op1, llvm::ConstantInt::get(op1->getType(), 16));
	storeOp(pi->operands[0], op1, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeCr0(irb, pi, op1); // ?
}

/**
 * PPC_INS_MR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	storeOp(pi->operands[0], op1, irb);
	storeCr0(irb, pi, op1);
}

/**
 * PPC_INS_MTCRF
 *
 * `mtcrf CRM, rS` writes the condition register fields selected by the 8-bit
 * mask CRM from the low word of rS. It is the counterpart of `mfcr`, and the
 * pair is how a compiler spills and reloads the condition register; `mtcr rS`
 * is the same instruction with CRM = 0xff, and capstone reports it that way.
 *
 * This was a pseudo-assembly call returning a struct of four `i1`s and seven
 * `i4`s -- the four CR0 bits the rest of the translator actually reads, then
 * CR1..CR7 as four-bit registers that nothing reads. The condition register
 * was modelled twice, in two widths, and mtcrf wrote both: the halves that
 * mattered came out of an opaque function, and the halves that did not were
 * the only ones with the field structure. Every write to CR1..CR7 was dead,
 * and every write to CR0 was unknown.
 *
 * The bit positions are the architecture's and are the same ones `mfcr` reads:
 * CR field n occupies bits 4n..4n+3 counted from the MOST significant end of
 * the word, so CR0's LT bit is bit 31 and CR7's SO bit is bit 0. The mask is
 * numbered the same way -- CRM bit 0 is 0x80 and selects CR0 -- which is the
 * half of this that a plain `1 << f` gets backwards.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMtcrf(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	// `mtcr rS` is the Capstone 6 alias of `mtcrf 255, rS` (one operand).
	EXPECT_IS_EXPR(i, pi, irb, (pi->op_count == 1 || pi->op_count == 2));

	auto* i1 = irb.getInt1Ty();
	auto* i32 = irb.getInt32Ty();
	uint64_t mask = 0xff;
	llvm::Value* src = nullptr;
	if (pi->op_count == 1)
	{
		src = loadOpUnary(pi, irb);
	}
	else
	{
		// mtcrf: (IMM CRM, rS). mtocrf in Capstone 6: (CRf, rS).
		if (!ppcFxmMask(pi->operands[0], mask))
		{
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		src = loadOpBinaryOp1(pi, irb);
	}
	src = irb.CreateZExtOrTrunc(src, i32);

	for (unsigned f = 0; f < 8; ++f)
	{
		// CRM is numbered from the most significant end too: bit 0x80
		// selects CR0.
		if ((mask & (0x80u >> f)) == 0)
		{
			continue;
		}

		uint32_t lt = 0, gt = 0, eq = 0, so = 0;
		crFieldRegisters(PPC_REG_CR0 + f, lt, gt, eq, so);
		const uint32_t bits[4] = {lt, gt, eq, so};

		for (unsigned b = 0; b < 4; ++b)
		{
			unsigned shift = 31 - (4 * f + b);
			llvm::Value* v = irb.CreateLShr(src, llvm::ConstantInt::get(i32, shift));
			storeRegister(bits[b], irb.CreateTrunc(v, i1), irb);
		}
	}
}

/**
 * PPC_INS_MTCTR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMtctr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, pi, irb);

	op0 = loadOpUnary(pi, irb);
	storeRegister(PPC_REG_CTR, op0, irb);
}

/**
 * PPC_INS_MTLR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMtlr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, pi, irb);

	op0 = loadOpUnary(pi, irb);
	storeRegister(PPC_REG_LR, op0, irb);
}

/**
 * PPC_INS_CRAND, PPC_INS_CRANDC, PPC_INS_CREQV, PPC_INS_CRNAND, PPC_INS_CRNOR,
 * PPC_INS_CROR, PPC_INS_CRORC, PPC_INS_CRXOR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateCrModifTernary(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(pi, irb);

	auto* i1 = irb.getInt1Ty();
	auto* i4 = irb.getIntNTy(4);
	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			llvm::StructType::create(llvm::ArrayRef<llvm::Type*>{
					i1, i1, i1, i1, i4, i4, i4, i4, i4, i4, i4}),
			llvm::ArrayRef<llvm::Type*>{
					op0->getType(),
					op1->getType(),
					op2->getType()});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0, op1, op2});

	storeRegister(PPC_REG_CR0LT, irb.CreateExtractValue(c, {0}), irb);
	storeRegister(PPC_REG_CR0GT, irb.CreateExtractValue(c, {1}), irb);
	storeRegister(PPC_REG_CR0EQ, irb.CreateExtractValue(c, {2}), irb);
	storeRegister(PPC_REG_CR0UN, irb.CreateExtractValue(c, {3}), irb);

	storeRegister(PPC_REG_CR1, irb.CreateExtractValue(c, {4}), irb);
	storeRegister(PPC_REG_CR2, irb.CreateExtractValue(c, {5}), irb);
	storeRegister(PPC_REG_CR3, irb.CreateExtractValue(c, {6}), irb);
	storeRegister(PPC_REG_CR4, irb.CreateExtractValue(c, {7}), irb);
	storeRegister(PPC_REG_CR5, irb.CreateExtractValue(c, {8}), irb);
	storeRegister(PPC_REG_CR6, irb.CreateExtractValue(c, {9}), irb);
	storeRegister(PPC_REG_CR7, irb.CreateExtractValue(c, {10}), irb);
}

/**
 * PPC_INS_CRNOT  - crnot bx, by = crnor bx, by, by
 * PPC_INS_CRMOVE - crmove bx, by = cror bx, by, by
 */
/**
 * PPC_INS_CRAND, CRANDC, CREQV, CRNAND, CRNOR, CROR, CRORC, CRXOR
 *
 * One condition-register bit in, one bit out. Capstone reports all three
 * operands as the individual bit registers -- `cror cr0lt, cr0eq, cr0lt` comes
 * back as PPC_REG_CR0LT, PPC_REG_CR0EQ, PPC_REG_CR0LT -- and this translator
 * has an `i1` global for every one of the 32 bits, which is what storeCrX()
 * and storeCr0() write on every comparison. So there is nothing to model: it
 * is a bitwise operation between two registers that already exist.
 *
 * What was there instead clobbered CR0
 * ------------------------------------
 *
 * translateCrModifTernary() emitted a pseudo-assembly call returning a
 * struct, and then did this, unconditionally, for every instruction in the
 * family:
 *
 *     storeRegister(PPC_REG_CR0LT, ExtractValue(c, {0}), irb);
 *     storeRegister(PPC_REG_CR0GT, ExtractValue(c, {1}), irb);
 *     storeRegister(PPC_REG_CR0EQ, ExtractValue(c, {2}), irb);
 *     storeRegister(PPC_REG_CR0UN, ExtractValue(c, {3}), irb);
 *     storeRegister(PPC_REG_CR1,   ExtractValue(c, {4}), irb);   // ... CR7
 *
 * `cror cr7lt, cr7eq, cr7lt` does not touch CR0. That code overwrote all four
 * of CR0's bits with the results of an undefined function -- so a comparison
 * into CR0, then a CR-bit operation on any other field, then a branch on CR0,
 * read whatever the call returned. A wrong branch, not a missing translation.
 *
 * The seven `PPC_REG_CR1`..`CR7` writes went nowhere at all. Those are `i4`
 * globals that exist alongside the 32 `i1` bit registers, and nothing in this
 * translator ever reads them: the condition register is modelled twice and one
 * of the two models is write-only.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateCrBitOp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	auto isBit = [this](cs_ppc_op& op) { return op.type == PPC_OP_REG && isCrBitRegister(op.reg); };
	if (!isBit(pi->operands[0]) || !isBit(pi->operands[1]) || !isBit(pi->operands[2]))
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	llvm::Value* a = loadRegister(pi->operands[1].reg, irb);
	llvm::Value* b = loadRegister(pi->operands[2].reg, irb);

	// The operation comes from the MNEMONIC, not the id.
	//
	// Capstone 5.0.9 decodes `cror` with PPC_INS_CRMOVE's id (227) while
	// printing "cror" -- the alias table wins on the id and loses on the
	// text. A dispatch table keyed on ids alone therefore sent every `cror`
	// to translateCrNotMove(), whose EXPECT_IS_BINARY rejected its three
	// operands and fell through to the generic pseudo-assembly path. That is
	// why PSEUDO-01 saw `__asm_cror(i1, i1, i1)` -- a void call with three
	// arguments, which is not the shape translateCrModifTernary() emitted:
	// the entry the table had for CROR was never reached at all.
	//
	// Third instance of this shape in the branch, after ARM's NOP/HINT and
	// VMRS/FMSTAT: a table key that Capstone does not produce.
	std::string mnem = i->mnemonic;
	llvm::Value* res = nullptr;
	if (mnem == "crand")
		res = irb.CreateAnd(a, b);
	else if (mnem == "cror")
		res = irb.CreateOr(a, b);
	else if (mnem == "crxor")
		res = irb.CreateXor(a, b);
	else if (mnem == "crandc")
		res = irb.CreateAnd(a, irb.CreateNot(b));
	else if (mnem == "crorc")
		res = irb.CreateOr(a, irb.CreateNot(b));
	else if (mnem == "crnand")
		res = irb.CreateNot(irb.CreateAnd(a, b));
	else if (mnem == "crnor")
		res = irb.CreateNot(irb.CreateOr(a, b));
	else if (mnem == "creqv")
		res = irb.CreateNot(irb.CreateXor(a, b));
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	storeRegister(pi->operands[0].reg, res, irb);
}

/**
 * PPC_INS_MFCR -- move the whole condition register into a GPR.
 * PPC_INS_MFOCRF -- the same, but only the CR field(s) selected by FXM; every
 * other dest bit is 0. Capstone 6 reports the FXM slot as a CR field register
 * (`mfocrf r3, 8` -> REG CR4), matching mtocrf, not as an immediate.
 *
 * 9,008 occurrences in the static parity corpus, the largest unmodelled
 * pseudo-assembly call on PowerPC. It is how a compiler spills the condition
 * register, and it was `__asm_mfcr()`.
 *
 * The 32 bits are the 32 `i1` registers, and their positions are fixed by the
 * architecture: CR field n occupies bits 4n..4n+3 counted from the MOST
 * significant end, so CR0's LT bit is bit 31 of the word and CR7's SO bit is
 * bit 0. Getting that backwards puts CR0 where CR7 belongs, which the test
 * catches by giving each field a different value.
 *
 * Built at 32 bits and stored zero-extended, which is what the instruction
 * does on 64-bit PowerPC and a no-op on 32-bit.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMfcr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, pi, irb);

	auto* i32 = irb.getInt32Ty();
	llvm::Value* res = llvm::ConstantInt::get(i32, 0);

	uint64_t mask = 0xff;
	if (pi->op_count == 2)
	{
		if (!ppcFxmMask(pi->operands[1], mask))
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
	}
	else if (ppcIsId(i, PPC_INS_MFOCRF))
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	for (unsigned f = 0; f < 8; ++f)
	{
		if ((mask & (0x80u >> f)) == 0)
		{
			continue;
		}
		uint32_t lt = 0, gt = 0, eq = 0, so = 0;
		crFieldRegisters(PPC_REG_CR0 + f, lt, gt, eq, so);
		const uint32_t bits[4] = {lt, gt, eq, so};
		for (unsigned b = 0; b < 4; ++b)
		{
			// Bit 0 is the most significant, so field f bit b is at
			// 31 - (4f + b) counting from the bottom.
			unsigned shift = 31 - (4 * f + b);
			llvm::Value* v = irb.CreateZExt(loadRegister(bits[b], irb), i32);
			res = irb.CreateOr(res, irb.CreateShl(v, llvm::ConstantInt::get(i32, shift)));
		}
	}

	storeOp(pi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateCrNotMove(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	// Capstone hands the CRMOVE and CRNOT ids to instructions that are not
	// those aliases -- `cror` arrives here with three operands and the
	// mnemonic "cror" -- so the operand count decides which of the two this
	// is before anything else does.
	if (pi->op_count == 3)
	{
		translateCrBitOp(i, pi, irb);
		return;
	}

	EXPECT_IS_BINARY(i, pi, irb);

	uint32_t crReg0 = 0;
	uint32_t crReg1 = 0;
	if (pi->operands[0].type == PPC_OP_REG
			&& isCrRegister(pi->operands[0].reg)
			&& pi->operands[1].type == PPC_OP_REG
			&& isCrRegister(pi->operands[1].reg))
	{
		auto r0 = pi->operands[0].reg;
		crReg0 = r0;
		auto r1 = pi->operands[1].reg;
		crReg1 = r1;
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	op1 = loadRegister(crReg1, irb);

	if (ppcIsId(i, PPC_INS_CRMOVE))
	{
		storeRegister(crReg0, op1, irb);
	}
	else if (ppcIsId(i, PPC_INS_CRNOT))
	{
		op1 = generateValueNegate(irb, op1);
		storeRegister(crReg0, op1, irb);
	}
}

/**
 * PPC_INS_CRSET - set CR bit
 * PPC_INS_CRCLR - clear CR bit
 * Unary, operand is general purpose register r0-r31 == bit 0-31 of CR.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateCrSetClr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, pi, irb);

	uint32_t crReg = 0;
	if (pi->operands[0].type == PPC_OP_REG
			&& isCrRegister(pi->operands[0].reg))
	{
		auto r = pi->operands[0].reg;
		crReg = r;
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	if (ppcIsId(i, PPC_INS_CRSET))
	{
		storeRegister(crReg, irb.getTrue(), irb);
	}
	else if (ppcIsId(i, PPC_INS_CRCLR))
	{
		storeRegister(crReg, irb.getFalse(), irb);
	}
}

/**
 * PPC_INS_MCRF
 *
 * `mcrf crD, crS` copies one condition register field to another: four bits,
 * unchanged, in one instruction. It is how a compiler keeps a comparison
 * result alive across code that would clobber CR0.
 *
 * It was three pseudo-assembly calls, one per case, and the shape of them says
 * what went wrong. `mcrf crD, cr0` became `__asm_mcrf_cr0_read(lt, gt, eq,
 * so) -> i4` -- it read the four `i1` bits of CR0 correctly and then stored
 * the opaque result into crD as a four-bit register that nothing reads.
 * `mcrf cr0, crS` became `__asm_mcrf_cr0_write(i4) -> {i1,i1,i1,i1}`, reading
 * crS as the same dead `i4`. And `mcrf cr3, cr5`, where neither side is CR0
 * and which is most of the 466 occurrences, became `__asm_mcrf(i4) -> i4`:
 * both halves dead.
 *
 * Each of the three was the condition register modelled in two widths at once,
 * with an opaque function bridging between them. It is four `i1` copies.
 * Reading all four before writing any keeps it a copy rather than a sequence
 * of assignments, which matters only for crD == crS -- handled above -- but
 * costs nothing to get right.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMcrf(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	auto isField = [](cs_ppc_op& op) {
		return op.type == PPC_OP_REG && PPC_REG_CR0 <= op.reg && op.reg <= PPC_REG_CR7;
	};

	if (!isField(pi->operands[0]) || !isField(pi->operands[1]))
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	if (pi->operands[0].reg == pi->operands[1].reg)
	{
		return;
	}

	uint32_t dLt = 0, dGt = 0, dEq = 0, dSo = 0;
	uint32_t sLt = 0, sGt = 0, sEq = 0, sSo = 0;
	crFieldRegisters(pi->operands[0].reg, dLt, dGt, dEq, dSo);
	crFieldRegisters(pi->operands[1].reg, sLt, sGt, sEq, sSo);

	const uint32_t dst[4] = {dLt, dGt, dEq, dSo};
	const uint32_t src[4] = {sLt, sGt, sEq, sSo};

	llvm::Value* vals[4] = {nullptr, nullptr, nullptr, nullptr};
	for (unsigned b = 0; b < 4; ++b)
	{
		vals[b] = loadRegister(src[b], irb);
	}
	for (unsigned b = 0; b < 4; ++b)
	{
		storeRegister(dst[b], vals[b], irb);
	}
}

/**
 * PPC_INS_MFCTR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMfctr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, pi, irb);

	auto* ctr = loadRegister(PPC_REG_CTR, irb);
	storeOp(pi->operands[0], ctr, irb);
}

/**
 * PPC_INS_MFLR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMflr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, pi, irb);

	auto* ctr = loadRegister(PPC_REG_LR, irb);
	storeOp(pi->operands[0], ctr, irb);
}

/**
 * PPC_INS_MFXER
 *
 * XER itself is not modelled; its carry bit is, as PPC_REG_CARRY, which is
 * what every ADDC/ADDE/SUBF chain reads and writes. CA is XER bit 34 in
 * 64-bit numbering, value 0x20000000. SO and OV are not tracked and read as
 * zero. As the generic MFSPR the value came from an opaque call, so a
 * save/restore of XER around a carry chain cut the chain.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMfxer(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, pi, irb);

	auto* ca = irb.CreateZExt(loadRegister(PPC_REG_CARRY, irb), getDefaultType());
	auto* xer = irb.CreateShl(ca, llvm::ConstantInt::get(getDefaultType(), 29));
	auto* ovb = irb.CreateZExt(loadRegister(kXerOvReg, irb), getDefaultType());
	auto* sob = irb.CreateZExt(loadRegister(kXerSoReg, irb), getDefaultType());
	xer = irb.CreateOr(xer, irb.CreateShl(ovb, llvm::ConstantInt::get(getDefaultType(), 30)));
	xer = irb.CreateOr(xer, irb.CreateShl(sob, llvm::ConstantInt::get(getDefaultType(), 31)));
	storeOp(pi->operands[0], xer, irb);
}

/**
 * PPC_INS_MTXER -- the inverse of MFXER: only CA is kept.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMtxer(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, pi, irb);

	auto* v = loadOp(pi->operands[0], irb);
	auto* bit = irb.CreateLShr(v, llvm::ConstantInt::get(v->getType(), 29));
	storeRegister(PPC_REG_CARRY, irb.CreateTrunc(bit, irb.getInt1Ty()), irb);
	auto bitAt = [&](unsigned n) {
		return irb.CreateTrunc(irb.CreateLShr(v, llvm::ConstantInt::get(v->getType(), n)), irb.getInt1Ty());
	};
	storeRegister(kXerOvReg, bitAt(30), irb);
	storeRegister(kXerSoReg, bitAt(31), irb);
}

/**
 * PPC_INS_MULHW, PPC_INS_MULHWU, PPC_INS_MULHD, PPC_INS_MULHDU
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMulhw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	const bool dbl = (i->id == PPC_INS_MULHD || i->id == PPC_INS_MULHDU);
	const bool isSigned = (i->id == PPC_INS_MULHW || i->id == PPC_INS_MULHD);
	if (dbl)
	{
		op1 = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt64Ty());
		auto* wide = irb.getIntNTy(128);
		op1 = isSigned ? irb.CreateSExt(op1, wide) : irb.CreateZExt(op1, wide);
		op2 = isSigned ? irb.CreateSExt(op2, wide) : irb.CreateZExt(op2, wide);
		auto* val = irb.CreateTrunc(irb.CreateLShr(irb.CreateMul(op1, op2), 64), irb.getInt64Ty());
		storeOp(pi->operands[0], val, irb);
		storeCr0(irb, pi, val);
		return;
	}

	// Narrow to the word FIRST. SExtOrTrunc to i64 is a no-op when the
	// operand is already i64, so on PPC64 this was a 64x64 multiply with
	// bits 63:32 taken.
	op1 = narrowToWord(op1, irb);
	op2 = narrowToWord(op2, irb);
	if (isSigned)
	{
		op1 = irb.CreateSExt(op1, irb.getInt64Ty());
		op2 = irb.CreateSExt(op2, irb.getInt64Ty());
	}
	else
	{
		op1 = irb.CreateZExt(op1, irb.getInt64Ty());
		op2 = irb.CreateZExt(op2, irb.getInt64Ty());
	}
	auto* val = irb.CreateMul(op1, op2);
	val = irb.CreateLShr(val, 32);
	val = irb.CreateTrunc(val, irb.getInt32Ty());
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_MULLW, PPC_INS_MULLI, PPC_INS_MULLD
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateMullw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	// MULLI is NOT a word operation and must not be narrowed. Both ids are
	// dispatched here and the body named neither, so mullw's semantics were
	// applied to both:
	//
	//   mullw RT,RA,RB   RT <- (RA)[32:63] x (RB)[32:63]   -- low words
	//   mulli RT,RA,SI   prod[0:127] <- (RA) x EXTS(SI); RT <- prod[64:127]
	//
	// There is no "mulliw". With RA = 0x0000000100000002 and SI = 3, PowerPC
	// gives 0x0000000300000006 and narrowing gave 6. Inert on PPC32, where
	// the narrowing is a no-op; capstone maps both MULLI and MULLI8 to this
	// id, so the 64-bit form arrives here with i64 operands.
	if (i->id == PPC_INS_MULLI || i->id == PPC_INS_MULLD)
	{
		if (i->id == PPC_INS_MULLD)
		{
			op1 = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
			op2 = irb.CreateZExtOrTrunc(op2, irb.getInt64Ty());
		}
		auto* prod = irb.CreateMul(op1, op2);
		storeOp(pi->operands[0], prod, irb);
		storeCr0(irb, pi, prod);
		return;
	}

	// The 64-bit product of the low WORDS. In 32-bit mode storeOp truncates
	// it back, which is that mode's answer.
	auto* i64 = irb.getInt64Ty();
	auto* val = irb.CreateMul(irb.CreateSExt(narrowToWord(op1, irb), i64), irb.CreateSExt(narrowToWord(op2, irb), i64));
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_NAND
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateNand(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* val = irb.CreateAnd(op1, op2);
	val = generateValueNegate(irb, val);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_NEG
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateNeg(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	auto* val = irb.CreateSub(llvm::ConstantInt::get(op1->getType(), 0), op1);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_NOP
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateNop(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	// nothing
}

/**
 * PPC_INS_SYNC, PPC_INS_LWSYNC, PPC_INS_ISYNC, PPC_INS_EIEIO,
 * PPC_INS_MBAR, PPC_INS_MSYNC, PPC_INS_TLBSYNC, PPC_INS_PTESYNC
 * Barriers → LLVM fence. No I-cache / TLB model.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateFence(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	irb.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
}

/**
 * PPC_INS_NOR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateNor(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* val = irb.CreateOr(op1, op2);
	val = generateValueNegate(irb, val);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_NOT
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateNot(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	op2 = op1;
	auto* val = irb.CreateOr(op1, op2);
	val = generateValueNegate(irb, val);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_OR, PPC_INS_ORI
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateOr(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* val = irb.CreateOr(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_ORC
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateOrc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = generateValueNegate(irb, op2);
	auto* val = irb.CreateOr(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_ORIS
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateOris(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = irb.CreateShl(op2, llvm::ConstantInt::get(op2->getType(), 16));
	auto* val = irb.CreateOr(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_RLWINM, PPC_INS_RLWIMI, PPC_INS_RLWNM
 */
/**
 * The 32-bit mask PowerPC's rotate-and-mask instructions take, built from a
 * begin and an end bit.
 *
 * PowerPC numbers bits from the MOST significant end: bit 0 is 0x80000000.
 * Getting that backwards produces a mask that is the bit-reverse of the right
 * one, which for the common `rlwinm rA, rS, n, 0, 31-n` (a plain shift) is a
 * plausible-looking wrong answer rather than an obvious one.
 *
 * MB > ME is not an error. It means the mask wraps: bits MB..31 and 0..ME,
 * which is how a rotate-and-mask extracts a field that straddles the word
 * boundary after rotation.
 */
static uint32_t ppcRotateMask(unsigned mb, unsigned me)
{
	uint32_t m = 0;
	if (mb <= me)
	{
		for (unsigned b = mb; b <= me; ++b)
		{
			m |= 1u << (31 - b);
		}
	}
	else
	{
		for (unsigned b = mb; b <= 31; ++b)
		{
			m |= 1u << (31 - b);
		}
		for (unsigned b = 0; b <= me; ++b)
		{
			m |= 1u << (31 - b);
		}
	}
	return m;
}

/**
 * PPC_INS_RLWINM, PPC_INS_RLWIMI, PPC_INS_RLWNM -- rotate left word, then mask.
 *
 *     rlwinm rA, rS, SH, MB, ME    rA = ROTL32(rS, SH) & MASK(MB, ME)
 *     rlwnm  rA, rS, rB, MB, ME    the same, rotating by rB[27:31]
 *     rlwimi rA, rS, SH, MB, ME    rA = (ROTL32(rS,SH) & MASK) | (rA & ~MASK)
 *
 * These were dispatched to translateRotateComplex5op(), which emits an
 * __asm_rlwinm() call and stores its return value. That is 42 of the 8,736
 * instructions in the PowerPC half of the parity corpus -- 0.48%, and the most
 * frequent unmodelled pseudo-assembly call on any of the five architectures.
 * COV-01 scored them covered, because the dispatch table has a function
 * pointer for them; PSEUDO-01 is the measurement that says a function pointer
 * and a translation are not the same thing.
 *
 * There is nothing here that needs a pseudo-asm call. Every operand but rS is
 * an immediate (or, for RLWNM, a register read), the rotate is 32-bit, and
 * the mask is a compile-time constant. GCC emits rlwinm for every shift and
 * every bitfield extract on 32-bit PowerPC, which is why it is this frequent:
 * `rlwinm r3, r3, 0, 16, 31` is `r3 = (uint16_t) r3`, and it was coming out of
 * the decompiler as a call to an undefined function.
 *
 * The word is the low 32 bits of the source and the result is written
 * zero-extended, which matters on 64-bit PowerPC and is a no-op on 32-bit.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateRotateWordMask(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NARY(i, pi, irb, 5);

	if (pi->operands[3].type != PPC_OP_IMM || pi->operands[4].type != PPC_OP_IMM
		|| (i->id != PPC_INS_RLWNM && pi->operands[2].type != PPC_OP_IMM))
	{
		throwUnexpectedOperands(i);
		translateRotateComplex5op(i, pi, irb);
		return;
	}

	auto* i32 = irb.getInt32Ty();
	llvm::Value* w = irb.CreateZExtOrTrunc(loadOp(pi->operands[1], irb), i32);

	unsigned mb = static_cast<unsigned>(pi->operands[3].imm) & 31;
	unsigned me = static_cast<unsigned>(pi->operands[4].imm) & 31;
	auto* mask = llvm::ConstantInt::get(i32, ppcRotateMask(mb, me));

	llvm::Value* rot = nullptr;
	if (i->id == PPC_INS_RLWNM)
	{
		// The rotate amount is dynamic, so the "rotate by zero" case has to be
		// selected rather than decided: `lshr i32 %w, 32` is poison.
		llvm::Value* sh =
			irb.CreateAnd(irb.CreateZExtOrTrunc(loadOp(pi->operands[2], irb), i32), llvm::ConstantInt::get(i32, 31));
		llvm::Value* spun =
			irb.CreateOr(irb.CreateShl(w, sh), irb.CreateLShr(w, irb.CreateSub(llvm::ConstantInt::get(i32, 32), sh)));
		rot = irb.CreateSelect(irb.CreateICmpEQ(sh, llvm::ConstantInt::get(i32, 0)), w, spun);
	}
	else
	{
		unsigned sh = static_cast<unsigned>(pi->operands[2].imm) & 31;
		rot = sh == 0 ? w
					  : irb.CreateOr(
							irb.CreateShl(w, llvm::ConstantInt::get(i32, sh)),
							irb.CreateLShr(w, llvm::ConstantInt::get(i32, 32 - sh)));
	}

	llvm::Value* res = irb.CreateAnd(rot, mask);
	if (i->id == PPC_INS_RLWIMI)
	{
		// The insert form keeps the destination's bits outside the mask, which
		// is the only reason it reads rA at all.
		llvm::Value* old = irb.CreateZExtOrTrunc(loadOp(pi->operands[0], irb), i32);
		res = irb.CreateOr(res, irb.CreateAnd(old, irb.CreateNot(mask)));
	}

	storeOp(pi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeCr0Word(irb, pi, res);
}

/**
 * 64-bit PowerPC mask: bit 0 is the MSB (0x8000...0). Same wrap rule as
 * ppcRotateMask(): MB > ME means bits MB..63 and 0..ME.
 */
static uint64_t ppcRotateMask64(unsigned mb, unsigned me)
{
	mb &= 63;
	me &= 63;
	uint64_t m = 0;
	if (mb <= me)
	{
		for (unsigned b = mb; b <= me; ++b)
		{
			m |= uint64_t(1) << (63 - b);
		}
	}
	else
	{
		for (unsigned b = mb; b <= 63; ++b)
		{
			m |= uint64_t(1) << (63 - b);
		}
		for (unsigned b = 0; b <= me; ++b)
		{
			m |= uint64_t(1) << (63 - b);
		}
	}
	return m;
}

static llvm::Value* ppcRotateLeft64(
		llvm::Value* w,
		llvm::Value* sh,
		llvm::IRBuilder<>& irb)
{
	auto* i64 = irb.getInt64Ty();
	w = irb.CreateZExtOrTrunc(w, i64);
	sh = irb.CreateAnd(irb.CreateZExtOrTrunc(sh, i64), llvm::ConstantInt::get(i64, 63));
	llvm::Value* spun = irb.CreateOr(
			irb.CreateShl(w, sh),
			irb.CreateLShr(w, irb.CreateSub(llvm::ConstantInt::get(i64, 64), sh)));
	return irb.CreateSelect(irb.CreateICmpEQ(sh, llvm::ConstantInt::get(i64, 0)), w, spun);
}

static llvm::Value* ppcRotateLeft64Imm(llvm::Value* w, unsigned sh, llvm::IRBuilder<>& irb)
{
	auto* i64 = irb.getInt64Ty();
	w = irb.CreateZExtOrTrunc(w, i64);
	sh &= 63;
	if (sh == 0)
	{
		return w;
	}
	return irb.CreateOr(
			irb.CreateShl(w, llvm::ConstantInt::get(i64, sh)),
			irb.CreateLShr(w, llvm::ConstantInt::get(i64, 64 - sh)));
}

/**
 * PPC_INS_RLDICL, RLDICR, RLDIC, RLDIMI, RLDCL, RLDCR, and the compiler
 * aliases CLRLDI / ROTLDI / ROTLD / SLDI.
 *
 *     rldicl rA, rS, SH, MB    rA = ROTL64(rS, SH) & MASK(MB, 63)
 *     rldicr rA, rS, SH, ME    rA = ROTL64(rS, SH) & MASK(0, ME)
 *     rldic  rA, rS, SH, MB    rA = ROTL64(rS, SH) & MASK(MB, 63-SH)
 *     rldimi rA, rS, SH, MB    insert into MASK(MB, 63-SH)
 *
 * GCC/clang emit these for every 64-bit shift, zero-extend, and bitfield
 * extract (`clrldi` = uint32_t cast, `srdi`/`sldi` = shifts, `rotldi` =
 * rotate). They were nullptr → __asm_rldicl() on ppc64.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateRotateDoubleMask(
		cs_insn* i,
		cs_ppc* pi,
		llvm::IRBuilder<>& irb)
{
	std::set<int> opCountTemp = {3, 4};
	EXPECT_IS_SET(i, pi, irb, opCountTemp)

	auto* i64 = irb.getInt64Ty();
	llvm::Value* src = loadOp(pi->operands[1], irb);
	llvm::Value* rot = nullptr;
	unsigned shImm = 0;
	bool shIsImm = false;
	unsigned mb = 0;
	unsigned me = 63;

	auto takeImm = [&](const cs_ppc_op& op) -> unsigned {
		return static_cast<unsigned>(op.imm) & 63;
	};

	if (pi->op_count == 4)
	{
		if (pi->operands[3].type != PPC_OP_IMM)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		if (pi->operands[2].type == PPC_OP_IMM)
		{
			shIsImm = true;
			shImm = takeImm(pi->operands[2]);
			rot = ppcRotateLeft64Imm(src, shImm, irb);
		}
		else
		{
			rot = ppcRotateLeft64(src, loadOp(pi->operands[2], irb), irb);
		}
		unsigned field = takeImm(pi->operands[3]);
		switch (i->id)
		{
			case PPC_INS_RLDICR:
			case PPC_INS_RLDCR:
				mb = 0;
				me = field;
				break;
			case PPC_INS_RLDIC:
			case PPC_INS_RLDIMI:
				mb = field;
				me = shIsImm ? ((63 - shImm) & 63) : 63;
				break;
			default:
				mb = field;
				me = 63;
				break;
			}
	}
	else
	{
		// Three-operand aliases. Capstone 6 reports these via alias_id
		// (ALIAS_CLRLDI etc.); PPC_INS_CLRLDI is that alias token when
		// the 5.x enumerator is gone.
		if (ppcIsClrldi(i))
		{
			shIsImm = true;
			shImm = 0;
			rot = ppcRotateLeft64Imm(src, 0, irb);
			mb = (pi->operands[2].type == PPC_OP_IMM) ? takeImm(pi->operands[2]) : 0;
			me = 63;
		}
		else if (ppcIsRotldi(i) || ppcIsSldi(i))
		{
			if (pi->operands[2].type != PPC_OP_IMM)
			{
				throwUnexpectedOperands(i);
				translatePseudoAsmGeneric(i, pi, irb);
				return;
			}
			shIsImm = true;
			shImm = takeImm(pi->operands[2]);
			rot = ppcRotateLeft64Imm(src, shImm, irb);
			if (ppcIsSldi(i))
			{
				// `sldi` / `rldicr RA,RS,SH,63-SH` keeps bits 0..63-SH.
				mb = 0;
				me = (63 - shImm) & 63;
			}
			else
			{
				mb = 0;
				me = 63;
			}
		}
		else if (ppcIsId(i, PPC_INS_ROTLD))
		{
			rot = ppcRotateLeft64(src, loadOp(pi->operands[2], irb), irb);
			mb = 0;
			me = 63;
		}
		else
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
	}

	if (i->id == PPC_INS_RLDIC && !shIsImm)
	{
		// ME = 63-SH is dynamic. MASK(MB, 63-SH) is "clear the SH bits that
		// rotated into the right, and also the bits left of MB".
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	auto* mask = llvm::ConstantInt::get(i64, ppcRotateMask64(mb, me));
	llvm::Value* res = irb.CreateAnd(rot, mask);
	if (i->id == PPC_INS_RLDIMI)
	{
		llvm::Value* old = irb.CreateZExtOrTrunc(loadOp(pi->operands[0], irb), i64);
		res = irb.CreateOr(res, irb.CreateAnd(old, irb.CreateNot(mask)));
	}

	storeOp(pi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeCr0(irb, pi, res);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateRotateComplex5op(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NARY(i, pi, irb, 5);

	auto* op1 = loadOp(pi->operands[1], irb);
	auto* op2 = loadOp(pi->operands[2], irb);
	auto* op3 = loadOp(pi->operands[3], irb);
	auto* op4 = loadOp(pi->operands[4], irb);

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			op1->getType(),
			llvm::ArrayRef<llvm::Type*>{
					op1->getType(),
					op2->getType(),
					op3->getType(),
					op4->getType()});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op1, op2, op3, op4});

	storeOp(pi->operands[0], c, irb);
	storeCr0(irb, pi, c);
}

/**
 * PPC_INS_ROTLW, PPC_INS_ROTLWI
 */
/**
 * PPC_INS_ROTLWI, PPC_INS_ROTLW -- rotate left WORD.
 *
 * `rotlwi rA, rS, n` is `rlwinm rA, rS, n, 0, 31`: a rotate of the low 32 bits,
 * written back zero-extended. This rotated the whole register, which is right
 * on 32-bit PowerPC and wrong on 64-bit, where `rotlwi r0, r1, 8` with
 * r1 = 0x12345678 answered 0x1234567800 instead of 0x34567812. Capstone only
 * reports the extended mnemonic in 64-bit mode -- in 32-bit mode the same
 * assembly comes back as a five-operand `rlwinm` -- so the one mode that
 * reached this code was the one it was wrong for.
 *
 * The bits shifted out of bit 31 have to come back at bit 0. At 64 bits they
 * moved into bit 32 instead and stayed there, which is not a rotate of
 * anything.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateRotlw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* i32 = irb.getInt32Ty();
	llvm::Value* w = irb.CreateZExtOrTrunc(op1, i32);
	llvm::Value* n = irb.CreateAnd(irb.CreateZExtOrTrunc(op2, i32), llvm::ConstantInt::get(i32, 31));

	// `lshr i32 %w, 32` is poison, and n is not always a constant here -- the
	// register form exists -- so the zero case is selected rather than decided.
	llvm::Value* spun =
		irb.CreateOr(irb.CreateShl(w, n), irb.CreateLShr(w, irb.CreateSub(llvm::ConstantInt::get(i32, 32), n)));
	llvm::Value* res = irb.CreateSelect(irb.CreateICmpEQ(n, llvm::ConstantInt::get(i32, 0)), w, spun);

	storeOp(pi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeCr0Word(irb, pi, res);
}

/**
 * PPC_INS_SLW, PPC_INS_SLD
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateShiftLeft(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	const bool dbl = (i->id == PPC_INS_SLD);
	auto* ty = dbl ? irb.getInt64Ty() : irb.getInt32Ty();
	unsigned width = dbl ? 64u : 32u;
	unsigned countMask = dbl ? 0x7fu : 0x3fu;
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	op2 = irb.CreateAnd(irb.CreateZExtOrTrunc(op2, ty), llvm::ConstantInt::get(ty, countMask));

	// slw reads six bits / sld seven, but the extra bit does not participate
	// in the shift: a count of `width` or more gives ZERO, not a wrapped
	// shift. Shifting an iN by N or more is poison.
	auto* tooBig = irb.CreateICmpUGE(op2, llvm::ConstantInt::get(ty, width));
	auto* safe = irb.CreateAnd(op2, llvm::ConstantInt::get(ty, width - 1));
	auto* val = irb.CreateSelect(tooBig, llvm::ConstantInt::get(ty, 0), irb.CreateShl(op1, safe));
	storeOp(pi->operands[0], val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	if (dbl)
	{
		storeCr0(irb, pi, val);
	}
	else
	{
		storeCr0Word(irb, pi, val);
	}
}

/**
 * PPC_INS_SRW, PPC_INS_SRD
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateShiftRight(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	const bool dbl = (i->id == PPC_INS_SRD);
	auto* ty = dbl ? irb.getInt64Ty() : irb.getInt32Ty();
	unsigned width = dbl ? 64u : 32u;
	unsigned countMask = dbl ? 0x7fu : 0x3fu;
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	op2 = irb.CreateAnd(irb.CreateZExtOrTrunc(op2, ty), llvm::ConstantInt::get(ty, countMask));

	auto* tooBig = irb.CreateICmpUGE(op2, llvm::ConstantInt::get(ty, width));
	auto* safe = irb.CreateAnd(op2, llvm::ConstantInt::get(ty, width - 1));
	auto* val = irb.CreateSelect(tooBig, llvm::ConstantInt::get(ty, 0), irb.CreateLShr(op1, safe));
	storeOp(pi->operands[0], val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	if (dbl)
	{
		storeCr0(irb, pi, val);
	}
	else
	{
		storeCr0Word(irb, pi, val);
	}
}

/**
 * PPC_INS_SLWI
 */
/**
 * PPC_INS_SLWI -- shift left WORD immediate, `rlwinm rA, rS, n, 0, 31-n`.
 *
 * Same correction as ROTLWI and CLRLWI: the bits that leave bit 31 are dropped,
 * not carried into bit 32, and the result is zero-extended.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSlwi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	auto* i32 = irb.getInt32Ty();
	llvm::Value* n = irb.CreateAnd(irb.CreateZExtOrTrunc(op2, i32), llvm::ConstantInt::get(i32, 31));
	auto* shl = irb.CreateShl(irb.CreateZExtOrTrunc(op1, i32), n);
	storeOp(pi->operands[0], shl, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeCr0Word(irb, pi, shl);
}

/**
 * PPC_INS_SRWI
 */
/**
 * PPC_INS_SRWI -- shift right WORD immediate, `rlwinm rA, rS, 32-n, n, 31`.
 *
 * The upper 32 bits of a 64-bit rS do not participate; shifting the whole
 * register brought them down into the result.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSrwi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	auto* i32 = irb.getInt32Ty();
	llvm::Value* n = irb.CreateAnd(irb.CreateZExtOrTrunc(op2, i32), llvm::ConstantInt::get(i32, 31));
	auto* shr = irb.CreateLShr(irb.CreateZExtOrTrunc(op1, i32), n);
	storeOp(pi->operands[0], shr, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeCr0Word(irb, pi, shr);
}

/**
 * PPC_INS_SRAW, PPC_INS_SRAWI, PPC_INS_SRAD, PPC_INS_SRADI
 *
 * Arithmetic right shift of the word or doubleword. CA is set when RS was
 * negative and any 1-bit was shifted out. The register forms read one extra
 * count bit (6 for sraw, 7 for srad); when that bit is set the result is
 * all sign bits.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSraw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	const bool dbl = (i->id == PPC_INS_SRAD || i->id == PPC_INS_SRADI);
	const bool imm = (i->id == PPC_INS_SRAWI || i->id == PPC_INS_SRADI);
	unsigned bits = dbl ? 64u : 32u;
	auto* ty = irb.getIntNTy(bits);
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	op2 = irb.CreateZExtOrTrunc(op2, ty);

	unsigned nmask = dbl ? (imm ? 63u : 127u) : (imm ? 31u : 63u);
	auto* n = irb.CreateAnd(op2, llvm::ConstantInt::get(ty, nmask));
	auto* tooBig = irb.CreateICmpUGE(n, llvm::ConstantInt::get(ty, bits));
	auto* safeN = irb.CreateAnd(n, llvm::ConstantInt::get(ty, bits - 1));
	auto* shifted = irb.CreateAShr(op1, safeN);
	auto* signFill = irb.CreateAShr(op1, llvm::ConstantInt::get(ty, bits - 1));
	auto* result = irb.CreateSelect(tooBig, signFill, shifted);

	// 64-bit `srawi` sign-extends the word into the GPR. QPX GPRs are i64 in
	// this translator but the tests compare the 32-bit result, so zero-extend
	// there (and in 32-bit mode, where the register is already i32).
	eOpConv wordConv =
		(!dbl && _origBasicMode != CS_MODE_64) ? eOpConv::ZEXT_TRUNC_OR_BITCAST : eOpConv::SEXT_TRUNC_OR_BITCAST;
	storeOp(pi->operands[0], result, irb, wordConv);
	storeCr0(irb, pi, result);

	auto* wasNeg = irb.CreateICmpSLT(op1, llvm::ConstantInt::get(ty, 0));
	auto* one = llvm::ConstantInt::get(ty, 1);
	auto* lowMask = irb.CreateSub(irb.CreateShl(one, safeN), one);
	auto* lost = irb.CreateSelect(tooBig, op1, irb.CreateAnd(op1, lowMask));
	storeRegister(PPC_REG_CARRY, irb.CreateAnd(wasNeg, irb.CreateICmpNE(lost, llvm::ConstantInt::get(ty, 0))), irb);
}

/**
 * PPC_INS_SUBF,
 * PPC_INS_SUB  - sub rD, rA, rB = subf rD, rB, rA
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSubf(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	// Capstone 6.x reports `subf RT, RA, RB` sources as [RB, RA] (the
	// `sub` mnemonic order), so the 5.x `if (id == SUB) swap` never
	// fires: PPC_INS_SUB is ALIAS_SUB and i->id stays SUBF.
	std::swap(op1, op2);
	auto* val = irb.CreateSub(op2, op1);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_SUBFC, PPC_INS_SUBFIC
 * PPC_INS_SUBC - subfc rD, rA, rB = subfc rD, rB, rA
 * TODO: This is different than the original semantics, it is according to
 * PowerPC specification.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSubfc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	// Capstone 6.x lists subfc sources as [RB, RA]. subfic stays [RA, SIMM]:
	// RT = ~RA + SIMM + 1.
	if (i->id != PPC_INS_SUBFIC)
	{
		std::swap(op1, op2);
	}

	// PowerPC specification.
	//	op1 = generateValueNegate(irb, op1);
	//	auto* val = irb.CreateAdd(op1, op2);
	//	val = irb.CreateAdd(val, llvm::ConstantInt::get(val->getType(), 1));

	// The same but simpler?
	auto* val = irb.CreateSub(op2, op1);
	op1 = generateValueNegate(irb, op1);

	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);

	// TODO: This is according to old semantics, but I'm not sure if it is ok.
	storeRegister(PPC_REG_CARRY, generateCarryAddC(op1, op2, irb, irb.getTrue()), irb);
	// TODO: PPC_INS_SUBFIC was using overflow_add_c() instead in an old
	// sematics, but these operations looks the same, so I have no idea why.
	// It may be the same thing.
//	storeRegister(PPC_REG_CARRY, genOverflowAddC(val, op1, op2, irb, irb.getTrue()), irb);
}

/**
 * PPC_INS_SUBFE
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSubfe(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* op1Neg = generateValueNegate(irb, op1);
	auto* val = irb.CreateAdd(op1Neg, op2);
	auto* carry = loadRegister(PPC_REG_CARRY, irb);
	carry = irb.CreateZExtOrTrunc(carry, val->getType());
	val = irb.CreateAdd(val, carry);

	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
	// `subfe RT,RA,RB` is ~RA + RB + CA, and CA is the carry out of THAT sum.
	// The value above uses op1Neg; this used the un-complemented op1, so it
	// answered carryout(RA + RB + CA). The three sibling translators --
	// subfc, subfme, subfze -- all pass the complemented operand.
	storeRegister(PPC_REG_CARRY, generateCarryAddC(op1Neg, op2, irb, carry), irb);
}

/**
 * PPC_INS_SUBFME
 * TODO: This is modeled as it was in an old semantics, It looks a bit different
 * than in specification, but it may be doing the same thing, or may not,
 * I'm not really sure.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSubfme(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	auto* op1Neg = generateValueNegate(irb, op1);
	auto* carry = loadRegister(PPC_REG_CARRY, irb);
	carry = irb.CreateZExtOrTrunc(carry, op1->getType());
	auto* negativeTwo = llvm::ConstantInt::getSigned(op1->getType(), -2);
	auto* negativeOne = llvm::ConstantInt::getSigned(op1->getType(), -1);

	auto* sub = irb.CreateSub(negativeTwo, op1);
	auto* val = irb.CreateAdd(sub, carry);

	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
	storeRegister(PPC_REG_CARRY, generateCarryAddC(op1Neg, negativeOne, irb, carry), irb);
}

/**
 * PPC_INS_SUBFZE
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSubfze(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	op1 = loadOpBinaryOp1(pi, irb);
	auto* op1Neg = generateValueNegate(irb, op1);
	auto* carry = loadRegister(PPC_REG_CARRY, irb);
	carry = irb.CreateZExtOrTrunc(carry, op1->getType());
	auto* val = irb.CreateAdd(op1Neg, carry);

	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
	auto* zero = llvm::ConstantInt::get(op1Neg->getType(), 0);
	storeRegister(PPC_REG_CARRY, generateCarryAddC(op1Neg, zero, irb, carry), irb);
}

/**
 * PPC_INS_XOR, PPC_INS_XORI
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateXor(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* val = irb.CreateXor(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

/**
 * PPC_INS_XORIS
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateXoris(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = irb.CreateShl(op2, llvm::ConstantInt::get(op2->getType(), 16));
	auto* val = irb.CreateXor(op1, op2);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

//
//==============================================================================
//

/**
 * link   = Store next insn address to LR.
 * toLR   = Branch to LR.
 * toCTR  = branch to CTR.
 * primal = Complex variants all other simplified mnemonics are derived from.
 *          Right now, we probably will not be able handle it -- to many ops.
 *          E.g. bc BO, BI, target_addr
 *          No idea if Capstone actually generates this with three ops.
 *
 * Basic unconditional branches (not really, it can be conditional):
 * PPC_INS_B       - uncond/cond (blt, beq, bne, ...)
 * PPC_INS_BA      - absolute, cond/uncond
 * PPC_INS_BL      - link, cond/uncond
 * PPC_INS_BLA     - link, absolute, cond/uncond
 *
 * Basic conditional branches (not really sure it they are ever used):
 * PPC_INS_BC      - cond, primal
 * PPC_INS_BCA     - cond, absolute, primal
 * PPC_INS_BCLR    - cond, toLR, primal
 * PPC_INS_BCCTR   - cond, toCTR, primal
 * PPC_INS_BCL     - cond, link, primal
 * PPC_INS_BCLA    - cond, absolute, link, primal
 * PPC_INS_BCLRL   - cond, toLR, link, primal
 * PPC_INS_BCCTRL  - cond, toCTR, link, primal
 *
 * Branch unconditionally (not really, it can be conditional):
 * PPC_INS_BLR      - uncond/cond (beqlr, ...), toLR
 * PPC_INS_BCTR     - uncond/cond, toCTR
 * PPC_INS_BLRL     - uncond/cond, toLR, link
 * PPC_INS_BCTRL    - uncond/cond, toCTR, link
 *
 * Branch if condition true:
 * Not sure if these are ever used, looks like they are equal to:
 * (equal to b, ba, blr, bctr, bl, bla, blrl, bctrl).
 * PPC_INS_BT       - cond                  =>  (b)
 * PPC_INS_BTA      - cond, absolute        =>  (ba)
 * PPC_INS_BTLR     - cond, toLR            =>  (blr)
 * PPC_INS_BTCTR    - cond, toCTR           =>  (bctr)
 * PPC_INS_BTL      - cond, link            =>  (bl)
 * PPC_INS_BTLA     - cond, link, absolute  =>  (bla)
 * PPC_INS_BTLRL    - cond, link, toLR      =>  (blrl)
 * PPC_INS_BTCTRL   - cond, link, toCTR     =>  (bctrl)
 *
 * Branch if condition false:
 * Not sure if these are ever used, looks like they are translated to:
 * (b, ba, blr, bctr, bl, bla, blrl, bctrl) reversed conditions.
 * PPC_INS_BF       - cond                  =>  (b)
 * PPC_INS_BFA      - cond, absolute        =>  (ba)
 * PPC_INS_BFLR     - cond, toLR            =>  (blr)
 * PPC_INS_BFCTR    - cond, toCTR           =>  (bctr)
 * PPC_INS_BFL      - cond, link            =>  (bl)
 * PPC_INS_BFLA     - cond, link, absolute  =>  (bla)
 * PPC_INS_BFLRL    - cond, link, toLR      =>  (blrl)
 * PPC_INS_BFCTRL   - cond, link, toCTR     =>  (bctrl)
 *
 * Decrement CTR, branch if CTR != 0:
 * PPC_INS_BDNZ     - cond
 * PPC_INS_BDNZA    - cond, absolute
 * PPC_INS_BDNZLR   - cond, toLR
 * PPC_INS_BDNZL    - cond, link
 * PPC_INS_BDNZLA   - cond, absolute, link
 * PPC_INS_BDNZLRL  - cond, link, toLR
 *
 * Decrement CTR, branch if CTR != 0 & cond true:
 * PPC_INS_BDNZT    - cond
 * PPC_INS_BDNZTA   - cond, absolute
 * PPC_INS_BDNZTLR  - cond, toLR
 * PPC_INS_BDNZTL   - cond, link
 * PPC_INS_BDNZTLA  - cond, absolute, link
 * PPC_INS_BDNZTLRL - cond, link, toLR
 *
 * Decrement CTR, branch if CTR != 0 & cond false:
 * PPC_INS_BDNZF    - cond
 * PPC_INS_BDNZFA   - cond, absolute
 * PPC_INS_BDNZFLR  - cond, toLR, (missing)
 * PPC_INS_BDNZFL   - cond, link
 * PPC_INS_BDNZFLA  - cond, ansolute, link
 * PPC_INS_BDNZFLRL - cond, link, toLR
 *
 * Decrement CTR, branch if CTR == 0:
 * PPC_INS_BDZ      - cond
 * PPC_INS_BDZA     - cond, absolute
 * PPC_INS_BDZLR    - cond, toLR
 * PPC_INS_BDZL     - cond, link
 * PPC_INS_BDZLA    - cond, absolute, link
 * PPC_INS_BDZLRL   - cond, link, toLR
 *
 * Decrement CTR, branch if CTR == 0 & cond true:
 * PPC_INS_BDZT     - cond
 * PPC_INS_BDZTA    - cond, absolute
 * PPC_INS_BDZTLR   - cond, toLR
 * PPC_INS_BDZTL    - cond, link
 * PPC_INS_BDZTLA   - cond, absolute, link
 * PPC_INS_BDZTLRL  - cond, link, toLR
 *
 * Decrement CTR, branch if CTR == 0 & cond false:
 * PPC_INS_BDZF     - cond
 * PPC_INS_BDZFA    - cond, absolute
 * PPC_INS_BDZFLR   - cond, toLR
 * PPC_INS_BDZFL    - cond, link
 * PPC_INS_BDZFLA   - cond, absolute, link
 * PPC_INS_BDZFLRL  - cond, link, toLR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateB(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	// Capstone 6 reports BC/BCL as i->id and bdnz/bdnzl as alias_id.
	const unsigned bid = i->is_alias ? static_cast<unsigned>(i->alias_id) : i->id;

	// Link.
	//
	static std::set<unsigned int> linkIds =
	{
			PPC_INS_BL, PPC_INS_BLA,
			PPC_INS_BCL, PPC_INS_BCLA, PPC_INS_BCLRL, PPC_INS_BCCTRL,
			PPC_INS_BLRL, PPC_INS_BCTRL,
			PPC_INS_BTL, PPC_INS_BTLA, PPC_INS_BTLRL, PPC_INS_BTCTRL,
			PPC_INS_BFL, PPC_INS_BFLA, PPC_INS_BFLRL, PPC_INS_BFCTRL,

			PPC_INS_BDNZL, PPC_INS_BDNZLA, PPC_INS_BDNZLRL,
			PPC_INS_BDNZTL, PPC_INS_BDNZTLA, PPC_INS_BDNZTLRL,
			PPC_INS_BDNZFL, PPC_INS_BDNZFLA, PPC_INS_BDNZFLRL,

			PPC_INS_BDZL, PPC_INS_BDZLA, PPC_INS_BDZLRL,
			PPC_INS_BDZTL, PPC_INS_BDZTLA, PPC_INS_BDZTLRL,
			PPC_INS_BDZFL, PPC_INS_BDZFLA, PPC_INS_BDZFLRL,

			PPC_INS_BLTL, PPC_INS_BLTLA, PPC_INS_BLTLRL, PPC_INS_BLTCTRL,
			PPC_INS_BLEL, PPC_INS_BLELA, PPC_INS_BLELRL, PPC_INS_BLECTRL,
			PPC_INS_BEQL, PPC_INS_BEQLA, PPC_INS_BEQLRL, PPC_INS_BEQCTRL,
			PPC_INS_BGEL, PPC_INS_BGELA, PPC_INS_BGELRL, PPC_INS_BGECTRL,
			PPC_INS_BGTL, PPC_INS_BGTLA, PPC_INS_BGTLRL, PPC_INS_BGTCTRL,
			PPC_INS_BNLL, PPC_INS_BNLLA, PPC_INS_BNLLRL, PPC_INS_BNLCTRL,
			PPC_INS_BNEL, PPC_INS_BNELA, PPC_INS_BNELRL, PPC_INS_BNECTRL,
			PPC_INS_BNGL, PPC_INS_BNGLA, PPC_INS_BNGLRL, PPC_INS_BNGCTRL,
			PPC_INS_BSOL, PPC_INS_BSOLA, PPC_INS_BSOLRL, PPC_INS_BSOCTRL,
			PPC_INS_BNSL, PPC_INS_BNSLA, PPC_INS_BNSLRL, PPC_INS_BNSCTRL,
	};
	bool link = linkIds.count(bid) || linkIds.count(i->id);

	// toLR.
	//
	static std::set<unsigned int> toLRIds =
	{
			PPC_INS_BCLR, PPC_INS_BCLRL,
			PPC_INS_BLR, PPC_INS_BLRL,
			PPC_INS_BTLR, PPC_INS_BTLRL,
			PPC_INS_BFLR, PPC_INS_BFLRL,

			PPC_INS_BDNZLR, PPC_INS_BDNZLRL,
			PPC_INS_BDNZTLR, PPC_INS_BDNZTLRL,
			PPC_INS_BDNZFLRL,

			PPC_INS_BDZLR, PPC_INS_BDZLRL,
			PPC_INS_BDZTLR, PPC_INS_BDZTLRL,
			PPC_INS_BDZFLR, PPC_INS_BDZFLRL,

			PPC_INS_BLTLR, PPC_INS_BLTLRL,
			PPC_INS_BLELR, PPC_INS_BLELRL,
			PPC_INS_BEQLR, PPC_INS_BEQLRL,
			PPC_INS_BGELR, PPC_INS_BGELRL,
			PPC_INS_BGTLR, PPC_INS_BGTLRL,
			PPC_INS_BNLLR, PPC_INS_BNLLRL,
			PPC_INS_BNELR, PPC_INS_BNELRL,
			PPC_INS_BNGLR, PPC_INS_BNGLRL,
			PPC_INS_BSOLR, PPC_INS_BSOLRL,
			PPC_INS_BNSLR, PPC_INS_BNSLRL,
	};
	bool toLR = toLRIds.count(bid) || toLRIds.count(i->id);

	// toCTR.
	//
	static std::set<unsigned int> toCTRIds =
	{
			PPC_INS_BCCTR, PPC_INS_BCCTRL,
			PPC_INS_BCTR, PPC_INS_BCTRL,
			PPC_INS_BTCTR, PPC_INS_BTCTRL,
			PPC_INS_BFCTR, PPC_INS_BFCTRL,

			PPC_INS_BLTCTR, PPC_INS_BLTCTRL,
			PPC_INS_BLECTR, PPC_INS_BLECTRL,
			PPC_INS_BEQCTR, PPC_INS_BEQCTRL,
			PPC_INS_BGECTR, PPC_INS_BGECTRL,
			PPC_INS_BGTCTR, PPC_INS_BGTCTRL,
			PPC_INS_BNLCTR, PPC_INS_BNLCTRL,
			PPC_INS_BNECTR, PPC_INS_BNECTRL,
			PPC_INS_BNGCTR, PPC_INS_BNGCTRL,
			PPC_INS_BSOCTR, PPC_INS_BSOCTRL,
			PPC_INS_BNSCTR, PPC_INS_BNSCTRL,
	};
	bool toCTR = toCTRIds.count(bid) || toCTRIds.count(i->id);

	// Reverse condition.
	//
	static std::set<unsigned int> reverseCondIds =
	{
			PPC_INS_BF, PPC_INS_BFA, PPC_INS_BFLR, PPC_INS_BFCTR,
			PPC_INS_BFL, PPC_INS_BFLA, PPC_INS_BFLRL, PPC_INS_BFCTRL,

			PPC_INS_BDNZF, PPC_INS_BDNZFA,
			PPC_INS_BDNZFL, PPC_INS_BDNZFLA, PPC_INS_BDNZFLRL,

			PPC_INS_BDZF, PPC_INS_BDZFA, PPC_INS_BDZFLR,
			PPC_INS_BDZFL, PPC_INS_BDZFLA, PPC_INS_BDZFLRL,
	};
	bool reverseCond = reverseCondIds.count(bid) || reverseCondIds.count(i->id);

	// Decrement CTR, branch if CTR != 0.
	//
	static std::set<unsigned int> ctrNonzeroCondIds =
	{
			PPC_INS_BDNZ, PPC_INS_BDNZLR,
			PPC_INS_BDNZL, PPC_INS_BDNZLA, PPC_INS_BDNZLRL,
			PPC_INS_ALIAS_BDNZ, PPC_INS_ALIAS_BDNZLR,
			PPC_INS_ALIAS_BDNZL, PPC_INS_ALIAS_BDNZLA, PPC_INS_ALIAS_BDNZLRL,

			PPC_INS_BDNZT, PPC_INS_BDNZTA, PPC_INS_BDNZTLR,
			PPC_INS_BDNZTL, PPC_INS_BDNZTLA, PPC_INS_BDNZTLRL,

			PPC_INS_BDNZF, PPC_INS_BDNZFA,
			PPC_INS_BDNZFL, PPC_INS_BDNZFLA, PPC_INS_BDNZFLRL,
	};
	bool ctrNonzero = ctrNonzeroCondIds.count(bid) || ctrNonzeroCondIds.count(i->id);

	// Decrement CTR, branch if CTR == 0.
	//
	static std::set<unsigned int> ctrZeroCondIds =
	{
			PPC_INS_BDZ, PPC_INS_BDZLR,
			PPC_INS_BDZL, PPC_INS_BDZLA, PPC_INS_BDZLRL,
			PPC_INS_ALIAS_BDZ, PPC_INS_ALIAS_BDZLR,
			PPC_INS_ALIAS_BDZL, PPC_INS_ALIAS_BDZLA, PPC_INS_ALIAS_BDZLRL,

			PPC_INS_BDZT, PPC_INS_BDZTA, PPC_INS_BDZTLR,
			PPC_INS_BDZTL, PPC_INS_BDZTLA, PPC_INS_BDZTLRL,

			PPC_INS_BDZF, PPC_INS_BDZFA, PPC_INS_BDZFLR,
			PPC_INS_BDZFL, PPC_INS_BDZFLA, PPC_INS_BDZFLRL,
	};
	bool ctrZero = ctrZeroCondIds.count(bid) || ctrZeroCondIds.count(i->id);

	// Decrement CTR, and condition.
	//
	static std::set<unsigned int> ctrAndCondIds =
	{
			PPC_INS_BDNZT, PPC_INS_BDNZTA, PPC_INS_BDNZTLR,
			PPC_INS_BDNZTL, PPC_INS_BDNZTLA, PPC_INS_BDNZTLRL,

			PPC_INS_BDNZF, PPC_INS_BDNZFA,
			PPC_INS_BDNZFL, PPC_INS_BDNZFLA, PPC_INS_BDNZFLRL,

			PPC_INS_BDZT, PPC_INS_BDZTA, PPC_INS_BDZTLR,
			PPC_INS_BDZTL, PPC_INS_BDZTLA, PPC_INS_BDZTLRL,

			PPC_INS_BDZF, PPC_INS_BDZFA, PPC_INS_BDZFLR,
			PPC_INS_BDZFL, PPC_INS_BDZFLA, PPC_INS_BDZFLRL,
	};
	bool ctrAndCond = ctrAndCondIds.count(bid) || ctrAndCondIds.count(i->id);

	// Capstone 6.x bdnz/bdz are aliases of BC: i->id is BC and alias_id is
	// ALIAS_BDNZ, a different enumerator from leftover PPC_INS_BDNZ.
	// Honour the BO field as well.
	if (cs_ppc_bc_decr_ctr(pi->bc.bo))
	{
		if (cs_ppc_bc_tests_ctr_is_zero(pi->bc.bo))
		{
			ctrZero = true;
		}
		else
		{
			ctrNonzero = true;
		}
		if (!cs_ppc_bc_cr_is_tested(pi->bc.bo))
		{
			ctrAndCond = false;
		}
	}

	// Get target and CR register.
	//
	llvm::Value* target = nullptr;
	uint32_t crReg = (pi->bc.crX != PPC_REG_INVALID) ? ppcBranchCr(pi->bc.crX, ctrAndCond) : PPC_REG_CR0;
	// Hint variants (LE_MINUS, …) are the same condition. gBC already folds
	// BO's compare-to-0 into GE/LE/NE/NU, so `bf` must not invert again.
	ppc_pred crBc = PPC_get_no_hint_pred(pi->bc.pred_cr);

	// Capstone 6.x `ble`/`bne`/`bge` aliases of BC often leave pred_cr as
	// a single CR bit (GT) plus reverse, which is not LT|EQ. Honour the
	// assembler mnemonic. Keystone's `ble`/`bge`/`bne`/`bns` decode as gBC
	// and print as `bf <crbit>`, whose pred_cr is already LE/GE/NE/NU.
	if (ppcIsId(i, PPC_INS_BLE) || ppcIsId(i, PPC_INS_BLEA) || ppcIsId(i, PPC_INS_BLEL) || ppcIsId(i, PPC_INS_BLELA)
		|| ppcIsId(i, PPC_INS_BLELR) || ppcIsId(i, PPC_INS_BLELRL) || ppcIsId(i, PPC_INS_BLECTR)
		|| ppcIsId(i, PPC_INS_BLECTRL))
	{
		crBc = PPC_BC_LE;
		reverseCond = false;
	}
	else if (
		ppcIsId(i, PPC_INS_BNE) || ppcIsId(i, PPC_INS_BNEA) || ppcIsId(i, PPC_INS_BNEL) || ppcIsId(i, PPC_INS_BNELA)
		|| ppcIsId(i, PPC_INS_BNELR) || ppcIsId(i, PPC_INS_BNELRL) || ppcIsId(i, PPC_INS_BNECTR)
		|| ppcIsId(i, PPC_INS_BNECTRL))
	{
		crBc = PPC_BC_NE;
		reverseCond = false;
	}
	else if (
		ppcIsId(i, PPC_INS_BGE) || ppcIsId(i, PPC_INS_BGEA) || ppcIsId(i, PPC_INS_BGEL) || ppcIsId(i, PPC_INS_BGELA)
		|| ppcIsId(i, PPC_INS_BGELR) || ppcIsId(i, PPC_INS_BGELRL) || ppcIsId(i, PPC_INS_BGECTR)
		|| ppcIsId(i, PPC_INS_BGECTRL))
	{
		crBc = PPC_BC_GE;
		reverseCond = false;
	}
	else if (
		ppcIsId(i, PPC_INS_BNS) || ppcIsId(i, PPC_INS_BNSA) || ppcIsId(i, PPC_INS_BNSL) || ppcIsId(i, PPC_INS_BNSLA)
		|| ppcIsId(i, PPC_INS_BNSLR) || ppcIsId(i, PPC_INS_BNSLRL) || ppcIsId(i, PPC_INS_BNSCTR)
		|| ppcIsId(i, PPC_INS_BNSCTRL) || ppcIsId(i, PPC_INS_BNU) || ppcIsId(i, PPC_INS_BNUA)
		|| ppcIsId(i, PPC_INS_BNUL) || ppcIsId(i, PPC_INS_BNULA) || ppcIsId(i, PPC_INS_BNULR)
		|| ppcIsId(i, PPC_INS_BNULRL) || ppcIsId(i, PPC_INS_BNUCTR) || ppcIsId(i, PPC_INS_BNUCTRL))
	{
		crBc = PPC_BC_NU;
		reverseCond = false;
	}
	else if (
		ppcIsId(i, PPC_INS_BF) || ppcIsId(i, PPC_INS_BFA) || ppcIsId(i, PPC_INS_BFL) || ppcIsId(i, PPC_INS_BFLA)
		|| ppcIsId(i, PPC_INS_BFLR) || ppcIsId(i, PPC_INS_BFLRL) || ppcIsId(i, PPC_INS_BFCTR)
		|| ppcIsId(i, PPC_INS_BFCTRL))
	{
		// pred_cr is already "branch if the CR bit is 0" (GE/LE/NE/NU).
		reverseCond = false;
	}

	// CTR+CR forms (bdnzf/bdzf) test the named bit, then the F mnemonic
	// inverts it. pred_cr for the F form is already a field predicate, so
	// using it loads GT|EQ and the boolean comes out the wrong way.
	if (ctrAndCond)
	{
		crBc = PPC_BC_LT;
		reverseCond = reverseCondIds.count(bid) || reverseCondIds.count(i->id);
	}

	if (toLR)
	{
		target = loadRegister(PPC_REG_LR, irb);

		if (pi->op_count == 0)
		{
			if (pi->bc.crX == PPC_REG_INVALID)
			{
				crReg = PPC_REG_CR0;
			}
		}
		else
		{
			// Capstone 6.x blr/bctrl aliases of BCLR/BCCTR keep BO/BI.
			for (unsigned k = 0; k < pi->op_count; ++k)
			{
				if (isCrRegister(pi->operands[k]))
				{
					crReg = ppcBranchCr(pi->operands[k].reg, ctrAndCond);
				}
			}
		}
	}
	else if (toCTR)
	{
		target = loadRegister(PPC_REG_CTR, irb);

		if (pi->op_count == 0)
		{
			if (pi->bc.crX == PPC_REG_INVALID)
			{
				crReg = PPC_REG_CR0;
			}
		}
		else if (pi->op_count == 1 && isCrRegister(pi->operands[0]))
		{
			crReg = ppcBranchCr(pi->operands[0].reg, ctrAndCond);
		}
		else if (pi->op_count == 1 && pi->operands[0].type == PPC_OP_IMM)
		{
			crReg = PPC_REG_CR0;
		}
		else
		{
			for (unsigned k = 0; k < pi->op_count; ++k)
			{
				if (isCrRegister(pi->operands[k]))
				{
					crReg = ppcBranchCr(pi->operands[k].reg, ctrAndCond);
				}
			}
		}
	}
	else
	{
		if (pi->op_count == 1 && pi->operands[0].type == PPC_OP_IMM)
		{
			if (pi->bc.crX == PPC_REG_INVALID)
			{
				crReg = PPC_REG_CR0;
			}
			target = loadOpUnary(pi, irb);
		}
		else if (pi->op_count == 2 && isCrRegister(pi->operands[0]) && pi->operands[1].type == PPC_OP_IMM)
		{
			// `bf 4*cr4+lt` names the LT bit. The condition uses the field
			// (CR4's GT|EQ), not that bit register twice.
			crReg = ppcBranchCr(pi->operands[0].reg, ctrAndCond);
			target = loadOpBinaryOp1(pi, irb);
		}
		// capstone-dumper -a ppc -m 32 -e big -c "40 b1 00 00"
		// Has only one operand = cr4.
		// IDA: 10003F80: 40 B1 00 00        ble+ cr4, loc_10003F80
		// Branches to itself.
		// TODO: Maybe report to Capstone as a bug, there should be a parameter,
		// even when relative branch to zero == itself.
		//
		else if (pi->op_count == 1 && isCrRegister(pi->operands[0]))
		{
			crReg = ppcBranchCr(pi->operands[0].reg, ctrAndCond);
			target = getThisInsnAddress(i);
		}
		// The same without parameters.
		// IDA: 000383B8: 43 53 00 00        bc 26, 4*cr4+so, loc_383B8
		//
		else if (pi->op_count == 0)
		{
			crReg = PPC_REG_CR0;
			target = getThisInsnAddress(i);
		}
		else
		{
			// Capstone 6.x bdnz/bdz aliases of BC keep extra BO/BI
			// operands in front of the target immediate.
			for (int k = static_cast<int>(pi->op_count) - 1; k >= 0; --k)
			{
				if (pi->operands[k].type == PPC_OP_IMM)
				{
					target = loadOp(pi->operands[k], irb);
					break;
				}
				if (isCrRegister(pi->operands[k]))
				{
					crReg = ppcBranchCr(pi->operands[k].reg, ctrAndCond);
				}
			}
			if (target == nullptr)
			{
				throw GenericError("unhandled branch instruction format #3");
			}
		}
	}

	if (target == nullptr)
	{
		throw GenericError("unhandled branch instruction format #4");
	}

	// Store to LR, right before branch generation.
	//
	if (link)
	{
		storeRegister(PPC_REG_LR, getNextInsnAddress(i), irb);
	}

	// Unconditional branch.
	//
	if (crBc == PPC_BC_INVALID
			&& !ctrNonzero
			&& !ctrZero)
	{
		if (toLR)
		{
			if (link)
			{
				generateBranchFunctionCall(irb, target);
			}
			else
			{
				generateReturnFunctionCall(irb, target);
			}
		}
		else if (toCTR)
		{
			if (link)
			{
				// TODO: It is probably not guaranteed that this is function call,
				// but if we use generateBranchFunctionCall(), decoder will not follow
				// to the next address after branch.
				// This should be done for all LR updating branches?
				// The best would be if decoder could try next block even if branch
				// was simple unconditional.
				generateCallFunctionCall(irb, target);
			}
			else
			{
				generateBranchFunctionCall(irb, target);
			}
		}
		else if (link)
		{
			generateCallFunctionCall(irb, target);
		}
		else
		{
			generateBranchFunctionCall(irb, target);
		}
		return;
	}

	// Conditional branch.
	//
	llvm::Value* condCtr = nullptr;
	llvm::Value* condCr = nullptr;
	if (ctrNonzero || ctrZero)
	{
		auto* ctr = loadRegister(PPC_REG_CTR, irb);
		ctr = irb.CreateSub(ctr, llvm::ConstantInt::get(ctr->getType(), 1));
		storeRegister(PPC_REG_CTR, ctr, irb);

		if (ctrNonzero)
		{
			condCtr = irb.CreateICmpNE(ctr, llvm::ConstantInt::get(ctr->getType(), 0));
		}
		else if (ctrZero)
		{
			condCtr = irb.CreateICmpEQ(ctr, llvm::ConstantInt::get(ctr->getType(), 0));
		}
	}

	if (ctrAndCond || !(ctrNonzero || ctrZero))
	{
		switch (crBc)
		{
			case PPC_BC_LT:
				condCr = loadCrX(irb, crReg, PPC_CR_LT);
				break;
			case PPC_BC_LE:
				condCr = irb.CreateOr(
						loadCrX(irb, crReg, PPC_CR_LT),
						loadCrX(irb, crReg, PPC_CR_EQ));
				break;
			case PPC_BC_EQ:
				condCr = loadCrX(irb, crReg, PPC_CR_EQ);
				break;
			case PPC_BC_GE:
				condCr = irb.CreateOr(
						loadCrX(irb, crReg, PPC_CR_GT),
						loadCrX(irb, crReg, PPC_CR_EQ));
				break;
			case PPC_BC_GT:
				condCr = loadCrX(irb, crReg, PPC_CR_GT);
				break;
			case PPC_BC_NE:
				condCr = irb.CreateNot(loadCrX(irb, crReg, PPC_CR_EQ));
				break;
			case PPC_BC_UN:
				// Capstone 6: PPC_PRED_UN == PPC_PRED_SO (CR bit 3).
				condCr = loadCrX(irb, crReg, PPC_CR_SO);
				break;
			case PPC_BC_NU:
				// Capstone 6: PPC_PRED_NU == PPC_PRED_NS.
				condCr = irb.CreateNot(loadCrX(irb, crReg, PPC_CR_SO));
				break;
			case PPC_BC_INVALID: // Already handled, should not get here.
			default:
				throw GenericError("Unhandled branch condidition.");
		}

		if (reverseCond)
		{
			condCr = irb.CreateICmpEQ(condCr, irb.getFalse());
		}
	}

	llvm::Value* cond = nullptr;
	if (condCtr && condCr == nullptr)
	{
		cond = condCtr;
	}
	else if (condCr && condCtr == nullptr)
	{
		cond = condCr;
	}
	else if (condCtr && condCr)
	{
		cond = irb.CreateAnd(condCtr, condCr);
	}
	else
	{
		throw GenericError("no cond created");
	}

	if (toLR)
	{
		generateCondReturnFunctionCall(irb, cond, target);
	}
	else if (toCTR)
	{
		// Some combos (e.g. with link) could be cond function calls.
		generateCondBranchFunctionCall(irb, cond, target);
	}
	else if (link)
	{
		// Conditional function call?
		generateCondBranchFunctionCall(irb, cond, target);
	}
	else
	{
		generateCondBranchFunctionCall(irb, cond, target);
	}
}

/**
 * vand / vandc / vor / vorc / vxor / vnor / veqv / vnand and the VSX
 * xxland / xxlandc / xxlor / xxlorc / xxlxor / xxlnor / xxleqv / xxlnand
 * family. gcc -O1 uses vand/vor/vxor for vector bitwise C and xxlor as the
 * VSX register move.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecLogical(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	llvm::Value* val = nullptr;
	switch (i->id)
	{
	case PPC_INS_VAND:
	case PPC_INS_XXLAND: val = irb.CreateAnd(a, b); break;
	case PPC_INS_VANDC:
	case PPC_INS_XXLANDC: val = irb.CreateAnd(a, irb.CreateNot(b)); break;
	case PPC_INS_VOR:
	case PPC_INS_XXLOR: val = irb.CreateOr(a, b); break;
	case PPC_INS_VORC:
	case PPC_INS_XXLORC: val = irb.CreateOr(a, irb.CreateNot(b)); break;
	case PPC_INS_VXOR:
	case PPC_INS_XXLXOR: val = irb.CreateXor(a, b); break;
	case PPC_INS_VNOR:
	case PPC_INS_XXLNOR: val = irb.CreateNot(irb.CreateOr(a, b)); break;
	case PPC_INS_VEQV:
	case PPC_INS_XXLEQV: val = irb.CreateNot(irb.CreateXor(a, b)); break;
	case PPC_INS_VNAND:
	case PPC_INS_XXLNAND: val = irb.CreateNot(irb.CreateAnd(a, b)); break;
	default: return;
	}
	storeOpI128(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVsxMove(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);
	(void)i;
	storeOpI128(pi->operands[0], loadOpI128(pi->operands[1], irb), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecLoadIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	auto* add = loadIndexedEffectiveAddress(pi, irb);
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	if (i->id == PPC_INS_LXVDSX)
	{
		auto* dw = loadIntPtr(irb, add, i64);
		auto* ext = irb.CreateZExt(dw, i128);
		storeOpI128(pi->operands[0], irb.CreateOr(irb.CreateShl(ext, 64), ext), irb);
		return;
	}
	// Two 64-bit transfers: Power is big-endian, so the lowest address is
	// the high doubleword of the 128-bit VR. LXVW4X is the same 16-byte
	// transfer as LXVD2X / LVX.
	auto* hi = loadIntPtr(irb, add, i64);
	auto* lo = loadIntPtr(irb, irb.CreateAdd(add, llvm::ConstantInt::get(add->getType(), 8)), i64);
	auto* val = irb.CreateOr(
			irb.CreateShl(irb.CreateZExt(hi, i128), 64),
			irb.CreateZExt(lo, i128));
	storeOpI128(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecStoreIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);
	(void)i;

	auto* val = loadOpI128(pi->operands[0], irb);
	auto* add = loadIndexedEffectiveAddress(pi, irb);
	auto* i64 = irb.getInt64Ty();
	auto* hi = irb.CreateTrunc(irb.CreateLShr(val, 64), i64);
	auto* lo = irb.CreateTrunc(val, i64);
	storeIntPtr(irb, hi, add, i64);
	storeIntPtr(irb, lo, irb.CreateAdd(add, llvm::ConstantInt::get(add->getType(), 8)), i64);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecSplatImm(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	int64_t simm = 0;
	if (pi->operands[1].type == PPC_OP_IMM)
	{
		simm = pi->operands[1].imm;
	}
	// 5-bit signed immediate. Capstone 6 may already have sign-extended it.
	if (simm > 15 || simm < -16)
	{
		simm = static_cast<int64_t>(static_cast<int8_t>(simm << 3) >> 3);
	}

	unsigned laneBits = 32;
	unsigned lanes = 4;
	switch (i->id)
	{
	case PPC_INS_VSPLTISB:
		laneBits = 8;
		lanes = 16;
		break;
	case PPC_INS_VSPLTISH:
		laneBits = 16;
		lanes = 8;
		break;
	case PPC_INS_VSPLTISW:
	default:
		laneBits = 32;
		lanes = 4;
		break;
	}

	auto* laneTy = irb.getIntNTy(laneBits);
	llvm::APInt lane(laneBits, static_cast<uint64_t>(simm), true);
	auto* splat = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(lanes),
			llvm::ConstantInt::get(laneTy, lane));
	storeOpI128(pi->operands[0], irb.CreateBitCast(splat, irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecFpArith(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	llvm::Type* elem = irb.getFloatTy();
	unsigned lanes = 4;
	switch (i->id)
	{
	case PPC_INS_XVADDDP:
	case PPC_INS_XVSUBDP:
	case PPC_INS_XVMULDP:
	case PPC_INS_XVDIVDP:
	case PPC_INS_XVMAXDP:
	case PPC_INS_XVMINDP:
		elem = irb.getDoubleTy();
		lanes = 2;
		break;
	default:
		break;
	}

	auto* a = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* b = asVec128(loadOpI128(pi->operands[2], irb), elem, lanes, irb);
	llvm::Value* r = nullptr;
	switch (i->id)
	{
	case PPC_INS_VADDFP:
	case PPC_INS_XVADDSP:
	case PPC_INS_XVADDDP: r = irb.CreateFAdd(a, b); break;
	case PPC_INS_VSUBFP:
	case PPC_INS_XVSUBSP:
	case PPC_INS_XVSUBDP: r = irb.CreateFSub(a, b); break;
	case PPC_INS_XVMULSP:
	case PPC_INS_XVMULDP: r = irb.CreateFMul(a, b); break;
	case PPC_INS_XVDIVSP:
	case PPC_INS_XVDIVDP: r = irb.CreateFDiv(a, b); break;
	// x86 MAXPD/MINPD: ogt/olt select, second operand on unordered.
	case PPC_INS_VMAXFP:
	case PPC_INS_XVMAXSP:
	case PPC_INS_XVMAXDP: r = irb.CreateSelect(irb.CreateFCmpOGT(a, b), a, b); break;
	case PPC_INS_VMINFP:
	case PPC_INS_XVMINSP:
	case PPC_INS_XVMINDP: r = irb.CreateSelect(irb.CreateFCmpOLT(a, b), a, b); break;
	default: return;
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(r, irb.getInt128Ty()), irb);
}

/**
 * VSX packed FMA. Assembly is XT, XA, XB with XT also a source.
 *
 * Power ISA type-A (xvmaddadp): XT = XA * XB + XT
 * type-M (xvmaddmdp):           XT = XT * XB + XA
 * msub subtracts the addend; nmsub is -(product - addend);
 * nmadd is -(product + addend), same sign pattern as scalar fnmadd.
 *
 * Vector fmul+fadd/fsub, same shape as ARM64 scalar FMLA (llvmir-emul
 * does not honour the addend of llvm.fma on some packed views). VSL0 is
 * i128 whose high doubleword is f0 (see loadRegister / storeRegister).
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecFpFma(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	switch (i->id)
	{
	case PPC_INS_XSMADDADP:
	case PPC_INS_XSMADDMDP:
	case PPC_INS_XSMSUBADP:
	case PPC_INS_XSMSUBMDP:
	case PPC_INS_XSNMADDADP:
	case PPC_INS_XSNMADDMDP:
	case PPC_INS_XSNMSUBADP:
	case PPC_INS_XSNMSUBMDP:
	case PPC_INS_XSMADDASP:
	case PPC_INS_XSMADDMSP:
	case PPC_INS_XSMSUBASP:
	case PPC_INS_XSMSUBMSP:
	case PPC_INS_XSNMADDASP:
	case PPC_INS_XSNMADDMSP:
	case PPC_INS_XSNMSUBASP:
	case PPC_INS_XSNMSUBMSP:
	{
		EXPECT_IS_TERNARY(i, pi, irb);
		auto* xt = loadOpDouble(pi->operands[0], irb);
		auto* xa = loadOpDouble(pi->operands[1], irb);
		auto* xb = loadOpDouble(pi->operands[2], irb);
		const auto id = i->id;
		const bool typeA = (id == PPC_INS_XSMADDADP || id == PPC_INS_XSMSUBADP
				|| id == PPC_INS_XSNMADDADP || id == PPC_INS_XSNMSUBADP
				|| id == PPC_INS_XSMADDASP || id == PPC_INS_XSMSUBASP
				|| id == PPC_INS_XSNMADDASP || id == PPC_INS_XSNMSUBASP);
		const bool sub = (id == PPC_INS_XSMSUBADP || id == PPC_INS_XSMSUBMDP
				|| id == PPC_INS_XSNMSUBADP || id == PPC_INS_XSNMSUBMDP
				|| id == PPC_INS_XSMSUBASP || id == PPC_INS_XSMSUBMSP
				|| id == PPC_INS_XSNMSUBASP || id == PPC_INS_XSNMSUBMSP);
		const bool neg = (id == PPC_INS_XSNMADDADP || id == PPC_INS_XSNMADDMDP
				|| id == PPC_INS_XSNMSUBADP || id == PPC_INS_XSNMSUBMDP
				|| id == PPC_INS_XSNMADDASP || id == PPC_INS_XSNMADDMSP
				|| id == PPC_INS_XSNMSUBASP || id == PPC_INS_XSNMSUBMSP);
		auto* mulA = typeA ? xa : xt;
		auto* addend = typeA ? xt : xa;
		if (sub)
		{
			addend = irb.CreateFNeg(addend);
		}
		llvm::Value* val = fpIntrinsic(irb, llvm::Intrinsic::fma, {mulA, xb, addend});
		if (neg)
		{
			val = irb.CreateFNeg(val);
		}
		if (isSinglePrecisionForm(id))
		{
			val = roundToSingle(irb, val);
		}
		storeOp(pi->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}
	default:
		break;
	}

	if (i->id == PPC_INS_VMADDFP || i->id == PPC_INS_VNMSUBFP)
	{
		if (pi->op_count < 4)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		auto* a = asVec128(loadOpI128(pi->operands[1], irb), irb.getFloatTy(), 4, irb);
		auto* c = asVec128(loadOpI128(pi->operands[2], irb), irb.getFloatTy(), 4, irb);
		auto* b = asVec128(loadOpI128(pi->operands[3], irb), irb.getFloatTy(), 4, irb);
		auto* prod = irb.CreateFMul(a, c);
		llvm::Value* r = (i->id == PPC_INS_VNMSUBFP) ? irb.CreateFSub(b, prod) : irb.CreateFAdd(prod, b);
		storeOpI128(pi->operands[0], irb.CreateBitCast(r, irb.getInt128Ty()), irb);
		return;
	}

	EXPECT_IS_TERNARY(i, pi, irb);

	llvm::Type* elem = irb.getFloatTy();
	unsigned lanes = 4;
	bool typeA = true;
	bool sub = false;
	bool nmsub = false;
	bool nmadd = false;
	switch (i->id)
	{
	case PPC_INS_XVMADDADP:
		elem = irb.getDoubleTy();
		lanes = 2;
		break;
	case PPC_INS_XVMADDASP:
		break;
	case PPC_INS_XVMADDMDP:
		elem = irb.getDoubleTy();
		lanes = 2;
		typeA = false;
		break;
	case PPC_INS_XVMADDMSP:
		typeA = false;
		break;
	case PPC_INS_XVMSUBADP:
		elem = irb.getDoubleTy();
		lanes = 2;
		sub = true;
		break;
	case PPC_INS_XVMSUBASP:
		sub = true;
		break;
	case PPC_INS_XVMSUBMDP:
		elem = irb.getDoubleTy();
		lanes = 2;
		typeA = false;
		sub = true;
		break;
	case PPC_INS_XVMSUBMSP:
		typeA = false;
		sub = true;
		break;
	case PPC_INS_XVNMADDADP:
		elem = irb.getDoubleTy();
		lanes = 2;
		nmadd = true;
		break;
	case PPC_INS_XVNMADDASP:
		nmadd = true;
		break;
	case PPC_INS_XVNMADDMDP:
		elem = irb.getDoubleTy();
		lanes = 2;
		typeA = false;
		nmadd = true;
		break;
	case PPC_INS_XVNMADDMSP:
		typeA = false;
		nmadd = true;
		break;
	case PPC_INS_XVNMSUBADP:
		elem = irb.getDoubleTy();
		lanes = 2;
		sub = true;
		nmsub = true;
		break;
	case PPC_INS_XVNMSUBASP:
		sub = true;
		nmsub = true;
		break;
	case PPC_INS_XVNMSUBMDP:
		elem = irb.getDoubleTy();
		lanes = 2;
		typeA = false;
		sub = true;
		nmsub = true;
		break;
	case PPC_INS_XVNMSUBMSP:
		typeA = false;
		sub = true;
		nmsub = true;
		break;
	default:
		return;
	}

	auto* xt = asVec128(loadOpI128(pi->operands[0], irb), elem, lanes, irb);
	auto* xa = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* xb = asVec128(loadOpI128(pi->operands[2], irb), elem, lanes, irb);

	llvm::Value* mulA = typeA ? xa : xt;
	llvm::Value* mulB = xb;
	llvm::Value* addend = typeA ? xt : xa;
	llvm::Value* prod = irb.CreateFMul(mulA, mulB);
	llvm::Value* r = nullptr;
	if (nmadd)
	{
		// -(prod + addend) as fsub from +0; llvmir-emul faults on vector fneg.
		r = irb.CreateFSub(llvm::Constant::getNullValue(prod->getType()), irb.CreateFAdd(prod, addend));
	}
	else if (nmsub)
	{
		r = irb.CreateFSub(addend, prod);
	}
	else if (sub)
	{
		r = irb.CreateFSub(prod, addend);
	}
	else
	{
		r = irb.CreateFAdd(prod, addend);
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(r, irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecIntAdd(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	unsigned laneBits = 32;
	unsigned lanes = 4;
	bool isSub = false;
	bool saturate = false;
	bool isSigned = true;
	bool carryOut = false;
	switch (i->id)
	{
	case PPC_INS_VADDUBM:
	case PPC_INS_VSUBUBM:
		laneBits = 8;
		lanes = 16;
		isSub = (i->id == PPC_INS_VSUBUBM);
		break;
	case PPC_INS_VADDUHM:
	case PPC_INS_VSUBUHM:
		laneBits = 16;
		lanes = 8;
		isSub = (i->id == PPC_INS_VSUBUHM);
		break;
	case PPC_INS_VSUBUWM:
		isSub = true;
		break;
	case PPC_INS_VADDSBS:
	case PPC_INS_VSUBSBS:
		laneBits = 8;
		lanes = 16;
		saturate = true;
		isSub = (i->id == PPC_INS_VSUBSBS);
		break;
	case PPC_INS_VADDSHS:
	case PPC_INS_VSUBSHS:
		laneBits = 16;
		lanes = 8;
		saturate = true;
		isSub = (i->id == PPC_INS_VSUBSHS);
		break;
	case PPC_INS_VADDSWS:
	case PPC_INS_VSUBSWS:
		saturate = true;
		isSub = (i->id == PPC_INS_VSUBSWS);
		break;
	case PPC_INS_VADDUBS:
	case PPC_INS_VSUBUBS:
		laneBits = 8;
		lanes = 16;
		saturate = true;
		isSigned = false;
		isSub = (i->id == PPC_INS_VSUBUBS);
		break;
	case PPC_INS_VADDUHS:
	case PPC_INS_VSUBUHS:
		laneBits = 16;
		lanes = 8;
		saturate = true;
		isSigned = false;
		isSub = (i->id == PPC_INS_VSUBUHS);
		break;
	case PPC_INS_VADDUWS:
	case PPC_INS_VSUBUWS:
		saturate = true;
		isSigned = false;
		isSub = (i->id == PPC_INS_VSUBUWS);
		break;
	case PPC_INS_VADDUDM:
	case PPC_INS_VSUBUDM:
		laneBits = 64;
		lanes = 2;
		isSub = (i->id == PPC_INS_VSUBUDM);
		break;
	case PPC_INS_VADDCUW:
		carryOut = true;
		break;
	case PPC_INS_VSUBCUW:
		carryOut = true;
		isSub = true;
		break;
	case PPC_INS_VADDUWM:
	default:
		break;
	}

	auto* elem = irb.getIntNTy(laneBits);
	auto* a = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* b = asVec128(loadOpI128(pi->operands[2], irb), elem, lanes, irb);
	llvm::Value* r = nullptr;
	if (carryOut)
	{
		if (isSub)
		{
			r = irb.CreateZExt(irb.CreateICmpUGE(a, b), a->getType());
		}
		else
		{
			auto* wide = llvm::FixedVectorType::get(irb.getInt64Ty(), lanes);
			auto* sum = irb.CreateAdd(irb.CreateZExt(a, wide), irb.CreateZExt(b, wide));
			r = irb.CreateTrunc(irb.CreateLShr(sum, 32), a->getType());
		}
		storeOpI128(pi->operands[0], irb.CreateBitCast(r, irb.getInt128Ty()), irb);
		return;
	}
	if (saturate)
	{
		// Same select clamp as x86 PADDSB/PADDUSB: widen, add/sub, clip to
		// the element range. llvm.*.sat would name a call the emitted C
		// would have to teach; a select reads as arithmetic.
		auto* wideTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits * 2), lanes);
		auto* aw = isSigned ? irb.CreateSExt(a, wideTy) : irb.CreateZExt(a, wideTy);
		auto* bw = isSigned ? irb.CreateSExt(b, wideTy) : irb.CreateZExt(b, wideTy);
		r = isSub ? irb.CreateSub(aw, bw) : irb.CreateAdd(aw, bw);
		unsigned wideBits = laneBits * 2;
		llvm::APInt lo = isSigned ? llvm::APInt::getSignedMinValue(laneBits).sext(wideBits)
								  : llvm::APInt::getZero(wideBits);
		llvm::APInt hi = isSigned ? llvm::APInt::getSignedMaxValue(laneBits).sext(wideBits)
								  : llvm::APInt::getMaxValue(laneBits).zext(wideBits);
		auto* loC = llvm::ConstantInt::get(wideTy, lo);
		auto* hiC = llvm::ConstantInt::get(wideTy, hi);
		r = irb.CreateSelect(irb.CreateICmpSLT(r, loC), loC, r);
		r = irb.CreateSelect(irb.CreateICmpSGT(r, hiC), hiC, r);
		r = irb.CreateTrunc(r, a->getType());
	}
	else
	{
		r = isSub ? irb.CreateSub(a, b) : irb.CreateAdd(a, b);
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(r, irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateXxpermdi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);
	(void)i;

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	unsigned dm = 0;
	if (pi->operands[3].type == PPC_OP_IMM)
	{
		dm = static_cast<unsigned>(pi->operands[3].imm) & 3u;
	}

	auto* i64 = irb.getInt64Ty();
	auto* sh = llvm::ConstantInt::get(a->getType(), 64);
	auto* aHi = irb.CreateTrunc(irb.CreateLShr(a, sh), i64);
	auto* aLo = irb.CreateTrunc(a, i64);
	auto* bHi = irb.CreateTrunc(irb.CreateLShr(b, sh), i64);
	auto* bLo = irb.CreateTrunc(b, i64);

	auto* outHi = (dm & 2u) ? aLo : aHi;
	auto* outLo = (dm & 1u) ? bLo : bHi;
	auto* i128 = irb.getInt128Ty();
	auto* val = irb.CreateOr(
			irb.CreateShl(irb.CreateZExt(outHi, i128), 64),
			irb.CreateZExt(outLo, i128));
	storeOpI128(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateXxspltw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	auto* src = loadOpI128(pi->operands[1], irb);
	unsigned uim = 0;
	if (pi->operands[2].type == PPC_OP_IMM)
	{
		uim = static_cast<unsigned>(pi->operands[2].imm) & 3u;
	}
	// Word 0 is the leftmost (high) 32 bits of the VSR.
	unsigned shift = (3u - uim) * 32u;
	auto* word = irb.CreateTrunc(irb.CreateLShr(src, llvm::ConstantInt::get(src->getType(), shift)), irb.getInt32Ty());
	auto* splat = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(4),
			llvm::UndefValue::get(irb.getInt32Ty()));
	llvm::Value* vec = splat;
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		vec = irb.CreateInsertElement(vec, word, irb.getInt32(lane));
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(vec, irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateLoadFloatAsInt(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	auto* add = loadIndexedEffectiveAddress(pi, irb);
	auto* w = loadIntPtr(irb, add, irb.getInt32Ty());
	llvm::Value* bits = (i->id == PPC_INS_LFIWAX) ? irb.CreateSExt(w, irb.getInt64Ty()) : irb.CreateZExt(w, irb.getInt64Ty());
	storeOp(pi->operands[0], irb.CreateBitCast(bits, irb.getDoubleTy()), irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateStoreFloatAsInt(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);
	(void)i;

	auto* src = loadOp(pi->operands[0], irb);
	llvm::Value* bits = nullptr;
	if (src->getType()->isDoubleTy())
	{
		bits = irb.CreateBitCast(src, irb.getInt64Ty());
	}
	else if (src->getType()->isIntegerTy(128))
	{
		bits = irb.CreateTrunc(irb.CreateLShr(src, llvm::ConstantInt::get(src->getType(), 64)), irb.getInt64Ty());
	}
	else
	{
		bits = irb.CreateZExtOrTrunc(src, irb.getInt64Ty());
	}
	auto* w = irb.CreateTrunc(bits, irb.getInt32Ty());
	auto* add = loadIndexedEffectiveAddress(pi, irb);
	storeIntPtr(irb, w, add, irb.getInt32Ty());
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecPerm(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	auto* c = loadOpI128(pi->operands[3], irb);
	auto* i256 = irb.getIntNTy(256);
	auto* concat = irb.CreateOr(
			irb.CreateShl(irb.CreateZExt(a, i256), 128),
			irb.CreateZExt(b, i256));
	llvm::Value* result = llvm::ConstantInt::get(irb.getInt128Ty(), 0);
	for (unsigned lane = 0; lane < 16; ++lane)
	{
		unsigned place = (15u - lane) * 8u;
		auto* cbyte = irb.CreateTrunc(
				irb.CreateLShr(c, llvm::ConstantInt::get(c->getType(), place)),
				irb.getInt8Ty());
		llvm::Value* idx = irb.CreateZExt(irb.CreateAnd(cbyte, irb.getInt8(31)), irb.getInt32Ty());
		if (i->id == PPC_INS_VPERMR)
		{
			idx = irb.CreateSub(irb.getInt32(31), idx);
		}
		auto* fromRight = irb.CreateMul(
				irb.CreateSub(irb.getInt32(31), idx),
				irb.getInt32(8));
		auto* byte = irb.CreateTrunc(
				irb.CreateLShr(concat, irb.CreateZExt(fromRight, i256)),
				irb.getInt8Ty());
		result = irb.CreateOr(
				result,
				irb.CreateShl(irb.CreateZExt(byte, irb.getInt128Ty()), place));
	}
	storeOpI128(pi->operands[0], result, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecSel(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);
	(void)i;

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	auto* c = loadOpI128(pi->operands[3], irb);
	storeOpI128(pi->operands[0], irb.CreateOr(irb.CreateAnd(a, irb.CreateNot(c)), irb.CreateAnd(b, c)), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecSldoi(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count != 3 && pi->op_count != 4)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	unsigned shb = 0;
	if (pi->op_count >= 4 && pi->operands[3].type == PPC_OP_IMM)
	{
		shb = static_cast<unsigned>(pi->operands[3].imm) & 15u;
	}
	auto* i256 = irb.getIntNTy(256);
	auto* concat = irb.CreateOr(
			irb.CreateShl(irb.CreateZExt(a, i256), 128),
			irb.CreateZExt(b, i256));
	auto* shifted = irb.CreateShl(concat, llvm::ConstantInt::get(i256, static_cast<uint64_t>(shb) * 8u));
	auto* val = irb.CreateTrunc(irb.CreateLShr(shifted, 128), irb.getInt128Ty());
	storeOpI128(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecMerge(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	auto* i128 = irb.getInt128Ty();
	unsigned elemBits = 32;
	bool high = true;
	switch (i->id)
	{
	case PPC_INS_VMRGHB:
		elemBits = 8;
		break;
	case PPC_INS_VMRGHH:
		elemBits = 16;
		break;
	case PPC_INS_VMRGHW:
		break;
	case PPC_INS_VMRGLB:
		elemBits = 8;
		high = false;
		break;
	case PPC_INS_VMRGLH:
		elemBits = 16;
		high = false;
		break;
	case PPC_INS_VMRGLW:
	case PPC_INS_XXMRGLW:
		high = false;
		break;
	case PPC_INS_XXMRGHW:
		break;
	default:
		break;
	}
	if (i->id == PPC_INS_VMRGEW || i->id == PPC_INS_VMRGOW)
	{
		// Even/odd words: [a0, b0, a2, b2] / [a1, b1, a3, b3].
		unsigned odd = i->id == PPC_INS_VMRGOW ? 1u : 0u;
		auto w = [&](llvm::Value* v, unsigned idx) {
			return irb.CreateZExt(irb.CreateTrunc(irb.CreateLShr(v, (3u - idx) * 32u), irb.getInt32Ty()), i128);
		};
		llvm::Value* r = irb.CreateShl(w(a, odd), 96);
		r = irb.CreateOr(r, irb.CreateShl(w(b, odd), 64));
		r = irb.CreateOr(r, irb.CreateShl(w(a, 2u + odd), 32));
		r = irb.CreateOr(r, w(b, 2u + odd));
		storeOpI128(pi->operands[0], r, irb);
		return;
	}
	const unsigned n = 128u / elemBits;
	const unsigned pairs = n / 2u;
	const unsigned base = high ? 0u : pairs;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto elem = [&](llvm::Value* v, unsigned ppcIdx) {
		unsigned sh = (n - 1u - ppcIdx) * elemBits;
		return irb.CreateZExt(irb.CreateTrunc(irb.CreateLShr(v, sh), elemTy), i128);
	};
	llvm::Value* val = llvm::ConstantInt::get(i128, 0);
	for (unsigned k = 0; k < pairs; ++k)
	{
		val = irb.CreateOr(val, irb.CreateShl(elem(a, base + k), (n - 1u - 2u * k) * elemBits));
		val = irb.CreateOr(val, irb.CreateShl(elem(b, base + k), (n - 1u - (2u * k + 1u)) * elemBits));
	}
	storeOpI128(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecCmp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	llvm::Type* laneTy = irb.getInt32Ty();
	llvm::Type* cmpTy = laneTy;
	unsigned lanes = 4;
	bool fp = false;
	bool gt = false;
	bool ge = false;
	bool sgt = false;
	bool bounds = false;
	switch (i->id)
	{
	case PPC_INS_VCMPEQUB:
		laneTy = irb.getInt8Ty();
		cmpTy = laneTy;
		lanes = 16;
		break;
	case PPC_INS_VCMPEQUH:
		laneTy = irb.getInt16Ty();
		cmpTy = laneTy;
		lanes = 8;
		break;
	case PPC_INS_VCMPEQUW:
		break;
	case PPC_INS_VCMPEQUD:
		laneTy = irb.getInt64Ty();
		cmpTy = laneTy;
		lanes = 2;
		break;
	case PPC_INS_VCMPEQUQ:
		laneTy = irb.getInt128Ty();
		cmpTy = laneTy;
		lanes = 1;
		break;
	case PPC_INS_VCMPGTUB:
		laneTy = irb.getInt8Ty();
		cmpTy = laneTy;
		lanes = 16;
		gt = true;
		break;
	case PPC_INS_VCMPGTUH:
		laneTy = irb.getInt16Ty();
		cmpTy = laneTy;
		lanes = 8;
		gt = true;
		break;
	case PPC_INS_VCMPGTUW:
		gt = true;
		break;
	case PPC_INS_VCMPGTUD:
		laneTy = irb.getInt64Ty();
		cmpTy = laneTy;
		lanes = 2;
		gt = true;
		break;
	case PPC_INS_VCMPGTSB:
		laneTy = irb.getInt8Ty();
		cmpTy = laneTy;
		lanes = 16;
		gt = true;
		sgt = true;
		break;
	case PPC_INS_VCMPGTSH:
		laneTy = irb.getInt16Ty();
		cmpTy = laneTy;
		lanes = 8;
		gt = true;
		sgt = true;
		break;
	case PPC_INS_VCMPGTSW:
		gt = true;
		sgt = true;
		break;
	case PPC_INS_VCMPGTSD:
		laneTy = irb.getInt64Ty();
		cmpTy = laneTy;
		lanes = 2;
		gt = true;
		sgt = true;
		break;
	case PPC_INS_VCMPBFP:
		fp = true;
		cmpTy = irb.getFloatTy();
		bounds = true;
		break;
	case PPC_INS_VCMPGEFP:
		fp = true;
		ge = true;
		cmpTy = irb.getFloatTy();
		break;
	case PPC_INS_VCMPEQFP:
	case PPC_INS_XVCMPEQSP:
		fp = true;
		cmpTy = irb.getFloatTy();
		break;
	case PPC_INS_VCMPGTFP:
	case PPC_INS_XVCMPGTSP:
		fp = true;
		gt = true;
		cmpTy = irb.getFloatTy();
		break;
	case PPC_INS_XVCMPGESP:
		fp = true;
		ge = true;
		cmpTy = irb.getFloatTy();
		break;
	case PPC_INS_XVCMPEQDP:
		fp = true;
		lanes = 2;
		laneTy = irb.getInt64Ty();
		cmpTy = irb.getDoubleTy();
		break;
	case PPC_INS_XVCMPGTDP:
		fp = true;
		gt = true;
		lanes = 2;
		laneTy = irb.getInt64Ty();
		cmpTy = irb.getDoubleTy();
		break;
	case PPC_INS_XVCMPGEDP:
		fp = true;
		ge = true;
		lanes = 2;
		laneTy = irb.getInt64Ty();
		cmpTy = irb.getDoubleTy();
		break;
	default:
		return;
	}

	auto* a = asVec128(loadOpI128(pi->operands[1], irb), cmpTy, lanes, irb);
	auto* b = asVec128(loadOpI128(pi->operands[2], irb), cmpTy, lanes, irb);
	if (bounds)
	{
		// vcmpbfp: 0 if |a|<=|b|, 0x80000000 if a>|b|, 0x40000000 if a<-|b|,
		// 0xC0000000 if unordered. Abs via sign-bit clear (vector fabs faults).
		auto* i32x4 = llvm::FixedVectorType::get(irb.getInt32Ty(), 4);
		auto* bitsA = irb.CreateBitCast(a, i32x4);
		auto* bitsB = irb.CreateBitCast(b, i32x4);
		auto* mag = llvm::ConstantInt::get(i32x4, 0x7fffffff);
		auto* absA = irb.CreateBitCast(irb.CreateAnd(bitsA, mag), a->getType());
		auto* absB = irb.CreateBitCast(irb.CreateAnd(bitsB, mag), b->getType());
		auto* negB = irb.CreateFSub(llvm::Constant::getNullValue(b->getType()), absB);
		auto* hi = llvm::ConstantInt::get(i32x4, 0x80000000);
		auto* lo = llvm::ConstantInt::get(i32x4, 0x40000000);
		auto* both = llvm::ConstantInt::get(i32x4, 0xc0000000);
		auto* z = llvm::ConstantInt::get(i32x4, 0);
		llvm::Value* r = irb.CreateSelect(irb.CreateFCmpOGT(a, absB), hi, z);
		r = irb.CreateSelect(irb.CreateFCmpOLT(a, negB), irb.CreateOr(r, lo), r);
		r = irb.CreateSelect(irb.CreateFCmpUNO(a, b), both, r);
		storeOpI128(pi->operands[0], irb.CreateBitCast(r, irb.getInt128Ty()), irb);
		return;
	}
	llvm::Value* pred = nullptr;
	if (fp)
	{
		if (gt)
		{
			pred = irb.CreateFCmpOGT(a, b);
		}
		else if (ge)
		{
			pred = irb.CreateFCmpOGE(a, b);
		}
		else
		{
			pred = irb.CreateFCmpOEQ(a, b);
		}
	}
	else if (sgt)
	{
		pred = irb.CreateICmpSGT(a, b);
	}
	else
	{
		pred = gt ? irb.CreateICmpUGT(a, b) : irb.CreateICmpEQ(a, b);
	}
	auto* maskTy = llvm::FixedVectorType::get(laneTy, lanes);
	storeOpI128(
			pi->operands[0],
			irb.CreateBitCast(irb.CreateSExt(pred, maskTy), irb.getInt128Ty()),
			irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecSplat(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	cs_ppc_op* srcOp = nullptr;
	int64_t uimm = 0;
	bool haveImm = false;
	for (uint8_t n = 1; n < pi->op_count; ++n)
	{
		if (pi->operands[n].type == PPC_OP_IMM)
		{
			uimm = pi->operands[n].imm;
			haveImm = true;
		}
		else if (pi->operands[n].type == PPC_OP_REG && srcOp == nullptr)
		{
			srcOp = &pi->operands[n];
		}
	}
	if (srcOp == nullptr || !haveImm)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	unsigned laneBits = 32;
	unsigned lanes = 4;
	switch (i->id)
	{
	case PPC_INS_VSPLTB:
		laneBits = 8;
		lanes = 16;
		break;
	case PPC_INS_VSPLTH:
		laneBits = 16;
		lanes = 8;
		break;
	case PPC_INS_VSPLTW:
	default:
		break;
	}

	unsigned uim = static_cast<unsigned>(uimm) & (lanes - 1u);
	unsigned shift = (lanes - 1u - uim) * laneBits;
	auto* src = loadOpI128(*srcOp, irb);
	auto* elemTy = irb.getIntNTy(laneBits);
	auto* elem = irb.CreateTrunc(
			irb.CreateLShr(src, llvm::ConstantInt::get(src->getType(), shift)),
			elemTy);
	llvm::Value* vec = llvm::UndefValue::get(llvm::FixedVectorType::get(elemTy, lanes));
	for (unsigned lane = 0; lane < lanes; ++lane)
	{
		vec = irb.CreateInsertElement(vec, elem, irb.getInt32(lane));
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(vec, irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecLvsl(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	auto* ea = loadIndexedEffectiveAddress(pi, irb);
	auto* sh = irb.CreateTrunc(
			irb.CreateAnd(ea, llvm::ConstantInt::get(ea->getType(), 15)),
			irb.getInt8Ty());
	auto* i128 = irb.getInt128Ty();
	llvm::Value* result = llvm::ConstantInt::get(i128, 0);
	const unsigned bias = (i->id == PPC_INS_LVSR) ? 16u : 0u;
	for (unsigned k = 0; k < 16; ++k)
	{
		auto* b = irb.CreateAdd(sh, irb.getInt8(static_cast<uint8_t>(bias + k)));
		unsigned place = (15u - k) * 8u;
		result = irb.CreateOr(
				result,
				irb.CreateShl(irb.CreateZExt(b, i128), place));
	}
	storeOpI128(pi->operands[0], result, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecPack(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	if (i->id == PPC_INS_VPKPX)
	{
		// ISA 1-5-5-5: word xRGB8888 → halfword ((s>>16)&0x8000)|((s>>9)&0x7c00)|((s>>6)&0x03e0)|((s>>3)&0x001f).
		// VA supplies the high four halfwords, VB the low four.
		auto* a = loadOpI128(pi->operands[1], irb);
		auto* b = loadOpI128(pi->operands[2], irb);
		auto* i32 = irb.getInt32Ty();
		auto* i128 = irb.getInt128Ty();
		auto packWord = [&](llvm::Value* s) {
			s = irb.CreateZExt(s, i128);
			auto* p1 = irb.CreateAnd(irb.CreateLShr(s, 16), llvm::ConstantInt::get(i128, 0x8000));
			auto* p2 = irb.CreateAnd(irb.CreateLShr(s, 9), llvm::ConstantInt::get(i128, 0x7c00));
			auto* p3 = irb.CreateAnd(irb.CreateLShr(s, 6), llvm::ConstantInt::get(i128, 0x03e0));
			auto* p4 = irb.CreateAnd(irb.CreateLShr(s, 3), llvm::ConstantInt::get(i128, 0x001f));
			return irb.CreateOr(irb.CreateOr(p1, p2), irb.CreateOr(p3, p4));
		};
		llvm::Value* result = llvm::ConstantInt::get(i128, 0);
		auto packSrc = [&](llvm::Value* src, unsigned dstBase) {
			for (unsigned k = 0; k < 4; ++k)
			{
				auto* w = irb.CreateTrunc(irb.CreateLShr(src, (3u - k) * 32u), i32);
				unsigned place = (7u - (dstBase + k)) * 16u;
				result = irb.CreateOr(result, irb.CreateShl(packWord(w), place));
			}
		};
		packSrc(a, 0);
		packSrc(b, 4);
		storeOpI128(pi->operands[0], result, irb);
		return;
	}

	unsigned srcBits = 16;
	unsigned nSrc = 8;
	bool saturate = false;
	bool srcSigned = false;
	bool dstUnsigned = false;
	switch (i->id)
	{
	case PPC_INS_VPKUHUM:
		break;
	case PPC_INS_VPKUHUS:
		saturate = true;
		break;
	case PPC_INS_VPKSHSS:
		saturate = true;
		srcSigned = true;
		break;
	case PPC_INS_VPKSHUS:
		saturate = true;
		srcSigned = true;
		dstUnsigned = true;
		break;
	case PPC_INS_VPKUWUM:
		srcBits = 32;
		nSrc = 4;
		break;
	case PPC_INS_VPKUWUS:
		srcBits = 32;
		nSrc = 4;
		saturate = true;
		break;
	case PPC_INS_VPKSWSS:
		srcBits = 32;
		nSrc = 4;
		saturate = true;
		srcSigned = true;
		break;
	case PPC_INS_VPKSWUS:
		srcBits = 32;
		nSrc = 4;
		saturate = true;
		srcSigned = true;
		dstUnsigned = true;
		break;
	case PPC_INS_VPKUDUM:
		srcBits = 64;
		nSrc = 2;
		break;
	case PPC_INS_VPKUDUS:
		srcBits = 64;
		nSrc = 2;
		saturate = true;
		break;
	case PPC_INS_VPKSDSS:
		srcBits = 64;
		nSrc = 2;
		saturate = true;
		srcSigned = true;
		break;
	case PPC_INS_VPKSDUS:
		srcBits = 64;
		nSrc = 2;
		saturate = true;
		srcSigned = true;
		dstUnsigned = true;
		break;
	default:
		return;
	}

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	unsigned dstBits = srcBits / 2;
	auto* srcTy = irb.getIntNTy(srcBits);
	auto* dstTy = irb.getIntNTy(dstBits);
	auto* i128 = irb.getInt128Ty();
	llvm::Value* result = llvm::ConstantInt::get(i128, 0);
	auto packHalf = [&](llvm::Value* src, unsigned dstBase) {
		for (unsigned k = 0; k < nSrc; ++k)
		{
			unsigned srcShift = (nSrc - 1u - k) * srcBits;
			auto* elem = irb.CreateTrunc(
					irb.CreateLShr(src, llvm::ConstantInt::get(src->getType(), srcShift)),
					srcTy);
			if (saturate)
			{
				if (dstUnsigned)
				{
					auto* zero = llvm::ConstantInt::get(srcTy, 0);
					auto* hiC = llvm::ConstantInt::get(
							srcTy, llvm::APInt::getMaxValue(dstBits).zext(srcBits));
					elem = irb.CreateSelect(irb.CreateICmpSLT(elem, zero), zero, elem);
					elem = irb.CreateSelect(irb.CreateICmpSGT(elem, hiC), hiC, elem);
				}
				else
				{
					llvm::APInt lo = srcSigned ? llvm::APInt::getSignedMinValue(dstBits).sext(srcBits)
											   : llvm::APInt::getZero(srcBits);
					llvm::APInt hi = srcSigned ? llvm::APInt::getSignedMaxValue(dstBits).sext(srcBits)
											   : llvm::APInt::getMaxValue(dstBits).zext(srcBits);
					auto* loC = llvm::ConstantInt::get(srcTy, lo);
					auto* hiC = llvm::ConstantInt::get(srcTy, hi);
					if (srcSigned)
					{
						elem = irb.CreateSelect(irb.CreateICmpSLT(elem, loC), loC, elem);
						elem = irb.CreateSelect(irb.CreateICmpSGT(elem, hiC), hiC, elem);
					}
					else
					{
						elem = irb.CreateSelect(irb.CreateICmpUGT(elem, hiC), hiC, elem);
					}
				}
			}
			elem = irb.CreateTrunc(elem, dstTy);
			unsigned dstPlace = (2u * nSrc - 1u - (dstBase + k)) * dstBits;
			result = irb.CreateOr(
					result,
					irb.CreateShl(irb.CreateZExt(elem, i128), dstPlace));
		}
	};
	packHalf(a, 0);
	packHalf(b, nSrc);
	storeOpI128(pi->operands[0], result, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecShift128(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);

	unsigned laneBits = 0;
	unsigned lanes = 0;
	bool left = false;
	bool arithmetic = false;
	bool rotate = false;
	switch (i->id)
	{
	case PPC_INS_VSL:
	{
		auto* sh = irb.CreateAnd(b, llvm::ConstantInt::get(b->getType(), 7));
		storeOpI128(pi->operands[0], irb.CreateShl(a, sh), irb);
		return;
	}
	case PPC_INS_VSR:
	{
		auto* sh = irb.CreateAnd(b, llvm::ConstantInt::get(b->getType(), 7));
		storeOpI128(pi->operands[0], irb.CreateLShr(a, sh), irb);
		return;
	}
	case PPC_INS_VSLO:
	case PPC_INS_VSRO:
	{
		// sh = VRB[121:124] = (low byte >> 3) & 15; shift the whole i128 by sh*8.
		auto* byte = irb.CreateTrunc(b, irb.getInt8Ty());
		auto* oct = irb.CreateAnd(irb.CreateLShr(byte, 3), llvm::ConstantInt::get(byte->getType(), 15));
		auto* sh = irb.CreateMul(irb.CreateZExt(oct, a->getType()), llvm::ConstantInt::get(a->getType(), 8));
		storeOpI128(
				pi->operands[0],
				(i->id == PPC_INS_VSLO) ? irb.CreateShl(a, sh) : irb.CreateLShr(a, sh),
				irb);
		return;
	}
	case PPC_INS_VSLB:
		laneBits = 8;
		lanes = 16;
		left = true;
		break;
	case PPC_INS_VSLH:
		laneBits = 16;
		lanes = 8;
		left = true;
		break;
	case PPC_INS_VSLW:
		laneBits = 32;
		lanes = 4;
		left = true;
		break;
	case PPC_INS_VSLD:
		laneBits = 64;
		lanes = 2;
		left = true;
		break;
	case PPC_INS_VSRB:
		laneBits = 8;
		lanes = 16;
		break;
	case PPC_INS_VSRH:
		laneBits = 16;
		lanes = 8;
		break;
	case PPC_INS_VSRW:
		laneBits = 32;
		lanes = 4;
		break;
	case PPC_INS_VSRD:
		laneBits = 64;
		lanes = 2;
		break;
	case PPC_INS_VSRAB:
		laneBits = 8;
		lanes = 16;
		arithmetic = true;
		break;
	case PPC_INS_VSRAH:
		laneBits = 16;
		lanes = 8;
		arithmetic = true;
		break;
	case PPC_INS_VSRAW:
		laneBits = 32;
		lanes = 4;
		arithmetic = true;
		break;
	case PPC_INS_VSRAD:
		laneBits = 64;
		lanes = 2;
		arithmetic = true;
		break;
	case PPC_INS_VRLB:
		laneBits = 8;
		lanes = 16;
		rotate = true;
		break;
	case PPC_INS_VRLH:
		laneBits = 16;
		lanes = 8;
		rotate = true;
		break;
	case PPC_INS_VRLW:
		laneBits = 32;
		lanes = 4;
		rotate = true;
		break;
	case PPC_INS_VRLD:
		laneBits = 64;
		lanes = 2;
		rotate = true;
		break;
	default:
		return;
	}

	auto* elem = irb.getIntNTy(laneBits);
	auto* va = asVec128(a, elem, lanes, irb);
	auto* vb = asVec128(b, elem, lanes, irb);
	auto* sh = irb.CreateAnd(vb, llvm::ConstantInt::get(vb->getType(), laneBits - 1));
	llvm::Value* r = nullptr;
	if (rotate)
	{
		// x86 VPROLD analogue: amount is the low bits of each rb lane
		// (Capstone: vrt, vra, vrb — not a splat). rsh = -sh & (w-1) so a
		// zero count does not lshr by the element width.
		auto* rsh = irb.CreateAnd(
				irb.CreateSub(llvm::ConstantInt::get(vb->getType(), 0), sh),
				llvm::ConstantInt::get(vb->getType(), laneBits - 1));
		r = irb.CreateOr(irb.CreateShl(va, sh), irb.CreateLShr(va, rsh));
	}
	else if (left)
	{
		r = irb.CreateShl(va, sh);
	}
	else if (arithmetic)
	{
		r = irb.CreateAShr(va, sh);
	}
	else
	{
		r = irb.CreateLShr(va, sh);
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(r, irb.getInt128Ty()), irb);
}

/**
 * xvabsdp/xvabssp / xvnegdp/xvnegsp / xvnabs* / xvcpsgn*. llvmir-emul
 * faults on vector fneg/copysign, so this is the x86 AND/XOR sign-bit
 * form on the whole i128. nabs sets the sign bit (-abs). cpsgn takes
 * magnitude from XB and sign from XA.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecFpSign(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	bool dp = false;
	bool neg = false;
	bool nabs = false;
	bool cpsgn = false;
	switch (i->id)
	{
	case PPC_INS_XVABSDP:
		dp = true;
		break;
	case PPC_INS_XVABSSP:
		break;
	case PPC_INS_XVNEGDP:
		dp = true;
		neg = true;
		break;
	case PPC_INS_XVNEGSP:
		neg = true;
		break;
	case PPC_INS_XVNABSDP:
		dp = true;
		nabs = true;
		break;
	case PPC_INS_XVNABSSP:
		nabs = true;
		break;
	case PPC_INS_XVCPSGNDP:
		dp = true;
		cpsgn = true;
		break;
	case PPC_INS_XVCPSGNSP:
		cpsgn = true;
		break;
	default:
		return;
	}

	if (cpsgn)
	{
		EXPECT_IS_TERNARY(i, pi, irb);
	}
	else
	{
		EXPECT_IS_BINARY(i, pi, irb);
	}

	uint64_t signLane = dp ? 0x8000000000000000ULL : 0x8000000080000000ULL;
	uint64_t magLane = dp ? 0x7fffffffffffffffULL : 0x7fffffff7fffffffULL;
	const uint64_t signWords[2] = {signLane, signLane};
	auto* signMask = llvm::ConstantInt::get(irb.getInt128Ty(), llvm::APInt(128, llvm::ArrayRef<uint64_t>(signWords, 2)));

	if (cpsgn)
	{
		auto* a = loadOpI128(pi->operands[1], irb);
		auto* b = loadOpI128(pi->operands[2], irb);
		auto* signDiff = irb.CreateAnd(irb.CreateXor(a, b), signMask);
		storeOpI128(pi->operands[0], irb.CreateXor(b, signDiff), irb);
		return;
	}

	auto* src = loadOpI128(pi->operands[1], irb);
	if (nabs)
	{
		storeOpI128(pi->operands[0], irb.CreateOr(src, signMask), irb);
		return;
	}
	if (neg)
	{
		storeOpI128(pi->operands[0], irb.CreateXor(src, signMask), irb);
		return;
	}
	const uint64_t magWords[2] = {magLane, magLane};
	auto* magMask = llvm::ConstantInt::get(irb.getInt128Ty(), llvm::APInt(128, llvm::ArrayRef<uint64_t>(magWords, 2)));
	storeOpI128(pi->operands[0], irb.CreateAnd(src, magMask), irb);
}

/**
 * vcfsx: signed word → SP (x86 CVTDQ2PS). vcfux: unsigned word → SP.
 * vctuxs: SP → unsigned word saturate with chop (CVTTPS2DQ-class, unsigned).
 * vctsxs: SP → signed word saturate with chop. Capstone reports UIM as IMM.
 * gcc -O2 vec_ctf/vec_ctu emit UIM=0; a non-zero UIM scales by 2^±UIM.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecFpConvert(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	cs_ppc_op* srcOp = nullptr;
	unsigned uim = 0;
	for (uint8_t n = 1; n < pi->op_count; ++n)
	{
		if (pi->operands[n].type == PPC_OP_REG && srcOp == nullptr)
		{
			srcOp = &pi->operands[n];
		}
		else if (pi->operands[n].type == PPC_OP_IMM)
		{
			uim = static_cast<unsigned>(pi->operands[n].imm) & 31u;
		}
	}
	if (srcOp == nullptr)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	auto* i32x4 = llvm::FixedVectorType::get(irb.getInt32Ty(), 4);
	auto* f32x4 = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
	if (i->id == PPC_INS_VCFSX || i->id == PPC_INS_VCFUX)
	{
		auto* src = asVec128(loadOpI128(*srcOp, irb), irb.getInt32Ty(), 4, irb);
		llvm::Value* fp = (i->id == PPC_INS_VCFSX) ? irb.CreateSIToFP(src, f32x4) : irb.CreateUIToFP(src, f32x4);
		if (uim != 0)
		{
			fp = irb.CreateFMul(fp, llvm::ConstantFP::get(f32x4, std::ldexp(1.0, -static_cast<int>(uim))));
		}
		storeOpI128(pi->operands[0], irb.CreateBitCast(fp, irb.getInt128Ty()), irb);
		return;
	}
	if (i->id == PPC_INS_VCTUXS || i->id == PPC_INS_VCTSXS)
	{
		llvm::Value* src = asVec128(loadOpI128(*srcOp, irb), irb.getFloatTy(), 4, irb);
		if (uim != 0)
		{
			src = irb.CreateFMul(src, llvm::ConstantFP::get(f32x4, std::ldexp(1.0, static_cast<int>(uim))));
		}
		// Per-lane: llvmir-emul's vector fcmp against a 2^32 splat saturates
		// in-range positives. Scalar generateFpToIntBounded is the fctiw path.
		const bool isSigned = i->id == PPC_INS_VCTSXS;
		llvm::Value* conv = llvm::UndefValue::get(i32x4);
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			auto* e = irb.CreateExtractElement(src, lane);
			conv = irb.CreateInsertElement(
					conv, generateFpToIntBounded(e, irb.getInt32Ty(), isSigned, irb), lane);
		}
		storeOpI128(pi->operands[0], irb.CreateBitCast(conv, irb.getInt128Ty()), irb);
		return;
	}

	auto* bits = loadOpI128(*srcOp, irb);
	if (i->id == PPC_INS_XVCVSPDP)
	{
		// Even BE words (high 32 of each doubleword) fpext to DP.
		auto* i64 = irb.getInt64Ty();
		auto* hi = irb.CreateTrunc(irb.CreateLShr(bits, 64), i64);
		auto* lo = irb.CreateTrunc(bits, i64);
		auto* s0 = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(hi, 32), irb.getInt32Ty()), irb.getFloatTy());
		auto* s1 = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(lo, 32), irb.getInt32Ty()), irb.getFloatTy());
		auto* d0 = irb.CreateZExt(irb.CreateBitCast(irb.CreateFPExt(s0, irb.getDoubleTy()), i64), irb.getInt128Ty());
		auto* d1 = irb.CreateZExt(irb.CreateBitCast(irb.CreateFPExt(s1, irb.getDoubleTy()), i64), irb.getInt128Ty());
		storeOpI128(pi->operands[0], irb.CreateOr(irb.CreateShl(d0, 64), d1), irb);
		return;
	}
	if (i->id == PPC_INS_XVCVDPSP)
	{
		auto* i64 = irb.getInt64Ty();
		auto* d0 = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(bits, 64), i64), irb.getDoubleTy());
		auto* d1 = irb.CreateBitCast(irb.CreateTrunc(bits, i64), irb.getDoubleTy());
		auto* w0 = irb.CreateZExt(irb.CreateBitCast(irb.CreateFPTrunc(d0, irb.getFloatTy()), irb.getInt32Ty()), i64);
		auto* w1 = irb.CreateZExt(irb.CreateBitCast(irb.CreateFPTrunc(d1, irb.getFloatTy()), irb.getInt32Ty()), i64);
		auto* hi = irb.CreateShl(w0, 32);
		auto* lo = irb.CreateShl(w1, 32);
		storeOpI128(
				pi->operands[0],
				irb.CreateOr(irb.CreateShl(irb.CreateZExt(hi, irb.getInt128Ty()), 64), irb.CreateZExt(lo, irb.getInt128Ty())),
				irb);
		return;
	}
	if (i->id == PPC_INS_XVCVSXWSP || i->id == PPC_INS_XVCVUXWSP)
	{
		auto* src = asVec128(bits, irb.getInt32Ty(), 4, irb);
		auto* fp = (i->id == PPC_INS_XVCVSXWSP)
				? irb.CreateSIToFP(src, f32x4)
				: irb.CreateUIToFP(src, f32x4);
		storeOpI128(pi->operands[0], irb.CreateBitCast(fp, irb.getInt128Ty()), irb);
		return;
	}
	if (i->id == PPC_INS_XVCVSPSXWS || i->id == PPC_INS_XVCVSPUXWS)
	{
		auto* src = asVec128(bits, irb.getFloatTy(), 4, irb);
		const bool isSigned = i->id == PPC_INS_XVCVSPSXWS;
		llvm::Value* conv = llvm::UndefValue::get(i32x4);
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			conv = irb.CreateInsertElement(
					conv,
					generateFpToIntBounded(irb.CreateExtractElement(src, lane), irb.getInt32Ty(), isSigned, irb),
					lane);
		}
		storeOpI128(pi->operands[0], irb.CreateBitCast(conv, irb.getInt128Ty()), irb);
		return;
	}
	if (i->id == PPC_INS_XVCVSXDDP || i->id == PPC_INS_XVCVUXDDP)
	{
		auto* src = asVec128(bits, irb.getInt64Ty(), 2, irb);
		auto* f64x2 = llvm::FixedVectorType::get(irb.getDoubleTy(), 2);
		auto* fp = (i->id == PPC_INS_XVCVSXDDP)
				? irb.CreateSIToFP(src, f64x2)
				: irb.CreateUIToFP(src, f64x2);
		storeOpI128(pi->operands[0], irb.CreateBitCast(fp, irb.getInt128Ty()), irb);
		return;
	}
	if (i->id == PPC_INS_XVCVDPSXDS || i->id == PPC_INS_XVCVDPUXDS)
	{
		auto* src = asVec128(bits, irb.getDoubleTy(), 2, irb);
		auto* i64x2 = llvm::FixedVectorType::get(irb.getInt64Ty(), 2);
		const bool isSigned = i->id == PPC_INS_XVCVDPSXDS;
		llvm::Value* conv = llvm::UndefValue::get(i64x2);
		for (unsigned lane = 0; lane < 2; ++lane)
		{
			conv = irb.CreateInsertElement(
					conv,
					generateFpToIntBounded(irb.CreateExtractElement(src, lane), irb.getInt64Ty(), isSigned, irb),
					lane);
		}
		storeOpI128(pi->operands[0], irb.CreateBitCast(conv, irb.getInt128Ty()), irb);
		return;
	}
	if (i->id == PPC_INS_XVCVDPSXWS || i->id == PPC_INS_XVCVDPUXWS)
	{
		// Two DP → two i32 in even BE words (high 32 of each doubleword); odd words 0.
		auto* i64 = irb.getInt64Ty();
		auto* d0 = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(bits, 64), i64), irb.getDoubleTy());
		auto* d1 = irb.CreateBitCast(irb.CreateTrunc(bits, i64), irb.getDoubleTy());
		const bool isSigned = i->id == PPC_INS_XVCVDPSXWS;
		auto* w0 = irb.CreateZExt(generateFpToIntBounded(d0, irb.getInt32Ty(), isSigned, irb), i64);
		auto* w1 = irb.CreateZExt(generateFpToIntBounded(d1, irb.getInt32Ty(), isSigned, irb), i64);
		auto* hi = irb.CreateShl(w0, 32);
		auto* lo = irb.CreateShl(w1, 32);
		storeOpI128(
				pi->operands[0],
				irb.CreateOr(irb.CreateShl(irb.CreateZExt(hi, irb.getInt128Ty()), 64), irb.CreateZExt(lo, irb.getInt128Ty())),
				irb);
		return;
	}
	if (i->id == PPC_INS_XVCVSPSXDS || i->id == PPC_INS_XVCVSPUXDS)
	{
		// Even BE words (high 32 of each DW) as SP → two i64.
		auto* i64 = irb.getInt64Ty();
		auto* hi = irb.CreateTrunc(irb.CreateLShr(bits, 64), i64);
		auto* lo = irb.CreateTrunc(bits, i64);
		auto* s0 = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(hi, 32), irb.getInt32Ty()), irb.getFloatTy());
		auto* s1 = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(lo, 32), irb.getInt32Ty()), irb.getFloatTy());
		const bool isSigned = i->id == PPC_INS_XVCVSPSXDS;
		auto* d0 = irb.CreateZExt(generateFpToIntBounded(s0, i64, isSigned, irb), irb.getInt128Ty());
		auto* d1 = irb.CreateZExt(generateFpToIntBounded(s1, i64, isSigned, irb), irb.getInt128Ty());
		storeOpI128(pi->operands[0], irb.CreateOr(irb.CreateShl(d0, 64), d1), irb);
		return;
	}
	if (i->id == PPC_INS_XVCVSXDSP || i->id == PPC_INS_XVCVUXDSP)
	{
		// Two i64 → two SP in even BE words.
		auto* i64 = irb.getInt64Ty();
		auto* src = asVec128(bits, i64, 2, irb);
		auto* f32 = irb.getFloatTy();
		llvm::Value* result = llvm::ConstantInt::get(irb.getInt128Ty(), 0);
		for (unsigned lane = 0; lane < 2; ++lane)
		{
			auto* iv = irb.CreateExtractElement(src, lane);
			auto* fp = (i->id == PPC_INS_XVCVSXDSP) ? irb.CreateSIToFP(iv, f32) : irb.CreateUIToFP(iv, f32);
			auto* w = irb.CreateZExt(irb.CreateBitCast(fp, irb.getInt32Ty()), irb.getInt128Ty());
			unsigned place = (lane == 0) ? 96u : 32u;
			result = irb.CreateOr(result, irb.CreateShl(w, place));
		}
		storeOpI128(pi->operands[0], result, irb);
		return;
	}
	if (i->id == PPC_INS_XVCVSXWDP || i->id == PPC_INS_XVCVUXWDP)
	{
		// Even BE i32 (high 32 of each DW) → two DP.
		auto* i64 = irb.getInt64Ty();
		auto* hi = irb.CreateTrunc(irb.CreateLShr(bits, 64), i64);
		auto* lo = irb.CreateTrunc(bits, i64);
		auto* w0 = irb.CreateTrunc(irb.CreateLShr(hi, 32), irb.getInt32Ty());
		auto* w1 = irb.CreateTrunc(irb.CreateLShr(lo, 32), irb.getInt32Ty());
		auto* f64 = irb.getDoubleTy();
		llvm::Value* d0 = nullptr;
		llvm::Value* d1 = nullptr;
		if (i->id == PPC_INS_XVCVSXWDP)
		{
			d0 = irb.CreateSIToFP(w0, f64);
			d1 = irb.CreateSIToFP(w1, f64);
		}
		else
		{
			d0 = irb.CreateUIToFP(w0, f64);
			d1 = irb.CreateUIToFP(w1, f64);
		}
		auto* p0 = irb.CreateZExt(irb.CreateBitCast(d0, i64), irb.getInt128Ty());
		auto* p1 = irb.CreateZExt(irb.CreateBitCast(d1, i64), irb.getInt128Ty());
		storeOpI128(pi->operands[0], irb.CreateOr(irb.CreateShl(p0, 64), p1), irb);
		return;
	}
}

/**
 * vrfin/vrfiz/vrfip/vrfim — packed SP round to integral value, same modes as
 * x86 ROUNDPS (nearest-even / trunc / ceil / floor).
 * xvrdpi* — packed DP; nearbyint (current / nearest) / floor / ceil / trunc.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecFpRound(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	llvm::Intrinsic::ID iid = llvm::Intrinsic::roundeven;
	llvm::Type* elem = irb.getFloatTy();
	unsigned lanes = 4;
	switch (i->id)
	{
	case PPC_INS_VRFIN:
		break;
	case PPC_INS_VRFIZ:
		iid = llvm::Intrinsic::trunc;
		break;
	case PPC_INS_VRFIP:
		iid = llvm::Intrinsic::ceil;
		break;
	case PPC_INS_VRFIM:
		iid = llvm::Intrinsic::floor;
		break;
	case PPC_INS_XVRDPI:
	case PPC_INS_XVRDPIC:
		iid = llvm::Intrinsic::nearbyint;
		elem = irb.getDoubleTy();
		lanes = 2;
		break;
	case PPC_INS_XVRDPIM:
		iid = llvm::Intrinsic::floor;
		elem = irb.getDoubleTy();
		lanes = 2;
		break;
	case PPC_INS_XVRDPIP:
		iid = llvm::Intrinsic::ceil;
		elem = irb.getDoubleTy();
		lanes = 2;
		break;
	case PPC_INS_XVRDPIZ:
		iid = llvm::Intrinsic::trunc;
		elem = irb.getDoubleTy();
		lanes = 2;
		break;
	case PPC_INS_XVRSPI:
	case PPC_INS_XVRSPIC:
		iid = llvm::Intrinsic::nearbyint;
		break;
	case PPC_INS_XVRSPIM:
		iid = llvm::Intrinsic::floor;
		break;
	case PPC_INS_XVRSPIP:
		iid = llvm::Intrinsic::ceil;
		break;
	case PPC_INS_XVRSPIZ:
		iid = llvm::Intrinsic::trunc;
		break;
	default:
		return;
	}

	auto* src = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid, src->getType());
	storeOpI128(pi->operands[0], irb.CreateBitCast(irb.CreateCall(f, {src}), irb.getInt128Ty()), irb);
}

/**
 * QPX packed arithmetic on the 256-bit QFRs. Each register is four doubles
 * (i256 bitcast of <4 x double>), same shape as VSX XV* on i128.
 *
 * QVF*S rounds through <4 x float> and fpext back: QPX singles live in the
 * four 64-bit slots after a convert, which is what qvlfsx/qvstfsx do.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxFpArith(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	const bool sp = isQpxSingle(i->id);
	auto* a = qpxAsArithVec(loadOpI256(pi->operands[1], irb), sp, irb);
	auto* b = qpxAsArithVec(loadOpI256(pi->operands[2], irb), sp, irb);
	llvm::Value* r = nullptr;
	switch (i->id)
	{
	case PPC_INS_QVFADD:
	case PPC_INS_QVFADDS: r = irb.CreateFAdd(a, b); break;
	case PPC_INS_QVFSUB:
	case PPC_INS_QVFSUBS: r = irb.CreateFSub(a, b); break;
	case PPC_INS_QVFMUL:
	case PPC_INS_QVFMULS: r = irb.CreateFMul(a, b); break;
	default: return;
	}
	storeOpI256(pi->operands[0], qpxFromArithVec(r, irb), irb);
}

/**
 * QPX FMA. Assembly is FRT, FRA, FRC, FRB: FRT = FRA * FRC ± FRB, same
 * operand names as scalar fmadd. Vector fmul+fadd/fsub because llvmir-emul
 * faults on vector fneg/fma.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxFpFma(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);

	const bool sp = isQpxSingle(i->id);
	auto* a = qpxAsArithVec(loadOpI256(pi->operands[1], irb), sp, irb);
	auto* c = qpxAsArithVec(loadOpI256(pi->operands[2], irb), sp, irb);
	auto* b = qpxAsArithVec(loadOpI256(pi->operands[3], irb), sp, irb);
	auto* prod = irb.CreateFMul(a, c);
	llvm::Value* r = nullptr;
	switch (i->id)
	{
	case PPC_INS_QVFMADD:
	case PPC_INS_QVFMADDS:
		r = irb.CreateFAdd(prod, b);
		break;
	case PPC_INS_QVFMSUB:
	case PPC_INS_QVFMSUBS:
		r = irb.CreateFSub(prod, b);
		break;
	case PPC_INS_QVFNMADD:
	case PPC_INS_QVFNMADDS:
		r = irb.CreateFSub(llvm::Constant::getNullValue(prod->getType()), irb.CreateFAdd(prod, b));
		break;
	case PPC_INS_QVFNMSUB:
	case PPC_INS_QVFNMSUBS:
		r = irb.CreateFSub(b, prod);
		break;
	default:
		return;
	}
	storeOpI256(pi->operands[0], qpxFromArithVec(r, irb), irb);
}

/**
 * qvfabs/qvfneg/qvfnabs/qvfcpsgn/qvfmr. Sign-bit AND/XOR on the whole i256
 * (four 0x8000… lanes), same as XVABSDP. qvfmr is a register move.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxFpSign(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	bool neg = false;
	bool nabs = false;
	bool cpsgn = false;
	bool move = false;
	switch (i->id)
	{
	case PPC_INS_QVFABS:
		break;
	case PPC_INS_QVFNEG:
		neg = true;
		break;
	case PPC_INS_QVFNABS:
		nabs = true;
		break;
	case PPC_INS_QVFCPSGN:
		cpsgn = true;
		break;
	case PPC_INS_QVFMR:
		move = true;
		break;
	default:
		return;
	}

	if (cpsgn)
	{
		EXPECT_IS_TERNARY(i, pi, irb);
	}
	else
	{
		EXPECT_IS_BINARY(i, pi, irb);
	}

	if (move)
	{
		storeOpI256(pi->operands[0], loadOpI256(pi->operands[1], irb), irb);
		return;
	}

	auto* i256 = irb.getIntNTy(256);
	const uint64_t signW[4] = {
			0x8000000000000000ULL, 0x8000000000000000ULL,
			0x8000000000000000ULL, 0x8000000000000000ULL};
	const uint64_t magW[4] = {
			0x7fffffffffffffffULL, 0x7fffffffffffffffULL,
			0x7fffffffffffffffULL, 0x7fffffffffffffffULL};
	auto* signMask = llvm::ConstantInt::get(i256, llvm::APInt(256, llvm::ArrayRef<uint64_t>(signW, 4)));
	auto* magMask = llvm::ConstantInt::get(i256, llvm::APInt(256, llvm::ArrayRef<uint64_t>(magW, 4)));

	if (cpsgn)
	{
		auto* a = loadOpI256(pi->operands[1], irb);
		auto* b = loadOpI256(pi->operands[2], irb);
		auto* signDiff = irb.CreateAnd(irb.CreateXor(a, b), signMask);
		storeOpI256(pi->operands[0], irb.CreateXor(b, signDiff), irb);
		return;
	}

	auto* src = loadOpI256(pi->operands[1], irb);
	if (nabs)
	{
		storeOpI256(pi->operands[0], irb.CreateOr(src, signMask), irb);
		return;
	}
	if (neg)
	{
		storeOpI256(pi->operands[0], irb.CreateXor(src, signMask), irb);
		return;
	}
	storeOpI256(pi->operands[0], irb.CreateAnd(src, magMask), irb);
}

/**
 * qvfcmpeq/qvfcmpgt/qvfcmplt. QPX booleans are 1.0 (true) and -1.0 (false)
 * so qvfsel's sign-bit select matches LLVM vselect: true is non-negative.
 * Scalar per-lane fcmp: llvmir-emul does not honour vector ConstantFP splats.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxFpCmp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	auto* aBits = loadOpI256(pi->operands[1], irb);
	auto* bBits = loadOpI256(pi->operands[2], irb);
	auto* i64 = irb.getInt64Ty();
	auto* i256 = irb.getIntNTy(256);
	auto* one = llvm::ConstantInt::get(i64, 0x3ff0000000000000ULL);
	auto* negOne = llvm::ConstantInt::get(i64, 0xbff0000000000000ULL);
	llvm::Value* val = llvm::ConstantInt::get(i256, 0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		unsigned sh = (3u - lane) * 64u;
		auto* al = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(aBits, sh), i64), irb.getDoubleTy());
		auto* bl = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(bBits, sh), i64), irb.getDoubleTy());
		llvm::Value* pred = nullptr;
		switch (i->id)
		{
		case PPC_INS_QVFCMPEQ: pred = irb.CreateFCmpOEQ(al, bl); break;
		case PPC_INS_QVFCMPGT: pred = irb.CreateFCmpOGT(al, bl); break;
		case PPC_INS_QVFCMPLT: pred = irb.CreateFCmpOLT(al, bl); break;
		case PPC_INS_QVFTSTNAN: pred = irb.CreateFCmpUNO(al, bl); break;
		default: return;
		}
		auto* bits = irb.CreateSelect(pred, one, negOne);
		val = irb.CreateOr(val, irb.CreateShl(irb.CreateZExt(bits, i256), sh));
	}
	storeOpI256(pi->operands[0], val, irb);
}

/**
 * qvfsel FRT, FRA, FRC, FRB: FRT = FRA >= 0 ? FRC : FRB (ordered; NaN → FRB).
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxFpSel(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);
	(void)i;

	auto* a = asVec256(loadOpI256(pi->operands[1], irb), irb.getDoubleTy(), 4, irb);
	auto* c = asVec256(loadOpI256(pi->operands[2], irb), irb.getDoubleTy(), 4, irb);
	auto* b = asVec256(loadOpI256(pi->operands[3], irb), irb.getDoubleTy(), 4, irb);
	auto* zero = llvm::ConstantFP::get(a->getType(), 0.0);
	auto* r = irb.CreateSelect(irb.CreateFCmpOGE(a, zero), c, b);
	storeOpI256(pi->operands[0], irb.CreateBitCast(r, irb.getIntNTy(256)), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::qpxWritebackIndexed(
		cs_insn* i, cs_ppc* pi, llvm::Value* ea, llvm::IRBuilder<>& irb)
{
	if (!qpxIsUpdateForm(i->id))
	{
		return;
	}
	auto& ppcOp1 = pi->operands[1];
	if (ppcOp1.type == PPC_OP_REG && ppcOp1.reg != PPC_REG_INVALID)
	{
		storeRegister(ppcOp1.reg, ea, irb);
	}
	else if (ppcOp1.type == PPC_OP_MEM && ppcOp1.mem.base != PPC_REG_INVALID
			&& ppcOp1.mem.base != PPC_REG_R0 && ppcOp1.mem.base != PPC_REG_ZERO)
	{
		storeRegister(ppcOp1.mem.base, ea, irb);
	}
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxFpConvert(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	auto* bits = loadOpI256(pi->operands[1], irb);
	auto* i64 = irb.getInt64Ty();
	auto* i256 = irb.getIntNTy(256);
	auto* f64 = irb.getDoubleTy();
	llvm::Value* val = llvm::ConstantInt::get(i256, 0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		auto* slot = qpxGetSlot(bits, lane, irb);
		llvm::Value* out = nullptr;
		switch (i->id)
		{
		case PPC_INS_QVFCFID:
		case PPC_INS_QVFCFIDS:
		case PPC_INS_QVFCFIDU:
		case PPC_INS_QVFCFIDUS: {
			bool uns = (i->id == PPC_INS_QVFCFIDU || i->id == PPC_INS_QVFCFIDUS);
			auto* fp = uns ? irb.CreateUIToFP(slot, f64) : irb.CreateSIToFP(slot, f64);
			if (i->id == PPC_INS_QVFCFIDS || i->id == PPC_INS_QVFCFIDUS)
			{
				fp = roundToSingle(irb, fp);
			}
			out = irb.CreateBitCast(fp, i64);
			break;
		}
		case PPC_INS_QVFCTID:
		case PPC_INS_QVFCTIDU:
		case PPC_INS_QVFCTIDUZ:
		case PPC_INS_QVFCTIDZ:
		case PPC_INS_QVFCTIW:
		case PPC_INS_QVFCTIWU:
		case PPC_INS_QVFCTIWUZ:
		case PPC_INS_QVFCTIWZ: {
			auto* fp = irb.CreateBitCast(slot, f64);
			bool wide = (i->id == PPC_INS_QVFCTID || i->id == PPC_INS_QVFCTIDU
					|| i->id == PPC_INS_QVFCTIDUZ || i->id == PPC_INS_QVFCTIDZ);
			bool uns = (i->id == PPC_INS_QVFCTIDU || i->id == PPC_INS_QVFCTIDUZ
					|| i->id == PPC_INS_QVFCTIWU || i->id == PPC_INS_QVFCTIWUZ);
			bool z = (i->id == PPC_INS_QVFCTIDZ || i->id == PPC_INS_QVFCTIDUZ
					|| i->id == PPC_INS_QVFCTIWZ || i->id == PPC_INS_QVFCTIWUZ);
			if (!z)
			{
				fp = fpIntrinsic(irb, llvm::Intrinsic::roundeven, {fp});
			}
			auto* ity = wide ? i64 : irb.getInt32Ty();
			out = generateFpToIntBounded(fp, ity, !uns, irb);
			out = uns ? irb.CreateZExtOrTrunc(out, i64) : irb.CreateSExtOrTrunc(out, i64);
			break;
		}
		default:
			return;
		}
		val = qpxPutSlot(val, out, lane, irb);
	}
	storeOpI256(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxFpRound(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	auto* bits = loadOpI256(pi->operands[1], irb);
	auto* i64 = irb.getInt64Ty();
	auto* i256 = irb.getIntNTy(256);
	auto* f64 = irb.getDoubleTy();
	auto* one = llvm::ConstantFP::get(f64, 1.0);
	llvm::Value* val = llvm::ConstantInt::get(i256, 0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		auto* src = irb.CreateBitCast(qpxGetSlot(bits, lane, irb), f64);
		llvm::Value* r = nullptr;
		switch (i->id)
		{
		case PPC_INS_QVFRE:
		case PPC_INS_QVFRES:
			r = irb.CreateFDiv(one, src);
			break;
		case PPC_INS_QVFRSQRTE:
		case PPC_INS_QVFRSQRTES:
			r = irb.CreateFDiv(one, fpIntrinsic(irb, llvm::Intrinsic::sqrt, {src}));
			break;
		case PPC_INS_QVFRIM:
			r = fpIntrinsic(irb, llvm::Intrinsic::floor, {src});
			break;
		case PPC_INS_QVFRIP:
			r = fpIntrinsic(irb, llvm::Intrinsic::ceil, {src});
			break;
		case PPC_INS_QVFRIZ:
			r = fpIntrinsic(irb, llvm::Intrinsic::trunc, {src});
			break;
		case PPC_INS_QVFRIN:
			r = fpIntrinsic(irb, llvm::Intrinsic::round, {src});
			break;
		case PPC_INS_QVFRSP:
			r = roundToSingle(irb, src);
			break;
		default:
			return;
		}
		if (isQpxSingle(i->id))
		{
			r = roundToSingle(irb, r);
		}
		val = qpxPutSlot(val, irb.CreateBitCast(r, i64), lane, irb);
	}
	storeOpI256(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxLogical(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, pi, irb, pi->op_count >= 2 && pi->op_count <= 4);

	auto* a = loadOpI256(pi->operands[1], irb);
	auto* b = (pi->op_count >= 3)
			? loadOpI256(pi->operands[2], irb)
			: llvm::ConstantInt::get(a->getType(), 0);
	unsigned tt = qpxLogicalTt(i, pi);
	auto* nota = irb.CreateNot(a);
	auto* notb = irb.CreateNot(b);
	llvm::Value* r = llvm::ConstantInt::get(a->getType(), 0);
	if (tt & 1u)
	{
		r = irb.CreateOr(r, irb.CreateAnd(a, b));
	}
	if (tt & 2u)
	{
		r = irb.CreateOr(r, irb.CreateAnd(nota, b));
	}
	if (tt & 4u)
	{
		r = irb.CreateOr(r, irb.CreateAnd(a, notb));
	}
	if (tt & 8u)
	{
		r = irb.CreateOr(r, irb.CreateAnd(nota, notb));
	}
	storeOpI256(pi->operands[0], r, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxAlign(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);
	(void)i;

	auto* a = loadOpI256(pi->operands[1], irb);
	auto* b = loadOpI256(pi->operands[2], irb);
	unsigned idx = 0;
	if (pi->operands[3].type == PPC_OP_IMM)
	{
		idx = static_cast<unsigned>(pi->operands[3].imm) & 3u;
	}
	auto* i256 = irb.getIntNTy(256);
	llvm::Value* val = llvm::ConstantInt::get(i256, 0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		unsigned src = idx + lane;
		auto* slot = (src < 4) ? qpxGetSlot(a, src, irb) : qpxGetSlot(b, src - 4, irb);
		val = qpxPutSlot(val, slot, lane, irb);
	}
	storeOpI256(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxSplat(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);
	(void)i;

	auto* src = loadOpI256(pi->operands[1], irb);
	unsigned idx = 0;
	if (pi->operands[2].type == PPC_OP_IMM)
	{
		idx = static_cast<unsigned>(pi->operands[2].imm) & 3u;
	}
	auto* slot = qpxGetSlot(src, idx, irb);
	auto* i256 = irb.getIntNTy(256);
	llvm::Value* val = llvm::ConstantInt::get(i256, 0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		val = qpxPutSlot(val, slot, lane, irb);
	}
	storeOpI256(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxPerm(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);
	(void)i;

	auto* a = loadOpI256(pi->operands[1], irb);
	auto* b = loadOpI256(pi->operands[2], irb);
	auto* c = loadOpI256(pi->operands[3], irb);
	llvm::Value* slots[8];
	for (unsigned k = 0; k < 4; ++k)
	{
		slots[k] = qpxGetSlot(a, k, irb);
		slots[k + 4] = qpxGetSlot(b, k, irb);
	}
	auto* i64 = irb.getInt64Ty();
	auto* i256 = irb.getIntNTy(256);
	llvm::Value* val = llvm::ConstantInt::get(i256, 0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		auto* idx = irb.CreateAnd(qpxGetSlot(c, lane, irb), llvm::ConstantInt::get(i64, 7));
		llvm::Value* pick = slots[0];
		for (unsigned k = 1; k < 8; ++k)
		{
			pick = irb.CreateSelect(
					irb.CreateICmpEQ(idx, llvm::ConstantInt::get(i64, k)),
					slots[k],
					pick);
		}
		val = qpxPutSlot(val, pick, lane, irb);
	}
	storeOpI256(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxGpci(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);
	(void)i;

	uint64_t imm = 0;
	if (pi->operands[1].type == PPC_OP_IMM)
	{
		imm = static_cast<uint64_t>(pi->operands[1].imm) & 0xfffu;
	}
	auto* i256 = irb.getIntNTy(256);
	llvm::Value* val = llvm::ConstantInt::get(i256, 0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		uint64_t idx = (imm >> (9u - 3u * lane)) & 7u;
		val = qpxPutSlot(val, irb.getInt64(idx), lane, irb);
	}
	storeOpI256(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxCrossFma(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	const bool mulOnly = (i->id == PPC_INS_QVFXMUL || i->id == PPC_INS_QVFXMULS);
	if (mulOnly)
	{
		EXPECT_IS_TERNARY(i, pi, irb);
	}
	else
	{
		EXPECT_IS_QUATERNARY(i, pi, irb);
	}

	auto* aBits = loadOpI256(pi->operands[1], irb);
	auto* cBits = loadOpI256(pi->operands[2], irb);
	const bool xx = (i->id == PPC_INS_QVFXXMADD || i->id == PPC_INS_QVFXXMADDS
			|| i->id == PPC_INS_QVFXXNPMADD || i->id == PPC_INS_QVFXXNPMADDS
			|| i->id == PPC_INS_QVFXXCPNMADD || i->id == PPC_INS_QVFXXCPNMADDS);
	const bool cpn = (i->id == PPC_INS_QVFXXCPNMADD || i->id == PPC_INS_QVFXXCPNMADDS);
	const bool negProd = (i->id == PPC_INS_QVFXXNPMADD || i->id == PPC_INS_QVFXXNPMADDS);
	if (xx)
	{
		aBits = qpxCrossI256(aBits, irb);
	}
	cBits = qpxCrossI256(cBits, irb);
	if (cpn)
	{
		cBits = qpxCpnI256(cBits, irb);
	}

	const bool sp = isQpxSingle(i->id);
	auto* a = qpxAsArithVec(aBits, sp, irb);
	auto* c = qpxAsArithVec(cBits, sp, irb);
	auto* prod = irb.CreateFMul(a, c);
	llvm::Value* r = prod;
	if (!mulOnly)
	{
		auto* b = qpxAsArithVec(loadOpI256(pi->operands[3], irb), sp, irb);
		r = negProd ? irb.CreateFSub(b, prod) : irb.CreateFAdd(prod, b);
	}
	storeOpI256(pi->operands[0], qpxFromArithVec(r, irb), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxLoadIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	enum class Kind { D4, S4, D2, S2, Iwa, Iwz, PclD, PclS, PcrD, PcrS };
	Kind kind = Kind::D4;
	switch (i->id)
	{
	case PPC_INS_QVLFDX:
	case PPC_INS_QVLFDUX:
	case PPC_INS_QVLFDXA:
	case PPC_INS_QVLFDUXA:
		kind = Kind::D4;
		break;
	case PPC_INS_QVLFSX:
	case PPC_INS_QVLFSUX:
	case PPC_INS_QVLFSXA:
	case PPC_INS_QVLFSUXA:
		kind = Kind::S4;
		break;
	case PPC_INS_QVLFCDX:
	case PPC_INS_QVLFCDUX:
	case PPC_INS_QVLFCDXA:
	case PPC_INS_QVLFCDUXA:
		kind = Kind::D2;
		break;
	case PPC_INS_QVLFCSX:
	case PPC_INS_QVLFCSUX:
	case PPC_INS_QVLFCSXA:
	case PPC_INS_QVLFCSUXA:
		kind = Kind::S2;
		break;
	case PPC_INS_QVLFIWAX:
	case PPC_INS_QVLFIWAXA:
		kind = Kind::Iwa;
		break;
	case PPC_INS_QVLFIWZX:
	case PPC_INS_QVLFIWZXA:
		kind = Kind::Iwz;
		break;
	case PPC_INS_QVLPCLDX:
		kind = Kind::PclD;
		break;
	case PPC_INS_QVLPCLSX:
		kind = Kind::PclS;
		break;
	case PPC_INS_QVLPCRDX:
		kind = Kind::PcrD;
		break;
	case PPC_INS_QVLPCRSX:
		kind = Kind::PcrS;
		break;
	default:
		return;
	}

	auto* add = loadIndexedEffectiveAddress(pi, irb);
	auto* i64 = irb.getInt64Ty();
	auto* i256 = irb.getIntNTy(256);
	llvm::Value* val = llvm::ConstantInt::get(i256, 0);

	if (kind == Kind::PclD || kind == Kind::PclS || kind == Kind::PcrD || kind == Kind::PcrS)
	{
		unsigned log2 = (kind == Kind::PclS || kind == Kind::PcrS) ? 2u : 3u;
		auto* align = irb.CreateAnd(
				irb.CreateLShr(add, llvm::ConstantInt::get(add->getType(), log2)),
				llvm::ConstantInt::get(add->getType(), 3));
		if (kind == Kind::PcrD || kind == Kind::PcrS)
		{
			align = irb.CreateSub(llvm::ConstantInt::get(add->getType(), 4), align);
		}
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			auto* idx = irb.CreateAdd(align, llvm::ConstantInt::get(add->getType(), lane));
			val = qpxPutSlot(val, irb.CreateZExtOrTrunc(idx, i64), lane, irb);
		}
	}
	else
	{
		const bool two = (kind == Kind::D2 || kind == Kind::S2);
		const unsigned n = two ? 2u : 4u;
		for (unsigned lane = 0; lane < n; ++lane)
		{
			llvm::Value* slot = nullptr;
			if (kind == Kind::S4 || kind == Kind::S2)
			{
				auto* addr = irb.CreateAdd(add, llvm::ConstantInt::get(add->getType(), static_cast<uint64_t>(lane) * 4u));
				auto* bits32 = loadIntPtr(irb, addr, irb.getInt32Ty());
				auto* f = irb.CreateBitCast(bits32, irb.getFloatTy());
				slot = irb.CreateBitCast(irb.CreateFPExt(f, irb.getDoubleTy()), i64);
			}
			else if (kind == Kind::Iwa || kind == Kind::Iwz)
			{
				auto* addr = irb.CreateAdd(add, llvm::ConstantInt::get(add->getType(), static_cast<uint64_t>(lane) * 4u));
				auto* w = loadIntPtr(irb, addr, irb.getInt32Ty());
				auto* fp = (kind == Kind::Iwa)
						? irb.CreateSIToFP(w, irb.getDoubleTy())
						: irb.CreateUIToFP(w, irb.getDoubleTy());
				slot = irb.CreateBitCast(fp, i64);
			}
			else
			{
				auto* addr = irb.CreateAdd(add, llvm::ConstantInt::get(add->getType(), static_cast<uint64_t>(lane) * 8u));
				slot = loadIntPtr(irb, addr, i64);
			}
			val = qpxPutSlot(val, slot, lane, irb);
		}
	}
	storeOpI256(pi->operands[0], val, irb);
	qpxWritebackIndexed(i, pi, add, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateQpxStoreIndexed(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	enum class Kind { D4, S4, D2, S2, Iwx };
	Kind kind = Kind::D4;
	switch (i->id)
	{
	case PPC_INS_QVSTFDX:
	case PPC_INS_QVSTFDUX:
	case PPC_INS_QVSTFDXA:
	case PPC_INS_QVSTFDUXA:
	case PPC_INS_QVSTFDXI:
	case PPC_INS_QVSTFDXIA:
	case PPC_INS_QVSTFDUXI:
	case PPC_INS_QVSTFDUXIA:
		kind = Kind::D4;
		break;
	case PPC_INS_QVSTFSX:
	case PPC_INS_QVSTFSUX:
	case PPC_INS_QVSTFSXA:
	case PPC_INS_QVSTFSUXA:
	case PPC_INS_QVSTFSXI:
	case PPC_INS_QVSTFSXIA:
	case PPC_INS_QVSTFSUXI:
	case PPC_INS_QVSTFSUXIA:
		kind = Kind::S4;
		break;
	case PPC_INS_QVSTFCDX:
	case PPC_INS_QVSTFCDUX:
	case PPC_INS_QVSTFCDXA:
	case PPC_INS_QVSTFCDUXA:
	case PPC_INS_QVSTFCDXI:
	case PPC_INS_QVSTFCDXIA:
	case PPC_INS_QVSTFCDUXI:
	case PPC_INS_QVSTFCDUXIA:
		kind = Kind::D2;
		break;
	case PPC_INS_QVSTFCSX:
	case PPC_INS_QVSTFCSUX:
	case PPC_INS_QVSTFCSXA:
	case PPC_INS_QVSTFCSUXA:
	case PPC_INS_QVSTFCSXI:
	case PPC_INS_QVSTFCSXIA:
	case PPC_INS_QVSTFCSUXI:
	case PPC_INS_QVSTFCSUXIA:
		kind = Kind::S2;
		break;
	case PPC_INS_QVSTFIWX:
	case PPC_INS_QVSTFIWXA:
		kind = Kind::Iwx;
		break;
	default:
		return;
	}

	auto* val = loadOpI256(pi->operands[0], irb);
	auto* add = loadIndexedEffectiveAddress(pi, irb);
	auto* i64 = irb.getInt64Ty();
	const unsigned n = (kind == Kind::D2 || kind == Kind::S2) ? 2u : 4u;
	for (unsigned lane = 0; lane < n; ++lane)
	{
		auto* slot = qpxGetSlot(val, lane, irb);
		if (kind == Kind::S4 || kind == Kind::S2)
		{
			auto* addr = irb.CreateAdd(add, llvm::ConstantInt::get(add->getType(), static_cast<uint64_t>(lane) * 4u));
			auto* f = irb.CreateFPTrunc(irb.CreateBitCast(slot, irb.getDoubleTy()), irb.getFloatTy());
			storeIntPtr(irb, irb.CreateBitCast(f, irb.getInt32Ty()), addr, irb.getInt32Ty());
		}
		else if (kind == Kind::Iwx)
		{
			auto* addr = irb.CreateAdd(add, llvm::ConstantInt::get(add->getType(), static_cast<uint64_t>(lane) * 4u));
			storeIntPtr(irb, irb.CreateTrunc(slot, irb.getInt32Ty()), addr, irb.getInt32Ty());
		}
		else
		{
			auto* addr = irb.CreateAdd(add, llvm::ConstantInt::get(add->getType(), static_cast<uint64_t>(lane) * 8u));
			storeIntPtr(irb, slot, addr, i64);
		}
	}
	qpxWritebackIndexed(i, pi, add, irb);
}

//
//==============================================================================
// SPE (e500) EV* — 64-bit GPR overlay S0..S31 (rN:rhN).
//==============================================================================
//

namespace {

llvm::Value* spePackWords(llvm::IRBuilder<>& irb, llvm::Value* hi, llvm::Value* lo)
{
	auto* i64 = irb.getInt64Ty();
	return irb.CreateOr(
			irb.CreateShl(irb.CreateZExt(hi, i64), 32),
			irb.CreateZExt(lo, i64));
}

llvm::Value* speHiWord(llvm::IRBuilder<>& irb, llvm::Value* v)
{
	return irb.CreateTrunc(irb.CreateLShr(v, 32), irb.getInt32Ty());
}

llvm::Value* speLoWord(llvm::IRBuilder<>& irb, llvm::Value* v)
{
	return irb.CreateTrunc(v, irb.getInt32Ty());
}

llvm::Value* speSplatWord(llvm::IRBuilder<>& irb, uint32_t w)
{
	return irb.getInt64((static_cast<uint64_t>(w) << 32) | w);
}

int64_t speSimm5(int64_t imm)
{
	return static_cast<int64_t>(static_cast<int32_t>(imm << 27) >> 27);
}

llvm::Value* speLaneAbs(llvm::IRBuilder<>& irb, llvm::Value* w)
{
	auto* zero = irb.getInt32(0);
	auto* neg = irb.CreateSub(zero, w);
	return irb.CreateSelect(irb.CreateICmpSLT(w, zero), neg, w);
}

llvm::Value* speShiftAmt(llvm::IRBuilder<>& irb, llvm::Value* w)
{
	return irb.CreateAnd(w, irb.getInt32(31));
}

} // namespace

uint32_t Capstone2LlvmIrTranslatorPowerpc_impl::speOverlayReg(uint32_t r) const
{
	if (r >= PPC_REG_R0 && r <= PPC_REG_R31)
	{
		return PPC_REG_S0 + (r - PPC_REG_R0);
	}
	if (r >= PPC_REG_X0 && r <= PPC_REG_X31)
	{
		return PPC_REG_S0 + (r - PPC_REG_X0);
	}
	return r;
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::loadSpeI64(
		cs_ppc_op& op, llvm::IRBuilder<>& irb)
{
	auto* i64 = irb.getInt64Ty();
	if (op.type == PPC_OP_IMM)
	{
		return speSplatWord(irb, static_cast<uint32_t>(op.imm));
	}
	if (op.type != PPC_OP_REG)
	{
		return llvm::ConstantInt::get(i64, 0);
	}
	uint32_t s = speOverlayReg(op.reg);
	if (s >= PPC_REG_S0 && s <= PPC_REG_S31)
	{
		return loadRegister(s, irb);
	}
	auto* v = loadRegister(op.reg, irb);
	return irb.CreateZExtOrTrunc(v, i64);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::storeSpeI64(
		cs_ppc_op& op, llvm::Value* val, llvm::IRBuilder<>& irb)
{
	if (op.type != PPC_OP_REG)
	{
		return;
	}
	val = irb.CreateZExtOrTrunc(val, irb.getInt64Ty());
	uint32_t s = speOverlayReg(op.reg);
	if (s >= PPC_REG_S0 && s <= PPC_REG_S31)
	{
		storeRegister(s, val, irb);
	}
	uint32_t gpr = PPC_REG_INVALID;
	if (op.reg >= PPC_REG_R0 && op.reg <= PPC_REG_R31)
	{
		gpr = op.reg;
	}
	else if (op.reg >= PPC_REG_S0 && op.reg <= PPC_REG_S31)
	{
		gpr = PPC_REG_R0 + (op.reg - PPC_REG_S0);
	}
	else if (op.reg >= PPC_REG_X0 && op.reg <= PPC_REG_X31)
	{
		gpr = PPC_REG_R0 + (op.reg - PPC_REG_X0);
	}
	if (gpr != PPC_REG_INVALID)
	{
		storeRegister(gpr, irb.CreateTrunc(val, irb.getInt32Ty()), irb);
	}
}

llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::speEffectiveAddress(
		cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	return loadIndexedEffectiveAddress(pi, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeArith(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		return;
	}

	llvm::Value* a = nullptr;
	llvm::Value* b = nullptr;
	if (i->id == PPC_INS_EVADDIW || i->id == PPC_INS_EVSUBIFW)
	{
		cs_ppc_op* regOp = nullptr;
		int64_t imm = 0;
		bool haveImm = false;
		for (unsigned n = 1; n < pi->op_count; ++n)
		{
			if (pi->operands[n].type == PPC_OP_REG && regOp == nullptr)
			{
				regOp = &pi->operands[n];
			}
			else if (pi->operands[n].type == PPC_OP_IMM)
			{
				imm = pi->operands[n].imm;
				haveImm = true;
			}
		}
		if (regOp == nullptr)
		{
			return;
		}
		auto* r = loadSpeI64(*regOp, irb);
		auto* s = haveImm ? speSplatWord(irb, static_cast<uint32_t>(imm))
				: llvm::ConstantInt::get(irb.getInt64Ty(), 0);
		if (i->id == PPC_INS_EVSUBIFW)
		{
			a = s;
			b = r;
		}
		else
		{
			a = r;
			b = s;
		}
	}
	else
	{
		if (pi->op_count < 3)
		{
			return;
		}
		a = loadSpeI64(pi->operands[1], irb);
		b = loadSpeI64(pi->operands[2], irb);
	}

	llvm::Value* hi = nullptr;
	llvm::Value* lo = nullptr;
	if (i->id == PPC_INS_EVSUBFW)
	{
		hi = irb.CreateSub(speHiWord(irb, b), speHiWord(irb, a));
		lo = irb.CreateSub(speLoWord(irb, b), speLoWord(irb, a));
	}
	else
	{
		hi = irb.CreateAdd(speHiWord(irb, a), speHiWord(irb, b));
		lo = irb.CreateAdd(speLoWord(irb, a), speLoWord(irb, b));
		if (i->id == PPC_INS_EVSUBIFW)
		{
			hi = irb.CreateSub(speHiWord(irb, a), speHiWord(irb, b));
			lo = irb.CreateSub(speLoWord(irb, a), speLoWord(irb, b));
		}
	}
	storeSpeI64(pi->operands[0], spePackWords(irb, hi, lo), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeBrinc(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 3)
	{
		return;
	}

	auto* ra = speLoWord(irb, loadSpeI64(pi->operands[1], irb));
	auto* rb = speLoWord(irb, loadSpeI64(pi->operands[2], irb));
	auto* nrb = irb.CreateNot(rb);
	auto* tmp = irb.CreateAdd(irb.CreateOr(ra, nrb), irb.getInt32(1));
	auto* rd = irb.CreateOr(irb.CreateAnd(tmp, rb), irb.CreateAnd(ra, nrb));
	storeSpeI64(pi->operands[0], irb.CreateZExt(rd, irb.getInt64Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeLogical(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 3)
	{
		return;
	}
	auto* a = loadSpeI64(pi->operands[1], irb);
	auto* b = loadSpeI64(pi->operands[2], irb);
	llvm::Value* r = nullptr;
	switch (i->id)
	{
	case PPC_INS_EVAND:
		r = irb.CreateAnd(a, b);
		break;
	case PPC_INS_EVANDC:
		r = irb.CreateAnd(a, irb.CreateNot(b));
		break;
	case PPC_INS_EVOR:
		r = irb.CreateOr(a, b);
		break;
	case PPC_INS_EVORC:
		r = irb.CreateOr(a, irb.CreateNot(b));
		break;
	case PPC_INS_EVXOR:
		r = irb.CreateXor(a, b);
		break;
	case PPC_INS_EVNOR:
		r = irb.CreateNot(irb.CreateOr(a, b));
		break;
	case PPC_INS_EVEQV:
		r = irb.CreateNot(irb.CreateXor(a, b));
		break;
	case PPC_INS_EVNAND:
		r = irb.CreateNot(irb.CreateAnd(a, b));
		break;
	default:
		return;
	}
	storeSpeI64(pi->operands[0], r, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeUnary(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		return;
	}
	auto* v = loadSpeI64(pi->operands[1], irb);
	auto* hi = speHiWord(irb, v);
	auto* lo = speLoWord(irb, v);
	auto* i32 = irb.getInt32Ty();
	auto* ctlz = llvm::Intrinsic::getOrInsertDeclaration(
			_module, llvm::Intrinsic::ctlz, i32);

	llvm::Value* rhi = hi;
	llvm::Value* rlo = lo;
	switch (i->id)
	{
	case PPC_INS_EVABS:
		rhi = speLaneAbs(irb, hi);
		rlo = speLaneAbs(irb, lo);
		break;
	case PPC_INS_EVNEG:
		rhi = irb.CreateSub(irb.getInt32(0), hi);
		rlo = irb.CreateSub(irb.getInt32(0), lo);
		break;
	case PPC_INS_EVEXTSB:
		rhi = irb.CreateSExt(irb.CreateTrunc(hi, irb.getInt8Ty()), i32);
		rlo = irb.CreateSExt(irb.CreateTrunc(lo, irb.getInt8Ty()), i32);
		break;
	case PPC_INS_EVEXTSH:
		rhi = irb.CreateSExt(irb.CreateTrunc(hi, irb.getInt16Ty()), i32);
		rlo = irb.CreateSExt(irb.CreateTrunc(lo, irb.getInt16Ty()), i32);
		break;
	case PPC_INS_EVCNTLZW:
		rhi = irb.CreateCall(ctlz, {hi, irb.getFalse()});
		rlo = irb.CreateCall(ctlz, {lo, irb.getFalse()});
		break;
	case PPC_INS_EVCNTLSW:
		rhi = irb.CreateCall(ctlz, {irb.CreateXor(hi, irb.CreateAShr(hi, 31)), irb.getFalse()});
		rlo = irb.CreateCall(ctlz, {irb.CreateXor(lo, irb.CreateAShr(lo, 31)), irb.getFalse()});
		break;
	case PPC_INS_EVRNDW:
	{
		auto* add = irb.getInt32(0x10);
		auto* clr = irb.getInt32(0xffffffe0u);
		rhi = irb.CreateAnd(irb.CreateAdd(hi, add), clr);
		rlo = irb.CreateAnd(irb.CreateAdd(lo, add), clr);
		break;
	}
	default:
		return;
	}
	storeSpeI64(pi->operands[0], spePackWords(irb, rhi, rlo), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeCmp(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		return;
	}
	uint32_t cr = PPC_REG_CR0;
	unsigned ai = 0;
	unsigned bi = 1;
	if (pi->operands[0].type == PPC_OP_IMM)
	{
		int64_t f = pi->operands[0].imm;
		if (f >= 0 && f <= 7)
		{
			cr = PPC_REG_CR0 + static_cast<uint32_t>(f);
		}
		ai = 1;
		bi = 2;
	}
	else if (isCrRegister(pi->operands[0]))
	{
		cr = pi->operands[0].reg;
		ai = 1;
		bi = 2;
	}
	if (bi >= pi->op_count)
	{
		return;
	}

	auto* a = loadSpeI64(pi->operands[ai], irb);
	auto* b = loadSpeI64(pi->operands[bi], irb);
	auto* ahi = speHiWord(irb, a);
	auto* alo = speLoWord(irb, a);
	auto* bhi = speHiWord(irb, b);
	auto* blo = speLoWord(irb, b);

	llvm::Value* chi = nullptr;
	llvm::Value* clo = nullptr;
	switch (i->id)
	{
	case PPC_INS_EVCMPEQ:
		chi = irb.CreateICmpEQ(ahi, bhi);
		clo = irb.CreateICmpEQ(alo, blo);
		break;
	case PPC_INS_EVCMPGTS:
		chi = irb.CreateICmpSGT(ahi, bhi);
		clo = irb.CreateICmpSGT(alo, blo);
		break;
	case PPC_INS_EVCMPGTU:
		chi = irb.CreateICmpUGT(ahi, bhi);
		clo = irb.CreateICmpUGT(alo, blo);
		break;
	case PPC_INS_EVCMPLTS:
		chi = irb.CreateICmpSLT(ahi, bhi);
		clo = irb.CreateICmpSLT(alo, blo);
		break;
	case PPC_INS_EVCMPLTU:
		chi = irb.CreateICmpULT(ahi, bhi);
		clo = irb.CreateICmpULT(alo, blo);
		break;
	default:
		return;
	}

	auto* lt = irb.CreateAnd(chi, clo);
	auto* gt = irb.CreateOr(chi, clo);
	auto* eq = irb.CreateICmpEQ(chi, clo);
	auto* so = irb.getInt1(false);

	uint32_t ltR = PPC_REG_CR0LT;
	uint32_t gtR = PPC_REG_CR0GT;
	uint32_t eqR = PPC_REG_CR0EQ;
	uint32_t soR = PPC_REG_CR0UN;
	crFieldRegisters(cr, ltR, gtR, eqR, soR);
	storeRegister(ltR, lt, irb);
	storeRegister(gtR, gt, irb);
	storeRegister(eqR, eq, irb);
	storeRegister(soR, so, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeShift(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		return;
	}
	auto* a = loadSpeI64(pi->operands[1], irb);
	auto* ahi = speHiWord(irb, a);
	auto* alo = speLoWord(irb, a);

	llvm::Value* shi = nullptr;
	llvm::Value* slo = nullptr;
	bool immForm = i->id == PPC_INS_EVSLWI || i->id == PPC_INS_EVSRWIS
			|| i->id == PPC_INS_EVSRWIU || i->id == PPC_INS_EVRLWI;
	if (immForm)
	{
		int64_t imm = 0;
		if (pi->op_count >= 3 && pi->operands[2].type == PPC_OP_IMM)
		{
			imm = pi->operands[2].imm;
		}
		else if (pi->operands[1].type == PPC_OP_IMM)
		{
			imm = pi->operands[1].imm;
			a = loadSpeI64(pi->operands[2], irb);
			ahi = speHiWord(irb, a);
			alo = speLoWord(irb, a);
		}
		shi = slo = irb.getInt32(static_cast<uint32_t>(imm) & 31u);
	}
	else
	{
		if (pi->op_count < 3)
		{
			return;
		}
		auto* b = loadSpeI64(pi->operands[2], irb);
		shi = speShiftAmt(irb, speHiWord(irb, b));
		slo = speShiftAmt(irb, speLoWord(irb, b));
	}

	auto rotl = [&](llvm::Value* w, llvm::Value* amt) {
		auto* shl = irb.CreateShl(w, amt);
		auto* inv = irb.CreateAnd(
				irb.CreateSub(irb.getInt32(0), amt),
				irb.getInt32(31));
		return irb.CreateOr(shl, irb.CreateLShr(w, inv));
	};
	llvm::Value* rhi = nullptr;
	llvm::Value* rlo = nullptr;
	switch (i->id)
	{
	case PPC_INS_EVSLW:
	case PPC_INS_EVSLWI:
		rhi = irb.CreateShl(ahi, shi);
		rlo = irb.CreateShl(alo, slo);
		break;
	case PPC_INS_EVSRWU:
	case PPC_INS_EVSRWIU:
		rhi = irb.CreateLShr(ahi, shi);
		rlo = irb.CreateLShr(alo, slo);
		break;
	case PPC_INS_EVSRWS:
	case PPC_INS_EVSRWIS:
		rhi = irb.CreateAShr(ahi, shi);
		rlo = irb.CreateAShr(alo, slo);
		break;
	case PPC_INS_EVRLW:
	case PPC_INS_EVRLWI:
		rhi = rotl(ahi, shi);
		rlo = rotl(alo, slo);
		break;
	default:
		return;
	}
	storeSpeI64(pi->operands[0], spePackWords(irb, rhi, rlo), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeSplat(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	int64_t imm = 0;
	for (unsigned n = 0; n < pi->op_count; ++n)
	{
		if (pi->operands[n].type == PPC_OP_IMM)
		{
			imm = pi->operands[n].imm;
			break;
		}
	}
	int32_t simm = static_cast<int32_t>(speSimm5(imm));
	uint32_t w = static_cast<uint32_t>(simm);
	if (i->id == PPC_INS_EVSPLATFI)
	{
		w = static_cast<uint32_t>(simm) << 27;
	}
	storeSpeI64(pi->operands[0], speSplatWord(irb, w), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeMerge(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 3)
	{
		return;
	}
	auto* a = loadSpeI64(pi->operands[1], irb);
	auto* b = loadSpeI64(pi->operands[2], irb);
	auto* ahi = speHiWord(irb, a);
	auto* alo = speLoWord(irb, a);
	auto* bhi = speHiWord(irb, b);
	auto* blo = speLoWord(irb, b);
	llvm::Value* rhi = ahi;
	llvm::Value* rlo = blo;
	switch (i->id)
	{
	case PPC_INS_EVMERGEHI:
		rhi = ahi;
		rlo = bhi;
		break;
	case PPC_INS_EVMERGELO:
		rhi = alo;
		rlo = blo;
		break;
	case PPC_INS_EVMERGEHILO:
		rhi = ahi;
		rlo = blo;
		break;
	case PPC_INS_EVMERGELOHI:
		rhi = alo;
		rlo = bhi;
		break;
	default:
		return;
	}
	storeSpeI64(pi->operands[0], spePackWords(irb, rhi, rlo), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeLoad(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		return;
	}
	auto* ea = speEffectiveAddress(pi, irb);
	llvm::Value* val = nullptr;
	switch (i->id)
	{
	case PPC_INS_EVLDD:
	case PPC_INS_EVLDDX:
	case PPC_INS_EVLDW:
	case PPC_INS_EVLDWX:
	case PPC_INS_EVLDH:
	case PPC_INS_EVLDHX:
		val = loadIntPtr(irb, ea, irb.getInt64Ty());
		break;
	case PPC_INS_EVLWHE:
	case PPC_INS_EVLWHEX:
	{
		auto* w = loadIntPtr(irb, ea, irb.getInt32Ty());
		auto* h0 = irb.CreateLShr(w, 16);
		auto* h1 = irb.CreateAnd(w, irb.getInt32(0xffff));
		val = spePackWords(irb, irb.CreateShl(h0, 16), irb.CreateShl(h1, 16));
		break;
	}
	case PPC_INS_EVLWHOU:
	case PPC_INS_EVLWHOUX:
	{
		auto* w = loadIntPtr(irb, ea, irb.getInt32Ty());
		auto* h0 = irb.CreateLShr(w, 16);
		auto* h1 = irb.CreateAnd(w, irb.getInt32(0xffff));
		val = spePackWords(irb, h0, h1);
		break;
	}
	case PPC_INS_EVLHHESPLAT:
	case PPC_INS_EVLHHESPLATX:
	{
		auto* h = irb.CreateZExt(loadIntPtr(irb, ea, irb.getInt16Ty()), irb.getInt32Ty());
		auto* even = irb.CreateShl(h, 16);
		val = spePackWords(irb, even, even);
		break;
	}
	case PPC_INS_EVLHHOSSPLAT:
	case PPC_INS_EVLHHOSSPLATX:
	{
		auto* h = irb.CreateSExt(loadIntPtr(irb, ea, irb.getInt16Ty()), irb.getInt32Ty());
		val = spePackWords(irb, h, h);
		break;
	}
	case PPC_INS_EVLHHOUSPLAT:
	case PPC_INS_EVLHHOUSPLATX:
	{
		auto* h = irb.CreateZExt(loadIntPtr(irb, ea, irb.getInt16Ty()), irb.getInt32Ty());
		val = spePackWords(irb, h, h);
		break;
	}
	case PPC_INS_EVLWHOS:
	case PPC_INS_EVLWHOSX:
	{
		auto* w = loadIntPtr(irb, ea, irb.getInt32Ty());
		auto* h0 = irb.CreateAShr(w, 16);
		auto* h1 = irb.CreateSExt(irb.CreateTrunc(w, irb.getInt16Ty()), irb.getInt32Ty());
		val = spePackWords(irb, h0, h1);
		break;
	}
	case PPC_INS_EVLWHSPLAT:
	case PPC_INS_EVLWHSPLATX:
	{
		auto* w = loadIntPtr(irb, ea, irb.getInt32Ty());
		auto* h0 = irb.CreateAnd(irb.CreateLShr(w, 16), irb.getInt32(0xffff));
		auto* h1 = irb.CreateAnd(w, irb.getInt32(0xffff));
		auto splat = [&](llvm::Value* h) {
			return irb.CreateOr(irb.CreateShl(h, 16), h);
		};
		val = spePackWords(irb, splat(h0), splat(h1));
		break;
	}
	case PPC_INS_EVLWWSPLAT:
	case PPC_INS_EVLWWSPLATX:
	{
		auto* w = loadIntPtr(irb, ea, irb.getInt32Ty());
		val = spePackWords(irb, w, w);
		break;
	}
	default:
		return;
	}
	storeSpeI64(pi->operands[0], val, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeStore(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		return;
	}
	auto* val = loadSpeI64(pi->operands[0], irb);
	auto* ea = speEffectiveAddress(pi, irb);
	switch (i->id)
	{
	case PPC_INS_EVSTDD:
	case PPC_INS_EVSTDDX:
	case PPC_INS_EVSTDW:
	case PPC_INS_EVSTDWX:
	case PPC_INS_EVSTDH:
	case PPC_INS_EVSTDHX:
		storeIntPtr(irb, val, ea, irb.getInt64Ty());
		break;
	case PPC_INS_EVSTWHE:
	case PPC_INS_EVSTWHEX:
	{
		auto* hi = irb.CreateLShr(speHiWord(irb, val), 16);
		auto* lo = irb.CreateLShr(speLoWord(irb, val), 16);
		auto* w = irb.CreateOr(irb.CreateShl(hi, 16), lo);
		storeIntPtr(irb, w, ea, irb.getInt32Ty());
		break;
	}
	case PPC_INS_EVSTWHO:
	case PPC_INS_EVSTWHOX:
	{
		auto* hi = irb.CreateAnd(speHiWord(irb, val), irb.getInt32(0xffff));
		auto* lo = irb.CreateAnd(speLoWord(irb, val), irb.getInt32(0xffff));
		auto* w = irb.CreateOr(irb.CreateShl(hi, 16), lo);
		storeIntPtr(irb, w, ea, irb.getInt32Ty());
		break;
	}
	case PPC_INS_EVSTWWE:
	case PPC_INS_EVSTWWEX:
		storeIntPtr(irb, speHiWord(irb, val), ea, irb.getInt32Ty());
		break;
	case PPC_INS_EVSTWWO:
	case PPC_INS_EVSTWWOX:
		storeIntPtr(irb, speLoWord(irb, val), ea, irb.getInt32Ty());
		break;
	default:
		return;
	}
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeDiv(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 3)
	{
		return;
	}
	auto* a = loadSpeI64(pi->operands[1], irb);
	auto* b = loadSpeI64(pi->operands[2], irb);
	auto* ahi = speHiWord(irb, a);
	auto* alo = speLoWord(irb, a);
	auto* bhi = speHiWord(irb, b);
	auto* blo = speLoWord(irb, b);
	auto* zero = irb.getInt32(0);
	auto laneDiv = [&](llvm::Value* n, llvm::Value* d) {
		auto* safe = irb.CreateSelect(irb.CreateICmpEQ(d, zero), irb.getInt32(1), d);
		llvm::Value* q = (i->id == PPC_INS_EVDIVWU)
				? irb.CreateUDiv(n, safe)
				: irb.CreateSDiv(n, safe);
		if (i->id == PPC_INS_EVDIVWS)
		{
			auto* ov = irb.CreateAnd(
					irb.CreateICmpEQ(n, irb.getInt32(0x80000000u)),
					irb.CreateICmpEQ(d, irb.getInt32(0xffffffffu)));
			q = irb.CreateSelect(ov, n, q);
		}
		return irb.CreateSelect(irb.CreateICmpEQ(d, zero), zero, q);
	};
	storeSpeI64(pi->operands[0], spePackWords(irb, laneDiv(ahi, bhi), laneDiv(alo, blo)), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeFscr(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 1 || pi->operands[0].type != PPC_OP_REG)
	{
		return;
	}
	const bool mt = i->id == PPC_INS_MTSPEFSCR
			|| (i->is_alias && static_cast<unsigned>(i->alias_id) == PPC_INS_ALIAS_MTSPEFSCR)
			|| i->id == PPC_INS_ALIAS_MTSPEFSCR;
	if (mt)
	{
		auto* v = loadRegister(pi->operands[0].reg, irb);
		storeRegister(PPC_REG_SPEFSCR, irb.CreateZExtOrTrunc(v, irb.getInt32Ty()), irb);
	}
	else
	{
		auto* v = loadRegister(PPC_REG_SPEFSCR, irb);
		storeRegister(pi->operands[0].reg, v, irb);
	}
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeMac(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* i64 = irb.getInt64Ty();
	std::string name = std::string("ppc.spe.") + (i->mnemonic ? i->mnemonic : "mac");
	std::vector<llvm::Type*> tys;
	std::vector<llvm::Value*> args;
	for (unsigned n = 1; n < pi->op_count; ++n)
	{
		auto& op = pi->operands[n];
		if (op.type == PPC_OP_REG)
		{
			args.push_back(loadSpeI64(op, irb));
			tys.push_back(i64);
		}
		else if (op.type == PPC_OP_IMM)
		{
			args.push_back(llvm::ConstantInt::get(i64, static_cast<uint64_t>(op.imm)));
			tys.push_back(i64);
		}
	}
	auto* f = getPseudoAsmFunction(i, llvm::FunctionType::get(i64, tys, false), name);
	llvm::Value* c = args.empty() ? irb.CreateCall(f) : irb.CreateCall(f, args);
	if (pi->op_count >= 1 && pi->operands[0].type == PPC_OP_REG)
	{
		storeSpeI64(pi->operands[0], c, irb);
	}
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateIsel(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	auto* ifTrue = loadOp(pi->operands[1], irb);
	auto* ifFalse = loadOp(pi->operands[2], irb);
	llvm::Value* cond = loadRegister(PPC_REG_CR0LT, irb);

	unsigned aid = i->is_alias ? static_cast<unsigned>(i->alias_id) : i->id;
	switch (aid)
	{
	case PPC_INS_ALIAS_ISELLT:
		cond = loadRegister(PPC_REG_CR0LT, irb);
		break;
	case PPC_INS_ALIAS_ISELGT:
		cond = loadRegister(PPC_REG_CR0GT, irb);
		break;
	case PPC_INS_ALIAS_ISELEQ:
		cond = loadRegister(PPC_REG_CR0EQ, irb);
		break;
	default:
		break;
	}

	if (pi->op_count >= 4)
	{
		cs_ppc_op& cOp = pi->operands[3];
		if (cOp.type == PPC_OP_REG)
		{
			if (isCrBitRegister(cOp.reg))
			{
				cond = loadRegister(cOp.reg, irb);
			}
			else if (isCrRegister(cOp.reg))
			{
				cond = loadCrX(irb, cOp.reg, PPC_CR_LT);
			}
		}
		else if (cOp.type == PPC_OP_IMM)
		{
			unsigned bi = static_cast<unsigned>(cOp.imm) & 31u;
			static const uint32_t kBits[4][8] = {
				{PPC_REG_CR0LT, PPC_REG_CR1LT, PPC_REG_CR2LT, PPC_REG_CR3LT,
				 PPC_REG_CR4LT, PPC_REG_CR5LT, PPC_REG_CR6LT, PPC_REG_CR7LT},
				{PPC_REG_CR0GT, PPC_REG_CR1GT, PPC_REG_CR2GT, PPC_REG_CR3GT,
				 PPC_REG_CR4GT, PPC_REG_CR5GT, PPC_REG_CR6GT, PPC_REG_CR7GT},
				{PPC_REG_CR0EQ, PPC_REG_CR1EQ, PPC_REG_CR2EQ, PPC_REG_CR3EQ,
				 PPC_REG_CR4EQ, PPC_REG_CR5EQ, PPC_REG_CR6EQ, PPC_REG_CR7EQ},
				{PPC_REG_CR0UN, PPC_REG_CR1UN, PPC_REG_CR2UN, PPC_REG_CR3UN,
				 PPC_REG_CR4UN, PPC_REG_CR5UN, PPC_REG_CR6UN, PPC_REG_CR7UN},
			};
			cond = loadRegister(kBits[bi % 4][bi / 4], irb);
		}
	}

	if (!cond->getType()->isIntegerTy(1))
	{
		cond = irb.CreateICmpNE(cond, llvm::Constant::getNullValue(cond->getType()));
	}
	if (ifTrue->getType() != ifFalse->getType())
	{
		ifFalse = irb.CreateZExtOrTrunc(ifFalse, ifTrue->getType());
	}
	storeOp(pi->operands[0], irb.CreateSelect(cond, ifTrue, ifFalse), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateCmpb(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	auto* ty = getArchBitSize() == 64 ? irb.getInt64Ty() : irb.getInt32Ty();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	op2 = irb.CreateZExtOrTrunc(op2, ty);
	unsigned bytes = ty->getIntegerBitWidth() / 8;
	auto* vecTy = llvm::FixedVectorType::get(irb.getInt8Ty(), bytes);
	auto* eq = irb.CreateICmpEQ(irb.CreateBitCast(op1, vecTy), irb.CreateBitCast(op2, vecTy));
	storeOp(pi->operands[0], irb.CreateBitCast(irb.CreateSExt(eq, vecTy), ty), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateDivde(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(pi, irb);
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	op1 = irb.CreateZExtOrTrunc(op1, i64);
	op2 = irb.CreateZExtOrTrunc(op2, i64);
	const bool isSigned = (i->id == PPC_INS_DIVDE);
	if (isSigned)
	{
		auto* overflow = irb.CreateAnd(
				irb.CreateICmpEQ(op1, llvm::ConstantInt::get(i64, llvm::APInt::getSignedMinValue(64))),
				irb.CreateICmpEQ(op2, llvm::ConstantInt::getSigned(i64, -1)));
		op2 = irb.CreateSelect(overflow, llvm::ConstantInt::get(i64, 1), op2);
	}
	op2 = irb.CreateBinaryIntrinsic(llvm::Intrinsic::umax, op2, llvm::ConstantInt::get(i64, 1));

	llvm::Value* dividend = isSigned ? irb.CreateSExt(op1, i128) : irb.CreateZExt(op1, i128);
	dividend = irb.CreateShl(dividend, 64);
	llvm::Value* divisor = isSigned ? irb.CreateSExt(op2, i128) : irb.CreateZExt(op2, i128);
	auto* quot = isSigned ? irb.CreateSDiv(dividend, divisor) : irb.CreateUDiv(dividend, divisor);
	auto* val = irb.CreateTrunc(quot, i64);
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecClz(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	unsigned laneBits = 32;
	unsigned lanes = 4;
	switch (i->id)
	{
	case PPC_INS_VCLZB:
		laneBits = 8;
		lanes = 16;
		break;
	case PPC_INS_VCLZH:
		laneBits = 16;
		lanes = 8;
		break;
	case PPC_INS_VCLZD:
		laneBits = 64;
		lanes = 2;
		break;
	case PPC_INS_VCLZW:
	default:
		break;
	}

	auto* elem = irb.getIntNTy(laneBits);
	auto* src = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctlz, elem);
	llvm::Value* dst = llvm::PoisonValue::get(src->getType());
	for (unsigned n = 0; n < lanes; ++n)
	{
		auto* lane = irb.CreateExtractElement(src, n);
		dst = irb.CreateInsertElement(dst, irb.CreateCall(f, {lane, irb.getFalse()}), n);
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(dst, irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecAvg(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	unsigned laneBits = 32;
	unsigned lanes = 4;
	bool isSigned = false;
	switch (i->id)
	{
	case PPC_INS_VAVGSB:
		laneBits = 8;
		lanes = 16;
		isSigned = true;
		break;
	case PPC_INS_VAVGSH:
		laneBits = 16;
		lanes = 8;
		isSigned = true;
		break;
	case PPC_INS_VAVGSW:
		isSigned = true;
		break;
	case PPC_INS_VAVGUB:
		laneBits = 8;
		lanes = 16;
		break;
	case PPC_INS_VAVGUH:
		laneBits = 16;
		lanes = 8;
		break;
	case PPC_INS_VAVGUW:
	default:
		break;
	}

	auto* elem = irb.getIntNTy(laneBits);
	auto* wide = irb.getIntNTy(laneBits * 2);
	auto* a = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* b = asVec128(loadOpI128(pi->operands[2], irb), elem, lanes, irb);
	auto* wty = llvm::FixedVectorType::get(wide, lanes);
	auto* aw = isSigned ? irb.CreateSExt(a, wty) : irb.CreateZExt(a, wty);
	auto* bw = isSigned ? irb.CreateSExt(b, wty) : irb.CreateZExt(b, wty);
	auto* sum = irb.CreateAdd(irb.CreateAdd(aw, bw), llvm::ConstantInt::get(wty, 1));
	auto* avg = isSigned ? irb.CreateAShr(sum, 1) : irb.CreateLShr(sum, 1);
	storeOpI128(
			pi->operands[0],
			irb.CreateBitCast(irb.CreateTrunc(avg, a->getType()), irb.getInt128Ty()),
			irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecElementLoadStore(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	unsigned elemBits = 8;
	bool isStore = false;
	switch (i->id)
	{
	case PPC_INS_LVEHX:
	case PPC_INS_STVEHX:
		elemBits = 16;
		isStore = (i->id == PPC_INS_STVEHX);
		break;
	case PPC_INS_LVEWX:
	case PPC_INS_STVEWX:
		elemBits = 32;
		isStore = (i->id == PPC_INS_STVEWX);
		break;
	case PPC_INS_STVEBX:
		isStore = true;
		break;
	case PPC_INS_LVEBX:
	default:
		break;
	}

	unsigned elemBytes = elemBits / 8;
	unsigned nElems = 16 / elemBytes;
	auto* ea = loadIndexedEffectiveAddress(pi, irb);
	auto* aligned = irb.CreateAnd(
			ea,
			llvm::ConstantInt::get(ea->getType(), ~static_cast<uint64_t>(elemBytes - 1)));
	auto* byteOff = irb.CreateAnd(ea, llvm::ConstantInt::get(ea->getType(), 15));
	unsigned shiftLog = (elemBytes == 4) ? 2 : (elemBytes == 2) ? 1 : 0;
	auto* idx = shiftLog ? irb.CreateLShr(byteOff, llvm::ConstantInt::get(ea->getType(), shiftLog)) : byteOff;
	auto* fromLeft = irb.CreateSub(llvm::ConstantInt::get(ea->getType(), nElems - 1), idx);
	auto* shiftAmt = irb.CreateMul(fromLeft, llvm::ConstantInt::get(ea->getType(), elemBits));
	auto* i128 = irb.getInt128Ty();
	auto* sh = irb.CreateZExtOrTrunc(shiftAmt, i128);
	auto* elemTy = irb.getIntNTy(elemBits);

	if (isStore)
	{
		auto* val = loadOpI128(pi->operands[0], irb);
		auto* elem = irb.CreateTrunc(irb.CreateLShr(val, sh), elemTy);
		storeIntPtr(irb, elem, aligned, elemTy);
		return;
	}

	auto* loaded = irb.CreateZExt(loadIntPtr(irb, aligned, elemTy), i128);
	storeOpI128(pi->operands[0], irb.CreateShl(loaded, sh), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecFpEstimate(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	llvm::Type* elem = irb.getFloatTy();
	unsigned lanes = 4;
	bool recip = false;
	bool sqrtOnly = false;
	switch (i->id)
	{
	case PPC_INS_XVREDP:
		elem = irb.getDoubleTy();
		lanes = 2;
		recip = true;
		break;
	case PPC_INS_VREFP:
	case PPC_INS_XVRESP:
		recip = true;
		break;
	case PPC_INS_XVSQRTDP:
		elem = irb.getDoubleTy();
		lanes = 2;
		sqrtOnly = true;
		break;
	case PPC_INS_XVSQRTSP:
		sqrtOnly = true;
		break;
	case PPC_INS_XVRSQRTEDP:
		elem = irb.getDoubleTy();
		lanes = 2;
		break;
	case PPC_INS_XVRSQRTESP:
	case PPC_INS_VRSQRTEFP:
	default:
		break;
	}

	if (i->id == PPC_INS_VEXPTEFP || i->id == PPC_INS_VLOGEFP)
	{
		auto* src = asVec128(loadOpI128(pi->operands[1], irb), irb.getFloatTy(), 4, irb);
		auto iid = (i->id == PPC_INS_VEXPTEFP) ? llvm::Intrinsic::exp2 : llvm::Intrinsic::log2;
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid, irb.getFloatTy());
		llvm::Value* dst = llvm::PoisonValue::get(src->getType());
		for (unsigned n = 0; n < 4; ++n)
		{
			dst = irb.CreateInsertElement(dst, irb.CreateCall(f, {irb.CreateExtractElement(src, n)}), n);
		}
		storeOpI128(pi->operands[0], irb.CreateBitCast(dst, irb.getInt128Ty()), irb);
		return;
	}

	auto* src = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* one = llvm::ConstantFP::get(elem, 1.0);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sqrt, elem);
	llvm::Value* dst = llvm::PoisonValue::get(src->getType());
	for (unsigned n = 0; n < lanes; ++n)
	{
		auto* lane = irb.CreateExtractElement(src, n);
		llvm::Value* r = nullptr;
		if (recip)
		{
			r = irb.CreateFDiv(one, lane);
		}
		else if (sqrtOnly)
		{
			r = irb.CreateCall(f, {lane});
		}
		else
		{
			r = irb.CreateFDiv(one, irb.CreateCall(f, {lane}));
		}
		dst = irb.CreateInsertElement(dst, r, n);
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(dst, irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecMinMax(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	unsigned laneBits = 32;
	unsigned lanes = 4;
	bool isMin = false;
	bool isSigned = true;
	switch (i->id)
	{
	case PPC_INS_VMAXSB:
	case PPC_INS_VMINSB:
		laneBits = 8;
		lanes = 16;
		isMin = (i->id == PPC_INS_VMINSB);
		break;
	case PPC_INS_VMAXSH:
	case PPC_INS_VMINSH:
		laneBits = 16;
		lanes = 8;
		isMin = (i->id == PPC_INS_VMINSH);
		break;
	case PPC_INS_VMAXSW:
	case PPC_INS_VMINSW:
		isMin = (i->id == PPC_INS_VMINSW);
		break;
	case PPC_INS_VMAXSD:
	case PPC_INS_VMINSD:
		laneBits = 64;
		lanes = 2;
		isMin = (i->id == PPC_INS_VMINSD);
		break;
	case PPC_INS_VMAXUB:
	case PPC_INS_VMINUB:
		laneBits = 8;
		lanes = 16;
		isSigned = false;
		isMin = (i->id == PPC_INS_VMINUB);
		break;
	case PPC_INS_VMAXUH:
	case PPC_INS_VMINUH:
		laneBits = 16;
		lanes = 8;
		isSigned = false;
		isMin = (i->id == PPC_INS_VMINUH);
		break;
	case PPC_INS_VMAXUW:
	case PPC_INS_VMINUW:
		isSigned = false;
		isMin = (i->id == PPC_INS_VMINUW);
		break;
	case PPC_INS_VMAXUD:
	case PPC_INS_VMINUD:
		laneBits = 64;
		lanes = 2;
		isSigned = false;
		isMin = (i->id == PPC_INS_VMINUD);
		break;
	default:
		return;
	}

	auto* elem = irb.getIntNTy(laneBits);
	auto* a = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* b = asVec128(loadOpI128(pi->operands[2], irb), elem, lanes, irb);
	llvm::Value* pred = nullptr;
	if (isMin)
	{
		pred = isSigned ? irb.CreateICmpSLT(a, b) : irb.CreateICmpULT(a, b);
	}
	else
	{
		pred = isSigned ? irb.CreateICmpSGT(a, b) : irb.CreateICmpUGT(a, b);
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(irb.CreateSelect(pred, a, b), irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecPopcnt(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	unsigned laneBits = 32;
	unsigned lanes = 4;
	switch (i->id)
	{
	case PPC_INS_VPOPCNTB:
		laneBits = 8;
		lanes = 16;
		break;
	case PPC_INS_VPOPCNTH:
		laneBits = 16;
		lanes = 8;
		break;
	case PPC_INS_VPOPCNTD:
		laneBits = 64;
		lanes = 2;
		break;
	case PPC_INS_VPOPCNTW:
	default:
		break;
	}

	auto* elem = irb.getIntNTy(laneBits);
	auto* src = asVec128(loadOpI128(pi->operands[1], irb), elem, lanes, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctpop, elem);
	llvm::Value* dst = llvm::PoisonValue::get(src->getType());
	for (unsigned n = 0; n < lanes; ++n)
	{
		dst = irb.CreateInsertElement(dst, irb.CreateCall(f, {irb.CreateExtractElement(src, n)}), n);
	}
	storeOpI128(pi->operands[0], irb.CreateBitCast(dst, irb.getInt128Ty()), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecMulWiden(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	auto* i128 = irb.getInt128Ty();
	if (i->id == PPC_INS_VMULUWM)
	{
		auto* va = asVec128(a, irb.getInt32Ty(), 4, irb);
		auto* vb = asVec128(b, irb.getInt32Ty(), 4, irb);
		storeOpI128(pi->operands[0], irb.CreateBitCast(irb.CreateMul(va, vb), i128), irb);
		return;
	}

	unsigned srcBits = 8;
	bool even = true;
	bool isSigned = true;
	switch (i->id)
	{
	case PPC_INS_VMULESB:
		break;
	case PPC_INS_VMULESH:
		srcBits = 16;
		break;
	case PPC_INS_VMULESW:
		srcBits = 32;
		break;
	case PPC_INS_VMULEUB:
		isSigned = false;
		break;
	case PPC_INS_VMULEUH:
		srcBits = 16;
		isSigned = false;
		break;
	case PPC_INS_VMULEUW:
		srcBits = 32;
		isSigned = false;
		break;
	case PPC_INS_VMULOSB:
		even = false;
		break;
	case PPC_INS_VMULOSH:
		srcBits = 16;
		even = false;
		break;
	case PPC_INS_VMULOSW:
		srcBits = 32;
		even = false;
		break;
	case PPC_INS_VMULOUB:
		even = false;
		isSigned = false;
		break;
	case PPC_INS_VMULOUH:
		srcBits = 16;
		even = false;
		isSigned = false;
		break;
	case PPC_INS_VMULOUW:
		srcBits = 32;
		even = false;
		isSigned = false;
		break;
	default:
		break;
	}

	const unsigned dstBits = srcBits * 2u;
	const unsigned nDst = 128u / dstBits;
	const unsigned nSrc = 128u / srcBits;
	auto* srcTy = irb.getIntNTy(srcBits);
	auto* dstTy = irb.getIntNTy(dstBits);
	llvm::Value* result = llvm::ConstantInt::get(i128, 0);
	for (unsigned k = 0; k < nDst; ++k)
	{
		unsigned srcIdx = even ? (2u * k) : (2u * k + 1u);
		if (srcIdx >= nSrc)
		{
			break;
		}
		auto take = [&](llvm::Value* v) {
			unsigned sh = (nSrc - 1u - srcIdx) * srcBits;
			auto* e = irb.CreateTrunc(irb.CreateLShr(v, sh), srcTy);
			return isSigned ? irb.CreateSExt(e, dstTy) : irb.CreateZExt(e, dstTy);
		};
		auto* prod = irb.CreateMul(take(a), take(b));
		unsigned place = (nDst - 1u - k) * dstBits;
		result = irb.CreateOr(result, irb.CreateShl(irb.CreateZExt(prod, i128), place));
	}
	storeOpI128(pi->operands[0], result, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecMsum(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, pi, irb);

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	auto* c = loadOpI128(pi->operands[3], irb);
	auto* i16 = irb.getInt16Ty();
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();

	auto beElem = [&](llvm::Value* v, unsigned idx, unsigned bits) {
		unsigned n = 128u / bits;
		unsigned sh = (n - 1u - idx) * bits;
		return irb.CreateTrunc(irb.CreateLShr(v, sh), irb.getIntNTy(bits));
	};

	auto satI16 = [&](llvm::Value* w64) {
		auto* lo = llvm::ConstantInt::get(i64, llvm::APInt(64, static_cast<uint64_t>(-32768), true));
		auto* hi = llvm::ConstantInt::get(i64, 32767);
		w64 = irb.CreateSelect(irb.CreateICmpSLT(w64, lo), lo, w64);
		w64 = irb.CreateSelect(irb.CreateICmpSGT(w64, hi), hi, w64);
		return irb.CreateTrunc(w64, i16);
	};

	if (i->id == PPC_INS_VMHADDSHS || i->id == PPC_INS_VMHRADDSHS || i->id == PPC_INS_VMLADDUHM)
	{
		llvm::Value* result = llvm::ConstantInt::get(i128, 0);
		const bool round = (i->id == PPC_INS_VMHRADDSHS);
		const bool uns = (i->id == PPC_INS_VMLADDUHM);
		for (unsigned k = 0; k < 8; ++k)
		{
			auto* ah = beElem(a, k, 16);
			auto* bh = beElem(b, k, 16);
			auto* ch = beElem(c, k, 16);
			llvm::Value* out = nullptr;
			if (uns)
			{
				auto* prod = irb.CreateMul(irb.CreateZExt(ah, i32), irb.CreateZExt(bh, i32));
				out = irb.CreateTrunc(irb.CreateAdd(prod, irb.CreateZExt(ch, i32)), i16);
			}
			else
			{
				auto* prod = irb.CreateMul(irb.CreateSExt(ah, i64), irb.CreateSExt(bh, i64));
				auto* acc = irb.CreateAdd(prod, irb.CreateShl(irb.CreateSExt(ch, i64), 16));
				if (round)
				{
					acc = irb.CreateAdd(acc, llvm::ConstantInt::get(i64, 0x4000));
				}
				out = satI16(irb.CreateAShr(acc, 16));
			}
			unsigned place = (7u - k) * 16u;
			result = irb.CreateOr(result, irb.CreateShl(irb.CreateZExt(out, i128), place));
		}
		storeOpI128(pi->operands[0], result, irb);
		return;
	}

	const bool bytes = (i->id == PPC_INS_VMSUMMBM || i->id == PPC_INS_VMSUMUBM);
	const bool aSigned = (i->id == PPC_INS_VMSUMMBM || i->id == PPC_INS_VMSUMSHM
			|| i->id == PPC_INS_VMSUMSHS);
	const bool bSigned = (i->id == PPC_INS_VMSUMSHM || i->id == PPC_INS_VMSUMSHS);
	const bool saturate = (i->id == PPC_INS_VMSUMSHS || i->id == PPC_INS_VMSUMUHS);
	const bool satSigned = (i->id == PPC_INS_VMSUMSHS);

	llvm::Value* result = llvm::ConstantInt::get(i128, 0);
	for (unsigned w = 0; w < 4; ++w)
	{
		llvm::Value* sum = irb.CreateSExt(beElem(c, w, 32), i64);
		if (!aSigned && !bSigned)
		{
			sum = irb.CreateZExt(beElem(c, w, 32), i64);
		}
		if (bytes)
		{
			for (unsigned k = 0; k < 4; ++k)
			{
				auto* ab = beElem(a, 4u * w + k, 8);
				auto* bb = beElem(b, 4u * w + k, 8);
				auto* ae = aSigned ? irb.CreateSExt(ab, i64) : irb.CreateZExt(ab, i64);
				auto* be = irb.CreateZExt(bb, i64);
				sum = irb.CreateAdd(sum, irb.CreateMul(ae, be));
			}
		}
		else
		{
			for (unsigned k = 0; k < 2; ++k)
			{
				auto* ah = beElem(a, 2u * w + k, 16);
				auto* bh = beElem(b, 2u * w + k, 16);
				auto* ae = aSigned ? irb.CreateSExt(ah, i64) : irb.CreateZExt(ah, i64);
				auto* be = bSigned ? irb.CreateSExt(bh, i64) : irb.CreateZExt(bh, i64);
				sum = irb.CreateAdd(sum, irb.CreateMul(ae, be));
			}
		}
		llvm::Value* out = nullptr;
		if (saturate)
		{
			if (satSigned)
			{
				auto* lo = llvm::ConstantInt::get(i64, llvm::APInt(64, static_cast<uint64_t>(INT32_MIN), true));
				auto* hi = llvm::ConstantInt::get(i64, INT32_MAX);
				sum = irb.CreateSelect(irb.CreateICmpSLT(sum, lo), lo, sum);
				sum = irb.CreateSelect(irb.CreateICmpSGT(sum, hi), hi, sum);
			}
			else
			{
				auto* hi = llvm::ConstantInt::get(i64, 0xFFFFFFFFull);
				sum = irb.CreateSelect(irb.CreateICmpUGT(sum, hi), hi, sum);
			}
		}
		out = irb.CreateTrunc(sum, i32);
		unsigned place = (3u - w) * 32u;
		result = irb.CreateOr(result, irb.CreateShl(irb.CreateZExt(out, i128), place));
	}
	storeOpI128(pi->operands[0], result, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecUnpack(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, pi, irb);

	auto* src = loadOpI128(pi->operands[1], irb);
	auto* i128 = irb.getInt128Ty();
	unsigned srcBits = 8;
	unsigned dstBits = 16;
	bool high = true;
	bool pixel = false;
	switch (i->id)
	{
	case PPC_INS_VUPKHSB:
		break;
	case PPC_INS_VUPKLSB:
		high = false;
		break;
	case PPC_INS_VUPKHSH:
		srcBits = 16;
		dstBits = 32;
		break;
	case PPC_INS_VUPKLSH:
		srcBits = 16;
		dstBits = 32;
		high = false;
		break;
	case PPC_INS_VUPKHSW:
		srcBits = 32;
		dstBits = 64;
		break;
	case PPC_INS_VUPKLSW:
		srcBits = 32;
		dstBits = 64;
		high = false;
		break;
	case PPC_INS_VUPKHPX:
		srcBits = 16;
		dstBits = 32;
		pixel = true;
		break;
	case PPC_INS_VUPKLPX:
		srcBits = 16;
		dstBits = 32;
		high = false;
		pixel = true;
		break;
	default:
		break;
	}

	const unsigned nDst = 128u / dstBits;
	const unsigned nSrc = 128u / srcBits;
	const unsigned base = high ? 0u : nDst;
	auto* srcTy = irb.getIntNTy(srcBits);
	auto* dstTy = irb.getIntNTy(dstBits);
	llvm::Value* result = llvm::ConstantInt::get(i128, 0);
	for (unsigned k = 0; k < nDst; ++k)
	{
		unsigned srcIdx = base + k;
		if (srcIdx >= nSrc)
		{
			break;
		}
		unsigned sh = (nSrc - 1u - srcIdx) * srcBits;
		auto* e = irb.CreateTrunc(irb.CreateLShr(src, sh), srcTy);
		llvm::Value* w = nullptr;
		if (pixel)
		{
			auto* p = irb.CreateZExt(e, irb.getInt32Ty());
			auto* aBit = irb.CreateAnd(irb.CreateLShr(p, 15), llvm::ConstantInt::get(p->getType(), 1));
			auto* r = irb.CreateAnd(irb.CreateLShr(p, 10), llvm::ConstantInt::get(p->getType(), 0x1f));
			auto* g = irb.CreateAnd(irb.CreateLShr(p, 5), llvm::ConstantInt::get(p->getType(), 0x1f));
			auto* b = irb.CreateAnd(p, llvm::ConstantInt::get(p->getType(), 0x1f));
			auto* aByte = irb.CreateSub(
					llvm::ConstantInt::get(p->getType(), 0),
					aBit);
			aByte = irb.CreateAnd(aByte, llvm::ConstantInt::get(p->getType(), 0xFF000000u));
			w = irb.CreateOr(
					irb.CreateOr(aByte, irb.CreateShl(r, 19)),
					irb.CreateOr(irb.CreateShl(g, 11), irb.CreateShl(b, 3)));
		}
		else
		{
			w = irb.CreateSExt(e, dstTy);
		}
		unsigned place = (nDst - 1u - k) * dstBits;
		result = irb.CreateOr(result, irb.CreateShl(irb.CreateZExt(w, i128), place));
	}
	storeOpI128(pi->operands[0], result, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecSum(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, pi, irb);

	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();

	auto beElem = [&](llvm::Value* v, unsigned idx, unsigned bits) {
		unsigned n = 128u / bits;
		unsigned sh = (n - 1u - idx) * bits;
		return irb.CreateTrunc(irb.CreateLShr(v, sh), irb.getIntNTy(bits));
	};
	auto satI32 = [&](llvm::Value* w64) {
		auto* lo = llvm::ConstantInt::get(i64, llvm::APInt(64, static_cast<uint64_t>(INT32_MIN), true));
		auto* hi = llvm::ConstantInt::get(i64, INT32_MAX);
		w64 = irb.CreateSelect(irb.CreateICmpSLT(w64, lo), lo, w64);
		w64 = irb.CreateSelect(irb.CreateICmpSGT(w64, hi), hi, w64);
		return irb.CreateTrunc(w64, i32);
	};
	auto satU32 = [&](llvm::Value* w64) {
		auto* hi = llvm::ConstantInt::get(i64, 0xFFFFFFFFull);
		w64 = irb.CreateSelect(irb.CreateICmpUGT(w64, hi), hi, w64);
		return irb.CreateTrunc(w64, i32);
	};

	llvm::Value* words[4] = {
			llvm::ConstantInt::get(i32, 0),
			llvm::ConstantInt::get(i32, 0),
			llvm::ConstantInt::get(i32, 0),
			llvm::ConstantInt::get(i32, 0)};

	if (i->id == PPC_INS_VSUM4SBS || i->id == PPC_INS_VSUM4UBS)
	{
		const bool uns = (i->id == PPC_INS_VSUM4UBS);
		for (unsigned w = 0; w < 4; ++w)
		{
			llvm::Value* sum = uns ? irb.CreateZExt(beElem(b, w, 32), i64)
					: irb.CreateSExt(beElem(b, w, 32), i64);
			for (unsigned k = 0; k < 4; ++k)
			{
				auto* e = beElem(a, 4u * w + k, 8);
				sum = irb.CreateAdd(sum, uns ? irb.CreateZExt(e, i64) : irb.CreateSExt(e, i64));
			}
			words[w] = uns ? satU32(sum) : satI32(sum);
		}
	}
	else if (i->id == PPC_INS_VSUM4SHS)
	{
		for (unsigned w = 0; w < 4; ++w)
		{
			llvm::Value* sum = irb.CreateSExt(beElem(b, w, 32), i64);
			sum = irb.CreateAdd(sum, irb.CreateSExt(beElem(a, 2u * w, 16), i64));
			sum = irb.CreateAdd(sum, irb.CreateSExt(beElem(a, 2u * w + 1u, 16), i64));
			words[w] = satI32(sum);
		}
	}
	else if (i->id == PPC_INS_VSUM2SWS)
	{
		auto sumPair = [&](unsigned a0, unsigned a1, unsigned bIdx) {
			auto* s = irb.CreateSExt(beElem(a, a0, 32), i64);
			s = irb.CreateAdd(s, irb.CreateSExt(beElem(a, a1, 32), i64));
			s = irb.CreateAdd(s, irb.CreateSExt(beElem(b, bIdx, 32), i64));
			return satI32(s);
		};
		words[1] = sumPair(0, 1, 1);
		words[3] = sumPair(2, 3, 3);
	}
	else
	{
		llvm::Value* s = irb.CreateSExt(beElem(b, 3, 32), i64);
		for (unsigned w = 0; w < 4; ++w)
		{
			s = irb.CreateAdd(s, irb.CreateSExt(beElem(a, w, 32), i64));
		}
		words[3] = satI32(s);
	}

	llvm::Value* result = llvm::ConstantInt::get(i128, 0);
	for (unsigned w = 0; w < 4; ++w)
	{
		result = irb.CreateOr(result, irb.CreateShl(irb.CreateZExt(words[w], i128), (3u - w) * 32u));
	}
	storeOpI128(pi->operands[0], result, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecFpTest(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 1)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	uint32_t crReg = PPC_REG_CR6;
	llvm::Value* va = nullptr;
	llvm::Value* vb = nullptr;
	unsigned nVec = 0;
	for (uint8_t n = 0; n < pi->op_count; ++n)
	{
		if (pi->operands[n].type != PPC_OP_REG)
		{
			continue;
		}
		if (isCrRegister(pi->operands[n].reg) && !isCrBitRegister(pi->operands[n].reg))
		{
			crReg = pi->operands[n].reg;
			continue;
		}
		if (nVec == 0)
		{
			va = loadOpI128(pi->operands[n], irb);
		}
		else
		{
			vb = loadOpI128(pi->operands[n], irb);
		}
		++nVec;
	}
	if (va == nullptr)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	const bool isDiv = (i->id == PPC_INS_XVTDIVDP || i->id == PPC_INS_XVTDIVSP
			|| i->id == PPC_INS_XSTDIVDP || i->id == PPC_INS_FTDIV);
	const bool isDp = (i->id == PPC_INS_XVTDIVDP || i->id == PPC_INS_XVTSQRTDP
			|| i->id == PPC_INS_XSTDIVDP || i->id == PPC_INS_XSTSQRTDP || i->id == PPC_INS_FTDIV
			|| i->id == PPC_INS_FTSQRT);
	const bool isScalar = (i->id == PPC_INS_XSTDIVDP || i->id == PPC_INS_XSTSQRTDP
			|| i->id == PPC_INS_FTDIV || i->id == PPC_INS_FTSQRT);
	if (isDiv && vb == nullptr)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}

	if (isScalar)
	{
		auto* la = i128HighDouble(va, irb);
		auto* fabsF = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, irb.getDoubleTy());
		auto* inf = llvm::ConstantFP::getInfinity(irb.getDoubleTy());
		auto* zero = llvm::ConstantFP::get(irb.getDoubleTy(), 0.0);
		auto* absA = irb.CreateCall(fabsF, {la});
		llvm::Value* ok = irb.CreateAnd(irb.CreateFCmpORD(la, la), irb.CreateFCmpOLT(absA, inf));
		if (isDiv)
		{
			auto* lb = i128HighDouble(vb, irb);
			auto* absB = irb.CreateCall(fabsF, {lb});
			auto* finB = irb.CreateAnd(irb.CreateFCmpORD(lb, lb), irb.CreateFCmpOLT(absB, inf));
			ok = irb.CreateAnd(irb.CreateAnd(ok, finB), irb.CreateFCmpONE(lb, zero));
		}
		else
		{
			ok = irb.CreateAnd(ok, irb.CreateFCmpOGE(la, zero));
		}
		uint32_t ltR = PPC_REG_CR0LT;
		uint32_t gtR = PPC_REG_CR0GT;
		uint32_t eqR = PPC_REG_CR0EQ;
		uint32_t soR = PPC_REG_CR0UN;
		crFieldRegisters(crReg, ltR, gtR, eqR, soR);
		storeRegister(ltR, irb.getInt1(false), irb);
		storeRegister(gtR, irb.getInt1(false), irb);
		storeRegister(eqR, ok, irb);
		storeRegister(soR, irb.CreateNot(ok), irb);
		return;
	}

	llvm::Type* elem = isDp ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned lanes = isDp ? 2u : 4u;
	auto* srcA = asVec128(va, elem, lanes, irb);
	auto* srcB = (vb != nullptr) ? asVec128(vb, elem, lanes, irb) : nullptr;
	auto* fabsF = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, elem);
	auto* inf = llvm::ConstantFP::getInfinity(elem);
	auto* zero = llvm::ConstantFP::get(elem, 0.0);
	llvm::Value* allOk = irb.getInt1(true);
	for (unsigned n = 0; n < lanes; ++n)
	{
		auto* la = irb.CreateExtractElement(srcA, n);
		auto* absA = irb.CreateCall(fabsF, {la});
		auto* finA = irb.CreateAnd(irb.CreateFCmpORD(la, la), irb.CreateFCmpOLT(absA, inf));
		llvm::Value* ok = finA;
		if (isDiv)
		{
			auto* lb = irb.CreateExtractElement(srcB, n);
			auto* absB = irb.CreateCall(fabsF, {lb});
			auto* finB = irb.CreateAnd(irb.CreateFCmpORD(lb, lb), irb.CreateFCmpOLT(absB, inf));
			ok = irb.CreateAnd(irb.CreateAnd(finA, finB), irb.CreateFCmpONE(lb, zero));
		}
		else
		{
			ok = irb.CreateAnd(finA, irb.CreateFCmpOGE(la, zero));
		}
		allOk = irb.CreateAnd(allOk, ok);
	}

	uint32_t ltR = PPC_REG_CR0LT;
	uint32_t gtR = PPC_REG_CR0GT;
	uint32_t eqR = PPC_REG_CR0EQ;
	uint32_t soR = PPC_REG_CR0UN;
	crFieldRegisters(crReg, ltR, gtR, eqR, soR);
	storeRegister(ltR, irb.getInt1(false), irb);
	storeRegister(gtR, irb.getInt1(false), irb);
	storeRegister(eqR, allOk, irb);
	storeRegister(soR, irb.CreateNot(allOk), irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateXxPerm(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto dw = [&](llvm::Value* v, bool high) {
		return high ? irb.CreateTrunc(irb.CreateLShr(v, 64), i64) : irb.CreateTrunc(v, i64);
	};
	auto pack = [&](llvm::Value* hi, llvm::Value* lo) {
		return irb.CreateOr(irb.CreateShl(irb.CreateZExt(hi, i128), 64), irb.CreateZExt(lo, i128));
	};

	if (ppcIsId(i, PPC_INS_XXSWAPD))
	{
		EXPECT_IS_BINARY(i, pi, irb);
		auto* src = loadOpI128(pi->operands[1], irb);
		storeOpI128(pi->operands[0], pack(dw(src, false), dw(src, true)), irb);
		return;
	}
	if (ppcIsId(i, PPC_INS_XXSPLTD))
	{
		if (pi->op_count < 2)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		auto* src = loadOpI128(pi->operands[1], irb);
		unsigned uim = 0;
		if (pi->op_count >= 3 && pi->operands[2].type == PPC_OP_IMM)
		{
			uim = static_cast<unsigned>(pi->operands[2].imm) & 1u;
		}
		auto* lane = dw(src, uim == 0);
		storeOpI128(pi->operands[0], pack(lane, lane), irb);
		return;
	}
	if (ppcIsId(i, PPC_INS_XXMRGHD) || ppcIsId(i, PPC_INS_XXMRGLD))
	{
		EXPECT_IS_TERNARY(i, pi, irb);
		auto* a = loadOpI128(pi->operands[1], irb);
		auto* b = loadOpI128(pi->operands[2], irb);
		const bool high = ppcIsId(i, PPC_INS_XXMRGHD);
		storeOpI128(pi->operands[0], pack(dw(a, high), dw(b, high)), irb);
		return;
	}
	if (i->id == PPC_INS_XXSLDWI)
	{
		if (pi->op_count < 3)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		auto* a = loadOpI128(pi->operands[1], irb);
		llvm::Value* b = a;
		unsigned shw = 0;
		if (pi->op_count >= 4)
		{
			b = loadOpI128(pi->operands[2], irb);
			if (pi->operands[3].type == PPC_OP_IMM)
			{
				shw = static_cast<unsigned>(pi->operands[3].imm) & 3u;
			}
		}
		else if (pi->operands[2].type == PPC_OP_IMM)
		{
			shw = static_cast<unsigned>(pi->operands[2].imm) & 3u;
		}
		else
		{
			b = loadOpI128(pi->operands[2], irb);
		}
		auto* i256 = irb.getIntNTy(256);
		auto* concat = irb.CreateOr(irb.CreateShl(irb.CreateZExt(a, i256), 128), irb.CreateZExt(b, i256));
		auto* shifted = irb.CreateShl(concat, llvm::ConstantInt::get(i256, static_cast<uint64_t>(shw) * 32u));
		storeOpI128(pi->operands[0], irb.CreateTrunc(irb.CreateLShr(shifted, 128), i128), irb);
		return;
	}

	throwUnexpectedOperands(i);
	translatePseudoAsmGeneric(i, pi, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateFpscr(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();

	auto fpscrField = [](const cs_ppc_op& op) -> unsigned {
		if (op.type == PPC_OP_REG && op.reg >= PPC_REG_CR0 && op.reg <= PPC_REG_CR7)
		{
			return static_cast<unsigned>(op.reg - PPC_REG_CR0);
		}
		if (op.type == PPC_OP_IMM)
		{
			return static_cast<unsigned>(op.imm) & 7u;
		}
		return 0;
	};

	if (ppcIsId(i, PPC_INS_MFFS))
	{
		if (pi->op_count < 1)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		auto* bits = irb.CreateZExt(loadRegister(kFpscrReg, irb), i64);
		storeOp(pi->operands[0], bits, irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}

	// ISA 3.0 light-weight FPSCR moves. Only the low word of the FPSCR is
	// modelled, so DRN reads as zero.
	if (ppcIsId(i, PPC_INS_MFFSL) || ppcIsId(i, PPC_INS_MFFSCE) || ppcIsId(i, PPC_INS_MFFSCRN)
			|| ppcIsId(i, PPC_INS_MFFSCRNI))
	{
		if (pi->op_count < 1)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		auto* old = loadRegister(kFpscrReg, irb);
		uint32_t mask = 0xffffffffu;
		if (ppcIsId(i, PPC_INS_MFFSL))
		{
			mask = 0x0001f0ffu; // FPRF, enables, NI, RN
		}
		else if (!ppcIsId(i, PPC_INS_MFFSCE))
		{
			mask = 0x000000ffu; // enables, NI, RN
		}
		storeOp(pi->operands[0], irb.CreateZExt(irb.CreateAnd(old, llvm::ConstantInt::get(i32, mask)), i64), irb,
				eOpConv::FPCAST_OR_BITCAST);
		if (ppcIsId(i, PPC_INS_MFFSCE))
		{
			// Clear VE, OE, UE, ZE and XE.
			storeRegister(kFpscrReg, irb.CreateAnd(old, llvm::ConstantInt::get(i32, ~0xf8u)), irb);
		}
		else if (ppcIsId(i, PPC_INS_MFFSCRN) || ppcIsId(i, PPC_INS_MFFSCRNI))
		{
			if (pi->op_count < 2)
			{
				return;
			}
			llvm::Value* rn = nullptr;
			if (pi->operands[1].type == PPC_OP_IMM)
			{
				rn = llvm::ConstantInt::get(i32, uint64_t(pi->operands[1].imm) & 3u);
			}
			else
			{
				auto* b = loadOp(pi->operands[1], irb);
				b = b->getType()->isIntegerTy() ? irb.CreateZExtOrTrunc(b, i64) : irb.CreateBitCast(b, i64);
				rn = irb.CreateAnd(irb.CreateTrunc(b, i32), llvm::ConstantInt::get(i32, 3));
			}
			storeRegister(kFpscrReg, irb.CreateOr(irb.CreateAnd(old, llvm::ConstantInt::get(i32, ~3u)), rn), irb);
		}
		return;
	}

	if (ppcIsId(i, PPC_INS_MTFSB0) || ppcIsId(i, PPC_INS_MTFSB1))
	{
		if (pi->op_count < 1 || pi->operands[0].type != PPC_OP_IMM)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		unsigned bt = static_cast<unsigned>(pi->operands[0].imm) & 31u;
		auto* bit = llvm::ConstantInt::get(i32, 1u << (31u - bt));
		auto* old = loadRegister(kFpscrReg, irb);
		llvm::Value* val = ppcIsId(i, PPC_INS_MTFSB1)
				? irb.CreateOr(old, bit)
				: irb.CreateAnd(old, irb.CreateNot(bit));
		storeRegister(kFpscrReg, val, irb);
		return;
	}

	if (ppcIsId(i, PPC_INS_MTFSFI))
	{
		if (pi->op_count < 2)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		unsigned bf = fpscrField(pi->operands[0]);
		unsigned u = 0;
		if (pi->operands[1].type == PPC_OP_IMM)
		{
			u = static_cast<unsigned>(pi->operands[1].imm) & 15u;
		}
		unsigned sh = (7u - bf) * 4u;
		auto* mask = llvm::ConstantInt::get(i32, 0xFu << sh);
		auto* old = loadRegister(kFpscrReg, irb);
		auto* val = irb.CreateOr(
				irb.CreateAnd(old, irb.CreateNot(mask)),
				llvm::ConstantInt::get(i32, u << sh));
		storeRegister(kFpscrReg, val, irb);
		return;
	}

	if (ppcIsId(i, PPC_INS_MTFSF))
	{
		if (pi->op_count < 2)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		unsigned flm = 0xff;
		cs_ppc_op* frb = &pi->operands[1];
		if (pi->operands[0].type == PPC_OP_IMM)
		{
			flm = static_cast<unsigned>(pi->operands[0].imm) & 0xffu;
		}
		else
		{
			frb = &pi->operands[0];
		}
		if (pi->op_count >= 3 && pi->operands[2].type == PPC_OP_REG)
		{
			frb = &pi->operands[2];
		}
		uint32_t maskBits = 0;
		for (unsigned n = 0; n < 8; ++n)
		{
			if (flm & (0x80u >> n))
			{
				maskBits |= 0xF0000000u >> (n * 4u);
			}
		}
		auto* raw = loadOp(*frb, irb);
		llvm::Value* incoming = nullptr;
		if (raw != nullptr && raw->getType()->isDoubleTy())
		{
			incoming = irb.CreateTrunc(irb.CreateBitCast(raw, i64), i32);
		}
		else if (raw != nullptr && raw->getType()->isIntegerTy())
		{
			incoming = irb.CreateZExtOrTrunc(raw, i32);
		}
		else
		{
			incoming = irb.CreateTrunc(irb.CreateBitCast(loadOpDouble(*frb, irb), i64), i32);
		}
		auto* mask = llvm::ConstantInt::get(i32, maskBits);
		auto* old = loadRegister(kFpscrReg, irb);
		auto* val = irb.CreateOr(irb.CreateAnd(old, irb.CreateNot(mask)), irb.CreateAnd(incoming, mask));
		storeRegister(kFpscrReg, val, irb);
		return;
	}

	if (ppcIsId(i, PPC_INS_MCRFS))
	{
		if (pi->op_count < 2)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		unsigned bf = fpscrField(pi->operands[0]);
		unsigned bfa = fpscrField(pi->operands[1]);
		unsigned sh = (7u - bfa) * 4u;
		auto* old = loadRegister(kFpscrReg, irb);
		auto* nibble = irb.CreateAnd(irb.CreateLShr(old, sh), llvm::ConstantInt::get(i32, 0xFu));
		uint32_t ltR = PPC_REG_CR0LT;
		uint32_t gtR = PPC_REG_CR0GT;
		uint32_t eqR = PPC_REG_CR0EQ;
		uint32_t soR = PPC_REG_CR0UN;
		crFieldRegisters(PPC_REG_CR0 + bf, ltR, gtR, eqR, soR);
		storeRegister(ltR, irb.CreateTrunc(irb.CreateLShr(nibble, 3), irb.getInt1Ty()), irb);
		storeRegister(gtR, irb.CreateTrunc(irb.CreateLShr(nibble, 2), irb.getInt1Ty()), irb);
		storeRegister(eqR, irb.CreateTrunc(irb.CreateLShr(nibble, 1), irb.getInt1Ty()), irb);
		storeRegister(soR, irb.CreateTrunc(nibble, irb.getInt1Ty()), irb);
		auto* clear = llvm::ConstantInt::get(i32, ~(0xFu << sh));
		storeRegister(kFpscrReg, irb.CreateAnd(old, clear), irb);
		return;
	}

	throwUnexpectedOperands(i);
	translatePseudoAsmGeneric(i, pi, irb);
}

void Capstone2LlvmIrTranslatorPowerpc_impl::translateVscr(
		cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 1 || pi->operands[0].type != PPC_OP_REG)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	if (ppcIsId(i, PPC_INS_MTVSCR))
	{
		auto* src = loadOpI128(pi->operands[0], irb);
		storeRegister(kVscrReg, irb.CreateTrunc(src, irb.getInt32Ty()), irb);
		return;
	}
	auto* bits = irb.CreateZExt(loadRegister(kVscrReg, irb), irb.getInt128Ty());
	storeOpI128(pi->operands[0], bits, irb);
}

/**
 * Overflow-enable (OE=1) forms compute the same result as the plain
 * instruction; the XER OV/SO side effect is not modelled, so they reuse the
 * plain translator with the id swapped for the duration of the call.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateTwin(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	unsigned sid = 0;
	void (Capstone2LlvmIrTranslatorPowerpc_impl::*fn)(cs_insn*, cs_ppc*, llvm::IRBuilder<>&) = nullptr;
	switch (i->id)
	{
		case PPC_INS_ADDO: sid = PPC_INS_ADD; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateAdd; break;
		case PPC_INS_ADDCO: sid = PPC_INS_ADDC; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateAddc; break;
		case PPC_INS_ADDEO: sid = PPC_INS_ADDE; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateAdde; break;
		case PPC_INS_ADDMEO: sid = PPC_INS_ADDME; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateAddme; break;
		case PPC_INS_ADDZEO: sid = PPC_INS_ADDZE; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateAddze; break;
		case PPC_INS_NEGO: sid = PPC_INS_NEG; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateNeg; break;
		case PPC_INS_MULLWO: sid = PPC_INS_MULLW; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateMullw; break;
		case PPC_INS_MULLDO: sid = PPC_INS_MULLD; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateMullw; break;
		case PPC_INS_DIVWO: sid = PPC_INS_DIVW; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateDivw; break;
		case PPC_INS_DIVWUO: sid = PPC_INS_DIVWU; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateDivw; break;
		case PPC_INS_DIVDO: sid = PPC_INS_DIVD; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateDivw; break;
		case PPC_INS_DIVDUO: sid = PPC_INS_DIVDU; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateDivw; break;
		case PPC_INS_DIVWEO: sid = PPC_INS_DIVWE; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translatePpcMisc; break;
		case PPC_INS_DIVWEUO: sid = PPC_INS_DIVWEU; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translatePpcMisc; break;
		case PPC_INS_DIVDEO: sid = PPC_INS_DIVDE; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateDivde; break;
		case PPC_INS_DIVDEUO: sid = PPC_INS_DIVDEU; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateDivde; break;
		case PPC_INS_SUBFO: sid = PPC_INS_SUBF; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateSubf; break;
		case PPC_INS_SUBFCO: sid = PPC_INS_SUBFC; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateSubfc; break;
		case PPC_INS_SUBFEO: sid = PPC_INS_SUBFE; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateSubfe; break;
		case PPC_INS_SUBFMEO: sid = PPC_INS_SUBFME; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateSubfme; break;
		case PPC_INS_SUBFZEO: sid = PPC_INS_SUBFZE; fn = &Capstone2LlvmIrTranslatorPowerpc_impl::translateSubfze; break;
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
	unsigned hid = i->id;
	// Operands are read before the plain translator may overwrite RT.
	auto* ov = overflowOf(i, pi, irb);
	// Capstone 6 prints SUBF/SUBFC through their SUB/SUBC aliases, so the
	// plain translators expect [RB, RA]; the OE forms list [RA, RB].
	bool swap = (hid == PPC_INS_SUBFO || hid == PPC_INS_SUBFCO) && pi->op_count == 3;
	if (swap)
	{
		std::swap(pi->operands[1], pi->operands[2]);
	}
	i->id = sid;
	try
	{
		(this->*fn)(i, pi, irb);
	}
	catch (...)
	{
		i->id = hid;
		if (swap)
		{
			std::swap(pi->operands[1], pi->operands[2]);
		}
		throw;
	}
	i->id = hid;
	if (swap)
	{
		std::swap(pi->operands[1], pi->operands[2]);
	}
	if (ov)
	{
		storeRegister(kXerOvReg, ov, irb);
		storeRegister(kXerSoReg, irb.CreateOr(loadRegister(kXerSoReg, irb), ov), irb);
	}
}

namespace {

unsigned ppcDpdToBcd(unsigned d)
{
	auto b = [&](unsigned k) { return (d >> k) & 1u; };
	unsigned p = b(9), q = b(8), r = b(7), s = b(6), t = b(5), u = b(4), v = b(3), w = b(2), x = b(1), y = b(0);
	unsigned pqr = (p << 2) | (q << 1) | r, stu = (s << 2) | (t << 1) | u, wxy = (w << 2) | (x << 1) | y;
	unsigned d2 = 0, d1 = 0, d0 = 0;
	if (!v) { d2 = pqr; d1 = stu; d0 = wxy; }
	else if (!w && !x) { d2 = pqr; d1 = stu; d0 = 8 + y; }
	else if (!w && x) { d2 = pqr; d1 = 8 + u; d0 = (s << 2) | (t << 1) | y; }
	else if (w && !x) { d2 = 8 + r; d1 = stu; d0 = (p << 2) | (q << 1) | y; }
	else if (!s && !t) { d2 = 8 + r; d1 = 8 + u; d0 = (p << 2) | (q << 1) | y; }
	else if (!s && t) { d2 = 8 + r; d1 = (p << 2) | (q << 1) | u; d0 = 8 + y; }
	else if (s && !t) { d2 = pqr; d1 = 8 + u; d0 = 8 + y; }
	else { d2 = 8 + r; d1 = 8 + u; d0 = 8 + y; }
	return (d2 << 8) | (d1 << 4) | d0;
}

} // anonymous namespace

/**
 * ADDG6S, CDTBCD and CBCDTD: decimal-assist operations. The DPD<->BCD
 * declet conversions index constant lookup vectors.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translatePpcDecimalAssist(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* dt = getDefaultType();
	unsigned xlen = dt->getIntegerBitWidth();
	auto c = [&](uint64_t v) { return llvm::ConstantInt::get(dt, v); };
	if (i->id == PPC_INS_ADDG6S)
	{
		if (pi->op_count != 3) { translatePseudoAsmGeneric(i, pi, irb); return; }
		auto* a = irb.CreateZExtOrTrunc(loadOp(pi->operands[1], irb), dt);
		auto* b = irb.CreateZExtOrTrunc(loadOp(pi->operands[2], irb), dt);
		auto* sum = irb.CreateAdd(a, b);
		// Carry into bit k is bit k of a ^ b ^ sum; the carry out of the
		// top nibble is the carry out of the whole addition.
		auto* cin = irb.CreateXor(irb.CreateXor(a, b), sum);
		auto* co = irb.CreateZExt(irb.CreateICmpULT(sum, a), dt);
		llvm::Value* carries = irb.CreateOr(irb.CreateLShr(cin, 4), irb.CreateShl(co, xlen - 4));
		uint64_t nib = 0;
		for (unsigned k = 0; k < xlen; k += 4) nib |= uint64_t(1) << k;
		auto* noCarry = irb.CreateAnd(irb.CreateNot(carries), c(nib));
		auto* r = irb.CreateMul(noCarry, c(6));
		storeOp(pi->operands[0], r, irb);
		return;
	}
	if (pi->op_count != 2)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	bool toBcd = i->id == PPC_INS_CDTBCD;
	auto* i16 = irb.getInt16Ty();
	std::vector<uint16_t> tab(toBcd ? 1024 : 4096, 0);
	for (unsigned d = 0; d < 1024; ++d)
	{
		unsigned bcd = ppcDpdToBcd(d);
		if (toBcd)
		{
			tab[d] = uint16_t(bcd);
		}
		else
		{
			bool nonCanon = ((d >> 1) & 1) && ((d >> 2) & 1) && ((d >> 3) & 1) && ((d >> 5) & 1)
					&& ((d >> 6) & 1) && ((d >> 8) & 3);
			if (!nonCanon) tab[bcd] = uint16_t(d);
		}
	}
	// A constant vector with a dynamic extractelement rather than a global
	// table: no memory access appears in the lifted code.
	std::vector<llvm::Constant*> elems;
	for (auto v : tab) elems.push_back(llvm::ConstantInt::get(i16, v));
	auto* table = llvm::ConstantVector::get(elems);
	auto look = [&](llvm::Value* idx) {
		return irb.CreateZExt(irb.CreateExtractElement(table, irb.CreateZExt(idx, irb.getInt32Ty())), dt);
	};
	auto* src = irb.CreateZExtOrTrunc(loadOp(pi->operands[1], irb), dt);
	llvm::Value* r = c(0);
	unsigned inBits = toBcd ? 10 : 12, outBits = toBcd ? 12 : 10;
	for (unsigned w = 0; w < xlen; w += 32)
	{
		for (unsigned h = 0; h < 2; ++h)
		{
			auto* part = irb.CreateAnd(irb.CreateLShr(src, w + h * inBits), c((1u << inBits) - 1));
			auto* conv = look(irb.CreateTrunc(part, irb.getIntNTy(inBits)));
			r = irb.CreateOr(r, irb.CreateShl(conv, w + h * outBits));
		}
	}
	storeOp(pi->operands[0], r, irb);
}


/**
 * XER[OV] of an OE=1 form, from its source operands and, for the
 * carry-in forms, XER[CA].
 */
llvm::Value* Capstone2LlvmIrTranslatorPowerpc_impl::overflowOf(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* dt = getDefaultType();
	auto ld = [&](unsigned k, llvm::Type* t) {
		return irb.CreateSExtOrTrunc(loadOp(pi->operands[k], irb), t);
	};
	auto neg = [&](llvm::Value* v) {
		return irb.CreateICmpSLT(v, llvm::ConstantInt::get(v->getType(), 0));
	};
	auto mulOv = [&](llvm::Type* t) {
		auto* w = irb.getIntNTy(2 * t->getIntegerBitWidth());
		auto* p = irb.CreateMul(irb.CreateSExt(ld(1, t), w), irb.CreateSExt(ld(2, t), w));
		return irb.CreateICmpNE(p, irb.CreateSExt(irb.CreateTrunc(p, t), w));
	};
	auto divOv = [&](llvm::Type* t, bool sgn) -> llvm::Value* {
		auto* a = ld(1, t);
		auto* b = ld(2, t);
		llvm::Value* z = irb.CreateICmpEQ(b, llvm::ConstantInt::get(t, 0));
		if (sgn)
		{
			auto bw = t->getIntegerBitWidth();
			auto* mn = irb.CreateICmpEQ(a, llvm::ConstantInt::get(t, llvm::APInt::getSignedMinValue(bw)));
			auto* m1 = irb.CreateICmpEQ(b, llvm::ConstantInt::getSigned(t, -1));
			z = irb.CreateOr(z, irb.CreateAnd(mn, m1));
		}
		return z;
	};
	if (pi->op_count < 2)
	{
		return nullptr;
	}
	auto carryInOv = [&](llvm::Value* x, llvm::Value* y) {
		auto bw = dt->getIntegerBitWidth();
		auto* w = irb.getIntNTy(bw + 1);
		auto* ca = irb.CreateZExt(loadRegister(PPC_REG_CARRY, irb), w);
		auto* sum = irb.CreateAdd(irb.CreateAdd(irb.CreateSExt(x, w), irb.CreateSExt(y, w)), ca);
		return irb.CreateICmpNE(sum, irb.CreateSExt(irb.CreateTrunc(sum, dt), w));
	};
	auto* allOnes = llvm::ConstantInt::getSigned(dt, -1);
	auto* zeroDt = llvm::ConstantInt::get(dt, 0);
	switch (i->id)
	{
		case PPC_INS_ADDEO:
			if (pi->op_count != 3) return nullptr;
			return carryInOv(ld(1, dt), ld(2, dt));
		case PPC_INS_SUBFEO:
			if (pi->op_count != 3) return nullptr;
			return carryInOv(irb.CreateNot(ld(1, dt)), ld(2, dt));
		case PPC_INS_ADDMEO:
			return carryInOv(ld(1, dt), allOnes);
		case PPC_INS_ADDZEO:
			return carryInOv(ld(1, dt), zeroDt);
		case PPC_INS_SUBFMEO:
			return carryInOv(irb.CreateNot(ld(1, dt)), allOnes);
		case PPC_INS_SUBFZEO:
			return carryInOv(irb.CreateNot(ld(1, dt)), zeroDt);
		default:
			break;
	}
	switch (i->id)
	{
		case PPC_INS_ADDO:
		case PPC_INS_ADDCO:
		{
			if (pi->op_count != 3) return nullptr;
			auto* a = ld(1, dt);
			auto* b = ld(2, dt);
			auto* r = irb.CreateAdd(a, b);
			return neg(irb.CreateAnd(irb.CreateXor(a, r), irb.CreateXor(b, r)));
		}
		case PPC_INS_SUBFO:
		case PPC_INS_SUBFCO:
		{
			if (pi->op_count != 3) return nullptr;
			auto* a = ld(1, dt);
			auto* b = ld(2, dt);
			auto* r = irb.CreateSub(b, a);
			return neg(irb.CreateAnd(irb.CreateXor(b, a), irb.CreateXor(b, r)));
		}
		case PPC_INS_NEGO:
		{
			auto bw = dt->getIntegerBitWidth();
			return irb.CreateICmpEQ(ld(1, dt), llvm::ConstantInt::get(dt, llvm::APInt::getSignedMinValue(bw)));
		}
		case PPC_INS_MULLWO: return pi->op_count == 3 ? mulOv(irb.getInt32Ty()) : nullptr;
		case PPC_INS_MULLDO: return pi->op_count == 3 ? mulOv(irb.getInt64Ty()) : nullptr;
		case PPC_INS_DIVWO: return pi->op_count == 3 ? divOv(irb.getInt32Ty(), true) : nullptr;
		case PPC_INS_DIVWUO: return pi->op_count == 3 ? divOv(irb.getInt32Ty(), false) : nullptr;
		case PPC_INS_DIVDO: return pi->op_count == 3 ? divOv(irb.getInt64Ty(), true) : nullptr;
		case PPC_INS_DIVDUO: return pi->op_count == 3 ? divOv(irb.getInt64Ty(), false) : nullptr;
		case PPC_INS_DIVWEO:
		case PPC_INS_DIVWEUO:
		case PPC_INS_DIVDEO:
		case PPC_INS_DIVDEUO:
		{
			// Extended divide: (RA << n) / RB overflows when RB is zero or the
			// quotient does not fit in n bits.
			if (pi->op_count != 3) return nullptr;
			bool dw = i->id == PPC_INS_DIVDEO || i->id == PPC_INS_DIVDEUO;
			bool sg = i->id == PPC_INS_DIVWEO || i->id == PPC_INS_DIVDEO;
			unsigned n = dw ? 64 : 32;
			auto* t = irb.getIntNTy(n);
			auto* w = irb.getIntNTy(2 * n);
			auto* a = irb.CreateTrunc(irb.CreateZExtOrTrunc(loadOp(pi->operands[1], irb), irb.getInt64Ty()), t);
			auto* b = irb.CreateTrunc(irb.CreateZExtOrTrunc(loadOp(pi->operands[2], irb), irb.getInt64Ty()), t);
			auto* zero = irb.CreateICmpEQ(b, llvm::ConstantInt::get(t, 0));
			auto* bs = irb.CreateSelect(zero, llvm::ConstantInt::get(t, 1), b);
			auto* dvd = irb.CreateShl(sg ? irb.CreateSExt(a, w) : irb.CreateZExt(a, w), n);
			auto* q = sg ? irb.CreateSDiv(dvd, irb.CreateSExt(bs, w)) : irb.CreateUDiv(dvd, irb.CreateZExt(bs, w));
			auto* qt = irb.CreateTrunc(q, t);
			auto* fits = irb.CreateICmpEQ(q, sg ? irb.CreateSExt(qt, w) : irb.CreateZExt(qt, w));
			return irb.CreateOr(zero, irb.CreateNot(fits));
		}
		default:
			return nullptr;
	}
}

/**
 * Power ISA 2.06-3.1 integer additions: MODSW/MODUW/MODSD/MODUD,
 * CNTTZW/CNTTZD, BRH/BRW/BRD, MADDLD/MADDHD/MADDHDU, DIVWE/DIVWEU and
 * EXTSWSLI.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translatePpcMisc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto ld = [&](unsigned k) { return loadOp(pi->operands[k], irb); };
	llvm::Value* val = nullptr;
	switch (i->id)
	{
		case PPC_INS_MODSW:
		case PPC_INS_MODUW:
		{
			auto* a = irb.CreateTrunc(ld(1), i32);
			auto* b = irb.CreateTrunc(ld(2), i32);
			// Division by zero is undefined in the ISA; avoid UB in the IR.
			auto* z = irb.CreateICmpEQ(b, llvm::ConstantInt::get(i32, 0));
			b = irb.CreateSelect(z, llvm::ConstantInt::get(i32, 1), b);
			val = i->id == PPC_INS_MODSW ? irb.CreateSRem(a, b) : irb.CreateURem(a, b);
			val = i->id == PPC_INS_MODSW ? irb.CreateSExt(val, getDefaultType()) : irb.CreateZExt(val, getDefaultType());
			break;
		}
		case PPC_INS_MODSD:
		case PPC_INS_MODUD:
		{
			auto* a = irb.CreateZExtOrTrunc(ld(1), i64);
			auto* b = irb.CreateZExtOrTrunc(ld(2), i64);
			auto* z = irb.CreateICmpEQ(b, llvm::ConstantInt::get(i64, 0));
			b = irb.CreateSelect(z, llvm::ConstantInt::get(i64, 1), b);
			val = i->id == PPC_INS_MODSD ? irb.CreateSRem(a, b) : irb.CreateURem(a, b);
			break;
		}
		case PPC_INS_CNTTZW:
		case PPC_INS_CNTTZD:
		{
			llvm::Value* a = ld(1);
			a = i->id == PPC_INS_CNTTZW ? irb.CreateTrunc(a, i32) : irb.CreateZExtOrTrunc(a, i64);
			auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::cttz, a->getType());
			val = irb.CreateCall(f, {a, irb.getFalse()});
			break;
		}
		case PPC_INS_BRH:
		case PPC_INS_BRW:
		case PPC_INS_BRD:
		{
			unsigned lane = i->id == PPC_INS_BRH ? 2 : i->id == PPC_INS_BRW ? 4 : 8;
			auto* a = irb.CreateZExtOrTrunc(ld(1), i64);
			llvm::Value* r = llvm::ConstantInt::get(i64, 0);
			for (unsigned b = 0; b < 8; ++b)
			{
				unsigned base = b / lane * lane;
				unsigned dst = base + (lane - 1 - (b - base));
				auto* byte = irb.CreateAnd(irb.CreateLShr(a, 8 * b), llvm::ConstantInt::get(i64, 0xff));
				r = irb.CreateOr(r, irb.CreateShl(byte, 8 * dst));
			}
			val = r;
			break;
		}
		case PPC_INS_MADDLD:
		case PPC_INS_MADDHD:
		case PPC_INS_MADDHDU:
		{
			auto* i128 = irb.getInt128Ty();
			bool u = i->id == PPC_INS_MADDHDU;
			auto ext = [&](llvm::Value* v) {
				v = irb.CreateZExtOrTrunc(v, i64);
				return u ? irb.CreateZExt(v, i128) : irb.CreateSExt(v, i128);
			};
			auto* p = irb.CreateAdd(irb.CreateMul(ext(ld(1)), ext(ld(2))), ext(ld(3)));
			val = i->id == PPC_INS_MADDLD ? irb.CreateTrunc(p, i64)
					: irb.CreateTrunc(irb.CreateLShr(p, 64), i64);
			break;
		}
		case PPC_INS_DIVWE:
		case PPC_INS_DIVWEU:
		{
			bool sg = i->id == PPC_INS_DIVWE;
			auto* a = irb.CreateShl(irb.CreateZExt(irb.CreateTrunc(ld(1), i32), i64), 32);
			auto* b32 = irb.CreateTrunc(ld(2), i32);
			auto* b = sg ? irb.CreateSExt(b32, i64) : irb.CreateZExt(b32, i64);
			auto* z = irb.CreateICmpEQ(b, llvm::ConstantInt::get(i64, 0));
			b = irb.CreateSelect(z, llvm::ConstantInt::get(i64, 1), b);
			auto* q = sg ? irb.CreateSDiv(a, b) : irb.CreateUDiv(a, b);
			val = irb.CreateZExtOrTrunc(irb.CreateTrunc(q, i32), getDefaultType());
			break;
		}
		case PPC_INS_EXTSWSLI:
		{
			auto* a = irb.CreateSExt(irb.CreateTrunc(ld(1), i32), i64);
			val = irb.CreateShl(a, llvm::ConstantInt::get(i64, pi->operands[2].imm & 63));
			break;
		}
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
	storeOp(pi->operands[0], val, irb);
	storeCr0(irb, pi, val);
}


/**
 * Instructions decoded from the raw big-endian encoding: Power10 prefixed
 * D-form loads/stores and PADDI (incl. the PC-relative R=1 forms),
 * SETB/SETBC/SETBCR/SETNBC/SETNBCR, POPCNTB, BPERMD and the Power10
 * bit-gather/scatter group PDEPD/PEXTD/CFUGED/CNTLZDM/CNTTZDM.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translatePpcRaw(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto be32 = [&](unsigned o) {
		return (uint32_t(i->bytes[o]) << 24) | (uint32_t(i->bytes[o + 1]) << 16)
				| (uint32_t(i->bytes[o + 2]) << 8) | uint32_t(i->bytes[o + 3]);
	};
	auto* i64 = irb.getInt64Ty();
	auto* dt = getDefaultType();
	auto c64 = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };
	auto gpr = [&](unsigned n) { return PPC_REG_R0 + n; };
	auto ldg = [&](unsigned n) { return irb.CreateZExtOrTrunc(loadRegister(gpr(n), irb), i64); };
	auto stg = [&](unsigned n, llvm::Value* v) { storeRegister(gpr(n), irb.CreateZExtOrTrunc(v, dt), irb); };
	uint32_t w = be32(0);

	switch (i->id)
	{
		case PPC_INS_PLBZ: case PPC_INS_PLHZ: case PPC_INS_PLHA: case PPC_INS_PLWZ:
		case PPC_INS_PLWA: case PPC_INS_PLD: case PPC_INS_PSTB: case PPC_INS_PSTH:
		case PPC_INS_PSTW: case PPC_INS_PSTD: case PPC_INS_PADDI:
		case PPC_INS_PLI: case PPC_INS_PLA: case PPC_INS_PSUBI:
		{
			if (i->size < 8) break;
			uint32_t pfx = w, sfx = be32(4);
			bool r = (pfx >> 20) & 1;
			int64_t d = (int64_t(pfx & 0x3ffff) << 16) | (sfx & 0xffff);
			d = (d << 30) >> 30;
			unsigned rt = (sfx >> 21) & 31, ra = (sfx >> 16) & 31;
			llvm::Value* ea = r ? c64(i->address + d)
					: irb.CreateAdd(ra ? ldg(ra) : c64(0), c64(d));
			if (i->id == PPC_INS_PADDI || i->id == PPC_INS_PLI || i->id == PPC_INS_PLA || i->id == PPC_INS_PSUBI)
			{
				stg(rt, ea);
				return;
			}
			llvm::Type* ty = irb.getInt8Ty();
			switch (i->id)
			{
				case PPC_INS_PLHZ: case PPC_INS_PLHA: case PPC_INS_PSTH: ty = irb.getInt16Ty(); break;
				case PPC_INS_PLWZ: case PPC_INS_PLWA: case PPC_INS_PSTW: ty = irb.getInt32Ty(); break;
				case PPC_INS_PLD: case PPC_INS_PSTD: ty = i64; break;
				default: break;
			}
			ea = irb.CreateZExtOrTrunc(ea, dt);
			switch (i->id)
			{
				case PPC_INS_PSTB: case PPC_INS_PSTH: case PPC_INS_PSTW: case PPC_INS_PSTD:
					storeIntPtr(irb, irb.CreateTrunc(ldg(rt), ty), ea, ty);
					return;
				default:
				{
					llvm::Value* l = loadIntPtr(irb, ea, ty);
					bool sx = i->id == PPC_INS_PLHA || i->id == PPC_INS_PLWA;
					l = sx ? irb.CreateSExt(l, i64) : irb.CreateZExtOrTrunc(l, i64);
					stg(rt, l);
					return;
				}
			}
		}
		case PPC_INS_CMPEQB:
		case PPC_INS_CMPRB:
		{
			// Byte-in-set / byte-in-range tests; only the GT bit of BF is set.
			static const uint32_t crb[8][4] = {
				{PPC_REG_CR0LT, PPC_REG_CR0GT, PPC_REG_CR0EQ, PPC_REG_CR0UN},
				{PPC_REG_CR1LT, PPC_REG_CR1GT, PPC_REG_CR1EQ, PPC_REG_CR1UN},
				{PPC_REG_CR2LT, PPC_REG_CR2GT, PPC_REG_CR2EQ, PPC_REG_CR2UN},
				{PPC_REG_CR3LT, PPC_REG_CR3GT, PPC_REG_CR3EQ, PPC_REG_CR3UN},
				{PPC_REG_CR4LT, PPC_REG_CR4GT, PPC_REG_CR4EQ, PPC_REG_CR4UN},
				{PPC_REG_CR5LT, PPC_REG_CR5GT, PPC_REG_CR5EQ, PPC_REG_CR5UN},
				{PPC_REG_CR6LT, PPC_REG_CR6GT, PPC_REG_CR6EQ, PPC_REG_CR6UN},
				{PPC_REG_CR7LT, PPC_REG_CR7GT, PPC_REG_CR7EQ, PPC_REG_CR7UN},
			};
			unsigned bf = (w >> 23) & 7, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
			auto* i8 = irb.getInt8Ty();
			auto* x = irb.CreateTrunc(ldg(ra), i8);
			auto* b = ldg(rb);
			auto by = [&](unsigned k) { return irb.CreateTrunc(irb.CreateLShr(b, 8 * k), i8); };
			llvm::Value* r = irb.getFalse();
			if (i->id == PPC_INS_CMPEQB)
			{
				for (unsigned k = 0; k < 8; ++k) r = irb.CreateOr(r, irb.CreateICmpEQ(x, by(k)));
			}
			else
			{
				auto inr = [&](unsigned lo, unsigned hi) {
					return irb.CreateAnd(irb.CreateICmpULE(by(lo), x), irb.CreateICmpULE(x, by(hi)));
				};
				r = inr(1, 0);
				if ((w >> 21) & 1) r = irb.CreateOr(r, inr(3, 2));
			}
			storeRegister(crb[bf][0], irb.getFalse(), irb);
			storeRegister(crb[bf][1], r, irb);
			storeRegister(crb[bf][2], irb.getFalse(), irb);
			storeRegister(crb[bf][3], irb.getFalse(), irb);
			return;
		}
		case PPC_INS_ADDPCIS:
		{
			// rt = next instruction address + (d << 16), d split over three fields.
			int64_t d = int64_t(((w >> 6) & 0x3ff) << 6 | ((w >> 16) & 31) << 1 | (w & 1));
			if (d & 0x8000) d -= 0x10000;
			stg((w >> 21) & 31, c64(uint64_t(i->address + 4 + (d << 16))));
			return;
		}
		case PPC_INS_SETB:
		case PPC_INS_SETBC: case PPC_INS_SETBCR: case PPC_INS_SETNBC: case PPC_INS_SETNBCR:
		{
			static const uint32_t bits[8][4] = {
				{PPC_REG_CR0LT, PPC_REG_CR0GT, PPC_REG_CR0EQ, PPC_REG_CR0UN},
				{PPC_REG_CR1LT, PPC_REG_CR1GT, PPC_REG_CR1EQ, PPC_REG_CR1UN},
				{PPC_REG_CR2LT, PPC_REG_CR2GT, PPC_REG_CR2EQ, PPC_REG_CR2UN},
				{PPC_REG_CR3LT, PPC_REG_CR3GT, PPC_REG_CR3EQ, PPC_REG_CR3UN},
				{PPC_REG_CR4LT, PPC_REG_CR4GT, PPC_REG_CR4EQ, PPC_REG_CR4UN},
				{PPC_REG_CR5LT, PPC_REG_CR5GT, PPC_REG_CR5EQ, PPC_REG_CR5UN},
				{PPC_REG_CR6LT, PPC_REG_CR6GT, PPC_REG_CR6EQ, PPC_REG_CR6UN},
				{PPC_REG_CR7LT, PPC_REG_CR7GT, PPC_REG_CR7EQ, PPC_REG_CR7UN},
			};
			unsigned rt = (w >> 21) & 31;
			auto bit = [&](uint32_t r) { return irb.CreateTrunc(loadRegister(r, irb), irb.getInt1Ty()); };
			if (i->id == PPC_INS_SETB)
			{
				unsigned bf = (w >> 18) & 7;
				auto* v = irb.CreateSelect(bit(bits[bf][1]), c64(1), c64(0));
				v = irb.CreateSelect(bit(bits[bf][0]), c64(~0ull), v);
				stg(rt, v);
				return;
			}
			unsigned bi = (w >> 16) & 31;
			auto* b = bit(bits[bi >> 2][bi & 3]);
			uint64_t yes = (i->id == PPC_INS_SETNBC || i->id == PPC_INS_SETNBCR) ? ~0ull : 1;
			bool rev = i->id == PPC_INS_SETBCR || i->id == PPC_INS_SETNBCR;
			stg(rt, rev ? irb.CreateSelect(b, c64(0), c64(yes)) : irb.CreateSelect(b, c64(yes), c64(0)));
			return;
		}
		default:
			break;
	}

	// X-form RS, RA, RB (RA is the target).
	unsigned rs = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
	auto pext = [&](llvm::Value* src, llvm::Value* m) {
		llvm::Value* res = c64(0);
		llvm::Value* k = c64(0);
		for (unsigned j = 0; j < 64; ++j)
		{
			auto* mb = irb.CreateAnd(irb.CreateLShr(m, j), c64(1));
			auto* sb = irb.CreateAnd(irb.CreateLShr(src, j), c64(1));
			res = irb.CreateOr(res, irb.CreateShl(irb.CreateAnd(sb, mb), k));
			k = irb.CreateAdd(k, mb);
		}
		return res;
	};
	auto ctpop = [&](llvm::Value* v) {
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctpop, i64);
		return irb.CreateCall(f, {v});
	};
	llvm::Value* val = nullptr;
	switch (i->id)
	{
		case PPC_INS_POPCNTB:
		{
			auto* x = ldg(rs);
			x = irb.CreateSub(x, irb.CreateAnd(irb.CreateLShr(x, 1), c64(0x5555555555555555ull)));
			x = irb.CreateAdd(irb.CreateAnd(x, c64(0x3333333333333333ull)),
					irb.CreateAnd(irb.CreateLShr(x, 2), c64(0x3333333333333333ull)));
			val = irb.CreateAnd(irb.CreateAdd(x, irb.CreateLShr(x, 4)), c64(0x0f0f0f0f0f0f0f0full));
			break;
		}
		case PPC_INS_BPERMD:
		{
			auto* s = ldg(rs);
			auto* b = ldg(rb);
			val = c64(0);
			for (unsigned k = 0; k < 8; ++k)
			{
				auto* idx = irb.CreateAnd(irb.CreateLShr(s, 56 - 8 * k), c64(0xff));
				auto* ok = irb.CreateICmpULT(idx, c64(64));
				auto* sh = irb.CreateSelect(ok, irb.CreateSub(c64(63), idx), c64(0));
				auto* bt = irb.CreateAnd(irb.CreateLShr(b, sh), c64(1));
				bt = irb.CreateSelect(ok, bt, c64(0));
				val = irb.CreateOr(val, irb.CreateShl(bt, 7 - k));
			}
			break;
		}
		case PPC_INS_PDEPD:
		{
			auto* s = ldg(rs);
			auto* m = ldg(rb);
			llvm::Value* res = c64(0);
			llvm::Value* k = c64(0);
			for (unsigned j = 0; j < 64; ++j)
			{
				auto* mb = irb.CreateAnd(irb.CreateLShr(m, j), c64(1));
				auto* sb = irb.CreateAnd(irb.CreateLShr(s, k), c64(1));
				res = irb.CreateOr(res, irb.CreateShl(irb.CreateAnd(sb, mb), j));
				k = irb.CreateAdd(k, mb);
			}
			val = res;
			break;
		}
		case PPC_INS_PEXTD:
			val = pext(ldg(rs), ldg(rb));
			break;
		case PPC_INS_CFUGED:
		{
			auto* s = ldg(rs);
			auto* m = ldg(rb);
			auto* lo = pext(s, m);
			auto* hi = pext(s, irb.CreateNot(m));
			auto* n = ctpop(m);
			auto* full = irb.CreateICmpEQ(n, c64(64));
			auto* hs = irb.CreateShl(hi, irb.CreateSelect(full, c64(0), n));
			val = irb.CreateOr(irb.CreateSelect(full, c64(0), hs), lo);
			break;
		}
		case PPC_INS_CNTLZDM:
		case PPC_INS_CNTTZDM:
		{
			auto* m = ldg(rb);
			auto* e = pext(ldg(rs), m);
			auto* n = ctpop(m);
			if (i->id == PPC_INS_CNTLZDM)
			{
				auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctlz, i64);
				val = irb.CreateSub(irb.CreateCall(f, {e, irb.getFalse()}), irb.CreateSub(c64(64), n));
			}
			else
			{
				auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::cttz, i64);
				auto* t = irb.CreateCall(f, {e, irb.getFalse()});
				val = irb.CreateSelect(irb.CreateICmpULT(t, n), t, n);
			}
			break;
		}
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
	stg(ra, val);
	if (i->id != PPC_INS_PDEPD && i->id != PPC_INS_PEXTD && i->id != PPC_INS_CFUGED
			&& i->id != PPC_INS_CNTLZDM && i->id != PPC_INS_CNTTZDM)
	{
		storeCr0(irb, pi, irb.CreateZExtOrTrunc(val, dt));
	}
}


/**
 * Power ISA 3.0/3.1 VMX/VSX integer additions, lifted lane by lane on the
 * 128-bit register value (lane 0 is the most significant, big-endian).
 * Record forms do not update CR6 (consistent with translateVecCmp).
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecLane(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	auto* i128 = irb.getInt128Ty();
	auto c128 = [&](uint64_t v) { return llvm::ConstantInt::get(i128, v); };
	unsigned id = i->id;
	bool unary = false;
	unsigned w = 32;
	enum K { ABSD, NEG, CTZ, DIVS, DIVU, MODS, MODU, MULL, MULHS, MULHU, EXTS, CMPNE, CMPNEZ,
		MULEO, Q, BR, EXPAND, PRTY, CMPGTQ } k;
	unsigned extFrom = 0;
	bool odd = false, sgn = false;
	switch (id)
	{
		case PPC_INS_VABSDUB: k = ABSD; w = 8; break;
		case PPC_INS_VABSDUH: k = ABSD; w = 16; break;
		case PPC_INS_VABSDUW: k = ABSD; w = 32; break;
		case PPC_INS_VNEGW: k = NEG; unary = true; w = 32; break;
		case PPC_INS_VNEGD: k = NEG; unary = true; w = 64; break;
		case PPC_INS_VCTZB: k = CTZ; unary = true; w = 8; break;
		case PPC_INS_VCTZH: k = CTZ; unary = true; w = 16; break;
		case PPC_INS_VCTZW: k = CTZ; unary = true; w = 32; break;
		case PPC_INS_VCTZD: k = CTZ; unary = true; w = 64; break;
		case PPC_INS_VDIVSW: k = DIVS; w = 32; break;
		case PPC_INS_VDIVUW: k = DIVU; w = 32; break;
		case PPC_INS_VDIVSD: k = DIVS; w = 64; break;
		case PPC_INS_VDIVUD: k = DIVU; w = 64; break;
		case PPC_INS_VDIVSQ: k = DIVS; w = 128; break;
		case PPC_INS_VDIVUQ: k = DIVU; w = 128; break;
		case PPC_INS_VMODSW: k = MODS; w = 32; break;
		case PPC_INS_VMODUW: k = MODU; w = 32; break;
		case PPC_INS_VMODSD: k = MODS; w = 64; break;
		case PPC_INS_VMODUD: k = MODU; w = 64; break;
		case PPC_INS_VMODSQ: k = MODS; w = 128; break;
		case PPC_INS_VMODUQ: k = MODU; w = 128; break;
		case PPC_INS_VMULLD: k = MULL; w = 64; break;
		case PPC_INS_VMULHSW: k = MULHS; w = 32; break;
		case PPC_INS_VMULHUW: k = MULHU; w = 32; break;
		case PPC_INS_VMULHSD: k = MULHS; w = 64; break;
		case PPC_INS_VMULHUD: k = MULHU; w = 64; break;
		case PPC_INS_VEXTSB2W: k = EXTS; unary = true; w = 32; extFrom = 8; break;
		case PPC_INS_VEXTSH2W: k = EXTS; unary = true; w = 32; extFrom = 16; break;
		case PPC_INS_VEXTSB2D: k = EXTS; unary = true; w = 64; extFrom = 8; break;
		case PPC_INS_VEXTSH2D: k = EXTS; unary = true; w = 64; extFrom = 16; break;
		case PPC_INS_VEXTSW2D: k = EXTS; unary = true; w = 64; extFrom = 32; break;
		case PPC_INS_VEXTSD2Q: k = EXTS; unary = true; w = 128; extFrom = 64; break;
		case PPC_INS_VCMPNEB: k = CMPNE; w = 8; break;
		case PPC_INS_VCMPNEH: k = CMPNE; w = 16; break;
		case PPC_INS_VCMPNEW: k = CMPNE; w = 32; break;
		case PPC_INS_VCMPNEZB: k = CMPNEZ; w = 8; break;
		case PPC_INS_VCMPNEZH: k = CMPNEZ; w = 16; break;
		case PPC_INS_VCMPNEZW: k = CMPNEZ; w = 32; break;
		case PPC_INS_VCMPGTSQ: k = CMPGTQ; w = 128; sgn = true; break;
		case PPC_INS_VCMPGTUQ: k = CMPGTQ; w = 128; break;
		case PPC_INS_VMULESD: k = MULEO; sgn = true; break;
		case PPC_INS_VMULEUD: k = MULEO; break;
		case PPC_INS_VMULOSD: k = MULEO; sgn = true; odd = true; break;
		case PPC_INS_VMULOUD: k = MULEO; odd = true; break;
		case PPC_INS_VADDUQM: case PPC_INS_VSUBUQM: case PPC_INS_VADDCUQ: case PPC_INS_VSUBCUQ:
		case PPC_INS_VADDEUQM: case PPC_INS_VSUBEUQM: case PPC_INS_VADDECUQ: case PPC_INS_VSUBECUQ:
		case PPC_INS_VRLQ: case PPC_INS_VSLQ: case PPC_INS_VSRQ: case PPC_INS_VSRAQ:
			k = Q; w = 128; break;
		case PPC_INS_XXBRH: k = BR; unary = true; w = 16; break;
		case PPC_INS_XXBRW: k = BR; unary = true; w = 32; break;
		case PPC_INS_XXBRD: k = BR; unary = true; w = 64; break;
		case PPC_INS_XXBRQ: k = BR; unary = true; w = 128; break;
		case PPC_INS_VEXPANDBM: k = EXPAND; unary = true; w = 8; break;
		case PPC_INS_VEXPANDHM: k = EXPAND; unary = true; w = 16; break;
		case PPC_INS_VEXPANDWM: k = EXPAND; unary = true; w = 32; break;
		case PPC_INS_VEXPANDDM: k = EXPAND; unary = true; w = 64; break;
		case PPC_INS_VEXPANDQM: k = EXPAND; unary = true; w = 128; break;
		case PPC_INS_VPRTYBW: k = PRTY; unary = true; w = 32; break;
		case PPC_INS_VPRTYBD: k = PRTY; unary = true; w = 64; break;
		case PPC_INS_VPRTYBQ: k = PRTY; unary = true; w = 128; break;
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
	if (!unary && pi->op_count < 3)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	auto* a = loadOpI128(pi->operands[1], irb);
	llvm::Value* b = unary ? nullptr : loadOpI128(pi->operands[2], irb);
	llvm::Value* res = nullptr;

	if (k == Q)
	{
		auto* one = c128(1);
		llvm::Value* c = (pi->op_count >= 4 && pi->operands[3].type == PPC_OP_REG)
				? irb.CreateAnd(loadOpI128(pi->operands[3], irb), one) : c128(0);
		auto* nb = irb.CreateNot(b);
		auto* sh = irb.CreateAnd(irb.CreateLShr(b, 64), c128(127));
		auto carry = [&](llvm::Value* x, llvm::Value* y, llvm::Value* ci) {
			auto* s1 = irb.CreateAdd(x, y);
			auto* c1 = irb.CreateICmpULT(s1, x);
			auto* s2 = irb.CreateAdd(s1, ci);
			auto* c2 = irb.CreateICmpULT(s2, s1);
			return irb.CreateZExt(irb.CreateOr(c1, c2), i128);
		};
		switch (id)
		{
			case PPC_INS_VADDUQM: res = irb.CreateAdd(a, b); break;
			case PPC_INS_VSUBUQM: res = irb.CreateSub(a, b); break;
			case PPC_INS_VADDCUQ: res = carry(a, b, c128(0)); break;
			case PPC_INS_VSUBCUQ: res = carry(a, nb, one); break;
			case PPC_INS_VADDEUQM: res = irb.CreateAdd(irb.CreateAdd(a, b), c); break;
			case PPC_INS_VSUBEUQM: res = irb.CreateAdd(irb.CreateAdd(a, nb), c); break;
			case PPC_INS_VADDECUQ: res = carry(a, b, c); break;
			case PPC_INS_VSUBECUQ: res = carry(a, nb, c); break;
			case PPC_INS_VSLQ: res = irb.CreateShl(a, sh); break;
			case PPC_INS_VSRQ: res = irb.CreateLShr(a, sh); break;
			case PPC_INS_VSRAQ: res = irb.CreateAShr(a, sh); break;
			case PPC_INS_VRLQ:
			{
				auto* rsh = irb.CreateAnd(irb.CreateSub(c128(128), sh), c128(127));
				res = irb.CreateOr(irb.CreateShl(a, sh), irb.CreateLShr(a, rsh));
				break;
			}
			default: break;
		}
		storeOpI128(pi->operands[0], res, irb);
		return;
	}
	if (k == MULEO)
	{
		auto* i64 = irb.getInt64Ty();
		unsigned s = odd ? 0 : 64;
		auto ext = [&](llvm::Value* v) {
			auto* t = irb.CreateTrunc(irb.CreateLShr(v, s), i64);
			return sgn ? irb.CreateSExt(t, i128) : irb.CreateZExt(t, i128);
		};
		storeOpI128(pi->operands[0], irb.CreateMul(ext(a), ext(b)), irb);
		return;
	}

	auto* lt = irb.getIntNTy(w);
	auto* wt = irb.getIntNTy(2 * w);
	auto cl = [&](uint64_t v) { return llvm::ConstantInt::get(lt, v); };
	unsigned n = 128 / w;
	res = c128(0);
	for (unsigned l = 0; l < n; ++l)
	{
		unsigned sh = (n - 1 - l) * w;
		auto lane = [&](llvm::Value* v) { return irb.CreateTrunc(irb.CreateLShr(v, sh), lt); };
		auto* x = lane(a);
		llvm::Value* y = b ? lane(b) : nullptr;
		llvm::Value* r = nullptr;
		switch (k)
		{
			case ABSD:
				r = irb.CreateSelect(irb.CreateICmpUGT(x, y), irb.CreateSub(x, y), irb.CreateSub(y, x));
				break;
			case NEG: r = irb.CreateNeg(x); break;
			case CTZ:
			{
				auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::cttz, lt);
				r = irb.CreateCall(f, {x, irb.getFalse()});
				break;
			}
			case DIVS: case DIVU: case MODS: case MODU:
			{
				auto* z = irb.CreateICmpEQ(y, cl(0));
				auto* d = irb.CreateSelect(z, cl(1), y);
				if (k == DIVS || k == MODS)
				{
					// Avoid the INT_MIN / -1 overflow trap; the ISA leaves it undefined.
					auto* ovf = irb.CreateAnd(
							irb.CreateICmpEQ(x, llvm::ConstantInt::get(lt, llvm::APInt::getSignedMinValue(w))),
							irb.CreateICmpEQ(y, llvm::ConstantInt::get(lt, llvm::APInt::getAllOnes(w))));
					d = irb.CreateSelect(ovf, cl(1), d);
				}
				r = k == DIVS ? irb.CreateSDiv(x, d) : k == DIVU ? irb.CreateUDiv(x, d)
						: k == MODS ? irb.CreateSRem(x, d) : irb.CreateURem(x, d);
				break;
			}
			case MULL: r = irb.CreateMul(x, y); break;
			case MULHS: case MULHU:
			{
				auto e = [&](llvm::Value* v) { return k == MULHS ? irb.CreateSExt(v, wt) : irb.CreateZExt(v, wt); };
				r = irb.CreateTrunc(irb.CreateLShr(irb.CreateMul(e(x), e(y)), w), lt);
				break;
			}
			case EXTS: r = irb.CreateSExt(irb.CreateTrunc(x, irb.getIntNTy(extFrom)), lt); break;
			case CMPNE:
				r = irb.CreateSExt(irb.CreateICmpNE(x, y), lt);
				break;
			case CMPNEZ:
				r = irb.CreateSExt(irb.CreateOr(irb.CreateICmpNE(x, y),
						irb.CreateOr(irb.CreateICmpEQ(x, cl(0)), irb.CreateICmpEQ(y, cl(0)))), lt);
				break;
			case CMPGTQ:
				r = irb.CreateSExt(sgn ? irb.CreateICmpSGT(x, y) : irb.CreateICmpUGT(x, y), lt);
				break;
			case BR:
			{
				r = cl(0);
				for (unsigned bi = 0; bi < w / 8; ++bi)
				{
					auto* by = irb.CreateAnd(irb.CreateLShr(x, 8 * bi), cl(0xff));
					r = irb.CreateOr(r, irb.CreateShl(by, w - 8 - 8 * bi));
				}
				break;
			}
			case EXPAND: r = irb.CreateAShr(x, w - 1); break;
			case PRTY:
			{
				r = cl(0);
				for (unsigned bi = 0; bi < w / 8; ++bi)
					r = irb.CreateXor(r, irb.CreateAnd(irb.CreateLShr(x, 8 * bi), cl(1)));
				break;
			}
			default: break;
		}
		res = irb.CreateOr(res, irb.CreateShl(irb.CreateZExt(r, i128), sh));
	}
	storeOpI128(pi->operands[0], res, irb);
}


/**
 * TW/TWI/TD/TDI/TRAP: the trap is a call of a pseudo function guarded by the
 * TO-encoded comparison (decoded from the raw word, since Capstone folds the
 * TO field into extended mnemonics).
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateTrap(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	uint32_t w = (uint32_t(i->bytes[0]) << 24) | (uint32_t(i->bytes[1]) << 16)
			| (uint32_t(i->bytes[2]) << 8) | uint32_t(i->bytes[3]);
	unsigned op = w >> 26;
	unsigned to = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
	bool imm = op == 2 || op == 3;
	bool word = op == 3 || (op == 31 && ((w >> 1) & 0x3ff) == 4);
	auto* ty = word ? irb.getInt32Ty() : irb.getInt64Ty();
	auto ld = [&](unsigned r) {
		return irb.CreateZExtOrTrunc(loadRegister(PPC_REG_R0 + r, irb), ty);
	};
	auto* a = ld(ra);
	llvm::Value* b = imm ? llvm::ConstantInt::getSigned(ty, int16_t(w & 0xffff)) : ld(rb);
	llvm::Value* cond = irb.getFalse();
	if (to & 16) cond = irb.CreateOr(cond, irb.CreateICmpSLT(a, b));
	if (to & 8) cond = irb.CreateOr(cond, irb.CreateICmpSGT(a, b));
	if (to & 4) cond = irb.CreateOr(cond, irb.CreateICmpEQ(a, b));
	if (to & 2) cond = irb.CreateOr(cond, irb.CreateICmpULT(a, b));
	if (to & 1) cond = irb.CreateOr(cond, irb.CreateICmpUGT(a, b));
	(void)pi;
	llvm::IRBuilder<> body(generateIfThen(cond, irb));
	auto* fnc = getPseudoAsmFunction(i, irb.getVoidTy(), llvm::ArrayRef<llvm::Type*>{});
	body.CreateCall(fnc);
}


void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeFloat(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	// e500 SPE scalar single precision: the float lives in the low word of a GPR.
	uint32_t w = (uint32_t(i->bytes[0]) << 24) | (uint32_t(i->bytes[1]) << 16)
			| (uint32_t(i->bytes[2]) << 8) | uint32_t(i->bytes[3]);
	unsigned rd = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
	auto* i32 = irb.getInt32Ty();
	auto* ft = irb.getFloatTy();
	auto* dt = getDefaultType();
	auto gpr = [](unsigned n) { return PPC_REG_R0 + n; };
	auto ldw = [&](unsigned n) { return irb.CreateTrunc(irb.CreateZExtOrTrunc(loadRegister(gpr(n), irb), irb.getInt64Ty()), i32); };
	auto ldf = [&](unsigned n) { return irb.CreateBitCast(ldw(n), ft); };
	auto stw = [&](llvm::Value* v) { storeRegister(gpr(rd), irb.CreateZExtOrTrunc(v, dt), irb); };
	auto stf = [&](llvm::Value* v) { stw(irb.CreateBitCast(v, i32)); };
	auto* sign = llvm::ConstantInt::get(i32, 0x80000000u);
	switch (i->id)
	{
		case PPC_INS_EFSADD: stf(irb.CreateFAdd(ldf(ra), ldf(rb))); return;
		case PPC_INS_EFSSUB: stf(irb.CreateFSub(ldf(ra), ldf(rb))); return;
		case PPC_INS_EFSMUL: stf(irb.CreateFMul(ldf(ra), ldf(rb))); return;
		case PPC_INS_EFSDIV: stf(irb.CreateFDiv(ldf(ra), ldf(rb))); return;
		case PPC_INS_EFSABS: stw(irb.CreateAnd(ldw(ra), llvm::ConstantInt::get(i32, 0x7fffffffu))); return;
		case PPC_INS_EFSNABS: stw(irb.CreateOr(ldw(ra), sign)); return;
		case PPC_INS_EFSNEG: stw(irb.CreateXor(ldw(ra), sign)); return;
		case PPC_INS_EFSCFSI: stf(irb.CreateSIToFP(ldw(rb), ft)); return;
		case PPC_INS_EFSCFUI: stf(irb.CreateUIToFP(ldw(rb), ft)); return;
		// Rounding mode is not modelled; the non-Z forms truncate too.
		case PPC_INS_EFSCTSI: case PPC_INS_EFSCTSIZ: stw(irb.CreateFPToSI(ldf(rb), i32)); return;
		case PPC_INS_EFSCTUI: case PPC_INS_EFSCTUIZ: stw(irb.CreateFPToUI(ldf(rb), i32)); return;
		case PPC_INS_EFSCMPEQ: case PPC_INS_EFSTSTEQ:
		case PPC_INS_EFSCMPGT: case PPC_INS_EFSTSTGT:
		case PPC_INS_EFSCMPLT: case PPC_INS_EFSTSTLT:
		{
			static const uint32_t crb[8][4] = {
				{PPC_REG_CR0LT, PPC_REG_CR0GT, PPC_REG_CR0EQ, PPC_REG_CR0UN},
				{PPC_REG_CR1LT, PPC_REG_CR1GT, PPC_REG_CR1EQ, PPC_REG_CR1UN},
				{PPC_REG_CR2LT, PPC_REG_CR2GT, PPC_REG_CR2EQ, PPC_REG_CR2UN},
				{PPC_REG_CR3LT, PPC_REG_CR3GT, PPC_REG_CR3EQ, PPC_REG_CR3UN},
				{PPC_REG_CR4LT, PPC_REG_CR4GT, PPC_REG_CR4EQ, PPC_REG_CR4UN},
				{PPC_REG_CR5LT, PPC_REG_CR5GT, PPC_REG_CR5EQ, PPC_REG_CR5UN},
				{PPC_REG_CR6LT, PPC_REG_CR6GT, PPC_REG_CR6EQ, PPC_REG_CR6UN},
				{PPC_REG_CR7LT, PPC_REG_CR7GT, PPC_REG_CR7EQ, PPC_REG_CR7UN},
			};
			auto* a = ldf(ra);
			auto* b = ldf(rb);
			llvm::Value* r = nullptr;
			if (i->id == PPC_INS_EFSCMPEQ || i->id == PPC_INS_EFSTSTEQ) r = irb.CreateFCmpOEQ(a, b);
			else if (i->id == PPC_INS_EFSCMPGT || i->id == PPC_INS_EFSTSTGT) r = irb.CreateFCmpOGT(a, b);
			else r = irb.CreateFCmpOLT(a, b);
			// The result goes to the GT bit; the other bits are undefined, cleared here.
			unsigned bf = (w >> 23) & 7;
			storeRegister(crb[bf][0], irb.getFalse(), irb);
			storeRegister(crb[bf][1], r, irb);
			storeRegister(crb[bf][2], irb.getFalse(), irb);
			storeRegister(crb[bf][3], irb.getFalse(), irb);
			return;
		}
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
}


void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeDouble(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	// e500v2 SPE scalar double precision: the double fills the whole 64-bit GPR.
	// Rounding mode and SPEFSCR flags are not modelled.
	uint32_t w = (uint32_t(i->bytes[0]) << 24) | (uint32_t(i->bytes[1]) << 16)
			| (uint32_t(i->bytes[2]) << 8) | uint32_t(i->bytes[3]);
	unsigned rd = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
	auto* i64 = irb.getInt64Ty();
	auto* i32 = irb.getInt32Ty();
	auto* dt = irb.getDoubleTy();
	auto opOf = [](unsigned n) {
		cs_ppc_op o{};
		o.type = PPC_OP_REG;
		o.reg = ppc_reg(PPC_REG_R0 + n);
		return o;
	};
	auto ld = [&](unsigned n) { auto o = opOf(n); return loadSpeI64(o, irb); };
	auto ldd = [&](unsigned n) { return irb.CreateBitCast(ld(n), dt); };
	auto ldw = [&](unsigned n) { return irb.CreateTrunc(ld(n), i32); };
	auto st = [&](llvm::Value* v) { auto o = opOf(rd); storeSpeI64(o, irb.CreateZExtOrTrunc(v, i64), irb); };
	auto std_ = [&](llvm::Value* v) { st(irb.CreateBitCast(v, i64)); };
	auto stw = [&](llvm::Value* v) {
		// 32-bit results replace only the low word.
		auto* hi = irb.CreateAnd(ld(rd), llvm::ConstantInt::get(i64, 0xffffffff00000000ull));
		st(irb.CreateOr(hi, irb.CreateZExt(v, i64)));
	};
	auto* sign = llvm::ConstantInt::get(i64, 0x8000000000000000ull);
	switch (i->id)
	{
		case PPC_INS_EFDADD: std_(irb.CreateFAdd(ldd(ra), ldd(rb))); return;
		case PPC_INS_EFDSUB: std_(irb.CreateFSub(ldd(ra), ldd(rb))); return;
		case PPC_INS_EFDMUL: std_(irb.CreateFMul(ldd(ra), ldd(rb))); return;
		case PPC_INS_EFDDIV: std_(irb.CreateFDiv(ldd(ra), ldd(rb))); return;
		case PPC_INS_EFDABS: st(irb.CreateAnd(ld(ra), llvm::ConstantInt::get(i64, 0x7fffffffffffffffull))); return;
		case PPC_INS_EFDNABS: st(irb.CreateOr(ld(ra), sign)); return;
		case PPC_INS_EFDNEG: st(irb.CreateXor(ld(ra), sign)); return;
		case PPC_INS_EFDCFS: std_(irb.CreateFPExt(irb.CreateBitCast(ldw(rb), irb.getFloatTy()), dt)); return;
		case PPC_INS_EFDCFSI: std_(irb.CreateSIToFP(ldw(rb), dt)); return;
		case PPC_INS_EFDCFUI: std_(irb.CreateUIToFP(ldw(rb), dt)); return;
		case PPC_INS_EFDCFSID: std_(irb.CreateSIToFP(ld(rb), dt)); return;
		case PPC_INS_EFDCFUID: std_(irb.CreateUIToFP(ld(rb), dt)); return;
		case PPC_INS_EFDCTSI: case PPC_INS_EFDCTSIZ: stw(irb.CreateFPToSI(ldd(rb), i32)); return;
		case PPC_INS_EFDCTUI: case PPC_INS_EFDCTUIZ: stw(irb.CreateFPToUI(ldd(rb), i32)); return;
		case PPC_INS_EFDCTSIDZ: st(irb.CreateFPToSI(ldd(rb), i64)); return;
		case PPC_INS_EFDCTUIDZ: st(irb.CreateFPToUI(ldd(rb), i64)); return;
		case PPC_INS_EFDCMPEQ: case PPC_INS_EFDTSTEQ:
		case PPC_INS_EFDCMPGT: case PPC_INS_EFDTSTGT:
		case PPC_INS_EFDCMPLT: case PPC_INS_EFDTSTLT:
		{
			auto* a = ldd(ra);
			auto* b = ldd(rb);
			llvm::Value* r = nullptr;
			if (i->id == PPC_INS_EFDCMPEQ || i->id == PPC_INS_EFDTSTEQ) r = irb.CreateFCmpOEQ(a, b);
			else if (i->id == PPC_INS_EFDCMPGT || i->id == PPC_INS_EFDTSTGT) r = irb.CreateFCmpOGT(a, b);
			else r = irb.CreateFCmpOLT(a, b);
			unsigned bf = (w >> 23) & 7;
			static const uint32_t gt[8] = {PPC_REG_CR0GT, PPC_REG_CR1GT, PPC_REG_CR2GT, PPC_REG_CR3GT,
					PPC_REG_CR4GT, PPC_REG_CR5GT, PPC_REG_CR6GT, PPC_REG_CR7GT};
			static const uint32_t lt[8] = {PPC_REG_CR0LT, PPC_REG_CR1LT, PPC_REG_CR2LT, PPC_REG_CR3LT,
					PPC_REG_CR4LT, PPC_REG_CR5LT, PPC_REG_CR6LT, PPC_REG_CR7LT};
			static const uint32_t eq[8] = {PPC_REG_CR0EQ, PPC_REG_CR1EQ, PPC_REG_CR2EQ, PPC_REG_CR3EQ,
					PPC_REG_CR4EQ, PPC_REG_CR5EQ, PPC_REG_CR6EQ, PPC_REG_CR7EQ};
			static const uint32_t un[8] = {PPC_REG_CR0UN, PPC_REG_CR1UN, PPC_REG_CR2UN, PPC_REG_CR3UN,
					PPC_REG_CR4UN, PPC_REG_CR5UN, PPC_REG_CR6UN, PPC_REG_CR7UN};
			storeRegister(lt[bf], irb.getFalse(), irb);
			storeRegister(gt[bf], r, irb);
			storeRegister(eq[bf], irb.getFalse(), irb);
			storeRegister(un[bf], irb.getFalse(), irb);
			return;
		}
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
}


/**
 * SPE fractional conversions, EFSCFD, the vector single-precision EVFS*
 * family and EVSEL. Each 64-bit GPR holds two singles (high word = element
 * 0). Rounding mode and SPEFSCR are not modelled; float->fraction
 * conversions truncate and saturate.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateSpeVecFloat(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	uint32_t w = (uint32_t(i->bytes[0]) << 24) | (uint32_t(i->bytes[1]) << 16)
			| (uint32_t(i->bytes[2]) << 8) | uint32_t(i->bytes[3]);
	unsigned rd = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
	auto* i64 = irb.getInt64Ty();
	auto* i32 = irb.getInt32Ty();
	auto* ft = irb.getFloatTy();
	auto* dt = irb.getDoubleTy();
	auto opOf = [](unsigned n) {
		cs_ppc_op o{};
		o.type = PPC_OP_REG;
		o.reg = ppc_reg(PPC_REG_R0 + n);
		return o;
	};
	auto ld = [&](unsigned n) { auto o = opOf(n); return loadSpeI64(o, irb); };
	auto st = [&](llvm::Value* v) { auto o = opOf(rd); storeSpeI64(o, irb.CreateZExtOrTrunc(v, i64), irb); };
	auto hi = [&](llvm::Value* v) { return irb.CreateTrunc(irb.CreateLShr(v, 32), i32); };
	auto lo = [&](llvm::Value* v) { return irb.CreateTrunc(v, i32); };
	auto pack = [&](llvm::Value* h, llvm::Value* l) {
		return irb.CreateOr(irb.CreateShl(irb.CreateZExt(h, i64), 32), irb.CreateZExt(l, i64));
	};
	auto stLow = [&](llvm::Value* v) {
		auto* keep = irb.CreateAnd(ld(rd), llvm::ConstantInt::get(i64, 0xffffffff00000000ull));
		st(irb.CreateOr(keep, irb.CreateZExt(v, i64)));
	};
	auto f = [&](llvm::Value* v) { return irb.CreateBitCast(v, ft); };
	auto bits = [&](llvm::Value* v) { return irb.CreateBitCast(v, i32); };
	auto cdbl = [&](double v) { return llvm::ConstantFP::get(dt, v); };
	// Fraction <-> float. `x` is a float or double value.
	auto fromFrac = [&](llvm::Value* x, bool sg, llvm::Type* to) -> llvm::Value* {
		auto* d = sg ? irb.CreateSIToFP(x, dt) : irb.CreateUIToFP(x, dt);
		d = irb.CreateFMul(d, cdbl(sg ? std::ldexp(1.0, -31) : std::ldexp(1.0, -32)));
		return to == dt ? d : irb.CreateFPTrunc(d, to);
	};
	auto toFrac = [&](llvm::Value* x, bool sg) -> llvm::Value* {
		if (!x->getType()->isDoubleTy()) x = irb.CreateFPExt(x, dt);
		auto* d = irb.CreateFMul(x, cdbl(sg ? std::ldexp(1.0, 31) : std::ldexp(1.0, 32)));
		auto* maxV = cdbl(sg ? std::ldexp(1.0, 31) : std::ldexp(1.0, 32));
		auto* minV = cdbl(sg ? -std::ldexp(1.0, 31) : 0.0);
		auto* safe = irb.CreateSelect(irb.CreateFCmpUNO(d, d), cdbl(0.0), d);
		safe = irb.CreateSelect(irb.CreateFCmpOGE(safe, maxV), cdbl(0.0), safe);
		safe = irb.CreateSelect(irb.CreateFCmpOLT(safe, minV), cdbl(0.0), safe);
		llvm::Value* r = sg ? irb.CreateFPToSI(safe, i32) : irb.CreateFPToUI(safe, i32);
		r = irb.CreateSelect(irb.CreateFCmpOGE(d, maxV), llvm::ConstantInt::get(i32, sg ? 0x7fffffffu : 0xffffffffu), r);
		r = irb.CreateSelect(irb.CreateFCmpOLT(d, minV), llvm::ConstantInt::get(i32, sg ? 0x80000000u : 0u), r);
		return irb.CreateSelect(irb.CreateFCmpUNO(d, d), llvm::ConstantInt::get(i32, 0), r);
	};
	auto each = [&](unsigned src, const std::function<llvm::Value*(llvm::Value*)>& op) {
		auto* v = ld(src);
		st(pack(op(hi(v)), op(lo(v))));
	};
	auto each2 = [&](const std::function<llvm::Value*(llvm::Value*, llvm::Value*)>& op) {
		auto* a = ld(ra);
		auto* b = ld(rb);
		st(pack(op(hi(a), hi(b)), op(lo(a), lo(b))));
	};
	auto* sign = llvm::ConstantInt::get(i32, 0x80000000u);
	static const uint32_t crf[8][4] = {
		{PPC_REG_CR0LT, PPC_REG_CR0GT, PPC_REG_CR0EQ, PPC_REG_CR0UN},
		{PPC_REG_CR1LT, PPC_REG_CR1GT, PPC_REG_CR1EQ, PPC_REG_CR1UN},
		{PPC_REG_CR2LT, PPC_REG_CR2GT, PPC_REG_CR2EQ, PPC_REG_CR2UN},
		{PPC_REG_CR3LT, PPC_REG_CR3GT, PPC_REG_CR3EQ, PPC_REG_CR3UN},
		{PPC_REG_CR4LT, PPC_REG_CR4GT, PPC_REG_CR4EQ, PPC_REG_CR4UN},
		{PPC_REG_CR5LT, PPC_REG_CR5GT, PPC_REG_CR5EQ, PPC_REG_CR5UN},
		{PPC_REG_CR6LT, PPC_REG_CR6GT, PPC_REG_CR6EQ, PPC_REG_CR6UN},
		{PPC_REG_CR7LT, PPC_REG_CR7GT, PPC_REG_CR7EQ, PPC_REG_CR7UN},
	};
	switch (i->id)
	{
		case PPC_INS_EVFSADD: each2([&](auto* a, auto* b) { return bits(irb.CreateFAdd(f(a), f(b))); }); return;
		case PPC_INS_EVFSSUB: each2([&](auto* a, auto* b) { return bits(irb.CreateFSub(f(a), f(b))); }); return;
		case PPC_INS_EVFSMUL: each2([&](auto* a, auto* b) { return bits(irb.CreateFMul(f(a), f(b))); }); return;
		case PPC_INS_EVFSDIV: each2([&](auto* a, auto* b) { return bits(irb.CreateFDiv(f(a), f(b))); }); return;
		case PPC_INS_EVFSABS: each(ra, [&](auto* v) { return irb.CreateAnd(v, llvm::ConstantInt::get(i32, 0x7fffffffu)); }); return;
		case PPC_INS_EVFSNABS: each(ra, [&](auto* v) { return irb.CreateOr(v, sign); }); return;
		case PPC_INS_EVFSNEG: each(ra, [&](auto* v) { return irb.CreateXor(v, sign); }); return;
		case PPC_INS_EVFSCFSI: each(rb, [&](auto* v) { return bits(irb.CreateSIToFP(v, ft)); }); return;
		case PPC_INS_EVFSCFUI: each(rb, [&](auto* v) { return bits(irb.CreateUIToFP(v, ft)); }); return;
		case PPC_INS_EVFSCFSF: each(rb, [&](auto* v) { return bits(fromFrac(v, true, ft)); }); return;
		case PPC_INS_EVFSCFUF: each(rb, [&](auto* v) { return bits(fromFrac(v, false, ft)); }); return;
		case PPC_INS_EVFSCTSI: case PPC_INS_EVFSCTSIZ: each(rb, [&](auto* v) { return irb.CreateFPToSI(f(v), i32); }); return;
		case PPC_INS_EVFSCTUI: each(rb, [&](auto* v) { return irb.CreateFPToUI(f(v), i32); }); return;
		case PPC_INS_EVFSCTSF: each(rb, [&](auto* v) { return toFrac(f(v), true); }); return;
		case PPC_INS_EFSCFSF: stLow(bits(fromFrac(lo(ld(rb)), true, ft))); return;
		case PPC_INS_EFSCFUF: stLow(bits(fromFrac(lo(ld(rb)), false, ft))); return;
		case PPC_INS_EFSCTSF: stLow(toFrac(f(lo(ld(rb))), true)); return;
		case PPC_INS_EFSCTUF: stLow(toFrac(f(lo(ld(rb))), false)); return;
		case PPC_INS_EFSCFD: stLow(bits(irb.CreateFPTrunc(irb.CreateBitCast(ld(rb), dt), ft))); return;
		case PPC_INS_EFDCFSF: st(irb.CreateBitCast(fromFrac(lo(ld(rb)), true, dt), i64)); return;
		case PPC_INS_EFDCFUF: st(irb.CreateBitCast(fromFrac(lo(ld(rb)), false, dt), i64)); return;
		case PPC_INS_EFDCTSF: stLow(toFrac(irb.CreateBitCast(ld(rb), dt), true)); return;
		case PPC_INS_EFDCTUF: stLow(toFrac(irb.CreateBitCast(ld(rb), dt), false)); return;
		case PPC_INS_EVFSCMPEQ: case PPC_INS_EVFSTSTEQ:
		case PPC_INS_EVFSCMPGT: case PPC_INS_EVFSTSTGT:
		case PPC_INS_EVFSCMPLT: case PPC_INS_EVFSTSTLT:
		{
			auto* a = ld(ra);
			auto* b = ld(rb);
			auto cmp = [&](llvm::Value* x, llvm::Value* y) -> llvm::Value* {
				switch (i->id)
				{
					case PPC_INS_EVFSCMPEQ: case PPC_INS_EVFSTSTEQ: return irb.CreateFCmpOEQ(f(x), f(y));
					case PPC_INS_EVFSCMPGT: case PPC_INS_EVFSTSTGT: return irb.CreateFCmpOGT(f(x), f(y));
					default: return irb.CreateFCmpOLT(f(x), f(y));
				}
			};
			auto* ch = cmp(hi(a), hi(b));
			auto* cl = cmp(lo(a), lo(b));
			unsigned bf = (w >> 23) & 7;
			storeRegister(crf[bf][0], ch, irb);
			storeRegister(crf[bf][1], cl, irb);
			storeRegister(crf[bf][2], irb.CreateOr(ch, cl), irb);
			storeRegister(crf[bf][3], irb.CreateAnd(ch, cl), irb);
			return;
		}
		case PPC_INS_EVSEL:
		{
			unsigned crs = w & 7;
			auto* a = ld(ra);
			auto* b = ld(rb);
			auto* ch = loadRegister(crf[crs][0], irb);
			auto* cl = loadRegister(crf[crs][1], irb);
			auto toI1 = [&](llvm::Value* v) { return irb.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0)); };
			st(pack(irb.CreateSelect(toI1(ch), hi(a), hi(b)), irb.CreateSelect(toI1(cl), lo(a), lo(b))));
			return;
		}
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
}


void Capstone2LlvmIrTranslatorPowerpc_impl::translateVsxGprMove(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	// Moves between GPRs and VSRs; doubleword 0 is the high half of the 128-bit value.
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	if (pi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	for (unsigned k = 0; k < pi->op_count; ++k)
	{
		if (pi->operands[k].type == PPC_OP_REG)
		{
			auto r = pi->operands[k].reg;
			if (r >= PPC_REG_VF0 && r <= PPC_REG_VF31)
			{
				r = ppc_reg(PPC_REG_V0 + (r - PPC_REG_VF0));
			}
			pi->operands[k].reg = ppc_reg(canonicalRegister(r));
		}
	}
	auto gpr = [&](unsigned k) -> llvm::Value* {
		auto& o = pi->operands[k];
		if (o.type == PPC_OP_REG && (o.reg == PPC_REG_R0 || o.reg == PPC_REG_X0) && i->id == PPC_INS_MTVSRDD && k == 1)
		{
			return llvm::ConstantInt::get(i64, 0);
		}
		return irb.CreateZExtOrTrunc(loadOp(o, irb), i64);
	};
	auto pack = [&](llvm::Value* hi, llvm::Value* lo) {
		return irb.CreateOr(irb.CreateShl(irb.CreateZExt(hi, i128), 64), irb.CreateZExt(lo, i128));
	};
	auto toGpr = [&](llvm::Value* v) {
		storeOp(pi->operands[0], irb.CreateZExtOrTrunc(v, getDefaultType()), irb);
	};
	switch (i->id)
	{
	case PPC_INS_MFVSRD:
		toGpr(irb.CreateTrunc(irb.CreateLShr(loadOpI128(pi->operands[1], irb), 64), i64));
		return;
	case PPC_INS_MFVSRWZ:
		toGpr(irb.CreateZExt(irb.CreateTrunc(irb.CreateLShr(loadOpI128(pi->operands[1], irb), 64), irb.getInt32Ty()), i64));
		return;
	case PPC_INS_MFVSRLD:
		toGpr(irb.CreateTrunc(loadOpI128(pi->operands[1], irb), i64));
		return;
	case PPC_INS_MTVSRD:
		storeOpI128(pi->operands[0], pack(gpr(1), llvm::ConstantInt::get(i64, 0)), irb);
		return;
	case PPC_INS_MTVSRWZ:
		storeOpI128(pi->operands[0], pack(irb.CreateAnd(gpr(1), 0xffffffffull), llvm::ConstantInt::get(i64, 0)), irb);
		return;
	case PPC_INS_MTVSRWA:
		storeOpI128(pi->operands[0], pack(irb.CreateSExt(irb.CreateTrunc(gpr(1), irb.getInt32Ty()), i64),
				llvm::ConstantInt::get(i64, 0)), irb);
		return;
	case PPC_INS_MTVSRDD:
		if (pi->op_count < 3) break;
		storeOpI128(pi->operands[0], pack(gpr(1), gpr(2)), irb);
		return;
	case PPC_INS_MTVSRWS:
	{
		auto* w = irb.CreateAnd(gpr(1), 0xffffffffull);
		auto* d = irb.CreateOr(irb.CreateShl(w, 32), w);
		storeOpI128(pi->operands[0], pack(d, d), irb);
		return;
	}
	default:
		break;
	}
	translatePseudoAsmGeneric(i, pi, irb);
}



/**
 * VSX permute/blend/splat-immediate ops: XXPERM, XXPERMR, XXBLENDV{B,H,W,D},
 * XXSPLTIB, XXSPLTIW, XXSPLTIDP and XXSPLTI32DX.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVsxPermMisc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* i128 = irb.getInt128Ty();
	auto imm = [&](unsigned k) { return static_cast<uint64_t>(pi->operands[k].imm); };
	auto c128 = [&](uint64_t hi, uint64_t lo) {
		return llvm::ConstantInt::get(i128, (llvm::APInt(128, hi).shl(64)) | llvm::APInt(128, lo));
	};
	switch (i->id)
	{
		case PPC_INS_XXPERM:
		case PPC_INS_XXPERMR:
		{
			EXPECT_IS_TERNARY(i, pi, irb);
			bool rev = i->id == PPC_INS_XXPERMR;
			auto* t = loadOpI128(pi->operands[0], irb);
			auto* a = loadOpI128(pi->operands[1], irb);
			auto* c = loadOpI128(pi->operands[2], irb);
			auto* i256 = irb.getIntNTy(256);
			auto* concat = irb.CreateOr(irb.CreateShl(irb.CreateZExt(a, i256), 128), irb.CreateZExt(t, i256));
			llvm::Value* result = llvm::ConstantInt::get(i128, 0);
			for (unsigned lane = 0; lane < 16; ++lane)
			{
				unsigned place = (15u - lane) * 8u;
				auto* cbyte = irb.CreateTrunc(irb.CreateLShr(c, llvm::ConstantInt::get(i128, place)), irb.getInt8Ty());
				auto* idx = irb.CreateZExt(irb.CreateAnd(cbyte, irb.getInt8(31)), irb.getInt32Ty());
				if (rev)
				{
					idx = irb.CreateSub(irb.getInt32(31), idx);
				}
				auto* fromRight = irb.CreateMul(irb.CreateSub(irb.getInt32(31), idx), irb.getInt32(8));
				auto* byte = irb.CreateTrunc(irb.CreateLShr(concat, irb.CreateZExt(fromRight, i256)), irb.getInt8Ty());
				result = irb.CreateOr(result, irb.CreateShl(irb.CreateZExt(byte, i128), place));
			}
			storeOpI128(pi->operands[0], result, irb);
			return;
		}
		case PPC_INS_XXBLENDVB:
		case PPC_INS_XXBLENDVH:
		case PPC_INS_XXBLENDVW:
		case PPC_INS_XXBLENDVD:
		{
			EXPECT_IS_QUATERNARY(i, pi, irb);
			unsigned lb = i->id == PPC_INS_XXBLENDVB ? 8 : i->id == PPC_INS_XXBLENDVH ? 16 : i->id == PPC_INS_XXBLENDVW ? 32 : 64;
			auto* a = loadOpI128(pi->operands[1], irb);
			auto* b = loadOpI128(pi->operands[2], irb);
			auto* c = loadOpI128(pi->operands[3], irb);
			// Build a full-lane mask by arithmetic-shifting each lane's sign bit.
			auto* vt = llvm::FixedVectorType::get(irb.getIntNTy(lb), 128 / lb);
			auto* m = irb.CreateBitCast(irb.CreateAShr(irb.CreateBitCast(c, vt),
					llvm::ConstantInt::get(vt, lb - 1)), i128);
			storeOpI128(pi->operands[0], irb.CreateOr(irb.CreateAnd(a, irb.CreateNot(m)), irb.CreateAnd(b, m)), irb);
			return;
		}
		case PPC_INS_XXSPLTIB:
		{
			uint64_t v = (imm(1) & 0xff) * 0x0101010101010101ULL;
			storeOpI128(pi->operands[0], c128(v, v), irb);
			return;
		}
		case PPC_INS_XXSPLTIW:
		{
			uint64_t v = (imm(1) & 0xffffffffULL) * 0x0000000100000001ULL;
			storeOpI128(pi->operands[0], c128(v, v), irb);
			return;
		}
		case PPC_INS_XXSPLTIDP:
		{
			uint32_t w = static_cast<uint32_t>(imm(1));
			float f;
			std::memcpy(&f, &w, 4);
			double d = f;
			uint64_t v;
			std::memcpy(&v, &d, 8);
			storeOpI128(pi->operands[0], c128(v, v), irb);
			return;
		}
		case PPC_INS_XXSPLTI32DX:
		{
			unsigned ix = imm(1) & 1u;
			uint64_t w = imm(2) & 0xffffffffULL;
			uint64_t keepMask = ix ? 0xffffffff00000000ULL : 0x00000000ffffffffULL;
			uint64_t put = ix ? w : (w << 32);
			auto* t = loadOpI128(pi->operands[0], irb);
			auto* r = irb.CreateOr(irb.CreateAnd(t, c128(keepMask, keepMask)), c128(put, put));
			storeOpI128(pi->operands[0], r, irb);
			return;
		}
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
}


/**
 * DCBZ/DCBZL: zero the data cache block containing EA. The block size is
 * implementation-defined; 128 bytes (POWER4 and later, PPC970) is assumed.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateDcbz(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	(void)i;
	const uint64_t blockSize = 128;
	auto* ea = loadIndexedEffectiveAddress(pi, irb);
	auto* ty = ea->getType();
	auto* base = irb.CreateAnd(ea, llvm::ConstantInt::get(ty, ~(blockSize - 1)));
	auto* i64 = irb.getInt64Ty();
	auto* zero = llvm::ConstantInt::get(i64, 0);
	for (uint64_t off = 0; off < blockSize; off += 8)
	{
		storeIntPtr(irb, zero, irb.CreateAdd(base, llvm::ConstantInt::get(ty, off)), i64);
	}
}


/**
 * Scalar VSX loads/stores of doubleword 0 of a VSR:
 * PPC_INS_LXSD, PPC_INS_STXSD, PPC_INS_LXSSP, PPC_INS_STXSSP,
 * PPC_INS_LXSSPX, PPC_INS_STXSSPX, PPC_INS_LXSIWZX, PPC_INS_LXSIWAX,
 * PPC_INS_LXSIBZX, PPC_INS_LXSIHZX, PPC_INS_STXSIWX, PPC_INS_STXSIBX,
 * PPC_INS_STXSIHX. Doubleword 1 of the target is undefined and is zeroed.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVsxScalarLoadStore(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);

	cs_ppc_op op0 = pi->operands[0];
	if (op0.type == PPC_OP_REG && op0.reg >= PPC_REG_VF0 && op0.reg <= PPC_REG_VF31)
	{
		op0.reg = ppc_reg(PPC_REG_V0 + (op0.reg - PPC_REG_VF0));
	}
	auto* add = loadIndexedEffectiveAddress(pi, irb);
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();

	auto storeDw0 = [&](llvm::Value* dw) {
		storeOpI128(op0, irb.CreateShl(irb.CreateZExt(dw, i128), 64), irb);
	};
	auto loadDw0 = [&]() {
		return irb.CreateTrunc(irb.CreateLShr(loadOpI128(op0, irb), 64), i64);
	};

	switch (i->id)
	{
	case PPC_INS_LXSD:
		storeDw0(loadIntPtr(irb, add, i64));
		break;
	case PPC_INS_LXSSP:
	case PPC_INS_LXSSPX:
	{
		auto* w = loadIntPtr(irb, add, irb.getInt32Ty());
		auto* d = irb.CreateFPExt(irb.CreateBitCast(w, irb.getFloatTy()), irb.getDoubleTy());
		storeDw0(irb.CreateBitCast(d, i64));
		break;
	}
	case PPC_INS_LXSIWZX:
		storeDw0(irb.CreateZExt(loadIntPtr(irb, add, irb.getInt32Ty()), i64));
		break;
	case PPC_INS_LXSIWAX:
		storeDw0(irb.CreateSExt(loadIntPtr(irb, add, irb.getInt32Ty()), i64));
		break;
	case PPC_INS_LXSIBZX:
		storeDw0(irb.CreateZExt(loadIntPtr(irb, add, irb.getInt8Ty()), i64));
		break;
	case PPC_INS_LXSIHZX:
		storeDw0(irb.CreateZExt(loadIntPtr(irb, add, irb.getInt16Ty()), i64));
		break;
	case PPC_INS_STXSD:
		storeIntPtr(irb, loadDw0(), add, i64);
		break;
	case PPC_INS_STXSSP:
	case PPC_INS_STXSSPX:
	{
		auto* d = irb.CreateBitCast(loadDw0(), irb.getDoubleTy());
		auto* s = irb.CreateBitCast(irb.CreateFPTrunc(d, irb.getFloatTy()), irb.getInt32Ty());
		storeIntPtr(irb, s, add, irb.getInt32Ty());
		break;
	}
	case PPC_INS_STXSIWX:
		storeIntPtr(irb, irb.CreateTrunc(loadDw0(), irb.getInt32Ty()), add, irb.getInt32Ty());
		break;
	case PPC_INS_STXSIBX:
		storeIntPtr(irb, irb.CreateTrunc(loadDw0(), irb.getInt8Ty()), add, irb.getInt8Ty());
		break;
	case PPC_INS_STXSIHX:
		storeIntPtr(irb, irb.CreateTrunc(loadDw0(), irb.getInt16Ty()), add, irb.getInt16Ty());
		break;
	default:
		break;
	}
}


namespace {

/// Big-endian byte offset `off` (iW) of a `size`-byte field in a `bits`-wide value.
llvm::Value* ppcBeFieldShift(llvm::Value* off, unsigned size, unsigned bits, llvm::IRBuilder<>& irb)
{
	auto* ty = irb.getIntNTy(bits);
	off = irb.CreateZExtOrTrunc(off, ty);
	auto* bytes = irb.CreateSub(llvm::ConstantInt::get(ty, bits / 8 - size), off);
	return irb.CreateShl(bytes, llvm::ConstantInt::get(ty, 3));
}

llvm::Value* ppcBeExtract(llvm::Value* v, llvm::Value* off, unsigned size, llvm::IRBuilder<>& irb)
{
	unsigned bits = v->getType()->getIntegerBitWidth();
	auto* sh = ppcBeFieldShift(off, size, bits, irb);
	return irb.CreateTrunc(irb.CreateLShr(v, sh), irb.getIntNTy(size * 8));
}

llvm::Value* ppcBeInsert(llvm::Value* v, llvm::Value* off, unsigned size, llvm::Value* val, llvm::IRBuilder<>& irb)
{
	unsigned bits = v->getType()->getIntegerBitWidth();
	auto* ty = v->getType();
	auto* sh = ppcBeFieldShift(off, size, bits, irb);
	auto* mask = irb.CreateShl(
			llvm::ConstantInt::get(ty, llvm::APInt::getLowBitsSet(bits, size * 8)), sh);
	auto* field = irb.CreateShl(
			irb.CreateZExt(irb.CreateTrunc(val, irb.getIntNTy(size * 8)), ty), sh);
	return irb.CreateOr(irb.CreateAnd(v, irb.CreateNot(mask)), field);
}

} // anonymous namespace

/**
 * PPC_INS_VEXTU[BHW][LR]X, PPC_INS_VEXTRACTU[BHW], PPC_INS_VEXTRACTD,
 * PPC_INS_VEXTDU[BHW]V[LR]X, PPC_INS_VEXTDDV[LR]X, PPC_INS_VINSERT[BHWD],
 * PPC_INS_VINS[WD], PPC_INS_VINS[BHWD][LR]X, PPC_INS_VINS[BHW]V[LR]X
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecInsertExtract(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto gpr = [&](unsigned n) {
		return irb.CreateZExtOrTrunc(loadOp(pi->operands[n], irb), i64);
	};
	auto imm = [&](unsigned n) -> llvm::Value* {
		return llvm::ConstantInt::get(i64, pi->operands[n].type == PPC_OP_IMM ? pi->operands[n].imm & 0x1f : 0);
	};
	auto rightOff = [&](llvm::Value* idx, unsigned size, unsigned total) {
		return irb.CreateSub(llvm::ConstantInt::get(i64, total - size), idx);
	};
	auto mask = [&](llvm::Value* x, uint64_t m) {
		return irb.CreateAnd(x, llvm::ConstantInt::get(i64, m));
	};
	// Element taken from dword 0 (low-order end) of a VR: byte 7, hword 3, word 1, dword 0.
	auto dw0 = [&](unsigned n) {
		return irb.CreateTrunc(irb.CreateLShr(loadOpI128(pi->operands[n], irb), 64), i64);
	};

	unsigned size = 0;
	bool right = false;
	switch (i->id)
	{
	// GPR <- VR element, variable index.
	case PPC_INS_VEXTUBLX: size = 1; break;
	case PPC_INS_VEXTUHLX: size = 2; break;
	case PPC_INS_VEXTUWLX: size = 4; break;
	case PPC_INS_VEXTUBRX: size = 1; right = true; break;
	case PPC_INS_VEXTUHRX: size = 2; right = true; break;
	case PPC_INS_VEXTUWRX: size = 4; right = true; break;
	default: break;
	}
	if (size)
	{
		EXPECT_IS_TERNARY(i, pi, irb);
		auto* idx = mask(gpr(1), 0xf);
		auto* off = right ? rightOff(idx, size, 16) : idx;
		auto* v = ppcBeExtract(loadOpI128(pi->operands[2], irb), off, size, irb);
		storeOp(pi->operands[0], irb.CreateZExt(v, i64), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	switch (i->id)
	{
	// VR dword0 <- VR element, immediate index.
	case PPC_INS_VEXTRACTUB: size = 1; break;
	case PPC_INS_VEXTRACTUH: size = 2; break;
	case PPC_INS_VEXTRACTUW: size = 4; break;
	case PPC_INS_VEXTRACTD: size = 8; break;
	default: break;
	}
	if (size)
	{
		EXPECT_IS_TERNARY(i, pi, irb);
		auto* v = ppcBeExtract(loadOpI128(pi->operands[1], irb), mask(imm(2), 0xf), size, irb);
		storeOpI128(pi->operands[0], irb.CreateShl(irb.CreateZExt(v, i128), 64), irb);
		return;
	}

	switch (i->id)
	{
	// VR dword0 <- element of VRA||VRB, GPR index.
	case PPC_INS_VEXTDUBVLX: size = 1; break;
	case PPC_INS_VEXTDUHVLX: size = 2; break;
	case PPC_INS_VEXTDUWVLX: size = 4; break;
	case PPC_INS_VEXTDDVLX: size = 8; break;
	case PPC_INS_VEXTDUBVRX: size = 1; right = true; break;
	case PPC_INS_VEXTDUHVRX: size = 2; right = true; break;
	case PPC_INS_VEXTDUWVRX: size = 4; right = true; break;
	case PPC_INS_VEXTDDVRX: size = 8; right = true; break;
	default: break;
	}
	if (size)
	{
		if (pi->op_count != 4)
		{
			throw GenericError("unexpected operand count");
		}
		auto* i256 = irb.getIntNTy(256);
		auto* cat = irb.CreateOr(
				irb.CreateShl(irb.CreateZExt(loadOpI128(pi->operands[1], irb), i256), 128),
				irb.CreateZExt(loadOpI128(pi->operands[2], irb), i256));
		auto* idx = mask(gpr(3), 0x1f);
		auto* off = right ? rightOff(idx, size, 32) : idx;
		auto* v = ppcBeExtract(cat, off, size, irb);
		storeOpI128(pi->operands[0], irb.CreateShl(irb.CreateZExt(v, i128), 64), irb);
		return;
	}

	// Inserts: VRT is read-modify-write.
	llvm::Value* off = nullptr;
	llvm::Value* val = nullptr;
	EXPECT_IS_TERNARY(i, pi, irb);
	switch (i->id)
	{
	case PPC_INS_VINSERTB: size = 1; break;
	case PPC_INS_VINSERTH: size = 2; break;
	case PPC_INS_VINSERTW: size = 4; break;
	case PPC_INS_VINSERTD: size = 8; break;
	case PPC_INS_VINSW: size = 4; off = mask(imm(2), 0xf); val = gpr(1); break;
	case PPC_INS_VINSD: size = 8; off = mask(imm(2), 0xf); val = gpr(1); break;
	case PPC_INS_VINSBLX: size = 1; break;
	case PPC_INS_VINSHLX: size = 2; break;
	case PPC_INS_VINSWLX: size = 4; break;
	case PPC_INS_VINSDLX: size = 8; break;
	case PPC_INS_VINSBRX: size = 1; right = true; break;
	case PPC_INS_VINSHRX: size = 2; right = true; break;
	case PPC_INS_VINSWRX: size = 4; right = true; break;
	case PPC_INS_VINSDRX: size = 8; right = true; break;
	case PPC_INS_VINSBVLX: size = 1; break;
	case PPC_INS_VINSHVLX: size = 2; break;
	case PPC_INS_VINSWVLX: size = 4; break;
	case PPC_INS_VINSBVRX: size = 1; right = true; break;
	case PPC_INS_VINSHVRX: size = 2; right = true; break;
	case PPC_INS_VINSWVRX: size = 4; right = true; break;
	default:
		throw GenericError("unhandled insert/extract");
	}
	switch (i->id)
	{
	case PPC_INS_VINSERTB:
	case PPC_INS_VINSERTH:
	case PPC_INS_VINSERTW:
	case PPC_INS_VINSERTD:
		off = mask(imm(2), 0xf);
		val = dw0(1);
		break;
	case PPC_INS_VINSBVLX:
	case PPC_INS_VINSHVLX:
	case PPC_INS_VINSWVLX:
	case PPC_INS_VINSBVRX:
	case PPC_INS_VINSHVRX:
	case PPC_INS_VINSWVRX:
	{
		auto* idx = mask(gpr(1), 0xf);
		off = right ? rightOff(idx, size, 16) : idx;
		val = dw0(2);
		break;
	}
	case PPC_INS_VINSW:
	case PPC_INS_VINSD:
		break;
	default:
	{
		auto* idx = mask(gpr(1), 0xf);
		off = right ? rightOff(idx, size, 16) : idx;
		val = gpr(2);
		break;
	}
	}
	auto* vt = loadOpI128(pi->operands[0], irb);
	storeOpI128(pi->operands[0], ppcBeInsert(vt, off, size, val, irb), irb);
}


/**
 * PPC_INS_VSLV, PPC_INS_VSRV, PPC_INS_VRLWNM, PPC_INS_VRLDNM, PPC_INS_VRLWMI,
 * PPC_INS_VRLDMI, PPC_INS_VEXTRACTBM, PPC_INS_VEXTRACTHM, PPC_INS_VEXTRACTWM,
 * PPC_INS_VEXTRACTDM, PPC_INS_VEXTRACTQM, PPC_INS_VPMSUMB, PPC_INS_VPMSUMH,
 * PPC_INS_VPMSUMW, PPC_INS_VPMSUMD
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecBitOps(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* i128 = irb.getInt128Ty();
	// Big-endian element numbering: element 0 is the most significant.
	auto elem = [&](llvm::Value* v, unsigned idx, unsigned bits) -> llvm::Value* {
		unsigned n = 128u / bits;
		return irb.CreateTrunc(irb.CreateLShr(v, (n - 1u - idx) * bits), irb.getIntNTy(bits));
	};
	auto place = [&](llvm::Value* acc, llvm::Value* e, unsigned idx, unsigned bits) -> llvm::Value* {
		unsigned n = 128u / bits;
		return irb.CreateOr(acc, irb.CreateShl(irb.CreateZExt(e, i128), (n - 1u - idx) * bits));
	};
	llvm::Value* zero128 = llvm::ConstantInt::get(i128, 0);

	switch (i->id)
	{
	case PPC_INS_VEXTRACTBM:
	case PPC_INS_VEXTRACTHM:
	case PPC_INS_VEXTRACTWM:
	case PPC_INS_VEXTRACTDM:
	case PPC_INS_VEXTRACTQM:
	{
		EXPECT_IS_BINARY(i, pi, irb);
		unsigned bits = i->id == PPC_INS_VEXTRACTBM ? 8 : i->id == PPC_INS_VEXTRACTHM ? 16
				: i->id == PPC_INS_VEXTRACTWM ? 32 : i->id == PPC_INS_VEXTRACTDM ? 64 : 128;
		unsigned n = 128u / bits;
		auto* v = loadOpI128(pi->operands[1], irb);
		auto* dt = getDefaultType();
		llvm::Value* r = llvm::ConstantInt::get(dt, 0);
		for (unsigned k = 0; k < n; ++k)
		{
			auto* msb = irb.CreateTrunc(irb.CreateLShr(v, (n - 1u - k) * bits + bits - 1u), irb.getInt1Ty());
			r = irb.CreateOr(r, irb.CreateShl(irb.CreateZExt(msb, dt), n - 1u - k));
		}
		storeOp(pi->operands[0], r, irb);
		return;
	}
	default:
		break;
	}

	EXPECT_IS_TERNARY(i, pi, irb);
	auto* a = loadOpI128(pi->operands[1], irb);
	auto* b = loadOpI128(pi->operands[2], irb);
	llvm::Value* r = zero128;

	switch (i->id)
	{
	case PPC_INS_VSLV:
	case PPC_INS_VSRV:
	{
		bool left = i->id == PPC_INS_VSLV;
		auto* i16 = irb.getInt16Ty();
		for (unsigned k = 0; k < 16; ++k)
		{
			auto* sh = irb.CreateZExt(irb.CreateAnd(elem(b, k, 8), irb.getInt8(7)), i16);
			llvm::Value* e;
			if (left)
			{
				auto* hi = irb.CreateZExt(elem(a, k, 8), i16);
				llvm::Value* lo = k < 15 ? irb.CreateZExt(elem(a, k + 1, 8), i16) : irb.getInt16(0);
				auto* src = irb.CreateOr(irb.CreateShl(hi, 8), lo);
				e = irb.CreateTrunc(irb.CreateLShr(irb.CreateShl(src, sh), 8), irb.getInt8Ty());
			}
			else
			{
				llvm::Value* hi = k > 0 ? irb.CreateZExt(elem(a, k - 1, 8), i16) : irb.getInt16(0);
				auto* lo = irb.CreateZExt(elem(a, k, 8), i16);
				auto* src = irb.CreateOr(irb.CreateShl(hi, 8), lo);
				e = irb.CreateTrunc(irb.CreateLShr(src, sh), irb.getInt8Ty());
			}
			r = place(r, e, k, 8);
		}
		break;
	}
	case PPC_INS_VRLWNM:
	case PPC_INS_VRLDNM:
	case PPC_INS_VRLWMI:
	case PPC_INS_VRLDMI:
	{
		bool dw = i->id == PPC_INS_VRLDNM || i->id == PPC_INS_VRLDMI;
		bool mi = i->id == PPC_INS_VRLWMI || i->id == PPC_INS_VRLDMI;
		unsigned bits = dw ? 64 : 32;
		unsigned n = 128u / bits;
		auto* ty = irb.getIntNTy(bits);
		auto* fieldMask = llvm::ConstantInt::get(ty, bits - 1);
		auto* ones = llvm::ConstantInt::getAllOnesValue(ty);
		auto* t = mi ? loadOpI128(pi->operands[0], irb) : nullptr;
		for (unsigned k = 0; k < n; ++k)
		{
			auto* ctl = elem(b, k, bits);
			auto* sh = irb.CreateAnd(ctl, fieldMask);
			auto* mb = irb.CreateAnd(irb.CreateLShr(ctl, 16), fieldMask);
			auto* me = irb.CreateAnd(irb.CreateLShr(ctl, 8), fieldMask);
			auto* src = elem(a, k, bits);
			auto* back = irb.CreateAnd(irb.CreateSub(llvm::ConstantInt::get(ty, bits), sh), fieldMask);
			auto* rot = irb.CreateOr(irb.CreateShl(src, sh), irb.CreateLShr(src, back));
			auto* m1 = irb.CreateLShr(ones, mb);
			auto* m2 = irb.CreateShl(ones, irb.CreateSub(fieldMask, me));
			auto* mask = irb.CreateSelect(irb.CreateICmpULE(mb, me), irb.CreateAnd(m1, m2), irb.CreateOr(m1, m2));
			llvm::Value* e = irb.CreateAnd(rot, mask);
			if (mi)
			{
				e = irb.CreateOr(e, irb.CreateAnd(elem(t, k, bits), irb.CreateNot(mask)));
			}
			r = place(r, e, k, bits);
		}
		break;
	}
	case PPC_INS_VPMSUMB:
	case PPC_INS_VPMSUMH:
	case PPC_INS_VPMSUMW:
	case PPC_INS_VPMSUMD:
	{
		unsigned bits = i->id == PPC_INS_VPMSUMB ? 8 : i->id == PPC_INS_VPMSUMH ? 16
				: i->id == PPC_INS_VPMSUMW ? 32 : 64;
		unsigned n = 128u / bits;
		auto* wide = irb.getIntNTy(2 * bits);
		auto clmul = [&](llvm::Value* x, llvm::Value* y) -> llvm::Value* {
			auto* xw = irb.CreateZExt(x, wide);
			auto* yw = irb.CreateZExt(y, wide);
			llvm::Value* acc = llvm::ConstantInt::get(wide, 0);
			for (unsigned bit = 0; bit < bits; ++bit)
			{
				auto* set = irb.CreateTrunc(irb.CreateLShr(yw, bit), irb.getInt1Ty());
				acc = irb.CreateXor(acc, irb.CreateSelect(set, irb.CreateShl(xw, bit), llvm::ConstantInt::get(wide, 0)));
			}
			return acc;
		};
		for (unsigned k = 0; k < n / 2u; ++k)
		{
			auto* e = irb.CreateXor(
					clmul(elem(a, 2 * k, bits), elem(b, 2 * k, bits)),
					clmul(elem(a, 2 * k + 1, bits), elem(b, 2 * k + 1, bits)));
			r = place(r, e, k, 2 * bits);
		}
		break;
	}
	default:
		throw GenericError("unhandled vector bit op");
	}
	storeOpI128(pi->operands[0], r, irb);
}


/**
 * PPC_INS_VCLRLB, PPC_INS_VCLRRB, PPC_INS_VSLDBI, PPC_INS_VSRDBI,
 * PPC_INS_VCLZLSBB, PPC_INS_VCTZLSBB, PPC_INS_VGBBD, PPC_INS_VBPERMQ
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecBytePerm(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto byteAt = [&](llvm::Value* v, unsigned idx) {
		return irb.CreateTrunc(irb.CreateLShr(v, (15u - idx) * 8u), irb.getInt8Ty());
	};

	switch (i->id)
	{
	case PPC_INS_VCLRLB:
	case PPC_INS_VCLRRB:
	{
		EXPECT_IS_TERNARY(i, pi, irb);
		auto* a = loadOpI128(pi->operands[1], irb);
		auto* n = irb.CreateZExtOrTrunc(loadOp(pi->operands[2], irb), i64);
		auto* big = irb.CreateICmpUGE(n, llvm::ConstantInt::get(i64, 16));
		auto* sh = irb.CreateZExt(irb.CreateMul(irb.CreateSub(llvm::ConstantInt::get(i64, 16), n), llvm::ConstantInt::get(i64, 8)), i128);
		auto* safeSh = irb.CreateSelect(big, llvm::ConstantInt::get(i128, 0), sh);
		auto* ones = llvm::ConstantInt::getAllOnesValue(i128);
		// Clear-left keeps the rightmost N bytes; clear-right the leftmost N.
		llvm::Value* mask = i->id == PPC_INS_VCLRLB
				? irb.CreateLShr(ones, safeSh)
				: irb.CreateShl(ones, safeSh);
		mask = irb.CreateSelect(irb.CreateICmpEQ(n, llvm::ConstantInt::get(i64, 0)),
				llvm::ConstantInt::get(i128, 0), mask);
		mask = irb.CreateSelect(big, ones, mask);
		storeOpI128(pi->operands[0], irb.CreateAnd(a, mask), irb);
		return;
	}
	case PPC_INS_VSLDBI:
	case PPC_INS_VSRDBI:
	{
		EXPECT_IS_QUATERNARY(i, pi, irb);
		auto* i256 = irb.getIntNTy(256);
		auto* cat = irb.CreateOr(
				irb.CreateShl(irb.CreateZExt(loadOpI128(pi->operands[1], irb), i256), 128),
				irb.CreateZExt(loadOpI128(pi->operands[2], irb), i256));
		unsigned sh = pi->operands[3].type == PPC_OP_IMM ? unsigned(pi->operands[3].imm & 7) : 0u;
		llvm::Value* r = i->id == PPC_INS_VSLDBI
				? irb.CreateTrunc(irb.CreateLShr(irb.CreateShl(cat, sh), 128), i128)
				: irb.CreateTrunc(irb.CreateLShr(cat, sh), i128);
		storeOpI128(pi->operands[0], r, irb);
		return;
	}
	case PPC_INS_VCLZLSBB:
	case PPC_INS_VCTZLSBB:
	{
		EXPECT_IS_BINARY(i, pi, irb);
		auto* v = loadOpI128(pi->operands[1], irb);
		// Gather the least-significant bit of each byte, element 0 first.
		llvm::Value* bitsV = llvm::ConstantInt::get(irb.getInt16Ty(), 0);
		for (unsigned k = 0; k < 16; ++k)
		{
			auto* lsb = irb.CreateZExt(irb.CreateAnd(byteAt(v, k), irb.getInt8(1)), irb.getInt16Ty());
			bitsV = irb.CreateOr(bitsV, irb.CreateShl(lsb, 15u - k));
		}
		auto id = i->id == PPC_INS_VCLZLSBB ? llvm::Intrinsic::ctlz : llvm::Intrinsic::cttz;
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, id, irb.getInt16Ty());
		auto* cnt = irb.CreateCall(f, {bitsV, irb.getFalse()});
		storeOp(pi->operands[0], irb.CreateZExt(cnt, getDefaultType()), irb);
		return;
	}
	case PPC_INS_VGBBD:
	{
		EXPECT_IS_BINARY(i, pi, irb);
		auto* v = loadOpI128(pi->operands[1], irb);
		llvm::Value* r = llvm::ConstantInt::get(i128, 0);
		// Per doubleword, result bit (8*j + k) = source bit (8*k + j), with
		// bit 0 the most significant.
		for (unsigned d = 0; d < 2; ++d)
		{
			unsigned base = d == 0 ? 64u : 0u;
			for (unsigned j = 0; j < 8; ++j)
			{
				for (unsigned k = 0; k < 8; ++k)
				{
					unsigned src = base + 63u - (8u * k + j);
					unsigned dst = base + 63u - (8u * j + k);
					auto* bit = irb.CreateAnd(irb.CreateLShr(v, src), llvm::ConstantInt::get(i128, 1));
					r = irb.CreateOr(r, irb.CreateShl(bit, dst));
				}
			}
		}
		storeOpI128(pi->operands[0], r, irb);
		return;
	}
	case PPC_INS_VBPERMQ:
	{
		EXPECT_IS_TERNARY(i, pi, irb);
		auto* a = loadOpI128(pi->operands[1], irb);
		auto* b = loadOpI128(pi->operands[2], irb);
		llvm::Value* r = llvm::ConstantInt::get(i128, 0);
		for (unsigned k = 0; k < 16; ++k)
		{
			auto* idx = irb.CreateZExt(byteAt(b, k), i128);
			auto* inRange = irb.CreateICmpULT(idx, llvm::ConstantInt::get(i128, 128));
			auto* sh = irb.CreateSub(llvm::ConstantInt::get(i128, 127), idx);
			auto* safe = irb.CreateSelect(inRange, sh, llvm::ConstantInt::get(i128, 0));
			auto* bit = irb.CreateAnd(irb.CreateLShr(a, safe), llvm::ConstantInt::get(i128, 1));
			bit = irb.CreateSelect(inRange, bit, llvm::ConstantInt::get(i128, 0));
			r = irb.CreateOr(r, irb.CreateShl(bit, 64u + 15u - k));
		}
		storeOpI128(pi->operands[0], r, irb);
		return;
	}
	default:
		throw GenericError("unhandled vector byte op");
	}
}


/**
 * PPC_INS_MTVSRBM, PPC_INS_MTVSRHM, PPC_INS_MTVSRWM, PPC_INS_MTVSRDM,
 * PPC_INS_MTVSRQM, PPC_INS_MTVSRBMI, PPC_INS_VCNTMBB, PPC_INS_VCNTMBH,
 * PPC_INS_VCNTMBW, PPC_INS_VCNTMBD, PPC_INS_VSTRIBL, PPC_INS_VSTRIBR,
 * PPC_INS_VSTRIHL, PPC_INS_VSTRIHR
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecMaskOps(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, pi, irb);
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	unsigned bits = 0;
	switch (i->id)
	{
	case PPC_INS_MTVSRBM:
	case PPC_INS_MTVSRBMI:
	case PPC_INS_VCNTMBB:
	case PPC_INS_VSTRIBL:
	case PPC_INS_VSTRIBR:
		bits = 8;
		break;
	case PPC_INS_MTVSRHM:
	case PPC_INS_VCNTMBH:
	case PPC_INS_VSTRIHL:
	case PPC_INS_VSTRIHR:
		bits = 16;
		break;
	case PPC_INS_MTVSRWM:
	case PPC_INS_VCNTMBW:
		bits = 32;
		break;
	case PPC_INS_MTVSRDM:
	case PPC_INS_VCNTMBD:
		bits = 64;
		break;
	case PPC_INS_MTVSRQM:
		bits = 128;
		break;
	default:
		throw GenericError("unhandled vector mask op");
	}
	unsigned n = 128u / bits;
	auto* eTy = irb.getIntNTy(bits);
	auto elem = [&](llvm::Value* v, unsigned idx) {
		return irb.CreateTrunc(irb.CreateLShr(v, (n - 1u - idx) * bits), eTy);
	};
	auto place = [&](llvm::Value* acc, llvm::Value* e, unsigned idx) {
		return irb.CreateOr(acc, irb.CreateShl(irb.CreateZExt(e, i128), (n - 1u - idx) * bits));
	};
	llvm::Value* r = llvm::ConstantInt::get(i128, 0);

	switch (i->id)
	{
	case PPC_INS_MTVSRBM:
	case PPC_INS_MTVSRHM:
	case PPC_INS_MTVSRWM:
	case PPC_INS_MTVSRDM:
	case PPC_INS_MTVSRQM:
	case PPC_INS_MTVSRBMI:
	{
		if (pi->op_count != 2)
		{
			throw GenericError("unexpected operand count");
		}
		llvm::Value* src = pi->operands[1].type == PPC_OP_IMM
				? static_cast<llvm::Value*>(llvm::ConstantInt::get(i64, uint64_t(pi->operands[1].imm) & 0xffff))
				: irb.CreateZExtOrTrunc(loadOp(pi->operands[1], irb), i64);
		for (unsigned k = 0; k < n; ++k)
		{
			auto* bit = irb.CreateTrunc(irb.CreateLShr(src, n - 1u - k), irb.getInt1Ty());
			r = place(r, irb.CreateSExt(bit, eTy), k);
		}
		storeOpI128(pi->operands[0], r, irb);
		return;
	}
	case PPC_INS_VCNTMBB:
	case PPC_INS_VCNTMBH:
	case PPC_INS_VCNTMBW:
	case PPC_INS_VCNTMBD:
	{
		if (pi->op_count != 3)
		{
			throw GenericError("unexpected operand count");
		}
		auto* v = loadOpI128(pi->operands[1], irb);
		bool mp = pi->operands[2].type == PPC_OP_IMM && (pi->operands[2].imm & 1);
		llvm::Value* cnt = llvm::ConstantInt::get(i64, 0);
		for (unsigned k = 0; k < n; ++k)
		{
			auto* msb = irb.CreateICmpSLT(elem(v, k), llvm::ConstantInt::get(eTy, 0));
			auto* hit = mp ? msb : irb.CreateNot(msb);
			cnt = irb.CreateAdd(cnt, irb.CreateZExt(hit, i64));
		}
		// The count is left-justified: RT bits 0:7 / 0:6 / 0:5 / 0:4.
		unsigned width = bits == 8 ? 8 : bits == 16 ? 7 : bits == 32 ? 6 : 5;
		storeOp(pi->operands[0], irb.CreateShl(cnt, 64u - width), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	default:
		break;
	}

	// VSTRI*: copy elements up to the first zero element, clear the rest.
	if (pi->op_count != 2)
	{
		throw GenericError("unexpected operand count");
	}
	bool fromRight = i->id == PPC_INS_VSTRIBR || i->id == PPC_INS_VSTRIHR;
	auto* v = loadOpI128(pi->operands[1], irb);
	llvm::Value* seen = irb.getFalse();
	for (unsigned s = 0; s < n; ++s)
	{
		unsigned k = fromRight ? n - 1u - s : s;
		auto* e = elem(v, k);
		auto* isNull = irb.CreateICmpEQ(e, llvm::ConstantInt::get(eTy, 0));
		r = place(r, irb.CreateSelect(seen, llvm::ConstantInt::get(eTy, 0), e), k);
		seen = irb.CreateOr(seen, isNull);
	}
	storeOpI128(pi->operands[0], r, irb);
	std::string mn = i->mnemonic;
	if (!mn.empty() && mn.back() == '.')
	{
		uint32_t ltR = PPC_REG_CR0LT, gtR = PPC_REG_CR0GT, eqR = PPC_REG_CR0EQ, soR = PPC_REG_CR0UN;
		crFieldRegisters(PPC_REG_CR6, ltR, gtR, eqR, soR);
		storeRegister(ltR, irb.getFalse(), irb);
		storeRegister(gtR, irb.getFalse(), irb);
		storeRegister(eqR, seen, irb);
		storeRegister(soR, irb.getFalse(), irb);
	}
}


/**
 * PPC_INS_XSXEXPDP, PPC_INS_XSXSIGDP, PPC_INS_XSIEXPDP, PPC_INS_XVXEXPDP,
 * PPC_INS_XVXEXPSP, PPC_INS_XVXSIGDP, PPC_INS_XVXSIGSP, PPC_INS_XVIEXPDP,
 * PPC_INS_XVIEXPSP, PPC_INS_XXEVAL, PPC_INS_XXEXTRACTUW, PPC_INS_XXINSERTW
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVsxBitFields(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_SET(i, pi, irb, (std::set<int>{2, 3, 5}));
	cs_ppc_op ops[5];
	for (unsigned k = 0; k < pi->op_count && k < 5; ++k)
	{
		ops[k] = pi->operands[k];
		if (ops[k].type == PPC_OP_REG && ops[k].reg >= PPC_REG_VF0 && ops[k].reg <= PPC_REG_VF31)
		{
			ops[k].reg = ppc_reg(PPC_REG_V0 + (ops[k].reg - PPC_REG_VF0));
		}
		else if (ops[k].type == PPC_OP_REG && ops[k].reg >= PPC_REG_X0 && ops[k].reg <= PPC_REG_X31)
		{
			ops[k].reg = ppc_reg(PPC_REG_R0 + (ops[k].reg - PPC_REG_X0));
		}
	}
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto c64 = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };
	auto dw0 = [&](unsigned k) {
		return irb.CreateTrunc(irb.CreateLShr(loadOpI128(ops[k], irb), 64), i64);
	};
	auto gpr = [&](unsigned k) {
		return irb.CreateZExtOrTrunc(loadOp(ops[k], irb), i64);
	};
	// Apply fn to each `bits`-wide lane of a (and b), big-endian lane order.
	auto lanes = [&](unsigned bits, llvm::Value* a, llvm::Value* b,
			const std::function<llvm::Value*(llvm::Value*, llvm::Value*)>& fn) {
		auto* ty = irb.getIntNTy(bits);
		llvm::Value* r = llvm::ConstantInt::get(i128, 0);
		for (unsigned sh = 0; sh < 128; sh += bits)
		{
			auto* x = irb.CreateTrunc(irb.CreateLShr(a, sh), ty);
			auto* y = b ? irb.CreateTrunc(irb.CreateLShr(b, sh), ty) : nullptr;
			r = irb.CreateOr(r, irb.CreateShl(irb.CreateZExt(fn(x, y), i128), sh));
		}
		return r;
	};
	auto sig = [&](llvm::Value* x, unsigned bits) {
		unsigned fracBits = bits == 64 ? 52 : 23;
		unsigned expBits = bits == 64 ? 11 : 8;
		auto* ty = x->getType();
		auto* frac = irb.CreateAnd(x, llvm::ConstantInt::get(ty, (uint64_t(1) << fracBits) - 1));
		auto* e = irb.CreateAnd(irb.CreateLShr(x, fracBits), llvm::ConstantInt::get(ty, (1u << expBits) - 1));
		auto* normal = irb.CreateAnd(
				irb.CreateICmpNE(e, llvm::ConstantInt::get(ty, 0)),
				irb.CreateICmpNE(e, llvm::ConstantInt::get(ty, (1u << expBits) - 1)));
		return irb.CreateOr(frac, irb.CreateShl(irb.CreateZExt(normal, ty), fracBits));
	};

	switch (i->id)
	{
	case PPC_INS_XSXEXPDP:
		storeOp(ops[0], irb.CreateAnd(irb.CreateLShr(dw0(1), 52), c64(0x7ff)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	case PPC_INS_XSXSIGDP:
		storeOp(ops[0], sig(dw0(1), 64), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	case PPC_INS_XSIEXPDP:
	{
		auto* a = gpr(1);
		auto* r = irb.CreateOr(
				irb.CreateAnd(a, c64(0x800fffffffffffffULL)),
				irb.CreateShl(irb.CreateAnd(gpr(2), c64(0x7ff)), 52));
		storeOpI128(ops[0], irb.CreateShl(irb.CreateZExt(r, i128), 64), irb);
		return;
	}
	case PPC_INS_XVXEXPDP:
	case PPC_INS_XVXEXPSP:
	{
		bool dp = i->id == PPC_INS_XVXEXPDP;
		auto r = lanes(dp ? 64 : 32, loadOpI128(ops[1], irb), nullptr, [&](llvm::Value* x, llvm::Value*) {
			return irb.CreateAnd(irb.CreateLShr(x, dp ? 52 : 23), llvm::ConstantInt::get(x->getType(), dp ? 0x7ff : 0xff));
		});
		storeOpI128(ops[0], r, irb);
		return;
	}
	case PPC_INS_XVXSIGDP:
	case PPC_INS_XVXSIGSP:
	{
		unsigned bits = i->id == PPC_INS_XVXSIGDP ? 64 : 32;
		auto r = lanes(bits, loadOpI128(ops[1], irb), nullptr, [&](llvm::Value* x, llvm::Value*) {
			return sig(x, bits);
		});
		storeOpI128(ops[0], r, irb);
		return;
	}
	case PPC_INS_XVIEXPDP:
	case PPC_INS_XVIEXPSP:
	{
		bool dp = i->id == PPC_INS_XVIEXPDP;
		auto r = lanes(dp ? 64 : 32, loadOpI128(ops[1], irb), loadOpI128(ops[2], irb), [&](llvm::Value* x, llvm::Value* y) {
			auto* ty = x->getType();
			uint64_t em = dp ? 0x7ffULL : 0xffULL;
			unsigned sh = dp ? 52 : 23;
			return irb.CreateOr(
					irb.CreateAnd(x, llvm::ConstantInt::get(ty, ~(em << sh))),
					irb.CreateShl(irb.CreateAnd(y, llvm::ConstantInt::get(ty, em)), sh));
		});
		storeOpI128(ops[0], r, irb);
		return;
	}
	case PPC_INS_XXEVAL:
	{
		if (pi->op_count != 5 || ops[4].type != PPC_OP_IMM)
		{
			throw GenericError("unexpected xxeval operands");
		}
		auto* a = loadOpI128(ops[1], irb);
		auto* b = loadOpI128(ops[2], irb);
		auto* c = loadOpI128(ops[3], irb);
		unsigned imm = unsigned(ops[4].imm) & 0xff;
		llvm::Value* r = llvm::ConstantInt::get(i128, 0);
		for (unsigned idx = 0; idx < 8; ++idx)
		{
			// IMM bit idx (bit 0 = MSB) selects minterm A*4 + B*2 + C.
			if (!((imm >> (7u - idx)) & 1u))
			{
				continue;
			}
			auto* ta = (idx & 4) ? a : irb.CreateNot(a);
			auto* tb = (idx & 2) ? b : irb.CreateNot(b);
			auto* tc = (idx & 1) ? c : irb.CreateNot(c);
			r = irb.CreateOr(r, irb.CreateAnd(irb.CreateAnd(ta, tb), tc));
		}
		storeOpI128(ops[0], r, irb);
		return;
	}
	case PPC_INS_XXEXTRACTUW:
	case PPC_INS_XXINSERTW:
	{
		unsigned uim = ops[2].type == PPC_OP_IMM ? unsigned(ops[2].imm) & 0xf : 0u;
		if (uim > 12)
		{
			uim = 12;
		}
		unsigned sh = (12u - uim) * 8u;
		auto* src = loadOpI128(ops[1], irb);
		if (i->id == PPC_INS_XXEXTRACTUW)
		{
			auto* w = irb.CreateAnd(irb.CreateLShr(src, sh), llvm::ConstantInt::get(i128, 0xffffffffULL));
			storeOpI128(ops[0], irb.CreateShl(w, 64), irb);
		}
		else
		{
			auto* w = irb.CreateAnd(irb.CreateLShr(src, 64), llvm::ConstantInt::get(i128, 0xffffffffULL));
			auto* mask = llvm::ConstantInt::get(i128, llvm::APInt(128, 0xffffffffULL).shl(sh));
			auto* t = loadOpI128(ops[0], irb);
			storeOpI128(ops[0], irb.CreateOr(irb.CreateAnd(t, irb.CreateNot(mask)), irb.CreateShl(w, sh)), irb);
		}
		return;
	}
	default:
		throw GenericError("unhandled VSX bit-field op");
	}
}


/**
 * Prefixed (ISA 3.1) VSX/FP loads and stores, VSX register-pair loads and
 * stores, load/store rightmost element, load/store with length, LXVWSX and
 * LXVKQ:
 * PPC_INS_PLXV, PPC_INS_PSTXV, PPC_INS_PLFD, PPC_INS_PLFS, PPC_INS_PSTFD,
 * PPC_INS_PSTFS, PPC_INS_PLXSD, PPC_INS_PLXSSP, PPC_INS_PSTXSD,
 * PPC_INS_PSTXSSP, PPC_INS_PLXVP, PPC_INS_PSTXVP, PPC_INS_LXVP,
 * PPC_INS_STXVP, PPC_INS_LXVPX, PPC_INS_STXVPX, PPC_INS_LXVR[BHWD]X,
 * PPC_INS_STXVR[BHWD]X, PPC_INS_LXVL, PPC_INS_LXVLL, PPC_INS_STXVL,
 * PPC_INS_STXVLL, PPC_INS_LXVWSX, PPC_INS_LXVKQ
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVsxMemMisc(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	auto* i8 = irb.getInt8Ty();
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto* dt = getDefaultType();
	auto c64 = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };

	cs_ppc_op op0 = pi->operands[0];
	if (op0.type == PPC_OP_REG && op0.reg >= PPC_REG_VF0 && op0.reg <= PPC_REG_VF31)
	{
		op0.reg = ppc_reg(PPC_REG_V0 + (op0.reg - PPC_REG_VF0));
	}
	auto vsr = [&](unsigned k) {
		cs_ppc_op o = op0;
		o.type = PPC_OP_REG;
		o.reg = ppc_reg(k < 32 ? PPC_REG_VSL0 + k : PPC_REG_V0 + (k - 32));
		return o;
	};

	// Effective address.
	llvm::Value* ea = nullptr;
	bool prefixed = false;
	switch (i->id)
	{
		case PPC_INS_PLXV: case PPC_INS_PSTXV: case PPC_INS_PLFD: case PPC_INS_PLFS:
		case PPC_INS_PSTFD: case PPC_INS_PSTFS: case PPC_INS_PLXSD: case PPC_INS_PLXSSP:
		case PPC_INS_PSTXSD: case PPC_INS_PSTXSSP: case PPC_INS_PLXVP: case PPC_INS_PSTXVP:
			prefixed = true;
			break;
		default:
			break;
	}
	if (prefixed)
	{
		if (i->size < 8)
		{
			translatePseudoAsmGeneric(i, pi, irb);
			return;
		}
		auto be32 = [&](unsigned o) {
			return (uint32_t(i->bytes[o]) << 24) | (uint32_t(i->bytes[o + 1]) << 16)
					| (uint32_t(i->bytes[o + 2]) << 8) | uint32_t(i->bytes[o + 3]);
		};
		uint32_t pfx = be32(0), sfx = be32(4);
		int64_t d = (int64_t(pfx & 0x3ffff) << 16) | (sfx & 0xffff);
		d = (d << 30) >> 30;
		unsigned ra = (sfx >> 16) & 31;
		if ((pfx >> 20) & 1)
		{
			ea = c64(i->address + d);
		}
		else
		{
			llvm::Value* base = ra ? irb.CreateZExtOrTrunc(loadRegister(PPC_REG_R0 + ra, irb), i64) : c64(0);
			ea = irb.CreateAdd(base, c64(d));
		}
		ea = irb.CreateZExtOrTrunc(ea, dt);
	}
	else if (i->id != PPC_INS_LXVKQ)
	{
		if (pi->operands[1].type == PPC_OP_MEM)
		{
			ea = loadOp(pi->operands[1], irb, nullptr, true);
		}
		else
		{
			ea = loadIndexedEffectiveAddress(pi, irb);
		}
	}
	auto off = [&](llvm::Value* a, uint64_t o) {
		return o ? irb.CreateAdd(a, llvm::ConstantInt::get(a->getType(), o)) : a;
	};
	auto ld128 = [&](llvm::Value* a) {
		auto* hi = loadIntPtr(irb, a, i64);
		auto* lo = loadIntPtr(irb, off(a, 8), i64);
		return irb.CreateOr(irb.CreateShl(irb.CreateZExt(hi, i128), 64), irb.CreateZExt(lo, i128));
	};
	auto st128 = [&](llvm::Value* v, llvm::Value* a) {
		storeIntPtr(irb, irb.CreateTrunc(irb.CreateLShr(v, 64), i64), a, i64);
		storeIntPtr(irb, irb.CreateTrunc(v, i64), off(a, 8), i64);
	};
	auto dw0 = [&](llvm::Value* dw) { return irb.CreateShl(irb.CreateZExt(dw, i128), 64); };
	auto pairBase = [&]() -> unsigned {
		uint32_t r = op0.reg;
		if (r >= PPC_REG_VSRP0 && r <= PPC_REG_VSRP31)
		{
			return 2u * (r - PPC_REG_VSRP0);
		}
		throw GenericError("unexpected VSX pair register");
	};

	switch (i->id)
	{
		case PPC_INS_PLXV:
			storeOpI128(op0, ld128(ea), irb);
			return;
		case PPC_INS_PSTXV:
			st128(loadOpI128(op0, irb), ea);
			return;
		case PPC_INS_PLFD:
			storeOp(op0, irb.CreateBitCast(loadIntPtr(irb, ea, i64), irb.getDoubleTy()), irb, eOpConv::FPCAST_OR_BITCAST);
			return;
		case PPC_INS_PLFS:
		{
			auto* f = irb.CreateBitCast(loadIntPtr(irb, ea, irb.getInt32Ty()), irb.getFloatTy());
			storeOp(op0, irb.CreateFPExt(f, irb.getDoubleTy()), irb, eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case PPC_INS_PSTFD:
			storeIntPtr(irb, irb.CreateBitCast(loadOpDouble(op0, irb), i64), ea, i64);
			return;
		case PPC_INS_PSTFS:
		{
			auto* f = irb.CreateFPTrunc(loadOpDouble(op0, irb), irb.getFloatTy());
			storeIntPtr(irb, irb.CreateBitCast(f, irb.getInt32Ty()), ea, irb.getInt32Ty());
			return;
		}
		case PPC_INS_PLXSD:
			storeOpI128(op0, dw0(loadIntPtr(irb, ea, i64)), irb);
			return;
		case PPC_INS_PLXSSP:
		{
			auto* f = irb.CreateBitCast(loadIntPtr(irb, ea, irb.getInt32Ty()), irb.getFloatTy());
			storeOpI128(op0, dw0(irb.CreateBitCast(irb.CreateFPExt(f, irb.getDoubleTy()), i64)), irb);
			return;
		}
		case PPC_INS_PSTXSD:
			storeIntPtr(irb, irb.CreateTrunc(irb.CreateLShr(loadOpI128(op0, irb), 64), i64), ea, i64);
			return;
		case PPC_INS_PSTXSSP:
		{
			auto* d = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(loadOpI128(op0, irb), 64), i64), irb.getDoubleTy());
			auto* f = irb.CreateFPTrunc(d, irb.getFloatTy());
			storeIntPtr(irb, irb.CreateBitCast(f, irb.getInt32Ty()), ea, irb.getInt32Ty());
			return;
		}
		case PPC_INS_PLXVP:
		case PPC_INS_LXVP:
		case PPC_INS_LXVPX:
		{
			unsigned k = pairBase();
			auto a = vsr(k), b = vsr(k + 1);
			auto* va = ld128(ea);
			auto* vb = ld128(off(ea, 16));
			storeOpI128(a, va, irb);
			storeOpI128(b, vb, irb);
			return;
		}
		case PPC_INS_PSTXVP:
		case PPC_INS_STXVP:
		case PPC_INS_STXVPX:
		{
			unsigned k = pairBase();
			auto a = vsr(k), b = vsr(k + 1);
			st128(loadOpI128(a, irb), ea);
			st128(loadOpI128(b, irb), off(ea, 16));
			return;
		}
		case PPC_INS_LXVRBX: case PPC_INS_LXVRHX: case PPC_INS_LXVRWX: case PPC_INS_LXVRDX:
		case PPC_INS_STXVRBX: case PPC_INS_STXVRHX: case PPC_INS_STXVRWX: case PPC_INS_STXVRDX:
		{
			unsigned bits = 8;
			switch (i->id)
			{
				case PPC_INS_LXVRHX: case PPC_INS_STXVRHX: bits = 16; break;
				case PPC_INS_LXVRWX: case PPC_INS_STXVRWX: bits = 32; break;
				case PPC_INS_LXVRDX: case PPC_INS_STXVRDX: bits = 64; break;
				default: break;
			}
			auto* ty = irb.getIntNTy(bits);
			bool load = i->id == PPC_INS_LXVRBX || i->id == PPC_INS_LXVRHX
					|| i->id == PPC_INS_LXVRWX || i->id == PPC_INS_LXVRDX;
			if (load)
			{
				storeOpI128(op0, irb.CreateZExt(loadIntPtr(irb, ea, ty), i128), irb);
			}
			else
			{
				storeIntPtr(irb, irb.CreateTrunc(loadOpI128(op0, irb), ty), ea, ty);
			}
			return;
		}
		case PPC_INS_LXVWSX:
		{
			auto* w = irb.CreateZExt(loadIntPtr(irb, ea, irb.getInt32Ty()), i128);
			llvm::Value* r = w;
			for (unsigned k = 1; k < 4; ++k)
			{
				r = irb.CreateOr(r, irb.CreateShl(w, 32 * k));
			}
			storeOpI128(op0, r, irb);
			return;
		}
		case PPC_INS_LXVL: case PPC_INS_LXVLL: case PPC_INS_STXVL: case PPC_INS_STXVLL:
		{
			// Length is bits 0:7 of RB; bytes beyond it are zeroed (loads)
			// or left untouched (stores).
			cs_ppc_op& rbOp = pi->operands[pi->op_count - 1];
			auto* rb = irb.CreateZExtOrTrunc(loadOp(rbOp, irb), i64);
			auto* n = irb.CreateLShr(rb, 56);
			bool load = i->id == PPC_INS_LXVL || i->id == PPC_INS_LXVLL;
			if (load)
			{
				storeOpI128(op0, llvm::ConstantInt::get(i128, 0), irb);
			}
			auto* src = load ? nullptr : loadOpI128(op0, irb);
			for (unsigned k = 0; k < 16; ++k)
			{
				llvm::IRBuilder<> body(generateIfThen(irb.CreateICmpUGT(n, c64(k)), irb));
				unsigned sh = 8 * (15 - k);
				auto* a = k ? body.CreateAdd(ea, llvm::ConstantInt::get(ea->getType(), k)) : ea;
				if (load)
				{
					auto* b = body.CreateShl(body.CreateZExt(loadIntPtr(body, a, i8), i128), sh);
					storeOpI128(op0, body.CreateOr(loadOpI128(op0, body), b), body);
				}
				else
				{
					storeIntPtr(body, body.CreateTrunc(body.CreateLShr(src, sh), i8), a, i8);
				}
			}
			return;
		}
		case PPC_INS_LXVKQ:
		{
			unsigned uim = pi->operands[1].type == PPC_OP_IMM ? unsigned(pi->operands[1].imm) & 31 : 0;
			static const uint64_t hiTab[10] = {
				0, 0x3fff000000000000ULL, 0x4000000000000000ULL, 0x4000800000000000ULL,
				0x4001000000000000ULL, 0x4001400000000000ULL, 0x4001800000000000ULL,
				0x4001c00000000000ULL, 0x7fff000000000000ULL, 0x7fff800000000000ULL,
			};
			uint64_t hi = 0;
			if (uim >= 1 && uim <= 9) hi = hiTab[uim];
			else if (uim == 16) hi = 0x8000000000000000ULL;
			else if (uim >= 17 && uim <= 24) hi = hiTab[uim - 16] | 0x8000000000000000ULL;
			else
			{
				translatePseudoAsmGeneric(i, pi, irb);
				return;
			}
			storeOpI128(op0, llvm::ConstantInt::get(i128, llvm::APInt(128, hi).shl(64)), irb);
			return;
		}
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
}


/**
 * ISA 3.0/3.1 vector integer odds and ends:
 * PPC_INS_VDIVE[SU][WDQ], PPC_INS_VMUL10[C|E|EC]UQ, PPC_INS_VCMP[SU]Q,
 * PPC_INS_VRLQNM, PPC_INS_VRLQMI, PPC_INS_XXPERMX, PPC_INS_VPDEPD,
 * PPC_INS_VPEXTD, PPC_INS_VCFUGED, PPC_INS_VCLZDM, PPC_INS_VCTZDM,
 * PPC_INS_VGNB, PPC_INS_VBPERMD, PPC_INS_VMSUMUDM, PPC_INS_VMSUMCUD,
 * PPC_INS_VPERMXOR, PPC_INS_VSHASIGMAW, PPC_INS_VSHASIGMAD,
 * PPC_INS_XVTLSBB, PPC_INS_XVCVBF16SPN, PPC_INS_XVCVSPBF16
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecIsa31Ops(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	cs_ppc_op ops[5];
	for (unsigned k = 0; k < pi->op_count && k < 5; ++k)
	{
		ops[k] = pi->operands[k];
		if (ops[k].type == PPC_OP_REG && ops[k].reg >= PPC_REG_VF0 && ops[k].reg <= PPC_REG_VF31)
		{
			ops[k].reg = ppc_reg(PPC_REG_V0 + (ops[k].reg - PPC_REG_VF0));
		}
	}
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto* i256 = irb.getIntNTy(256);
	auto c64 = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };
	auto c128 = [&](uint64_t v) { return llvm::ConstantInt::get(i128, v); };
	auto imm = [&](unsigned k) -> uint64_t {
		return k < pi->op_count && ops[k].type == PPC_OP_IMM ? uint64_t(ops[k].imm) : 0;
	};
	auto vr = [&](unsigned k) { return loadOpI128(ops[k], irb); };
	auto out = [&](llvm::Value* v) { storeOpI128(ops[0], v, irb); };
	// Apply fn lane by lane (lane width `bits`), result lanes OR-ed back.
	auto lanes = [&](unsigned bits, llvm::Value* a, llvm::Value* b,
			const std::function<llvm::Value*(llvm::Value*, llvm::Value*)>& fn) {
		auto* ty = irb.getIntNTy(bits);
		llvm::Value* r = c128(0);
		for (unsigned sh = 0; sh < 128; sh += bits)
		{
			auto* x = irb.CreateTrunc(irb.CreateLShr(a, sh), ty);
			auto* y = b ? irb.CreateTrunc(irb.CreateLShr(b, sh), ty) : nullptr;
			r = irb.CreateOr(r, irb.CreateShl(irb.CreateZExt(fn(x, y), i128), sh));
		}
		return r;
	};
	auto pext = [&](llvm::Value* src, llvm::Value* m) {
		llvm::Value* res = c64(0);
		llvm::Value* k = c64(0);
		for (unsigned j = 0; j < 64; ++j)
		{
			auto* mb = irb.CreateAnd(irb.CreateLShr(m, j), c64(1));
			auto* sb = irb.CreateAnd(irb.CreateLShr(src, j), c64(1));
			res = irb.CreateOr(res, irb.CreateShl(irb.CreateAnd(sb, mb), k));
			k = irb.CreateAdd(k, mb);
		}
		return res;
	};
	auto pdep = [&](llvm::Value* src, llvm::Value* m) {
		llvm::Value* res = c64(0);
		llvm::Value* k = c64(0);
		for (unsigned j = 0; j < 64; ++j)
		{
			auto* mb = irb.CreateAnd(irb.CreateLShr(m, j), c64(1));
			auto* sb = irb.CreateAnd(irb.CreateLShr(src, k), c64(1));
			res = irb.CreateOr(res, irb.CreateShl(irb.CreateAnd(sb, mb), j));
			k = irb.CreateAdd(k, mb);
		}
		return res;
	};
	auto ctpop64 = [&](llvm::Value* v) {
		auto* fn = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctpop, i64);
		return irb.CreateCall(fn, {v});
	};
	auto crField = [&](unsigned k) -> int {
		if (ops[k].type == PPC_OP_REG && ops[k].reg >= PPC_REG_CR0 && ops[k].reg <= PPC_REG_CR7)
		{
			return int(ops[k].reg - PPC_REG_CR0);
		}
		return -1;
	};
	static const uint32_t crb[8][4] = {
		{PPC_REG_CR0LT, PPC_REG_CR0GT, PPC_REG_CR0EQ, PPC_REG_CR0UN},
		{PPC_REG_CR1LT, PPC_REG_CR1GT, PPC_REG_CR1EQ, PPC_REG_CR1UN},
		{PPC_REG_CR2LT, PPC_REG_CR2GT, PPC_REG_CR2EQ, PPC_REG_CR2UN},
		{PPC_REG_CR3LT, PPC_REG_CR3GT, PPC_REG_CR3EQ, PPC_REG_CR3UN},
		{PPC_REG_CR4LT, PPC_REG_CR4GT, PPC_REG_CR4EQ, PPC_REG_CR4UN},
		{PPC_REG_CR5LT, PPC_REG_CR5GT, PPC_REG_CR5EQ, PPC_REG_CR5UN},
		{PPC_REG_CR6LT, PPC_REG_CR6GT, PPC_REG_CR6EQ, PPC_REG_CR6UN},
		{PPC_REG_CR7LT, PPC_REG_CR7GT, PPC_REG_CR7EQ, PPC_REG_CR7UN},
	};
	auto setCr = [&](unsigned bf, llvm::Value* lt, llvm::Value* gt, llvm::Value* eq) {
		storeRegister(crb[bf][0], lt, irb);
		storeRegister(crb[bf][1], gt, irb);
		storeRegister(crb[bf][2], eq, irb);
		storeRegister(crb[bf][3], irb.getFalse(), irb);
	};
	auto rotr = [&](llvm::Value* x, unsigned n) {
		unsigned w = x->getType()->getIntegerBitWidth();
		return irb.CreateOr(irb.CreateLShr(x, n), irb.CreateShl(x, w - n));
	};

	switch (i->id)
	{
		case PPC_INS_VDIVESW: case PPC_INS_VDIVEUW:
		case PPC_INS_VDIVESD: case PPC_INS_VDIVEUD:
		case PPC_INS_VDIVESQ: case PPC_INS_VDIVEUQ:
		{
			if (pi->op_count != 3) break;
			unsigned bits = 32;
			if (i->id == PPC_INS_VDIVESD || i->id == PPC_INS_VDIVEUD) bits = 64;
			if (i->id == PPC_INS_VDIVESQ || i->id == PPC_INS_VDIVEUQ) bits = 128;
			bool sgn = i->id == PPC_INS_VDIVESW || i->id == PPC_INS_VDIVESD || i->id == PPC_INS_VDIVESQ;
			out(lanes(bits, vr(1), vr(2), [&](llvm::Value* x, llvm::Value* y) {
				auto* wty = irb.getIntNTy(2 * bits);
				auto* dividend = irb.CreateShl(sgn ? irb.CreateSExt(x, wty) : irb.CreateZExt(x, wty), bits);
				auto* zero = irb.CreateICmpEQ(y, llvm::ConstantInt::get(y->getType(), 0));
				auto* yy = irb.CreateSelect(zero, llvm::ConstantInt::get(y->getType(), 1), y);
				auto* div = sgn ? irb.CreateSDiv(dividend, irb.CreateSExt(yy, wty))
						: irb.CreateUDiv(dividend, irb.CreateZExt(yy, wty));
				// Division by zero is undefined; zero is produced.
				return irb.CreateSelect(zero, llvm::ConstantInt::get(x->getType(), 0), irb.CreateTrunc(div, x->getType()));
			}));
			return;
		}
		case PPC_INS_VMUL10UQ: case PPC_INS_VMUL10CUQ:
		case PPC_INS_VMUL10EUQ: case PPC_INS_VMUL10ECUQ:
		{
			bool ext = i->id == PPC_INS_VMUL10EUQ || i->id == PPC_INS_VMUL10ECUQ;
			bool carry = i->id == PPC_INS_VMUL10CUQ || i->id == PPC_INS_VMUL10ECUQ;
			if (ext && pi->op_count != 3) break;
			auto* p = irb.CreateMul(irb.CreateZExt(vr(1), i256), llvm::ConstantInt::get(i256, 10));
			if (ext)
			{
				p = irb.CreateAdd(p, irb.CreateZExt(irb.CreateAnd(vr(2), c128(0xf)), i256));
			}
			out(irb.CreateTrunc(carry ? irb.CreateLShr(p, 128) : p, i128));
			return;
		}
		case PPC_INS_VCMPSQ: case PPC_INS_VCMPUQ:
		{
			int bf = crField(0);
			if (bf < 0 || pi->op_count != 3) break;
			auto* a = vr(1);
			auto* b = vr(2);
			bool s = i->id == PPC_INS_VCMPSQ;
			setCr(unsigned(bf), s ? irb.CreateICmpSLT(a, b) : irb.CreateICmpULT(a, b),
					s ? irb.CreateICmpSGT(a, b) : irb.CreateICmpUGT(a, b), irb.CreateICmpEQ(a, b));
			return;
		}
		case PPC_INS_VRLQNM: case PPC_INS_VRLQMI:
		{
			if (pi->op_count != 3) break;
			auto* a = vr(1);
			auto* b = vr(2);
			auto* sh = irb.CreateAnd(irb.CreateLShr(b, 64), c128(127));
			auto* me = irb.CreateAnd(irb.CreateLShr(b, 72), c128(127));
			auto* mb = irb.CreateAnd(irb.CreateLShr(b, 80), c128(127));
			auto* rsh = irb.CreateAnd(irb.CreateSub(c128(128), sh), c128(127));
			auto* rot = irb.CreateOr(irb.CreateShl(a, sh), irb.CreateLShr(a, rsh));
			auto* ones = llvm::ConstantInt::get(i128, llvm::APInt::getAllOnes(128));
			auto* m1 = irb.CreateLShr(ones, mb);
			auto* m2 = irb.CreateShl(ones, irb.CreateSub(c128(127), me));
			auto* mask = irb.CreateSelect(irb.CreateICmpULE(mb, me), irb.CreateAnd(m1, m2), irb.CreateOr(m1, m2));
			auto* r = irb.CreateAnd(rot, mask);
			if (i->id == PPC_INS_VRLQMI)
			{
				r = irb.CreateOr(r, irb.CreateAnd(vr(0), irb.CreateNot(mask)));
			}
			out(r);
			return;
		}
		case PPC_INS_XXPERMX:
		{
			if (pi->op_count != 5) break;
			auto* ab = irb.CreateOr(irb.CreateShl(irb.CreateZExt(vr(1), i256), 128), irb.CreateZExt(vr(2), i256));
			auto* c = vr(3);
			uint64_t uim = imm(4) & 7;
			llvm::Value* r = c128(0);
			for (unsigned k = 0; k < 16; ++k)
			{
				auto* idx = irb.CreateAnd(irb.CreateLShr(c, 8 * (15 - k)), c128(0xff));
				auto* sel = irb.CreateICmpEQ(irb.CreateLShr(idx, 5), c128(uim));
				auto* pos = irb.CreateZExt(irb.CreateSub(c128(31), irb.CreateAnd(idx, c128(31))), i256);
				auto* byte = irb.CreateTrunc(irb.CreateAnd(irb.CreateLShr(ab, irb.CreateShl(pos, 3)), llvm::ConstantInt::get(i256, 0xff)), i128);
				byte = irb.CreateSelect(sel, byte, c128(0));
				r = irb.CreateOr(r, irb.CreateShl(byte, 8 * (15 - k)));
			}
			out(r);
			return;
		}
		case PPC_INS_VPDEPD: case PPC_INS_VPEXTD: case PPC_INS_VCFUGED:
		case PPC_INS_VCLZDM: case PPC_INS_VCTZDM:
		{
			if (pi->op_count != 3) break;
			auto id = i->id;
			out(lanes(64, vr(1), vr(2), [&](llvm::Value* s, llvm::Value* m) -> llvm::Value* {
				switch (id)
				{
					case PPC_INS_VPDEPD: return pdep(s, m);
					case PPC_INS_VPEXTD: return pext(s, m);
					case PPC_INS_VCFUGED:
					{
						auto* lo = pext(s, m);
						auto* hi = pext(s, irb.CreateNot(m));
						auto* n = ctpop64(m);
						auto* full = irb.CreateICmpEQ(n, c64(64));
						auto* hs = irb.CreateShl(hi, irb.CreateSelect(full, c64(0), n));
						return irb.CreateOr(irb.CreateSelect(full, c64(0), hs), lo);
					}
					case PPC_INS_VCLZDM:
					{
						auto* e = pext(s, m);
						auto* n = ctpop64(m);
						auto* fn = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctlz, i64);
						return irb.CreateSub(irb.CreateCall(fn, {e, irb.getFalse()}), irb.CreateSub(c64(64), n));
					}
					default:
					{
						auto* e = pext(s, m);
						auto* n = ctpop64(m);
						auto* fn = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::cttz, i64);
						auto* t = irb.CreateCall(fn, {e, irb.getFalse()});
						return irb.CreateSelect(irb.CreateICmpULT(t, n), t, n);
					}
				}
			}));
			return;
		}
		case PPC_INS_VGNB:
		{
			if (pi->op_count != 3) break;
			unsigned n = unsigned(imm(2));
			if (n < 2 || n > 7) break;
			auto* src = vr(1);
			unsigned cnt = (128 + n - 1) / n;
			llvm::Value* r = c64(0);
			for (unsigned j = 0; j < cnt; ++j)
			{
				unsigned bit = j * n; // big-endian bit number
				auto* b = irb.CreateTrunc(irb.CreateAnd(irb.CreateLShr(src, 127 - bit), c128(1)), i64);
				r = irb.CreateOr(r, irb.CreateShl(b, cnt - 1 - j));
			}
			storeOp(ops[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		case PPC_INS_VBPERMD:
		{
			if (pi->op_count != 3) break;
			out(lanes(64, vr(1), vr(2), [&](llvm::Value* a, llvm::Value* b) {
				llvm::Value* v = c64(0);
				for (unsigned k = 0; k < 8; ++k)
				{
					auto* idx = irb.CreateAnd(irb.CreateLShr(b, 56 - 8 * k), c64(0xff));
					auto* ok = irb.CreateICmpULT(idx, c64(64));
					auto* sh = irb.CreateSelect(ok, irb.CreateSub(c64(63), idx), c64(0));
					auto* bt = irb.CreateSelect(ok, irb.CreateAnd(irb.CreateLShr(a, sh), c64(1)), c64(0));
					v = irb.CreateOr(v, irb.CreateShl(bt, 7 - k));
				}
				return v;
			}));
			return;
		}
		case PPC_INS_VMSUMUDM: case PPC_INS_VMSUMCUD:
		{
			if (pi->op_count != 4) break;
			auto* a = vr(1);
			auto* b = vr(2);
			auto dw = [&](llvm::Value* v, unsigned k) {
				return irb.CreateZExt(irb.CreateTrunc(irb.CreateLShr(v, 64 * k), i64), i256);
			};
			auto* s = irb.CreateAdd(irb.CreateMul(dw(a, 1), dw(b, 1)), irb.CreateMul(dw(a, 0), dw(b, 0)));
			s = irb.CreateAdd(s, irb.CreateZExt(vr(3), i256));
			out(irb.CreateTrunc(i->id == PPC_INS_VMSUMCUD ? irb.CreateLShr(s, 128) : s, i128));
			return;
		}
		case PPC_INS_VPERMXOR:
		{
			if (pi->op_count != 4) break;
			auto* a = vr(1);
			auto* b = vr(2);
			auto* c = vr(3);
			llvm::Value* r = c128(0);
			auto byteAt = [&](llvm::Value* v, llvm::Value* idx) {
				auto* sh = irb.CreateShl(irb.CreateSub(c128(15), idx), 3);
				return irb.CreateAnd(irb.CreateLShr(v, sh), c128(0xff));
			};
			for (unsigned k = 0; k < 16; ++k)
			{
				auto* ctl = irb.CreateAnd(irb.CreateLShr(c, 8 * (15 - k)), c128(0xff));
				auto* x = irb.CreateXor(byteAt(a, irb.CreateLShr(ctl, 4)), byteAt(b, irb.CreateAnd(ctl, c128(0xf))));
				r = irb.CreateOr(r, irb.CreateShl(x, 8 * (15 - k)));
			}
			out(r);
			return;
		}
		case PPC_INS_VSHASIGMAW: case PPC_INS_VSHASIGMAD:
		{
			if (pi->op_count != 4) break;
			bool w = i->id == PPC_INS_VSHASIGMAW;
			unsigned st = unsigned(imm(2)) & 1, six = unsigned(imm(3)) & 0xf;
			auto* src = vr(1);
			unsigned bits = w ? 32 : 64;
			llvm::Value* r = c128(0);
			for (unsigned k = 0; k < 128 / bits; ++k)
			{
				// Lane k counts from the most significant element.
				unsigned sh = 128 - bits * (k + 1);
				auto* x = irb.CreateTrunc(irb.CreateLShr(src, sh), irb.getIntNTy(bits));
				unsigned sbit = w ? (six >> (3 - k)) & 1 : (six >> (3 - 2 * k)) & 1;
				llvm::Value* v = nullptr;
				if (w)
				{
					if (!st) v = sbit ? irb.CreateXor(irb.CreateXor(rotr(x, 17), rotr(x, 19)), irb.CreateLShr(x, 10))
							: irb.CreateXor(irb.CreateXor(rotr(x, 7), rotr(x, 18)), irb.CreateLShr(x, 3));
					else v = sbit ? irb.CreateXor(irb.CreateXor(rotr(x, 6), rotr(x, 11)), rotr(x, 25))
							: irb.CreateXor(irb.CreateXor(rotr(x, 2), rotr(x, 13)), rotr(x, 22));
				}
				else
				{
					if (!st) v = sbit ? irb.CreateXor(irb.CreateXor(rotr(x, 19), rotr(x, 61)), irb.CreateLShr(x, 6))
							: irb.CreateXor(irb.CreateXor(rotr(x, 1), rotr(x, 8)), irb.CreateLShr(x, 7));
					else v = sbit ? irb.CreateXor(irb.CreateXor(rotr(x, 14), rotr(x, 18)), rotr(x, 41))
							: irb.CreateXor(irb.CreateXor(rotr(x, 28), rotr(x, 34)), rotr(x, 39));
				}
				r = irb.CreateOr(r, irb.CreateShl(irb.CreateZExt(v, i128), sh));
			}
			out(r);
			return;
		}
		case PPC_INS_XVTLSBB:
		{
			int bf = crField(0);
			if (bf < 0) break;
			auto* lsb = irb.CreateAnd(vr(1), llvm::ConstantInt::get(i128, llvm::APInt::getSplat(128, llvm::APInt(8, 1))));
			auto* allOnes = irb.CreateICmpEQ(lsb, llvm::ConstantInt::get(i128, llvm::APInt::getSplat(128, llvm::APInt(8, 1))));
			auto* allZero = irb.CreateICmpEQ(lsb, c128(0));
			setCr(unsigned(bf), allOnes, irb.getFalse(), allZero);
			return;
		}
		case PPC_INS_XVCVBF16SPN:
			out(lanes(32, vr(1), nullptr, [&](llvm::Value* x, llvm::Value*) {
				return irb.CreateShl(x, 16);
			}));
			return;
		case PPC_INS_XVCVSPBF16:
			out(lanes(32, vr(1), nullptr, [&](llvm::Value* x, llvm::Value*) {
				auto* ty = x->getType();
				auto c = [&](uint64_t v) { return llvm::ConstantInt::get(ty, v); };
				auto* isNan = irb.CreateICmpUGT(irb.CreateAnd(x, c(0x7fffffff)), c(0x7f800000));
				auto* lsb = irb.CreateAnd(irb.CreateLShr(x, 16), c(1));
				auto* rounded = irb.CreateLShr(irb.CreateAdd(irb.CreateAdd(x, c(0x7fff)), lsb), 16);
				auto* nan = irb.CreateOr(irb.CreateLShr(x, 16), c(0x40));
				return irb.CreateSelect(isNan, nan, rounded);
			}));
			return;
		default:
			break;
	}
	translatePseudoAsmGeneric(i, pi, irb);
}

/**
 * AES VCIPHER[LAST]/VNCIPHER[LAST]/VSBOX as the llvm.ppc.altivec.crypto
 * intrinsics, DARN as llvm.ppc.darn32/darn/darnraw by L, the half-precision
 * conversions XSCVHPDP/XSCVDPHP/XVCVHPSP/XVCVSPHP, and XXGENPCV[BHWD]M
 * permute-control generation (BE/LE expansion and compression).
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVecCryptoHalf(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 1)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	cs_ppc_op ops[4];
	for (unsigned k = 0; k < pi->op_count && k < 4; ++k)
	{
		ops[k] = pi->operands[k];
		if (ops[k].type == PPC_OP_REG && ops[k].reg >= PPC_REG_VF0 && ops[k].reg <= PPC_REG_VF31)
		{
			ops[k].reg = ppc_reg(PPC_REG_V0 + (ops[k].reg - PPC_REG_VF0));
		}
		if (ops[k].type == PPC_OP_REG && ops[k].reg >= PPC_REG_X0 && ops[k].reg <= PPC_REG_X31)
		{
			ops[k].reg = ppc_reg(PPC_REG_R0 + (ops[k].reg - PPC_REG_X0));
		}
	}
	auto* i16 = irb.getInt16Ty();
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto* v2i64 = llvm::FixedVectorType::get(i64, 2);
	auto c128 = [&](uint64_t v) { return llvm::ConstantInt::get(i128, v); };
	auto vr = [&](unsigned k) { return loadOpI128(ops[k], irb); };
	auto out = [&](llvm::Value* v) { storeOpI128(ops[0], v, irb); };
	auto intr = [&](llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Value*> args) -> llvm::Value* {
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, id);
		return irb.CreateCall(f, args);
	};
	auto c64 = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };
	// Half -> float/double, exact: the magnitude is an integer times 2^-24.
	auto halfToFp = [&](llvm::Value* h, llvm::Type* fty) -> llvm::Value* {
		auto* h64 = irb.CreateZExt(h, i64);
		auto* e = irb.CreateAnd(irb.CreateLShr(h64, 10), c64(31));
		auto* m = irb.CreateAnd(h64, c64(0x3ff));
		auto* sgn = irb.CreateICmpNE(irb.CreateAnd(h64, c64(0x8000)), c64(0));
		auto* isSub = irb.CreateICmpEQ(e, c64(0));
		auto* norm = irb.CreateShl(irb.CreateOr(m, c64(0x400)), irb.CreateSelect(isSub, c64(0), irb.CreateSub(e, c64(1))));
		auto* mag = irb.CreateFMul(irb.CreateUIToFP(irb.CreateSelect(isSub, m, norm), fty),
				llvm::ConstantFP::get(fty, std::ldexp(1.0, -24)));
		llvm::Value* special = nullptr;
		if (fty->isDoubleTy())
		{
			special = irb.CreateOr(c64(0x7ff0000000000000ULL), irb.CreateShl(m, 42));
			special = irb.CreateSelect(irb.CreateICmpNE(m, c64(0)), irb.CreateOr(special, c64(1ULL << 51)), special);
			special = irb.CreateBitCast(special, fty);
		}
		else
		{
			auto* m32 = irb.CreateTrunc(m, i32);
			llvm::Value* sp = irb.CreateOr(irb.getInt32(0x7f800000), irb.CreateShl(m32, 13));
			sp = irb.CreateSelect(irb.CreateICmpNE(m32, irb.getInt32(0)), irb.CreateOr(sp, irb.getInt32(1u << 22)), sp);
			special = irb.CreateBitCast(sp, fty);
		}
		llvm::Value* v = irb.CreateSelect(irb.CreateICmpEQ(e, c64(31)), special, mag);
		return irb.CreateSelect(sgn, irb.CreateFNeg(v), v);
	};
	// Double -> half with round-to-nearest-even, built on the integer bits.
	auto dblToHalf = [&](llvm::Value* d) -> llvm::Value* {
		auto* b = irb.CreateBitCast(d, i64);
		auto* sgn = irb.CreateShl(irb.CreateLShr(b, 63), 15);
		auto* E = irb.CreateAnd(irb.CreateLShr(b, 52), c64(0x7ff));
		auto* M = irb.CreateAnd(b, c64((1ULL << 52) - 1));
		auto* S = irb.CreateOr(M, c64(1ULL << 52));
		auto* e = irb.CreateSub(E, c64(1023));
		auto* normal = irb.CreateICmpSGE(e, llvm::ConstantInt::getSigned(i64, -14));
		llvm::Value* sh = irb.CreateSelect(normal, c64(42), irb.CreateSub(c64(28), e));
		sh = irb.CreateSelect(irb.CreateICmpUGT(sh, c64(63)), c64(63), sh);
		auto* base = irb.CreateSelect(normal, irb.CreateShl(irb.CreateAdd(e, c64(14)), 10), c64(0));
		auto* q = irb.CreateLShr(S, sh);
		auto* rem = irb.CreateAnd(S, irb.CreateSub(irb.CreateShl(c64(1), sh), c64(1)));
		auto* halfway = irb.CreateShl(c64(1), irb.CreateSub(sh, c64(1)));
		auto* inc = irb.CreateOr(irb.CreateICmpUGT(rem, halfway),
				irb.CreateAnd(irb.CreateICmpEQ(rem, halfway), irb.CreateICmpNE(irb.CreateAnd(q, c64(1)), c64(0))));
		llvm::Value* r = irb.CreateAdd(irb.CreateAdd(base, q), irb.CreateZExt(inc, i64));
		r = irb.CreateSelect(irb.CreateICmpSGT(e, c64(15)), c64(0x7c00), r);
		auto* nan = irb.CreateOr(c64(0x7e00), irb.CreateLShr(M, 42));
		auto* special = irb.CreateSelect(irb.CreateICmpEQ(M, c64(0)), c64(0x7c00), nan);
		r = irb.CreateSelect(irb.CreateICmpEQ(E, c64(0x7ff)), special, r);
		return irb.CreateTrunc(irb.CreateOr(r, sgn), i16);
	};

	switch (i->id)
	{
		case PPC_INS_DARN:
		{
			if (pi->op_count != 2 || ops[1].type != PPC_OP_IMM || ops[1].imm > 2)
			{
				break;
			}
			llvm::Value* r = ops[1].imm == 0 ? irb.CreateZExt(intr(llvm::Intrinsic::ppc_darn32, {}), i64)
					: intr(ops[1].imm == 1 ? llvm::Intrinsic::ppc_darn : llvm::Intrinsic::ppc_darnraw, {});
			storeRegister(ops[0].reg, irb.CreateZExtOrTrunc(r, getDefaultType()), irb);
			return;
		}
		case PPC_INS_VCIPHER:
		case PPC_INS_VCIPHERLAST:
		case PPC_INS_VNCIPHER:
		case PPC_INS_VNCIPHERLAST:
		case PPC_INS_VSBOX:
		{
			bool sbox = i->id == PPC_INS_VSBOX;
			if (pi->op_count != (sbox ? 2u : 3u))
			{
				break;
			}
			llvm::Intrinsic::ID id = llvm::Intrinsic::ppc_altivec_crypto_vsbox;
			switch (i->id)
			{
				case PPC_INS_VCIPHER: id = llvm::Intrinsic::ppc_altivec_crypto_vcipher; break;
				case PPC_INS_VCIPHERLAST: id = llvm::Intrinsic::ppc_altivec_crypto_vcipherlast; break;
				case PPC_INS_VNCIPHER: id = llvm::Intrinsic::ppc_altivec_crypto_vncipher; break;
				case PPC_INS_VNCIPHERLAST: id = llvm::Intrinsic::ppc_altivec_crypto_vncipherlast; break;
				default: break;
			}
			std::vector<llvm::Value*> args = {irb.CreateBitCast(vr(1), v2i64)};
			if (!sbox) args.push_back(irb.CreateBitCast(vr(2), v2i64));
			out(irb.CreateBitCast(intr(id, args), i128));
			return;
		}
		case PPC_INS_XSCVHPDP:
		{
			if (pi->op_count != 2) break;
			auto* d = irb.CreateBitCast(halfToFp(irb.CreateTrunc(irb.CreateLShr(vr(1), 64), i16), irb.getDoubleTy()), i64);
			out(irb.CreateShl(irb.CreateZExt(d, i128), 64));
			return;
		}
		case PPC_INS_XSCVDPHP:
		{
			if (pi->op_count != 2) break;
			auto* d = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(vr(1), 64), i64), irb.getDoubleTy());
			out(irb.CreateShl(irb.CreateZExt(dblToHalf(d), i128), 64));
			return;
		}
		case PPC_INS_XVCVHPSP:
		case PPC_INS_XVCVSPHP:
		{
			if (pi->op_count != 2) break;
			bool toSp = i->id == PPC_INS_XVCVHPSP;
			auto* b = vr(1);
			llvm::Value* r = c128(0);
			for (unsigned sh = 0; sh < 128; sh += 32)
			{
				auto* w = irb.CreateTrunc(irb.CreateLShr(b, sh), i32);
				llvm::Value* e;
				if (toSp)
				{
					e = irb.CreateBitCast(halfToFp(irb.CreateTrunc(w, i16), irb.getFloatTy()), i32);
				}
				else
				{
					auto* f = irb.CreateFPExt(irb.CreateBitCast(w, irb.getFloatTy()), irb.getDoubleTy());
					e = irb.CreateZExt(dblToHalf(f), i32);
				}
				r = irb.CreateOr(r, irb.CreateShl(irb.CreateZExt(e, i128), sh));
			}
			out(r);
			return;
		}
		case PPC_INS_XXGENPCVBM:
		case PPC_INS_XXGENPCVHM:
		case PPC_INS_XXGENPCVWM:
		case PPC_INS_XXGENPCVDM:
		{
			if (pi->op_count != 3 || ops[2].type != PPC_OP_IMM || ops[2].imm > 3) break;
			unsigned sz = i->id == PPC_INS_XXGENPCVBM ? 1 : i->id == PPC_INS_XXGENPCVHM ? 2
					: i->id == PPC_INS_XXGENPCVWM ? 4 : 8;
			bool le = ops[2].imm >= 2;
			bool comp = ops[2].imm & 1;
			auto* b = vr(1);
			// Byte offset (big-endian numbering) of an element, its mask bit, and
			// the running count j of selected bytes before it.
			auto msbOf = [&](unsigned p) {
				return irb.CreateTrunc(irb.CreateLShr(b, 127 - 8 * p), irb.getInt1Ty());
			};
			llvm::Value* r = nullptr;
			if (!comp)
			{
				// Start from the all-zero-mask result and overwrite selected elements.
				uint64_t hi = 0, lo = 0;
				for (unsigned p = 0; p < 16; ++p)
				{
					uint64_t v = le ? 0x1f - p : 0x10 + p;
					if (p < 8) hi |= v << (8 * (7 - p));
					else lo |= v << (8 * (15 - p));
				}
				r = llvm::ConstantInt::get(i128, (llvm::APInt(128, hi).shl(64)) | llvm::APInt(128, lo));
			}
			else
			{
				r = c128(0);
			}
			llvm::Value* j = c128(0);
			for (unsigned i0 = 0; i0 < 16; i0 += sz)
			{
				unsigned p = le ? 16 - i0 - sz : i0;
				auto* sel = msbOf(p);
				if (!comp)
				{
					// Bytes p+pos(k) get j+k; pos(k) = k (BE) or sz-1-k (LE).
					llvm::Value* pat = c128(0);
					for (unsigned k = 0; k < sz; ++k)
					{
						unsigned bytePos = p + (le ? sz - 1 - k : k);
						pat = irb.CreateOr(pat, irb.CreateShl(irb.CreateAdd(j, c128(k)), 8 * (15 - bytePos)));
					}
					llvm::APInt fieldMask = llvm::APInt::getBitsSet(128, 8 * (16 - p - sz), 8 * (16 - p));
					auto* cleared = irb.CreateAnd(r, llvm::ConstantInt::get(i128, ~fieldMask));
					r = irb.CreateSelect(sel, irb.CreateOr(cleared, pat), r);
				}
				else
				{
					// The element's scan indices i0+k land at destination byte d+pos(k),
					// d = j (BE) or 16-j-sz (LE).
					uint64_t pat = 0;
					for (unsigned t = 0; t < sz; ++t)
					{
						uint64_t v = le ? i0 + sz - 1 - t : i0 + t;
						pat |= v << (8 * (sz - 1 - t));
					}
					llvm::Value* shift = le ? irb.CreateMul(j, c128(8))
							: irb.CreateMul(irb.CreateSub(c128(16 - sz), j), c128(8));
					auto* placed = irb.CreateShl(c128(pat), shift);
					r = irb.CreateSelect(sel, irb.CreateOr(r, placed), r);
				}
				j = irb.CreateAdd(j, irb.CreateSelect(sel, c128(sz), c128(0)));
			}
			out(r);
			return;
		}
		default:
			break;
	}
	translatePseudoAsmGeneric(i, pi, irb);
}

/**
 * Quad-precision VSX (XS*QP) on fp128: arithmetic (the round-to-odd O forms
 * round to nearest here), fused multiply-add, compares to CR or mask, C-style
 * max/min, conversions with saturation, and the sign/exponent/significand
 * bit operations. Also XSCMPEXPDP/QP and the data-class tests
 * XSTSTDC{SP,DP,QP} and XVTSTDC{SP,DP}. FPSCR flags and FPCC are not
 * modelled; XSRQPI/XSRQPXP (round to integer) are not lifted.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVsxQuad(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2 || pi->op_count > 3)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	cs_ppc_op ops[3];
	for (unsigned k = 0; k < pi->op_count; ++k)
	{
		ops[k] = pi->operands[k];
		if (ops[k].type == PPC_OP_REG && ops[k].reg >= PPC_REG_VF0 && ops[k].reg <= PPC_REG_VF31)
		{
			ops[k].reg = ppc_reg(PPC_REG_V0 + (ops[k].reg - PPC_REG_VF0));
		}
	}
	auto* i1 = irb.getInt1Ty();
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto* f128 = llvm::Type::getFP128Ty(irb.getContext());
	auto* f64 = irb.getDoubleTy();
	auto c128 = [&](uint64_t v) { return llvm::ConstantInt::get(i128, v); };
	auto vr = [&](unsigned k) { return loadOpI128(ops[k], irb); };
	auto qf = [&](unsigned k) { return irb.CreateBitCast(vr(k), f128); };
	auto out = [&](llvm::Value* v) { storeOpI128(ops[0], v, irb); };
	auto outF = [&](llvm::Value* f) { out(irb.CreateBitCast(f, i128)); };
	auto hiDw = [&](llvm::Value* v) { return irb.CreateTrunc(irb.CreateLShr(v, 64), i64); };
	auto toHiDw = [&](llvm::Value* v64) { return irb.CreateShl(irb.CreateZExt(v64, i128), 64); };
	auto signBit = llvm::APInt::getOneBitSet(128, 127);
	auto setCr = [&](llvm::Value* lt, llvm::Value* gt, llvm::Value* eq, llvm::Value* un) {
		uint32_t crReg = ops[0].type == PPC_OP_REG && ops[0].reg >= PPC_REG_CR0 && ops[0].reg <= PPC_REG_CR7
				? ops[0].reg : PPC_REG_CR0;
		uint32_t ltR = PPC_REG_CR0LT, gtR = PPC_REG_CR0GT, eqR = PPC_REG_CR0EQ, unR = PPC_REG_CR0UN;
		crFieldRegisters(crReg, ltR, gtR, eqR, unR);
		storeRegister(ltR, lt, irb);
		storeRegister(gtR, gt, irb);
		storeRegister(eqR, eq, irb);
		storeRegister(unR, un, irb);
	};
	// Fields of an IEEE value held in the low `w` bits of x.
	struct Fields { llvm::Value* sign; llvm::Value* exp; llvm::Value* frac; llvm::Value* expMax; };
	auto fields = [&](llvm::Value* x, unsigned eBits, unsigned fBits) {
		auto* ty = x->getType();
		Fields f;
		f.sign = irb.CreateTrunc(irb.CreateLShr(x, eBits + fBits), i1);
		f.exp = irb.CreateAnd(irb.CreateLShr(x, fBits), llvm::ConstantInt::get(ty, (1ull << eBits) - 1));
		f.frac = irb.CreateAnd(x, llvm::ConstantInt::get(ty, llvm::APInt::getLowBitsSet(ty->getIntegerBitWidth(), fBits)));
		f.expMax = irb.CreateICmpEQ(f.exp, llvm::ConstantInt::get(ty, (1ull << eBits) - 1));
		return f;
	};
	auto isNan = [&](const Fields& f) {
		return irb.CreateAnd(f.expMax, irb.CreateICmpNE(f.frac, llvm::ConstantInt::get(f.frac->getType(), 0)));
	};
	auto dataClass = [&](llvm::Value* x, unsigned eBits, unsigned fBits, uint64_t dcmx) -> llvm::Value* {
		auto f = fields(x, eBits, fBits);
		auto* fz = irb.CreateICmpEQ(f.frac, llvm::ConstantInt::get(f.frac->getType(), 0));
		auto* ez = irb.CreateICmpEQ(f.exp, llvm::ConstantInt::get(f.exp->getType(), 0));
		auto* nan = irb.CreateAnd(f.expMax, irb.CreateNot(fz));
		auto* inf = irb.CreateAnd(f.expMax, fz);
		auto* zero = irb.CreateAnd(ez, fz);
		auto* den = irb.CreateAnd(ez, irb.CreateNot(fz));
		auto* pos = irb.CreateNot(f.sign);
		llvm::Value* m = irb.getFalse();
		if (dcmx & 0x40) m = irb.CreateOr(m, nan);
		if (dcmx & 0x20) m = irb.CreateOr(m, irb.CreateAnd(inf, pos));
		if (dcmx & 0x10) m = irb.CreateOr(m, irb.CreateAnd(inf, f.sign));
		if (dcmx & 0x08) m = irb.CreateOr(m, irb.CreateAnd(zero, pos));
		if (dcmx & 0x04) m = irb.CreateOr(m, irb.CreateAnd(zero, f.sign));
		if (dcmx & 0x02) m = irb.CreateOr(m, irb.CreateAnd(den, pos));
		if (dcmx & 0x01) m = irb.CreateOr(m, irb.CreateAnd(den, f.sign));
		return m;
	};
	auto fp128Const = [&](double d) { return llvm::ConstantFP::get(f128, d); };
	// Saturating truncating conversion of fp128 to an n-bit integer (NaN gives
	// the minimum for signed, 0 for unsigned), widened to i128.
	auto cvtSat = [&](llvm::Value* x, unsigned n, bool sgn) -> llvm::Value* {
		auto* ity = irb.getIntNTy(n);
		auto* nan = irb.CreateFCmpUNO(x, x);
		llvm::Value* mx = llvm::ConstantInt::get(ity, sgn ? llvm::APInt::getSignedMaxValue(n) : llvm::APInt::getMaxValue(n));
		llvm::Value* mn = llvm::ConstantInt::get(ity, sgn ? llvm::APInt::getSignedMinValue(n) : llvm::APInt(n, 0));
		auto* hiF = fp128Const(std::ldexp(1.0, sgn ? int(n) - 1 : int(n)));
		auto* loF = sgn ? fp128Const(-std::ldexp(1.0, int(n) - 1)) : fp128Const(0.0);
		auto* tooBig = irb.CreateFCmpOGE(x, hiF);
		auto* tooSmall = irb.CreateFCmpOLT(x, loF);
		auto* safe = irb.CreateSelect(irb.CreateOr(nan, irb.CreateOr(tooBig, tooSmall)), fp128Const(0.0), x);
		llvm::Value* v = sgn ? irb.CreateFPToSI(safe, ity) : irb.CreateFPToUI(safe, ity);
		v = irb.CreateSelect(tooBig, mx, irb.CreateSelect(irb.CreateOr(tooSmall, nan), mn, v));
		return sgn ? irb.CreateSExt(v, i128) : irb.CreateZExt(v, i128);
	};

	switch (i->id)
	{
		case PPC_INS_XSABSQP: out(irb.CreateAnd(vr(1), llvm::ConstantInt::get(i128, ~signBit))); return;
		case PPC_INS_XSNABSQP: out(irb.CreateOr(vr(1), llvm::ConstantInt::get(i128, signBit))); return;
		case PPC_INS_XSNEGQP: out(irb.CreateXor(vr(1), llvm::ConstantInt::get(i128, signBit))); return;
		case PPC_INS_XSCPSGNQP:
			out(irb.CreateOr(irb.CreateAnd(vr(1), llvm::ConstantInt::get(i128, signBit)),
					irb.CreateAnd(vr(2), llvm::ConstantInt::get(i128, ~signBit))));
			return;
		case PPC_INS_XSXEXPQP:
		{
			auto f = fields(vr(1), 15, 112);
			out(irb.CreateShl(f.exp, 64));
			return;
		}
		case PPC_INS_XSXSIGQP:
		{
			auto f = fields(vr(1), 15, 112);
			auto* hidden = irb.CreateAnd(irb.CreateICmpNE(f.exp, c128(0)), irb.CreateNot(f.expMax));
			out(irb.CreateOr(f.frac, irb.CreateShl(irb.CreateZExt(hidden, i128), 112)));
			return;
		}
		case PPC_INS_XSIEXPQP:
		{
			// Capstone names VRB as an FPR (f4 for v4); it is a VR.
			if (ops[2].type == PPC_OP_REG && ops[2].reg >= PPC_REG_F0 && ops[2].reg <= PPC_REG_F31)
			{
				ops[2].reg = ppc_reg(PPC_REG_V0 + (ops[2].reg - PPC_REG_F0));
			}
			auto* a = vr(1);
			auto* keep = llvm::ConstantInt::get(i128, signBit | llvm::APInt::getLowBitsSet(128, 112));
			auto* e = irb.CreateShl(irb.CreateAnd(irb.CreateLShr(vr(2), 64), c128(0x7fff)), 112);
			out(irb.CreateOr(irb.CreateAnd(a, keep), e));
			return;
		}
		case PPC_INS_XSCMPEXPQP:
		case PPC_INS_XSCMPEXPDP:
		{
			bool q = i->id == PPC_INS_XSCMPEXPQP;
			llvm::Value* a = q ? vr(1) : hiDw(vr(1));
			llvm::Value* b = q ? vr(2) : hiDw(vr(2));
			auto fa = fields(a, q ? 15 : 11, q ? 112 : 52);
			auto fb = fields(b, q ? 15 : 11, q ? 112 : 52);
			auto* un = irb.CreateOr(isNan(fa), isNan(fb));
			auto* nun = irb.CreateNot(un);
			setCr(irb.CreateAnd(nun, irb.CreateICmpULT(fa.exp, fb.exp)), irb.CreateAnd(nun, irb.CreateICmpUGT(fa.exp, fb.exp)),
					irb.CreateAnd(nun, irb.CreateICmpEQ(fa.exp, fb.exp)), un);
			return;
		}
		case PPC_INS_XSTSTDCQP:
		case PPC_INS_XSTSTDCDP:
		case PPC_INS_XSTSTDCSP:
		{
			if (ops[2].type != PPC_OP_IMM) break;
			uint64_t dcmx = uint64_t(ops[2].imm) & 0x7f;
			llvm::Value* x = vr(1);
			llvm::Value* m = nullptr;
			llvm::Value* sign = nullptr;
			if (i->id == PPC_INS_XSTSTDCQP)
			{
				m = dataClass(x, 15, 112, dcmx);
				sign = irb.CreateTrunc(irb.CreateLShr(x, 127), i1);
			}
			else if (i->id == PPC_INS_XSTSTDCDP)
			{
				auto* d = hiDw(x);
				m = dataClass(d, 11, 52, dcmx);
				sign = irb.CreateTrunc(irb.CreateLShr(d, 63), i1);
			}
			else
			{
				// The operand is single-precision held in double format.
				auto* s = irb.CreateBitCast(irb.CreateFPTrunc(irb.CreateBitCast(hiDw(x), f64), irb.getFloatTy()), i32);
				m = dataClass(s, 8, 23, dcmx);
				sign = irb.CreateTrunc(irb.CreateLShr(s, 31), i1);
			}
			setCr(sign, irb.getFalse(), m, irb.getFalse());
			return;
		}
		case PPC_INS_XVTSTDCDP:
		case PPC_INS_XVTSTDCSP:
		{
			if (ops[2].type != PPC_OP_IMM) break;
			uint64_t dcmx = uint64_t(ops[2].imm) & 0x7f;
			unsigned w = i->id == PPC_INS_XVTSTDCDP ? 64 : 32;
			auto* x = vr(1);
			llvm::Value* r = c128(0);
			for (unsigned sh = 0; sh < 128; sh += w)
			{
				auto* lane = irb.CreateTrunc(irb.CreateLShr(x, sh), irb.getIntNTy(w));
				auto* m = w == 64 ? dataClass(lane, 11, 52, dcmx) : dataClass(lane, 8, 23, dcmx);
				auto* ones = irb.CreateSExt(m, irb.getIntNTy(w));
				r = irb.CreateOr(r, irb.CreateShl(irb.CreateZExt(ones, i128), sh));
			}
			out(r);
			return;
		}
		case PPC_INS_XSCMPUQP:
		case PPC_INS_XSCMPOQP:
		{
			auto* a = qf(1);
			auto* b = qf(2);
			setCr(irb.CreateFCmpOLT(a, b), irb.CreateFCmpOGT(a, b), irb.CreateFCmpOEQ(a, b), irb.CreateFCmpUNO(a, b));
			return;
		}
		case PPC_INS_XSCMPEQQP:
		case PPC_INS_XSCMPGEQP:
		case PPC_INS_XSCMPGTQP:
		{
			auto* a = qf(1);
			auto* b = qf(2);
			llvm::Value* c = i->id == PPC_INS_XSCMPEQQP ? irb.CreateFCmpOEQ(a, b)
					: i->id == PPC_INS_XSCMPGEQP ? irb.CreateFCmpOGE(a, b) : irb.CreateFCmpOGT(a, b);
			out(irb.CreateSExt(c, i128));
			return;
		}
		case PPC_INS_XSMAXCQP:
		case PPC_INS_XSMINCQP:
		{
			auto* a = qf(1);
			auto* b = qf(2);
			auto* c = i->id == PPC_INS_XSMAXCQP ? irb.CreateFCmpOGT(a, b) : irb.CreateFCmpOLT(a, b);
			outF(irb.CreateSelect(c, a, b));
			return;
		}
		case PPC_INS_XSADDQP: case PPC_INS_XSADDQPO: outF(irb.CreateFAdd(qf(1), qf(2))); return;
		case PPC_INS_XSSUBQP: case PPC_INS_XSSUBQPO: outF(irb.CreateFSub(qf(1), qf(2))); return;
		case PPC_INS_XSMULQP: case PPC_INS_XSMULQPO: outF(irb.CreateFMul(qf(1), qf(2))); return;
		case PPC_INS_XSDIVQP: case PPC_INS_XSDIVQPO: outF(irb.CreateFDiv(qf(1), qf(2))); return;
		case PPC_INS_XSSQRTQP: case PPC_INS_XSSQRTQPO:
			outF(irb.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sqrt, {f128}), {qf(1)}));
			return;
		case PPC_INS_XSMADDQP: case PPC_INS_XSMADDQPO:
		case PPC_INS_XSMSUBQP: case PPC_INS_XSMSUBQPO:
		case PPC_INS_XSNMADDQP: case PPC_INS_XSNMADDQPO:
		case PPC_INS_XSNMSUBQP: case PPC_INS_XSNMSUBQPO:
		{
			bool sub = i->id == PPC_INS_XSMSUBQP || i->id == PPC_INS_XSMSUBQPO || i->id == PPC_INS_XSNMSUBQP
					|| i->id == PPC_INS_XSNMSUBQPO;
			bool neg = i->id == PPC_INS_XSNMADDQP || i->id == PPC_INS_XSNMADDQPO || i->id == PPC_INS_XSNMSUBQP
					|| i->id == PPC_INS_XSNMSUBQPO;
			llvm::Value* t = qf(0);
			if (sub) t = irb.CreateFNeg(t);
			llvm::Value* r = irb.CreateCall(
					llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fma, {f128}), {qf(1), qf(2), t});
			if (neg) r = irb.CreateFNeg(r);
			outF(r);
			return;
		}
		case PPC_INS_XSCVDPQP:
			outF(irb.CreateFPExt(irb.CreateBitCast(hiDw(vr(1)), f64), f128));
			return;
		case PPC_INS_XSCVQPDP:
		case PPC_INS_XSCVQPDPO:
			out(toHiDw(irb.CreateBitCast(irb.CreateFPTrunc(qf(1), f64), i64)));
			return;
		case PPC_INS_XSCVQPSDZ: out(irb.CreateShl(cvtSat(qf(1), 64, true), 64)); return;
		case PPC_INS_XSCVQPUDZ: out(irb.CreateShl(cvtSat(qf(1), 64, false), 64)); return;
		case PPC_INS_XSCVQPSWZ:
			out(toHiDw(irb.CreateSExt(irb.CreateTrunc(cvtSat(qf(1), 32, true), i32), i64)));
			return;
		case PPC_INS_XSCVQPUWZ: out(irb.CreateShl(cvtSat(qf(1), 32, false), 64)); return;
		case PPC_INS_XSCVQPSQZ: out(cvtSat(qf(1), 128, true)); return;
		case PPC_INS_XSCVQPUQZ: out(cvtSat(qf(1), 128, false)); return;
		case PPC_INS_XSCVSDQP: outF(irb.CreateSIToFP(hiDw(vr(1)), f128)); return;
		case PPC_INS_XSCVUDQP: outF(irb.CreateUIToFP(hiDw(vr(1)), f128)); return;
		case PPC_INS_XSCVSQQP: outF(irb.CreateSIToFP(vr(1), f128)); return;
		case PPC_INS_XSCVUQQP: outF(irb.CreateUIToFP(vr(1), f128)); return;
		default:
			break;
	}
	translatePseudoAsmGeneric(i, pi, irb);
}


/**
 * PPC_INS_LQ, PPC_INS_LQARX, PPC_INS_STQ, PPC_INS_STQCX -- quadword load and
 * store of an even/odd GPR pair. The even register holds the doubleword at
 * the lower address (big-endian element order).
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateLoadStoreQuad(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count < 2 || pi->operands[0].type != PPC_OP_REG)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	auto toR = [](unsigned r) -> ppc_reg {
		if (r >= PPC_REG_X0 && r <= PPC_REG_X31) return ppc_reg(PPC_REG_R0 + (r - PPC_REG_X0));
		if (r >= PPC_REG_G8P0 && r <= PPC_REG_G8P0 + 15) return ppc_reg(PPC_REG_R0 + 2 * (r - PPC_REG_G8P0));
		return ppc_reg(r);
	};
	for (unsigned k = 1; k < pi->op_count; ++k)
	{
		auto& op = pi->operands[k];
		if (op.type == PPC_OP_REG) op.reg = toR(op.reg);
		if (op.type == PPC_OP_MEM)
		{
			if (op.mem.base != PPC_REG_INVALID) op.mem.base = toR(op.mem.base);
			if (op.mem.offset != PPC_REG_INVALID) op.mem.offset = toR(op.mem.offset);
		}
	}
	unsigned rt = toR(pi->operands[0].reg);
	if (rt < PPC_REG_R0 || rt > PPC_REG_R30)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	auto* i64 = irb.getInt64Ty();
	auto* ea = loadIndexedEffectiveAddress(pi, irb);
	auto* ea8 = irb.CreateAdd(ea, llvm::ConstantInt::get(ea->getType(), 8));
	bool atomic = i->id == PPC_INS_LQARX || i->id == PPC_INS_STQCX;
	if (i->id == PPC_INS_LQ || i->id == PPC_INS_LQARX)
	{
		auto* hi = loadIntPtr(irb, ea, i64);
		auto* lo = loadIntPtr(irb, ea8, i64);
		if (atomic)
		{
			hi->setAtomic(llvm::AtomicOrdering::Monotonic);
			lo->setAtomic(llvm::AtomicOrdering::Monotonic);
		}
		storeRegister(rt, hi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		storeRegister(rt + 1, lo, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	auto* hi = irb.CreateZExtOrTrunc(loadRegister(rt, irb), i64);
	auto* lo = irb.CreateZExtOrTrunc(loadRegister(rt + 1, irb), i64);
	auto* s0 = storeIntPtr(irb, hi, ea, i64);
	auto* s1 = storeIntPtr(irb, lo, ea8, i64);
	if (atomic)
	{
		s0->setAtomic(llvm::AtomicOrdering::Monotonic);
		s1->setAtomic(llvm::AtomicOrdering::Monotonic);
		storeRegister(PPC_REG_CR0EQ, irb.getInt1(true), irb);
	}
}


/**
 * PPC_INS_LDAT, PPC_INS_LWAT, PPC_INS_STDAT, PPC_INS_STWAT -- ISA 3.0
 * atomic memory operations. The fetch-and-op and store-op function codes
 * become atomicrmw; the compare and bounded-increment codes stay pseudo.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateAtomicMemOp(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count != 3 || pi->operands[0].type != PPC_OP_REG || pi->operands[1].type != PPC_OP_REG
			|| pi->operands[2].type != PPC_OP_IMM)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	auto toR = [](unsigned r) -> unsigned {
		return (r >= PPC_REG_X0 && r <= PPC_REG_X31) ? PPC_REG_R0 + (r - PPC_REG_X0) : r;
	};
	unsigned rt = toR(pi->operands[0].reg);
	unsigned ra = toR(pi->operands[1].reg);
	unsigned fc = unsigned(pi->operands[2].imm) & 31;
	bool load = i->id == PPC_INS_LDAT || i->id == PPC_INS_LWAT;
	bool dw = i->id == PPC_INS_LDAT || i->id == PPC_INS_STDAT;
	llvm::AtomicRMWInst::BinOp op;
	switch (fc)
	{
		case 0: op = llvm::AtomicRMWInst::Add; break;
		case 1: op = llvm::AtomicRMWInst::Xor; break;
		case 2: op = llvm::AtomicRMWInst::Or; break;
		case 3: op = llvm::AtomicRMWInst::And; break;
		case 4: op = llvm::AtomicRMWInst::UMax; break;
		case 5: op = llvm::AtomicRMWInst::Max; break;
		case 6: op = llvm::AtomicRMWInst::UMin; break;
		case 7: op = llvm::AtomicRMWInst::Min; break;
		case 8:
			if (!load)
			{
				translatePseudoAsmGeneric(i, pi, irb);
				return;
			}
			op = llvm::AtomicRMWInst::Xchg;
			break;
		default:
			translatePseudoAsmGeneric(i, pi, irb);
			return;
	}
	if (rt < PPC_REG_R0 || rt > PPC_REG_R31 || ra < PPC_REG_R0 || ra > PPC_REG_R31 || (load && rt == PPC_REG_R31))
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	auto* ty = dw ? irb.getInt64Ty() : irb.getInt32Ty();
	auto* ea = irb.CreateZExtOrTrunc(loadRegister(ra, irb), getDefaultType());
	auto* ptr = irb.CreateIntToPtr(ea, llvm::PointerType::get(irb.getContext(), 0));
	auto* val = irb.CreateZExtOrTrunc(loadRegister(load ? rt + 1 : rt, irb), ty);
	auto* old = irb.CreateAtomicRMW(op, ptr, val, llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
	if (load)
	{
		storeRegister(rt, irb.CreateZExt(old, getDefaultType()), irb);
	}
}


/**
 * PPC_INS_XSRQPI, PPC_INS_XSRQPIX -- round quad-precision to an integral
 * value; PPC_INS_XSRQPXP -- round to double-extended (64-bit significand)
 * precision. Operands are R, VRT, VRB, RMC.
 */
void Capstone2LlvmIrTranslatorPowerpc_impl::translateVsxRoundQuad(cs_insn* i, cs_ppc* pi, llvm::IRBuilder<>& irb)
{
	if (pi->op_count != 4 || pi->operands[0].type != PPC_OP_IMM || pi->operands[1].type != PPC_OP_REG
			|| pi->operands[2].type != PPC_OP_REG || pi->operands[3].type != PPC_OP_IMM)
	{
		translatePseudoAsmGeneric(i, pi, irb);
		return;
	}
	auto vrOp = [](cs_ppc_op op) {
		if (op.reg >= PPC_REG_VF0 && op.reg <= PPC_REG_VF31) op.reg = ppc_reg(PPC_REG_V0 + (op.reg - PPC_REG_VF0));
		return op;
	};
	auto dst = vrOp(pi->operands[1]);
	auto src = vrOp(pi->operands[2]);
	auto* f128 = llvm::Type::getFP128Ty(irb.getContext());
	auto* x = irb.CreateBitCast(loadOpI128(src, irb), f128);
	bool r = pi->operands[0].imm & 1;
	unsigned rmc = unsigned(pi->operands[3].imm) & 3;
	llvm::Value* res = nullptr;
	if (i->id == PPC_INS_XSRQPXP)
	{
		// The rounding mode is not modelled; x86_fp80 has the same 64-bit
		// significand, and the conversion rounds to nearest even.
		res = irb.CreateFPExt(irb.CreateFPTrunc(x, llvm::Type::getX86_FP80Ty(irb.getContext())), f128);
	}
	else
	{
		llvm::Intrinsic::ID id = llvm::Intrinsic::roundeven;
		if (!r)
		{
			// R=0: RMC 0 is round to nearest away, 3 is FPSCR[RN] (assumed
			// nearest even); 1 and 2 are reserved.
			id = rmc == 0 ? llvm::Intrinsic::round : llvm::Intrinsic::roundeven;
		}
		else
		{
			static const llvm::Intrinsic::ID ids[] = {
					llvm::Intrinsic::roundeven, llvm::Intrinsic::trunc, llvm::Intrinsic::ceil, llvm::Intrinsic::floor};
			id = ids[rmc];
		}
		res = irb.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(_module, id, {f128}), {x});
	}
	storeOpI128(dst, irb.CreateBitCast(res, irb.getInt128Ty()), irb);
}

} // namespace capstone2llvmir
} // namespace retdec
